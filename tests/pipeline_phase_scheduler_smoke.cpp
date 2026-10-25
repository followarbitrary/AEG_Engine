/**
 * @file pipeline_phase_scheduler_smoke.cpp
 * @brief Exercises custom global Phase ordering and stage-only execution traces.
 */
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <pipeline.hpp>
#include <string>
#include <utility>
#include <vector>

#include "../Engine/Pipeline/private/phase_registry_execution.hpp"
#include "../Engine/Pipeline/private/pipeline_compiler_execution.hpp"

namespace {
aeg::foundation::Result declare_phases(aeg::pipeline::PipelineBuilder& builder) {
    const auto io_stage = builder.add_stage({
        aeg::pipeline::StageId("IoStage"),
        aeg::pipeline::EnginePhase::kPreUpdate,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        true,
        aeg::pipeline::PhaseId("IoPhase"),
    });
    const auto render_stage = builder.add_stage({
        aeg::pipeline::StageId("RenderThreadStage"),
        aeg::pipeline::EnginePhase::kPreUpdate,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        true,
        aeg::pipeline::PhaseId("RenderThreadPhase"),
    });
    const auto gpu_stage = builder.add_stage({
        aeg::pipeline::StageId("GpuStage"),
        aeg::pipeline::EnginePhase::kPreUpdate,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        true,
        aeg::pipeline::PhaseId("GpuPhase"),
    });
    if (!io_stage || !render_stage || !gpu_stage) {
        return builder.status();
    }

    return aeg::foundation::Result::success();
}

aeg::foundation::ResultValue<aeg::pipeline::PipelineDefinition> phase_definition() {
    aeg::pipeline::PipelineBuilder builder(
        aeg::pipeline::PipelineDescriptor{.id = aeg::pipeline::PipelineId("PhaseSchedulerSmoke"),
                                          .name = std::string("PhaseSchedulerSmoke"),
                                          .category = aeg::pipeline::PipelineCategory::kCore});
    if (const auto result = declare_phases(builder); !result)
        return {result, {}};
    return builder.finish();
}

aeg::foundation::Result declare_invalid_phase(aeg::pipeline::PipelineBuilder& builder, bool invalid_compatibility) {
    const auto stage = builder.add_stage({
        .id = aeg::pipeline::StageId("InvalidPhaseStage"),
        .phase = invalid_compatibility ? static_cast<aeg::pipeline::EnginePhase>(255)
                                       : aeg::pipeline::EnginePhase::kPreUpdate,
        .phase_id = invalid_compatibility ? aeg::pipeline::PhaseId{} : aeg::pipeline::PhaseId("MissingPhase"),
    });
    return stage ? aeg::foundation::Result::success() : builder.status();
}

aeg::foundation::ResultValue<aeg::pipeline::PipelineDefinition> invalid_phase_definition(bool invalid_compatibility) {
    aeg::pipeline::PipelineBuilder builder(aeg::pipeline::PipelineDescriptor{
        .id = aeg::pipeline::PipelineId("InvalidPhaseSchema"), .name = std::string("InvalidPhaseSchema")});
    if (const auto result = declare_invalid_phase(builder, invalid_compatibility); !result)
        return {result, {}};
    return builder.finish();
}

bool visits_stage(const aeg::pipeline::PipelinePlan& plan, const aeg::pipeline::PipelineExecutionContext& context,
                  aeg::pipeline::StageId stage, aeg::pipeline::PhaseId phase) {
    const auto visited = context.visited_nodes();
    return visited.size() == 1 && visited.front() < plan.nodes.size() && plan.nodes[visited.front()].stage == stage &&
           plan.nodes[visited.front()].phase_id == phase;
}

bool invalid_phase_schema_is_rejected() {
    using namespace aeg::pipeline;
    using aeg::foundation::ResultCode;
    PipelineRegistry registry;
    const auto invalid_compatibility = invalid_phase_definition(true);
    if (invalid_compatibility || invalid_compatibility.result.code != ResultCode::kInvalidArgument ||
        invalid_compatibility.result.message.find("phase") == std::string::npos ||
        registry.find_definition(PipelineId("InvalidPhaseSchema")))
        return false;
    auto malformed_value = invalid_phase_definition(false);
    if (!malformed_value)
        return false;
    malformed_value.value.stages.front().descriptor.phase = static_cast<EnginePhase>(255);
    malformed_value.value.stages.front().descriptor.phase_id = {};
    const auto rejected_value = registry.register_pipeline(std::move(malformed_value.value));
    if (rejected_value || rejected_value.code != ResultCode::kInvalidArgument ||
        rejected_value.message.find("phase") == std::string::npos ||
        registry.find_definition(PipelineId("InvalidPhaseSchema")))
        return false;
    auto missing_phase = invalid_phase_definition(false);
    if (!missing_phase || !registry.register_pipeline(std::move(missing_phase.value)))
        return false;
    const auto invalid_custom = detail::PipelineCompilerExecution::compile({
        .pipeline = PipelineId("InvalidPhaseSchema"),
        .scope = PipelineScope::kEngine,
        .registry = &registry,
        .instance = PipelineInstanceId("InvalidPhaseInstance"),
    });
    return !invalid_custom && invalid_custom.result.code == ResultCode::kNotFound &&
           invalid_custom.result.message.find("phase") != std::string::npos;
}

bool invalid_phase_declarations_preserve_registry(aeg::pipeline::PhaseRegistry& registry) {
    using namespace aeg::pipeline;
    using aeg::foundation::ResultCode;
    const auto before = detail::PhaseRegistryExecution::snapshot(registry);
    if (!before)
        return false;
    const PhaseDescriptor invalid[] = {
        {{}, "InvalidId", to_phase_id(EnginePhase::kPreUpdate), PhaseOrderRelation::kAfter},
        {PhaseId("SelfPhase"), "SelfPhase", PhaseId("SelfPhase"), PhaseOrderRelation::kAfter},
        {PhaseId("InvalidRelation"), "InvalidRelation", to_phase_id(EnginePhase::kPreUpdate),
         static_cast<PhaseOrderRelation>(255)},
    };
    for (const auto& descriptor : invalid) {
        const auto result = registry.register_phase(descriptor);
        const auto after = detail::PhaseRegistryExecution::snapshot(registry);
        if (result || result.code != ResultCode::kInvalidArgument || !after ||
            after.value.revision != before.value.revision || after.value.ordered_phases != before.value.ordered_phases)
            return false;
    }
    const auto missing_anchor = registry.register_phase({PhaseId("MissingAnchorPhase"), "MissingAnchorPhase",
                                                         PhaseId("MissingAnchor"), PhaseOrderRelation::kAfter});
    const auto after = detail::PhaseRegistryExecution::snapshot(registry);
    return !missing_anchor && missing_anchor.code == ResultCode::kNotFound && after &&
           after.value.revision == before.value.revision && after.value.ordered_phases == before.value.ordered_phases;
}
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    if (const auto result = registry.phase_registry().register_phase({
            aeg::pipeline::PhaseId("IoPhase"),
            "IoPhase",
            aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kPreUpdate),
            aeg::pipeline::PhaseOrderRelation::kAfter,
        });
        !result) {
        return 1;
    }
    if (const auto result = registry.phase_registry().register_phase({
            aeg::pipeline::PhaseId("RenderThreadPhase"),
            "RenderThreadPhase",
            aeg::pipeline::PhaseId("IoPhase"),
            aeg::pipeline::PhaseOrderRelation::kAfter,
        });
        !result) {
        return 2;
    }
    if (const auto result = registry.phase_registry().register_phase({
            aeg::pipeline::PhaseId("GpuPhase"),
            "GpuPhase",
            aeg::pipeline::PhaseId("RenderThreadPhase"),
            aeg::pipeline::PhaseOrderRelation::kAfter,
        });
        !result) {
        return 3;
    }

    const auto phase_snapshot = aeg::pipeline::detail::PhaseRegistryExecution::snapshot(registry.phase_registry());
    if (!phase_snapshot) {
        return 4;
    }
    const auto& ordered_phases = phase_snapshot.value.ordered_phases;
    const auto io_iterator = std::find(ordered_phases.begin(), ordered_phases.end(), aeg::pipeline::PhaseId("IoPhase"));
    const auto render_iterator =
        std::find(ordered_phases.begin(), ordered_phases.end(), aeg::pipeline::PhaseId("RenderThreadPhase"));
    const auto gpu_iterator =
        std::find(ordered_phases.begin(), ordered_phases.end(), aeg::pipeline::PhaseId("GpuPhase"));
    const auto fixed_update_iterator = std::find(ordered_phases.begin(), ordered_phases.end(),
                                                 aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kFixedUpdate));
    if (io_iterator == ordered_phases.end() || render_iterator == ordered_phases.end() ||
        gpu_iterator == ordered_phases.end() || fixed_update_iterator == ordered_phases.end() ||
        !(io_iterator < render_iterator && render_iterator < gpu_iterator && gpu_iterator < fixed_update_iterator)) {
        return 5;
    }

    auto declaration = phase_definition();
    if (!declaration || !registry.register_pipeline(std::move(declaration.value))) {
        return 6;
    }

    const auto compiled = aeg::pipeline::detail::PipelineCompilerExecution::compile({
        .pipeline = aeg::pipeline::PipelineId("PhaseSchedulerSmoke"),
        .scope = aeg::pipeline::PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
        .instance = aeg::pipeline::PipelineInstanceId("PhaseSchedulerSmokeInstance"),
    });
    if (!compiled || compiled.value.phase_revision != phase_snapshot.value.revision) {
        return 7;
    }

    aeg::pipeline::PipelineScheduler scheduler;
    if (compiled.value.nodes.size() != 3) {
        return 8;
    }

    aeg::pipeline::PipelineExecutionContext context(aeg::pipeline::PipelineScope::kEngine);
    if (const auto result = scheduler.execute_phase(compiled.value, context, aeg::pipeline::PhaseId("IoPhase"));
        !result || !visits_stage(compiled.value, context, aeg::pipeline::StageId("IoStage"),
                                 aeg::pipeline::PhaseId("IoPhase"))) {
        return 9;
    }
    if (const auto result =
            scheduler.execute_phase(compiled.value, context, aeg::pipeline::PhaseId("RenderThreadPhase"));
        !result || !visits_stage(compiled.value, context, aeg::pipeline::StageId("RenderThreadStage"),
                                 aeg::pipeline::PhaseId("RenderThreadPhase"))) {
        return 10;
    }
    if (const auto result = scheduler.execute_phase(compiled.value, context, aeg::pipeline::PhaseId("GpuPhase"));
        !result || !visits_stage(compiled.value, context, aeg::pipeline::StageId("GpuStage"),
                                 aeg::pipeline::PhaseId("GpuPhase"))) {
        return 11;
    }

    if (!scheduler.execute(compiled.value, context)) return 12;
    const auto all_visited = context.visited_nodes();
    const std::array expected{aeg::pipeline::StageId("IoStage"), aeg::pipeline::StageId("RenderThreadStage"),
                              aeg::pipeline::StageId("GpuStage")};
    if (all_visited.size() != expected.size()) return 13;
    for (std::size_t index = 0; index < expected.size(); ++index)
        if (all_visited[index] >= compiled.value.nodes.size() ||
            compiled.value.nodes[all_visited[index]].stage != expected[index]) return 14;
    if (!scheduler.execute_phase(compiled.value, context,
                                 aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kInput)) ||
        !context.visited_nodes().empty()) return 15;
    if (!scheduler.execute_phase(compiled.value, context, aeg::pipeline::PhaseId("IoPhase"))) return 16;
    if (scheduler.execute_phase(compiled.value, context, {}).code != aeg::foundation::ResultCode::kInvalidArgument ||
        !context.visited_nodes().empty()) return 16;
    auto malformed = compiled.value;
    malformed.batches.back().node_indices.push_back(std::numeric_limits<uint32_t>::max());
    if (scheduler.execute(malformed, context) || !context.visited_nodes().empty()) return 17;
    if (!invalid_phase_schema_is_rejected() || !invalid_phase_declarations_preserve_registry(registry.phase_registry()))
        return 18;

    if (const auto result = registry.phase_registry().unregister_phase(aeg::pipeline::PhaseId("IoPhase"));
        result || result.code != aeg::foundation::ResultCode::kInvalidState ||
        aeg::pipeline::detail::PhaseRegistryExecution::revision(registry.phase_registry()) != phase_snapshot.value.revision) {
        return 19;
    }
    if (const auto result = registry.phase_registry().unregister_phase(aeg::pipeline::PhaseId("GpuPhase")); !result) {
        return 20;
    }
    if (const auto result = registry.phase_registry().unregister_phase(aeg::pipeline::PhaseId("RenderThreadPhase"));
        !result) {
        return 21;
    }
    if (const auto result = registry.phase_registry().unregister_phase(aeg::pipeline::PhaseId("IoPhase")); !result) {
        return 22;
    }
    const auto retired = aeg::pipeline::detail::PhaseRegistryExecution::snapshot(registry.phase_registry());
    if (!retired || retired.value.revision <= phase_snapshot.value.revision ||
        registry.phase_registry().contains(aeg::pipeline::PhaseId("IoPhase")) ||
        registry.phase_registry().contains(aeg::pipeline::PhaseId("RenderThreadPhase")) ||
        registry.phase_registry().contains(aeg::pipeline::PhaseId("GpuPhase"))) return 23;
    return 0;
}
