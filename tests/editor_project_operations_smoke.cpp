/** @brief Actual ImGui project-operation controls call the real asynchronous Tooling service. */
#include "feature_clock_test_fixture.hpp"
#include "../Application/Editor/private/editor_project_operations.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <imgui_internal.h>

#include "Components/component_bundle.hpp"
#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "asset_metadata.hpp"
#include "asset_package_reader.hpp"
#include "concurrency.hpp"
#include "feature_graph_io.hpp"
#include "feature_registry.hpp"
#include "game_build_manifest.hpp"
#include "module_bundle_test_fixture.hpp"
#include "module_dynamic_feature_fixture.hpp"
#include "project_context.hpp"
#include "runtime_bootstrap.hpp"
#include "runtime_feature_control.hpp"
#include "scene_asset.hpp"
#include "serialization.hpp"

namespace {
using namespace aeg;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void succeeds(const foundation::Result& result, const std::string& message) {
    require(bool(result), message + ": " + result.message);
}
template <typename T>
void succeeds(const foundation::ResultValue<T>& result, const std::string& message) {
    succeeds(result.result, message);
}
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "cannot read actual file: " + path.string());
    return {std::istreambuf_iterator<char>(stream), {}};
}

struct Fixture final {
    fs::path root, project, scene, graph;
    module::ModuleBundle bundle{};
    application::project::ProjectConfig config{};
    foundation::AssetId scene_asset{}, graph_asset{};
};

