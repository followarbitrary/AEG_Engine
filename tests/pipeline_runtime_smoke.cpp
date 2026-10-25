/** @brief Exercises Artifact owner publication and read-only scope bridging without raw business tasks. */
#include "feature_clock_test_fixture.hpp"
#include <memory>
#include <pipeline.hpp>
#include <string>
#include <utility>

#include "../Engine/Pipeline/private/pipeline_scope_host_execution.hpp"
#include "feature_logic_test_fixture.hpp"

namespace {
using namespace aeg;
namespace logic = aeg::tests::logic;
constexpr pipeline::ArtifactId kValue{"RuntimeValue"}, kSnapshot{"WorldSnapshot"};
constexpr pipeline::ArtifactTypeId kIntegerType{"RuntimeInteger"}, kSnapshotType{"WorldSnapshot"};

foundation::Result declare_runtime(pipeline::PipelineBuilder& builder) {
    const auto write = builder.add_stage({pipeline::StageId("WriteRuntime"), pipeline::EnginePhase::kPreUpdate});
    const auto read = builder.add_stage({pipeline::StageId("ReadRuntime"), pipeline::EnginePhase::kVariableUpdate});
    const auto artifact = builder.declare_artifact(
        {kValue, kIntegerType, pipeline::PipelineScope::kEngine, pipeline::ArtifactStorage::kValue,
         pipeline::ArtifactLifetime::kFrame, pipeline::OwnershipPolicy::kProducerOwned, 1,
         pipeline::to_mask(pipeline::PipelineScope::kEngine), false});
    if (!write || !read || !artifact)
        return builder.status();
    if (const auto result = builder.writes(
            write, {kValue, pipeline::ArtifactAccess::kWrite, 1, pipeline::AccessFlags::kDiscardContents});
        !result)
        return result;
    return builder.reads(read, {kValue, pipeline::ArtifactAccess::kRead, 1});
}

foundation::ResultValue<pipeline::PipelineDefinition> runtime_definition() {
    pipeline::PipelineBuilder builder(pipeline::PipelineDescriptor{.id = pipeline::PipelineId("RuntimePipeline"),
                                                                   .name = std::string("RuntimePipeline"),
                                                                   .category = pipeline::PipelineCategory::kCore});
    if (const auto result = declare_runtime(builder); !result)
        return {result, {}};
    return builder.finish();
}

foundation::Result declare_snapshot(pipeline::PipelineBuilder& builder, bool frame) {
    const auto stage =
        builder.add_stage({pipeline::StageId(frame ? "ReadWorldSnapshot" : "PublishWorldSnapshot"),
                           frame ? pipeline::EnginePhase::kRenderExtract : pipeline::EnginePhase::kWorldCommit});
    const auto artifact = builder.declare_artifact(
        {kSnapshot, kSnapshotType, pipeline::PipelineScope::kWorld, pipeline::ArtifactStorage::kImmutableSnapshot,
         pipeline::ArtifactLifetime::kWorld, pipeline::OwnershipPolicy::kSharedImmutable, 1, 0, frame});
    if (!stage || !artifact)
        return builder.status();
    if (frame) {
        if (const auto result = builder.expose_artifact(kSnapshot, pipeline::PipelineScope::kFrame); !result)
            return result;
        return builder.reads(stage, {kSnapshot, pipeline::ArtifactAccess::kRead, 1});
    }
    return builder.publish_snapshot(stage, {kSnapshot, pipeline::ArtifactAccess::kPublishSnapshot, 1});
}

foundation::ResultValue<pipeline::PipelineDefinition> snapshot_definition(bool frame) {
    pipeline::PipelineBuilder builder(pipeline::PipelineDescriptor{
        .id = pipeline::PipelineId(frame ? "FrameSnapshotPipeline" : "WorldSnapshotPipeline"),
        .name = std::string(frame ? "FrameSnapshotPipeline" : "WorldSnapshotPipeline"),
        .category = frame ? pipeline::PipelineCategory::kRendering : pipeline::PipelineCategory::kSimulation,
        .supported_scopes =
            pipeline::to_mask(frame ? pipeline::PipelineScope::kFrame : pipeline::PipelineScope::kWorld)});
    if (const auto result = declare_snapshot(builder, frame); !result)
        return {result, {}};
    return builder.finish();
}
}  // namespace

