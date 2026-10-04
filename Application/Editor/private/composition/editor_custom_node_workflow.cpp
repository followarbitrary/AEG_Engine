#include "editor_custom_node_workflow.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "module_bundle.hpp"
#include "editor_node_contract_storage.hpp"

namespace aeg::application {
namespace {
using namespace pipeline;

bool identifier(std::string_view value) {
    constexpr std::array keywords{"alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch",
        "char", "class", "concept", "const", "consteval", "constexpr", "constinit", "continue", "co_await",
        "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
        "mutable", "namespace", "new", "noexcept", "nullptr", "operator", "private", "protected", "public",
        "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
        "volatile", "wchar_t", "while", "and", "or", "not", "xor", "bitand", "bitor", "compl", "and_eq",
        "or_eq", "xor_eq", "not_eq", "dynamic_cast", "const_cast", "char8_t", "char16_t", "char32_t"};
    if (value.empty() || value.front() == '_' || !std::isalpha(static_cast<unsigned char>(value.front())) ||
        std::find(keywords.begin(), keywords.end(), value) != keywords.end())
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 128 && (std::isalnum(c) || c == '_');
    });
}

bool cpp_namespace(std::string_view value) {
    while (!value.empty()) {
        const auto end = value.find("::");
        if (!identifier(value.substr(0, end)))
            return false;
        if (end == std::string_view::npos)
            return true;
        value.remove_prefix(end + 2);
    }
    return false;
}

std::optional<FeatureDomain> single_domain(const TypeCatalogEntry& entry) {
    for (auto domain : {FeatureDomain::kLogic, FeatureDomain::kPhysics, FeatureDomain::kRender,
                         FeatureDomain::kAudio, FeatureDomain::kStream})
        if (entry.domains == to_mask(domain))
            return domain;
    return std::nullopt;
}

const char* domain_name(FeatureDomain domain) {
    switch (domain) {
        case FeatureDomain::kLogic: return "Logic";
        case FeatureDomain::kPhysics: return "Physics";
        case FeatureDomain::kRender: return "Render";
        case FeatureDomain::kAudio: return "Audio";
        case FeatureDomain::kStream: return "Stream";
        default: return "Logic";
    }
}

std::string cpp_string(std::string_view value) {
    std::ostringstream result;
    result << '"';
    for (unsigned char c : value) {
        if (c >= 32 && c < 127 && c != '\\' && c != '"')
            result << static_cast<char>(c);
        else
            result << '\\' << std::oct << std::setw(3) << std::setfill('0') << static_cast<unsigned>(c);
    }
    result << '"';
    return result.str();
}

bool below(const std::filesystem::path& root, const std::filesystem::path& child) {
    const auto relative = child.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
        return false;
    for (const auto& component : relative)
        if (component == "..")
            return false;
    return true;
}

foundation::ResultValue<std::filesystem::path> output_directory(const CustomNodeScaffoldRequest& request) {
    if (request.project_root.empty() || !request.project_root.is_absolute() || request.source_subdirectory.empty() ||
        request.source_subdirectory.has_root_path())
        return {foundation::invalid_argument("custom node requires an absolute project root and a relative source directory"), {}};
    for (const auto& component : request.source_subdirectory)
        if (component == "..")
            return {foundation::invalid_argument("custom node source directory cannot contain parent traversal"), {}};
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(request.project_root, error);
    if (error)
        return {foundation::invalid_argument("custom node project root cannot be resolved"), {}};
    const auto directory = std::filesystem::weakly_canonical(root / request.source_subdirectory, error);
    if (error || !below(root, directory))
        return {foundation::invalid_argument("custom node source directory resolves outside the project"), {}};
    return {foundation::Result::success(), directory};
}

foundation::ResultValue<std::string> contract_json(const NodeDefinitionDraft& draft) {
    NodeContractFile file{};
    file.draft = draft;
    file.source = draft.source;
    return encode_editor_node_contract(file);
}