Fixture write_fixture(const fs::path& root, const fs::path& binary,
                      const char* project_name = "Project.aegproj") {
    Fixture fixture{};
    fixture.root = fs::absolute(root);
    fixture.project = fixture.root / project_name;
    fixture.scene = fixture.root / "Assets" / "Scenes" / "Main.aegscene";
    fixture.graph = fixture.root / "Assets" / "Features" / "Main.aegfeature";
    fs::create_directories(fixture.scene.parent_path());
    fs::create_directories(fixture.graph.parent_path());
    fixture.scene_asset = foundation::AssetId::parse("00000000-0000-4000-8000-000000000201").value;
    fixture.graph_asset = foundation::AssetId::parse("00000000-0000-4000-8000-000000000203").value;
    const auto definition = dynamic_feature_test::definition();
    if (!binary.empty()) {
        fixture.bundle = module_bundle_test::write_bundle(fs::absolute(binary), fixture.root / "ModuleBundle",
            module_bundle_test::metadata_for(definition));
    } else {
        require(!fs::exists(fixture.root / "ModuleBundle"), "source Module fixture requires an absent initial bundle");
    }

    reflection::TypeRegistry reflection;
    succeeds(engine::components::register_render_component_reflection(reflection), "register real Scene component schemas");
    scene::SceneAsset scene(scene::SceneAssetId{0x4000, 0x8000000000000201ull}, "Editor Operation", 1);
    const scene::SceneEntityId camera_entity{3, 4};
    succeeds(scene.add_entity({camera_entity, {}, "Camera", scene::SceneEntityFlags::kNone, {}}), "author real Scene entity");
    engine::components::Transform transform{};
    transform.translation = {5.f, 2.f, 3.f};
    const engine::components::Camera camera{};
    succeeds(scene.add_reflected_component(camera_entity, ecs::ComponentTypeId(engine::components::kTransformName),
        reflection, reflection::TypeId(engine::components::kTransformName), &transform), "author real Game.Transform");
    succeeds(scene.add_reflected_component(camera_entity, ecs::ComponentTypeId(engine::components::kCameraName),
        reflection, reflection::TypeId(engine::components::kCameraName), &camera), "author real Camera");
    serialization::BinaryWriter scene_bytes;
    succeeds(scene::SceneSerializer::serialize(scene, scene_bytes), "serialize actual Scene");
    {
        std::ofstream output(fixture.scene, std::ios::binary);
        output.write(reinterpret_cast<const char*>(scene_bytes.data().data()),
                     static_cast<std::streamsize>(scene_bytes.size()));
        require(bool(output), "write actual Scene source");
    }
    tooling::AssetMeta scene_meta{};
    scene_meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000202").value;
    scene_meta.importer = "aeg.scene";
    scene_meta.products = {{fixture.scene_asset, tooling::AssetKind::kScene, "Main"}};
    succeeds(tooling::AssetMetaCodec{}.save(fixture.scene.string() + ".meta", scene_meta), "write actual Scene metadata");

    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    for (const auto& feature : definition.feature_definitions)
        succeeds(aeg::feature::test::register_clock_test_definition(registry, feature), "register actual module Feature contract for authored graph");
    const auto* process = registry.find_definition(dynamic_feature_test::kProcessLogic);
    require(process != nullptr, "actual DLL Process definition missing");
    const auto parameters = feature::encode_feature_parameters(
        std::vector<feature::FeatureParameterRecord>{{feature::ParameterId("Channel"), 1, {4}}},
        process->contract.parameters);
    succeeds(parameters, "encode actual Process parameter contract");
    feature::FeatureGraphDocument graph{};
    graph.asset_uuid = fixture.graph_asset.to_string(); graph.schema_version = 1; graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    const feature::FeatureNodeId process_node{"Operation.Logic"};
    graph.nodes = {{process_node, process->id, process->version, {}, parameters.value}};
    graph.process_subgraphs = {{process_node, {"Idle", "Active"}, {"Idle->Active", "Active->Idle"}, 1, "Idle", {}}};
    succeeds(application::authoring::save_feature_graph_document(fixture.graph, graph), "save non-empty actual FeatureGraph");
    tooling::AssetMeta graph_meta{};
    graph_meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000204").value;
    graph_meta.importer = "aeg.feature_graph";
    graph_meta.products = {{fixture.graph_asset, tooling::AssetKind::kFeatureGraph, "Main"}};
    succeeds(tooling::AssetMetaCodec{}.save(fixture.graph.string() + ".meta", graph_meta), "write actual graph metadata");

    fixture.config.name = "Editor Operations";
    fixture.config.project_uuid = "00000000-0000-4000-8000-000000000200";
    fixture.config.target = "Fixture";
    fixture.config.module.id = "com.aeg.dynamic.fixture";
    fixture.config.module.target = "ActualModuleTarget";
    fixture.config.startup_scene = "Assets/Scenes/Main.aegscene";
    fixture.config.startup_feature_graphs = {"Assets/Features/Main.aegfeature"};
    fixture.config.paths.build = "Actual Build";
    fixture.config.paths.cache = "Actual Cache";
    fixture.config.paths.binaries = "Actual Binaries";
    succeeds(application::project::ProjectFileCodec{}.save(fixture.project, fixture.config), "save exact project source");
    if (binary.empty()) {
        const auto source_directory = fixture.root / "Source";
        fs::create_directories(source_directory);
        auto metadata = module_bundle_test::metadata_for(definition);
        metadata.build_id = "b"; metadata.target = "Fixture"; metadata.configuration = "Debug";
        const auto encoded = module::encode_module_metadata(metadata);
        succeeds(encoded, "encode actual source Module metadata with the official codec");
        {
            std::ofstream output(source_directory / "GameModule.metadata.json", std::ios::binary);
            output << encoded.value;
            output.close();
            require(bool(output), "write actual source Module metadata");
        }
        {
            std::ofstream output(source_directory / "GameModule.cpp");
            output << R"CPP(#include "module_export.hpp"
#include "module_dynamic_feature_fixture.hpp"

AEG_DEFINE_GAME_MODULE(aeg_define_OperationModule) {
    return dynamic_feature_test::definition();
}
AEG_EXPORT_GAME_MODULE_API(aeg_define_OperationModule);
)CPP";
            output.close();
            require(bool(output), "write actual executable Module source");
        }
        std::ofstream cmake(fixture.root / "CMakeLists.txt");
        cmake << R"CMAKE(cmake_minimum_required(VERSION 3.25)
project(EditorProjectOperationBuildFixture LANGUAGES CXX)
if(NOT DEFINED ENGINE_DIR OR NOT IS_DIRECTORY "${ENGINE_DIR}")
    message(FATAL_ERROR "Explicit real ENGINE_DIR is required")
endif()
set(CMAKE_BUILD_TYPE Debug CACHE STRING "Fixture configuration")
set(AEG_BUILD_EDITOR OFF CACHE BOOL "No nested Editor" FORCE)
set(AEG_BUILD_TESTS OFF CACHE BOOL "No nested engine tests" FORCE)
set(AEG_BUILD_TOOLS ON CACHE BOOL "Build the actual Module metadata tool" FORCE)
include("${ENGINE_DIR}/cmake/EngineTools.cmake")
aeg_add_game_module(ActualModuleTarget
    SOURCES Source/GameModule.cpp
    METADATA "${CMAKE_CURRENT_SOURCE_DIR}/Source/GameModule.metadata.json"
    BUILD_ID b
    TARGET_PLATFORM Fixture
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ModuleBundle")
target_include_directories(ActualModuleTarget PRIVATE "${ENGINE_DIR}/tests")
)CMAKE";
        cmake.close();
        require(bool(cmake), "write actual GameSDK CXX Module build project");
        require(!fs::exists(fixture.root / "ModuleBundle"), "source writer created a prebuilt Module bundle");
        return fixture;
    }
    // This technical CMake project tests real process launch/build/cancellation. The actual
    // executable Module used by Cook is the independently verified DLL bundle above.
    std::ofstream cmake(fixture.root / "CMakeLists.txt");
    cmake << R"CMAKE(cmake_minimum_required(VERSION 3.25)
