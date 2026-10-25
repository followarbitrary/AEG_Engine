#include "editor_feature_implementation_resolver.hpp"

#include "editor_feature_graph_document.hpp"
#include "editor_node_contract_storage.hpp"
#include "project_context.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace aeg::application {
namespace {
constexpr std::string_view kContractSuffix{".flowgraph.contract.json"};
constexpr std::size_t kMaxSourceBytes = 16u * 1024u * 1024u;

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}
bool below(const std::filesystem::path& root, const std::filesystem::path& file) {
    const auto relative = file.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return false;
    return std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; });
}
bool identifier(std::string_view value) {
    const auto initial = [](unsigned char c) { return c < 128 && (std::isalpha(c) || c == '_'); };
    return !value.empty() && initial(static_cast<unsigned char>(value.front())) &&
        std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
            return c < 128 && (std::isalnum(c) || c == '_');
        });
}
struct Token final { std::string_view text; std::size_t offset; };

// Only source tokens participate in navigation. Preserve byte offsets and line
// breaks while skipping all C++ comments, quoted literals and raw literals.
std::vector<Token> tokens(std::string_view source) {
    std::vector<Token> result;
    std::size_t position = 0;
    while (position < source.size()) {
        const auto remaining = source.substr(position);
        if (std::isspace(static_cast<unsigned char>(source[position]))) { ++position; continue; }
        if (remaining.starts_with("//")) {
            const auto end = source.find('\n', position + 2);
            position = end == std::string_view::npos ? source.size() : end;
            continue;
        }
        if (remaining.starts_with("/*")) {
            const auto end = source.find("*/", position + 2);
            if (end == std::string_view::npos) throw std::invalid_argument("implementation has an unterminated comment");
            position = end + 2; continue;
        }
        bool raw = false;
        for (const std::string_view prefix : {"R\"", "u8R\"", "uR\"", "UR\"", "LR\""}) {
            if (!remaining.starts_with(prefix)) continue;
            const auto delimiter_begin = position + prefix.size();
            const auto body = source.find('(', delimiter_begin);
            if (body == std::string_view::npos || body - delimiter_begin > 16)
                throw std::invalid_argument("implementation raw literal is malformed");
            const std::string close = ")" + std::string{source.substr(delimiter_begin, body - delimiter_begin)} + "\"";
            const auto end = source.find(close, body + 1);
            if (end == std::string_view::npos) throw std::invalid_argument("implementation raw literal is unterminated");
            position = end + close.size(); raw = true; break;
        }
        if (raw) continue;
        if (source[position] == '\"' || source[position] == '\'') {
            const auto quote = source[position++];
            bool closed = false;
            while (position < source.size()) {
                const auto c = source[position++];
                if (c == '\\' && position < source.size()) ++position;
                else if (c == quote) { closed = true; break; }
            }
            if (!closed) throw std::invalid_argument("implementation quoted literal is unterminated");
            continue;
        }
        const auto begin = position;
        const auto c = static_cast<unsigned char>(source[position]);
        if (c < 128 && std::isdigit(c)) {
            // Apostrophes inside numeric literals are digit separators, not
            // character literals (for example an author's 250'000 budget).
            while (++position < source.size()) {
                const auto next = static_cast<unsigned char>(source[position]);
                if (next >= 128 || (!std::isalnum(next) && next != '_' && next != '.' && next != '\'')) break;
            }
        } else if (c < 128 && (std::isalpha(c) || c == '_')) {
            while (++position < source.size()) {
                const auto next = static_cast<unsigned char>(source[position]);
                if (next >= 128 || (!std::isalnum(next) && next != '_')) break;
            }
        } else if (remaining.starts_with("::") || remaining.starts_with("&&") || remaining.starts_with("->")) {
            position += 2;
        } else ++position;
        result.push_back({source.substr(begin, position - begin), begin});
    }
    return result;
}

std::size_t after_parentheses(const std::vector<Token>& stream, std::size_t open) {
    std::size_t depth = 0;
    for (auto index = open; index < stream.size(); ++index) {
        if (stream[index].text == "(") ++depth;
        else if (stream[index].text == ")" && --depth == 0) return index + 1;
    }
    return stream.size();
}
bool has_body(const std::vector<Token>& stream, std::size_t after) {
    while (after < stream.size()) {
        const auto text = stream[after].text;
        if (text == "{") return true;
        if (text == "noexcept") {
            ++after;
            if (after < stream.size() && stream[after].text == "(") after = after_parentheses(stream, after);
        } else if (text == "const" || text == "volatile" || text == "&" || text == "&&") ++after;
        else return false;
    }
    return false;
}

