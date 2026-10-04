#include "editor_node_contract_storage.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

#include "module_bundle.hpp"

namespace aeg::application {
namespace {

using Json = nlohmann::json;
constexpr std::string_view kSuffix = ".flowgraph.contract.json";
constexpr std::string_view kDraftSuffix = ".flowgraph.draft.json";

const char* source_name(NodeContractSource source) noexcept {
    switch (source) {
        case NodeContractSource::kEditorDraft:
            return "editor_draft";
        case NodeContractSource::kCompiled:
            return "compiled";
        case NodeContractSource::kConflict:
            return "conflict";
    }
    return "editor_draft";
}

foundation::ResultValue<NodeContractSource> parse_source(const Json& root) {
    if (!root.contains("source") || !root.at("source").is_string())
        return {foundation::invalid_argument("editor contract source is missing"), {}};
    const auto source = root.at("source").get<std::string>();
    if (source == "editor_draft")
        return {foundation::Result::success(), NodeContractSource::kEditorDraft};
    if (source == "compiled")
        return {foundation::Result::success(), NodeContractSource::kCompiled};
    if (source == "conflict")
        return {foundation::Result::success(), NodeContractSource::kConflict};
    return {foundation::invalid_argument("editor contract source is invalid"), {}};
}

bool path_inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error)
        return false;
    error.clear();
    const auto canonical_path = std::filesystem::weakly_canonical(path, error);
    if (error)
        return false;
    const auto relative = std::filesystem::relative(canonical_path, canonical_root, error);
    if (error || relative.empty() || relative.is_absolute())
        return false;
    const auto first = relative.begin();
    return first == relative.end() || *first != "..";
}

foundation::ResultValue<std::string> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::not_found("editor contract cannot be opened: " + path.string()), {}};
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof())
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                    "editor contract cannot be read: " + path.string()), {}};
    return {foundation::Result::success(), std::move(bytes)};
}

foundation::ResultValue<std::string> canonical_without_digest(Json root) {
    root.erase("digest");
    return module::module_content_sha256(root.dump());
}

foundation::ResultValue<std::string> encode_metadata(const pipeline::TypeCatalogEntry& entry) {
    module::ModuleMetadata metadata{};
    metadata.module = {entry.owner_module, "Editor Node Contract", {1, 0, 0}, {}};
    metadata.descriptor = metadata.module;
    metadata.build_id = "editor-draft";
    metadata.target = "editor";
    metadata.configuration = "Editor";
    metadata.contract_digest = "editor-draft";
    if (const auto result = metadata.catalog.add(entry); !result)
        return {result, {}};
    const auto digest = module::module_contract_sha256(metadata);
    if (!digest)
        return {digest.result, {}};
    metadata.contract_digest = digest.value;
    return module::encode_module_metadata(metadata);
}

Json envelope(const NodeContractFile& file, bool include_digest) {
    Json root;
    const auto metadata = encode_metadata(file.draft.entry);
    root = metadata ? Json::parse(metadata.value) : Json::object();
    root["editor_contract"] = Json{{"schema_version", 1},
                                    {"source", source_name(file.draft.source)},
                                    {"generated", file.draft.generated},
                                    {"draft_revision_hash", file.draft.draft_revision_hash},
                                    {"retired_port_ids", file.draft.retired_port_ids},
                                    {"retired_parameter_ids", file.draft.retired_parameter_ids}};
    if (include_digest)
        root["digest"] = file.digest;
    return root;
}

foundation::ResultValue<std::vector<std::filesystem::path>> contract_paths(const std::filesystem::path& project_root) {
    const auto directory = project_root / "Source" / "FlowGraph";
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error)
            return {foundation::invalid_argument("cannot inspect FlowGraph contract directory: " + error.message()),
                    {}};
        return {foundation::Result::success(), std::move(paths)};
    }
    for (std::filesystem::recursive_directory_iterator iterator{
             directory, std::filesystem::directory_options::skip_permission_denied, error};
         !error && iterator != std::filesystem::recursive_directory_iterator{}; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error &&
            (iterator->path().filename().string().ends_with(kSuffix) ||
             iterator->path().filename().string().ends_with(kDraftSuffix)))
            paths.push_back(std::filesystem::weakly_canonical(iterator->path(), error));
        if (error)
            break;
    }
    if (error)
        return {foundation::invalid_argument("cannot scan FlowGraph contract directory: " + error.message()), {}};
    std::sort(paths.begin(), paths.end());
    return {foundation::Result::success(), std::move(paths)};
}

}  // namespace

