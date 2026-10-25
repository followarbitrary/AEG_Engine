/** @brief Exercises typed World outbox application before automatic snapshot publication. */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Components/component_bundle.hpp"
#include "engine_frame_orchestrator.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include "engine_frame_inspection.hpp"
#include "feature_graph.hpp"
#include "rhi.hpp"
#include "serialization.hpp"
#include "world_snapshot_execution.hpp"
#include "../Engine/Feature/private/feature_outbox.hpp"
#include "../Engine/Runtime/private/feature_world_commands.hpp"
#include "../Engine/Runtime/private/engine_frame_world_commands.hpp"

namespace {
using namespace aeg;
using namespace aeg::feature;
using foundation::Result;
using foundation::ResultCode;
using Transform = engine::components::Transform;
using Consumer = engine::detail::FeatureWorldCommandConsumer;
using Attachment = engine::detail::EngineFrameWorldCommands;
using Access = feature::detail::FeatureOutboxAccess;
using Delivery = feature::detail::OutboxDeliveryState;
constexpr SideEffectId kEffect{"World.Transform"};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class Value> void succeeds(const Value& result, const char* message) {
    require(static_cast<bool>(result), message);
}

Transform transform(float x) {
    Transform value{};
    value.translation = {x, 2.0f, 3.0f};
    value.rotation = {0.0f, 0.0f, 0.6f, 0.8f};
    value.scale = {1.0f, 2.0f, 3.0f};
    return value;
}
bool has_value(const Transform* value, float x) {
    return value && value->translation.x == x && value->translation.y == 2.0f && value->translation.z == 3.0f &&
        value->rotation.x == 0.0f && value->rotation.y == 0.0f && value->rotation.z == 0.6f && value->rotation.w == 0.8f &&
        value->scale.x == 1.0f && value->scale.y == 2.0f && value->scale.z == 3.0f;
}

struct FrameUserData final { uint32_t marker{0xAE60u}; };
// Business input carries values and immutable frame data. The production route seals World/entity targets.
struct WorldUpdate final {
    uint64_t expected_world_revision{};
    float before{}, after{};
    uint32_t marker{};
    uint64_t fixed_tick{};
    bool invalid_output_schema{false};
};
constexpr uint32_t kWorldUpdateSchema = 1;
constexpr uint32_t kWorldUpdateBytes = 36;

std::vector<uint8_t> encode_update(const WorldUpdate& update) {
    serialization::BinaryWriter writer;
    succeeds(writer.write_u32(kWorldUpdateSchema), "encode input schema");
    succeeds(writer.write_u64(update.expected_world_revision), "encode expected World revision");
    succeeds(writer.write_u32(std::bit_cast<uint32_t>(update.before)), "encode immutable before value");
    succeeds(writer.write_u32(std::bit_cast<uint32_t>(update.after)), "encode immutable after value");
    succeeds(writer.write_u32(update.marker), "encode immutable frame marker");
    succeeds(writer.write_u64(update.fixed_tick), "encode immutable fixed tick");
    succeeds(writer.write_u32(update.invalid_output_schema ? 1u : 0u), "encode schema rejection case");
    return {writer.data().begin(), writer.data().end()};
}
foundation::ResultValue<WorldUpdate> decode_update(std::span<const uint8_t> bytes) {
    if (bytes.size() != kWorldUpdateBytes) return {foundation::invalid_argument("World input has invalid size"), {}};
    serialization::BinaryReader reader(bytes);
    const auto schema = reader.read_u32(); const auto revision = reader.read_u64();
    const auto before = reader.read_u32(), after = reader.read_u32(), marker = reader.read_u32();
    const auto tick = reader.read_u64(); const auto invalid = reader.read_u32();
    if (!schema || !revision || !before || !after || !marker || !tick || !invalid ||
        schema.value != 1 || invalid.value > 1 || reader.remaining() != 0)
        return {foundation::invalid_argument("World input schema is invalid"), {}};
    WorldUpdate value{revision.value, std::bit_cast<float>(before.value), std::bit_cast<float>(after.value),
                      marker.value, tick.value, invalid.value != 0};
    if (!std::isfinite(value.before) || !std::isfinite(value.after))
        return {foundation::invalid_argument("World input values are invalid"), {}};
    return {Result::success(), value};
}

struct InvocationObservation final {
    std::vector<InvocationToken> tokens{};
    std::vector<WorldUpdate> inputs{};
    std::vector<std::string> order{};
};
FeatureDefinition world_definition(InvocationObservation& observation, const reflection::TypeRegistry& schemas) {
    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId("Fixture.World.UpdateTransform"); definition.version = {1, 0, 0};
    definition.owner_module = ModuleIdentity("com.aeg.world.fixture");
    auto& contract = definition.contract;
    contract.definition_id = definition.id; contract.version = definition.version;
    contract.domains = to_mask(FeatureDomain::kLogic); contract.execution = ExecutionKind::kCall;
    contract.state = StatePolicy::kInstanceState; contract.scope = {ScopeId("World")};
    contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("WorldTick"), 60, 4, LateDataPolicy::kReject, aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0); contract.permissions.version = 1;
    contract.state_payload = {{1, 0, 0}, kWorldUpdateBytes};
    contract.side_effects = {{kEffect, 1, SideEffectKind::kCommand, CommitPolicy::kOutbox}};
    definition.validate_state_payload = [](std::span<const uint8_t> bytes) { return decode_update(bytes).result; };
    // The callback receives no World/WorldScope/consumer pointer and cannot mutate ECS storage.
    definition.execute = [&observation, &schemas](const InvocationToken& token, const StateSnapshot& state,
        std::span<const uint8_t> input, std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
        const auto decoded = decode_update(input); if (!decoded) return {decoded.result, {}};
        const auto& value = decoded.value;
        if (state.version != token.state_version || state.schema_version != Version{1, 0, 0} ||
            value.marker != 0xAE60u || value.fixed_tick != 13 ||
            !((value.before == 5.0f && (value.after == 6.0f || value.after == 7.0f)) ||
              (value.before == 6.0f && value.after == 7.0f)))
            return {foundation::invalid_argument("World Feature immutable input does not match the frame"), {}};
        observation.tokens.push_back(token); observation.inputs.push_back(value); observation.order.push_back("Feature");
        const auto command = engine::detail::encode_world_transform_command(
            schemas, {value.expected_world_revision, transform(value.before), transform(value.after)});
        if (!command) return {command.result, {}};
        StateDelta delta{token.state_version}; delta.state_payload = encode_update(value); delta.outbox = {command.value};
        if (value.invalid_output_schema) delta.state_payload->front() = 2;
        return {Result::success(), std::move(delta)};
    };
    return definition;
}

