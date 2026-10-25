#include "feature_clock_test_fixture.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

#include <concurrency.hpp>
#include "project_operation.hpp"
#include "project_context.hpp"
#include "dynamic_module.hpp"
#include "feature_graph.hpp"
#include "feature_registry.hpp"
#include "module_dynamic_feature_fixture.hpp"
#include "module_bundle_test_fixture.hpp"
#include "asset_metadata.hpp"
#include "asset_package_reader.hpp"
#include "game_build_manifest.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"

namespace {
bool test_explicit_module_selection_checks_project_and_returns_verified_bundle(const std::filesystem::path& binary) {
    using namespace aeg;
    const auto root = std::filesystem::temp_directory_path() / "aeg_project_module_selection_smoke";
    const auto external_root = root.parent_path() / "aeg_project_module_selection_external_bundle";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove_all(external_root, error);
    const auto bundle = module_bundle_test::write_bundle(binary, root / "CustomBuildOutput",
        module_bundle_test::metadata_for(dynamic_feature_test::definition()));
    application::project::ProjectConfig config{};
    config.name = "Module Selection";
    config.project_uuid = "00000000-0000-0000-0000-000000000001";
    config.module.id = "com.aeg.dynamic.fixture";
    config.module.target = "IndependentCmakeTarget";
    config.target = "Fixture";
    const auto project_file = root / "Project.aegproj";
    const application::project::ProjectFileCodec codec;
    if (!codec.save(project_file, config)) return false;
    const auto selected = tooling::ProjectOperationService::select_module(project_file, bundle.manifest_path, "Debug");
    if (!selected || selected.value.metadata.build_id != "b" || selected.value.metadata.target != "Fixture" ||
        selected.value.metadata.configuration != "Debug" || selected.value.metadata.module.id != module::ModuleId("com.aeg.dynamic.fixture") ||
        selected.value.binary_sha256 != bundle.binary_sha256 || selected.value.metadata_sha256 != bundle.metadata_sha256 ||
        selected.value.manifest_path != bundle.manifest_path) return false;
    feature::FeatureRegistry features{aeg::feature::test::clock_test_host(true)};
    module::ModuleRegistry modules(features);
    module::DynamicModuleLoader loader(modules);
    module::DynamicModuleHandle handle;
    if (!loader.load(selected.value, handle)) return false;
    {
        const auto executable = loader.make_feature_module(handle);
        if (!executable || executable.value.feature_definitions.empty()) return false;
    }
    if (!loader.unload(handle)) return false;
    const auto previous_directory = std::filesystem::current_path();
    std::filesystem::create_directories(root / "Unrelated");
    std::filesystem::current_path(root / "Unrelated");
    const auto relative = tooling::ProjectOperationService::select_module(project_file,
        "CustomBuildOutput/module.manifest.json", "Debug");
    std::filesystem::current_path(previous_directory);
    if (!relative || relative.value.binary_sha256 != bundle.binary_sha256) return false;
    const auto external = module_bundle_test::write_bundle(binary, external_root,
        module_bundle_test::metadata_for(dynamic_feature_test::definition()));
    const auto external_selection = tooling::ProjectOperationService::select_module(project_file, external.manifest_path, "Debug");
    if (!external_selection || external_selection.value.binary_path != external.binary_path) return false;
    if (tooling::ProjectOperationService::select_module(project_file, {}, "Debug") ||
        tooling::ProjectOperationService::select_module(project_file, bundle.manifest_path, {}) ||
        tooling::ProjectOperationService::select_module(project_file, "../module.manifest.json", "Debug") ||
        tooling::ProjectOperationService::select_module(project_file, "missing.module.manifest.json", "Debug") ||
        tooling::ProjectOperationService::select_module(project_file, bundle.manifest_path, "Release")) return false;
    config.module.id = "com.aeg.other";
    if (!codec.save(project_file, config) ||
        tooling::ProjectOperationService::select_module(project_file, bundle.manifest_path, "Debug")) return false;
    config.module.id.clear();
    if (!codec.save(project_file, config) ||
        tooling::ProjectOperationService::select_module(project_file, bundle.manifest_path, "Debug")) return false;
    config.module.id = "com.aeg.dynamic.fixture";
    config.target = "windows-x64-vulkan";
    if (!codec.save(project_file, config) ||
        tooling::ProjectOperationService::select_module(project_file, bundle.manifest_path, "Debug")) return false;
    config.target = "Fixture";
    if (!codec.save(project_file, config)) return false;
    const auto next = module_bundle_test::write_bundle(binary, root / "CustomBuildOutput",
        module_bundle_test::metadata_for(dynamic_feature_test::definition()), "next-build");
    if (!module::validate_module_bundle(next.manifest_path)) return false;
    const auto stale_load = loader.load(selected.value, handle);
    if (stale_load) {
        static_cast<void>(loader.unload(handle));
        std::cerr << "Loader silently switched the selected valid bundle to another build\n";
        return false;
    }
    if (stale_load.code != foundation::ResultCode::kInvalidState || handle.loaded || handle.native_handle) return false;
    const auto newly_selected = tooling::ProjectOperationService::select_module(project_file, next.manifest_path, "Debug");
    if (!newly_selected || newly_selected.value.metadata.build_id != "next-build" ||
        !loader.load(newly_selected.value, handle) || !loader.unload(handle)) return false;
    { std::ofstream tampered(bundle.binary_path, std::ios::binary | std::ios::app); tampered.put('!'); }
    if (tooling::ProjectOperationService::select_module(project_file, bundle.manifest_path, "Debug")) return false;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove_all(external_root, error);
    return true;
}

bool has_actual_cook_process_observations(const aeg::tooling::ProjectOperationStatus& status, bool succeeded = false) {
    using namespace aeg;
    const auto expected_module = module::ModuleId("Engine.Tooling.ProjectOperations");
    const auto& stages = status.feature_executions;
    if (!status.execution_complete || status.feature_module.id != expected_module ||
        status.feature_module.name.empty() || stages.size() != 3) return false;
    const std::array expected_definitions{
        feature::FeatureDefinitionId("Engine.Tooling.ProjectValidationFeature"),
        feature::FeatureDefinitionId("Engine.Tooling.ProjectCookFeature"),
        feature::FeatureDefinitionId("Engine.Tooling.ProjectPublishFeature")};
    const std::array expected_nodes{
        feature::FeatureNodeId("ProjectValidationNode"), feature::FeatureNodeId("ProjectCookNode"),
        feature::FeatureNodeId("ProjectPublishNode")};
    const std::array expected_effects{
        feature::SideEffectId("Engine.Tooling.Project.ReadInputs"),
        feature::SideEffectId("Engine.Tooling.Project.Cook"),
        feature::SideEffectId("Engine.Tooling.Project.VerifyPublication")};
    for (std::size_t index = 0; index != stages.size(); ++index) {
        const auto& stage = stages[index];
        if (stage.source.definition_id != expected_definitions[index] || stage.source.node_id != expected_nodes[index] ||
            !stage.source.instance_id.is_valid() || stage.source.owner_module != expected_module ||
            stage.source.owner_path.nodes != std::vector<feature::FeatureNodeId>{expected_nodes[index]} ||
            stage.source.clock != feature::ClockId("Engine.Tooling.ProjectOperationClock") ||
            stage.source.domains != feature::to_mask(feature::FeatureDomain::kLogic) ||
            !stage.generation.is_valid() || stage.generation != stages.front().generation || stage.definition_digest == 0 ||
            !stage.process.id.is_valid() || stage.process.id != stage.source.process_id ||
            stage.process.owner != stage.source.instance_id || stage.process.owner_module != expected_module ||
            stage.side_effects.size() != 1 || stage.side_effects.front().id != expected_effects[index] ||
            !stage.invocation.open_attempts.empty()) return false;
        for (const auto& effect : stage.side_effects) {
            if (!effect.id.is_valid() || effect.version != 1 || effect.kind != feature::SideEffectKind::kExternalIo ||
                effect.commit != feature::CommitPolicy::kExternalOwner) return false;
        }
        if (succeeded || index == 0) {
            if (stage.process.status != feature::ProcessStatus::kCompleted || stage.process.current_state != "Done" ||
                stage.process.state.commit_count != 1 || !stage.invocation.last_issued ||
                !stage.invocation.last_committed || stage.invocation.last_failed) return false;
            const auto& commit = *stage.invocation.last_committed;
            if (commit.attempt.token != stage.invocation.last_issued->token ||
                stage.source.invocation_id != commit.attempt.token.invocation_id ||
                commit.attempt.token.generation != stage.generation ||
                commit.attempt.process != stage.process.id || commit.process_revision != stage.process.revision ||
                commit.state_version != stage.process.state.version ||
                commit.state_version != feature::StateVersion(commit.attempt.token.state_version.value() + 1)) return false;
        } else if (index == 1) {
            if (stage.process.status != feature::ProcessStatus::kReady || stage.process.current_state != "Ready" ||
                stage.process.state.commit_count != 0 || stage.invocation.last_committed ||
                !stage.invocation.last_issued || !stage.invocation.last_failed ||
                stage.invocation.last_failed->attempt.token != stage.invocation.last_issued->token ||
                stage.invocation.last_failed->attempt.process != stage.process.id ||
                stage.invocation.last_failed->stage != feature::InvocationFailureStage::kCallback ||
                stage.invocation.last_failed->result.code != status.result.code) return false;
        } else if (stage.process.status != feature::ProcessStatus::kReady || stage.process.current_state != "Ready" ||
                   stage.process.state.commit_count != 0 || stage.invocation.last_issued ||
                   stage.invocation.last_committed || stage.invocation.last_failed) return false;
    }
    return status.feature_stages == (succeeded
        ? std::vector<std::string>{"ProjectValidationFeature", "ProjectCookFeature", "ProjectPublishFeature"}
        : std::vector<std::string>{"ProjectValidationFeature"});
}

bool test_cook_publishes_actual_scene_and_selected_module(const std::filesystem::path& binary,
                                                         const std::filesystem::path& root) {
    using namespace aeg;
    std::filesystem::create_directories(root / "Assets/Scenes");
    const auto bundle = module_bundle_test::write_bundle(binary, root / "SelectedModule",
        module_bundle_test::metadata_for(dynamic_feature_test::definition()));
    application::project::ProjectConfig config{};
    config.name = "Actual Cook";
    config.project_uuid = "00000000-0000-4000-8000-000000000009";
    config.module.id = "com.aeg.dynamic.fixture";
    config.module.target = "IndependentCmakeTarget";
    config.target = "Fixture";
    config.startup_scene = "Assets/Scenes/Main.aegscene";
    config.startup_feature_graphs = {};  // A Scene-only package is a valid project.
    const auto project = root / "Project.aegproj";
    if (!application::project::ProjectFileCodec{}.save(project, config)) return false;
    const auto scene_path = root / config.startup_scene;
    const scene::SceneAssetId scene_id{0x4000, 0x8000000000000010ull};
    scene::SceneAsset authored(scene_id, "Actual Cook Scene", 1);
    if (!authored.add_entity({scene::SceneEntityId{1, 2}, {}, "Cooked entity", scene::SceneEntityFlags::kNone, {}}))
        return false;
    serialization::BinaryWriter archive;
    if (!scene::SceneSerializer::serialize(authored, archive)) return false;
    {
        std::ofstream output(scene_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(archive.data().data()), static_cast<std::streamsize>(archive.size()));
        if (!output) return false;
    }
    const auto asset_id = foundation::AssetId::parse("00000000-0000-4000-8000-000000000010").value;
    tooling::AssetMeta meta{};
    meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000011").value;
    meta.importer = "aeg.scene";
    meta.products = {{asset_id, tooling::AssetKind::kScene, "Main"}};
    if (!tooling::AssetMetaCodec{}.save(scene_path.string() + ".meta", meta)) return false;
    {
        std::ofstream cmake(root / "CMakeLists.txt");
        cmake << "message(FATAL_ERROR \"Cook must not configure or compile\")\n";
    }
    tooling::ProjectOperationService service;
    tooling::ProjectOperationRequest request{project, root, "Debug", config.module.target,
        tooling::ProjectOperation::kCook, bundle.manifest_path, root / "Cooked Output"};
    request.cmake_executable = root / "must-not-run-cmake.exe";
    const auto started = service.start(request);
    if (!started) return false;
    const auto completed = service.wait(started.value);
    if (!completed) return false;
    const auto& status = completed.value;
    if (status.state != tooling::ProjectOperationState::kSucceeded || !status.result ||
        !has_actual_cook_process_observations(status, true) || status.module_manifest != bundle.manifest_path ||
        !std::filesystem::is_regular_file(status.game_manifest) ||
        !std::filesystem::is_regular_file(request.output_directory / "current.toml")) return false;
    std::ifstream stream(status.game_manifest, std::ios::binary);
    const std::string manifest_bytes{std::istreambuf_iterator<char>(stream), {}};
    const auto manifest = asset::decode_game_build_manifest(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(manifest_bytes.data()), manifest_bytes.size()));
    if (!manifest || manifest.value.project_uuid != config.project_uuid ||
        manifest.value.module_id != config.module.id || manifest.value.build_id != "b" ||
        manifest.value.target != "Fixture" || manifest.value.configuration != "Debug" ||
        manifest.value.startup_scene != asset_id || !manifest.value.startup_feature_graphs.empty()) return false;
    const auto delivered = module::validate_module_bundle(status.game_manifest.parent_path() / manifest.value.module_manifest_name);
    if (!delivered || delivered.value.binary_sha256 != bundle.binary_sha256 ||
        delivered.value.metadata_sha256 != bundle.metadata_sha256 ||
        delivered.value.metadata.contract_digest != bundle.metadata.contract_digest) return false;
    const auto package = asset::AssetPackageReader::open(status.game_manifest.parent_path() / manifest.value.package_name);
    if (!package || package.value.entry_count() != 1) return false;
    const auto cooked = package.value.read_cooked(asset_id);
    if (!cooked || cooked.value.kind != foundation::AssetKind::kScene) return false;
    const auto payload = asset::decode_scene_payload(cooked.value.payload);
    if (!payload || !payload.value.references.empty()) return false;
    serialization::BinaryReader reader(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(payload.value.serialized_scene.data()), payload.value.serialized_scene.size()));
    const auto scene = scene::SceneSerializer::deserialize(reader);
    return scene && reader.remaining() == 0 && scene.value.id() == scene_id && scene.value.entities().size() == 1 &&
           scene.value.entities().front().name == "Cooked entity";
}