foundation::ResultValue<NodeContractFile> read_editor_node_contract(const std::filesystem::path& path) {
    if (path.empty())
        return {foundation::invalid_argument("editor contract path is empty"), {}};
    const auto bytes = read_bytes(path);
    if (!bytes)
        return {bytes.result, {}};
    try {
        const Json root = Json::parse(bytes.value);
        if (!root.is_object() || root.value("schema_version", 0u) != 1u ||
            (!root.contains("editor_contract") && !root.contains("metadata")))
            return {foundation::invalid_argument("editor contract envelope is invalid"), {}};
        const Json& envelope_root = root.contains("editor_contract") ? root.at("editor_contract") : root;
        const auto source = parse_source(envelope_root);
        if (!source)
            return {source.result, {}};
        const auto metadata = root.contains("metadata") ? module::decode_module_metadata(root.at("metadata").dump())
                                                          : module::decode_module_metadata(root.dump());
        if (!metadata || metadata.value.catalog.entries().size() != 1)
            return {metadata ? foundation::invalid_argument("editor contract metadata catalog is invalid")
                             : metadata.result,
                    {}};
        NodeContractFile result{};
        result.path = path;
        result.source = source.value;
        result.draft.source = source.value;
        result.draft.entry = metadata.value.catalog.entries().front();
        result.draft.generated = envelope_root.value("generated", false);
        result.draft.draft_revision_hash = envelope_root.value("draft_revision_hash", std::string{});
        if (envelope_root.contains("retired_port_ids"))
            result.draft.retired_port_ids = envelope_root.at("retired_port_ids").get<std::vector<uint64_t>>();
        if (envelope_root.contains("retired_parameter_ids"))
            result.draft.retired_parameter_ids =
                envelope_root.at("retired_parameter_ids").get<std::vector<uint64_t>>();
        const auto digest = canonical_without_digest(root);
        if (!digest)
            return {digest.result, {}};
        result.digest = root.value("digest", digest.value);
        if (root.contains("digest") && result.digest != digest.value)
            return {foundation::invalid_argument("editor contract digest does not match contents"), {}};
        if (const auto valid = validate_node_definition(result.draft); !valid)
            return {valid, {}};
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::exception& exception) {
        return {foundation::invalid_argument(std::string("editor contract JSON is invalid: ") + exception.what()), {}};
    }
}

foundation::ResultValue<std::string> encode_editor_node_contract(const NodeContractFile& file) {
    if (const auto valid = validate_node_definition(file.draft); !valid)
        return {valid, {}};
    try {
        NodeContractFile canonical = file;
        const auto digest = canonical_without_digest(envelope(canonical, false));
        if (!digest)
            return {digest.result, {}};
        canonical.digest = digest.value;
        return {foundation::Result::success(), envelope(canonical, true).dump(2) + "\n"};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

foundation::ResultValue<std::vector<NodeContractFile>> scan_editor_node_contracts(
    const std::filesystem::path& project_root) {
    if (project_root.empty())
        return {foundation::invalid_argument("project root is empty"), {}};
    const auto paths = contract_paths(project_root);
    if (!paths)
        return {paths.result, {}};
    std::vector<NodeContractFile> result;
    result.reserve(paths.value.size());
    for (const auto& path : paths.value) {
        if (!path_inside(project_root, path))
            return {foundation::invalid_argument("editor contract is outside the project: " + path.string()), {}};
        const auto file = read_editor_node_contract(path);
        if (!file)
            return {file.result, {}};
        result.push_back(file.value);
    }
    return {foundation::Result::success(), std::move(result)};
}

foundation::Result write_editor_node_contract_atomic(const NodeContractFile& file,
                                                     const std::filesystem::path& project_root) {
    if (project_root.empty() || file.path.empty() || !path_inside(project_root, file.path))
        return foundation::invalid_argument("editor contract path is outside the project");
    if (!file.path.filename().string().ends_with(kSuffix) &&
        !file.path.filename().string().ends_with(".flowgraph.draft.json"))
        return foundation::invalid_argument("editor contract path has an invalid suffix");
    if (const auto valid = validate_node_definition(file.draft); !valid)
        return valid;
    try {
        std::filesystem::create_directories(file.path.parent_path());
        const auto encoded = encode_editor_node_contract(file);
        if (!encoded)
            return encoded.result;
        const auto bytes = encoded.value;
        const auto temp = file.path.string() + ".tmp";
        const auto backup = file.path.string() + ".bak";
        {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            if (!output)
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "cannot open editor contract temp file: " + temp);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output)
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "cannot write editor contract temp file: " + temp);
        }
        std::error_code error;
        const bool had_previous = std::filesystem::exists(file.path, error) && !error;
        if (had_previous) {
            std::filesystem::remove(backup, error);
            error.clear();
            std::filesystem::rename(file.path, backup, error);
            if (error) {
                std::filesystem::remove(temp, error);
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "cannot back up editor contract: " + file.path.string());
            }
        }
        std::filesystem::rename(temp, file.path, error);
        if (error) {
            std::filesystem::remove(temp, error);
            if (had_previous) {
                error.clear();
                std::filesystem::rename(backup, file.path, error);
            }
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                       "cannot install editor contract: " + file.path.string());
        }
        std::filesystem::remove(backup, error);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("editor contract atomic write allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

}  // namespace aeg::application

\n