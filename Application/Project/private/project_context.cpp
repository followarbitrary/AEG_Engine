/**
 * @file project_context.cpp
 * @brief TOML project configuration and contained-path implementation.
 */
#include "project_context.hpp"

#include <fstream>
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
constexpr uint32_t kCurrentFormatVersion = 2;

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] std::filesystem::path source_path(const std::string& text) {
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

[[nodiscard]] toml::value parse_project_document(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input.good()) throw toml::file_io_error("toml::parse: error opening file", path_text(file));
    input.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    return toml::parse(input, path_text(file));
}

[[nodiscard]] foundation::Result reject_retired_project_file(const std::filesystem::path& file) {
    auto extension = path_text(file.extension());
    for (auto& character : extension)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    if (extension == ".aegproject")
        return foundation::unsupported("Unsupported retired project extension: .aegproject");
    std::ifstream input(file, std::ios::binary);
    std::string header;
    if (input && std::getline(input, header) && header.starts_with("AEG_PROJECT\t"))
        return foundation::unsupported("Unsupported retired project header: AEG_PROJECT");
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result validate_source_format(const toml::value& document) {
    if (!document.is_table() || !document.contains("format_version"))
        return foundation::unsupported("Unsupported project format_version: missing version");
    const auto& version = toml::find(document, "format_version");
    if (!version.is_integer() || version.as_integer() != kCurrentFormatVersion)
        return foundation::unsupported("Unsupported project format_version: only version 2 is supported");
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result reject_removed_feature_graph_extension(const std::filesystem::path& path) {
    auto extension = path_text(path.extension());
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    if (extension == ".aegpipe" || extension == ".aegframe")
        return foundation::unsupported("Removed startup asset extension in build.startup_feature_graphs: " +
                                       path_text(path));
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result reject_removed_startup_fields(const toml::value& document) {
    if (!document.is_table() || !document.contains("build")) return foundation::Result::success();
    const auto& build = toml::find(document, "build");
    if (!build.is_table()) return foundation::Result::success();
    for (const auto* key : {"startup_frame_projections", "startup_pipelines"}) {
        if (build.contains(key))
            return foundation::unsupported(std::string{"Removed project startup field: build."} + key);
    }
    if (build.contains("startup_feature_graphs")) {
        for (const auto& path : toml::find<std::vector<std::string>>(build, "startup_feature_graphs")) {
            if (const auto result = reject_removed_feature_graph_extension(source_path(path)); !result) return result;
        }
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result validate_existing_target(const std::filesystem::path& file) {
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error)
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "Could not inspect existing project target: " + error.message());
    if (!exists) return foundation::Result::success();
    if (!std::filesystem::is_regular_file(file, error)) {
        if (error)
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "Could not inspect existing project target: " + error.message());
        return foundation::invalid_argument("Existing project target is not a regular file");
    }
    try {
        const auto document = parse_project_document(file);
        if (const auto result = validate_source_format(document); !result) return result;
        return reject_removed_startup_fields(document);
    } catch (const std::exception& exception) {
        return foundation::invalid_argument(std::string{"Existing project target is invalid: "} + exception.what());
    }
}

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
        !is_safe_relative_path(config.source) || !is_safe_relative_path(config.build) ||
        !is_safe_relative_path(config.saved) || !is_safe_relative_path(config.binaries)) {
        return invalid_project_path();
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result validate_config(const ProjectConfig& config) {
    if (config.format_version != kCurrentFormatVersion)
        return foundation::unsupported("Unsupported project format_version: only version 2 is supported");
    if (config.name.empty() ||
        config.target.empty() || !is_safe_relative_path(config.startup_scene)) {
        return foundation::invalid_argument("Project configuration is invalid");
    }
    for (const auto& pipeline : config.startup_feature_graphs) {
        if (!is_safe_relative_path(pipeline)) {
            return invalid_project_path();
        }
        if (const auto result = reject_removed_feature_graph_extension(pipeline); !result) return result;
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
    auto temporary = file;
    temporary += ".tmp";
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
    paths.source_ = (paths.root_ / config.source).lexically_normal();
    paths.build_ = (paths.root_ / config.build).lexically_normal();
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
        case ProjectDirectory::kSource:
            return source_;
        case ProjectDirectory::kBuild:
            return build_;
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
        if (const auto result = reject_retired_project_file(file); !result) return {result, {}};
        auto retained = std::make_shared<toml::value>(parse_project_document(file));
        const toml::value& document = *retained;
        if (const auto result = validate_source_format(document); !result) return {result, {}};
        ProjectConfig config{};
        if (const auto result = reject_removed_startup_fields(document); !result) return {result, {}};

        config.name = toml::find<std::string>(document, "project", "name");
        {
            if (toml::find(document, "project").contains("uuid")) {
                config.project_uuid = toml::find<std::string>(document, "project", "uuid");
            }
            if (document.contains("engine")) {
                config.engine.association = toml::find<std::string>(document, "engine", "association");
                config.engine.sdk_requirement = toml::find<std::string>(document, "engine", "sdk_requirement");
                if (toml::find(document, "engine").contains("template")) {
                    config.engine.template_id = toml::find<std::string>(document, "engine", "template");
                }
            }
            if (document.contains("module")) {
                config.module.id = toml::find<std::string>(document, "module", "id");
                config.module.target = toml::find<std::string>(document, "module", "target");
            }
        }
        config.target = toml::find<std::string>(document, "build", "target");
        config.startup_scene = source_path(toml::find<std::string>(document, "build", "startup_scene"));
        if (toml::find(document, "build").contains("startup_feature_graphs")) {
            for (const auto& path : toml::find<std::vector<std::string>>(document, "build", "startup_feature_graphs")) {
                config.startup_feature_graphs.push_back(source_path(path));
            }
        }
        config.paths.assets = source_path(toml::find<std::string>(document, "paths", "assets"));
        config.paths.source = source_path(toml::find<std::string>(document, "paths", "source"));
        config.paths.build = source_path(toml::find<std::string>(document, "paths", "build"));
        config.paths.cache = source_path(toml::find<std::string>(document, "paths", "cache"));
        config.paths.saved = source_path(toml::find<std::string>(document, "paths", "saved"));
        config.paths.binaries = source_path(toml::find<std::string>(document, "paths", "binaries"));
        if (const foundation::Result result = validate_config(config); !result) {
            return {result, {}};
        }
        return {foundation::Result::success(),
                ProjectFile{std::move(config), std::move(retained)}};
    } catch (const std::exception& exception) {
        return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument, exception.what()), {}};
    }
}

