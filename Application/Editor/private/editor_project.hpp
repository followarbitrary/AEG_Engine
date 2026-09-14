/**
 * @file editor_project.hpp
 * @brief Editor-owned game project discovery and project file contract.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "result.hpp"

namespace aeg::application {

/** @brief Identity of the currently opened game project. */
struct EditorProjectDescriptor final {
    std::filesystem::path root{};
    std::string name{};
    std::filesystem::path assets_directory{"Assets"};
    std::filesystem::path scenes_directory{"Scenes"};
    std::filesystem::path default_scene{"Scenes/Main.aegscene"};
};

/** @brief One project entry persisted in the editor's recent-project list. */
struct EditorRecentProject final {
    std::filesystem::path root{};
    std::string name{};
};

/** @brief Creates and opens game projects without treating the engine checkout as a project. */
class EditorProjectManager final {
public:
    EditorProjectManager() = default;

    [[nodiscard]] foundation::Result new_project(const std::filesystem::path& root, std::string_view name);
    [[nodiscard]] foundation::Result import_project(const std::filesystem::path& root_or_project_file);
    [[nodiscard]] foundation::Result close_project() noexcept;

    [[nodiscard]] bool loaded() const noexcept { return descriptor_.root.has_filename(); }
    [[nodiscard]] const EditorProjectDescriptor& descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] std::filesystem::path project_file() const;
    [[nodiscard]] std::filesystem::path assets_directory() const;
    [[nodiscard]] std::filesystem::path scenes_directory() const;
    [[nodiscard]] std::filesystem::path default_scene_file() const;
    [[nodiscard]] std::filesystem::path recent_state_file() const;
    [[nodiscard]] std::filesystem::path recent_scene() const;
    [[nodiscard]] foundation::Result remember_scene(const std::filesystem::path& path) const;
    [[nodiscard]] foundation::ResultValue<std::vector<EditorRecentProject>> recent_projects() const;
    [[nodiscard]] foundation::ResultValue<std::filesystem::path> resolve_scene_path(
        const std::filesystem::path& path) const;

private:
    [[nodiscard]] foundation::Result read_project_file(const std::filesystem::path& root,
                                                       EditorProjectDescriptor& descriptor) const;
    [[nodiscard]] foundation::Result write_project_file(const EditorProjectDescriptor& descriptor) const;

    EditorProjectDescriptor descriptor_{};
};

}  // namespace aeg::application
