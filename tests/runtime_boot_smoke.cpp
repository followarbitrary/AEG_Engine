#include "feature_clock_test_fixture.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>

#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "engine_frame_orchestrator.hpp"
#include "game_build_manifest.hpp"
#include "module_bundle.hpp"
#include "module_api.h"
#include "feature_graph_asset.hpp"
#include "feature_registry.hpp"
#include "runtime_bootstrap.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"
#include "concurrency.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"
#include "world_scope.hpp"
#include "rhi.hpp"

namespace {
uint32_t g_runtime_task_runs = 0;
uint32_t g_runtime_game_starts = 0;
uint32_t g_runtime_game_stops = 0;
aeg::concurrency::CompletionSource g_runtime_started;
aeg::concurrency::CompletionSource g_render_started;
double g_runtime_clock_seconds = 0.0;

double runtime_clock(void*) noexcept {
    g_runtime_clock_seconds += 1.0 / 60.0;
    return g_runtime_clock_seconds;
}

aeg::application::runtime::GameRuntimeStatus runtime_game_start(void*, void*) {
    ++g_runtime_game_starts;
    return {};
}

aeg::application::runtime::GameRuntimeStatus runtime_game_stop(void*, void*) {
    ++g_runtime_game_stops;
    return {};
}

class ResolverProbe final : public aeg::renderer::RenderResourceResolver {
public:
    aeg::foundation::ResultValue<aeg::renderer::RenderMeshResource> resolve_mesh(
        aeg::renderer::MeshAssetId) const override {
        return {aeg::foundation::not_ready("resolver probe"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        aeg::renderer::MaterialInstanceId) const override {
        return {aeg::foundation::not_ready("resolver probe"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderObjectResource> resolve_object(
        const aeg::renderer::RenderInstance&, const aeg::renderer::RenderView&) const override {
        return {aeg::foundation::not_ready("resolver probe"), {}};
    }
};

aeg::feature::FeatureDefinition runtime_feature() {
    aeg::feature::FeatureDefinition definition{};
    definition.id = aeg::feature::FeatureDefinitionId("Smoke.Runtime");
    definition.version = {1, 0, 0};
    definition.contract.definition_id = definition.id;
    definition.contract.version = definition.version;
    definition.contract.domains = aeg::feature::to_mask(aeg::feature::FeatureDomain::kLogic);
    definition.contract.execution = aeg::feature::ExecutionKind::kProcess;
    definition.contract.state = aeg::feature::StatePolicy::kProcessState;
    definition.contract.scope = aeg::feature::ScopeContract{aeg::feature::ScopeId("Engine")};
    definition.contract.clock = aeg::feature::make_periodic_clock_contract(aeg::feature::ClockId("Smoke.RuntimeClock"), 60, 8, aeg::feature::LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    definition.contract.permissions.version = 1;
    definition.contract.has_continuation = true;
    definition.execute = [](const aeg::feature::InvocationToken& token, const aeg::feature::StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>)
        -> aeg::foundation::ResultValue<aeg::feature::StateDelta> {
        ++g_runtime_task_runs;
        static_cast<void>(g_runtime_started.complete());
        return {aeg::foundation::Result::success(), {token.state_version, g_runtime_task_runs}};
    };
    return definition;
}

aeg::feature::FeatureDefinition runtime_render_feature() {
    using namespace aeg::feature;
    auto definition = runtime_feature();
    definition.id = FeatureDefinitionId("Smoke.RuntimeRender");
    definition.contract.definition_id = definition.id;
    definition.contract.domains = to_mask(FeatureDomain::kRender);
    definition.contract.execution = ExecutionKind::kContinuous;
    definition.contract.state = StatePolicy::kStateless;
    definition.contract.scope = {ScopeId("Frame")};
    definition.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("Smoke.RenderClock"), 60, 4, LateDataPolicy::kDrop,
        aeg::feature::DeadlinePolicy::kObserve, definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    definition.contract.has_continuation = false;
    definition.contract.permissions = {kAllRenderIntentCapabilities, 1};
    definition.contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    definition.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        const RenderTextureReference texture{RenderResourceId("Boot.Color"), 1, false};
        RenderIntent intent{};
        const RenderTextureReference copied{RenderResourceId("Boot.Copy"), 1, false};
        intent.textures = {{texture.id, 1, 4, 4, RenderTextureLifetime::kHistory}, {copied.id, 1, 4, 4}};
        intent.operations = {RenderClearColor{texture, {1.f, 0.f, 0.f, 1.f}}, RenderCopyTexture{texture, copied, 4, 4}};
        intent.final_output = copied;
        StateDelta delta{token.state_version};
        delta.render_intent = std::move(intent);
        static_cast<void>(g_render_started.complete());
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    return definition;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace aeg;
    {
        module::FeatureModuleDefinition validation_definition{};
        validation_definition.descriptor.id = module::ModuleId{"OwnedHost.Validation"};
        validation_definition.descriptor.name = "OwnedHost.Validation";
        application::runtime::RuntimeBootRequest invalid_request{};
        invalid_request.owned_host = true;
        invalid_request.headless = true;
        application::runtime::RuntimeBootstrap validation_bootstrap;
        const auto invalid_result =
            validation_bootstrap.initialize(invalid_request, std::move(validation_definition),
                                            {"owned-host-validation", "windows-x64-vulkan", "Debug"});
        if (invalid_result || invalid_result.code != foundation::ResultCode::kInvalidArgument)
            return 1;
    }
    const fs::path root = fs::temp_directory_path() / "aeg_runtime_boot_smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);

    const auto scene_id = tooling::AssetId::parse("00000000-0000-4000-8000-000000000001");
    const auto feature_graph_id = tooling::AssetId::parse("00000000-0000-4000-8000-000000000002");
    const auto texture_id = tooling::AssetId::parse("00000000-0000-4000-8000-000000000003");
    if (!scene_id || !feature_graph_id || !texture_id)
        return 1;
    tooling::CookedAsset scene{};
    scene.id = scene_id.value;
    scene.kind = tooling::AssetKind::kScene;
    scene.schema_version = 1;
    aeg::asset::ScenePayload scene_payload{};
    scene::SceneAsset scene_document(scene::SceneAssetId{0x4000, 0x8000000000000001ull}, "RuntimeWorld", 1);
    if (!scene_document.add_entity({scene::SceneEntityId{1, 2}, {}, "Entity", scene::SceneEntityFlags::kNone, {}}))
        return 2;
    serialization::BinaryWriter archive;
    if (!scene::SceneSerializer::serialize(scene_document, archive))
        return 2;
    scene_payload.serialized_scene.assign(
        reinterpret_cast<const std::byte*>(archive.data().data()),
        reinterpret_cast<const std::byte*>(archive.data().data() + archive.data().size()));
    const auto scene_bytes = aeg::asset::encode_scene_payload(scene_payload);
    if (!scene_bytes)
        return 2;
    scene.payload = scene_bytes.value;
    auto feature_definition = runtime_feature();
    auto render_definition = runtime_render_feature();
    feature::ParameterMetadata texture_parameter{};
    texture_parameter.type = feature::ParameterType::kAssetReference;
    texture_parameter.default_value = foundation::AssetReference{texture_id.value.to_string()};
    texture_parameter.constraint = feature::AssetConstraint{false, foundation::AssetKind::kTexture};
    feature_definition.contract.parameters.push_back({feature::ParameterId{"Smoke.Texture"}, 1, 16, true, texture_parameter});
    // Runtime registration stamps module ownership into Feature definitions;
    // author the same owner before computing the graph contract digest so the
    // cooked startup graph and runtime registry describe the same contract.
    feature_definition.owner_module = aeg::feature::ModuleIdentity("Smoke.GameModule");
    render_definition.owner_module = feature_definition.owner_module;
    aeg::feature::FeatureRegistry feature_registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(feature_registry, feature_definition) || !aeg::feature::test::register_clock_test_definition(feature_registry, render_definition))
        return 3;
    aeg::feature::FeatureGraphDocument feature_graph{};
    feature_graph.asset_uuid = feature_graph_id.value.to_string();
    feature_graph.schema_version = 1;
    feature_graph.graph_version = 1;
    feature_graph.contract_digest = feature_registry.contract_digest();
    feature_graph.nodes.push_back({aeg::feature::FeatureNodeId("Runtime"), feature_definition.id,
                                   feature_definition.version, aeg::feature::OwnerPath{}, {}});
    feature_graph.nodes.push_back({aeg::feature::FeatureNodeId("Render"), render_definition.id,
                                   render_definition.version, aeg::feature::OwnerPath{}, {}});
    const auto feature_graph_payload = aeg::asset::encode_feature_graph_asset(feature_graph);
    if (!feature_graph_payload)
        return 3;
    tooling::CookedAsset feature_graph_asset = scene;
    feature_graph_asset.id = feature_graph_id.value;
    feature_graph_asset.kind = tooling::AssetKind::kFeatureGraph;
    feature_graph_asset.payload = feature_graph_payload.value;
    const auto scene_envelope = tooling::CookedAssetCodec{}.encode(scene);
    const auto feature_graph_envelope = tooling::CookedAssetCodec{}.encode(feature_graph_asset);
    if (!scene_envelope || !feature_graph_envelope)
        return 4;
    const auto write_package = [&](foundation::AssetKind actual_texture_kind) -> foundation::Result {
        tooling::CookedAsset texture{};
        texture.id = texture_id.value;
        texture.kind = actual_texture_kind;
        texture.schema_version = 1;
        texture.payload = {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                           std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                           std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0},
                           std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        const auto texture_envelope = tooling::CookedAssetCodec{}.encode(texture);
        if (!texture_envelope) return texture_envelope.result;
        return tooling::AssetPackageWriter{}.write(
            root / "game.aegbin",
            {{scene.id, scene.kind, scene.schema_version, scene_envelope.value},
             {feature_graph_asset.id, feature_graph_asset.kind, feature_graph_asset.schema_version,
              feature_graph_envelope.value},
             {texture.id, texture.kind, texture.schema_version, texture_envelope.value}});
    };
    if (!write_package(foundation::AssetKind::kTexture)) return 3;
    std::ofstream(root / "game.manifest") << "package = \"game.aegbin\"\n"
                                          << "build_id = \"smoke-build\"\n"
                                          << "target = \"windows-x64-vulkan\"\n"
                                          << "configuration = \"Debug\"\n";
    std::ifstream package_input(root / "game.aegbin", std::ios::binary);
    const std::string package_bytes((std::istreambuf_iterator<char>(package_input)), {});
    package_input.close();
    module::FeatureModuleDefinition definition{};
    definition.descriptor.id = module::ModuleId{"Smoke.GameModule"};
    definition.descriptor.name = "Smoke.GameModule";
    if (!module::add_feature_definition(definition, feature_definition) || !module::add_feature_definition(definition, render_definition))
        return 4;
    module::ModuleMetadata module_metadata{};
    module_metadata.schema_version = module::kModuleMetadataSchemaVersion;
    module_metadata.module = definition.descriptor;
    module_metadata.feature_catalog = definition.feature_catalog;
    module_metadata.abi_major = AEG_MODULE_ABI_MAJOR;
    module_metadata.abi_minor = AEG_MODULE_ABI_MINOR;
    module_metadata.sdk_contract_major = module::kModuleSdkContractMajor;
    module_metadata.sdk_contract_minor = module::kModuleSdkContractMinor;
    const auto module_contract = module::module_contract_sha256(module_metadata);
    if (!module_contract)
        return 4;
    aeg::asset::GameBuildManifest game_manifest{};
    game_manifest.project_uuid = "smoke-project";
    game_manifest.module_id = "Smoke.GameModule";
    game_manifest.build_id = "smoke-build";
    game_manifest.module_contract_digest = module_contract.value;
    const auto package_digest = aeg::module::module_content_sha256(package_bytes);
    if (!package_digest)
        return 5;
    game_manifest.package_digest = package_digest.value;
    game_manifest.startup_scene = aeg::foundation::AssetId::parse("00000000-0000-4000-8000-000000000001").value;
    game_manifest.startup_feature_graphs.push_back(feature_graph_id.value);
    game_manifest.target = "windows-x64-vulkan";
    game_manifest.configuration = "Debug";
    game_manifest.package_name = "game.aegbin";
    game_manifest.module_manifest_name = "module/module.manifest.json";
    game_manifest.module_manifest_sha256 = "test-module-manifest";
    const auto encoded_manifest = aeg::asset::encode_game_build_manifest(game_manifest);
    if (!encoded_manifest)
        return 6;
    std::ofstream binary_manifest(root / "game.manifest.bin", std::ios::binary);
    binary_manifest.write(reinterpret_cast<const char*>(encoded_manifest.value.data()),
                          static_cast<std::streamsize>(encoded_manifest.value.size()));
    if (!binary_manifest)
        return 7;
    binary_manifest.close();

    auto device = rhi::create_device({});
    if (!device) return 13;
    render_graph::FrameContext frame(*device.value);
    render_graph::RenderGraph render_graph(*device.value);
    application::runtime::RuntimeBootstrap bootstrap;
    const application::runtime::RuntimeBuildIdentity identity{"smoke-build", "windows-x64-vulkan", "Debug"};
    application::runtime::RuntimeBootRequest request{};
    request.manifest_path = root / "game.manifest";
    request.max_frames = 2;
    request.clock.now_seconds = &runtime_clock;
    request.trace_path = root / "trace.txt";
    application::runtime::RuntimeAssetResolver::UploadContext upload_context{};
    application::runtime::RuntimeFrameHostResources host_contract{};
    host_contract.frame_context = &frame;
    host_contract.render_graph = &render_graph;
    host_contract.upload_context = &upload_context;
    host_contract.require_resolved_render_resources = true;
    ResolverProbe resolver_probe;
    engine::EngineFrameDesc frame_desc_probe{};
    host_contract.apply_frame_desc(frame_desc_probe, &resolver_probe);
    if (frame_desc_probe.render_resource_resolver != &resolver_probe ||
        !frame_desc_probe.require_resolved_render_resources)
        return 8;
    request.frame_host = &host_contract;
    const auto pin_current_package = [&]() -> bool {
        std::ifstream input(root / "game.aegbin", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        const auto digest = module::module_content_sha256(bytes);
        if (!digest) return false;
        game_manifest.package_digest = digest.value;
        const auto encoded = asset::encode_game_build_manifest(game_manifest);
        if (!encoded) return false;
        std::ofstream output(root / "game.manifest.bin", std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded.value.data()), static_cast<std::streamsize>(encoded.value.size()));
        return static_cast<bool>(output);
    };
    // Pin the mutated package so rejection must come from the effective
    // Feature parameter's kind contract, rather than the package digest.
    if (!write_package(foundation::AssetKind::kMesh) || !pin_current_package()) return 15;
    const application::runtime::GameRuntimeCallbacks callbacks{
        &runtime_game_start, nullptr, nullptr, &runtime_game_stop, nullptr};
    const auto wrong_kind = bootstrap.initialize(request, definition, identity, callbacks);
    if (wrong_kind || wrong_kind.code != foundation::ResultCode::kInvalidArgument ||
        wrong_kind.message.find("kind") == std::string::npos || bootstrap.startup_feature_graph_count() != 0 ||
        bootstrap.frame_count() != 0 || g_runtime_task_runs != 0 || g_runtime_game_starts != 0 || g_runtime_game_stops != 0) {
        std::cerr << "Bootstrap did not reject the actual packaged parameter kind before publication: "
                  << wrong_kind.message << '\n';
        return 15;
    }
    // Reuse the same Bootstrap after failure. Successful startup and its
    // existing ticks prove cleanup and the resolver's owning package survive.
    if (!write_package(foundation::AssetKind::kTexture) || !pin_current_package()) return 16;
    if (const auto result = bootstrap.initialize(request, std::move(definition), identity, callbacks); !result) {
        std::cerr << "initialize failed: " << result.message << "\n";
        return 8;
    }
    if (bootstrap.startup_feature_graph_count() != 1 || !bootstrap.startup_world_scope() || g_runtime_game_starts != 1 ||
        g_runtime_game_stops != 0)
        return 9;
    const auto tick_one = bootstrap.tick();
    if (!g_runtime_started.completion().wait(2'000'000'000)) {
        std::cerr << "Runtime Feature did not execute asynchronously\n";
        return 10;
    }
    const auto tick_two = bootstrap.tick();
    if (!tick_one || !tick_two || bootstrap.frame_count() != 2) {
        std::cerr << "tick failed: one=" << tick_one.message << " two=" << tick_two.message
                  << " frames=" << bootstrap.frame_count() << "\n";
        return 10;
    }
    const auto snapshot = bootstrap.startup_world_scope()->latest_snapshot();
    if (!snapshot || snapshot.value->frame_number() != 2 || snapshot.value->entity_count() != 1)
        return 12;
    if (!g_render_started.completion().wait(2'000'000'000)) return 14;
    bool projected = false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        unsigned authored_passes = 0;
        for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(render_graph).pass_plans) {
            if (pass.feature_work && pass.feature_work->feature_definition_id == render_definition.id && pass.live &&
                pass.feature_work->invocation.invocation_id.is_valid() && pass.feature_work->frame_index == bootstrap.frame_count())
                ++authored_passes;
        }
        projected = authored_passes == 2;
        if (projected) break;
        if (!bootstrap.tick()) return 14;
    }
    if (!projected) {
        std::cerr << "RuntimeBootstrap never built the authored Render Feature\n";
        return 14;
    }
    const auto rendered_frames = bootstrap.frame_count();
    if (!bootstrap.shutdown() || !fs::exists(root / "trace.txt"))
        return 11;
    // Shutdown joins callbacks. Read the worker-owned counter only afterwards;
    // accepted queued clock work may be cancelled when the host stops.
    if (g_runtime_task_runs < 1 || g_runtime_task_runs > rendered_frames || g_runtime_game_starts != 1 ||
        g_runtime_game_stops != 1) return 10;
    fs::remove_all(root, error);
    std::cout << "Runtime boot smoke passed\n";
    return 0;
}