foundation::ResultValue<pipeline::PipelineDefinition> frame_pipeline_definition() {
    pipeline::PipelineDescriptor descriptor{};
    descriptor.id = pipeline::PipelineId("WorldFrameOrchestratorFrame"); descriptor.name = "WorldFrameOrchestratorFrame";
    descriptor.category = pipeline::PipelineCategory::kRendering;
    descriptor.supported_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame);
    pipeline::PipelineBuilder builder(descriptor);
    const auto stage = builder.add_stage({pipeline::StageId("WorldFrameOrchestratorExtract"),
        pipeline::EnginePhase::kRenderExtract, pipeline::ExecutionDomain::kMainThread, pipeline::TimePolicy::kFrame, false});
    const auto artifact = builder.declare_artifact({world::detail::kWorldSnapshotArtifactId,
        world::detail::kWorldSnapshotArtifactTypeId, pipeline::PipelineScope::kWorld,
        pipeline::ArtifactStorage::kImmutableSnapshot, pipeline::ArtifactLifetime::kWorld,
        pipeline::OwnershipPolicy::kSharedImmutable, world::detail::kWorldSnapshotArtifactVersion, 0, true});
    if (!stage || !artifact) return {builder.status(), {}};
    if (const auto result = builder.expose_artifact(world::detail::kWorldSnapshotArtifactId, pipeline::PipelineScope::kFrame);
        !result) return {result, {}};
    if (const auto result = builder.reads(stage, {world::detail::kWorldSnapshotArtifactId, pipeline::ArtifactAccess::kRead,
        world::detail::kWorldSnapshotArtifactVersion, pipeline::AccessFlags::kNone}); !result) return {result, {}};
    return builder.finish();
}

