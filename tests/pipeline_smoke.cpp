/**
 * @file pipeline_smoke.cpp
 * @brief Exercises pipeline definition validation, compilation, and execution.
 */
#include <memory>

#include <pipeline.hpp>

namespace {
    struct ExecutionState {
        int value{0};
    };

    [[nodiscard]] aeg::foundation::Result prepare_task(aeg::pipeline::PipelineTaskContext& context) {
        auto* state = static_cast<ExecutionState*>(context.execution_context().user_data());
        if (state == nullptr) {
            return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidArgument,
                                                    "pipeline execution state is null");
        }
        state->value = 1;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result process_task(aeg::pipeline::PipelineTaskContext& context) {
        auto* state = static_cast<ExecutionState*>(context.execution_context().user_data());
        if (state == nullptr || state->value != 1) {
            return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                    "pipeline dependency was not respected");
        }
        state->value = 2;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result finish_task(aeg::pipeline::PipelineTaskContext& context) {
        auto* state = static_cast<ExecutionState*>(context.execution_context().user_data());
        if (state == nullptr || state->value != 2) {
            return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                    "pipeline output dependency was not respected");
        }
        state->value = 3;
        return aeg::foundation::Result::success();
    }

    class SmokePipeline final : public aeg::pipeline::Pipeline {
    public:
        SmokePipeline() {
            descriptor_.id = aeg::pipeline::PipelineId("PipelineSmoke");
            descriptor_.name = "PipelineSmoke";
            descriptor_.category = aeg::pipeline::PipelineCategory::kCore;
            descriptor_.domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic);
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
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

            const auto prepare = builder.add_task({
                aeg::pipeline::TaskId("PrepareTask"),
                aeg::pipeline::TaskFlags::kMustRun,
                &prepare_task,
                nullptr,
            });
            const auto process = builder.add_task({
                aeg::pipeline::TaskId("ProcessTask"),
                aeg::pipeline::TaskFlags::kMustRun,
                &process_task,
                nullptr,
            });
            const auto finish = builder.add_task({
                aeg::pipeline::TaskId("FinishTask"),
                aeg::pipeline::TaskFlags::kMustRun,
                &finish_task,
                nullptr,
            });
            if (!prepare || !process || !finish) {
                return builder.status();
            }

            if (const auto result = builder.bind_task(prepare, prepare_stage); !result) {
                return result;
            }
            if (const auto result = builder.bind_task(process, process_stage); !result) {
                return result;
            }
            if (const auto result = builder.bind_task(finish, finish_stage); !result) {
                return result;
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

            if (const auto result = builder.reads(
                    process_stage,
                    {aeg::pipeline::ArtifactId("PipelineInput"), aeg::pipeline::ArtifactAccess::kRead, 1,
                     aeg::pipeline::AccessFlags::kNone});
                !result) {
                return result;
            }
            if (const auto result = builder.writes(
                    prepare_stage,
                    {aeg::pipeline::ArtifactId("PipelineInput"), aeg::pipeline::ArtifactAccess::kWrite, 1,
                     aeg::pipeline::AccessFlags::kDiscardContents});
                !result) {
                return result;
            }
            if (const auto result = builder.writes(
                    process_stage,
                    {aeg::pipeline::ArtifactId("PipelineOutput"), aeg::pipeline::ArtifactAccess::kWrite, 1,
                     aeg::pipeline::AccessFlags::kDiscardContents});
                !result) {
                return result;
            }
            return builder.reads(
                finish_stage,
                {aeg::pipeline::ArtifactId("PipelineOutput"), aeg::pipeline::ArtifactAccess::kRead, 1,
                 aeg::pipeline::AccessFlags::kNone});
        }

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
    };

    class CyclicPipeline final : public aeg::pipeline::Pipeline {
    public:
        CyclicPipeline() {
            descriptor_.id = aeg::pipeline::PipelineId("CyclicPipeline");
            descriptor_.name = "CyclicPipeline";
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
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

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
    };
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    if (const auto result = registry.register_pipeline([] { return std::make_unique<SmokePipeline>(); }); !result) {
        return 1;
    }
    if (!registry.find_definition(aeg::pipeline::PipelineId("PipelineSmoke"))) {
        return 2;
    }

    const auto compiled = aeg::pipeline::PipelineCompiler{}.compile({
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

    ExecutionState state;
    aeg::pipeline::PipelineExecutionContext context(&state, aeg::pipeline::PipelineScope::kEngine);
    aeg::pipeline::PipelineScheduler scheduler(2);
    if (const auto result = scheduler.execute(compiled.value, context); !result || state.value != 3) {
        return 5;
    }

    if (const auto result = registry.register_pipeline([] { return std::make_unique<CyclicPipeline>(); }); !result) {
        return 6;
    }
    const auto cyclic = aeg::pipeline::PipelineCompiler{}.compile({
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
