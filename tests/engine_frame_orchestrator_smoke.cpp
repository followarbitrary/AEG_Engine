/**
 * @file engine_frame_orchestrator_smoke.cpp
 * @brief Exercises custom Phase intervals and automatic RenderGraph orchestration.
 */
#include <engine_frame_orchestrator.hpp>
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include "engine_frame_inspection.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <pipeline.hpp>
#include <rhi.hpp>
#include <runtime_session.hpp>
#include <utility>
#include <vector>

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::pipeline::EnginePhase;
using aeg::pipeline::ExecutionDomain;
using aeg::pipeline::PhaseId;
using aeg::pipeline::PipelineScope;
using aeg::pipeline::TimePolicy;

inline constexpr PhaseId kBeforeStartupPhase{"BeforeStartupSmoke"};
inline constexpr PhaseId kPreFramePhase{"PreFrameSmoke"};
inline constexpr PhaseId kFramePhase{"FrameSmoke"};
inline constexpr PhaseId kPostFramePhase{"PostFrameSmoke"};
inline constexpr PhaseId kAfterShutdownPhase{"AfterShutdownSmoke"};

template <typename T>
concept HasLegacyRenderBuffers = requires(T value) { value.render_feature_buffers; };
template <typename T>
concept HasLegacyRenderState = requires(T value) { value.render_feature_state; };
static_assert(!HasLegacyRenderBuffers<aeg::engine::EngineFrameDesc>);
static_assert(!HasLegacyRenderState<aeg::engine::EngineFrameDesc>);

struct OrchestratorState final {
    std::vector<PhaseId> executed_phases{};
    void* expected_frame_user_data{nullptr};
    uint32_t render_pass_execution_count{0};
    uint32_t post_build_pass_execution_count{0};
    uint32_t legacy_callback_count{0};
    bool post_build_hook_invoked{false};
    bool marker_received_user_data{false};
};

[[nodiscard]] Result read_phase_trace(const aeg::engine::EngineFrameOrchestrator& owner, OrchestratorState& state) {
    const auto trace = aeg::engine::detail::EngineFrameInspection::capture(owner);
    if (!trace) return trace.result;
    state.executed_phases.clear();
    for (const auto& observation : trace.value.stages) {
        const std::array stages{aeg::pipeline::StageId{"BeforeStartupStage"}, aeg::pipeline::StageId{"PreFrameStage"},
            aeg::pipeline::StageId{"FrameStage"}, aeg::pipeline::StageId{"PostFrameStage"},
            aeg::pipeline::StageId{"PresentStage"}, aeg::pipeline::StageId{"AfterShutdownStage"}};
        if (std::find(stages.begin(), stages.end(), observation.stage) == stages.end()) continue;
        if (observation.phase == kPostFramePhase && observation.user_data_identity !=
            reinterpret_cast<uintptr_t>(state.expected_frame_user_data))
            return aeg::foundation::invalid_state("post-frame inspection lost actual frame user data");
        state.executed_phases.push_back(observation.phase);
    }
    return Result::success();
}
[[nodiscard]] aeg::foundation::ResultValue<aeg::pipeline::PipelineDefinition> orchestrator_pipeline_definition() {
    aeg::pipeline::PipelineDescriptor descriptor{};
    descriptor.id = aeg::pipeline::PipelineId("EngineFrameOrchestratorSmoke");
    descriptor.name = "EngineFrameOrchestratorSmoke";
    descriptor.category = aeg::pipeline::PipelineCategory::kRendering;
    descriptor.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kFrame);
    aeg::pipeline::PipelineBuilder builder(descriptor);
    const auto add_phase_stage = [&](aeg::pipeline::StageId stage_id,
                                    EnginePhase compatibility_phase, PhaseId phase,
                                    TimePolicy time_policy) -> Result {
        const auto stage = builder.add_stage({
            stage_id,
            compatibility_phase,
            ExecutionDomain::kMainThread,
            time_policy,
            false,
            phase,
        });
        return stage ? Result::success() : builder.status();
    };

    if (const Result result =
            add_phase_stage(aeg::pipeline::StageId("BeforeStartupStage"),
                           EnginePhase::kStartup, kBeforeStartupPhase, TimePolicy::kStartup);
        !result) {
        return {result, {}};
    }
    if (const Result result =
            add_phase_stage(aeg::pipeline::StageId("PreFrameStage"),
                           EnginePhase::kFrameBegin, kPreFramePhase, TimePolicy::kFrame);
        !result) {
        return {result, {}};
    }
    if (const Result result =
            add_phase_stage(aeg::pipeline::StageId("FrameStage"),
                           EnginePhase::kVariableUpdate, kFramePhase, TimePolicy::kFrame);
        !result) {
        return {result, {}};
    }
    if (const Result result =
            add_phase_stage(aeg::pipeline::StageId("PostFrameStage"),
                           EnginePhase::kFrameEnd, kPostFramePhase, TimePolicy::kFrame);
        !result) {
        return {result, {}};
    }
    if (const Result result = add_phase_stage(aeg::pipeline::StageId("PresentStage"), EnginePhase::kPresent,
                                             aeg::pipeline::to_phase_id(EnginePhase::kPresent), TimePolicy::kFrame);
        !result) {
        return {result, {}};
    }
    if (const Result result =
            add_phase_stage(aeg::pipeline::StageId("AfterShutdownStage"),
                           EnginePhase::kShutdown, kAfterShutdownPhase, TimePolicy::kShutdown);
        !result) {
        return {result, {}};
    }

    const auto render_anchor = builder.add_stage({
        aeg::pipeline::StageId("OrchestratorRenderAnchor"),
        EnginePhase::kRenderBuild,
        ExecutionDomain::kMainThread,
        TimePolicy::kFrame,
        false,
    });
    if (!render_anchor) {
        return {builder.status(), {}};
    }
    const auto marker_stage = builder.add_stage({
        aeg::pipeline::StageId("OrchestratorMarkerStage"),
        EnginePhase::kRenderBuild,
        ExecutionDomain::kMainThread,
        TimePolicy::kFrame,
        false,
    });
    if (!marker_stage) return {builder.status(), {}};
    return builder.finish();
}
}  // namespace

