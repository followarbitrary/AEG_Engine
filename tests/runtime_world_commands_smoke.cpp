/** @brief Packaged Runtime startup bindings deliver committed commands to actual Worlds. */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "../Engine/Runtime/private/feature_world_commands.hpp"
#include "Components/component_bundle.hpp"
#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "concurrency.hpp"
#include "engine_frame_orchestrator.hpp"
#include "feature_graph_asset.hpp"
#include "feature_registry.hpp"
#include "feature_world_transform.hpp"
#include "game_build_manifest.hpp"
#include "module_api.h"
#include "module_bundle.hpp"
#include "renderer.hpp"
#include "rhi.hpp"
#include "runtime_bootstrap.hpp"
#include "runtime_feature_control.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"
#include "world_scope.hpp"

namespace {
using namespace aeg;
using namespace aeg::feature;
using namespace aeg::application::runtime;
namespace fs = std::filesystem;

constexpr ModuleIdentity kModule{"Smoke.WorldCommands.Module"};
constexpr FeatureDefinitionId kPrimary{"Smoke.WorldCommands.Primary"};
constexpr FeatureDefinitionId kOtherAsset{"Smoke.WorldCommands.OtherAsset"};
constexpr FeatureDefinitionId kWrongChild{"Smoke.WorldCommands.ChildA"};
constexpr FeatureDefinitionId kSelectedChild{"Smoke.WorldCommands.ChildB"};
constexpr FeatureDefinitionId kComposite{"Smoke.WorldCommands.Composite"};
constexpr FeatureDefinitionId kRender{"Smoke.WorldCommands.Render"};
constexpr FeatureNodeId kSharedNode{"SharedNode"};
constexpr FeatureNodeId kCompositeNode{"CompositeRoot"};
constexpr FeatureNodeId kAuthorRoot{"World.Author"};
constexpr SideEffectId kEffect{"World.Transform"};
constexpr PortId kWorldInput{"Motion.WorldObservation"};
constexpr ClockId kClock{"Smoke.WorldCommands.Clock"};

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

engine::components::Transform transform(float x) {
    engine::components::Transform result{};
    result.translation = {x, 2.f, 3.f};
    result.scale = {2.f, 3.f, 4.f};
    return result;
}
bool is_transform(const engine::components::Transform* actual, float x) {
    const auto expected = transform(x);
    return actual && actual->translation == expected.translation && actual->rotation == expected.rotation &&
           actual->scale == expected.scale;
}

struct MotionCallbackRecord final {
    InvocationToken token{};
    StateVersion snapshot_version{};
    std::vector<uint8_t> input{};
};

struct ProducerProbe final {
    std::atomic_uint32_t calls{0};
    std::atomic_uint64_t worker_thread{0};
    std::mutex records_mutex{};
    std::vector<MotionCallbackRecord> motion_records{};
    std::atomic_bool release_motion{true};
};

FeatureDefinition producer(FeatureDefinitionId id, const OutboxIntent& command,
                           const std::shared_ptr<ProducerProbe>& probe, bool motion_observation = false) {
    FeatureDefinition definition{};
    definition.id = id; definition.version = {1, 0, 0}; definition.owner_module = kModule;
    auto& contract = definition.contract;
    contract.definition_id = id; contract.version = definition.version;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kProcess; contract.state = StatePolicy::kProcessState;
    contract.has_continuation = true; contract.scope = {ScopeId("World")};
    contract.clock = aeg::feature::make_periodic_clock_contract(kClock, 60, 4, LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.side_effects = {{kEffect, 1, SideEffectKind::kCommand, CommitPolicy::kOutbox}};
    if (motion_observation)
        contract.ports = {{kWorldInput, PortDirection::kInput, kWorldTransformInputSchema, kWorldTransformInputVersion}};
    definition.execute = [command, probe, motion_observation](const InvocationToken& token, const StateSnapshot& state,
        std::span<const uint8_t> input, std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
        probe->worker_thread.store(concurrency::current_thread_id().value, std::memory_order_relaxed);
        probe->calls.fetch_add(1, std::memory_order_relaxed);
        if (motion_observation) {
            std::scoped_lock lock(probe->records_mutex);
            probe->motion_records.push_back({token, state.version, {input.begin(), input.end()}});
        }
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.process_completed = !motion_observation;
        if (motion_observation) {
            while (!probe->release_motion.load(std::memory_order_acquire))
                static_cast<void>(concurrency::sleep_for(1'000'000));
            const auto observed = decode_world_transform_input(input);
            if (!observed) return {observed.result, {}};
            WorldTransformCommand update{};
            update.expected_world_revision = observed.value.observation.world_revision;
            update.expected = update.replacement = observed.value.observation.before;
            update.replacement.translation[0] += static_cast<float>(120.0 * observed.value.observation.elapsed_seconds);
            const auto encoded = encode_world_transform_command(update);
            if (!encoded) return {encoded.result, {}};
            delta.outbox = {encoded.value};
        } else delta.outbox = {command};
        return {foundation::Result::success(), std::move(delta)};
    };
    return definition;
}

FeatureDefinition render_producer() {
    FeatureDefinition definition{};
    definition.id = kRender; definition.version = {1, 0, 0}; definition.owner_module = kModule;
    auto& contract = definition.contract;
    contract.definition_id = definition.id; contract.version = definition.version;
    contract.domains = to_mask(FeatureDomain::kRender);
    contract.execution = ExecutionKind::kContinuous; contract.state = StatePolicy::kStateless;
    contract.scope = {ScopeId("Frame")}; contract.clock = aeg::feature::make_periodic_clock_contract(kClock, 60, 4, LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions = {kAllRenderIntentCapabilities, 1};
    contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    definition.execute = [](const InvocationToken& token, const StateSnapshot&,
        std::span<const uint8_t>, std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
        const RenderTextureReference color{RenderResourceId("WorldCommands.Color"), 1, true};
        RenderIntent intent{};
        intent.operations = {RenderDrawScene{{RenderResourceId("Runtime.Scene"), 1}, color}};
        intent.final_output = color;
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.render_intent = std::move(intent);
        return {foundation::Result::success(), std::move(delta)};
    };
    return definition;
}

struct Fixture final {
    fs::path root;
    foundation::AssetId scene{};
    foundation::AssetId graph_a{};
    foundation::AssetId graph_b{};
    std::string graph_a_uuid{};
    std::string graph_b_uuid{};
    foundation::Uuid entity_uuid{};
    scene::SceneEntityId entity{3, 4};
    module::FeatureModuleDefinition module_definition{};
    asset::GameBuildManifest manifest{};
    std::vector<std::shared_ptr<ProducerProbe>> probes{};
    std::shared_ptr<ProducerProbe> motion_probe{};

    explicit Fixture(bool with_renderer, bool motion_observation = false, bool ordinary_motion = false,
                     bool entry_loop = false, bool uppercase_graph = false) {
        root = fs::temp_directory_path() / (entry_loop ? "aeg_runtime_world_motion_entry_loop" : motion_observation ?
            (ordinary_motion ? (with_renderer ? "aeg_runtime_world_motion_ordinary_renderer" : "aeg_runtime_world_motion_ordinary_headless")
                             : "aeg_runtime_world_commands_motion_observation") :
            with_renderer ? "aeg_runtime_world_commands_renderer" : "aeg_runtime_world_commands_headless");
        std::error_code error;
        fs::remove_all(root, error);
        fs::create_directories(root, error);
        require(!error, "create isolated package directory");
        scene = foundation::AssetId::parse("00000000-0000-4000-8000-000000000101").value;
        graph_a = foundation::AssetId::parse("00000000-0000-4000-8000-000000000102").value;
        graph_b = foundation::AssetId::parse("00000000-0000-4000-8000-000000000103").value;
        if (uppercase_graph)
            graph_a = foundation::AssetId::parse("ABCDEF01-2345-4ABC-8DEF-ABCDEF000102").value;
        graph_a_uuid = uppercase_graph ? "ABCDEF01-2345-4ABC-8DEF-ABCDEF000102" : graph_a.to_string();
        graph_b_uuid = graph_b.to_string();
        for (unsigned byte = 0; byte != 8; ++byte) {
            entity_uuid.bytes[byte] = static_cast<uint8_t>(entity.high() >> (56u - byte * 8u));
            entity_uuid.bytes[byte + 8] = static_cast<uint8_t>(entity.low() >> (56u - byte * 8u));
        }

        reflection::TypeRegistry reflection;
        succeeds(engine::components::register_render_component_reflection(reflection), "register actual schemas");
        scene::SceneAsset document(scene::SceneAssetId{0x4000, 0x8000000000000101ull}, "WorldCommands", 1);
        succeeds(document.add_entity({entity, {}, "CameraAndTarget", scene::SceneEntityFlags::kNone, {}}), "author target");
        succeeds(document.add_entity({scene::SceneEntityId{5, 6}, {}, "Untargeted", scene::SceneEntityFlags::kNone, {}}),
                 "author untouched entity");
        succeeds(document.add_entity({scene::SceneEntityId{7, 8}, {}, "WithoutTransform", scene::SceneEntityFlags::kNone, {}}),
                 "author an entity without a Transform");
        const auto initial = transform(5.f);
        const auto untouched = transform(23.f);
        const engine::components::Camera camera{};
        for (const auto& value : std::vector<std::pair<scene::SceneEntityId, const engine::components::Transform*>>{
                 {entity, &initial}, {scene::SceneEntityId{5, 6}, &untouched}}) {
            succeeds(document.add_reflected_component(value.first, ecs::ComponentTypeId(engine::components::kTransformName),
                reflection, reflection::TypeId(engine::components::kTransformName), value.second), "author Game.Transform");
        }
        succeeds(document.add_reflected_component(entity, ecs::ComponentTypeId(engine::components::kCameraName),
            reflection, reflection::TypeId(engine::components::kCameraName), &camera), "author Camera");
        serialization::BinaryWriter archive;
        succeeds(scene::SceneSerializer::serialize(document, archive), "serialize actual Scene");
        asset::ScenePayload scene_payload{};
        scene_payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(archive.data().data()),
            reinterpret_cast<const std::byte*>(archive.data().data() + archive.size()));
        const auto scene_bytes = asset::encode_scene_payload(scene_payload);
        succeeds(scene_bytes, "encode Scene payload");

        module_definition.descriptor.id = module::ModuleId{"Smoke.WorldCommands.Module"};
        module_definition.descriptor.name = "Smoke.WorldCommands.Module";
        FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        for (const auto& row : std::vector<std::pair<FeatureDefinitionId, float>>{
                 {kPrimary, 7.f}, {kOtherAsset, 99.f}, {kWrongChild, 99.f}, {kSelectedChild, 7.f}}) {
            engine::detail::WorldTransformCommand command{};
            command.expected_world_revision = 1;
            command.expected = initial;
            command.replacement = transform(row.second);
            const auto encoded = engine::detail::encode_world_transform_command(reflection, command);
            succeeds(encoded, "encode actual typed World command");
            require(encoded.value.effect == kEffect && encoded.value.version == 1 && !encoded.value.payload.empty(),
                    "typed command codec changed its protocol");
            auto probe = std::make_shared<ProducerProbe>();
            const bool motion_primary = motion_observation && row.first == kPrimary;
            const auto definition = producer(row.first, encoded.value, probe, motion_primary);
            succeeds(aeg::feature::test::register_clock_test_definition(registry, definition), "register pure command producer");
            succeeds(module::add_feature_definition(module_definition, definition), "add producer to actual Module");
            if (motion_primary) motion_probe = probe;
            probes.push_back(std::move(probe));
        }
        FeatureDefinition composite{};
        composite.id = kComposite; composite.version = {1, 0, 0}; composite.owner_module = kModule;
        auto& contract = composite.contract;
        contract.definition_id = kComposite; contract.version = composite.version;
        contract.structure = FeatureStructure::kComposite; contract.clock.work_budget_ns = 0; contract.domains = to_mask(FeatureDomain::kLogic);
        contract.execution = ExecutionKind::kCall; contract.state = StatePolicy::kStateless;
        contract.scope = {ScopeId("World")}; contract.clock = aeg::feature::make_periodic_clock_contract(kClock, 60, 4, LateDataPolicy::kQueue,
            aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
        contract.permissions.version = 1; contract.children = {kWrongChild, kSelectedChild};
        succeeds(aeg::feature::test::register_clock_test_definition(registry, composite), "register Composite with two actual candidates");
        succeeds(module::add_feature_definition(module_definition, composite), "add Composite to Module");
        if (with_renderer) {
            const auto render = render_producer();
            succeeds(aeg::feature::test::register_clock_test_definition(registry, render), "register actual Scene Render producer");
            succeeds(module::add_feature_definition(module_definition, render), "add Render producer to Module");
        }

        std::vector<tooling::AssetPackageEntry> entries;
        const auto append = [&](foundation::AssetId id, foundation::AssetKind kind, std::vector<std::byte> bytes) {
            tooling::CookedAsset cooked{};
            cooked.id = id; cooked.kind = kind; cooked.schema_version = 1; cooked.payload = std::move(bytes);
            const auto envelope = tooling::CookedAssetCodec{}.encode(cooked);
            succeeds(envelope, "encode cooked asset");
            entries.push_back({id, kind, 1, envelope.value});
        };
        append(scene, foundation::AssetKind::kScene, scene_bytes.value);
        for (const auto id : {graph_a, graph_b}) {
            FeatureGraphDocument graph{};
            graph.asset_uuid = id == graph_a ? graph_a_uuid : graph_b_uuid;
            graph.schema_version = 1; graph.graph_version = 1;
            graph.contract_digest = registry.contract_digest();
            graph.nodes = {{kSharedNode, id == graph_a ? kPrimary : kOtherAsset, {1, 0, 0}, OwnerPath{{kAuthorRoot}}, {}}};
            if (id == graph_a) {
                graph.nodes.push_back({kCompositeNode, kComposite, {1, 0, 0}, OwnerPath{{kAuthorRoot}}, {}});
                if (with_renderer) graph.nodes.push_back({FeatureNodeId("Render"), kRender, {1, 0, 0}, {}, {}});
            }
            const auto encoded = asset::encode_feature_graph_asset(graph);
            succeeds(encoded, "encode startup graph");
            append(id, foundation::AssetKind::kFeatureGraph, encoded.value);
        }
        succeeds(tooling::AssetPackageWriter{}.write(root / "game.aegbin", entries), "write actual package");
        std::ofstream(root / "game.manifest") << "package = \"game.aegbin\"\n"
            "build_id = \"world-commands\"\ntarget = \"windows-x64-vulkan\"\nconfiguration = \"Debug\"\n";
        std::ifstream package(root / "game.aegbin", std::ios::binary);
        const std::string package_bytes((std::istreambuf_iterator<char>(package)), {});
        const auto package_digest = module::module_content_sha256(package_bytes);
        succeeds(package_digest, "hash package");
        module::ModuleMetadata metadata{};
        metadata.schema_version = module::kModuleMetadataSchemaVersion;
        metadata.module = module_definition.descriptor; metadata.feature_catalog = module_definition.feature_catalog;
        metadata.abi_major = AEG_MODULE_ABI_MAJOR; metadata.abi_minor = AEG_MODULE_ABI_MINOR;
        metadata.sdk_contract_major = module::kModuleSdkContractMajor;
        metadata.sdk_contract_minor = module::kModuleSdkContractMinor;
        const auto module_digest = module::module_contract_sha256(metadata);
        succeeds(module_digest, "hash real Module contract");
        manifest.project_uuid = "world-commands-project"; manifest.module_id = "Smoke.WorldCommands.Module";
        manifest.build_id = "world-commands"; manifest.module_contract_digest = module_digest.value;
        manifest.package_digest = package_digest.value; manifest.startup_scene = scene;
        manifest.startup_feature_graphs = {graph_a, graph_b};
        manifest.target = "windows-x64-vulkan"; manifest.configuration = "Debug";
        manifest.package_name = "game.aegbin"; manifest.module_manifest_name = "module/module.manifest.json";
        manifest.module_manifest_sha256 = "static-world-commands-module";
        save_manifest();
    }

    void save_manifest() {
        // Every negative case carries a valid package digest, so route validation owns rejection.
        std::ifstream package(root / "game.aegbin", std::ios::binary);
        require(bool(package), "open package before pinning the route manifest");
        const std::string bytes((std::istreambuf_iterator<char>(package)), {});
        const auto digest = module::module_content_sha256(bytes);
        succeeds(digest, "repin actual package before route startup");
        manifest.package_digest = digest.value;
        const auto encoded_manifest = asset::encode_game_build_manifest(manifest);
        succeeds(encoded_manifest, "encode binary startup manifest");
        std::ofstream output(root / "game.manifest.bin", std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded_manifest.value.data()),
                     static_cast<std::streamsize>(encoded_manifest.value.size()));
        require(bool(output), "write binary startup manifest");
    }

    void set_manifest_bindings(const std::vector<RuntimeWorldCommandBinding>& bindings) {
        manifest.world_command_routes.clear();
        for (const auto& binding : bindings) {
            asset::WorldCommandRoute route{};
            route.graph_asset_uuid = binding.graph_asset_uuid;
            route.node_id = binding.node_id;
            route.owner_path = binding.owner_path;
            route.scene_asset = binding.scene_asset;
            route.scene_entity = binding.scene_entity;
            route.effect = binding.effect;
            route.version = binding.version;
            route.observation_port = binding.observation_port;
            manifest.world_command_routes.push_back(std::move(route));
        }
        save_manifest();
    }

    RuntimeWorldCommandBinding binding(bool composite = false) const {
        RuntimeWorldCommandBinding result{};
        result.graph_asset_uuid = graph_a_uuid;
        result.node_id = composite ? kCompositeNode : kSharedNode;
        result.owner_path.nodes = {kAuthorRoot};
        if (composite) {
            result.owner_path.nodes.push_back(kCompositeNode);
            result.owner_path.nodes.push_back(FeatureNodeId(kSelectedChild.value()));
        }
        result.scene_asset = scene; result.scene_entity = entity_uuid; result.effect = kEffect; result.version = 1;
        return result;
    }
};

struct Clock final {
    double value{0};
    static double now(void* data) noexcept { return (static_cast<Clock*>(data)->value += 1.0 / 60.0); }
    static double frozen_now(void* data) noexcept { return static_cast<Clock*>(data)->value; }
};
RuntimeBootRequest request_for(const Fixture& fixture, Clock& clock) {
    RuntimeBootRequest request{};
    request.manifest_path = fixture.root / "game.manifest";
    request.headless = true; request.width = 4; request.height = 4;
    request.clock = {&Clock::now, &clock};
    return request;
}

const FeatureRuntimeOwnerSnapshot& owner(const FeatureRuntimeNodeSnapshot& snapshot, FeatureDefinitionId definition) {
    const auto found = std::find_if(snapshot.owners.begin(), snapshot.owners.end(),
        [&](const auto& candidate) { return candidate.definition_id == definition; });
    require(found != snapshot.owners.end(), "actual published owner missing");
    require(std::count_if(snapshot.owners.begin(), snapshot.owners.end(),
        [&](const auto& candidate) { return candidate.definition_id == definition; }) == 1, "ambiguous published owner");
    return *found;
}
bool committed(const FeatureRuntimeOwnerSnapshot& actual) {
    if (!actual.invocations.last_committed) return false;
    const auto& commit = *actual.invocations.last_committed;
    require(actual.owner_module == kModule && actual.processes.size() == 1 && !actual.invocations.last_failed &&
            actual.invocations.open_attempts.empty() && actual.processes.front().status == ProcessStatus::kCompleted &&
            actual.processes.front().state.commit_count == 1 && commit.attempt.token.invocation_id.is_valid() &&
            commit.attempt.token.attempt_id.is_valid() && commit.attempt.token.input_version == 0 &&
            commit.state_version == StateVersion(commit.attempt.token.state_version.value() + 1),
            "producer did not commit exactly one actual Process result");
    return true;
}

bool run_case(bool with_renderer, bool configured, bool composite, bool manifest_routes = false,
              bool uppercase_graph = false) {
    Fixture fixture(with_renderer, false, false, false, uppercase_graph);
    Clock clock;
    auto request = request_for(fixture, clock);
    // Pump the one accepted clock wave without manufacturing more due work while a worker waits.
    request.clock = {&Clock::frozen_now, &clock};
    if (configured) {
        if (manifest_routes) fixture.set_manifest_bindings({fixture.binding(composite)});
        else request.world_command_bindings = {fixture.binding(composite)};
    }
    if (manifest_routes) require(request.world_command_bindings.empty(), "delivery test supplied host bindings");
    auto device = with_renderer ? rhi::create_device({}) : foundation::ResultValue<std::unique_ptr<rhi::Device>>{};
    if (with_renderer) succeeds(device, "create real renderer host device");
    std::unique_ptr<render_graph::FrameContext> frame;
    std::unique_ptr<render_graph::RenderGraph> graph;
    renderer::Renderer scene_renderer;
    std::optional<ecs::WorldSnapshot> extracted;
    renderer::RenderViewId extracted_view{};
    uint32_t extraction_count = 0;
    RuntimeFrameHostResources host{};
    // Bootstrap must retire borrowed resources before the host objects unwind on any failed guard.
    RuntimeBootstrap bootstrap;
    if (with_renderer) {
        frame = std::make_unique<render_graph::FrameContext>(*device.value);
        graph = std::make_unique<render_graph::RenderGraph>(*device.value);
        succeeds(scene_renderer.register_builtin_extractors(), "register actual component extractors");
        succeeds(scene_renderer.register_extractor([&](const renderer::RenderExtractContext& context) {
            require(context.user_data == &bootstrap.last_frame_context(), "extraction lost actual frame carrier");
            require(context.view.world == context.world_snapshot.world() &&
                    context.view.id == renderer::make_render_view_id(context.world_snapshot.world(), 0),
                    "actual renderer View uses a foreign World key");
            extracted = context.world_snapshot; extracted_view = context.view.id; ++extraction_count;
            return foundation::Result::success();
        }), "register snapshot observation on actual Renderer");
        host.frame_context = frame.get(); host.render_graph = graph.get(); host.renderer = &scene_renderer;
        host.render_feature_preparation = [](const engine::EngineFramePreparationContext& context) {
            rhi::TextureDesc color{};
            color.extent = {4, 4, 1};
            color.format = rhi::Format::kRgba8Unorm;
            color.capabilities = rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kTransferSource;
            color.name = rhi::DebugName{"WorldCommands.Color"};
            const auto texture = context.render_graph.create_texture(color);
            if (!texture) return texture.result;
            return context.bind_feature_texture(RenderResourceId("WorldCommands.Color"), texture.value);
        };
        request.frame_host = &host;
    }
    succeeds(bootstrap.initialize(request, fixture.module_definition,
        {"world-commands", "windows-x64-vulkan", "Debug"}), "initialize packaged Runtime");
    require(bootstrap.startup_feature_graph_count() == 2 && bootstrap.startup_world_scope(), "startup assets missing");
    auto& scope = *bootstrap.startup_world_scope();
    ecs::EntityId target{}, untouched{};
    succeeds(scope.state().for_each<engine::components::Transform>([&](ecs::EntityId entity, auto& value) {
        if (is_transform(&value, 5.f)) target = entity;
        if (is_transform(&value, 23.f)) untouched = entity;
    }), "resolve the actual instantiated entities");
    require(target.is_valid() && untouched.is_valid() && target != untouched && scope.state().revision() == 1,
            "initial Scene did not create two genuine Game.Transform components at revision 1");
    const auto initial_snapshot = scope.latest_snapshot();
    succeeds(initial_snapshot, "read initial snapshot");
    require(initial_snapshot.value->revision() == 1 && initial_snapshot.value->frame_number() == 0 &&
            is_transform(initial_snapshot.value->get<engine::components::Transform>(target), 5.f), "initial snapshot invalid");

    bool ready = false;
    FeatureRuntimeNodeSnapshot primary, secondary, descendants, render_state;
    for (unsigned attempt = 0; attempt != 512; ++attempt) {
        // A real external failure stops both production paths before their World consumer/commit.
        // Feature advance still submits/pumps workers, so accepted output can wait without a stale base revision.
        succeeds(scope.command_buffer().enqueue([](ecs::World&) {
            return foundation::not_ready("World command input is temporarily unavailable");
        }), "queue real external input failure");
        const auto tick = bootstrap.tick();
        require(bootstrap.last_frame_context().delta_seconds == (attempt == 0 ? request.fixed_delta_seconds : 0.f),
                "one-shot completion pump synthesized new clock ticks");
        const bool upstream_guard = !tick && tick.code == foundation::ResultCode::kNotReady &&
            bootstrap.frame_count() == 0 && scope.command_buffer().empty() && scope.state().revision() == 1 &&
            is_transform(scope.state().get<engine::components::Transform>(target), 5.f);
        if (!upstream_guard) {
            const auto* current = scope.state().get<engine::components::Transform>(target);
            std::cerr << "upstream_failure host=" << (with_renderer ? "renderer" : "none")
                      << " configured=" << configured << " composite=" << composite << " attempt=" << attempt
                      << " tick_code=" << static_cast<unsigned>(tick.code) << " tick_message=" << tick.message
                      << " frame_count=" << bootstrap.frame_count() << " queue_empty=" << scope.command_buffer().empty()
                      << " world_revision=" << scope.state().revision()
                      << " x=" << (current ? std::to_string(current->translation.x) : "missing")
                      << " frame_active=" << (frame ? static_cast<int>(frame->is_frame_active()) : -1)
                      << " extracts=" << extraction_count << '\n';
        }
        require(upstream_guard,
                "upstream failure mutated World, published, or acknowledged pending work");
        if (with_renderer) require(!frame->is_frame_active() && extraction_count == 0, "failed frame retained/extracted live work");
        const auto a = RuntimeFeatureControl::inspect_node(bootstrap, fixture.graph_a_uuid, kSharedNode);
        const auto b = RuntimeFeatureControl::inspect_node(bootstrap, fixture.graph_b_uuid, kSharedNode);
        const auto c = RuntimeFeatureControl::inspect_node(bootstrap, fixture.graph_a_uuid, kCompositeNode);
        succeeds(a, "inspect actual primary owner"); succeeds(b, "inspect same-node other asset"); succeeds(c, "inspect Composite");
        primary = a.value; secondary = b.value; descendants = c.value;
        require(primary.owners.size() == 1 && secondary.owners.size() == 1 && descendants.owners.size() == 3 &&
                owner(primary, kPrimary).owner.instance_id != owner(secondary, kOtherAsset).owner.instance_id,
                "startup graph namespacing or Composite expansion aliased owners");
        ready = committed(owner(primary, kPrimary)) && committed(owner(secondary, kOtherAsset)) &&
                committed(owner(descendants, kWrongChild)) && committed(owner(descendants, kSelectedChild));
        if (with_renderer) {
            const auto observed = RuntimeFeatureControl::inspect_node(bootstrap, fixture.graph_a_uuid, FeatureNodeId("Render"));
            succeeds(observed, "inspect actual Render producer"); render_state = observed.value;
            const auto& render_owner = owner(render_state, kRender);
            ready = ready && render_owner.render_output.has_value() && render_owner.invocations.last_committed.has_value();
        }
        if (ready) break;
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    require(ready, "actual async producers did not finish within the bounded retry pump");
    for (const auto& probe : fixture.probes)
        require(probe->calls.load() == 1 && probe->worker_thread.load() != concurrency::current_thread_id().value,
                "producer ran repeatedly or on the World owner thread");
    const auto& selected = owner(composite ? descendants : primary, composite ? kSelectedChild : kPrimary);
    require(selected.invocations.last_committed->attempt.token.generation == primary.generation,
            "accepted command lost its actual published generation");
    const auto still_initial = scope.latest_snapshot();
    succeeds(still_initial, "snapshot after delayed command acceptance");
    require(still_initial.value == initial_snapshot.value, "failed upstream input replaced the immutable snapshot");

    succeeds(bootstrap.tick(), "retry actual Runtime frame after upstream input recovery");
    const auto snapshot = scope.latest_snapshot();
    succeeds(snapshot, "read successful frame snapshot");
    const auto* actual = scope.state().get<engine::components::Transform>(target);
    require(actual && (is_transform(actual, 5.f) || is_transform(actual, 7.f)), "unexpected binding selected another asset/child");
    const float actual_x = actual->translation.x;
    require(snapshot.value->revision() == 2 && scope.state().revision() == 2 && bootstrap.frame_count() == 1 &&
            snapshot.value->fixed_tick() == bootstrap.last_frame_context().fixed_tick &&
            is_transform(snapshot.value->get<engine::components::Transform>(target), actual_x) &&
            is_transform(scope.state().get<engine::components::Transform>(untouched), 23.f) &&
            is_transform(snapshot.value->get<engine::components::Transform>(untouched), 23.f) &&
            is_transform(initial_snapshot.value->get<engine::components::Transform>(target), 5.f),
            "actual command, snapshot, clocks or untouched component contract failed");
    if (with_renderer) {
        require(extraction_count == 1 && extracted && extracted_view.is_valid() &&
                extracted->revision() == 2 && extracted->frame_number() == frame->frame_number() &&
                snapshot.value->frame_number() == frame->frame_number() &&
                is_transform(extracted->get<engine::components::Transform>(target), actual_x),
                "Renderer did not extract the recovered actual World frame");
        const auto actual_render = RuntimeFeatureControl::inspect_node(
            bootstrap, fixture.graph_a_uuid, FeatureNodeId("Render"));
        succeeds(actual_render, "inspect the projected actual Render owner");
        const auto& actual_owner = owner(actual_render.value, kRender);
        require(actual_owner.render_output.has_value(), "projected owner lost its committed Render intent");
        std::size_t actual_scene_passes = 0;
        for (const auto& pass : render_graph::detail::RenderGraphExecution::plan(*graph).pass_plans) {
            if (!pass.live || !pass.feature_work || pass.feature_work->feature_definition_id != kRender) continue;
            const auto record = render_graph::detail::RenderGraphExecution::inspect_pass(*graph, pass.pass);
            succeeds(record, "inspect the actual native Scene pass");
            require(pass.feature_work->id.is_valid() && pass.feature_work->owner_module == kModule &&
                    pass.feature_work->instance_id == actual_owner.owner.instance_id &&
                    pass.feature_work->invocation == actual_owner.render_output->invocation &&
                    pass.feature_work->operation_index == 0 && pass.feature_work->frame_index == frame->frame_number(),
                    "actual native Scene pass lost its committed Feature provenance");
            ++actual_scene_passes;
        }
        require(actual_scene_passes > 0, "actual RenderDrawScene was not lowered into a live native pass");
    } else {
        require(request.frame_host == nullptr && snapshot.value->frame_number() == 1 && extraction_count == 0,
                "renderer-free production path did not commit its actual first frame");
    }
    foundation::Result foreign_shutdown;
    concurrency::ThreadId foreign_thread{};
    const auto owner_thread = concurrency::current_thread_id();
    concurrency::WorkerExecutor foreign({concurrency::ExecutorDomain::kCpuWorker, 1, false});
    require(foreign.domain() == concurrency::ExecutorDomain::kCpuWorker && foreign.worker_count() == 1,
            "foreign shutdown did not use the actual CPU worker executor");
    concurrency::TaskDesc shutdown_probe{};
    shutdown_probe.domain = concurrency::ExecutorDomain::kCpuWorker;
    shutdown_probe.execute = [&](const concurrency::TaskContext&) {
        foreign_thread = concurrency::current_thread_id();
        foreign_shutdown = bootstrap.shutdown();
        return foundation::Result::success();
    };
    const auto submitted = foreign.submit(std::move(shutdown_probe));
    succeeds(submitted, "submit real foreign-thread shutdown probe");
    succeeds(submitted.value.wait(2'000'000'000ull), "wait for actual foreign-thread shutdown");
    require(foreign_thread.is_valid() && foreign_thread != owner_thread && !foreign_shutdown &&
            foreign_shutdown.code == foundation::ResultCode::kInvalidState && scope.is_initialized(),
            "foreign-thread shutdown retired the live Runtime/World");
    succeeds(bootstrap.shutdown(), "owner-thread shutdown after rejected foreign attempt");
    require(is_transform(snapshot.value->get<engine::components::Transform>(target), actual_x) &&
            is_transform(initial_snapshot.value->get<engine::components::Transform>(target), 5.f),
            "owning snapshot did not survive complete Runtime teardown");
    if (with_renderer) succeeds(frame->drain(), "drain actual host before destruction");
    std::cout << "world_commands host=" << (with_renderer ? "renderer" : "none")
              << " configured=" << configured << " composite=" << composite << " committed=1 mutable=" << actual_x
              << " snapshot=" << snapshot.value->get<engine::components::Transform>(target)->translation.x
              << " revision=" << snapshot.value->revision() << " frame=" << snapshot.value->frame_number()
              << " extracts=" << extraction_count << '\n';
    if (manifest_routes)
        std::cout << "manifest_world_route graph=" << fixture.graph_a_uuid << " host_bindings=0 applied=" << actual_x << '\n';
    if (!configured) require(actual_x == 5.f, "unconfigured Runtime silently selected an outbox owner");
    return !configured || actual_x == 7.f;
}

int motion_observation_case(bool manifest_routes = false) {
    Fixture fixture(false, true);
    require(bool(fixture.motion_probe), "Motion observation fixture has no explicit Primary probe");
    double clock_seconds = 0.0;
    RuntimeBootRequest request{};
    request.manifest_path = fixture.root / "game.manifest";
    request.headless = true; request.width = 4; request.height = 4;
    request.world_command_bindings = {fixture.binding()};
    request.world_command_bindings.front().observation_port = kWorldInput;
    if (manifest_routes) {
        fixture.set_manifest_bindings(request.world_command_bindings);
        request.world_command_bindings.clear();
    }
    request.clock = {[](void* value) noexcept { return *static_cast<double*>(value); }, &clock_seconds};
    RuntimeBootstrap bootstrap;
    succeeds(bootstrap.initialize(request, fixture.module_definition,
        {"world-commands", "windows-x64-vulkan", "Debug"}), "initialize actual packaged Motion observation Runtime");
    require(bootstrap.startup_feature_graph_count() == 2 && bootstrap.startup_world_scope() && request.frame_host == nullptr,
            "Motion observation did not load the actual package/World without a renderer");
    auto& scope = *bootstrap.startup_world_scope();
    ecs::EntityId target{}, untouched{};
    succeeds(scope.state().for_each<engine::components::Transform>([&](ecs::EntityId entity, auto& value) {
        if (is_transform(&value, 5.f)) target = entity;
        if (is_transform(&value, 23.f)) untouched = entity;
    }), "resolve actual Motion Scene entities");
    const auto initial = scope.latest_snapshot();
    succeeds(initial, "retain actual initial Motion snapshot");
    require(target.is_valid() && untouched.is_valid() && target != untouched && scope.state().revision() == 1 &&
            initial.value->revision() == 1 && is_transform(initial.value->get<engine::components::Transform>(target), 5.f),
            "Motion observation initial World is not the original revision/value");
    const auto records = [&] {
        std::scoped_lock lock(fixture.motion_probe->records_mutex);
        return fixture.motion_probe->motion_records;
    };
    const auto wait_for_commit = [&](uint32_t count, uint64_t world_revision, float before,
                                     const std::shared_ptr<const ecs::WorldSnapshot>& retained, float& issuing_elapsed) {
        for (unsigned attempt = 0; attempt != 512; ++attempt) {
            succeeds(scope.command_buffer().enqueue([](ecs::World&) {
                return foundation::not_ready("Motion observation external input is temporarily unavailable");
            }), "queue actual Motion upstream failure");
            const auto tick = bootstrap.tick();
            if (attempt == 0) issuing_elapsed = bootstrap.last_frame_context().delta_seconds;
            else require(bootstrap.last_frame_context().delta_seconds == 0.f,
                         "Frozen Motion clock advanced while pumping an actual worker");
            const auto latest = scope.latest_snapshot();
            succeeds(latest, "observe Motion snapshot during upstream failure");
            require(!tick && tick.code == foundation::ResultCode::kNotReady && scope.command_buffer().empty() &&
                    scope.state().revision() == world_revision && latest.value == retained &&
                    is_transform(scope.state().get<engine::components::Transform>(target), before),
                    "Motion upstream failure mutated/committed the World or lost the external failure");
            const auto observed = RuntimeFeatureControl::inspect_node(bootstrap, fixture.graph_a.to_string(), kSharedNode);
            succeeds(observed, "inspect actual nonterminal Motion owner");
            const auto& actual = owner(observed.value, kPrimary);
            require(actual.processes.size() == 1 && actual.processes.front().status == ProcessStatus::kReady &&
                    !actual.invocations.last_failed && actual.processes.front().state.commit_count <= count,
                    "Motion Process terminated, failed, or committed an extra clock wave");
            if (actual.processes.front().state.commit_count == count) {
                const auto callback_records = records();
                require(callback_records.size() == count && fixture.motion_probe->calls.load() == count &&
                        fixture.motion_probe->worker_thread.load() != concurrency::current_thread_id().value &&
                        actual.invocations.last_committed && actual.invocations.open_attempts.empty(),
                        "Motion callback/commit count or actual worker ownership is inconsistent");
                const auto& callback = callback_records.back();
                const auto& commit = *actual.invocations.last_committed;
                require(commit.attempt.token == callback.token && callback.snapshot_version == callback.token.state_version &&
                        callback.token.generation == observed.value.generation && callback.token.invocation_id.is_valid() &&
                        callback.token.attempt_id.is_valid() && commit.attempt.process == actual.processes.front().id &&
                        callback.token.input_version == count - 1 &&
                        commit.state_version == StateVersion(callback.token.state_version.value() + 1) &&
                        commit.state_version == actual.processes.front().state.version &&
                        commit.process_revision == actual.processes.front().revision,
                        "Motion observation is not tied to the actual owner Process commit");
                return observed.value;
            }
            concurrency::yield_current_thread();
        }
        throw std::runtime_error("actual nonterminal Motion commit did not settle within its frozen-clock pump");
    };

    float first_elapsed{}, second_elapsed{};
    const auto first_owner = wait_for_commit(1, initial.value->revision(), 5.f, initial.value, first_elapsed);
    require(first_elapsed == request.fixed_delta_seconds && bootstrap.frame_count() == 0,
            "First Motion clock wave did not use the actual Runtime bootstrap delta");
    succeeds(bootstrap.tick(), "recover actual first Motion frame");
    const auto applied = scope.latest_snapshot();
    succeeds(applied, "retain actual first applied Motion snapshot");
    require(bootstrap.frame_count() == 1 && applied.value->revision() == initial.value->revision() + 1 &&
            applied.value->frame_number() == bootstrap.last_frame_context().frame_number &&
            applied.value->fixed_tick() == bootstrap.last_frame_context().fixed_tick &&
            is_transform(scope.state().get<engine::components::Transform>(target), 7.f) &&
            is_transform(applied.value->get<engine::components::Transform>(target), 7.f) &&
            is_transform(scope.state().get<engine::components::Transform>(untouched), 23.f),
            "The first official World.Transform command was not applied by actual Runtime consumption");

    succeeds(scope.command_buffer().enqueue([target](ecs::World& world) {
        const auto value = world.get_component_data(target, ecs::ComponentTypeId(engine::components::kTransformName));
        if (!value) return value.result;
        *static_cast<engine::components::Transform*>(value.value) = transform(41.f);
        return foundation::Result::success();
    }), "queue actual owner change before the second Motion wave");
    succeeds(scope.apply_commands(scope.command_buffer()), "apply actual owner change before the second Motion wave");
    const auto changed = scope.commit(bootstrap.last_frame_context().frame_number, bootstrap.last_frame_context().fixed_tick);
    succeeds(changed, "commit actual owner change before the second Motion wave");
    require(changed.value->revision() == applied.value->revision() + 1 &&
            changed.value->revision() != owner(first_owner, kPrimary).invocations.last_committed->attempt.token.input_version &&
            is_transform(changed.value->get<engine::components::Transform>(target), 41.f) &&
            is_transform(applied.value->get<engine::components::Transform>(target), 7.f),
            "Second Motion wave has no distinct actual World revision/before value");
    clock_seconds += 0.025;
    const auto second_owner = wait_for_commit(2, changed.value->revision(), 41.f, changed.value, second_elapsed);
    require(second_elapsed == 0.025f && second_elapsed != first_elapsed && bootstrap.frame_count() == 1,
            "Second Motion wave did not observe its distinct actual elapsed clock delta");
    const auto callbacks = records();
    require(callbacks.size() == 2 && callbacks[0].token.invocation_id != callbacks[1].token.invocation_id &&
            callbacks[0].token.state_version != callbacks[1].token.state_version &&
            callbacks[0].token.process_revision.is_valid() && callbacks[1].token.process_revision.is_valid() &&
            owner(first_owner, kPrimary).owner == owner(second_owner, kPrimary).owner &&
            owner(first_owner, kPrimary).processes.front().state.commit_count == 1 &&
            owner(second_owner, kPrimary).processes.front().state.commit_count == 2,
            "Motion waves did not retain distinct real invocation/owner revisions");
    const auto first_input = decode_world_transform_input(callbacks[0].input);
    const auto second_input = decode_world_transform_input(callbacks[1].input);
    succeeds(first_input, "decode actual first immutable World observation");
    succeeds(second_input, "decode actual second immutable World observation");
    const auto value_for = [](const engine::components::Transform& value) {
        return WorldTransformValue{{value.translation.x, value.translation.y, value.translation.z},
            {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w},
            {value.scale.x, value.scale.y, value.scale.z}};
    };
    require(first_input.value.observation.world_revision == initial.value->revision() &&
            second_input.value.observation.world_revision == changed.value->revision() &&
            first_input.value.observation.before == value_for(*initial.value->get<engine::components::Transform>(target)) &&
            second_input.value.observation.before == value_for(*changed.value->get<engine::components::Transform>(target)) &&
            first_input.value.observation.frame_delta_seconds == static_cast<double>(first_elapsed) &&
            second_input.value.observation.frame_delta_seconds == static_cast<double>(second_elapsed) &&
            first_input.value.observation.elapsed_seconds == 1.0 / 60.0 &&
            second_input.value.observation.elapsed_seconds == 1.0 / 60.0 &&
            first_input.value.channel_input.empty() && second_input.value.channel_input.empty(),
            "Motion projection changed the actual World observation or duplicated the entire host-frame elapsed");
    succeeds(bootstrap.tick(), "finish actual second Motion observation frame");
    const auto final_snapshot = scope.latest_snapshot();
    succeeds(final_snapshot, "retain final Motion observation snapshot");
    require(bootstrap.frame_count() == 2 && final_snapshot.value->revision() == changed.value->revision() + 1 &&
            fixture.motion_probe->calls.load() == 2 &&
            is_transform(scope.state().get<engine::components::Transform>(target), 43.f) &&
            is_transform(final_snapshot.value->get<engine::components::Transform>(target), 43.f) &&
            is_transform(final_snapshot.value->get<engine::components::Transform>(untouched), 23.f),
            "Actual observed second Motion command was not applied exactly once through the Runtime boundary");
    succeeds(bootstrap.shutdown(), "shutdown actual Motion observation Runtime");
    require(is_transform(initial.value->get<engine::components::Transform>(target), 5.f) &&
            is_transform(applied.value->get<engine::components::Transform>(target), 7.f) &&
            is_transform(changed.value->get<engine::components::Transform>(target), 41.f) &&
            is_transform(final_snapshot.value->get<engine::components::Transform>(target), 43.f),
            "Motion observation teardown invalidated owning World snapshots");
    std::cout << "motion_observation waves=2 first_input_bytes=" << callbacks[0].input.size()
              << " second_input_bytes=" << callbacks[1].input.size()
              << " first_input_version=" << callbacks[0].token.input_version
              << " second_input_version=" << callbacks[1].token.input_version
              << " first_state_version=" << callbacks[0].token.state_version.value()
              << " second_state_version=" << callbacks[1].token.state_version.value()
              << " first_process_revision=" << callbacks[0].token.process_revision.value()
              << " second_process_revision=" << callbacks[1].token.process_revision.value()
              << " first_world_revision=" << initial.value->revision()
              << " second_world_revision=" << changed.value->revision()
              << " first_before=5 second_before=41 first_elapsed=" << first_elapsed
              << " second_elapsed=" << second_elapsed << " first_applied_x=7 final_x=43\n";
    std::cout << "Actual observations decoded and both official Motion commands applied\n";
    return 0;
}

void ordinary_motion_case(bool with_renderer) {
    Fixture fixture(with_renderer, true, true);
    auto& probe = *fixture.motion_probe;
    double clock_seconds = 0.0;
    RuntimeBootRequest request{};
    request.manifest_path = fixture.root / "game.manifest";
    request.headless = true; request.width = 4; request.height = 4;
    request.world_command_bindings = {fixture.binding()};
    request.world_command_bindings.front().observation_port = kWorldInput;
    request.clock = {[](void* value) noexcept { return *static_cast<double*>(value); }, &clock_seconds};
    auto device = with_renderer ? rhi::create_device({}) : foundation::ResultValue<std::unique_ptr<rhi::Device>>{};
    if (with_renderer) succeeds(device, "create ordinary Motion renderer device");
    std::unique_ptr<render_graph::FrameContext> frame;
    std::unique_ptr<render_graph::RenderGraph> graph;
    renderer::Renderer scene_renderer;
    std::optional<ecs::WorldSnapshot> extracted;
    uint32_t extraction_count = 0;
    RuntimeFrameHostResources host{};
    RuntimeBootstrap bootstrap;
    // Release the real blocked callback before Bootstrap joins its worker on any failed guard.
    struct ReleaseMotion final {
        ProducerProbe& probe;
        ~ReleaseMotion() { probe.release_motion.store(true, std::memory_order_release); }
    } release{probe};
    if (with_renderer) {
        frame = std::make_unique<render_graph::FrameContext>(*device.value);
        graph = std::make_unique<render_graph::RenderGraph>(*device.value);
        succeeds(scene_renderer.register_builtin_extractors(), "register ordinary Motion extractors");
        succeeds(scene_renderer.register_extractor([&](const renderer::RenderExtractContext& context) {
            require(context.user_data == &bootstrap.last_frame_context() &&
                    context.view.world == context.world_snapshot.world() &&
                    context.view.id == renderer::make_render_view_id(context.world_snapshot.world(), 0),
                    "ordinary Motion extraction lost the actual frame/World/View");
            extracted = context.world_snapshot; ++extraction_count;
            return foundation::Result::success();
        }), "observe ordinary Motion Renderer snapshot");
        host.frame_context = frame.get(); host.render_graph = graph.get(); host.renderer = &scene_renderer;
        host.render_feature_preparation = [](const engine::EngineFramePreparationContext& context) {
            rhi::TextureDesc color{};
            color.extent = {4, 4, 1}; color.format = rhi::Format::kRgba8Unorm;
            color.capabilities = rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kTransferSource;
            color.name = rhi::DebugName{"WorldCommands.Color"};
            const auto texture = context.render_graph.create_texture(color);
            if (!texture) return texture.result;
            return context.bind_feature_texture(RenderResourceId("WorldCommands.Color"), texture.value);
        };
        request.frame_host = &host;
    }
    succeeds(bootstrap.initialize(request, fixture.module_definition,
        {"world-commands", "windows-x64-vulkan", "Debug"}), "initialize ordinary packaged Motion Runtime");
    require(bootstrap.startup_feature_graph_count() == 2 && bootstrap.startup_world_scope(),
            "ordinary Motion startup package missing");
    auto& scope = *bootstrap.startup_world_scope();
    ecs::EntityId target{}, untouched{};
    succeeds(scope.state().for_each<engine::components::Transform>([&](ecs::EntityId entity, auto& value) {
        if (is_transform(&value, 5.f)) target = entity;
        if (is_transform(&value, 23.f)) untouched = entity;
    }), "resolve actual ordinary Motion entities");
    const auto initial = scope.latest_snapshot();
    succeeds(initial, "retain initial ordinary Motion snapshot");
    require(target.is_valid() && untouched.is_valid() && target != untouched && initial.value->revision() == 1,
            "ordinary Motion initial World invalid");
    const auto records = [&] {
        std::scoped_lock lock(probe.records_mutex);
        return probe.motion_records;
    };
    const auto run_wave = [&](uint32_t count, const std::shared_ptr<const ecs::WorldSnapshot>& before, float x) {
        probe.release_motion.store(false, std::memory_order_release);
        float host_delta{};
        bool entered = false;
        for (unsigned attempt = 0; attempt != 512; ++attempt) {
            const auto result = bootstrap.tick();
            if (attempt == 0) host_delta = bootstrap.last_frame_context().delta_seconds;
            const auto latest = scope.latest_snapshot();
            succeeds(latest, "observe World while ordinary Motion worker is open");
            require(bootstrap.frame_count() == count - 1 && scope.state().revision() == before->revision() &&
                    latest.value == before && is_transform(scope.state().get<engine::components::Transform>(target), x) &&
                    extraction_count == (with_renderer ? count - 1 : 0) && (!frame || !frame->is_frame_active()),
                    "ordinary asynchronous observation advanced World/Frame before its actual callback retired");
            succeeds(result, "ordinary observation wait must be accepted by actual Runtime");
            const auto observed = RuntimeFeatureControl::inspect_node(bootstrap, fixture.graph_a.to_string(), kSharedNode);
            succeeds(observed, "inspect ordinary Motion open invocation");
            const auto& actual = owner(observed.value, kPrimary);
            require(!actual.invocations.last_failed && actual.processes.size() == 1 &&
                    actual.processes.front().state.commit_count == count - 1 &&
                    actual.processes.front().status == ProcessStatus::kReady &&
                    std::any_of(actual.invocations.open_attempts.begin(), actual.invocations.open_attempts.end(),
                                [](const auto& attempt) { return attempt.committable; }),
                    "ordinary wait did not preserve a real committable Process invocation");
            if (records().size() == count) { entered = true; break; }
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        require(entered && probe.calls.load() == count &&
                probe.worker_thread.load() != concurrency::current_thread_id().value,
                "ordinary Motion callback did not enter its actual worker");
        probe.release_motion.store(true, std::memory_order_release);
        bool applied = false;
        for (unsigned attempt = 0; attempt != 512; ++attempt) {
            succeeds(bootstrap.tick(), "pump ordinary Motion completion through actual Runtime");
            require(bootstrap.frame_count() <= count, "ordinary Motion committed an extra World frame");
            if (bootstrap.frame_count() == count) { applied = true; break; }
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        require(applied, "ordinary Motion completion did not reach World publication");
        const auto after = scope.latest_snapshot();
        succeeds(after, "retain ordinary Motion applied snapshot");
        const auto observed = RuntimeFeatureControl::inspect_node(bootstrap, fixture.graph_a.to_string(), kSharedNode);
        succeeds(observed, "inspect actual ordinary Motion commit");
        const auto& actual = owner(observed.value, kPrimary);
        const auto callback_records = records();
        std::cout << "ordinary_wave host=" << (with_renderer ? "renderer" : "none") << " wave=" << count
                  << " callbacks=" << callback_records.size() << " state_commits=" << actual.processes.front().state.commit_count
                  << " open=" << actual.invocations.open_attempts.size() << " before_revision=" << before->revision()
                  << " after_revision=" << after.value->revision() << " snapshot_frame=" << after.value->frame_number()
                  << " runtime_frame=" << bootstrap.last_frame_context().frame_number
                  << " backend_frame=" << (frame ? frame->frame_number() : 0) << '\n';
        require(callback_records.size() == count && actual.invocations.last_committed && !actual.invocations.last_failed &&
                actual.invocations.open_attempts.empty() && actual.processes.front().state.commit_count == count &&
                actual.processes.front().status == ProcessStatus::kReady &&
                actual.invocations.last_committed->attempt.token == callback_records.back().token &&
                callback_records.back().token.input_version == count - 1 &&
                after.value->revision() == before->revision() + 1 &&
                after.value->frame_number() == bootstrap.last_frame_context().frame_number &&
                after.value->fixed_tick() == bootstrap.last_frame_context().fixed_tick &&
                is_transform(after.value->get<engine::components::Transform>(target), x + 2.f) &&
                is_transform(scope.state().get<engine::components::Transform>(target), x + 2.f) &&
                is_transform(after.value->get<engine::components::Transform>(untouched), 23.f),
                "ordinary Motion did not apply exactly one real observed command before World publication");
        if (with_renderer)
            require(extracted && extraction_count == count && extracted->revision() == after.value->revision() &&
                    extracted->frame_number() == after.value->frame_number() &&
                    is_transform(extracted->get<engine::components::Transform>(target), x + 2.f) &&
                    !frame->is_frame_active(), "Renderer extracted an old or uncommitted Motion snapshot");
        const auto input = decode_world_transform_input(callback_records.back().input);
        succeeds(input, "decode actual ordinary Motion input");
        const auto* transform_before = before->get<engine::components::Transform>(target);
        const WorldTransformValue expected{{transform_before->translation.x, transform_before->translation.y, transform_before->translation.z},
            {transform_before->rotation.x, transform_before->rotation.y, transform_before->rotation.z, transform_before->rotation.w},
            {transform_before->scale.x, transform_before->scale.y, transform_before->scale.z}};
        require(input.value.observation.world_revision == before->revision() && input.value.observation.before == expected &&
                input.value.observation.elapsed_seconds == 1.0 / 60.0 &&
                input.value.observation.frame_delta_seconds == static_cast<double>(host_delta) &&
                input.value.channel_input.empty(), "ordinary Motion input lost actual owner observation or elapsed semantics");
        return after.value;
    };
    const auto first = run_wave(1, initial.value, 5.f);
    succeeds(scope.command_buffer().enqueue([target](ecs::World& world) {
        const auto value = world.get_component_data(target, ecs::ComponentTypeId(engine::components::kTransformName));
        if (!value) return value.result;
        *static_cast<engine::components::Transform*>(value.value) = transform(41.f);
        return foundation::Result::success();
    }), "queue actual ordinary Motion owner change");
    succeeds(scope.apply_commands(scope.command_buffer()), "apply ordinary Motion owner change");
    const auto changed = scope.commit(bootstrap.last_frame_context().frame_number, bootstrap.last_frame_context().fixed_tick);
    succeeds(changed, "commit actual ordinary Motion owner change");
    clock_seconds += 0.025;
    const auto second = run_wave(2, changed.value, 41.f);
    succeeds(bootstrap.shutdown(), "shutdown ordinary Motion Runtime");
    require(is_transform(initial.value->get<engine::components::Transform>(target), 5.f) &&
            is_transform(first->get<engine::components::Transform>(target), 7.f) &&
            is_transform(changed.value->get<engine::components::Transform>(target), 41.f) &&
            is_transform(second->get<engine::components::Transform>(target), 43.f),
            "ordinary Motion snapshots did not retain independent immutable values through shutdown");
    if (with_renderer) succeeds(frame->drain(), "drain ordinary Motion host");
    std::cout << "ordinary_motion host=" << (with_renderer ? "renderer" : "none")
              << " callbacks=2 applied_frames=2 revisions=1,2,3,4 x=5,7,41,43 extracts=" << extraction_count << '\n';
}

const RuntimeBootRequest* motion_loop_request = nullptr;

void motion_run_loop_case() {
    Fixture fixture(false, true, true, true);
    struct EntryClock final {
        Clock clock{};
        ProducerProbe* probe{};
    } clock{{}, fixture.motion_probe.get()};
    auto request = request_for(fixture, clock.clock);
    request.clock = {[](void* data) noexcept {
        auto& clock = *static_cast<EntryClock*>(data);
        // Exactly two due waves; completion pumps must not invent elapsed time.
        if (clock.probe->release_motion.load(std::memory_order_acquire)) clock.clock.value = 1.0 / 60.0;
        return clock.clock.value;
    }, &clock};
    request.max_frames = 2;
    request.trace_path = fixture.root / "motion-loop.trace";
    request.world_command_bindings = {fixture.binding()};
    request.world_command_bindings.front().observation_port = kWorldInput;
    auto& probe = *fixture.motion_probe;
    probe.release_motion.store(false, std::memory_order_release);
    concurrency::WorkerExecutor releaser({concurrency::ExecutorDomain::kCpuWorker, 1, false});
    concurrency::TaskDesc release_task{};
    release_task.domain = concurrency::ExecutorDomain::kCpuWorker;
    release_task.execute = [&](const concurrency::TaskContext&) {
        bool entered = false;
        for (unsigned attempt = 0; attempt != 1000; ++attempt) {
            if (probe.calls.load(std::memory_order_acquire) != 0) { entered = true; break; }
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        // Force ordinary temporary waits before permitting the real callback to complete.
        static_cast<void>(concurrency::sleep_for(20'000'000));
        probe.release_motion.store(true, std::memory_order_release);
        return entered ? foundation::Result::success() : foundation::not_ready("Motion entry callback never started");
    };
    const auto release = releaser.submit(std::move(release_task));
    succeeds(release, "submit actual Runtime entry-loop callback releaser");
    motion_loop_request = &request;
    char executable[] = "runtime-world-motion";
    char* arguments[] = {executable, nullptr};
    const auto result = run_game(1, arguments, fixture.module_definition,
        {"world-commands", "windows-x64-vulkan", "Debug"}, [](RuntimeBootRequest& configured) {
            configured = *motion_loop_request;
            return foundation::Result::success();
        });
    motion_loop_request = nullptr;
    succeeds(release.value.wait(2'000'000'000ull), "join real entry-loop callback releaser");
    require(result == 0, "actual Runtime entry loop rejected a legal asynchronous observation wait");
    std::ifstream trace(request.trace_path);
    std::vector<std::string> frames;
    for (std::string line; std::getline(trace, line);) frames.push_back(std::move(line));
    std::vector<MotionCallbackRecord> callbacks;
    {
        std::scoped_lock lock(probe.records_mutex);
        callbacks = probe.motion_records;
    }
    std::cout << "motion_run_loop actual_frames=" << frames.size() << " callbacks=" << callbacks.size() << '\n';
    require(frames == std::vector<std::string>{"frame 1", "frame 2"} && callbacks.size() == 2 && probe.calls.load() == 2,
            "Runtime max_frames counted temporary observation waits instead of committed World frames");
    const auto first = decode_world_transform_input(callbacks[0].input);
    const auto second = decode_world_transform_input(callbacks[1].input);
    succeeds(first, "decode actual entry-loop first observation");
    succeeds(second, "decode actual entry-loop second observation");
    require(first.value.observation.world_revision == 1 && second.value.observation.world_revision == 2 &&
            first.value.observation.before.translation[0] == 5.f && second.value.observation.before.translation[0] == 7.f &&
            callbacks[0].token.input_version == 0 && callbacks[1].token.input_version == 1,
            "entry loop reused an old World observation between its two actual Motion frames");
}

void invalid_bindings() {
    Fixture fixture(false);
    const auto valid = fixture.binding();
    std::vector<std::pair<std::string, std::vector<RuntimeWorldCommandBinding>>> cases;
    const auto add = [&](const char* name, auto change) {
        auto bad = valid; change(bad); cases.push_back({name, {std::move(bad)}});
    };
    add("empty asset", [](auto& b) { b.graph_asset_uuid.clear(); });
    add("malformed asset", [](auto& b) { b.graph_asset_uuid = "not-an-asset-uuid"; });
    add("missing asset", [](auto& b) { b.graph_asset_uuid = "00000000-0000-4000-8000-000000000199"; });
    add("empty node", [](auto& b) { b.node_id = {}; });
    add("missing node", [](auto& b) { b.node_id = FeatureNodeId("MissingNode"); });
    add("missing path", [](auto& b) { b.owner_path.nodes.clear(); });
    add("wrong path", [](auto& b) { b.owner_path.nodes = {FeatureNodeId("WrongParent")}; });
    add("wrong scene", [](auto& b) { b.scene_asset = foundation::AssetId::parse("00000000-0000-4000-8000-000000000199").value; });
    add("empty scene", [](auto& b) { b.scene_asset = {}; });
    add("empty entity", [](auto& b) { b.scene_entity = {}; });
    add("missing entity", [](auto& b) { b.scene_entity.bytes.fill(0xff); });
    add("wrong effect", [](auto& b) { b.effect = SideEffectId("Other.Command"); });
    add("empty effect", [](auto& b) { b.effect = {}; });
    add("zero version", [](auto& b) { b.version = 0; });
    add("unknown version", [](auto& b) { b.version = 2; });
    cases.push_back({"duplicate", {valid, valid}});
    auto conflict = valid;
    conflict.scene_entity = {};
    conflict.scene_entity.bytes[7] = 5;
    conflict.scene_entity.bytes[15] = 6;
    cases.push_back({"conflicting target", {valid, conflict}});
    auto composite_root = fixture.binding(true);
    composite_root.owner_path.nodes = {kAuthorRoot};
    cases.push_back({"Composite root is not a command leaf", {composite_root}});
    auto wrong_suffix = fixture.binding(true);
    wrong_suffix.owner_path.nodes.back() = FeatureNodeId("MissingLeaf");
    cases.push_back({"wrong Composite child suffix", {wrong_suffix}});
    for (const auto& [name, bindings] : cases) {
        Clock clock; auto request = request_for(fixture, clock); request.world_command_bindings = bindings;
        RuntimeBootstrap bootstrap;
        const auto initialized = bootstrap.initialize(request, fixture.module_definition,
            {"world-commands", "windows-x64-vulkan", "Debug"});
        require(!initialized && bootstrap.frame_count() == 0, "invalid startup binding accepted: " + name);
        succeeds(bootstrap.shutdown(), "cleanup failed startup: " + name);
        for (const auto& probe : fixture.probes) require(probe->calls.load() == 0, "failed binding executed a producer");
    }
}

struct GameLifecycleProbe final {
    uint32_t starts{0};
    uint32_t stops{0};
    static GameRuntimeStatus start(void*, void* user) {
        ++static_cast<GameLifecycleProbe*>(user)->starts;
        return {};
    }
    static GameRuntimeStatus stop(void*, void* user) {
        ++static_cast<GameLifecycleProbe*>(user)->stops;
        return {};
    }
    GameRuntimeCallbacks callbacks() { return {&start, nullptr, nullptr, &stop, this}; }
};

void invalid_manifest_routes() {
    Fixture fixture(false);
    const auto valid = fixture.binding();
    const auto reject_then_repair = [&](const std::string& name,
                                       const std::vector<RuntimeWorldCommandBinding>& routes,
                                       std::vector<RuntimeWorldCommandBinding> host_bindings = {},
                                       bool forge_duplicate = false) {
        fixture.set_manifest_bindings(routes);
        if (forge_duplicate) {
            // Start from a formally encoded manifest and replace only the equal-length graph spelling
            // in its second route. Package identity/digest stay valid; decoding sees duplicate sources.
            std::ifstream input(fixture.root / "game.manifest.bin", std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
            input.close();
            auto location = std::search(bytes.begin(), bytes.end(),
                                       fixture.graph_b_uuid.begin(), fixture.graph_b_uuid.end());
            require(location != bytes.end() && fixture.graph_a_uuid.size() == fixture.graph_b_uuid.size(),
                    "locate the independently encoded second route graph");
            require(std::search(location + static_cast<std::ptrdiff_t>(fixture.graph_b_uuid.size()), bytes.end(),
                                fixture.graph_b_uuid.begin(), fixture.graph_b_uuid.end()) == bytes.end(),
                    "second route graph string must have exactly one encoded occurrence");
            std::copy(fixture.graph_a_uuid.begin(), fixture.graph_a_uuid.end(), location);
            std::ofstream output(fixture.root / "game.manifest.bin", std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            require(bool(output), "write duplicate-source route manifest");
        }
        Clock clock;
        auto request = request_for(fixture, clock);
        request.world_command_bindings = std::move(host_bindings);
        GameLifecycleProbe lifecycle;
        RuntimeBootstrap bootstrap;
        const auto initialized = bootstrap.initialize(request, fixture.module_definition,
            {"world-commands", "windows-x64-vulkan", "Debug"}, lifecycle.callbacks());
        require(!initialized && bootstrap.frame_count() == 0 && lifecycle.starts == 0 && lifecycle.stops == 0,
                "invalid delivery route reached Game start: " + name);
        require(bootstrap.mode() == RuntimeMode::kStopped && bootstrap.startup_feature_graph_count() == 0 &&
                bootstrap.startup_world_scope() == nullptr,
                "failed delivery route retained published startup owners or World: " + name);
        for (const auto& probe : fixture.probes)
            require(probe->calls.load() == 0, "invalid delivery route executed a producer: " + name);

        // Repair the delivery bytes and reuse this exact object without calling shutdown first.
        fixture.set_manifest_bindings({valid});
        request.world_command_bindings.clear();
        succeeds(bootstrap.initialize(request, fixture.module_definition,
            {"world-commands", "windows-x64-vulkan", "Debug"}, lifecycle.callbacks()),
            "same Bootstrap accepts repaired route manifest: " + name);
        require(lifecycle.starts == 1 && lifecycle.stops == 0 && bootstrap.frame_count() == 0 &&
                bootstrap.startup_feature_graph_count() == 2 && bootstrap.startup_world_scope(),
                "repaired delivery route failed to publish/start once: " + name);
        for (const auto& probe : fixture.probes)
            require(probe->calls.load() == 0, "route initialization invoked a producer before the first frame: " + name);
        succeeds(bootstrap.shutdown(), "shutdown repaired delivery route: " + name);
        require(lifecycle.starts == 1 && lifecycle.stops == 1, "repaired delivery lifecycle count changed: " + name);
    };
    const auto reject = [&](const char* name, auto change) {
        auto bad = valid;
        change(bad);
        reject_then_repair(name, {std::move(bad)});
    };
    reject("graph outside startup roots", [](auto& b) {
        b.graph_asset_uuid = "00000000-0000-4000-8000-000000000199";
    });
    reject("missing authored node", [](auto& b) { b.node_id = FeatureNodeId("MissingNode"); });
    reject("missing authored parent", [](auto& b) { b.owner_path.nodes.clear(); });
    reject("wrong authored parent", [](auto& b) { b.owner_path.nodes = {FeatureNodeId("WrongParent")}; });
    reject("another Scene", [](auto& b) {
        b.scene_asset = foundation::AssetId::parse("00000000-0000-4000-8000-000000000199").value;
    });
    reject("missing Scene entity", [](auto& b) { b.scene_entity.bytes.fill(0xff); });
    reject("Scene entity without Transform", [](auto& b) {
        b.scene_entity = {};
        b.scene_entity.bytes[7] = 7;
        b.scene_entity.bytes[15] = 8;
    });
    reject("another command effect", [](auto& b) { b.effect = SideEffectId("Other.Command"); });
    reject("unsupported command version", [](auto& b) { b.version = 2; });
    reject("undeclared observation port", [](auto& b) { b.observation_port = PortId("Missing.Observation"); });
    auto wrong_suffix = fixture.binding(true);
    wrong_suffix.owner_path.nodes.back() = FeatureNodeId("MissingLeaf");
    reject_then_repair("valid first route then wrong Composite suffix", {valid, wrong_suffix});
    auto composite_root = fixture.binding(true);
    composite_root.owner_path.nodes = {kAuthorRoot};
    reject_then_repair("Composite root is not a command leaf", {composite_root});
    auto second_graph = valid;
    second_graph.graph_asset_uuid = fixture.graph_b_uuid;
    reject_then_repair("duplicate manifest source", {valid, second_graph}, {}, true);
    reject_then_repair("identical manifest and host sources", {valid}, {valid});
    reject_then_repair("distinct manifest and host sources", {valid}, {fixture.binding(true)});
    auto invalid_manifest = valid;
    invalid_manifest.node_id = FeatureNodeId("MissingNode");
    reject_then_repair("valid host cannot mask an invalid manifest", {invalid_manifest}, {valid});
}
}  // namespace

int main(int argc, char** argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "--motion-observation") return motion_observation_case();
    if (argc == 2 && std::string_view(argv[1]) == "--motion-observation-ordinary") {
        ordinary_motion_case(false);
        ordinary_motion_case(true);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--motion-run-loop") {
        motion_run_loop_case();
        return 0;
    }
    require(argc == 1, "unknown Runtime World command test mode");
    require(run_case(false, false, false), "unconfigured renderer-free baseline failed");
    const bool headless = run_case(false, true, false);
    if (!headless) {
        std::cerr << "configured committed World command was not consumed by the actual renderer-free Runtime path\n";
        return 73;
    }
    require(run_case(true, false, false), "unconfigured Renderer baseline failed");
    const bool renderer = run_case(true, true, false);
    const bool composite = run_case(false, true, true);
    if (!renderer || !composite) {
        std::cerr << "configured committed World command was not consumed by the actual Runtime path\n";
        return 73;
    }
    invalid_bindings();
    require(run_case(false, true, false, true), "manifest route did not update the actual headless World");
    require(run_case(true, true, false, true), "manifest route did not reach the actual Renderer World");
    require(run_case(false, true, true, true), "manifest route did not select the exact Composite descendant");
    require(run_case(false, true, false, true, true), "uppercase authored graph UUID was normalized or lost");
    require(motion_observation_case(true) == 0, "manifest route lost actual World observation before/revision");
    invalid_manifest_routes();
    std::cout << "Runtime explicit and delivered World command bindings passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Runtime World commands: " << error.what() << '\n';
    return 1;
}
