/**
 * @file project_context.hpp
 * @brief Project configuration and project-root-relative path contracts.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "result.hpp"

namespace aeg::application::project {
/** @brief Directories whose locations are defined by a project configuration. */
enum class ProjectDirectory : uint8_t {
    kRoot,
    kAssets,
    kSource,
    kBuild,
    kCache,
    kSaved,
    kBinaries,
};

/** @brief Project-relative directory names persisted in the project file. */
struct ProjectPathConfig final {
    std::filesystem::path assets{"Assets"};
    std::filesystem::path source{"Source"};
    std::filesystem::path build{"Build"};
    std::filesystem::path cache{"Cache"};
    std::filesystem::path saved{"Saved"};
    std::filesystem::path binaries{"Binaries"};
};

struct ProjectModuleConfig final {
    std::string id{};
    std::string target{"GameModule"};
};

struct ProjectEngineConfig final {
    std::string association{"AEG"};
    std::string sdk_requirement{"1.0"};
};

/** @brief Stable settings loaded from one .aegproject TOML file. */
struct ProjectConfig final {
    uint32_t format_version{2};
    std::string name{};
    std::string project_uuid{};
    std::string target{"windows-x64-vulkan"};
    ProjectEngineConfig engine{};
    ProjectModuleConfig module{};
    std::filesystem::path startup_scene{"Assets/Scenes/Main.aegscene"};
    std::vector<std::filesystem::path> startup_pipelines{};
    ProjectPathConfig paths{};
};

/** @brief Parsed project configuration together with its retained TOML document. */
struct ProjectFile final {
    ProjectConfig config{};
    std::shared_ptr<const void> retained_source{};
    bool legacy{false};
};

/** @brief Resolves and validates the directories of one project session. */
class ProjectPaths final {
public:
    [[nodiscard]] static foundation::ResultValue<ProjectPaths> resolve(const std::filesystem::path& project_file,
                                                                       const ProjectPathConfig& config);

    [[nodiscard]] const std::filesystem::path& directory(ProjectDirectory directory) const noexcept;
    [[nodiscard]] foundation::ResultValue<std::filesystem::path> resolve_under(
        ProjectDirectory directory, const std::filesystem::path& relative) const;

private:
    std::filesystem::path root_{};
    std::filesystem::path assets_{};
    std::filesystem::path source_{};
    std::filesystem::path build_{};
    std::filesystem::path cache_{};
    std::filesystem::path saved_{};
    std::filesystem::path binaries_{};
};

/** @brief The immutable configuration and directory mapping of an open project. */
struct ProjectContext final {
    std::filesystem::path file{};
    ProjectConfig config{};
    ProjectPaths paths{};
};

/** @brief Reads and writes the versioned TOML project-file contract. */
class ProjectFileCodec final {
public:
    [[nodiscard]] foundation::ResultValue<ProjectFile> load(const std::filesystem::path& file) const;
    [[nodiscard]] foundation::Result save(const std::filesystem::path& file, const ProjectConfig& config) const;
    [[nodiscard]] foundation::Result save(const std::filesystem::path& file, const ProjectFile& project) const;
};

/** @brief Finds the canonical Project.aegproj or legacy project.aegproject under a root. */
[[nodiscard]] foundation::ResultValue<std::filesystem::path> discover_project_file(
    const std::filesystem::path& root_or_file);

/** @brief A project-owned subsystem that participates in session startup and teardown. */
class ProjectLifecycleParticipant {
public:
    virtual ~ProjectLifecycleParticipant() = default;

    [[nodiscard]] virtual foundation::Result on_project_loaded(const ProjectContext& context) = 0;
    [[nodiscard]] virtual foundation::Result on_project_unloading(const ProjectContext& context) = 0;
};

/** @brief Opens and closes one engine project session without owning runtime resources. */
class ProjectManager final {
public:
    [[nodiscard]] foundation::Result register_participant(std::shared_ptr<ProjectLifecycleParticipant> participant);
    [[nodiscard]] foundation::Result load(const std::filesystem::path& file);
    [[nodiscard]] foundation::Result save();
    [[nodiscard]] foundation::Result unload() noexcept;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] const ProjectContext* context() const noexcept;

private:
    ProjectContext context_{};
    ProjectFile project_file_{};
    std::vector<std::shared_ptr<ProjectLifecycleParticipant>> participants_{};
    std::vector<std::shared_ptr<ProjectLifecycleParticipant>> active_participants_{};
    bool loaded_{false};
};
}  // namespace aeg::application::project
