/**
 * @file pipeline_hot_swap_smoke.cpp
 * @brief Exercises pipeline plan staging, safe switching, and retirement.
 */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <memory>
#include <pipeline.hpp>
#include <string>
#include <utility>

#include "../Engine/Pipeline/private/pipeline_scope_host_execution.hpp"
#include "feature_logic_test_fixture.hpp"

namespace {
aeg::foundation::Result declare_hot_swap(aeg::pipeline::PipelineBuilder& builder, uint32_t artifact_version,
                                        bool include_marker) {
    const auto artifact = builder.declare_artifact({
        aeg::pipeline::ArtifactId("HotSwapState"),
        aeg::pipeline::ArtifactTypeId("HotSwapState"),
        aeg::pipeline::PipelineScope::kEngine,
        aeg::pipeline::ArtifactStorage::kMutableState,
        aeg::pipeline::ArtifactLifetime::kEngine,
        aeg::pipeline::OwnershipPolicy::kProducerOwned,
        artifact_version,
        0,
        true,
    });
    if (!artifact) {
        return builder.status();
    }

    const auto anchor = builder.add_stage({
        aeg::pipeline::StageId("HotSwapAnchor"),
        aeg::pipeline::EnginePhase::kVariableUpdate,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        false,
    });
    if (!anchor) {
        return builder.status();
    }
    const auto independent = builder.add_stage({
        aeg::pipeline::StageId("HotSwapIndependent"),
        aeg::pipeline::EnginePhase::kVariableUpdate,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        false,
    });
    if (!independent) {
        return builder.status();
    }
    if (const auto result = builder.runs_before(anchor, aeg::pipeline::StageId("HotSwapIndependent")); !result) {
        return result;
    }
    if (include_marker) {
        const auto marker = builder.add_stage({aeg::pipeline::StageId("HotSwapMarkerStage"),
            aeg::pipeline::EnginePhase::kVariableUpdate, aeg::pipeline::ExecutionDomain::kMainThread,
            aeg::pipeline::TimePolicy::kFrame, false});
        if (!marker) return builder.status();
        if (const auto ordered = builder.runs_before(marker, aeg::pipeline::StageId("HotSwapAnchor")); !ordered)
            return ordered;
    }
    return aeg::foundation::Result::success();
}

aeg::foundation::ResultValue<aeg::pipeline::PipelineDefinition> hot_swap_definition(uint32_t artifact_version,
                                                                                 bool include_marker = false) {
    aeg::pipeline::PipelineBuilder builder(aeg::pipeline::PipelineDescriptor{
        .id = aeg::pipeline::PipelineId("HotSwapPipeline"),
        .name = std::string("HotSwapPipeline"),
        .version = {1, 0, artifact_version},
        .category = aeg::pipeline::PipelineCategory::kCore,
        .supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine)});
    if (const auto result = declare_hot_swap(builder, artifact_version, include_marker); !result)
        return {result, {}};
    return builder.finish();
}

}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    auto version_one = hot_swap_definition(1);
    if (!version_one || !registry.register_pipeline(std::move(version_one.value))) {
        return 1;
    }

    aeg::pipeline::PipelineScopeHost host(
        aeg::pipeline::PipelineScope::kEngine,
        aeg::pipeline::PipelineInstanceId("HotSwapHost"));
    if (const auto result = host.bind_pipeline(aeg::pipeline::PipelineId("HotSwapPipeline")); !result) {
        return 2;
    }
    if (const auto result = host.rebuild(registry); !result) {
        return 3;
    }

    aeg::pipeline::PipelinePlanHandle initial_plan = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (initial_plan == nullptr || initial_plan->nodes.size() != 2 || aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 4;
    }
    if (const auto result = host.activate({}); !result) {
        return 5;
    }

    const std::weak_ptr<const aeg::pipeline::PipelineDefinition> retained_initial_definition = initial_plan->definition_owner;
    if (const auto result = registry.unregister_pipeline(aeg::pipeline::PipelineId("HotSwapPipeline")); !result) {
        return 6;
    }
    auto with_marker = hot_swap_definition(1, true);
    if (!with_marker || !registry.register_pipeline(std::move(with_marker.value)) || retained_initial_definition.expired()) {
        return 7;
    }

    const auto hot_rebuild = host.rebuild(registry);
    if (!hot_rebuild || !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 8;
    }
    aeg::pipeline::PipelinePlanHandle active_before_apply = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (active_before_apply == nullptr || active_before_apply->revision != initial_plan->revision ||
        active_before_apply->nodes.size() != 2) {
        return 9;
    }

    if (const auto result = host.apply_pending_plan(); !result) {
        return 10;
    }
    aeg::pipeline::PipelinePlanHandle active_after_apply = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (active_after_apply == nullptr || active_after_apply->revision == initial_plan->revision ||
        active_after_apply->nodes.size() != 3 || aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host) || aeg::pipeline::detail::PipelineScopeHostExecution::retired_plan_count(host) != 1) {
        return 11;
    }
    if (!active_after_apply->definition_owner || !initial_plan->definition_owner ||
        active_after_apply->definition_owner == initial_plan->definition_owner) return 11;
    {
        const auto definition = registry.find_definition(aeg::pipeline::PipelineId("HotSwapPipeline"));
        if (!definition || definition.value != active_after_apply->definition_owner ||
            definition.value->stages.size() != 3 || initial_plan->definition_owner->stages.size() != 2 ||
            definition.value->stages[2].descriptor.id != aeg::pipeline::StageId("HotSwapMarkerStage")) return 11;
    }
    const std::weak_ptr<const aeg::pipeline::PipelineDefinition> retained_definition = active_after_apply->definition_owner;
    if (active_after_apply->dependencies.empty()) {
        return 17;
    }
    const auto compiled_dependency = active_after_apply->dependencies.front();
    if (const auto result = host.remove_runtime_ordering(compiled_dependency.source_node, compiled_dependency.destination_node);
        !result || !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 17;
    }
    if (const auto result = host.apply_pending_plan(); !result) {
        return 18;
    }
    auto without_compiled_order = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (without_compiled_order == nullptr || std::any_of(without_compiled_order->dependencies.begin(),
        without_compiled_order->dependencies.end(), [&](const auto& edge) {
            return edge.source_node == compiled_dependency.source_node && edge.destination_node == compiled_dependency.destination_node;
        })) {
        return 19;
    }
    if (const auto result = host.add_runtime_ordering(1, 0); !result || !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 20;
    }
    // Saving a graph draft may replay an edge already present in the pending plan.
    // That replay must be harmless rather than aborting the rest of the save.
    if (const auto result = host.add_runtime_ordering(1, 0); !result) {
        return 26;
    }
    if (const auto result = host.apply_pending_plan(); !result) {
        return 21;
    }
    auto ordered_plan = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (ordered_plan == nullptr || ordered_plan->dependencies.empty() ||
        !ordered_plan->dependencies.back().runtime_override) {
        return 22;
    }
    if (const auto result = host.remove_runtime_ordering(1, 0); !result || !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 23;
    }
    if (const auto result = host.apply_pending_plan(); !result) {
        return 24;
    }
    auto restored_plan = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (restored_plan == nullptr || std::any_of(restored_plan->dependencies.begin(), restored_plan->dependencies.end(),
        [](const auto& edge) { return edge.runtime_override; })) {
        return 25;
    }

    if (const auto result = host.collect_retired_plans(); !result || aeg::pipeline::detail::PipelineScopeHostExecution::retired_plan_count(host) < 1) {
        return 12;
    }

    aeg::pipeline::PipelineExecutionContext context(aeg::pipeline::PipelineScope::kEngine, &host.artifacts());
    aeg::pipeline::PipelineScheduler scheduler{};
    if (const auto result = host.execute(scheduler, context); !result || context.visited_nodes().size() != 3) {
        return 13;
    }
    namespace logic = aeg::tests::logic;
    using namespace aeg::feature;
    std::vector<logic::CallbackObservation> observations;
    FeatureRegistry features{aeg::feature::test::clock_test_host()};
    const auto business = logic::definition("Fixture.HotSwap.Business", {.increment = 1, .expected_input = 0}, observations);
    if (!aeg::feature::test::register_clock_test_definition(features, business)) return 40;
    auto document = logic::graph("fixture://hot-swap-business", features);
    document.nodes = {{FeatureNodeId("Business"), business.id, business.version},
        {FeatureNodeId("OtherOwner"), business.id, business.version}};
    FeatureRuntime runtime(features);
    const auto graph = logic::publish(runtime, features, document);
    if (!graph) return 41;
    const auto* owner = logic::owner(graph.value, FeatureNodeId("Business"));
    const auto* other = logic::owner(graph.value, FeatureNodeId("OtherOwner"));
    if (!owner || !other || owner->instance_id == other->instance_id) return 41;
    logic::ScalarProjection projection(*owner);
    if (runtime.dispatch_call(owner->instance_id, logic::encode(-999)) ||
        runtime.find_instance(owner->instance_id)->state.commit_count != 0) return 42;
    const auto rejected = runtime.inspect_invocations(owner->instance_id);
    if (!rejected || !rejected.value.last_failed || rejected.value.last_committed ||
        rejected.value.last_failed->stage != InvocationFailureStage::kCommit ||
        rejected.value.last_failed->attempt.token != observations.back().token) return 42;
    if (!runtime.dispatch_call(owner->instance_id, logic::encode(0))) return 43;
    const auto old_token = logic::committed_token(runtime, owner->instance_id);
    const auto result = old_token ? projection.read(features, runtime, graph.value, owner->instance_id, old_token.value)
        : aeg::foundation::ResultValue<int32_t>{old_token.result, 0};
    if (!result || result.value != 1 || observations.back().token != old_token.value ||
        runtime.find_instance(owner->instance_id)->state.commit_count != 1 ||
        runtime.find_instance(owner->instance_id)->owner_module != logic::kModule) return 43;
    if (!runtime.dispatch_call(other->instance_id, logic::encode(0))) return 44;
    const auto other_token = logic::committed_token(runtime, other->instance_id);
    if (!other_token || projection.read(features, runtime, graph.value, other->instance_id, other_token.value) ||
        projection.read(features, runtime, graph.value, owner->instance_id, other_token.value)) return 44;
    if (!runtime.dispatch_call(owner->instance_id, logic::encode(0))) return 45;
    const auto current_token = logic::committed_token(runtime, owner->instance_id);
    if (!current_token || current_token.value == old_token.value ||
        projection.read(features, runtime, graph.value, owner->instance_id, old_token.value) ||
        !projection.read(features, runtime, graph.value, owner->instance_id, current_token.value)) return 45;
    const auto state_before_replay = runtime.find_instance(owner->instance_id)->state;
    projection.applied(current_token.value);
    if (projection.read(features, runtime, graph.value, owner->instance_id, current_token.value) ||
        runtime.find_instance(owner->instance_id)->state.version != state_before_replay.version ||
        runtime.find_instance(owner->instance_id)->state.payload != state_before_replay.payload) return 46;

    if (retained_definition.expired() || retained_initial_definition.expired()) return 47;

    initial_plan.reset();
    active_before_apply.reset();
    active_after_apply.reset();
    without_compiled_order.reset();
    ordered_plan.reset();
    if (const auto result = host.collect_retired_plans(); !result || aeg::pipeline::detail::PipelineScopeHostExecution::retired_plan_count(host) != 0) {
        return 16;
    }
    if (!retained_initial_definition.expired() || retained_definition.expired()) return 47;

    if (const auto result = registry.unregister_pipeline(aeg::pipeline::PipelineId("HotSwapPipeline")); !result) {
        return 17;
    }
    if (retained_definition.expired()) return 48;
    auto version_two = hot_swap_definition(2);
    if (!version_two || !registry.register_pipeline(std::move(version_two.value))) {
        return 18;
    }
    if (const auto result = host.rebuild(registry); !result || !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 19;
    }
    if (const auto result = host.apply_pending_plan(); result ||
        result.code != aeg::foundation::ResultCode::kInvalidState) {
        return 20;
    }
    const auto blocked_descriptor = host.artifacts().find_descriptor(
        aeg::pipeline::ArtifactId("HotSwapState"));
    if (!blocked_descriptor || blocked_descriptor.value.version != 1 || !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 21;
    }

    active_after_apply.reset();
    restored_plan.reset();
    aeg::pipeline::PipelineExecutionContext hot_swap_context(aeg::pipeline::PipelineScope::kEngine, &host.artifacts());
    if (const auto result = host.execute(scheduler, hot_swap_context); !result) {
        return 22;
    }
    aeg::pipeline::PipelinePlanHandle version_two_plan = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (version_two_plan == nullptr || version_two_plan->definition_version.patch != 2 ||
        aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 23;
    }
    if (const auto result = host.collect_retired_plans(); !result || aeg::pipeline::detail::PipelineScopeHostExecution::retired_plan_count(host) != 0) {
        return 24;
    }
    if (!retained_initial_definition.expired() || !retained_definition.expired()) return 49;
    version_two_plan.reset();

    if (const auto result = host.artifacts().emplace_artifact<int>(
            aeg::pipeline::ArtifactId("HotSwapState"),
            aeg::pipeline::ArtifactTypeId("HotSwapState"),
            7);
        !result) {
        return 25;
    }
    auto writer_result = host.artifacts().write(
        aeg::pipeline::ArtifactId("HotSwapState"),
        aeg::pipeline::ArtifactTypeId("HotSwapState"),
        2);
    if (!writer_result || !writer_result.value.is_valid()) {
        return 26;
    }
    aeg::pipeline::ArtifactWriter writer = std::move(writer_result.value);
    version_two_plan.reset();

    if (const auto result = registry.unregister_pipeline(aeg::pipeline::PipelineId("HotSwapPipeline")); !result) {
        return 27;
    }
    auto version_three = hot_swap_definition(3);
    if (!version_three || !registry.register_pipeline(std::move(version_three.value))) {
        return 28;
    }
    if (const auto result = host.rebuild(registry); !result || !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 29;
    }
    if (const auto result = host.apply_pending_plan(); result ||
        result.code != aeg::foundation::ResultCode::kInvalidState) {
        return 30;
    }
    const auto writer_blocked_descriptor = host.artifacts().find_descriptor(aeg::pipeline::ArtifactId("HotSwapState"));
    if (!writer_blocked_descriptor || writer_blocked_descriptor.value.version != 2 ||
        !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) return 51;
    if (const auto current = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
        current == nullptr || current->definition_version.patch != 2) return 51;
    if (const auto result = writer.commit(); !result) {
        return 31;
    }
    if (const auto result = host.apply_pending_plan(); !result) {
        return 32;
    }
    const aeg::pipeline::PipelinePlanHandle version_three_plan = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (version_three_plan == nullptr || version_three_plan->definition_version.patch != 3 ||
        aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host)) {
        return 33;
    }
    const auto installed_three = host.artifacts().find_descriptor(aeg::pipeline::ArtifactId("HotSwapState"));
    if (!installed_three || installed_three.value.version != 3) return 52;
    if (const auto result = host.collect_retired_plans(); !result || aeg::pipeline::detail::PipelineScopeHostExecution::retired_plan_count(host) != 0) {
        return 34;
    }
    if (const auto result = host.deactivate({}); !result) {
        return 35;
    }
    if (!runtime.shutdown_execution()) return 50;
    return 0;
}