int main() {
    pipeline::PipelineRegistry registry;
    auto runtime_declaration = runtime_definition();
    auto frame_declaration = snapshot_definition(true);
    auto world_declaration = snapshot_definition(false);
    if (!runtime_declaration || !frame_declaration || !world_declaration ||
        !registry.register_pipeline(std::move(runtime_declaration.value)) ||
        !registry.register_pipeline(std::move(frame_declaration.value)) ||
        !registry.register_pipeline(std::move(world_declaration.value))) return 1;
    pipeline::PipelineScopeHost runtime_host(pipeline::PipelineScope::kEngine, pipeline::PipelineInstanceId("RuntimeHost"));
    if (!runtime_host.bind_pipeline(pipeline::PipelineId("RuntimePipeline")) || !runtime_host.rebuild(registry) ||
        !runtime_host.artifacts().emplace_artifact<int>(kValue, kIntegerType, 0) || !runtime_host.activate({})) return 2;
    const auto unchanged = [&] {
        const auto value = runtime_host.artifacts().read(kValue, kIntegerType, 1);
        return value && value.value.revision() == 1 && value.value.owner_scope() == pipeline::PipelineScope::kEngine &&
            value.value.as<int>(kIntegerType) && *value.value.as<int>(kIntegerType) == 0;
    };
    if (!unchanged() || runtime_host.artifacts().read(kValue, pipeline::ArtifactTypeId("WrongType")) ||
        runtime_host.artifacts().read(kValue, kIntegerType, 2)) return 3;

    using namespace feature;
    std::vector<logic::CallbackObservation> observations;
    FeatureRegistry features{aeg::feature::test::clock_test_host()};
    const auto source = logic::definition("Fixture.Artifact.Source", {.value = 42}, observations);
    const auto relay = logic::definition("Fixture.Artifact.Relay", {.expected_input = 42}, observations);
    const auto snapshot_definition = logic::definition("Fixture.Artifact.Snapshot", {.value = 7}, observations);
    for (const auto* definition : {&source, &relay, &snapshot_definition})
        if (!aeg::feature::test::register_clock_test_definition(features, *definition)) return 4;
    auto document = logic::graph("fixture://artifact-owners", features);
    document.nodes = {{FeatureNodeId("Relay"), relay.id, relay.version},
        {FeatureNodeId("Snapshot"), snapshot_definition.id, snapshot_definition.version},
        {FeatureNodeId("Source"), source.id, source.version}};
    document.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Relay"), PortId("In"),
        EdgeKind::kData, logic::kClock, logic::kClock, 1, 1}};
    FeatureRuntime runtime(features);
    const auto compiled = logic::publish(runtime, features, document);
    if (!compiled) return 5;
    const auto* producer = logic::owner(compiled.value, FeatureNodeId("Source"));
    const auto* consumer = logic::owner(compiled.value, FeatureNodeId("Relay"));
    const auto* snapshot_owner = logic::owner(compiled.value, FeatureNodeId("Snapshot"));
    if (!producer || !consumer || !snapshot_owner) return 5;
    logic::ScalarProjection projection(*consumer);
    const auto publish_value = [&](FeatureInstanceId candidate, const InvocationToken& token) {
        const auto scalar = projection.read(features, runtime, compiled.value, candidate, token);
        if (!scalar) return scalar.result;
        auto write = runtime_host.artifacts().write(kValue, kIntegerType, 1);
        if (!write) return write.result;
        auto* value = write.value.as<int>(kIntegerType);
        if (!value) return foundation::invalid_state("Artifact owner writer is not Int32");
        *value = scalar.value; // Actual committed delta, never a host-supplied target value.
        if (const auto result = write.value.commit(); !result) return result;
        projection.applied(token);
        return foundation::Result::success();
    };
    if (runtime.dispatch_call(consumer->instance_id, logic::encode(-999)) || !unchanged()) return 6;
    const auto schema_failure = runtime.inspect_invocations(consumer->instance_id);
    if (!schema_failure || !schema_failure.value.last_failed || schema_failure.value.last_committed ||
        schema_failure.value.last_failed->stage != InvocationFailureStage::kCommit ||
        schema_failure.value.last_failed->attempt.token != observations.back().token) return 6;
    if (!runtime.dispatch_call(producer->instance_id)) return 7;
    const auto producer_token = logic::committed_token(runtime, producer->instance_id);
    const auto& messages = runtime.find_instance(consumer->instance_id)->pending_messages;
    if (!producer_token || messages.size() != 1 || messages.front().source.instance != producer->instance_id ||
        messages.front().source.invocation != producer_token.value || messages.front().schema != logic::kScalarSchema ||
        messages.front().source_port != PortId("Out") || messages.front().target_port != PortId("In") ||
        !runtime.dispatch_call(consumer->instance_id)) return 7;
    const auto old_token = logic::committed_token(runtime, consumer->instance_id);
    if (!old_token || !runtime.dispatch_call(consumer->instance_id, logic::encode(42))) return 8;
    const auto token = logic::committed_token(runtime, consumer->instance_id);
    if (!token || token.value == old_token.value ||
        publish_value(producer->instance_id, producer_token.value) ||
        publish_value(consumer->instance_id, producer_token.value) ||
        publish_value(consumer->instance_id, old_token.value) || !unchanged()) return 8;
    if (!publish_value(consumer->instance_id, token.value) || observations.back().token != token.value) return 9;
    const auto published = [&] {
        const auto value = runtime_host.artifacts().read(kValue, kIntegerType, 1);
        return value && value.value.revision() == 2 && value.value.as<int>(kIntegerType) &&
            *value.value.as<int>(kIntegerType) == 42;
    };
    if (!published() || publish_value(consumer->instance_id, token.value) || !published()) return 10;
    pipeline::PipelineScheduler scheduler{};
    pipeline::PipelineExecutionContext runtime_context(pipeline::PipelineScope::kEngine, &runtime_host.artifacts());
    if (!runtime_host.execute(scheduler, runtime_context) || runtime_context.visited_nodes().size() != 2 ||
        !published() || !runtime_host.deactivate({})) return 11;

    pipeline::PipelineScopeHost world_host(pipeline::PipelineScope::kWorld, pipeline::PipelineInstanceId("WorldHost"));
    if (!world_host.bind_pipeline(pipeline::PipelineId("WorldSnapshotPipeline")) || !world_host.rebuild(registry) ||
        !world_host.activate({}) || !runtime.dispatch_call(snapshot_owner->instance_id)) return 12;
    const auto snapshot_token = logic::committed_token(runtime, snapshot_owner->instance_id);
    logic::ScalarProjection snapshot_projection(*snapshot_owner);
    const auto scalar = snapshot_token ? snapshot_projection.read(features, runtime, compiled.value,
        snapshot_owner->instance_id, snapshot_token.value) : foundation::ResultValue<int32_t>{snapshot_token.result, 0};
    if (!scalar || scalar.value != 7 || !world_host.artifacts().publish(kSnapshot,
        pipeline::ArtifactPayload::owned(kSnapshotType, std::make_shared<const int>(scalar.value), false))) return 13;
    snapshot_projection.applied(snapshot_token.value);
    pipeline::PipelineExecutionContext world_context(pipeline::PipelineScope::kWorld, &world_host.artifacts());
    if (!world_host.execute(scheduler, world_context) || world_context.visited_nodes().size() != 1 ||
        world_host.artifacts().write(kSnapshot, kSnapshotType, 1)) return 14;

    pipeline::ArtifactScopeBridge bridge(pipeline::PipelineScope::kWorld, pipeline::PipelineScope::kFrame, world_host.artifacts());
    if (!bridge.expose(kSnapshot) || !bridge.capture() || bridge.read(kSnapshot, pipeline::ArtifactTypeId("WrongSnapshot")) ||
        bridge.read(kSnapshot, kSnapshotType, 2)) return 15;
    const auto captured = bridge.read(kSnapshot, kSnapshotType, 1);
    const auto* captured_value = captured ? captured.value.as<int>(kSnapshotType) : nullptr;
    if (!captured || !captured_value || *captured_value != 7 || captured.value.revision() != 1 ||
        captured.value.owner_scope() != pipeline::PipelineScope::kWorld ||
        captured.value.storage() != pipeline::ArtifactStorage::kImmutableSnapshot) return 16;
    // The actual bridge retains its immutable payload after source release.
    if (!world_host.artifacts().release_lifetime(pipeline::ArtifactLifetime::kWorld)) return 17;
    pipeline::PipelineScopeHost frame_host(pipeline::PipelineScope::kFrame, pipeline::PipelineInstanceId("FrameHost"));
    if (!frame_host.bind_pipeline(pipeline::PipelineId("FrameSnapshotPipeline")) || !frame_host.rebuild(registry)) return 18;
    pipeline::detail::PipelineScopeHostExecution::set_scope_bridge(frame_host, &bridge);
    if (!frame_host.activate({})) return 19;
    pipeline::PipelineExecutionContext frame_context(pipeline::PipelineScope::kFrame, &frame_host.artifacts(), &bridge);
    if (!frame_host.execute(scheduler, frame_context) || frame_context.visited_nodes().size() != 1 ||
        pipeline::detail::PipelineScopeHostExecution::scope_bridge(frame_host) != &bridge ||
        frame_context.scope_bridge() != &bridge) return 20;
    const auto frame_plan = pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(frame_host);
    if (!frame_plan || frame_plan->nodes[frame_context.visited_nodes().front()].artifact_bindings.size() != 1) return 20;
    const auto& read_binding = frame_plan->nodes[frame_context.visited_nodes().front()].artifact_bindings.front();
    if (read_binding.artifact != kSnapshot || read_binding.access != pipeline::ArtifactAccess::kRead ||
        read_binding.required_version != 1) return 20;
    const auto retained = bridge.read(kSnapshot, kSnapshotType, 1);
    if (!retained || retained.value.revision() != captured.value.revision() ||
        retained.value.owner_scope() != pipeline::PipelineScope::kWorld || retained.value.as<int>(kSnapshotType) == nullptr ||
        *retained.value.as<int>(kSnapshotType) != 7 || frame_host.artifacts().write(kSnapshot, kSnapshotType, 1)) return 21;
    if (!frame_host.deactivate({}) || !world_host.deactivate({}) || !runtime.shutdown_execution()) return 22;
    return 0;
}