std::string header(const CustomNodeScaffoldRequest& request, const TypeCatalogEntry& entry) {
    const auto kind = entry.key.kind;
    const std::string base = kind == AuthoringTypeKind::kPipeline ? "Pipeline" :
                             kind == AuthoringTypeKind::kFeature ? std::string(domain_name(*single_domain(entry))) + "Feature" : "RenderPass";
    const std::string descriptor = kind == AuthoringTypeKind::kPipeline ? "PipelineDescriptor" :
                                   kind == AuthoringTypeKind::kFeature ? "FeatureDescriptor" : "RenderPassDescriptor";
    std::ostringstream out;
    out << "// AEG_FLOWGRAPH_GENERATED_BEGIN\n#pragma once\n\n#include \"game_module.hpp\"\n\nnamespace " << request.namespace_name << " {\n\n"
        << "/** Generated by AEG FlowGraph. Implement the TODO bodies before preview. */\n"
        << "class " << request.type_name << " final : public aeg::pipeline::" << base << " {\npublic:\n"
        << "    [[nodiscard]] const aeg::pipeline::" << descriptor << "& descriptor() const noexcept override;\n";
    if (kind == AuthoringTypeKind::kPipeline)
        out << "    [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder&) const override;\n";
    else if (kind == AuthoringTypeKind::kFeature) {
        if (*single_domain(entry) == FeatureDomain::kRender)
            out << "    [[nodiscard]] bool uses_authored_render_passes() const noexcept override { return true; }\n";
        out << "protected:\n    [[nodiscard]] aeg::foundation::Result setup(BuilderType&) const override;\n";
    } else
        out << "    [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::RenderPassBuilder&, const aeg::pipeline::RenderPassConfiguration&) const override;\n"
            << "    [[nodiscard]] aeg::foundation::Result execute(aeg::pipeline::RenderPassExecutionContext&, const aeg::pipeline::RenderPassConfiguration&) const override;\n";
    out << "};\n\n[[nodiscard]] aeg::foundation::ResultValue<aeg::pipeline::TypeCatalogEntry> make_"
        << request.type_name << "_contract();\n[[nodiscard]] aeg::foundation::Result register_" << request.type_name
        << "(aeg::module::ModuleDefinition&);\n\n}  // namespace " << request.namespace_name
        << "\n// AEG_FLOWGRAPH_GENERATED_END\n";
    return out.str();
}

