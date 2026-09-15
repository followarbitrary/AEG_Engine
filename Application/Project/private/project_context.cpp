/**
 * @file project_context.cpp
 * @brief TOML project configuration and contained-path implementation.
 */
#include "project_context.hpp"

#include <fstream>
#include <limits>
#include <system_error>
#include <toml.hpp>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aeg::application::project {
namespace {
constexpr uint32_t kCurrentFormatVersion = 1;

[[nodiscard]] bool is_safe_relative_path(const std::filesystem::path& path) noexcept {
    if (path.empty() || path.is_absolute() || path.has_root_directory() || path.has_root_name()) {
        return false;
    }

    const std::filesystem::path normalized = path.lexically_normal();
    for (const auto& component : normalized) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] foundation::Result invalid_project_path() {
    return foundation::invalid_argument("Project paths must be non-empty relative paths under the project root");
}

[[nodiscard]] foundation::Result validate_paths(const ProjectPathConfig& config) {
    if (!is_safe_relative_path(config.assets) || !is_safe_relative_path(config.cache) ||
        !is_safe_relative_path(config.saved) || !is_safe_relative_path(config.binaries)) {
        return invalid_project_path();
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result validate_config(const ProjectConfig& config) {
    if (config.format_version != kCurrentFormatVersion || config.name.empty() || config.target.empty() ||
        !is_safe_relative_path(config.startup_scene)) {
        return foundation::invalid_argument("Project configuration is invalid");
    }
    return validate_paths(config.paths);
}

[[nodiscard]] foundation::ResultValue<std::filesystem::path> normalize_project_root(
    const std::filesystem::path& project_file) {
    if (project_file.empty() || project_file.filename().empty()) {
        return {foundation::invalid_argument("Project file path is invalid"), {}};
    }

    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(project_file.parent_path(), error);
    if (error || root.empty()) {
        return {foundation::invalid_argument("Project root cannot be resolved"), {}};
    }
    return {foundation::Result::success(), root};
}

[[nodiscard]] foundation::Result write_text(const std::filesystem::path& file, std::string_view text) {
    const std::filesystem::path temporary = file.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot open project file for writing");
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    if (!stream) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write project file");
    }
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace project file");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace project file");
    }
#endif
    return foundation::Result::success();
}
}  // namespace

foundation::ResultValue<ProjectPaths> ProjectPaths::resolve(const std::filesystem::path& project_file,
                                                            const ProjectPathConfig& config) {
    if (const foundation::Result result = validate_paths(config); !result) {
        return {result, {}};
    }

    const auto root = normalize_project_root(project_file);
    if (!root) {
        return {root.result, {}};
    }

    ProjectPaths paths{};
    paths.root_ = root.value;
    paths.assets_ = (paths.root_ / config.assets).lexically_normal();
    paths.cache_ = (paths.root_ / config.cache).lexically_normal();
    paths.saved_ = (paths.root_ / config.saved).lexically_normal();
    paths.binaries_ = (paths.root_ / config.binaries).lexically_normal();
    return {foundation::Result::success(), std::move(paths)};
}

const std::filesystem::path& ProjectPaths::directory(ProjectDirectory directory) const noexcept {
    switch (directory) {
        case ProjectDirectory::kRoot:
            return root_;
        case ProjectDirectory::kAssets:
            return assets_;
        case ProjectDirectory::kCache:
            return cache_;
        case ProjectDirectory::kSaved:
            return saved_;
        case ProjectDirectory::kBinaries:
            return binaries_;
    }
    return root_;
}

foundation::ResultValue<std::filesystem::path> ProjectPaths::resolve_under(
    ProjectDirectory directory, const std::filesystem::path& relative) const {
    if (!is_safe_relative_path(relative)) {
        return {invalid_project_path(), {}};
    }
    return {foundation::Result::success(), (this->directory(directory) / relative).lexically_normal()};
}

foundation::ResultValue<ProjectFile> ProjectFileCodec::load(const std::filesystem::path& file) const {
    try {
        toml::value document = toml::parse(file);
        ProjectConfig config{};
        const int64_t format_version = toml::find<int64_t>(document, "format_version");
        if (format_version < 0 || format_version > std::numeric_limits<uint32_t>::max() ||
            static_cast<uint32_t>(format_version) != kCurrentFormatVersion) {
            return {foundation::unsupported("Unsupported project format version"), {}};
        }
        config.format_version = static_cast<uint32_t>(format_version);

        config.name = toml::find<std::string>(document, "project", "name");
        config.target = toml::find<std::string>(document, "build", "target");
        config.startup_scene = toml::find<std::string>(document, "build", "startup_scene");
        config.paths.assets = toml::find<std::string>(document, "paths", "assets");
        config.paths.cache = toml::find<std::string>(document, "paths", "cache");
        config.paths.saved = toml::find<std::string>(document, "paths", "saved");
        config.paths.binaries = toml::find<std::string>(document, "paths", "binaries");
        if (const foundation::Result result = validate_config(config); !result) {
            return {result, {}};
        }
        return {foundation::Result::success(), ProjectFile{std::move(config)}};
    } catch (const std::exception& exception) {
        return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument, exception.what()), {}};
    }
}