foundation::Result ProjectFileCodec::save(const std::filesystem::path& file, const ProjectConfig& config) const {
    return save(file, ProjectFile{config, {}});
}

foundation::Result ProjectFileCodec::save(const std::filesystem::path& file, const ProjectFile& project) const {
    if (file.empty() || file.filename().empty()) {
        return foundation::invalid_argument("Project file path is invalid");
    }
    if (const auto result = reject_retired_project_file(file); !result) return result;
    if (const foundation::Result result = validate_config(project.config); !result) {
        return result;
    }
    try {
        toml::value document(toml::table{});
        if (project.retained_source != nullptr) {
            const auto retained = std::static_pointer_cast<const toml::value>(project.retained_source);
            document = *retained;
            if (const auto result = validate_source_format(document); !result) return result;
        }
        if (const auto result = reject_removed_startup_fields(document); !result) return result;
        document["format_version"] = static_cast<int64_t>(project.config.format_version);
        document["project"]["name"] = project.config.name;
        if (!project.config.project_uuid.empty())
            document["project"]["uuid"] = project.config.project_uuid;
        document["engine"]["association"] = project.config.engine.association;
        document["engine"]["sdk_requirement"] = project.config.engine.sdk_requirement;
        document["engine"]["template"] = project.config.engine.template_id;
        document["module"]["id"] = project.config.module.id;
        document["module"]["target"] = project.config.module.target;
        document["build"]["target"] = project.config.target;
        document["build"]["startup_scene"] = path_text(project.config.startup_scene);
        std::vector<std::string> graphs;
        graphs.reserve(project.config.startup_feature_graphs.size());
        for (const auto& graph : project.config.startup_feature_graphs)
            graphs.push_back(path_text(graph));
        document["build"]["startup_feature_graphs"] = std::move(graphs);
        document["paths"]["assets"] = path_text(project.config.paths.assets);
        document["paths"]["source"] = path_text(project.config.paths.source);
        document["paths"]["build"] = path_text(project.config.paths.build);
        document["paths"]["cache"] = path_text(project.config.paths.cache);
        document["paths"]["saved"] = path_text(project.config.paths.saved);
        document["paths"]["binaries"] = path_text(project.config.paths.binaries);
        if (const auto result = validate_existing_target(file); !result) return result;
        return write_text(file, toml::format(document));
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    }
}