std::string implementation(const CustomNodeScaffoldRequest& request, const TypeCatalogEntry& entry) {
    const auto kind = entry.key.kind;
    const std::string descriptor = kind == AuthoringTypeKind::kPipeline ? "PipelineDescriptor" :
                                   kind == AuthoringTypeKind::kFeature ? "FeatureDescriptor" : "RenderPassDescriptor";
    std::ostringstream out;
    out << "#include \"" << request.type_name << ".flowgraph.hpp\"\n\nnamespace " << request.namespace_name
        << " {\nnamespace {\n// AEG_FLOWGRAPH_DESCRIPTOR_BEGIN\nconst aeg::pipeline::" << descriptor << " kDescriptor{";
    if (kind == AuthoringTypeKind::kPipeline)
        out << "aeg::pipeline::PipelineId{" << entry.key.id << "ull}, " << cpp_string(entry.name) << ", {"
            << entry.version.major << ", " << entry.version.minor << ", " << entry.version.patch
            << "}, aeg::pipeline::PipelineCategory::kCustom, " << entry.scopes << ", " << entry.domains
            << ", aeg::pipeline::ExecutionDomain::kMainThread, aeg::pipeline::TimePolicy::kFrame, aeg::pipeline::ModuleId{"
            << entry.owner_module.value() << "ull}, 0";
    else if (kind == AuthoringTypeKind::kFeature)
        out << "aeg::pipeline::FeatureId{" << entry.key.id << "ull}, " << cpp_string(entry.name)
            << ", aeg::pipeline::FeatureDomain::k" << domain_name(*single_domain(entry)) << ", " << entry.domains
            << ", " << entry.scopes << ", {" << entry.version.major << ", " << entry.version.minor << ", "
            << entry.version.patch << "}, aeg::pipeline::ModuleId{" << entry.owner_module.value() << "ull}, {}";
    else {
        const auto queues = std::get<PassAuthoringDeclaration>(entry.declaration).allowed_queue_mask;
        const auto graph_type = (queues & 1u) ? "Graphics" : (queues & 2u) ? "Compute" : "Copy";
        out << "aeg::pipeline::RenderPassTypeId{" << entry.key.id << "ull}, aeg::pipeline::ModuleId{"
            << entry.owner_module.value() << "ull}, " << cpp_string(entry.name) << ", {" << entry.version.major
            << ", " << entry.version.minor << ", " << entry.version.patch
            << "}, aeg::pipeline::RenderGraphPassType::k" << graph_type << ", " << queues << "ull";
    }
    out << "};\n// AEG_FLOWGRAPH_DESCRIPTOR_END\n}  // namespace\n\nconst aeg::pipeline::" << descriptor << "& " << request.type_name
        << "::descriptor() const noexcept { return kDescriptor; }\n\n";
    const auto body = [&out] {
        out << "    // TODO: implement this node using the declared input/output contract.\n"
            << "    return aeg::foundation::unsupported(\"FlowGraph node implementation is not ready\");\n}\n\n";
    };
    out << "// AEG_FLOWGRAPH_CONTRACT_NOTES_BEGIN\n";
    for (const auto& port : entry.ports)
        out << "// " << (port.direction == PortDirection::kInput ? "Input " : "Output ") << port.name
            << " | PortId " << port.id << " | Schema " << port.schema_id << ':' << port.schema_version << '\n';
    for (const auto& parameter : entry.parameters)
        out << "// Compute Point " << parameter.name << " | ParameterId " << parameter.id << '\n';
    out << "// AEG_FLOWGRAPH_CONTRACT_NOTES_END\n\n";
    if (kind == AuthoringTypeKind::kPipeline) {
        out << "aeg::foundation::Result " << request.type_name << "::build(aeg::pipeline::PipelineBuilder&) const {\n";
        body();
    } else if (kind == AuthoringTypeKind::kFeature) {
        out << "aeg::foundation::Result " << request.type_name << "::setup(BuilderType&) const {\n";
        body();
    } else {
        out << "aeg::foundation::Result " << request.type_name
            << "::setup(aeg::pipeline::RenderPassBuilder&, const aeg::pipeline::RenderPassConfiguration&) const {\n";
        body();
        out << "aeg::foundation::Result " << request.type_name
            << "::execute(aeg::pipeline::RenderPassExecutionContext&, const aeg::pipeline::RenderPassConfiguration&) const {\n";
        body();
    }
    out << "}  // namespace " << request.namespace_name << "\n";
    return out.str();
}

