/**
 * @file project_scaffold.cpp
 * @brief Transactional game-project template creation.
 */
#include "project_scaffold.hpp"

#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <system_error>

#include "project_context.hpp"

#ifndef AEG_PROJECT_TEMPLATE_ROOT
#define AEG_PROJECT_TEMPLATE_ROOT ""
#endif

namespace aeg::application::project {
namespace {

[[nodiscard]] std::string make_uuid(const std::filesystem::path& root) {
    const std::string seed = root.generic_string();
    const uint64_t first = std::hash<std::string>{}(seed);
    const uint64_t second = std::hash<std::string>{}(seed + "#aeg-scaffold");
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << first << std::setw(16) << second;
    std::string raw = stream.str();
    raw[12] = '4';
    raw[16] = '8';
    return raw.substr(0, 8) + "-" + raw.substr(8, 4) + "-" + raw.substr(12, 4) + "-" + raw.substr(16, 4) + "-" +
           raw.substr(20, 12);
}

[[nodiscard]] foundation::Result copy_tree(const std::filesystem::path& source,
                                           const std::filesystem::path& destination) {
    std::error_code error;
    if (!std::filesystem::is_directory(source, error) || error) {
        return foundation::not_found("Game project template directory is missing");
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, error)) {
        if (error) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "Cannot enumerate game project template");
        }
        const auto relative = entry.path().lexically_relative(source);
        const auto target = destination / relative;
        if (entry.is_directory(error)) {
            std::filesystem::create_directories(target, error);
        } else if (entry.is_regular_file(error)) {
            std::filesystem::create_directories(target.parent_path(), error);
            if (!error) {
                std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing,
                                           error);
            }
        }
        if (error) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot copy game project template");
        }
    }
    return foundation::Result::success();
}

void write_u32(std::ofstream& stream, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8)
        stream.put(static_cast<char>((value >> shift) & 0xffu));
}

void write_u64(std::ofstream& stream, uint64_t value) {
    for (uint32_t shift = 0; shift < 64; shift += 8)
        stream.put(static_cast<char>((value >> shift) & 0xffu));
}

[[nodiscard]] foundation::Result write_main_scene(const std::filesystem::path& root) {
    const auto scene = root / "Assets" / "Scenes" / "Main.aegscene";
    std::ofstream stream(scene, std::ios::binary | std::ios::trunc);
    if (!stream)
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write Main scene");
    write_u64(stream, 0x3154534547414541ull);  // SceneSerializer::kMagic
    write_u32(stream, 2);                      // SceneSerializer::kFormatVersion
    write_u32(stream, 0);                      // reserved
    write_u64(stream, 1);
    write_u64(stream, 1);  // stable SceneAssetId
    write_u32(stream, 1);  // asset version
    write_u32(stream, 0);  // empty asset name
    write_u32(stream, 0);  // entities
    write_u32(stream, 0);  // components
    stream.close();
    return stream ? foundation::Result::success()
                  : foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write Main scene");
}

}  // namespace

foundation::ResultValue<std::filesystem::path> create_project(const ProjectScaffoldRequest& request) {
    if (request.root.empty() || request.name.empty() || request.module_target.empty()) {
        return {foundation::invalid_argument("Project scaffold requires root, name, and module target"), {}};
    }
    std::error_code error;
    const auto root = std::filesystem::absolute(request.root, error).lexically_normal();
    if (error || root.empty()) {
        return {foundation::invalid_argument("Project scaffold root is invalid"), {}};
    }
    if (std::filesystem::exists(root, error)) {
        if (error || !std::filesystem::is_directory(root, error) || error) {
            return {foundation::invalid_state("Project scaffold destination is not a directory"), {}};
        }

        // A new project may be created after an editor has already imported
        // source assets into its intended Assets directory. Preserve those
        // files while layering the engine template over the directory. Any
        // other pre-existing root entry remains a likely accidental overwrite
        // and keeps the original fail-fast behavior.
        bool assets_only = true;
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (error)
                break;
            if (entry.path().filename() != "Assets" || !entry.is_directory(error) || error) {
                assets_only = false;
                break;
            }
        }
        if (error || (!assets_only && !std::filesystem::is_empty(root, error)) || error) {
            return {foundation::invalid_state("Project scaffold destination already contains files"), {}};
        }
    }
    const std::filesystem::path template_root = AEG_PROJECT_TEMPLATE_ROOT;
    if (template_root.empty()) {
        return {foundation::not_found("Game project template root is not configured"), {}};
    }
    const auto result = copy_tree(template_root, root);
    if (!result) {
        std::filesystem::remove_all(root, error);
        return {result, {}};
    }

    for (const auto& directory : {"Assets/Scenes", "Source", "Build", "Cache", "Saved", "Binaries", ".aeg"}) {
        std::filesystem::create_directories(root / directory, error);
        if (error) {
            std::filesystem::remove_all(root, error);
            return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                "Cannot create scaffold project directory"),
                    {}};
        }
    }
    if (const foundation::Result scene_result = write_main_scene(root); !scene_result) {
        std::filesystem::remove_all(root, error);
        return {scene_result, {}};
    }

    const auto manifest = root / "Project.aegproj";
    ProjectFileCodec codec;
    const auto parsed = codec.load(manifest);
    if (!parsed) {
        std::filesystem::remove_all(root, error);
        return {parsed.result, {}};
    }
    ProjectFile project = parsed.value;
    project.config.name = request.name;
    project.config.project_uuid = make_uuid(root);
    project.config.module.target = request.module_target;
    project.config.module.id = request.module_target;
    if (const foundation::Result save_result = codec.save(manifest, project); !save_result) {
        std::filesystem::remove_all(root, error);
        return {save_result, {}};
    }
    return {foundation::Result::success(), manifest};
}

}  // namespace aeg::application::project