project(EditorProjectOperationFixture LANGUAGES NONE)
if(NOT DEFINED ENGINE_DIR OR NOT IS_DIRECTORY "${ENGINE_DIR}")
    message(FATAL_ERROR "Explicit real ENGINE_DIR is required")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/configured.marker" "${CMAKE_CURRENT_SOURCE_DIR}\n${ENGINE_DIR}\n")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hold-configure")
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/configure-entered.marker" "actual CMake configure\n")
    while(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hold-configure")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.01)
    endwhile()
endif()
add_custom_target(ActualModuleTarget
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/built.marker")
)CMAKE";
    require(bool(cmake), "write real CMake project");
    return fixture;
}

class Ui final {
public:
    explicit Ui(application::EditorProjectOperations& operations) : operations_(operations) {
        context_ = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = {1300, 950}; io.DeltaTime = 1.f / 60.f;
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        context_->ErrorCallbackUserData = &errors_;
        context_->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
            static_cast<std::vector<std::string>*>(data)->emplace_back(message);
        };
        frame();
    }
    ~Ui() { ImGui::DestroyContext(context_); }
    void frame(const char* button = nullptr) {
        ImGui::SetCurrentContext(context_);
        ImGui::NewFrame();
        if (button) {
            auto* window = ImGui::FindWindowByName("Project Operations");
            require(window != nullptr, "actual Project Operations window missing");
            context_->NavWindow = window; context_->NavId = window->GetID(button);
            context_->NavInputSource = ImGuiInputSource_Keyboard;
            context_->NavActivateId = context_->NavActivateDownId = context_->NavId;
        }
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Project Operations");
        operations_.draw();
        ImGui::End();
        ImGui::Render();
        static_cast<void>(operations_.poll());
        require(errors_.empty(), errors_.empty() ? "" : "actual operation UI recovery: " + errors_.front());
    }
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (operations_.active() && std::chrono::steady_clock::now() < deadline) {
            frame(); concurrency::yield_current_thread();
        }
        require(!operations_.active(), "real operation did not retire within the bounded UI pump");
        frame();
    }
private:
    application::EditorProjectOperations& operations_;
    ImGuiContext* context_{};
    std::vector<std::string> errors_{};
};

tooling::ProjectOperationRequest request_for(const Fixture& fixture, const fs::path& cmake, const fs::path& engine) {
    tooling::ProjectOperationRequest request{};
    request.project_file = fixture.project; request.engine_directory = engine;
    request.configuration = "Debug"; request.target = fixture.config.module.target;
    request.module_manifest = fixture.bundle.manifest_path;
    request.output_directory = fixture.root / "Cooked Output";
    request.cmake_executable = cmake;
    return request;
}

