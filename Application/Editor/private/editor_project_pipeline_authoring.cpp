/**
 * @file editor_project_pipeline_authoring.cpp
 * @brief Loads editable Pipeline documents and module declarations from a project checkout.
 */
#include "editor_project_pipeline_authoring.hpp"

#include <algorithm>
#include <system_error>

#include "module_bundle.hpp"
#include "pipeline_document_io.hpp"

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

    const auto pipeline_files = files_with_extension(assets_directory, ".aegpipe");
    if (!pipeline_files)
        return {pipeline_files.result, {}};
    try {
        result.documents.reserve(pipeline_files.value.size());
        for (const std::filesystem::path& path : pipeline_files.value) {
            const auto document = project::load_pipeline_document(path);
            if (!document)
                return {document.result, {}};
            result.documents.push_back({path, std::move(document.value)});
        }
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Project Pipeline authoring data allocation failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}

}  // namespace aeg::application
