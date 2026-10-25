/**
 * @file editor_project_feature_authoring.cpp
 * @brief Loads editable FeatureGraph documents and module declarations from a project checkout.
 */
#include "editor_project_feature_authoring.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "feature_graph_io.hpp"
#include "module_bundle.hpp"

namespace aeg::application {
namespace {

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
        if (iterator->is_regular_file(error) && !error) {
            auto candidate = iterator->path().extension().string();
            for (char& character : candidate)
                if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
            if (candidate == extension) result.push_back(iterator->path());
        }
    }
    if (error)
        return {foundation::invalid_argument("Could not scan project authoring directory: " + error.message()), {}};
    std::sort(result.begin(), result.end());
    return {foundation::Result::success(), std::move(result)};
}

}  // namespace

foundation::ResultValue<EditorProjectFeatureAuthoring> load_editor_project_feature_authoring(
    const std::filesystem::path& project_root, const std::filesystem::path& source_directory,
    const std::filesystem::path& assets_directory) {
    if (project_root.empty() || source_directory.empty() || assets_directory.empty())
        return {foundation::invalid_argument("Project authoring paths are invalid"), {}};

    EditorProjectFeatureAuthoring result{};
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
        for (const feature::FeatureCatalogEntry& entry : metadata.value.feature_catalog.entries()) {
            if (const foundation::Result added = result.feature_catalog.add_entry(entry); !added)
                return {added, {}};
            result.compiled_feature_ids.push_back(entry.definition_id);
        }
    }

    // Editor contracts are an overlay for types that do not yet exist in
    // compiled metadata. Matching envelopes contribute labels only; a changed
    // contract keeps the metadata authority and remains an explicit conflict.
    const auto contracts = scan_editor_node_contracts(project_root, source_directory, &result.contract_diagnostics);
    if (!contracts) return {contracts.result, {}};
    const auto remember_presentation = [&](const NodeDefinitionDraft& draft) {
        const auto previous = std::find_if(result.presentations.begin(), result.presentations.end(),
            [&](const auto& value) { return value.entry.id == draft.entry.id; });
        if (previous == result.presentations.end()) result.presentations.push_back(draft);
        // A matching recovery draft may rename a generated contract without
        // changing its meaning. Never use a changed contract as label metadata.
        else if (previous->generated && !draft.generated) *previous = draft;
    };
    for (const auto& file : contracts.value) {
        const auto& path = file.path;
        const foundation::ResultValue<NodeContractFile> contract{foundation::Result::success(), file};
        if (!contract.value.draft.generated)
            result.drafts.push_back(contract.value);
        else
            result.contracts.push_back(contract.value);
        const auto compiled = std::find_if(result.feature_catalog.entries().begin(), result.feature_catalog.entries().end(),
            [&](const auto& entry) { return entry.definition_id == contract.value.draft.entry.id; });
        if (compiled != result.feature_catalog.entries().end()) {
            const auto& definition = contract.value.draft.entry;
            if (compiled->owner_module == definition.owner_module && compiled->version == definition.version &&
                compiled->contract_digest == feature::feature_contract_digest(definition)) {
                remember_presentation(contract.value.draft);
                continue;
            }
            result.conflicts.push_back({contract.value.draft.entry.id, contract.value.draft, *compiled,
                                        "editor draft conflicts with compiled contract"});
            continue;
        }
        // A draft is an editor recovery artifact. It must remain visible in
        // `drafts`, but it cannot become the runtime catalog authority until
        // Save & Generate Stub installs a generated contract.
        if (!contract.value.draft.generated)
            continue;
        if (const auto added = result.feature_catalog.export_definition(contract.value.draft.entry); !added)
            result.contract_diagnostics.push_back(path.string() + ": " + added.message);
        else remember_presentation(contract.value.draft);
    }

    const auto feature_files = files_with_extension(assets_directory, ".aegfeature");
    if (!feature_files)
        return {feature_files.result, {}};
    try {
        result.feature_graphs.reserve(feature_files.value.size());
        for (const std::filesystem::path& path : feature_files.value) {
            // FeatureGraph is the canonical authoring format for .aegfeature.
            // Keep the source path and diagnostic together so a malformed
            // graph remains visible to the editor without aborting the whole
            // project load.
            const auto graph = authoring::load_feature_graph_document(path);
            if (graph) {
                result.feature_graphs.push_back({path, std::move(graph.value)});
                continue;
            }
            if (graph.result.code == foundation::ResultCode::kOutOfMemory ||
                graph.result.code == foundation::ResultCode::kDeviceLost)
                return {graph.result, {}};
            result.feature_graphs.push_back({path, {}, graph.result});
        }

        // Diagnose retired formats without invoking their reader or accepting
        // renamed Pipeline payloads as FeatureGraph documents.
        for (const auto extension : {".aegframe", ".aegpipe"}) {
            const auto retired = files_with_extension(assets_directory, extension);
            if (!retired) return {retired.result, {}};
            for (const auto& path : retired.value)
                result.contract_diagnostics.push_back(path.string() + ": Unsupported retired authoring asset " + extension);
        }
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Project FeatureGraph authoring data allocation failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}

}  // namespace aeg::application