std::string registration(const CustomNodeScaffoldRequest& request, const TypeCatalogEntry& entry,
                         std::string_view contract) {
    // One complete canonical contract is embedded in the compiled registration.
    // It is also written verbatim to the metadata file for host-only inspection.
    std::string delimiter = "AEG_CONTRACT";
    while (contract.find(")" + delimiter + "\"") != std::string_view::npos)
        delimiter += 'X';
    std::ostringstream out;
    out << "// AEG_FLOWGRAPH_GENERATED_BEGIN\n#include \"" << request.type_name << ".flowgraph.hpp\"\n#include \"module_bundle.hpp\"\n\nnamespace "
        << request.namespace_name << " {\n\naeg::foundation::ResultValue<aeg::pipeline::TypeCatalogEntry> make_"
        << request.type_name << "_contract() {\n    const auto metadata = aeg::module::decode_module_metadata(R\""
        << delimiter << '(' << contract << ')' << delimiter << "\");\n"
        << "    if (!metadata) return {metadata.result, {}};\n"
        << "    const auto* entry = metadata.value.catalog.find({aeg::pipeline::AuthoringTypeKind::k"
        << (entry.key.kind == AuthoringTypeKind::kPipeline ? "Pipeline" :
            entry.key.kind == AuthoringTypeKind::kFeature ? "Feature" : "RenderPass") << ", " << entry.key.id
        << "ull});\n    if (!entry) return {aeg::foundation::not_found(\"Generated node contract is missing\"), {}};\n"
        << "    return {aeg::foundation::Result::success(), *entry};\n}\n\n"
        << "aeg::foundation::Result register_" << request.type_name << "(aeg::module::ModuleDefinition& definition) {\n"
        << "    auto entry = make_" << request.type_name << "_contract();\n    if (!entry) return entry.result;\n"
        << "    return AEG_REGISTER_" << (entry.key.kind == AuthoringTypeKind::kPipeline ? "PIPELINE" :
                                         entry.key.kind == AuthoringTypeKind::kFeature ? "FEATURE" : "RENDER_PASS")
        << "(definition, " << request.type_name << ", std::move(entry.value));\n}\n\n}  // namespace "
        << request.namespace_name << "\n// AEG_FLOWGRAPH_GENERATED_END\n";
    return out.str();
}

struct FileTransaction {
    struct File { std::filesystem::path path, temp, backup; std::string bytes; bool backed_up{}, installed{}; };
    std::vector<File> files;
    std::filesystem::path lock;
    bool committed{};
    ~FileTransaction() {
        std::error_code error;
        for (auto& file : files) {
            if (!committed && file.installed)
                std::filesystem::remove(file.path, error);
            if (file.backed_up) {
                if (committed)
                    std::filesystem::remove(file.backup, error);
                else
                    std::filesystem::rename(file.backup, file.path, error);
            }
            std::filesystem::remove(file.temp, error);
        }
        if (!lock.empty())
            std::filesystem::remove(lock, error);
    }
};

foundation::ResultValue<std::string> replace_region(std::string_view existing, std::string_view generated,
                                                    std::string_view begin_marker,
                                                    std::string_view end_marker) {
    const auto begin = existing.find(begin_marker);
    const auto end = existing.find(end_marker, begin == std::string_view::npos ? 0 : begin + begin_marker.size());
    if (begin == std::string_view::npos || end == std::string_view::npos || end < begin)
        return {foundation::invalid_state("generated region markers are missing or ambiguous"), {}};
    if (existing.find(begin_marker, begin + begin_marker.size()) != std::string_view::npos ||
        existing.find(end_marker, end + end_marker.size()) != std::string_view::npos)
        return {foundation::invalid_state("duplicate generated region markers"), {}};
    const auto body_begin = existing.find('\n', begin);
    if (body_begin == std::string_view::npos || body_begin >= end)
        return {foundation::invalid_state("generated region header is invalid"), {}};
    const auto stored_digest = existing.substr(begin + begin_marker.size(), body_begin - begin - begin_marker.size());
    const auto actual_digest = module::module_content_sha256(existing.substr(body_begin + 1, end - body_begin - 1));
    if (!actual_digest || stored_digest != " sha256=" + actual_digest.value)
        return {foundation::invalid_state("generated region was edited manually; existing files preserved"), {}};
    const auto generated_begin = generated.find(begin_marker);
    const auto generated_end = generated.find(end_marker, generated_begin == std::string_view::npos
                                                           ? 0
                                                           : generated_begin + begin_marker.size());
    if (generated_begin == std::string_view::npos || generated_end == std::string_view::npos)
        return {foundation::invalid_state("generated template markers are missing"), {}};
    std::string result{existing.substr(0, begin)};
    result.append(generated.substr(generated_begin, generated_end + end_marker.size() - generated_begin));
    result.append(existing.substr(end + end_marker.size()));
    return {foundation::Result::success(), std::move(result)};
}