bool has_actual_build_owners(const aeg::tooling::ProjectOperationStatus& status) {
    using namespace aeg;
    if (status.feature_module.id != module::ModuleId("Engine.Tooling.ProjectOperations") ||
        status.feature_executions.size() != 3) return false;
    const std::array definitions{
        feature::FeatureDefinitionId("Engine.Tooling.ProjectValidationFeature"),
        feature::FeatureDefinitionId("Engine.Tooling.ProjectConfigureFeature"),
        feature::FeatureDefinitionId("Engine.Tooling.ProjectCompileFeature")};
    const std::array nodes{feature::FeatureNodeId("ProjectValidationNode"),
        feature::FeatureNodeId("ProjectConfigureNode"), feature::FeatureNodeId("ProjectCompileNode")};
    const std::array effects{feature::SideEffectId("Engine.Tooling.Project.ReadInputs"),
        feature::SideEffectId("Engine.Tooling.Project.Configure"), feature::SideEffectId("Engine.Tooling.Project.Compile")};
    for (std::size_t index = 0; index != definitions.size(); ++index) {
        const auto& observed = status.feature_executions[index];
        if (observed.source.definition_id != definitions[index] || observed.source.node_id != nodes[index] ||
            observed.source.owner_path.nodes != std::vector<feature::FeatureNodeId>{nodes[index]} ||
            observed.source.owner_module != status.feature_module.id || !observed.source.instance_id.is_valid() ||
            observed.source.clock != feature::ClockId("Engine.Tooling.ProjectOperationClock") ||
            observed.source.domains != feature::to_mask(feature::FeatureDomain::kLogic) ||
            observed.process.owner != observed.source.instance_id || observed.process.id != observed.source.process_id ||
            !observed.process.id.is_valid() || observed.process.owner_module != status.feature_module.id ||
            !observed.generation.is_valid() || observed.generation != status.feature_executions.front().generation ||
            observed.definition_digest == 0 || observed.side_effects.size() != 1 ||
            observed.side_effects.front().id != effects[index] || observed.side_effects.front().version != 1 ||
            observed.side_effects.front().kind != feature::SideEffectKind::kExternalIo ||
            observed.side_effects.front().commit != feature::CommitPolicy::kExternalOwner) return false;
    }
    return true;
}

