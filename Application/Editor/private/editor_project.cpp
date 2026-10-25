/**
 * @file editor_project.cpp
 * @brief Implements the editor game project lifecycle.
 */
#include "editor_project.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "project_context.hpp"
#include "project_scaffold.hpp"

namespace aeg::application {
namespace {
[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] std::filesystem::path source_path(std::string_view text) {
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

[[nodiscard]] EditorProjectDescriptor make_descriptor(const std::filesystem::path& root,
                                                      const project::ProjectConfig& config) {
    return {root, config.name, config.paths.assets, config.startup_scene.parent_path(), config.startup_scene};
}

[[nodiscard]] std::filesystem::path canonical_or_absolute(const std::filesystem::path& path, std::error_code& error) {
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        result = std::filesystem::absolute(path, error);
    }
    return result;
}

[[nodiscard]] bool valid_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::filesystem::path global_recent_projects_file() {
    static const std::filesystem::path kPath = [] {
        const char* app_data = std::getenv("APPDATA");
        if (app_data != nullptr && *app_data != '\0') {
            return std::filesystem::path(std::string(app_data)) / "AEG" / "recent_projects.tsv";
        }
        const char* home = std::getenv("USERPROFILE");
        if (home != nullptr && *home != '\0') {
            return std::filesystem::path(std::string(home)) / ".aeg" / "recent_projects.tsv";
        }
        return std::filesystem::temp_directory_path() / "AEG" / "recent_projects.tsv";
    }();
    return kPath;
}
}  // namespace

foundation::Result EditorProjectManager::new_project(const std::filesystem::path& root, std::string_view name) {
    return new_project(ProjectCreateOptions{root, std::string(name)});
}

foundation::Result EditorProjectManager::new_project(const ProjectCreateOptions& options) {
    const auto created = project::create_project(
        {options.root, options.name, "GameModule", options.template_id, options.engine_sdk, options.startup_feature_graph});
    if (!created)
        return created.result;
    return import_project(created.value);
}

foundation::Result EditorProjectManager::import_project(const std::filesystem::path& root_or_project_file) {
    if (root_or_project_file.empty()) {
        return foundation::invalid_argument("Project path is empty");
    }
    const auto discovered = project::discover_project_file(root_or_project_file);
    if (!discovered) return discovered.result;
    std::error_code error;
    auto selected_file = canonical_or_absolute(discovered.value, error);
    if (error || selected_file.empty())
        return foundation::invalid_argument("Selected project file cannot be resolved");
    EditorProjectDescriptor next{};
    if (const foundation::Result result = read_project_file(selected_file, next); !result)
        return result;
    descriptor_ = std::move(next);
    project_file_ = std::move(selected_file);
    const auto recent = recent_projects();
    std::vector<EditorRecentProject> entries = recent ? std::move(recent.value) : std::vector<EditorRecentProject>{};
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [this](const EditorRecentProject& entry) { return entry.project_file == project_file_; }),
                  entries.end());
    entries.insert(entries.begin(), EditorRecentProject{descriptor_.root, descriptor_.name, project_file_});
    if (entries.size() > 8) {
        entries.resize(8);
    }
    std::error_code recent_error;
    std::filesystem::create_directories(global_recent_projects_file().parent_path(), recent_error);
    if (!recent_error) {
        std::ofstream recent_file(global_recent_projects_file(), std::ios::binary | std::ios::trunc);
        if (recent_file) {
            for (const EditorRecentProject& entry : entries) {
                recent_file << path_text(entry.root) << '\t' << entry.name << '\t' << path_text(entry.project_file) << '\n';
            }
        }
    }
    return foundation::Result::success();
}

foundation::Result EditorProjectManager::close_project() noexcept {
    descriptor_ = {};
    project_file_.clear();
    return foundation::Result::success();
}

foundation::Result EditorProjectManager::update_project_settings(const project::ProjectConfig& config) {
    if (!loaded()) {
        return foundation::invalid_state("Cannot update settings without an open project");
    }
    const std::filesystem::path file = project_file();
    project::ProjectFileCodec codec;
    auto existing = codec.load(file);
    if (!existing) {
        return existing.result;
    }
    existing.value.config = config;
    if (const foundation::Result result = codec.save(file, existing.value); !result) {
        return result;
    }
    descriptor_ = make_descriptor(descriptor_.root, config);
    return foundation::Result::success();
}

std::filesystem::path EditorProjectManager::project_file() const {
    return loaded() ? project_file_ : std::filesystem::path{};
}

std::filesystem::path EditorProjectManager::assets_directory() const {
    return loaded() ? descriptor_.root / descriptor_.assets_directory : std::filesystem::path{};
}

std::filesystem::path EditorProjectManager::scenes_directory() const {
    return loaded() ? descriptor_.root / descriptor_.scenes_directory : std::filesystem::path{};
}

