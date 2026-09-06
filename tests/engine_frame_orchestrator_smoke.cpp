/**
 * @file engine_frame_orchestrator_smoke.cpp
 * @brief Exercises custom Phase intervals and automatic RenderGraph orchestration.
 */
#include <memory>
#include <vector>

#include <engine_frame_orchestrator.hpp>
#include <pipeline.hpp>
#include <rhi.hpp>

namespace {
    using aeg::foundation::Result;
    using aeg::foundation::ResultCode;
    using aeg::pipeline::EnginePhase;
    using aeg::pipeline::ExecutionDomain;
    using aeg::pipeline::FeatureDomain;
    using aeg::pipeline::FeaturePointFlags;
    using aeg::pipeline::PhaseId;
    using aeg::pipeline::PipelineScope;
    using aeg::pipeline::TimePolicy;

    inline constexpr PhaseId kBeforeStartupPhase{"BeforeStartupSmoke"};
    inline constexpr PhaseId kPreFramePhase{"PreFrameSmoke"};
    inline constexpr PhaseId kFramePhase{"FrameSmoke"};
    inline constexpr PhaseId kPostFramePhase{"PostFrameSmoke"};
    inline constexpr PhaseId kAfterShutdownPhase{"AfterShutdownSmoke"};

    struct OrchestratorState final {
        std::vector<PhaseId> executed_phases{};
        void* expected_frame_user_data{nullptr};
        uint32_t render_pass_execution_count{0};
        bool render_feature_received_user_data{false};
    };

    [[nodiscard]] Result record_phase(aeg::pipeline::PipelineTaskContext& context) {
        auto* state = static_cast<OrchestratorState*>(context.user_data());
        if (state == nullptr) {
            return Result::failure(ResultCode::kInvalidArgument, "orchestrator smoke state is null");
        }
        if (context.node().phase_id == kPostFramePhase &&
            context.execution_context().user_data() != state->expected_frame_user_data) {
            return Result::failure(ResultCode::kInvalidState, "post-frame phase lost frame user data");
        }
        state->executed_phases.push_back(context.node().phase_id);
        return Result::success();
    }

    class OrchestratorPipeline final : public aeg::pipeline::Pipeline {
    public:
        explicit OrchestratorPipeline(OrchestratorState& state) : state_(state) {
            descriptor_.id = aeg::pipeline::PipelineId("EngineFrameOrchestratorSmoke");
            descriptor_.name = "EngineFrameOrchestratorSmoke";
            descriptor_.category = aeg::pipeline::PipelineCategory::kRendering;
            descriptor_.domains = aeg::pipeline::to_mask(FeatureDomain::kRender);
            descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kFrame);
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
            const auto add_phase_task = [&](aeg::pipeline::StageId stage_id,
                                            aeg::pipeline::TaskId task_id,
                                            EnginePhase compatibility_phase,
                                            PhaseId phase,
                                            TimePolicy time_policy) -> Result {
                const auto stage = builder.add_stage({
                        stage_id,
                        compatibility_phase,
                        ExecutionDomain::kMainThread,
                        time_policy,
                        false,
                        phase,
                });
                const auto task = builder.add_task({
                        task_id,
                        aeg::pipeline::TaskFlags::kSideEffect,
                        &record_phase,
                        &state_,
                });
                if (!stage || !task) {
                    return builder.status();
                }
                return builder.bind_task(task, stage);
            };

            if (const Result result = add_phase_task(
                        aeg::pipeline::StageId("BeforeStartupStage"),
                        aeg::pipeline::TaskId("BeforeStartupTask"),
                        EnginePhase::kStartup, kBeforeStartupPhase, TimePolicy::kStartup);
                !result) {
                return result;
            }
            if (const Result result = add_phase_task(
                        aeg::pipeline::StageId("PreFrameStage"),
                        aeg::pipeline::TaskId("PreFrameTask"),
                        EnginePhase::kFrameBegin, kPreFramePhase, TimePolicy::kFrame);
                !result) {
                return result;
            }
            if (const Result result = add_phase_task(
                        aeg::pipeline::StageId("FrameStage"),
                        aeg::pipeline::TaskId("FrameTask"),
                        EnginePhase::kVariableUpdate, kFramePhase, TimePolicy::kFrame);
                !result) {
                return result;
            }
            if (const Result result = add_phase_task(
                        aeg::pipeline::StageId("PostFrameStage"),
                        aeg::pipeline::TaskId("PostFrameTask"),
                        EnginePhase::kFrameEnd, kPostFramePhase, TimePolicy::kFrame);
                !result) {
                return result;
            }
            if (const Result result = add_phase_task(
                        aeg::pipeline::StageId("AfterShutdownStage"),
                        aeg::pipeline::TaskId("AfterShutdownTask"),
                        EnginePhase::kShutdown, kAfterShutdownPhase, TimePolicy::kShutdown);
                !result) {
                return result;
            }

