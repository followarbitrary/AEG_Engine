/**
 * @file editor_project_pipeline_authoring.hpp
 * @brief Project-local Pipeline authoring data available before a game module is built.
 */
#pragma once

#include <filesystem>
#include <vector>

#include "pipeline_authoring.hpp"
#include "pipeline_document.hpp"
#include "result.hpp"

namespace aeg::application {

struct EditorProjectPipelineDocument final {
    std::filesystem::path path{};
    pipeline::PipelineDocument document{};
    /** Keeps unreadable project files visible without failing the entire authoring catalog. */
    foundation::Result load_result{};
};

struct EditorProjectPipelineAuthoring final {
    pipeline::TypeCatalog catalog{};
    std::vector<EditorProjectPipelineDocument> documents{};
};

/** Loads source module metadata and authored .aegpipe files without loading a game DLL. */
[[nodiscard]] foundation::ResultValue<EditorProjectPipelineAuthoring> load_editor_project_pipeline_authoring(
    const std::filesystem::path& project_root, const std::filesystem::path& source_directory,
    const std::filesystem::path& assets_directory);

}  // namespace aeg::application