std::string stamp_region(std::string bytes, std::string_view begin_marker, std::string_view end_marker) {
    const auto begin = bytes.find(begin_marker);
    const auto body = bytes.find('\n', begin);
    const auto end = bytes.find(end_marker, body);
    const auto digest = module::module_content_sha256(std::string_view(bytes).substr(body + 1, end - body - 1));
    if (digest)
        bytes.insert(begin + begin_marker.size(), " sha256=" + digest.value);
    return bytes;
}

foundation::ResultValue<std::string> read_existing(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::not_found("generated file cannot be opened: " + path.string()), {}};
    std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return {foundation::Result::success(), std::move(value)};
}

uint32_t symbol_line(std::string_view text, std::string_view symbol) {
    const auto offset = text.find(symbol);
    return offset == std::string_view::npos ? 1u :
        1u + static_cast<uint32_t>(std::count(text.begin(), text.begin() + offset, '\n'));
}
}  // namespace

foundation::Result validate_custom_node_scaffold_request(const CustomNodeScaffoldRequest& request,
                                                         const pipeline::TypeCatalogEntry& entry) {
    NodeDefinitionDraft draft{};
    draft.entry = entry;
    draft.generated = true;
    return validate_custom_node_scaffold_request(request, draft);
}

foundation::Result validate_custom_node_scaffold_request(const CustomNodeScaffoldRequest& request,
                                                         const NodeDefinitionDraft& draft) {
    if (const auto directory = output_directory(request); !directory)
        return directory.result;
    if (!cpp_namespace(request.namespace_name) || !identifier(request.type_name))
        return foundation::invalid_argument("custom node namespace or type name is not a valid non-reserved C++ identifier");
    if (draft.entry.key.kind == AuthoringTypeKind::kFeature && !single_domain(draft.entry))
        return foundation::unsupported("custom Feature scaffold requires one concrete domain");
    if (request.owner_module.is_valid() && request.owner_module != draft.entry.owner_module)
        return foundation::invalid_argument("custom node owner module does not match its contract");
    return validate_node_definition(draft);
}

foundation::ResultValue<CustomNodeScaffoldResult> scaffold_custom_node(const CustomNodeScaffoldRequest& request,
                                                                      const pipeline::TypeCatalogEntry& entry) {
    NodeDefinitionDraft draft{};
    draft.entry = entry;
    draft.generated = true;
    return scaffold_custom_node(request, draft);
}