bool committed_validation_once(const aeg::tooling::ProjectOperationFeatureExecution& observed) {
    using namespace aeg;
    if (observed.process.status != feature::ProcessStatus::kCompleted || observed.process.current_state != "Done" ||
        observed.process.state.commit_count != 1 || !observed.invocation.last_issued ||
        !observed.invocation.last_committed || observed.invocation.last_failed ||
        !observed.invocation.open_attempts.empty()) return false;
    const auto& commit = *observed.invocation.last_committed;
    return commit.attempt.token == observed.invocation.last_issued->token &&
        commit.attempt.token.invocation_id == observed.source.invocation_id &&
        commit.attempt.token.invocation_id.is_valid() && commit.attempt.token.attempt_id.is_valid() &&
        commit.attempt.token.generation == observed.generation && commit.attempt.process == observed.process.id &&
        commit.process_revision == observed.process.revision && commit.state_version == observed.process.state.version &&
        commit.state_version == feature::StateVersion(commit.attempt.token.state_version.value() + 1);
}

bool write_build_project(const std::filesystem::path& root) {
    aeg::application::project::ProjectConfig config{};
    config.name = "Strict Build";
    config.project_uuid = "00000000-0000-4000-8000-000000000021";
    config.module.id = "Smoke.GameModule";
    config.module.target = "MustNotCompile";
    config.target = "windows-x64-vulkan";
    config.startup_scene = "Assets/Scenes/Main.aegscene";
    std::filesystem::create_directories(root);
    return bool(aeg::application::project::ProjectFileCodec{}.save(root / "Project.aegproj", config));
}