void verify_cooked(const Fixture& fixture, const tooling::ProjectOperationStatus& status, const fs::path& output) {
    require(status.state == tooling::ProjectOperationState::kSucceeded && bool(status.result) &&
            !status.game_manifest.empty() && fs::is_regular_file(status.game_manifest) &&
            status.module_manifest == fixture.bundle.manifest_path && fs::is_regular_file(output / "current.toml"),
            "successful UI Cook did not publish actual Module/package/selector files");
    const auto contents = bytes(status.game_manifest);
    const auto manifest = asset::decode_game_build_manifest(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(contents.data()), contents.size()));
    succeeds(manifest, "decode actual Cook manifest");
    require(manifest.value.module_id == fixture.config.module.id && manifest.value.target == "Fixture" &&
            manifest.value.configuration == "Debug" && manifest.value.startup_scene == fixture.scene_asset &&
            manifest.value.startup_feature_graphs == std::vector<foundation::AssetId>{fixture.graph_asset},
            "Cook selected another project, Module, or startup roots");
    const auto package = asset::AssetPackageReader::open(status.game_manifest.parent_path() / manifest.value.package_name);
    succeeds(package, "open actual Cook package");
    const auto scene = package.value.read_cooked(fixture.scene_asset);
    const auto graph = package.value.read_cooked(fixture.graph_asset);
    succeeds(scene, "read actual Scene package entry"); succeeds(graph, "read actual graph package entry");
    require(scene.value.kind == foundation::AssetKind::kScene && !scene.value.payload.empty() &&
            graph.value.kind == foundation::AssetKind::kFeatureGraph && !graph.value.payload.empty(),
            "Cook produced empty or mistyped startup assets");
    const auto delivered = module::validate_module_bundle(status.game_manifest.parent_path() / manifest.value.module_manifest_name);
    succeeds(delivered, "validate actual delivered executable Module");
    require(delivered.value.binary_sha256 == fixture.bundle.binary_sha256 &&
            delivered.value.metadata_sha256 == fixture.bundle.metadata_sha256 &&
            delivered.value.metadata.contract_digest == fixture.bundle.metadata.contract_digest,
            "actual Cook delivery changed the selected Module bytes or contract");
}