struct Harness final {
    reflection::TypeRegistry schemas{};
    ecs::WorldManager manager{};
    std::shared_ptr<ecs::World> state{};
    std::unique_ptr<world::WorldScope> scope{};
    ecs::EntityId entity{}, untouched{};
    pipeline::PipelineRegistry registry{};
    pipeline::PipelineScopeHost frame_host{pipeline::PipelineScope::kFrame, pipeline::PipelineInstanceId("WorldFrameOrchestratorFrameHost")};
    pipeline::PipelineScheduler scheduler{};
    render_graph::FrameContext frame_context;
    render_graph::RenderGraph graph;
    engine::EngineFramePipelineHost pipeline_host;
    engine::EngineFrameOrchestrator orchestrator;

    explicit Harness(rhi::Device& device)
        : frame_context(device), graph(device), pipeline_host(registry, scheduler), orchestrator(pipeline_host, frame_context, graph) {
        const auto handle = manager.create_world({"OrchestratedWorld", true}); succeeds(handle, "create actual World");
        const auto found = manager.find(handle.value); succeeds(found, "find actual World"); state = found.value;
        succeeds(engine::components::register_render_component_bundle(*state, schemas), "register reflected Runtime components");
        const auto first = state->create_entity(), second = state->create_entity(); require(first && second, "create actual entities");
        entity = first.value; untouched = second.value;
        succeeds(state->emplace<Transform>(entity, transform(1.0f)), "place actual target Transform");
        succeeds(state->emplace<Transform>(untouched, transform(31.0f)), "place unbound actual Transform");
        scope = std::make_unique<world::WorldScope>(state); succeeds(scope->initialize(), "initialize actual World Scope");
        auto definition = frame_pipeline_definition(); succeeds(definition, "define technical Frame Artifact reader");
        succeeds(registry.register_pipeline(std::move(definition.value)), "register technical Frame plan");
        succeeds(frame_host.bind_pipeline(pipeline::PipelineId("WorldFrameOrchestratorFrame")), "bind technical Frame plan");
        succeeds(frame_host.rebuild(registry), "rebuild technical Frame plan");
        succeeds(orchestrator.register_world_frame(*scope, engine::EngineFramePipelineRegistration{frame_host}), "register World Frame");
        require(orchestrator.world_count() == 1 && orchestrator.world_frame_count() == 1, "World/view registration changed identity");
    }
    void apply_external_input() {
        succeeds(scope->command_buffer().enqueue([entity = entity](ecs::World& world) {
            auto* value = world.get<Transform>(entity);
            if (!value) return foundation::not_found("queued World command entity is missing");
            value->translation.x = 5.0f; return Result::success();
        }), "enqueue external input command");
        succeeds(scope->apply_commands(scope->command_buffer()), "apply external input before Feature invocation");
        require(scope->command_buffer().empty() && has_value(state->get<Transform>(entity), 5.0f) && state->revision() == 0,
                "external input did not produce uncommitted Transform 5");
    }
};