foundation::ResultValue<uint32_t> execute_line(std::string_view source, std::string_view type) {
    const auto stream = tokens(source);
    std::optional<std::size_t> found;
    for (std::size_t index = 0; index + 3 < stream.size(); ++index) {
        if (stream[index].text != type || stream[index + 1].text != "::" ||
            stream[index + 2].text != "execute" || stream[index + 3].text != "(") continue;
        if (!has_body(stream, after_parentheses(stream, index + 3))) continue;
        if (found) return {foundation::invalid_state("project implementation has ambiguous execute definitions"), {}};
        found = stream[index].offset;
    }
    if (!found) return {foundation::not_found("No project execute definition was found in the generated implementation"), {}};
    return {foundation::Result::success(), 1u + static_cast<uint32_t>(std::count(source.begin(), source.begin() + *found, '\n'))};
}
}  // namespace

foundation::ResultValue<CustomNodeDefinitionTarget> locate_feature_implementation(
    const std::filesystem::path& project_root_or_file, feature::FeatureDefinitionId definition) {
    if (project_root_or_file.empty() || !definition.is_valid())
        return {foundation::invalid_argument("implementation navigation requires a project and Definition identity"), {}};
    try {
        const auto project_file = resolve_editor_project_file(project_root_or_file);
        if (!project_file) return {project_file.result, {}};
        auto root = project_root_or_file;
        if (!project_file.value.empty()) {
            const auto project_config = project::ProjectFileCodec{}.load(project_file.value);
            if (!project_config) return {project_config.result, {}};
            root = project_file.value.parent_path();
        }
        std::error_code error;
        root = std::filesystem::canonical(root, error);
        if (error || !std::filesystem::is_directory(root, error) || error)
            return {foundation::invalid_argument("implementation project root is not an existing directory"), {}};
        const auto source_directory = resolve_editor_source_directory(
            project_file.value.empty() ? root : project_file.value);
        if (!source_directory) return {source_directory.result, {}};
        const auto contracts = scan_editor_node_contracts(root, source_directory.value);
        if (!contracts) return {contracts.result, {}};
        const NodeContractFile* contract = nullptr;
        for (const auto& candidate : contracts.value) {
            if (candidate.draft.entry.id != definition || !candidate.draft.generated ||
                !utf8(candidate.path.filename()).ends_with(kContractSuffix)) continue;
            if (contract) return {foundation::invalid_state("Definition has multiple generated project contracts"), {}};
            if (candidate.source == NodeContractSource::kConflict)
                return {foundation::invalid_state("Definition project contract is conflicted"), {}};
            contract = &candidate;
        }
        if (!contract) return {foundation::not_found("No project implementation is associated with this Definition"), {}};
        auto stem = utf8(contract->path.filename());
        stem.resize(stem.size() - kContractSuffix.size());
        if (!identifier(stem)) return {foundation::invalid_argument("generated implementation type name is invalid"), {}};
        const auto source_name = stem + ".flowgraph.cpp";
        const auto candidate = contract->path.parent_path() /
            std::filesystem::path{std::u8string{source_name.begin(), source_name.end()}};
        const auto source = std::filesystem::canonical(candidate, error);
        if (error) return {foundation::not_found("No project implementation source file is available"), {}};
        if (!below(root, source) || !std::filesystem::is_regular_file(source, error) || error)
            return {foundation::invalid_argument("implementation source must be an existing project-local regular file"), {}};
        const auto size = std::filesystem::file_size(source, error);
        if (error || size > kMaxSourceBytes)
            return {foundation::invalid_argument("implementation source cannot be read within the navigation size limit"), {}};
        std::string bytes(static_cast<std::size_t>(size), '\0');
        std::ifstream input(source, std::ios::binary);
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!input || input.peek() != std::ifstream::traits_type::eof())
            return {foundation::invalid_state("implementation source changed or could not be read completely"), {}};
        const auto line = execute_line(bytes, stem);
        if (!line) return {line.result, {}};
        return {foundation::Result::success(), {source, line.value, stem + "::execute"}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("implementation navigation allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::invalid_argument(error.what()), {}};
    }
}
}  // namespace aeg::application