std::filesystem::path EditorProjectManager::default_scene_file() const {
    return loaded() ? descriptor_.root / descriptor_.default_scene : std::filesystem::path{};
}

std::filesystem::path EditorProjectManager::recent_state_file() const {
    return descriptor_.root / ".aeg" / "editor_state.tsv";
}

std::filesystem::path EditorProjectManager::recent_scene() const {
    if (!loaded()) {
        return {};
    }
    std::ifstream file(recent_state_file(), std::ios::binary);
    std::string relative;
    if (!file || !std::getline(file, relative) || relative.empty()) {
        return {};
    }
    if (relative.back() == '\r') relative.pop_back();
    std::filesystem::path decoded;
    try {
        decoded = source_path(relative);
    } catch (const std::system_error&) {
        // Remembered UI state is optional. Preserve undecodable cache bytes
        // and let startup use the project's supported default scene.
        return {};
    }
    const auto resolved = resolve_scene_path(decoded);
    return resolved ? resolved.value : std::filesystem::path{};
}

foundation::Result EditorProjectManager::remember_scene(const std::filesystem::path& path) const {
    if (!loaded() || path.empty()) {
        return foundation::invalid_argument("Cannot remember a scene without an open project and scene path");
    }
    const auto resolved = resolve_scene_path(path);
    if (!resolved) {
        return foundation::invalid_argument("Recent scene path must be inside the game project");
    }
    const std::filesystem::path relative = resolved.value.lexically_relative(descriptor_.root);
    if (relative.empty() || !valid_relative_path(relative)) {
        return foundation::invalid_argument("Recent scene path must be inside the game project");
    }
    std::error_code error;
    std::filesystem::create_directories(recent_state_file().parent_path(), error);
    if (error) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "Cannot create the project editor state directory");
    }
    std::ofstream file(recent_state_file(), std::ios::binary | std::ios::trunc);
    if (!file) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write the project editor state");
    }
    file << path_text(relative) << '\n';
    return file
               ? foundation::Result::success()
               : foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write the project editor state");
}

foundation::ResultValue<std::filesystem::path> EditorProjectManager::resolve_scene_path(
    const std::filesystem::path& path) const {
    if (!loaded() || path.empty()) {
        return {foundation::invalid_argument("Cannot resolve a scene without an open project"), {}};
    }
    std::error_code error;
    const std::filesystem::path candidate = path.is_absolute() ? path : descriptor_.root / path;
    const std::filesystem::path resolved = canonical_or_absolute(candidate, error);
    if (error || resolved.empty()) {
        return {foundation::invalid_argument("Scene path is invalid"), {}};
    }
    const std::filesystem::path relative = resolved.lexically_relative(descriptor_.root);
    if (relative.empty() || !valid_relative_path(relative)) {
        return {foundation::invalid_argument("Scene path must be inside the game project"), {}};
    }
    return {foundation::Result::success(), resolved};
}

foundation::ResultValue<std::vector<EditorRecentProject>> EditorProjectManager::recent_projects() const {
    std::vector<EditorRecentProject> entries;
    std::ifstream file(global_recent_projects_file(), std::ios::binary);
    if (!file) {
        return {foundation::Result::success(), std::move(entries)};
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t separator = line.find('\t');
        const std::size_t source_separator = separator == std::string::npos ? separator : line.find('\t', separator + 1);
        // Old UI records held only a directory and cannot recover the selected
        // source. Skip those records rather than guessing a canonical file.
        if (separator == std::string::npos || separator == 0 || source_separator == std::string::npos ||
            source_separator == separator + 1 || source_separator + 1 >= line.size() ||
            line.find('\t', source_separator + 1) != std::string::npos) {
            continue;
        }
        std::filesystem::path root, source;
        try {
            root = source_path(std::string_view{line}.substr(0, separator));
            source = source_path(std::string_view{line}.substr(source_separator + 1));
        } catch (const std::system_error&) {
            // This UI cache is not a project source. An undecodable row must
            // not interrupt a valid import or hide its other recent records.
            continue;
        }
        entries.push_back({std::move(root), line.substr(separator + 1, source_separator - separator - 1),
            std::move(source)});
    }
    return {foundation::Result::success(), std::move(entries)};
}

foundation::Result EditorProjectManager::read_project_file(const std::filesystem::path& selected_file,
                                                           EditorProjectDescriptor& descriptor) const {
    const auto parsed = project::ProjectFileCodec{}.load(selected_file);
    if (!parsed) return parsed.result;
    const auto root = selected_file.parent_path();
    auto next = make_descriptor(root, parsed.value.config);
    std::error_code error;
    if (!std::filesystem::is_directory(root / next.assets_directory, error) || error ||
        !std::filesystem::is_directory(root / next.scenes_directory, error) || error)
        return foundation::invalid_argument("Project does not contain configured asset and scene directories");
    descriptor = std::move(next);
    return foundation::Result::success();
}

}  // namespace aeg::application
