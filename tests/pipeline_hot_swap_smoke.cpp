/**
 * @file pipeline_hot_swap_smoke.cpp
 * @brief Exercises pipeline plan staging, safe switching, and retirement.
 */
#include <algorithm>
#include <memory>

#include <pipeline.hpp>

namespace {
    struct ExecutionState {
        int feature_runs{0};
    };

    [[nodiscard]] aeg::foundation::Result run_feature(aeg::pipeline::PipelineTaskContext& context) {
        auto* state = static_cast<ExecutionState*>(context.execution_context().user_data());
        if (state == nullptr) {
            return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidArgument,
                                                    "hot-swap execution state is null");
        }
        ++state->feature_runs;
        return aeg::foundation::Result::success();
    }

    class HotSwapPipeline final : public aeg::pipeline::Pipeline {
    public:
        explicit HotSwapPipeline(uint32_t artifact_version) : artifact_version_(artifact_version) {
            descriptor_.id = aeg::pipeline::PipelineId("HotSwapPipeline");
            descriptor_.name = "HotSwapPipeline";
            descriptor_.version = {1, 0, artifact_version_};
            descriptor_.category = aeg::pipeline::PipelineCategory::kCore;
            descriptor_.domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic);
            descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine);
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
            const auto artifact = builder.declare_artifact({
                aeg::pipeline::ArtifactId("HotSwapState"),
                aeg::pipeline::ArtifactTypeId("HotSwapState"),
                aeg::pipeline::PipelineScope::kEngine,
                aeg::pipeline::ArtifactStorage::kMutableState,
                aeg::pipeline::ArtifactLifetime::kEngine,
                aeg::pipeline::OwnershipPolicy::kProducerOwned,
                artifact_version_,
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
            const auto point = builder.add_feature_point({
                aeg::pipeline::FeaturePointId("HotSwapFeaturePoint"),
                "HotSwapFeaturePoint",
                {},
                aeg::pipeline::StageId("HotSwapAnchor"),
                aeg::pipeline::FeaturePointPosition::kBefore,
                aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic),
                aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
                aeg::pipeline::FeaturePointFlags::kNone,
            });
            return point ? aeg::foundation::Result::success() : builder.status();
        }

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
        uint32_t artifact_version_{1};
    };

    class HotSwapFeature final : public aeg::pipeline::LogicFeature {
    public:
        [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
            static const aeg::pipeline::FeatureDescriptor descriptor{
                aeg::pipeline::FeatureId("HotSwapFeature"),
                "HotSwapFeature",
                aeg::pipeline::FeatureDomain::kLogic,
                aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic),
                aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
                {1, 0, 0},
                {},
            };
            return descriptor;
        }

    protected:
        [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::LogicGraphBuilder& builder) const override {
            const auto stage = builder.add_stage({
                aeg::pipeline::StageId("HotSwapFeatureStage"),
                aeg::pipeline::EnginePhase::kVariableUpdate,
                aeg::pipeline::ExecutionDomain::kMainThread,
                aeg::pipeline::TimePolicy::kFrame,
                false,
            });
            const auto task = builder.add_task({
                aeg::pipeline::TaskId("HotSwapFeatureTask"),
                aeg::pipeline::TaskFlags::kMustRun,
                &run_feature,
                nullptr,
            });
            if (!stage || !task) {
                return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                         "hot-swap feature declaration failed");
            }
            return builder.bind_task(task, stage);
        }
    };
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    if (const auto result = registry.register_pipeline([] { return std::make_unique<HotSwapPipeline>(1); }); !result) {
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

    aeg::pipeline::PipelinePlanHandle initial_plan = host.active_plan_snapshot();
    if (initial_plan == nullptr || initial_plan->nodes.size() != 2 || host.has_pending_plan()) {
        return 4;
    }
    if (const auto result = host.activate({}); !result) {
        return 5;
    }

    if (const auto result = registry.register_feature([] { return std::make_unique<HotSwapFeature>(); }); !result) {
        return 6;
    }
    if (const auto result = registry.bind_feature({
            aeg::pipeline::FeatureId("HotSwapFeature"),
            aeg::pipeline::PipelineId("HotSwapPipeline"),
            aeg::pipeline::PipelineScope::kEngine,
            aeg::pipeline::FeaturePointId("HotSwapFeaturePoint"),
            0,
            true,
            {},
        });
        !result) {
        return 7;
    }

    const auto hot_rebuild = host.rebuild(registry);
    if (!hot_rebuild || !host.has_pending_plan()) {
        return 8;
    }
    aeg::pipeline::PipelinePlanHandle active_before_apply = host.active_plan_snapshot();
    if (active_before_apply == nullptr || active_before_apply->revision != initial_plan->revision ||
        active_before_apply->nodes.size() != 2) {
        return 9;
    }

    if (const auto result = host.apply_pending_plan(); !result) {
        return 10;
    }
    aeg::pipeline::PipelinePlanHandle active_after_apply = host.active_plan_snapshot();
    if (active_after_apply == nullptr || active_after_apply->revision == initial_plan->revision ||
        active_after_apply->nodes.size() != 3 || host.has_pending_plan() || host.retired_plan_count() != 1) {
        return 11;
    }
    if (active_after_apply->dependencies.empty()) {
        return 17;
    }
    const auto compiled_dependency = active_after_apply->dependencies.front();
    if (const auto result = host.remove_runtime_ordering(compiled_dependency.source_node, compiled_dependency.destination_node);
        !result || !host.has_pending_plan()) {
        return 17;
    }
    if (const auto result = host.apply_pending_plan(); !result) {
        return 18;
    }
    auto without_compiled_order = host.active_plan_snapshot();
    if (without_compiled_order == nullptr || std::any_of(without_compiled_order->dependencies.begin(),
        without_compiled_order->dependencies.end(), [&](const auto& edge) {
            return edge.source_node == compiled_dependency.source_node && edge.destination_node == compiled_dependency.destination_node;
        })) {
        return 19;
    }
    if (const auto result = host.add_runtime_ordering(1, 0); !result || !host.has_pending_plan()) {
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
    auto ordered_plan = host.active_plan_snapshot();
    if (ordered_plan == nullptr || ordered_plan->dependencies.empty() ||
        !ordered_plan->dependencies.back().runtime_override) {
        return 22;
    }
    if (const auto result = host.remove_runtime_ordering(1, 0); !result || !host.has_pending_plan()) {
        return 23;
    }
    if (const auto result = host.apply_pending_plan(); !result) {
        return 24;
    }
    auto restored_plan = host.active_plan_snapshot();
    if (restored_plan == nullptr || std::any_of(restored_plan->dependencies.begin(), restored_plan->dependencies.end(),
        [](const auto& edge) { return edge.runtime_override; })) {
        return 25;
    }

    if (const auto result = host.collect_retired_plans(); !result || host.retired_plan_count() < 1) {
        return 12;
    }

    ExecutionState state;
    aeg::pipeline::PipelineExecutionContext context(
        &state,
        aeg::pipeline::PipelineScope::kEngine,
        &host.artifacts());
    aeg::pipeline::PipelineScheduler scheduler(1);
    if (const auto result = host.execute(scheduler, context); !result || state.feature_runs != 1) {
        return 13;
    }

    const aeg::pipeline::FeatureInstanceId feature_instance = aeg::pipeline::make_feature_instance_id(
        aeg::pipeline::FeatureId("HotSwapFeature"),
        aeg::pipeline::PipelineId("HotSwapPipeline"),
        aeg::pipeline::PipelineScope::kEngine,
        aeg::pipeline::FeaturePointId("HotSwapFeaturePoint"));
    if (const auto result = registry.unbind_feature(feature_instance); !result) {
        return 14;
    }
    if (const auto result = registry.unregister_feature(aeg::pipeline::FeatureId("HotSwapFeature")); !result) {
        return 15;
    }

    initial_plan.reset();
    active_before_apply.reset();
    active_after_apply.reset();
    without_compiled_order.reset();
    ordered_plan.reset();
    if (const auto result = host.collect_retired_plans(); !result || host.retired_plan_count() != 0) {
        return 16;
    }

    if (const auto result = registry.unregister_pipeline(aeg::pipeline::PipelineId("HotSwapPipeline")); !result) {
        return 17;
    }
    if (const auto result = registry.register_pipeline([] { return std::make_unique<HotSwapPipeline>(2); }); !result) {
        return 18;
    }
    if (const auto result = host.rebuild(registry); !result || !host.has_pending_plan()) {
        return 19;
    }
    if (const auto result = host.apply_pending_plan(); result ||
        result.code != aeg::foundation::ResultCode::kInvalidState) {
        return 20;
    }
    const auto blocked_descriptor = host.artifacts().find_descriptor(
        aeg::pipeline::ArtifactId("HotSwapState"));
    if (!blocked_descriptor || blocked_descriptor.value.version != 1 || !host.has_pending_plan()) {
        return 21;
    }

    active_after_apply.reset();
    restored_plan.reset();
    aeg::pipeline::PipelineExecutionContext hot_swap_context(
        &state,
        aeg::pipeline::PipelineScope::kEngine,
        &host.artifacts());
    if (const auto result = host.execute(scheduler, hot_swap_context); !result) {
        return 22;
    }
    aeg::pipeline::PipelinePlanHandle version_two_plan = host.active_plan_snapshot();
    if (version_two_plan == nullptr || version_two_plan->definition_version.patch != 2 ||
        host.has_pending_plan()) {
        return 23;
    }
    if (const auto result = host.collect_retired_plans(); !result || host.retired_plan_count() != 0) {
        return 24;
    }
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
    if (const auto result = registry.register_pipeline([] { return std::make_unique<HotSwapPipeline>(3); }); !result) {
        return 28;
    }
    if (const auto result = host.rebuild(registry); !result || !host.has_pending_plan()) {
        return 29;
    }
    if (const auto result = host.apply_pending_plan(); result ||
        result.code != aeg::foundation::ResultCode::kInvalidState) {
        return 30;
    }
    if (const auto result = writer.commit(); !result) {
        return 31;
    }
    if (const auto result = host.apply_pending_plan(); !result) {
        return 32;
    }
    const aeg::pipeline::PipelinePlanHandle version_three_plan = host.active_plan_snapshot();
    if (version_three_plan == nullptr || version_three_plan->definition_version.patch != 3 ||
        host.has_pending_plan()) {
        return 33;
    }
    if (const auto result = host.collect_retired_plans(); !result || host.retired_plan_count() != 0) {
        return 34;
    }
    if (const auto result = host.deactivate({}); !result) {
        return 35;
    }
    return 0;
}