foundation::ResultValue<CustomNodeScaffoldResult> scaffold_custom_node(const CustomNodeScaffoldRequest& request,
                                                                       const NodeDefinitionDraft& draft) {
    try {
        if (const auto valid = validate_custom_node_scaffold_request(request, draft); !valid)
            return {valid, {}};
        const auto directory = output_directory(request);
        if (!directory)
            return {directory.result, {}};
        const auto contract = contract_json(draft);
        if (!contract)
            return {contract.result, {}};
        std::error_code error;
        std::filesystem::create_directories(directory.value, error);
        if (error)
            return {foundation::invalid_state("custom node source directory cannot be created"), {}};
        FileTransaction transaction;
        const auto lock = directory.value / (request.type_name + ".flowgraph.lock");
        if (!std::filesystem::create_directory(lock, error))
            return {foundation::invalid_state("another custom node generation owns this output, or the output is inaccessible"), {}};
        transaction.lock = lock;
        static std::atomic<uint64_t> sequence{};
        const std::string suffix = "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                   "." + std::to_string(++sequence);
        auto implementation_bytes = stamp_region(implementation(request, draft.entry),
            "// AEG_FLOWGRAPH_DESCRIPTOR_BEGIN", "// AEG_FLOWGRAPH_DESCRIPTOR_END");
        implementation_bytes = stamp_region(std::move(implementation_bytes),
            "// AEG_FLOWGRAPH_CONTRACT_NOTES_BEGIN", "// AEG_FLOWGRAPH_CONTRACT_NOTES_END");
        const std::array<std::pair<std::string, std::string>, 4> outputs{{
            {".flowgraph.hpp", stamp_region(header(request, draft.entry),
                "// AEG_FLOWGRAPH_GENERATED_BEGIN", "// AEG_FLOWGRAPH_GENERATED_END")},
            {".flowgraph.cpp", std::move(implementation_bytes)},
            {".flowgraph.registration.cpp", stamp_region(registration(request, draft.entry, contract.value),
                "// AEG_FLOWGRAPH_GENERATED_BEGIN", "// AEG_FLOWGRAPH_GENERATED_END")},
            {".flowgraph.contract.json", contract.value}}};
        for (const auto& [extension, bytes] : outputs) {
            const auto path = directory.value / (request.type_name + extension);
            const auto status = std::filesystem::symlink_status(path, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return {foundation::invalid_state("cannot inspect custom node output"), {}};
            error.clear();
            const bool exists = std::filesystem::exists(status);
            if (exists && (!std::filesystem::is_regular_file(status) ||
                           (request.mode == CustomNodeGenerationMode::kCreate && !request.overwrite)))
                return {foundation::invalid_state("custom node output exists or is not a regular file: " + path.string()), {}};
            std::string staged = bytes;
            if (request.mode == CustomNodeGenerationMode::kUpdateGeneratedRegions && exists &&
                extension != ".flowgraph.contract.json") {
                const auto current = read_existing(path);
                if (!current)
                    return {current.result, {}};
                const auto begin_marker = extension == ".flowgraph.cpp" ? "// AEG_FLOWGRAPH_DESCRIPTOR_BEGIN"
                                  : "// AEG_FLOWGRAPH_GENERATED_BEGIN";
                const auto end_marker = extension == ".flowgraph.cpp" ? "// AEG_FLOWGRAPH_DESCRIPTOR_END"
                                : "// AEG_FLOWGRAPH_GENERATED_END";
                const auto replaced = replace_region(current.value, bytes, begin_marker, end_marker);
                if (!replaced)
                    return {replaced.result, {}};
                staged = replaced.value;
                if (extension == ".flowgraph.cpp") {
                    const auto notes = replace_region(staged, bytes, "// AEG_FLOWGRAPH_CONTRACT_NOTES_BEGIN",
                                                       "// AEG_FLOWGRAPH_CONTRACT_NOTES_END");
                    if (!notes)
                        return {notes.result, {}};
                    staged = notes.value;
                }
            }
            transaction.files.push_back({path, path.string() + suffix + ".tmp", path.string() + suffix + ".bak", staged});
        }
        for (auto& file : transaction.files) {
            std::ofstream stream(file.temp, std::ios::binary | std::ios::trunc);
            stream.write(file.bytes.data(), static_cast<std::streamsize>(file.bytes.size()));
            stream.close();
            if (!stream)
                return {foundation::invalid_state("custom node temporary file cannot be written"), {}};
        }
        for (auto& file : transaction.files) {
            if (std::filesystem::exists(file.path)) {
                std::filesystem::rename(file.path, file.backup, error);
                if (error)
                    return {foundation::invalid_state("custom node original file cannot be preserved"), {}};
                file.backed_up = true;
            }
            std::filesystem::rename(file.temp, file.path, error);
            if (error)
                return {foundation::invalid_state("custom node file cannot be installed; transaction rolled back"), {}};
            file.installed = true;
        }
        transaction.committed = true;
        CustomNodeScaffoldResult result;
        result.key = draft.entry.key;
        result.kind = draft.entry.key.kind == AuthoringTypeKind::kPipeline ? CustomNodeKind::kPipeline :
                      draft.entry.key.kind == AuthoringTypeKind::kFeature ? CustomNodeKind::kFeature : CustomNodeKind::kPass;
        for (const auto& file : transaction.files)
            result.files.push_back(file.path);
        std::string implementation_method = "execute";
        if (draft.entry.key.kind == AuthoringTypeKind::kPipeline)
            implementation_method = "build";
        else if (draft.entry.key.kind == AuthoringTypeKind::kFeature)
            implementation_method = "setup";
        const std::string implementation_symbol = request.type_name + "::" + implementation_method;
        result.implementation_target = {result.files[1], symbol_line(outputs[1].second, implementation_symbol), implementation_symbol};
        result.contract_target = {result.files[2], symbol_line(outputs[2].second, "make_" + request.type_name + "_contract()"),
                                  "make_" + request.type_name + "_contract"};
        result.registration_target = {result.files[2], symbol_line(outputs[2].second, "register_" + request.type_name + "("),
                                      "register_" + request.type_name};
        result.updated_generated_regions = request.mode == CustomNodeGenerationMode::kUpdateGeneratedRegions;
        result.implementation_preserved = request.mode == CustomNodeGenerationMode::kUpdateGeneratedRegions;
        result.contract_updated = true;
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("custom node generation allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

foundation::Result launch_editor_ide(const EditorIdeLaunchRequest& request) {
    if (request.project_root.empty() || request.source_file.empty() || request.line == 0 || request.column == 0)
        return foundation::invalid_argument("IDE launch requires a project root, source file and location");
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(request.project_root, error);
    if (error)
        return foundation::invalid_argument("IDE launch project root cannot be resolved");
    const auto file = std::filesystem::weakly_canonical(request.source_file.is_absolute() ? request.source_file
                                                                                           : root / request.source_file,
                                                        error);
    if (error || !below(root, file) || !std::filesystem::is_regular_file(file))
        return foundation::invalid_argument("IDE launch source file must be an existing project-local file");
#if defined(_WIN32)
    const auto quote = [](const std::wstring& value) {
        std::wstring result = L"\"";
        for (const wchar_t character : value) {
            if (character == L'\"')
                result += L"\\\"";
            else
                result.push_back(character);
        }
        result += L"\"";
        return result;
    };
    std::vector<std::pair<std::wstring, std::wstring>> candidates;
    const std::wstring source = file.wstring();
    switch (request.ide) {
        case EditorIdeKind::kVisualStudioCode:
            candidates.push_back({L"code", L" --reuse-window --goto " + quote(source) + L":" +
                                         std::to_wstring(request.line) + L":" + std::to_wstring(request.column)});
            break;
        case EditorIdeKind::kClion:
            candidates.push_back({L"clion64.exe", L" --line " + std::to_wstring(request.line) + L" " + quote(source)});
            break;
        case EditorIdeKind::kVisualStudio: {
            std::filesystem::path solution;
            for (const auto& entry : std::filesystem::directory_iterator(root, error))
                if (!error && entry.path().extension() == ".sln") {
                    solution = entry.path();
                    break;
                }
            candidates.push_back({L"devenv.exe", (solution.empty() ? L"" : quote(solution.wstring()) + L" ") +
                                                   L"/Edit " + quote(source)});
            break;
        }
        case EditorIdeKind::kAuto:
            candidates = {{L"code", L" --reuse-window --goto " + quote(source) + L":" +
                                     std::to_wstring(request.line) + L":" + std::to_wstring(request.column)},
                          {L"clion64.exe", L" --line " + std::to_wstring(request.line) + L" " + quote(source)},
                          {L"devenv.exe", L" /Edit " + quote(source)}};
            break;
    }
    for (const auto& [executable, arguments] : candidates) {
        std::wstring command = executable + arguments;
        std::vector<wchar_t> command_line(command.begin(), command.end());
        command_line.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, root.wstring().c_str(),
                            &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return foundation::Result::success();
        }
    }
    return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                       "No supported IDE executable could be started");
#else
    (void)request;
    return foundation::unsupported("IDE launching is not implemented on this platform");
#endif
}
}  // namespace aeg::application