struct PublishedFeature final {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry};
    FeatureDefinition definition{};
    FeatureGraphDocument document{};
    CompiledFeatureGraph compiled{};
    FeatureInstanceId primary{}, other{};

    PublishedFeature(InvocationObservation& observation, const reflection::TypeRegistry& schemas) {
        definition = world_definition(observation, schemas); succeeds(aeg::feature::test::register_clock_test_definition(registry, definition), "register real Logic Feature");
        document.asset_uuid = "fixture://world-transform"; document.schema_version = 1; document.graph_version = 1;
        document.contract_digest = registry.contract_digest();
        document.nodes = {{FeatureNodeId("Primary"), definition.id, definition.version, OwnerPath{{FeatureNodeId("WorldBusiness")}}},
            {FeatureNodeId("OtherOwner"), definition.id, definition.version, OwnerPath{{FeatureNodeId("OtherWorldBusiness")}}}};
        auto result = FeatureGraphCompiler{}.compile(document, registry); succeeds(result, "compile real Logic graph"); compiled = std::move(result.value);
        succeeds(runtime.publish_graph(document, compiled), "publish real Logic graph");
        require(compiled.provenance.size() == 2 && compiled.plans.size() == 1 &&
            compiled.plans.front().clock == ClockId("WorldTick") && compiled.plans.front().instances.size() == 2,
            "published graph lost actual clock plan or independent owners");
        for (const auto& source : compiled.provenance) {
            if (source.node_id == FeatureNodeId("Primary")) primary = source.instance_id;
            if (source.node_id == FeatureNodeId("OtherOwner")) other = source.instance_id;
        }
        require(primary.is_valid() && other.is_valid() && primary != other, "published graph conflated actual owners");
    }
    auto binding(FeatureInstanceId owner) { return Access::bind(runtime, owner, kEffect, 1); }
    auto capture(const feature::detail::FeatureOutboxBinding& binding) { return Access::capture(runtime, binding); }
};

struct FrameObservationState final {
    void* expected_user_data{};
    ecs::EntityId entity{};
    ecs::WorldHandle world{};
    uint64_t expected_frame{1}, expected_revision{1};
    float observed_x{};
    uint64_t observed_revision{};
    uint32_t observed_view_id{};
    bool received_user_data{};
    InvocationObservation* observation{};
};
Result read_world_snapshot(const engine::EngineFrameOrchestrator& owner, FrameObservationState& state) {
    const auto trace = engine::detail::EngineFrameInspection::capture(owner); if (!trace) return trace.result;
    uint32_t matches = 0;
    for (const auto& event : trace.value.stages) {
        if (event.stage != pipeline::StageId("WorldFrameOrchestratorExtract")) continue;
        ++matches;
        if (!event.world_frame || !event.world_snapshot || !event.bridged_snapshot || event.scope_identity == 0 ||
            event.phase != pipeline::to_phase_id(pipeline::EnginePhase::kRenderExtract))
            return foundation::invalid_state("Frame observation lost its owning World snapshot");
        const auto& key = *event.world_frame; const auto& snapshot = *event.world_snapshot;
        const auto* bridged = event.bridged_snapshot->as<ecs::WorldSnapshot>(world::detail::kWorldSnapshotArtifactTypeId);
        const auto* value = snapshot.get<Transform>(state.entity);
        if (!bridged || !has_value(value, 7.0f) || event.bridged_snapshot->owner_scope() != pipeline::PipelineScope::kWorld ||
            snapshot.world() != state.world || snapshot.world() != key.world || snapshot.world() != bridged->world() ||
            snapshot.revision() != bridged->revision() || snapshot.revision() != state.expected_revision ||
            snapshot.frame_number() != key.frame_number || snapshot.frame_number() != state.expected_frame ||
            snapshot.fixed_tick() != 13 || !state.observation)
            return foundation::invalid_state("Frame observation lost committed identity or phase ordering");
        state.received_user_data = event.user_data_identity == reinterpret_cast<uintptr_t>(state.expected_user_data);
        state.observed_x = value->translation.x; state.observed_revision = snapshot.revision(); state.observed_view_id = key.view_id;
        state.observation->order.push_back("SnapshotRead");
    }
    return matches == 1 ? Result::success() : foundation::invalid_state("Unexpected Frame visit count");
}

