/**
 * @file project_smoke.cpp
 * @brief Verifies project configuration persistence and path isolation.
 */
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include "project_context.hpp"

namespace {
class TestParticipant final : public aeg::application::project::ProjectLifecycleParticipant {
public:
    [[nodiscard]] aeg::foundation::Result on_project_loaded(const aeg::application::project::ProjectContext&) override {
        ++load_count;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result on_project_unloading(
        const aeg::application::project::ProjectContext&) override {
        ++unload_count;
        return aeg::foundation::Result::success();
    }

    uint32_t load_count{};
    uint32_t unload_count{};
};

class FailingParticipant final : public aeg::application::project::ProjectLifecycleParticipant {
public:
    [[nodiscard]] aeg::foundation::Result on_project_loaded(const aeg::application::project::ProjectContext&) override {
        return aeg::foundation::invalid_state("intentional lifecycle failure");
    }

    [[nodiscard]] aeg::foundation::Result on_project_unloading(
        const aeg::application::project::ProjectContext&) override {
        return aeg::foundation::Result::success();
    }
};

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    using aeg::application::project::ProjectConfig;
    using aeg::application::project::ProjectFileCodec;
    using aeg::application::project::ProjectPaths;

    const fs::path root = fs::temp_directory_path() / "aeg_project_smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / "Assets", error);
    fs::create_directories(root / "Cache", error);
    fs::create_directories(root / "Saved", error);
    fs::create_directories(root / "Binaries", error);
    if (!require(!error, "project smoke cannot create fixture directories")) {
        return 1;
    }

    ProjectConfig config{};
    config.name = "Project Smoke";
    config.target = "windows-x64-vulkan";

    const auto paths = ProjectPaths::resolve(root / "ProjectSmoke.aegproject", config.paths);
    if (!require(static_cast<bool>(paths), "project smoke cannot resolve valid directories") ||
        !require(paths.value.directory(aeg::application::project::ProjectDirectory::kAssets) == root / "Assets",
                 "project smoke Assets mapping is invalid") ||
        !require(
            paths.value.resolve_under(aeg::application::project::ProjectDirectory::kAssets, "Meshes/Cube.gltf").value ==
                root / "Assets" / "Meshes" / "Cube.gltf",
            "project smoke relative path mapping is invalid") ||
        !require(
            !paths.value.resolve_under(aeg::application::project::ProjectDirectory::kAssets, "../Saved/escape.txt"),
            "project smoke accepted a path outside Assets") ||
        !require(!paths.value.resolve_under(aeg::application::project::ProjectDirectory::kAssets,
                                            fs::temp_directory_path() / "outside.txt"),
                 "project smoke accepted an absolute path")) {
        fs::remove_all(root, error);
        return 2;
    }

    const fs::path project_file = root / "ProjectSmoke.aegproject";
    ProjectFileCodec codec;
    if (!require(static_cast<bool>(codec.save(project_file, config)), "project smoke cannot save TOML project") ||
        !require(static_cast<bool>(codec.load(project_file)), "project smoke cannot load TOML project")) {
        fs::remove_all(root, error);
        return 3;
    }

    const auto loaded = codec.load(project_file);
    if (!require(loaded.value.config.name == config.name, "project smoke name did not round-trip") ||
        !require(loaded.value.config.target == config.target, "project smoke target did not round-trip")) {
        fs::remove_all(root, error);
        return 4;
    }

    aeg::application::project::ProjectManager manager;
    const auto participant = std::make_shared<TestParticipant>();
    if (!require(static_cast<bool>(manager.register_participant(participant)),
                 "project smoke cannot register lifecycle participant") ||
        !require(static_cast<bool>(manager.load(project_file)), "project smoke cannot load a project session") ||
        !require(manager.loaded(), "project smoke did not mark the project session as loaded") ||
        !require(manager.context() != nullptr && manager.context()->config.name == config.name,
                 "project smoke context does not retain the loaded configuration") ||
        !require(participant->load_count == 1U, "project smoke did not notify lifecycle participant of loading") ||
        !require(static_cast<bool>(manager.save()), "project smoke cannot save a loaded project session") ||
        !require(static_cast<bool>(manager.unload()), "project smoke cannot unload a project session") ||
        !require(participant->unload_count == 1U, "project smoke did not notify lifecycle participant of unloading") ||
        !require(!manager.loaded() && manager.context() == nullptr,
                 "project smoke did not release the unloaded project context")) {
        fs::remove_all(root, error);
        return 5;
    }

    aeg::application::project::ProjectManager rollback_manager;
    const auto rollback_participant = std::make_shared<TestParticipant>();
    if (!require(static_cast<bool>(rollback_manager.register_participant(rollback_participant)),
                 "project smoke cannot register rollback participant") ||
        !require(static_cast<bool>(rollback_manager.register_participant(std::make_shared<FailingParticipant>())),
                 "project smoke cannot register failing participant") ||
        !require(!rollback_manager.load(project_file), "project smoke accepted failed lifecycle load") ||
        !require(!rollback_manager.loaded() && rollback_manager.context() == nullptr,
                 "project smoke retained context after lifecycle rollback") ||
        !require(rollback_participant->load_count == 1U && rollback_participant->unload_count == 1U,
                 "project smoke did not roll back the loaded lifecycle participant")) {
        fs::remove_all(root, error);
        return 6;
    }

    std::ofstream(project_file, std::ios::trunc) << "format_version = 999\n";
    if (!require(!codec.load(project_file), "project smoke accepted an unsupported format version")) {
        fs::remove_all(root, error);
        return 7;
    }

    fs::remove_all(root, error);
    return 0;
}