bool test_build_requires_explicit_inputs(const std::filesystem::path& root,
                                        const std::filesystem::path& cmake, bool missing_engine) {
    using namespace aeg;
    if (!write_build_project(root)) return false;
    if (missing_engine) {
        std::ofstream source(root / "CMakeLists.txt");
        source << "message(FATAL_ERROR \"Engine-less Build must fail before CMake\")\n";
    }
    tooling::ProjectOperationService service;
    tooling::ProjectOperationRequest request{root / "Project.aegproj",
        missing_engine ? std::filesystem::path{} : root, "Debug", "MustNotCompile", tooling::ProjectOperation::kBuild};
    request.cmake_executable = cmake;
    const auto started = service.start(request);
    if (!started) return false;
    const auto completed = service.wait(started.value);
    if (!completed) return false;
    const auto& status = completed.value;
    const auto expected_code = missing_engine ? foundation::ResultCode::kInvalidArgument : foundation::ResultCode::kNotFound;
    const std::string expected_diagnostic = missing_engine ? "project.engine" : "project.configure";
    std::cout << (missing_engine ? "strict_no_engine" : "strict_no_cmake")
              << " state=" << static_cast<int>(status.state) << " code=" << static_cast<int>(status.result.code)
              << " stages=" << status.feature_stages.size() << " actual=" << status.feature_executions.size()
              << " diagnostic=" << (status.diagnostics.empty() ? "none" : status.diagnostics.front().code) << '\n';
    if (status.state != tooling::ProjectOperationState::kFailed || status.result.code != expected_code ||
        !status.execution_complete || status.diagnostics.empty() || status.diagnostics.front().code != expected_diagnostic ||
        status.feature_stages != std::vector<std::string>{"ProjectValidationFeature"} ||
        !has_actual_build_owners(status) || !committed_validation_once(status.feature_executions.front())) return false;
    const auto& configure = status.feature_executions[1];
    const auto& compile = status.feature_executions[2];
    return configure.process.status == feature::ProcessStatus::kReady && configure.process.state.commit_count == 0 &&
        configure.invocation.last_issued && configure.invocation.last_failed && !configure.invocation.last_committed &&
        configure.invocation.open_attempts.empty() &&
        configure.invocation.last_failed->stage == feature::InvocationFailureStage::kCallback &&
        configure.invocation.last_failed->result.code == expected_code &&
        configure.invocation.last_failed->attempt.token == configure.invocation.last_issued->token &&
        configure.invocation.last_failed->attempt.process == configure.process.id &&
        compile.process.status == feature::ProcessStatus::kReady && compile.process.state.commit_count == 0 &&
        !compile.invocation.last_issued && !compile.invocation.last_committed && !compile.invocation.last_failed &&
        compile.invocation.open_attempts.empty();
}