void exercise(const fs::path& binary, const fs::path& cmake, const fs::path& engine) {
    const auto root = fs::temp_directory_path() / "aeg_editor_project_operations_smoke";
    std::error_code error; fs::remove_all(root, error);
    const auto fixture = write_fixture(root, binary, "Selected Project.aegproj");
    auto decoy = fixture.config;
    decoy.name = "Never select this project"; decoy.module.id = "com.aeg.other";
    const auto decoy_path = root / "Project.aegproj";
    succeeds(application::project::ProjectFileCodec{}.save(decoy_path, decoy), "write same-directory decoy project");
    const std::vector<fs::path> protected_paths{fixture.project, decoy_path, fixture.scene, fixture.graph,
        fixture.bundle.manifest_path, fixture.bundle.metadata_path, fixture.bundle.binary_path};
    std::vector<std::string> original;
    for (const auto& path : protected_paths) original.push_back(bytes(path));

    application::EditorProjectOperations operations;
    succeeds(operations.set_project(fixture.project), "select exact project file");
    require(operations.request().project_file == fixture.project, "controller rediscovered another same-directory project");
    Ui ui(operations);
    auto request = request_for(fixture, cmake, engine);
    request.module_manifest.clear();
    succeeds(operations.set_request(request), "set explicit incomplete Cook request");
    ui.frame("Cook"); ui.settle();
    require(operations.status().state == tooling::ProjectOperationState::kFailed && !operations.status().result &&
            !operations.status().diagnostics.empty() && !fs::exists(root / "Actual Build" / "configured.marker") &&
            !fs::exists(request.output_directory), "Cook without a manifest succeeded, launched CMake, or wrote delivery files");

    request = request_for(fixture, cmake, engine);
    succeeds(operations.set_request(request), "set explicit real Build request");
    { std::ofstream gate(root / "hold-configure"); gate << "external CMake configure gate\n"; }
    ui.frame("Build");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!fs::exists(root / "configure-entered.marker") && operations.active() &&
           std::chrono::steady_clock::now() < deadline) { ui.frame(); concurrency::yield_current_thread(); }
    require(fs::exists(root / "configure-entered.marker") && operations.active(), "Build UI did not enter the actual CMake process");
    const auto before = operations.request();
    auto changed = request; changed.configuration = "Release";
    require(!operations.set_request(changed) && !operations.start(tooling::ProjectOperation::kCook) &&
            operations.request().project_file == fixture.project && operations.request().configuration == before.configuration,
            "busy controller replaced the real operation/request or admitted another slot");
    ui.frame("Cook");
    require(operations.active() && operations.request().operation == tooling::ProjectOperation::kBuild,
            "disabled Cook button changed the active Build");
    ui.frame("Cancel"); ui.settle();
    require(operations.status().state == tooling::ProjectOperationState::kCancelled &&
            operations.status().result.code == foundation::ResultCode::kCancelled &&
            !fs::exists(request.output_directory / "current.toml") && !fs::exists(root / "Actual Build" / "built.marker"),
            "Cancel did not retire the actual process or published output");
    operations.shutdown();
    require(!operations.active(), "shutdown retained an actual worker operation");
    fs::remove(root / "hold-configure");

    succeeds(operations.set_project(fixture.project), "reuse controller after cancellation/join");
    require(operations.request().project_file == fixture.project && operations.request().module_manifest.empty() &&
            operations.request().output_directory.empty() && operations.request().engine_directory.empty() &&
            operations.request().cmake_executable.empty(), "project selection did not reset previous explicit inputs");
    succeeds(operations.set_request(request), "set exact valid Cook inputs");
    ui.frame("Cook"); ui.settle();
    verify_cooked(fixture, operations.status(), request.output_directory);
    const auto retained = operations.status();
    const auto first_selector = bytes(request.output_directory / "current.toml");

    succeeds(operations.set_request(request), "set explicit Build and Cook inputs");
    ui.frame("Build and Cook"); ui.settle();
    verify_cooked(fixture, operations.status(), request.output_directory);
    require(fs::is_regular_file(root / "Actual Build" / "built.marker") &&
            bytes(root / "Actual Build" / "configured.marker").find(root.generic_string()) != std::string::npos &&
            bytes(request.output_directory / "current.toml") != first_selector,
            "Build and Cook did not execute actual CMake and publish a new delivery");
    require(fs::is_regular_file(retained.game_manifest), "later UI operation retired the earlier owning publication");
    succeeds(operations.set_project(decoy_path), "change the exact selected project");
    require(!operations.active() && operations.request().project_file == decoy_path &&
            operations.request().module_manifest.empty(), "project switch kept prior session inputs");
    operations.shutdown();
    for (std::size_t index = 0; index != protected_paths.size(); ++index)
        require(bytes(protected_paths[index]) == original[index], "operation mutated source/selected Module bytes");
}

