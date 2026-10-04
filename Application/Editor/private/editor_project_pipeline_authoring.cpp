/**
 * @file editor_project_pipeline_authoring.cpp
 * @brief Loads editable Pipeline documents and module declarations from a project checkout.
 */
#include "editor_project_pipeline_authoring.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "module_bundle.hpp"
#include "pipeline_document_io.hpp"
#include "renderer_variants.hpp"

namespace aeg::application {
namespace {

std::vector<std::filesystem::path> flowgraph_contract_paths(const std::filesystem::path& project_root) {
    std::vector<std::filesystem::path> paths;
    const auto directory = project_root / "Source" / "FlowGraph";
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error)
        return paths;
    for (std::filesystem::recursive_directory_iterator iterator{
             directory, std::filesystem::directory_options::skip_permission_denied, error};
         !error && iterator != std::filesystem::recursive_directory_iterator{}; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error &&
            (iterator->path().filename().string().ends_with(".flowgraph.contract.json") ||
             iterator->path().filename().string().ends_with(".flowgraph.draft.json")))
            paths.push_back(iterator->path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

[[nodiscard]] bool has_metadata_extension(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name.size() > std::string_view{".metadata.json"}.size() && name.ends_with(".metadata.json");
}

[[nodiscard]] foundation::ResultValue<std::vector<std::filesystem::path>> files_with_extension(
    const std::filesystem::path& directory, std::string_view extension) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error)
            return {foundation::invalid_argument("Could not inspect project authoring directory: " + error.message()),
                    {}};
        return {foundation::Result::success(), std::move(result)};
    }
    for (std::filesystem::recursive_directory_iterator iterator{
             directory, std::filesystem::directory_options::skip_permission_denied, error};
         !error && iterator != std::filesystem::recursive_directory_iterator{}; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error && iterator->path().extension() == extension)
            result.push_back(iterator->path());
    }
    if (error)
        return {foundation::invalid_argument("Could not scan project authoring directory: " + error.message()), {}};
    std::sort(result.begin(), result.end());
    return {foundation::Result::success(), std::move(result)};
}

}  // namespace

foundation::ResultValue<EditorProjectPipelineAuthoring> load_editor_project_pipeline_authoring(
    const std::filesystem::path& project_root, const std::filesystem::path& source_directory,
    const std::filesystem::path& assets_directory) {
    if (project_root.empty() || source_directory.empty() || assets_directory.empty())
        return {foundation::invalid_argument("Project authoring paths are invalid"), {}};

    EditorProjectPipelineAuthoring result{};
    if (const foundation::Result builtins = renderer::register_renderer_authoring_types(result.catalog); !builtins) {
        return {builtins, {}};
    }
    const auto metadata_files = files_with_extension(source_directory, ".json");
    if (!metadata_files)
        return {metadata_files.result, {}};
    for (const std::filesystem::path& path : metadata_files.value) {
        if (!has_metadata_extension(path))
            continue;
        const auto metadata = module::read_module_metadata(path);
        if (!metadata)
            return {metadata.result, {}};
        // Source metadata intentionally has no contract digest until a game module is built.
        // Its declarations are still the editor's authoritative authoring schema.
        for (const pipeline::TypeCatalogEntry& entry : metadata.value.catalog.entries()) {
            if (const foundation::Result added = result.catalog.add(entry); !added)
                return {added, {}};
        }
    }

    // Editor contracts are an overlay for types that do not yet exist in
    // compiled metadata. When both sources describe the same stable identity,
    // retain the compiled entry and expose an explicit conflict for the UI.
    for (const auto& path : flowgraph_contract_paths(project_root)) {
        const auto contract = read_editor_node_contract(path);
        if (!contract) {
            result.contract_diagnostics.push_back(path.string() + ": " + contract.result.message);
            continue;
        }
        if (!contract.value.draft.generated)
            result.drafts.push_back(contract.value);
        else
            result.contracts.push_back(contract.value);
        const auto* compiled = result.catalog.find(contract.value.draft.entry.key);
        if (compiled) {
            result.conflicts.push_back({contract.value.draft.entry.key, contract.value.draft, *compiled,
                                        "editor draft conflicts with compiled contract"});
            continue;
        }
        // A draft is an editor recovery artifact. It must remain visible in
        // `drafts`, but it cannot become the runtime catalog authority until
        // Save & Generate Stub installs a generated contract.
        if (!contract.value.draft.generated)
            continue;
        if (const auto added = result.catalog.add(contract.value.draft.entry); !added)
            result.contract_diagnostics.push_back(path.string() + ": " + added.message);
    }

    const auto pipeline_files = files_with_extension(assets_directory, ".aegpipe");
    if (!pipeline_files)
        return {pipeline_files.result, {}};
    try {
        result.documents.reserve(pipeline_files.value.size());
        for (const std::filesystem::path& path : pipeline_files.value) {
            const auto document = project::load_pipeline_document(path);
            if (!document) {
                if (document.result.code == foundation::ResultCode::kOutOfMemory ||
                    document.result.code == foundation::ResultCode::kDeviceLost)
                    return {document.result, {}};
                result.documents.push_back({path, {}, document.result});
                continue;
            }
            result.documents.push_back({path, std::move(document.value)});
        }
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Project Pipeline authoring data allocation failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}

}  // namespace aeg::application
