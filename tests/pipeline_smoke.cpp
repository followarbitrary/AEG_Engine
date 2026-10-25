/**
 * @file pipeline_smoke.cpp
 * @brief Exercises pipeline definition validation, compilation, and execution.
 */
#include "feature_clock_test_fixture.hpp"
#include <memory>
#include <pipeline.hpp>
#include <string>
#include <utility>

#include "../Engine/Pipeline/private/pipeline_compiler_execution.hpp"
#include "feature_logic_test_fixture.hpp"

namespace {
aeg::foundation::Result declare_smoke(aeg::pipeline::PipelineBuilder& builder) {
    const auto prepare_stage = builder.add_stage({
        aeg::pipeline::StageId("Prepare"),
        aeg::pipeline::EnginePhase::kPreUpdate,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        false,
    });
    const auto process_stage = builder.add_stage({
        aeg::pipeline::StageId("Process"),
        aeg::pipeline::EnginePhase::kVariableUpdate,
        aeg::pipeline::ExecutionDomain::kCpuWorker,
        aeg::pipeline::TimePolicy::kFrame,
        false,
    });
    const auto finish_stage = builder.add_stage({
        aeg::pipeline::StageId("Finish"),
        aeg::pipeline::EnginePhase::kWorldCommit,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        false,
    });
    if (!prepare_stage || !process_stage || !finish_stage) {
        return builder.status();
    }

    const auto input = builder.declare_artifact({
        aeg::pipeline::ArtifactId("PipelineInput"),
        aeg::pipeline::ArtifactTypeId("IntegerState"),
        aeg::pipeline::PipelineScope::kEngine,
        aeg::pipeline::ArtifactStorage::kValue,
        aeg::pipeline::ArtifactLifetime::kFrame,
        aeg::pipeline::OwnershipPolicy::kProducerOwned,
        1,
        aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
        true,
    });
    const auto output = builder.declare_artifact({
        aeg::pipeline::ArtifactId("PipelineOutput"),
        aeg::pipeline::ArtifactTypeId("IntegerState"),
        aeg::pipeline::PipelineScope::kEngine,
        aeg::pipeline::ArtifactStorage::kValue,
        aeg::pipeline::ArtifactLifetime::kFrame,
        aeg::pipeline::OwnershipPolicy::kProducerOwned,
        1,
        aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
        false,
    });
    if (!input || !output) {
        return builder.status();
    }

    if (const auto result =
            builder.reads(process_stage, {aeg::pipeline::ArtifactId("PipelineInput"),
                                          aeg::pipeline::ArtifactAccess::kRead, 1, aeg::pipeline::AccessFlags::kNone});
        !result) {
        return result;
    }
    if (const auto result = builder.writes(
            prepare_stage, {aeg::pipeline::ArtifactId("PipelineInput"), aeg::pipeline::ArtifactAccess::kWrite, 1,
                            aeg::pipeline::AccessFlags::kDiscardContents});
        !result) {
        return result;
    }
    if (const auto result = builder.writes(
            process_stage, {aeg::pipeline::ArtifactId("PipelineOutput"), aeg::pipeline::ArtifactAccess::kWrite, 1,
                            aeg::pipeline::AccessFlags::kDiscardContents});
        !result) {
        return result;
    }
    return builder.reads(finish_stage, {aeg::pipeline::ArtifactId("PipelineOutput"),
                                        aeg::pipeline::ArtifactAccess::kRead, 1, aeg::pipeline::AccessFlags::kNone});
}

aeg::foundation::ResultValue<aeg::pipeline::PipelineDefinition> smoke_definition() {
    aeg::pipeline::PipelineBuilder builder(
        aeg::pipeline::PipelineDescriptor{.id = aeg::pipeline::PipelineId("PipelineSmoke"),
                                          .name = std::string("PipelineSmoke"),
                                          .category = aeg::pipeline::PipelineCategory::kCore});
    if (const auto result = declare_smoke(builder); !result)
        return {result, {}};
    return builder.finish();
}

aeg::foundation::Result declare_cycle(aeg::pipeline::PipelineBuilder& builder) {
    const auto first = builder.add_stage({
        aeg::pipeline::StageId("First"),
        aeg::pipeline::EnginePhase::kFrameBegin,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        false,
    });
    const auto second = builder.add_stage({
        aeg::pipeline::StageId("Second"),
        aeg::pipeline::EnginePhase::kFrameBegin,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        false,
    });
    if (!first || !second) {
        return builder.status();
    }
    if (const auto result = builder.runs_before(first, aeg::pipeline::StageId("Second")); !result) {
        return result;
    }
    return builder.runs_before(second, aeg::pipeline::StageId("First"));
}

aeg::foundation::ResultValue<aeg::pipeline::PipelineDefinition> cyclic_definition() {
    aeg::pipeline::PipelineBuilder builder(aeg::pipeline::PipelineDescriptor{
        .id = aeg::pipeline::PipelineId("CyclicPipeline"), .name = std::string("CyclicPipeline")});
    if (const auto result = declare_cycle(builder); !result)
        return {result, {}};
    return builder.finish();
}
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    auto smoke = smoke_definition();
    if (!smoke || !registry.register_pipeline(std::move(smoke.value))) {
        return 1;
    }
    if (!registry.find_definition(aeg::pipeline::PipelineId("PipelineSmoke"))) {
        return 2;
    }