            const auto render_anchor = builder.add_stage({
                    aeg::pipeline::StageId("OrchestratorRenderAnchor"),
                    EnginePhase::kRenderBuild,
                    ExecutionDomain::kMainThread,
                    TimePolicy::kFrame,
                    false,
            });
            if (!render_anchor) {
                return builder.status();
            }
            const auto point = builder.add_feature_point({
                    aeg::pipeline::FeaturePointId("OrchestratorRenderPoint"),
                    "OrchestratorRenderPoint",
                    {},
                    aeg::pipeline::StageId("OrchestratorRenderAnchor"),
                    aeg::pipeline::FeaturePointPosition::kBefore,
                    aeg::pipeline::to_mask(FeatureDomain::kRender),
                    aeg::pipeline::to_mask(PipelineScope::kFrame),
                    FeaturePointFlags::kMayCreateComputePass,
            });
            return point ? Result::success() : builder.status();
        }

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
        OrchestratorState& state_;
    };

    class OrchestratorRenderFeature final : public aeg::pipeline::RenderFeature {
    public:
        explicit OrchestratorRenderFeature(OrchestratorState& state) : state_(state) {
            descriptor_.id = aeg::pipeline::FeatureId("OrchestratorRenderFeature");
            descriptor_.name = "OrchestratorRenderFeature";
            descriptor_.domain = FeatureDomain::kRender;
            descriptor_.domain_mask = aeg::pipeline::to_mask(FeatureDomain::kRender);
            descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kFrame);
        }

        [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] Result build_render_graph(aeg::pipeline::RenderGraphBuilder& builder) const override {
            state_.render_feature_received_user_data =
                    builder.user_data() == state_.expected_frame_user_data;
            const auto pass = builder.add_compute_pass(
                    "OrchestratorRuntimePass",
                    [](aeg::pipeline::RenderPassBuilder& pass_builder) {
                        pass_builder.set_side_effects();
                        return Result::success();
                    },
                    [this](aeg::pipeline::RenderPassExecutionContext&) {
                        ++state_.render_pass_execution_count;
                        return Result::success();
                    },
                    aeg::rhi::QueueClass::kGraphics);
            return pass ? Result::success() : pass.result;
        }

    protected:
        [[nodiscard]] Result setup(aeg::pipeline::RenderGraphBuilder& builder) const override {
            const auto stage = builder.add_stage({
                    aeg::pipeline::StageId("OrchestratorRenderFeatureStage"),
                    EnginePhase::kRenderBuild,
                    ExecutionDomain::kMainThread,
                    TimePolicy::kFrame,
                    false,
            });
            return stage ? Result::success() : Result::failure(
                ResultCode::kInvalidState,
                "orchestrator render feature stage declaration failed");
        }

    private:
        aeg::pipeline::FeatureDescriptor descriptor_{};
        OrchestratorState& state_;
    };
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
    const auto register_phase = [&](PhaseId id, PhaseId anchor,
                                    aeg::pipeline::PhaseOrderRelation relation) {
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
    if (const Result result = registry.register_pipeline(
                [&state] { return std::make_unique<OrchestratorPipeline>(state); });
        !result) {
        return 3;
    }
    if (const Result result = registry.register_feature(
                [&state] { return std::make_unique<OrchestratorRenderFeature>(state); });
        !result) {
        return 4;
    }
    if (const Result result = registry.bind_feature({
                aeg::pipeline::FeatureId("OrchestratorRenderFeature"),
                aeg::pipeline::PipelineId("EngineFrameOrchestratorSmoke"),
                PipelineScope::kFrame,
                aeg::pipeline::FeaturePointId("OrchestratorRenderPoint"),
                0,
                true,
                {},
        });
        !result) {
        return 5;
    }

    aeg::pipeline::PipelineScopeHost host(
            PipelineScope::kFrame,
            aeg::pipeline::PipelineInstanceId("EngineFrameOrchestratorSmokeInstance"));
    if (const Result result = host.bind_pipeline(
                aeg::pipeline::PipelineId("EngineFrameOrchestratorSmoke"));
        !result) {
        return 6;
    }

    aeg::pipeline::PipelineScheduler scheduler({1, 1, 1, 1});
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFrameOrchestrator orchestrator(
            registry, scheduler, frame_context, render_graph);
    if (const Result result = orchestrator.register_pipeline(host); !result) {
        return 7;
    }
    if (const Result result = orchestrator.initialize(); !result) {
        return 8;
    }
    if (state.executed_phases != std::vector<PhaseId>{kBeforeStartupPhase}) {
        return 9;
    }

    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.user_data = &frame_user_data;
    if (const Result result = orchestrator.run_frame(frame_desc); !result) {
        return 10;
    }
    const std::vector<PhaseId> expected_frame_phases{
        kBeforeStartupPhase,
        kPreFramePhase,
        kFramePhase,
        kPostFramePhase,
    };
    if (state.executed_phases != expected_frame_phases ||
        state.render_pass_execution_count != 1 ||
        !state.render_feature_received_user_data) {
        return 11;
    }

    if (const Result result = orchestrator.shutdown(); !result) {
        return 12;
    }
    const std::vector<PhaseId> expected_lifecycle_phases{
        kBeforeStartupPhase,
        kPreFramePhase,
        kFramePhase,
        kPostFramePhase,
        kAfterShutdownPhase,
    };
    return state.executed_phases == expected_lifecycle_phases ? 0 : 13;
}