void verify_runtime(const fs::path& root) {
    const auto selector = fs::absolute(root) / "Cooked" / "current.toml";
    require(fs::is_regular_file(selector), "actual Main Cook selector is missing");
    const auto original_selector = bytes(selector);
    application::runtime::RuntimeBootstrap bootstrap;
    application::runtime::RuntimeBootRequest request{};
    request.manifest_path = selector;
    request.headless = true;
    request.offscreen = true;
    double clock_seconds = 0.0;
    request.clock = {[](void* value) noexcept { return *static_cast<double*>(value); }, &clock_seconds};
    succeeds(bootstrap.initialize(request, std::nullopt, {"b", "Fixture", "Debug"}),
             "boot actual Main Cook delivery with its packaged executable Module");
    require(bootstrap.startup_feature_graph_count() == 1 && bootstrap.startup_world_scope() != nullptr,
            "actual Cooked startup graph or Scene was not loaded");

    application::runtime::FeatureRuntimeNodeSnapshot committed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        succeeds(bootstrap.tick(), "pump actual Cooked DLL Feature execution");
        const auto observed = application::runtime::RuntimeFeatureControl::inspect_node(
            bootstrap, "00000000-0000-4000-8000-000000000203", feature::FeatureNodeId("Operation.Logic"));
        if (observed && observed.value.owners.size() == 1 &&
            observed.value.owners.front().invocations.last_committed) {
            committed = observed.value;
            break;
        }
        concurrency::yield_current_thread();
    }
    require(committed.owners.size() == 1, "actual Cooked DLL Process never committed its invocation");
    const auto& owner = committed.owners.front();
    const auto& commit = *owner.invocations.last_committed;
    require(owner.definition_id == dynamic_feature_test::kProcessLogic &&
            owner.owner_module == feature::ModuleIdentity("com.aeg.dynamic.fixture") &&
            owner.owner.instance_id.is_valid() && owner.owner.definition_digest != 0 &&
            owner.contract.execution == feature::ExecutionKind::kProcess && owner.processes.size() == 1 &&
            commit.attempt.process == owner.processes.front().id &&
            owner.processes.front().state.commit_count >= 1 &&
            owner.processes.front().state.payload == std::vector<uint8_t>{1} &&
            commit.state_version == owner.processes.front().state.version &&
            commit.process_revision == owner.processes.front().revision &&
            commit.attempt.token.generation == committed.generation &&
            commit.attempt.token.invocation_id.is_valid() && commit.attempt.token.attempt_id.is_valid() &&
            commit.attempt.token.state_version.is_valid() && commit.attempt.token.process_revision.is_valid(),
            "Cooked Runtime commit did not come from the actual authored DLL Process owner");
    succeeds(bootstrap.shutdown(), "shutdown actual Cooked Module Runtime");
    require(bytes(selector) == original_selector && owner.invocations.last_committed.has_value(),
            "Runtime changed the Cook selector or invalidated the owning commit snapshot");
    std::cout << "EDITOR_OPERATION_COOKED_RUNTIME commit=" << commit.attempt.token.invocation_id.value()
              << " process=" << commit.attempt.process.value()
              << " generation=" << committed.generation.value() << '\n';
}
}  // namespace

int main(int argc, char** argv) try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-editor-operation-build-fixture") {
        const auto fixture = write_fixture(fs::path(argv[2]), {});
        require(fixture.project.filename() == "Project.aegproj" &&
                fs::is_regular_file(fixture.root / "Source" / "GameModule.cpp") &&
                fs::is_regular_file(fixture.root / "Source" / "GameModule.metadata.json") &&
                fs::is_regular_file(fixture.scene) && fs::is_regular_file(fixture.graph) &&
                !fs::exists(fixture.root / "ModuleBundle"), "actual source Module fixture is incomplete");
        std::cout << "EDITOR_OPERATION_BUILD_FIXTURE " << fixture.project.string() << '\n';
        return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--write-editor-operation-fixture") {
        const auto fixture = write_fixture(fs::path(argv[2]), fs::absolute(argv[3]));
        require(fixture.project.filename() == "Project.aegproj" &&
                fs::is_regular_file(fixture.bundle.manifest_path) && fs::is_regular_file(fixture.scene) &&
                fs::is_regular_file(fixture.graph), "actual Main operation fixture is incomplete");
        std::cout << "EDITOR_OPERATION_FIXTURE " << fixture.project.string() << '\n';
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--verify-editor-operation-cooked") {
        verify_runtime(fs::path(argv[2]));
        return 0;
    }
    require(argc == 4, "expected actual DLL, absolute CMake executable, and engine directory");
    const auto binary = fs::absolute(argv[1]), cmake = fs::absolute(argv[2]), engine = fs::absolute(argv[3]);
    require(fs::is_regular_file(binary) && fs::is_regular_file(cmake) && fs::is_directory(engine), "explicit real inputs missing");
    exercise(binary, cmake, engine);
    std::cout << "Editor Project Operations actual UI, Cook, CMake and cancellation passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Editor project operations: " << error.what() << '\n';
    return 1;
}