foundation::ResultValue<std::filesystem::path> discover_project_file(const std::filesystem::path& root_or_file) {
    if (root_or_file.empty()) {
        return {foundation::invalid_argument("Project root or file path is empty"), {}};
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(root_or_file, error)) {
        if (error)
            return {foundation::invalid_argument("Project file cannot be inspected"), {}};
        const auto validated = ProjectFileCodec{}.load(root_or_file);
        if (!validated) return {validated.result, {}};
        return {foundation::Result::success(), std::filesystem::weakly_canonical(root_or_file, error)};
    }
    const auto root = std::filesystem::is_directory(root_or_file, error) ? root_or_file : root_or_file.parent_path();
    if (error || root.empty()) {
        return {foundation::not_found("Project root cannot be resolved"), {}};
    }
    if (root != root_or_file) {
        if (const auto result = reject_retired_project_file(root_or_file); !result) return {result, {}};
    }
    const auto canonical = root / "Project.aegproj";
    if (std::filesystem::exists(canonical, error)) {
        if (error)
            return {foundation::invalid_argument("Canonical project file cannot be inspected"), {}};
        const auto validated = ProjectFileCodec{}.load(canonical);
        if (!validated) return {validated.result, {}};
        return {foundation::Result::success(), std::filesystem::weakly_canonical(canonical, error)};
    }
    const auto legacy = root / "project.aegproject";
    if (std::filesystem::exists(legacy, error)) {
        if (error)
            return {foundation::invalid_argument("Legacy project file cannot be inspected"), {}};
        return {foundation::unsupported("Unsupported retired project extension: .aegproject"), {}};
    }
    return {foundation::not_found("No Project.aegproj was found"), {}};
}

foundation::Result ProjectManager::load(const std::filesystem::path& file) {
    if (loaded_) {
        return foundation::invalid_state("a project is already loaded");
    }
    const auto discovered = discover_project_file(file);
    if (!discovered) {
        return discovered.result;
    }
    const std::filesystem::path project_path = discovered.value;
    ProjectFileCodec codec;
    const auto parsed = codec.load(project_path);
    if (!parsed) {
        return parsed.result;
    }
    const auto paths = ProjectPaths::resolve(project_path, parsed.value.config.paths);
    if (!paths) {
        return paths.result;
    }

    std::error_code error;
    for (const ProjectDirectory directory :
         {ProjectDirectory::kAssets, ProjectDirectory::kSource, ProjectDirectory::kBuild, ProjectDirectory::kCache,
          ProjectDirectory::kSaved, ProjectDirectory::kBinaries}) {
        std::filesystem::create_directories(paths.value.directory(directory), error);
        if (error) {
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "failed to create a configured project directory");
        }
    }
    project_file_ = std::move(parsed.value);
    context_ = {project_path, project_file_.config, std::move(paths.value)};
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
            project_file_ = {};
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
    project_file_.config = context_.config;
    return ProjectFileCodec{}.save(context_.file, project_file_);
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
    project_file_ = {};
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