int main() {
    OrchestratorState state;
    int frame_user_data = 42;
    state.expected_frame_user_data = &frame_user_data;

    auto device_result = aeg::rhi::create_device({});
    if (!device_result) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);

    aeg::pipeline::PipelineRegistry registry;
    const auto register_phase = [&](PhaseId id, PhaseId anchor, aeg::pipeline::PhaseOrderRelation relation) {
        return registry.phase_registry().register_phase({id, "SmokePhase", anchor, relation});
    };
    if (!register_phase(kBeforeStartupPhase, aeg::pipeline::to_phase_id(EnginePhase::kStartup),
                        aeg::pipeline::PhaseOrderRelation::kBefore) ||
        !register_phase(kPreFramePhase, aeg::pipeline::to_phase_id(EnginePhase::kStartup),
                        aeg::pipeline::PhaseOrderRelation::kAfter) ||
        !register_phase(kFramePhase, aeg::pipeline::to_phase_id(EnginePhase::kVariableUpdate),
                        aeg::pipeline::PhaseOrderRelation::kAfter) ||
        !register_phase(kPostFramePhase, aeg::pipeline::to_phase_id(EnginePhase::kFrameEnd),
                        aeg::pipeline::PhaseOrderRelation::kAfter) ||
        !register_phase(kAfterShutdownPhase, aeg::pipeline::to_phase_id(EnginePhase::kShutdown),
                        aeg::pipeline::PhaseOrderRelation::kAfter)) {
        return 2;
    }
    auto definition = orchestrator_pipeline_definition();
    if (!definition || !registry.register_pipeline(std::move(definition.value))) {
        return 3;
    }
    aeg::pipeline::PipelineScopeHost host(PipelineScope::kFrame,
                                          aeg::pipeline::PipelineInstanceId("EngineFrameOrchestratorSmokeInstance"));
    if (const Result result = host.bind_pipeline(aeg::pipeline::PipelineId("EngineFrameOrchestratorSmoke")); !result) {
        return 6;
    }

    aeg::pipeline::PipelineScheduler scheduler;
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFramePipelineHost pipeline_host(registry, scheduler);
    aeg::engine::EngineFrameOrchestrator orchestrator(pipeline_host, frame_context, render_graph);
    if (const Result result = orchestrator.register_pipeline(
            aeg::engine::EngineFramePipelineRegistration{host}); !result) {
        return 7;
    }
    if (!aeg::engine::detail::EngineFrameInspection::enable(orchestrator)) return 8;
    aeg::engine::RuntimeSession runtime(orchestrator);
    if (const Result result = runtime.initialize(); !result) {
        return 8;
    }
    if (!read_phase_trace(orchestrator, state)) return 9;
    if (runtime.mode() != aeg::engine::RuntimeMode::kStopped ||
        state.executed_phases != std::vector<PhaseId>{kBeforeStartupPhase}) {
        return 9;
    }

    if (const Result result = runtime.play(); !result) {
        return 9;
    }
    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.user_data = &frame_user_data;
    frame_desc.render_feature_preparation = [&state, &orchestrator, carrier = frame_desc.user_data](
        const aeg::engine::EngineFramePreparationContext& context) {
        const auto trace = aeg::engine::detail::EngineFrameInspection::capture(orchestrator);
        if (!trace) return trace.result;
        const auto marker = std::find_if(trace.value.stages.begin(), trace.value.stages.end(),
            [](const auto& observation) { return observation.stage == aeg::pipeline::StageId("OrchestratorMarkerStage"); });
        if (marker == trace.value.stages.end() || !context.frame_context.is_frame_active() ||
            marker->frame_number != context.frame_context.frame_number() || carrier != state.expected_frame_user_data ||
            marker->user_data_identity != reinterpret_cast<uintptr_t>(carrier))
            return aeg::foundation::invalid_state("preparation marker lost the actual frame user data carrier");
        state.marker_received_user_data = true;
        const auto pass = context.render_graph.add_pass(
            "OrchestratorRuntimePass", aeg::rhi::QueueClass::kGraphics,
            [](aeg::render_graph::PassBuilder& builder) {
                builder.set_side_effects();
                return Result::success();
            },
            [&state, carrier](aeg::render_graph::RenderPassCommandContext&) {
                if (carrier != state.expected_frame_user_data)
                    return aeg::foundation::invalid_state("fixed marker execution lost frame user data");
                ++state.render_pass_execution_count;
                return Result::success();
            });
        return pass ? Result::success() : pass.result;
    };
    frame_desc.render_feature_post_build = [&state, &orchestrator](const aeg::engine::EngineFramePostBuildContext& context) {
        const auto before = aeg::engine::detail::EngineFrameInspection::capture(orchestrator);
        const auto change = aeg::engine::detail::EngineFrameInspection::enable(orchestrator, false);
        const auto after = aeg::engine::detail::EngineFrameInspection::capture(orchestrator);
        if (!before || !after || before.value.stages.empty() ||
            change.code != ResultCode::kInvalidState || before.value.stages.size() != after.value.stages.size() ||
            before.value.stages.front().stage != after.value.stages.front().stage)
            return aeg::foundation::invalid_state("active-frame inspection change lost committed visits");
        state.post_build_hook_invoked = true;
        const auto pass = context.render_graph.add_pass(
            "OrchestratorPostBuildPass", aeg::rhi::QueueClass::kGraphics,
            [](aeg::render_graph::PassBuilder& builder) {
                builder.set_side_effects();
                return Result::success();
            },
            [&state](aeg::render_graph::RenderPassCommandContext&) {
                ++state.post_build_pass_execution_count;
                return Result::success();
            });
        return pass ? Result::success() : pass.result;
    };
    if (const Result result = runtime.tick(frame_desc); !result || runtime.frame_number() != 1) {
        return 10;
    }
    if (!read_phase_trace(orchestrator, state)) return 11;
    const std::vector<PhaseId> expected_frame_phases{
        kBeforeStartupPhase, kPreFramePhase, kFramePhase, aeg::pipeline::to_phase_id(EnginePhase::kPresent),
        kPostFramePhase,
    };
    if (state.executed_phases != expected_frame_phases || state.render_pass_execution_count != 1 ||
        state.post_build_pass_execution_count != 1 || !state.post_build_hook_invoked ||
        !state.marker_received_user_data) {
        return 11;
    }

    if (const Result result = runtime.pause(); !result) {
        return 12;
    }
    if (const Result result = runtime.stop(); !result || runtime.frame_number() != 0) {
        return 12;
    }
    if (const Result result = runtime.shutdown(); !result) {
        return 12;
    }
    if (!read_phase_trace(orchestrator, state)) return 13;
    const std::vector<PhaseId> expected_lifecycle_phases{
        kBeforeStartupPhase, kPreFramePhase,      kFramePhase, aeg::pipeline::to_phase_id(EnginePhase::kPresent),
        kPostFramePhase,     kAfterShutdownPhase,
    };
    if (state.executed_phases != expected_lifecycle_phases) return 13;
    std::printf("legacy_callback_count=%u\n", state.legacy_callback_count);
    return state.legacy_callback_count == 0 ? 0 : 73;
}