void unexpected_apply(void* opaque, const OutboxIntent&) noexcept { ++*static_cast<uint32_t*>(opaque); }

void test_production_application_and_frame_snapshot(rhi::Device& device) {
    Harness host(device); FrameUserData frame_user_data; InvocationObservation observation;
    host.apply_external_input(); observation.order.push_back("CommandsApplied");
    PublishedFeature feature(observation, host.schemas);
    Consumer consumer(feature.runtime, host.schemas);
    const auto primary = feature.binding(feature.primary), other = feature.binding(feature.other);
    require(primary && other && primary.value.source() && primary.value.source()->instance_id == feature.primary &&
        primary.value.source()->node_id == FeatureNodeId("Primary") &&
        primary.value.source()->definition_id == FeatureDefinitionId("Fixture.World.UpdateTransform") &&
        primary.value.source()->owner_module == ModuleIdentity("com.aeg.world.fixture") &&
        primary.value.source()->owner_path.nodes == std::vector<FeatureNodeId>{FeatureNodeId("WorldBusiness")} &&
        primary.value.source()->clock == ClockId("WorldTick") && primary.value.source()->domains == to_mask(FeatureDomain::kLogic) &&
        primary.value.definition_digest() == feature_contract_digest(feature.definition) &&
        primary.value.generation() == feature.runtime.generation(), "canonical command source lost actual provenance");
    succeeds(consumer.bind(feature.primary, *host.scope, host.entity, kEffect, 1), "bind explicit target entity");
    const auto missing_transform = host.state->create_entity(); succeeds(missing_transform, "create actual missing-component target");
    require(!consumer.bind(feature.other, *host.scope, missing_transform.value, kEffect, 1), "typed route accepted missing Transform");
    const auto foreign_handle = host.manager.create_world({"ForeignWorld", true}); succeeds(foreign_handle, "create actual foreign World");
    const auto foreign = host.manager.find(foreign_handle.value); succeeds(foreign, "find actual foreign World");
    succeeds(engine::components::register_render_component_bundle(*foreign.value, host.schemas), "register foreign World components");
    const auto foreign_entity = foreign.value->create_entity(); succeeds(foreign_entity, "create actual foreign entity");
    succeeds(foreign.value->emplace<Transform>(foreign_entity.value, transform(19.0f)), "place foreign Transform");
    world::WorldScope foreign_scope(foreign.value); succeeds(foreign_scope.initialize(), "initialize foreign World Scope");
    require(!consumer.bind(feature.primary, foreign_scope, foreign_entity.value, kEffect, 1), "duplicate owner acquired a foreign target route");
    succeeds(consumer.consume(foreign_scope), "foreign World with no route consumes zero commands");
    require(has_value(foreign.value->get<Transform>(foreign_entity.value), 19.0f) && foreign.value->revision() == 0,
            "foreign World call mutated unbound storage");
    succeeds(host.orchestrator.register_world(foreign_scope), "register second independent World");
    require(host.orchestrator.world_count() == 2 && host.orchestrator.world_frame_count() == 1, "second World changed registered view identity");
    succeeds(host.orchestrator.initialize(), "initialize actual Orchestrator with both Worlds");

    engine::EngineFrameDesc frame_desc{}; frame_desc.fixed_tick = 13; frame_desc.user_data = &frame_user_data;
    const auto* immutable_input = static_cast<const FrameUserData*>(frame_desc.user_data);
    require(immutable_input == &frame_user_data, "frame input pointer changed");
    const WorldUpdate input{0, 5.0f, 7.0f, immutable_input->marker, frame_desc.fixed_tick};
    auto malformed = input; malformed.invalid_output_schema = true;
    require(!feature.runtime.dispatch_call(feature.primary, encode_update(malformed)), "Runtime accepted malformed Feature state");
    const auto failed = feature.runtime.inspect_invocations(feature.primary); const auto empty = feature.capture(primary.value);
    require(failed && failed.value.last_failed && !failed.value.last_committed && empty && empty.value.empty() &&
        failed.value.last_failed->stage == InvocationFailureStage::kCommit &&
        failed.value.last_failed->attempt.token == observation.tokens.back() &&
        feature.runtime.find_instance(feature.primary)->state.commit_count == 0 &&
        has_value(host.state->get<Transform>(host.entity), 5.0f) && host.state->revision() == 0,
        "failed state commit partially accepted an outbox or changed ECS");
    succeeds(feature.runtime.dispatch_call(feature.other, encode_update(input)), "dispatch real foreign-owner command");
    const auto other_commands = feature.capture(other.value); require(other_commands && other_commands.value.size() == 1, "missing foreign owner command");
    uint32_t wrong_applications = 0;
    require(!Access::consume(feature.runtime, primary.value, other_commands.value[0], unexpected_apply, &wrong_applications) &&
        wrong_applications == 0 && other_commands.value[0].receipt().state == Delivery::kPending,
        "another canonical owner acquired primary command authority");
    auto first = input; first.after = 6.0f;
    auto second = input; second.before = 6.0f;
    succeeds(feature.runtime.dispatch_call(feature.primary, encode_update(first)), "dispatch first actual pending commit");
    const auto old_token = observation.tokens.back();
    succeeds(feature.runtime.dispatch_call(feature.primary, encode_update(second)), "dispatch second actual pending commit");
    const auto current_token = observation.tokens.back(); const auto commands = feature.capture(primary.value);
    const auto current_state = feature.runtime.find_instance(feature.primary);
    const auto current_history = feature.runtime.inspect_invocations(feature.primary);
    require(commands && commands.value.size() == 2 && old_token != current_token &&
        *commands.value[0].invocation() == old_token && *commands.value[1].invocation() == current_token &&
        current_history && current_history.value.last_committed && current_history.value.last_committed->attempt.token == current_token &&
        current_state->state.version == StateVersion(current_token.state_version.value() + 1) && current_state->state.commit_count == 2 &&
        current_history.value.last_committed->state_version == current_state->state.version &&
        current_state->state.schema_version == Version{1, 0, 0} && current_state->scope == ScopeId("World") &&
        current_state->owner_module == ModuleIdentity("com.aeg.world.fixture") &&
        current_state->owner_path.nodes == std::vector<FeatureNodeId>{FeatureNodeId("WorldBusiness"), FeatureNodeId("Primary")} &&
        decode_update(current_state->state.payload).value.before == 6.0f &&
        observation.inputs.back().marker == frame_user_data.marker && observation.inputs.back().fixed_tick == 13 &&
        current_token.generation == feature.runtime.generation() && has_value(host.state->get<Transform>(host.entity), 5.0f),
        "multiple real commits or immutable frame inputs lost their identity before World application");
    succeeds(Attachment::attach(host.orchestrator, consumer), "attach production consumer between frames");
    bool active_attach_rejected = false;
    frame_desc.render_feature_post_build = [&](const engine::EngineFramePostBuildContext&) {
        active_attach_rejected = !Attachment::attach(host.orchestrator, consumer);
        if (!active_attach_rejected || commands.value[0].receipt().state != Delivery::kApplied ||
            commands.value[1].receipt().state != Delivery::kApplied || !has_value(host.state->get<Transform>(host.entity), 7.0f))
            return foundation::invalid_state("Orchestrator did not apply pending commands before render build");
        observation.order.push_back("AppliedObserved"); return Result::success();
    };
    succeeds(engine::detail::EngineFrameInspection::enable(host.orchestrator), "enable owning Frame observation");
    succeeds(host.orchestrator.run_frame(frame_desc), "run actual Orchestrator typed World commit");
    FrameObservationState observed{&frame_user_data, host.entity, host.scope->handle(), 1, 1}; observed.observation = &observation;
    succeeds(read_world_snapshot(host.orchestrator, observed), "observe owning technical Frame Artifact");
    const auto published = host.scope->latest_snapshot(), foreign_published = foreign_scope.latest_snapshot();
    require(published && foreign_published && published.value->world() == host.scope->handle() &&
        published.value->revision() == 1 && published.value->frame_number() == 1 && published.value->fixed_tick() == 13 &&
        observed.received_user_data && observed.observed_x == 7.0f && observed.observed_revision == 1 && observed.observed_view_id == 0 &&
        has_value(host.state->get<Transform>(host.untouched), 31.0f) && has_value(foreign_published.value->get<Transform>(foreign_entity.value), 19.0f) &&
        observation.order == std::vector<std::string>{"CommandsApplied", "Feature", "Feature", "Feature", "Feature", "AppliedObserved", "SnapshotRead"},
        "World application, two-World isolation, Frame user data or phase order changed");
    require(!Access::consume(feature.runtime, primary.value, commands.value[1], unexpected_apply, &wrong_applications) &&
        wrong_applications == 0 && commands.value[1].receipt().state == Delivery::kApplied &&
        other_commands.value[0].receipt().state == Delivery::kPending &&
        host.scope->publish_snapshot(*foreign_published.value).code == ResultCode::kInvalidArgument,
        "replay or foreign snapshot acquired World authority");
    auto retained_frame = host.scope->create_frame_scope(1, 0); succeeds(retained_frame, "create retained actual Frame view");
    succeeds(retained_frame.value.capture(), "capture first actual Frame snapshot");
    frame_desc.render_feature_post_build = {};
    succeeds(host.orchestrator.run_frame(frame_desc), "run drained consumer without replay");
    const auto second_snapshot = host.scope->latest_snapshot();
    const auto retained = retained_frame.value.read_snapshot();
    require(second_snapshot && second_snapshot.value->revision() == 2 && second_snapshot.value->frame_number() == 2 &&
        has_value(second_snapshot.value->get<Transform>(host.entity), 7.0f) && retained && retained.value.revision() == 1 &&
        has_value(retained.value.get<Transform>(host.entity), 7.0f) &&
        host.scope->publish_snapshot(*published.value).code == ResultCode::kInvalidState,
        "later Frame replayed Applied commands, changed retained snapshot or accepted backwards publication");
    succeeds(Access::reject(feature.runtime, other.value, other_commands.value[0], ResultCode::kCancelled), "explicitly retire unrelated pending command");
    auto replacement = FeatureGraphCompiler{}.compile(feature.document, feature.registry);
    succeeds(replacement, "compile actual replacement Feature generation");
    succeeds(feature.runtime.publish_graph(feature.document, replacement.value), "publish actual replacement Feature generation");
    require(!feature.capture(primary.value), "retired canonical binding remained authorized after real graph replacement");
    succeeds(feature.runtime.shutdown_execution(), "shutdown Feature execution");
    succeeds(host.orchestrator.shutdown(), "shutdown actual Orchestrator");
    succeeds(host.scope->shutdown(), "shutdown actual World Scope"); succeeds(foreign_scope.shutdown(), "shutdown second World Scope");
    require(retained_frame.value.capture().code == ResultCode::kInvalidState && retained_frame.value.read_snapshot(),
            "shutdown revived a World source or destroyed its retained immutable Frame snapshot");
}