bool test_running_build_publishes_real_commit_and_cancel_retains_it(const std::filesystem::path& root,
                                                                  const std::filesystem::path& cmake) {
    using namespace aeg;
    if (!write_build_project(root)) return false;
    const auto gate = root / "hold-configure";
    const auto entered = root / "configure-entered.marker";
    {
        std::ofstream hold(gate); hold << "real external CMake gate\n";
        std::ofstream source(root / "CMakeLists.txt");
        source << R"CMAKE(cmake_minimum_required(VERSION 3.25)
project(ProjectOperationProgress LANGUAGES NONE)
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/configure-entered.marker" "actual configure\n")
while(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hold-configure")
endwhile()
add_custom_target(MustNotCompile
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_SOURCE_DIR}/built.marker")
)CMAKE";
        if (!hold || !source) return false;
    }
    tooling::ProjectOperationService service;
    tooling::ProjectOperationRequest request{root / "Project.aegproj", root, "Debug", "MustNotCompile",
        tooling::ProjectOperation::kBuild};
    request.cmake_executable = cmake;
    const auto started = service.start(request);
    if (!started) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!std::filesystem::is_regular_file(entered) && std::chrono::steady_clock::now() < deadline) {
        const auto polled = service.poll(started.value);
        if (!polled || polled.value.execution_complete) break;
        concurrency::yield_current_thread();
    }
    const auto running = service.poll(started.value);
    const bool entered_actual = std::filesystem::is_regular_file(entered);
    const bool actual_progress = entered_actual && running && !running.value.execution_complete &&
        running.value.state == tooling::ProjectOperationState::kRunning && has_actual_build_owners(running.value) &&
        running.value.feature_stages == std::vector<std::string>{"ProjectValidationFeature"} &&
        committed_validation_once(running.value.feature_executions.front());
    std::cout << "live_configure_progress entered=" << entered_actual
              << " state=" << (running ? static_cast<int>(running.value.state) : -1)
              << " complete=" << (running && running.value.execution_complete)
              << " stages=" << (running ? running.value.feature_stages.size() : 0)
              << " actual=" << (running ? running.value.feature_executions.size() : 0) << '\n';
    // Always retire the real child process, including when the progress oracle is RED.
    const auto cancelled = service.cancel(started.value);
    const auto final = service.wait(started.value);
    std::error_code cleanup_error;
    std::filesystem::remove(gate, cleanup_error);
    const bool retained = cancelled && final && final.value.execution_complete &&
        final.value.state == tooling::ProjectOperationState::kCancelled &&
        final.value.result.code == foundation::ResultCode::kCancelled && has_actual_build_owners(final.value) &&
        final.value.feature_stages == std::vector<std::string>{"ProjectValidationFeature"} &&
        committed_validation_once(final.value.feature_executions.front()) &&
        final.value.feature_executions[1].process.status == feature::ProcessStatus::kCancelled &&
        final.value.feature_executions[2].process.status == feature::ProcessStatus::kCancelled &&
        !final.value.feature_executions[1].invocation.last_committed &&
        !final.value.feature_executions[2].invocation.last_issued &&
        !std::filesystem::exists(root / "built.marker") && !std::filesystem::exists(root / "Binaries/Debug/current.toml");
    const bool same_commit = actual_progress && retained &&
        running.value.feature_executions.front().invocation.last_committed->attempt.token ==
            final.value.feature_executions.front().invocation.last_committed->attempt.token &&
        running.value.feature_executions.front().process.id == final.value.feature_executions.front().process.id &&
        running.value.feature_executions.front().generation == final.value.feature_executions.front().generation;
    std::cout << "live_configure_cancel_retained=" << retained << " same_commit=" << same_commit << '\n';
    return actual_progress && retained && same_commit;
}
}