foundation::Result ProjectFileCodec::save(const std::filesystem::path& file, const ProjectConfig& config) const {
    if (file.empty() || file.filename().empty()) {
        return foundation::invalid_argument("Project file path is invalid");
    }
    if (const foundation::Result result = validate_config(config); !result) {
        return result;
    }

    try {
        toml::value document(toml::table{});
        document["format_version"] = static_cast<int64_t>(config.format_version);
        document["project"] = toml::table{};
        document["project"]["name"] = config.name;
        document["paths"] = toml::table{};
        document["paths"]["assets"] = config.paths.assets.generic_string();
        document["paths"]["cache"] = config.paths.cache.generic_string();
        document["paths"]["saved"] = config.paths.saved.generic_string();
        document["paths"]["binaries"] = config.paths.binaries.generic_string();
        document["build"] = toml::table{};
        document["build"]["target"] = config.target;
        document["build"]["startup_scene"] = config.startup_scene.generic_string();
        return write_text(file, toml::format(document));
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    }
}

foundation::Result ProjectManager::load(const std::filesystem::path& file) {
    if (loaded_) {
        return foundation::invalid_state("a project is already loaded");
    }
    ProjectFileCodec codec;
    const auto parsed = codec.load(file);
    if (!parsed) {
        return parsed.result;
    }
    const auto paths = ProjectPaths::resolve(file, parsed.value.config.paths);
    if (!paths) {
        return paths.result;
    }

    std::error_code error;
    for (const ProjectDirectory directory :
         {ProjectDirectory::kAssets, ProjectDirectory::kCache, ProjectDirectory::kSaved, ProjectDirectory::kBinaries}) {
        std::filesystem::create_directories(paths.value.directory(directory), error);
        if (error) {
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "failed to create a configured project directory");
        }
    }
    context_ = {file, std::move(parsed.value.config), std::move(paths.value)};
    for (const std::shared_ptr<ProjectLifecycleParticipant>& participant : participants_) {
        foundation::Result result{};
        try {
            result = participant->on_project_loaded(context_);
        } catch (const std::exception& exception) {
            result = foundation::unknown_exception(exception);
        } catch (...) {
            result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                 "project lifecycle participant threw an unknown exception");
        }
        if (!result) {
            for (auto active = active_participants_.rbegin(); active != active_participants_.rend(); ++active) {
                try {
                    static_cast<void>((*active)->on_project_unloading(context_));
                } catch (...) {}
            }
            active_participants_.clear();
            context_ = {};
            return result;
        }
        active_participants_.push_back(participant);
    }
    loaded_ = true;
    return foundation::Result::success();
}

foundation::Result ProjectManager::register_participant(std::shared_ptr<ProjectLifecycleParticipant> participant) {
    if (loaded_) {
        return foundation::invalid_state("cannot register a project participant while a project is loaded");
    }
    if (participant == nullptr) {
        return foundation::invalid_argument("cannot register a null project participant");
    }
    participants_.push_back(std::move(participant));
    return foundation::Result::success();
}

foundation::Result ProjectManager::save() {
    if (!loaded_) {
        return foundation::invalid_state("no project is loaded");
    }
    return ProjectFileCodec{}.save(context_.file, context_.config);
}

foundation::Result ProjectManager::unload() noexcept {
    foundation::Result first_failure = foundation::Result::success();
    for (auto participant = active_participants_.rbegin(); participant != active_participants_.rend(); ++participant) {
        foundation::Result result{};
        try {
            result = (*participant)->on_project_unloading(context_);
        } catch (const std::exception& exception) {
            result = foundation::unknown_exception(exception);
        } catch (...) {
            result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                 "project lifecycle participant threw an unknown exception");
        }
        if (!result && first_failure) {
            first_failure = result;
        }
    }
    active_participants_.clear();
    context_ = {};
    loaded_ = false;
    return first_failure;
}

bool ProjectManager::loaded() const noexcept {
    return loaded_;
}

const ProjectContext* ProjectManager::context() const noexcept {
    return loaded_ ? &context_ : nullptr;
}
}  // namespace aeg::application::project
