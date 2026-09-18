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
    config.project_uuid = "project-smoke-uuid";
    config.target = "windows-x64-vulkan";
    config.module.id = "ProjectSmoke.GameModule";
    config.startup_pipelines.emplace_back("Assets/Pipelines/Default.aegpipe");

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

    const fs::path project_file = root / "Project.aegproj";
    ProjectFileCodec codec;
    if (!require(static_cast<bool>(codec.save(project_file, config)), "project smoke cannot save TOML project") ||
        !require(static_cast<bool>(codec.load(project_file)), "project smoke cannot load TOML project")) {
        fs::remove_all(root, error);
        return 3;
    }

    const auto loaded = codec.load(project_file);
    if (!require(loaded.value.config.name == config.name, "project smoke name did not round-trip") ||
        !require(loaded.value.config.target == config.target, "project smoke target did not round-trip") ||
        !require(loaded.value.config.project_uuid == config.project_uuid, "project smoke uuid did not round-trip") ||
        !require(loaded.value.config.module.id == config.module.id, "project smoke module did not round-trip") ||
        !require(loaded.value.config.startup_pipelines == config.startup_pipelines,
                 "project smoke startup pipelines did not round-trip") ||
        !require(loaded.value.config.paths.source == "Source" && loaded.value.config.paths.build == "Build",
                 "project smoke v2 path defaults did not round-trip")) {
        fs::remove_all(root, error);
        return 4;
    }

    std::ofstream(project_file, std::ios::trunc)
        << "format_version = 2\n"
           "[project]\nname = \"Project Smoke\"\nuuid = \"project-smoke-uuid\"\n"
           "[engine]\nassociation = \"AEG\"\nsdk_requirement = \"1.0\"\n"
           "[module]\nid = \"ProjectSmoke.GameModule\"\ntarget = \"GameModule\"\n"
           "[paths]\nassets = \"Assets\"\nsource = \"Source\"\nbuild = \"Build\"\n"
           "cache = \"Cache\"\nsaved = \"Saved\"\nbinaries = \"Binaries\"\n"
           "[build]\ntarget = \"windows-x64-vulkan\"\nstartup_scene = \"Assets/Scenes/Main.aegscene\"\n"
           "startup_pipelines = [\"Assets/Pipelines/Default.aegpipe\"]\n"
           "[custom]\nkeep_me = \"yes\"\n";
    auto overlay = codec.load(project_file);
    if (!require(static_cast<bool>(overlay), "project smoke cannot load overlay fixture")) {
        fs::remove_all(root, error);
        return 8;
    }
    overlay.value.config.name = "Project Smoke Renamed";
    if (!require(static_cast<bool>(codec.save(project_file, overlay.value)),
                 "project smoke cannot save retained TOML document")) {
        fs::remove_all(root, error);
        return 9;
    }
    std::ifstream overlay_contents(project_file);
    const std::string overlay_text((std::istreambuf_iterator<char>(overlay_contents)),
                                   std::istreambuf_iterator<char>());
    if (!require(overlay_text.find("keep_me = \"yes\"") != std::string::npos,
                 "project smoke save dropped unknown TOML metadata")) {
        fs::remove_all(root, error);
        return 10;
    }
    overlay_contents.close();

    const auto discovered = aeg::application::project::discover_project_file(root);
    if (!require(static_cast<bool>(discovered) && discovered.value.filename() == project_file.filename(),
                 "project smoke did not discover canonical project file")) {
        fs::remove_all(root, error);
        return 11;
    }

    const fs::path legacy_file = root / "project.aegproject";
    std::ofstream(legacy_file, std::ios::trunc)
        << "format_version = 1\n"
           "[project]\nname = \"Legacy Smoke\"\n"
           "[paths]\nassets = \"Assets\"\ncache = \"Cache\"\nsaved = \"Saved\"\nbinaries = \"Binaries\"\n"
           "[build]\ntarget = \"windows-x64-vulkan\"\nstartup_scene = \"Assets/Scenes/Main.aegscene\"\n";
    fs::remove(project_file, error);
    const auto legacy_discovered = aeg::application::project::discover_project_file(root);
    const auto legacy_loaded = codec.load(legacy_file);
    if (!require(static_cast<bool>(legacy_discovered) && legacy_discovered.value.filename() == legacy_file.filename(),
                 "project smoke did not discover legacy project file") ||
        !require(static_cast<bool>(legacy_loaded) && legacy_loaded.value.legacy,
                 "project smoke did not load legacy project format") ||
        !require(
            legacy_loaded.value.config.paths.source == "Source" && legacy_loaded.value.config.paths.build == "Build",
            "project smoke legacy path defaults are invalid")) {
        fs::remove_all(root, error);
        return 12;
    }
    if (!require(static_cast<bool>(codec.save(project_file, overlay.value)),
                 "project smoke cannot restore canonical project fixture")) {
        fs::remove_all(root, error);
        return 13;
    }

    aeg::application::project::ProjectManager manager;
    const auto participant = std::make_shared<TestParticipant>();
    if (!require(static_cast<bool>(manager.register_participant(participant)),
                 "project smoke cannot register lifecycle participant") ||
        !require(static_cast<bool>(manager.load(root)),
                 "project smoke cannot load a project session from project root") ||
        !require(manager.loaded(), "project smoke did not mark the project session as loaded") ||
        !require(manager.context() != nullptr && manager.context()->config.name == "Project Smoke Renamed",
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
