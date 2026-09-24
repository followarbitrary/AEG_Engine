/**
 * @file editor_project.cpp
 * @brief Implements the editor game project lifecycle.
 */
#include "editor_project.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <system_error>

#include "project_context.hpp"
#include "project_scaffold.hpp"

namespace aeg::application {
namespace {
constexpr std::string_view kProjectHeader{"AEG_PROJECT\t2"};
constexpr std::string_view kLegacyProjectHeader{"AEG_PROJECT\t1"};

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

[[nodiscard]] bool valid_project_name(std::string_view name) noexcept {
    return !name.empty() && name.find_first_of("\t\r\n") == std::string_view::npos;
}

[[nodiscard]] std::string project_uuid_for(const std::filesystem::path& root) {
    const std::string seed = root.generic_string();
    const uint64_t first = std::hash<std::string>{}(seed);
    const uint64_t second = std::hash<std::string>{}(seed + "#aeg-project");
    std::ostringstream hex;
    hex << std::hex << std::setfill('0') << std::setw(16) << first << std::setw(16) << second;
    std::string raw = hex.str();
    raw[12] = '4';
    raw[16] = '8';
    return raw.substr(0, 8) + "-" + raw.substr(8, 4) + "-" + raw.substr(12, 4) + "-" + raw.substr(16, 4) + "-" +
           raw.substr(20, 12);
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
        {options.root, options.name, "GameModule", options.template_id, options.engine_sdk, options.render_pipeline});
    if (!created)
        return created.result;
    return import_project(created.value);
}

foundation::Result EditorProjectManager::import_project(const std::filesystem::path& root_or_project_file) {
    if (root_or_project_file.empty()) {
        return foundation::invalid_argument("Project path is empty");
    }
    std::error_code error;
    std::filesystem::path root = root_or_project_file;
    if (root.extension() == ".aegproj" || root.extension() == ".aegproject") {
        root = root.parent_path();
    }
    root = canonical_or_absolute(root, error);
    if (error || !std::filesystem::is_directory(root, error) || error) {
        return foundation::not_found("Game project directory does not exist");
    }
    EditorProjectDescriptor next{};
    if (const foundation::Result result = read_project_file(root, next); !result) {
        return result;
    }
    descriptor_ = std::move(next);
    const auto recent = recent_projects();
    std::vector<EditorRecentProject> entries = recent ? std::move(recent.value) : std::vector<EditorRecentProject>{};
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [this](const EditorRecentProject& entry) { return entry.root == descriptor_.root; }),
                  entries.end());
    entries.insert(entries.begin(), EditorRecentProject{descriptor_.root, descriptor_.name});
    if (entries.size() > 8) {
        entries.resize(8);
    }
    std::error_code recent_error;
    std::filesystem::create_directories(global_recent_projects_file().parent_path(), recent_error);
    if (!recent_error) {
        std::ofstream recent_file(global_recent_projects_file(), std::ios::trunc);
        if (recent_file) {
            for (const EditorRecentProject& entry : entries) {
                recent_file << entry.root.string() << '\t' << entry.name << '\n';
            }
        }
    }
    return foundation::Result::success();
}

foundation::Result EditorProjectManager::close_project() noexcept {
    descriptor_ = {};
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
    if (std::filesystem::exists(descriptor_.root / "Project.aegproj")) {
        return descriptor_.root / "Project.aegproj";
    }
    return descriptor_.root / "project.aegproject";
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
    std::ifstream file(recent_state_file());
    std::string relative;
    if (!file || !std::getline(file, relative) || relative.empty()) {
        return {};
    }
    const auto resolved = resolve_scene_path(relative);
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
    std::ofstream file(recent_state_file(), std::ios::trunc);
    if (!file) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write the project editor state");
    }
    file << relative.generic_string() << '\n';
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
    std::ifstream file(global_recent_projects_file());
    if (!file) {
        return {foundation::Result::success(), std::move(entries)};
    }
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t separator = line.find('\t');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size()) {
            continue;
        }
        entries.push_back({std::filesystem::path(line.substr(0, separator)), line.substr(separator + 1)});
    }
    return {foundation::Result::success(), std::move(entries)};
}

foundation::Result EditorProjectManager::read_project_file(const std::filesystem::path& root,
                                                           EditorProjectDescriptor& descriptor) const {
    project::ProjectFileCodec codec;
    const auto discovered = project::discover_project_file(root);
    const std::filesystem::path project_path = discovered ? discovered.value : root / "project.aegproject";
    const auto modern_project = codec.load(project_path);
    if (modern_project) {
        EditorProjectDescriptor parsed = make_descriptor(root, modern_project.value.config);
        std::error_code error;
        if (!std::filesystem::is_directory(root / parsed.assets_directory, error) || error ||
            !std::filesystem::is_directory(root / parsed.scenes_directory, error) || error) {
            return foundation::invalid_argument("Project does not contain configured asset and scene directories");
        }
        descriptor = std::move(parsed);
        return foundation::Result::success();
    }

    std::ifstream file(project_path);
    if (!file) {
        return foundation::not_found("Project file Project.aegproj or project.aegproject is missing");
    }
    std::string header;
    std::string name;
    if (!std::getline(file, header) || (header != kProjectHeader && header != kLegacyProjectHeader) ||
        !std::getline(file, name) || !valid_project_name(name)) {
        return foundation::invalid_argument("Project file is malformed");
    }
    EditorProjectDescriptor parsed{root, std::move(name), "Assets", "Scenes", "Scenes/Main.aegscene"};
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            return foundation::invalid_argument("Project file contains an invalid setting");
        }
        const std::string key = line.substr(0, separator);
        const std::filesystem::path value = line.substr(separator + 1);
        if (key == "assets" && valid_relative_path(value)) {
            parsed.assets_directory = value.lexically_normal();
        } else if (key == "scenes" && valid_relative_path(value)) {
            parsed.scenes_directory = value.lexically_normal();
        } else if (key == "default_scene" && valid_relative_path(value)) {
            parsed.default_scene = value.lexically_normal();
        } else if (key != "assets" && key != "scenes" && key != "default_scene") {
            continue;
        } else {
            return foundation::invalid_argument("Project file contains an invalid path setting");
        }
    }
    std::error_code error;
    if (!std::filesystem::is_directory(root / parsed.assets_directory, error) || error ||
        !std::filesystem::is_directory(root / parsed.scenes_directory, error) || error) {
        return foundation::invalid_argument("Project does not contain an Assets directory");
    }
    file.close();
    if (const foundation::Result result = write_project_file(parsed); !result) {
        return result;
    }
    descriptor = std::move(parsed);
    return foundation::Result::success();
}

foundation::Result EditorProjectManager::write_project_file(const EditorProjectDescriptor& descriptor) const {
    project::ProjectConfig config{};
    config.name = descriptor.name;
    config.startup_scene = descriptor.default_scene;
    config.paths.assets = descriptor.assets_directory;
    return project::ProjectFileCodec{}.save(descriptor.root / "Project.aegproj", config);
}

}  // namespace aeg::application
