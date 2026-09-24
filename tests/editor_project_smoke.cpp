/**
 * @file editor_project_smoke.cpp
 * @brief Verifies explicit game project creation and import without engine-root discovery.
 */
#include <filesystem>
#include <fstream>
#include <iostream>

#include "editor_project.hpp"
#include "project_context.hpp"

namespace {
bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_project_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    aeg::application::EditorProjectManager manager;
    if (!require(static_cast<bool>(manager.new_project(root, "Smoke Game")),
                 "editor project smoke project creation failed") ||
        !require(manager.loaded(), "editor project smoke project is not loaded") ||
        !require(manager.descriptor().name == "Smoke Game", "editor project smoke project name is invalid") ||
        !require(std::filesystem::is_directory(root / "Assets"), "editor project smoke Assets directory is missing") ||
        !require(std::filesystem::is_directory(root / "Assets" / "Scenes"),
                 "editor project smoke Scenes directory is missing") ||
        !require(std::filesystem::is_directory(root / "Cache"), "editor project smoke Cache directory is missing") ||
        !require(std::filesystem::is_directory(root / "Saved"), "editor project smoke Saved directory is missing") ||
        !require(std::filesystem::is_directory(root / "Binaries"),
                 "editor project smoke Binaries directory is missing") ||
        !require(std::filesystem::exists(root / "CMakeLists.txt"), "editor project smoke CMakeLists.txt is missing") ||
        !require(std::filesystem::exists(root / "Source" / "GameModule.cpp") &&
                     std::filesystem::exists(root / "Source" / "GameModule.metadata.json"),
                 "editor project smoke game module sources are missing") ||
        !require(std::filesystem::exists(root / "Assets" / "Pipelines" / "Default.aegpipe"),
                 "editor project smoke default pipeline asset is missing") ||
        !require(manager.assets_directory().filename() == "Assets",
                 "editor project smoke Assets path is not project-relative") ||
        !require(manager.scenes_directory().filename() == "Scenes",
                 "editor project smoke Scenes path is not project-relative") ||
        !require(manager.default_scene_file().filename() == "Main.aegscene" &&
                     manager.default_scene_file().parent_path().filename() == "Scenes" &&
                     manager.default_scene_file().parent_path().parent_path().filename() == "Assets",
                 "editor project smoke default scene path is incorrect") ||
        !require(std::filesystem::exists(root / "Project.aegproj"), "editor project smoke project file is missing")) {
        std::filesystem::remove_all(root, error);
        return 1;
    }

    std::ifstream project_contents(root / "Project.aegproj");
    const std::string project_text((std::istreambuf_iterator<char>(project_contents)),
                                   std::istreambuf_iterator<char>());
    if (!require(project_text.starts_with("format_version = 2"), "editor project smoke did not write TOML") ||
        !require(project_text.find("[paths]") != std::string::npos &&
                     project_text.find("cache = \"Cache\"") != std::string::npos,
                 "editor project smoke TOML paths are incomplete")) {
        std::filesystem::remove_all(root, error);
        return 6;
    }
    project_contents.close();

    std::ofstream(root / "Assets" / "example.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    aeg::application::EditorProjectManager imported;
    if (!require(static_cast<bool>(imported.import_project(root / "Project.aegproj")),
                 "editor project smoke project-file import failed") ||
        !require(imported.descriptor().root == manager.descriptor().root,
                 "editor project smoke imported root differs") ||
        !require(static_cast<bool>(imported.close_project()), "editor project smoke project close failed") ||
        !require(!imported.loaded(), "editor project smoke close left project loaded")) {
        std::filesystem::remove_all(root, error);
        return 2;
    }

    std::filesystem::remove(root / "Project.aegproj", error);
    if (!require(!imported.import_project(root), "editor project smoke accepted a project without project file")) {
        std::filesystem::remove_all(root, error);
        return 3;
    }

    const auto options_root = std::filesystem::temp_directory_path() / "aeg_editor_project_options_smoke";
    std::filesystem::remove_all(options_root, error);
    aeg::application::ProjectCreateOptions options{};
    options.root = options_root;
    options.name = "Options Game";
    options.engine_sdk = "2.0";
    options.render_pipeline = "Assets/Pipelines/Deferred.aegpipe";
    aeg::application::EditorProjectManager configured;
    if (!require(static_cast<bool>(configured.new_project(options)),
                 "editor project smoke configured project creation failed")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(options_root, error);
        return 8;
    }
    const auto configured_file = aeg::application::project::ProjectFileCodec{}.load(
        options_root / "Project.aegproj");
    if (!require(static_cast<bool>(configured_file), "editor project smoke configured file could not be loaded") ||
        !require(configured_file.value.config.engine.sdk_requirement == "2.0",
                 "editor project smoke did not persist configured SDK") ||
        !require(!configured_file.value.config.startup_pipelines.empty() &&
                     configured_file.value.config.startup_pipelines.front() == options.render_pipeline,
                 "editor project smoke did not persist configured pipeline")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(options_root, error);
        return 9;
    }
    auto updated_config = configured_file.value.config;
    updated_config.name = "Updated Options Game";
    if (!require(static_cast<bool>(configured.update_project_settings(updated_config)),
                 "editor project smoke settings update failed")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(options_root, error);
        return 10;
    }
    const auto updated_file = aeg::application::project::ProjectFileCodec{}.load(
        options_root / "Project.aegproj");
    if (!require(static_cast<bool>(updated_file) && updated_file.value.config.name == "Updated Options Game",
                 "editor project smoke settings update was not persisted")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(options_root, error);
        return 11;
    }

    std::filesystem::create_directories(root / "Scenes", error);
    if (!require(!error, "editor project smoke cannot create legacy scene directory")) {
        std::filesystem::remove_all(root, error);
        return 7;
    }
    std::ofstream(root / "project.aegproject")
        << "AEG_PROJECT\t2\nSmoke Game\nassets=Assets\nscenes=Scenes\ndefault_scene=Scenes/Main.aegscene\n";
    const auto legacy_import = imported.import_project(root);
    if (!legacy_import) {
        std::cerr << "editor project smoke legacy import diagnostic: " << legacy_import.message << '\n';
    }
    if (!require(static_cast<bool>(legacy_import), "editor project smoke versioned project import failed") ||
        !require(static_cast<bool>(imported.remember_scene(root / "Scenes" / "Example.aegscene")),
                 "editor project smoke recent scene write failed") ||
        !require(imported.recent_scene().filename() == "Example.aegscene" &&
                     imported.recent_scene().parent_path().filename() == "Scenes",
                 "editor project smoke recent scene was not resolved from the project root") ||
        !require(!imported.remember_scene(std::filesystem::temp_directory_path() / "outside.aegscene"),
                 "editor project smoke accepted a scene outside the project")) {
        std::filesystem::remove_all(root, error);
        return 4;
    }
    std::filesystem::remove_all(root, error);
    std::cout << "Editor project smoke passed\n";
    return 0;
}