    const auto compiled = aeg::pipeline::detail::PipelineCompilerExecution::compile({
        .pipeline = aeg::pipeline::PipelineId("PipelineSmoke"),
        .scope = aeg::pipeline::PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
    });
    if (!compiled) {
        return 3;
    }
    if (compiled.value.nodes.size() != 3 || compiled.value.dependencies.size() != 2 ||
        compiled.value.batches.size() != 3) {
        return 4;
    }

    aeg::pipeline::PipelineExecutionContext context(aeg::pipeline::PipelineScope::kEngine);
    aeg::pipeline::PipelineScheduler scheduler{};
    if (const auto result = scheduler.execute(compiled.value, context); !result ||
        !std::equal(context.visited_nodes().begin(), context.visited_nodes().end(),
            compiled.value.topological_order.begin(), compiled.value.topological_order.end())) {
        return 5;
    }
    auto invalid_batch = compiled.value;
    invalid_batch.batches.front().node_indices.push_back(static_cast<uint32_t>(invalid_batch.nodes.size()));
    if (scheduler.execute(invalid_batch, context) || !context.visited_nodes().empty()) return 8;
    if (!scheduler.execute_phase(compiled.value, context,
            aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kPreUpdate)) ||
        context.visited_nodes().size() != 1 || compiled.value.nodes[context.visited_nodes().front()].stage !=
            aeg::pipeline::StageId("Prepare")) return 9;
    if (!scheduler.execute_phase(compiled.value, context,
            aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kVariableUpdate)) ||
        context.visited_nodes().size() != 1 || compiled.value.nodes[context.visited_nodes().front()].stage !=
            aeg::pipeline::StageId("Process")) return 10;
    auto wrong_scope = compiled.value; wrong_scope.scope = aeg::pipeline::PipelineScope::kFrame;
    if (scheduler.execute(wrong_scope, context) || !context.visited_nodes().empty()) return 11;

    // Structural Stage traversal does not execute business. Three real Call
    // owners exchange declared typed data and commit their independent deltas.
    namespace logic = aeg::tests::logic;
    using namespace aeg::feature;
    std::vector<logic::CallbackObservation> observations;
    FeatureRegistry features{aeg::feature::test::clock_test_host()};
    const auto prepare = logic::definition("Fixture.Prepare", {.value = 1}, observations);
    const auto process = logic::definition("Fixture.Process", {.increment = 1, .expected_input = 1}, observations);
    const auto finish = logic::definition("Fixture.Finish", {.increment = 1, .expected_input = 2}, observations);
    for (const auto* definition : {&prepare, &process, &finish})
        if (!aeg::feature::test::register_clock_test_definition(features, *definition)) return 30;
    auto document = logic::graph("fixture://pipeline-business", features);
    // Reverse authoring order proves the real DataEdges determine execution.
    document.nodes = {{FeatureNodeId("Finish"), finish.id, finish.version},
        {FeatureNodeId("Process"), process.id, process.version},
        {FeatureNodeId("Prepare"), prepare.id, prepare.version}};
    document.edges = {{FeatureNodeId("Prepare"), PortId("Out"), FeatureNodeId("Process"), PortId("In"),
            EdgeKind::kData, logic::kClock, logic::kClock, 1, 1},
        {FeatureNodeId("Process"), PortId("Out"), FeatureNodeId("Finish"), PortId("In"),
            EdgeKind::kData, logic::kClock, logic::kClock, 1, 1}};
    FeatureRuntime runtime(features);
    const auto published = logic::publish(runtime, features, document);
    if (!published || published.value.plans.size() != 1) return 31;
    const auto* first = logic::owner(published.value, FeatureNodeId("Prepare"));
    const auto* middle = logic::owner(published.value, FeatureNodeId("Process"));
    const auto* last = logic::owner(published.value, FeatureNodeId("Finish"));
    if (first == nullptr || middle == nullptr || last == nullptr || published.value.plans.front().instances !=
        std::vector<FeatureInstanceId>{first->instance_id, middle->instance_id, last->instance_id}) return 31;
    if (runtime.dispatch_call(first->instance_id, logic::encode(-999)) ||
        runtime.find_instance(first->instance_id)->state.commit_count != 0 ||
        !runtime.find_instance(middle->instance_id)->pending_messages.empty()) return 32;
    const auto rejected = runtime.inspect_invocations(first->instance_id);
    if (!rejected || !rejected.value.last_failed || rejected.value.last_failed->stage != InvocationFailureStage::kCommit ||
        rejected.value.last_committed || rejected.value.last_failed->attempt.token != observations.back().token) return 32;
    FeatureInstanceId previous{};
    int32_t expected = 1;
    for (const auto instance : published.value.plans.front().instances) {
        if (previous.is_valid()) {
            const auto& messages = runtime.find_instance(instance)->pending_messages;
            const auto token = logic::committed_token(runtime, previous);
            if (!token || messages.size() != 1 || messages.front().source.instance != previous ||
                messages.front().source.invocation != token.value || messages.front().schema != logic::kScalarSchema ||
                messages.front().schema_version != 1 || messages.front().source_port != PortId("Out") ||
                messages.front().target_port != PortId("In") || logic::decode(messages.front().payload).value != expected - 1)
                return 33;
        }
        if (!runtime.dispatch_call(instance)) return 34;
        const auto row = std::find_if(published.value.provenance.begin(), published.value.provenance.end(),
            [instance](const auto& value) { return value.instance_id == instance; });
        const auto token = logic::committed_token(runtime, instance);
        if (row == published.value.provenance.end() || !token || token.value != observations.back().token) return 34;
        const auto scalar = logic::ScalarProjection(*row).read(features, runtime, published.value, instance, token.value);
        if (!scalar || scalar.value != expected++ || runtime.find_instance(instance)->state.commit_count != 1 ||
            runtime.find_instance(instance)->owner_module != logic::kModule) return 34;
        previous = instance;
    }
    logic::ScalarProjection final_projection(*last);
    const auto last_token = logic::committed_token(runtime, last->instance_id);
    const auto first_token = logic::committed_token(runtime, first->instance_id);
    const auto final_state = runtime.find_instance(last->instance_id)->state;
    if (!last_token || !first_token ||
        final_projection.read(features, runtime, published.value, first->instance_id, first_token.value) ||
        final_projection.read(features, runtime, published.value, last->instance_id, first_token.value)) return 35;
    final_projection.applied(last_token.value);
    if (final_projection.read(features, runtime, published.value, last->instance_id, last_token.value) ||
        runtime.find_instance(last->instance_id)->state.payload != final_state.payload ||
        runtime.find_instance(last->instance_id)->state.version != final_state.version) return 36;
    if (!runtime.shutdown_execution()) return 37;

    auto cycle = cyclic_definition();
    if (!cycle || !registry.register_pipeline(std::move(cycle.value))) {
        return 6;
    }
    const auto cyclic = aeg::pipeline::detail::PipelineCompilerExecution::compile({
        .pipeline = aeg::pipeline::PipelineId("CyclicPipeline"),
        .scope = aeg::pipeline::PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
    });
    if (cyclic || cyclic.result.code != aeg::foundation::ResultCode::kInvalidArgument) {
        return 7;
    }

    return 0;
}