struct ThrowOnSnapshot final {
    ThrowOnSnapshot() = default;
    ThrowOnSnapshot(const ThrowOnSnapshot&) { throw std::runtime_error("actual World component snapshot copy failed"); }
    ThrowOnSnapshot& operator=(const ThrowOnSnapshot&) = default;
    ThrowOnSnapshot(ThrowOnSnapshot&&) noexcept = default;
    ThrowOnSnapshot& operator=(ThrowOnSnapshot&&) noexcept = default;
};

void test_applied_delivery_survives_real_snapshot_failure(rhi::Device& device) {
    Harness host(device); InvocationObservation observation; host.apply_external_input();
    succeeds(host.orchestrator.initialize(), "initialize actual snapshot-failure Orchestrator");
    const auto baseline = host.scope->commit(0, 13); succeeds(baseline, "publish real baseline snapshot");
    PublishedFeature feature(observation, host.schemas); Consumer consumer(feature.runtime, host.schemas);
    succeeds(consumer.bind(feature.primary, *host.scope, host.entity, kEffect, 1), "bind snapshot failure command");
    succeeds(Attachment::attach(host.orchestrator, consumer), "attach snapshot failure consumer");
    succeeds(feature.runtime.dispatch_call(feature.primary, encode_update({1, 5.0f, 7.0f, 0xAE60u, 13})), "commit actual command before snapshot failure");
    const auto binding = feature.binding(feature.primary); succeeds(binding, "observe committed snapshot failure command");
    const auto commands = feature.capture(binding.value); require(commands && commands.value.size() == 1, "missing accepted command");
    succeeds(host.state->register_component<ThrowOnSnapshot>(ecs::ComponentTypeId("Fixture.ThrowOnSnapshot"), "ThrowOnSnapshot"),
             "register actual throwing-copy ECS component");
    succeeds(host.state->emplace<ThrowOnSnapshot>(host.entity, ThrowOnSnapshot{}), "place actual snapshot-failing component");
    engine::EngineFrameDesc desc{}; desc.fixed_tick = 13;
    const auto failed = host.orchestrator.run_frame(desc); const auto unchanged = host.scope->latest_snapshot();
    require(!failed && failed.code == ResultCode::kUnknown && !host.orchestrator.is_frame_active() &&
        !host.frame_context.is_frame_active() && has_value(host.state->get<Transform>(host.entity), 7.0f) &&
        host.state->revision() == 1 && unchanged && unchanged.value == baseline.value &&
        has_value(unchanged.value->get<Transform>(host.entity), 5.0f) && commands.value[0].receipt().state == Delivery::kApplied &&
        *commands.value[0].invocation() == observation.tokens.front(),
        "snapshot failure rolled back application, advanced publication or lost its actual Applied receipt");
    succeeds(host.state->remove<ThrowOnSnapshot>(host.entity), "remove actual failing ECS component");
    succeeds(host.orchestrator.run_frame(desc), "retry snapshot publication after real copy failure");
    const auto recovered = host.scope->latest_snapshot(); const auto pending = feature.capture(binding.value);
    require(recovered && recovered.value->revision() == 2 && recovered.value->frame_number() == 2 &&
        recovered.value->fixed_tick() == 13 && has_value(recovered.value->get<Transform>(host.entity), 7.0f) &&
        pending && pending.value.empty() && commands.value[0].receipt().state == Delivery::kApplied && observation.tokens.size() == 1 &&
        feature.runtime.find_instance(feature.primary)->state.commit_count == 1,
        "snapshot retry replayed the already Applied command or lost committed Frame identity");
    succeeds(feature.runtime.shutdown_execution(), "shutdown snapshot test Feature runtime");
    succeeds(host.orchestrator.shutdown(), "shutdown snapshot test Orchestrator"); succeeds(host.scope->shutdown(), "shutdown snapshot test World");
}
}  // namespace

int main() try {
    auto device = aeg::rhi::create_device({}); succeeds(device, "create actual RHI device");
    test_production_application_and_frame_snapshot(*device.value);
    test_applied_delivery_survives_real_snapshot_failure(*device.value);
    std::cout << "Production World commands precede Frame snapshots and Applied deliveries survive publication failure\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "World Frame outbox: " << error.what() << '\n';
    return 73;
}
