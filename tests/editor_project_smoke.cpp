/**
 * @file editor_project_smoke.cpp
 * @brief Verifies explicit game project creation and import without engine-root discovery.
 */
#include <filesystem>
#include <fstream>
#include <iostream>

#include "editor_project.hpp"

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
        !require(std::filesystem::is_directory(root / "Scenes"), "editor project smoke Scenes directory is missing") ||
        !require(manager.assets_directory().filename() == "Assets",
                 "editor project smoke Assets path is not project-relative") ||
        !require(manager.scenes_directory().filename() == "Scenes",
                 "editor project smoke Scenes path is not project-relative") ||
        !require(manager.default_scene_file().filename() == "Main.aegscene" &&
                     manager.default_scene_file().parent_path().filename() == "Scenes",
                 "editor project smoke default scene path is incorrect") ||
        !require(std::filesystem::exists(root / "project.aegproject"),
                 "editor project smoke project file is missing")) {
        std::filesystem::remove_all(root, error);
        return 1;
    }

    std::ofstream(root / "Assets" / "example.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    aeg::application::EditorProjectManager imported;
    if (!require(static_cast<bool>(imported.import_project(root / "project.aegproject")),
                 "editor project smoke project-file import failed") ||
        !require(imported.descriptor().root == manager.descriptor().root,
                 "editor project smoke imported root differs") ||
        !require(static_cast<bool>(imported.close_project()), "editor project smoke project close failed") ||
        !require(!imported.loaded(), "editor project smoke close left project loaded")) {
        std::filesystem::remove_all(root, error);
        return 2;
    }

    std::filesystem::remove(root / "project.aegproject", error);
    if (!require(!imported.import_project(root), "editor project smoke accepted a project without project file")) {
        std::filesystem::remove_all(root, error);
        return 3;
    }
    std::ofstream(root / "project.aegproject")
        << "AEG_PROJECT\t2\nSmoke Game\nassets=Assets\nscenes=Scenes\ndefault_scene=Scenes/Main.aegscene\n";
    if (!require(static_cast<bool>(imported.import_project(root)),
                 "editor project smoke versioned project import failed") ||
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