int main(int argc, char** argv) {
    if (argc != 3 || !std::filesystem::is_regular_file(std::filesystem::absolute(argv[2])) ||
        !test_explicit_module_selection_checks_project_and_returns_verified_bundle(std::filesystem::absolute(argv[1])))
        return 34;
    using namespace aeg;
    const auto root = std::filesystem::temp_directory_path() / "aeg_project_operation_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    const auto project = root / "Project.aegproj";
    std::ofstream(project)
        << "format_version = 2\n[project]\nname = \"Smoke\"\nuuid = \"00000000-0000-0000-0000-000000000001\"\n"
        << "[engine]\nassociation = \"AEG\"\nsdk_requirement = \"1.0\"\n"
        << "[module]\nid = \"Smoke.GameModule\"\ntarget = \"GameModule\"\n"
        << "[paths]\nassets = \"Assets\"\nsource = \"Source\"\nbuild = \"Build\"\ncache = \"Cache\"\nsaved = "
           "\"Saved\"\nbinaries = \"Binaries\"\n"
        << "[build]\ntarget = \"windows-x64-vulkan\"\nstartup_scene = "
           "\"Assets/Scenes/Main.aegscene\"\nstartup_feature_graphs = []\n";

    tooling::ProjectOperationService service;
    tooling::ProjectOperationRequest request{project, root, "Debug", "Game", tooling::ProjectOperation::kCook};
    request.cmake_executable = root / "must-not-run-cmake.exe";
    const auto started = service.start(request);
    if (!started || service.start(request))
        return 1;
    tooling::ProjectOperationStatus terminal{};
    bool done = false;
    for (int i = 0; i < 100000 && !done; ++i) {
        const auto polled = service.poll(started.value);
        if (!polled)
            return 2;
        terminal = polled.value;
        done = terminal.execution_complete;
        if (!done)
            aeg::concurrency::yield_current_thread();
    }
    std::cout << "requested=Cook manifest=empty state=" << static_cast<int>(terminal.state)
              << " done=" << done << " game_manifest=" << terminal.game_manifest.string()
              << " module_manifest=" << terminal.module_manifest.string() << '\n';
    if (!done || terminal.state != tooling::ProjectOperationState::kFailed ||
        terminal.result.code != foundation::ResultCode::kInvalidArgument ||
        terminal.diagnostics.empty() || terminal.diagnostics.front().code != "cook.module" ||
        !terminal.game_manifest.empty() || !terminal.module_manifest.empty()) {
        std::cerr << "Cook without a module manifest was silently downgraded instead of failing\n";
        return 73;
    }
    if (!has_actual_cook_process_observations(terminal)) {
        std::cerr << "Cook failure lost actual Module/Process/Invocation ownership or reported a false commit\n";
        return 74;
    }
    tooling::ProjectOperationRequest cook_request{
        project,        root, "Debug", "Game", tooling::ProjectOperation::kCook, root / "missing.module.manifest.json",
        root / "Cooked"};
    const auto cook_started = service.start(cook_request);
    if (!cook_started)
        return 31;
    done = false;
    for (int i = 0; i < 100000 && !done; ++i) {
        const auto polled = service.poll(cook_started.value);
        if (!polled)
            return 32;
        terminal = polled.value;
        done = terminal.execution_complete;
        if (!done)
            aeg::concurrency::yield_current_thread();
    }
    if (!done || terminal.state != tooling::ProjectOperationState::kFailed || terminal.diagnostics.empty() ||
        terminal.diagnostics.front().code != "cook.module" || !has_actual_cook_process_observations(terminal))
        return 33;
    const auto missing =
        service.start({root / "missing.aegproj", root, "Debug", "Game", tooling::ProjectOperation::kBuild});
    if (!missing)
        return 4;
    if (!service.cancel(missing.value))
        return 5;
    const auto cancelled = service.poll(missing.value);
    if (!cancelled || cancelled.value.state != tooling::ProjectOperationState::kCancelled)
        return 6;
    bool retired = false;
    for (int i = 0; i < 100000 && !retired; ++i) {
        const auto observed = service.poll(missing.value);
        if (!observed || observed.value.state != tooling::ProjectOperationState::kCancelled) return 75;
        retired = observed.value.execution_complete;
        if (!retired) aeg::concurrency::yield_current_thread();
    }
    if (!retired) return 76;
    if (service.poll(999999))
        return 7;
    const auto invalid_wait = service.wait(0);
    const auto unknown_wait = service.wait(999999);
    const auto final_cancelled = service.wait(missing.value);
    if (invalid_wait || invalid_wait.result.code != foundation::ResultCode::kInvalidArgument || unknown_wait ||
        unknown_wait.result.code != foundation::ResultCode::kNotFound || !final_cancelled ||
        final_cancelled.value.state != tooling::ProjectOperationState::kCancelled ||
        !final_cancelled.value.execution_complete) return 78;
    if (!test_cook_publishes_actual_scene_and_selected_module(std::filesystem::absolute(argv[1]), root / "ActualCook")) {
        std::cerr << "Cook failed to preserve actual commits, selected Module bytes, or readable Scene package\n";
        return 77;
    }
    const auto cmake = std::filesystem::absolute(argv[2]);
    const bool strict_no_engine = test_build_requires_explicit_inputs(root / "NoEngine", cmake, true);
    const bool strict_no_cmake = test_build_requires_explicit_inputs(root / "NoCmake", cmake, false);
    const bool live_progress = test_running_build_publishes_real_commit_and_cancel_retains_it(root / "LiveProgress", cmake);
    std::cout << "behavior_oracles strict_no_engine=" << strict_no_engine << " strict_no_cmake=" << strict_no_cmake
              << " live_configure_progress=" << live_progress << '\n';
    std::filesystem::remove_all(root, error);
    if (!strict_no_engine || !strict_no_cmake || !live_progress) return 73;
    std::cout << "Project operation smoke passed\n";
    return 0;
}
