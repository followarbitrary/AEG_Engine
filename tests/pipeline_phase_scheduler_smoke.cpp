/**
 * @file pipeline_phase_scheduler_smoke.cpp
 * @brief Exercises custom global Phases and all PipelineScheduler worker lanes.
 */
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <pipeline.hpp>
#include <thread>
#include <vector>

namespace {
struct TaskState final {
    std::atomic<uint32_t> execution_count{0};
    std::mutex mutex;
    std::thread::id worker_thread{};
};

[[nodiscard]] aeg::foundation::Result record_task(aeg::pipeline::PipelineTaskContext& context) {
    auto* state = static_cast<TaskState*>(context.user_data());
    if (state == nullptr) {
        return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidArgument,
                                                "pipeline task state is null");
    }
    state->execution_count.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(state->mutex);
        state->worker_thread = std::this_thread::get_id();
    }
    return aeg::foundation::Result::success();
}

class PhasePipeline final : public aeg::pipeline::Pipeline {
public:
    PhasePipeline(TaskState& io_state, TaskState& render_state, TaskState& gpu_state)
        : io_state_(io_state), render_state_(render_state), gpu_state_(gpu_state) {
        descriptor_.id = aeg::pipeline::PipelineId("PhaseSchedulerSmoke");
        descriptor_.name = "PhaseSchedulerSmoke";
        descriptor_.category = aeg::pipeline::PipelineCategory::kCore;
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto io_stage = builder.add_stage({
            aeg::pipeline::StageId("IoStage"),
            aeg::pipeline::EnginePhase::kPreUpdate,
            aeg::pipeline::ExecutionDomain::kIoWorker,
            aeg::pipeline::TimePolicy::kFrame,
            true,
            aeg::pipeline::PhaseId("IoPhase"),
        });
        const auto render_stage = builder.add_stage({
            aeg::pipeline::StageId("RenderThreadStage"),
            aeg::pipeline::EnginePhase::kPreUpdate,
            aeg::pipeline::ExecutionDomain::kRenderThread,
            aeg::pipeline::TimePolicy::kFrame,
            true,
            aeg::pipeline::PhaseId("RenderThreadPhase"),
        });
        const auto gpu_stage = builder.add_stage({
            aeg::pipeline::StageId("GpuStage"),
            aeg::pipeline::EnginePhase::kPreUpdate,
            aeg::pipeline::ExecutionDomain::kGpu,
            aeg::pipeline::TimePolicy::kFrame,
            true,
            aeg::pipeline::PhaseId("GpuPhase"),
        });
        if (!io_stage || !render_stage || !gpu_stage) {
            return builder.status();
        }

        const auto io_task = builder.add_task({
            aeg::pipeline::TaskId("IoTask"),
            aeg::pipeline::TaskFlags::kParallelSafe,
            &record_task,
            &io_state_,
        });
        const auto render_task = builder.add_task({
            aeg::pipeline::TaskId("RenderThreadTask"),
            aeg::pipeline::TaskFlags::kParallelSafe,
            &record_task,
            &render_state_,
        });
        const auto gpu_task = builder.add_task({
            aeg::pipeline::TaskId("GpuTask"),
            aeg::pipeline::TaskFlags::kParallelSafe,
            &record_task,
            &gpu_state_,
        });
        if (!io_task || !render_task || !gpu_task) {
            return builder.status();
        }

        if (const auto result = builder.bind_task(io_task, io_stage); !result) {
            return result;
        }
        if (const auto result = builder.bind_task(render_task, render_stage); !result) {
            return result;
        }
        return builder.bind_task(gpu_task, gpu_stage);
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
    TaskState& io_state_;
    TaskState& render_state_;
    TaskState& gpu_state_;
};
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

    const auto phase_snapshot = registry.phase_registry().snapshot();
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

    TaskState io_state;
    TaskState render_state;
    TaskState gpu_state;
    if (const auto result = registry.register_pipeline(
            [&] { return std::make_unique<PhasePipeline>(io_state, render_state, gpu_state); });
        !result) {
        return 6;
    }

    const auto compiled = aeg::pipeline::PipelineCompiler{}.compile({
        .pipeline = aeg::pipeline::PipelineId("PhaseSchedulerSmoke"),
        .scope = aeg::pipeline::PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
        .instance = aeg::pipeline::PipelineInstanceId("PhaseSchedulerSmokeInstance"),
    });
    if (!compiled || compiled.value.phase_revision != phase_snapshot.value.revision) {
        return 7;
    }

    aeg::pipeline::PipelineScheduler scheduler({1, 1, 1, 1});
    if (scheduler.worker_count(aeg::pipeline::ExecutionDomain::kIoWorker) != 1 ||
        scheduler.worker_count(aeg::pipeline::ExecutionDomain::kRenderThread) != 1 ||
        scheduler.worker_count(aeg::pipeline::ExecutionDomain::kGpu) != 1) {
        return 8;
    }

    aeg::pipeline::PipelineExecutionContext context(nullptr, aeg::pipeline::PipelineScope::kEngine);
    if (const auto result = scheduler.execute_phase(compiled.value, context, aeg::pipeline::PhaseId("IoPhase"));
        !result || io_state.execution_count.load() != 1 || render_state.execution_count.load() != 0 ||
        gpu_state.execution_count.load() != 0) {
        return 9;
    }
    if (const auto result =
            scheduler.execute_phase(compiled.value, context, aeg::pipeline::PhaseId("RenderThreadPhase"));
        !result || render_state.execution_count.load() != 1) {
        return 10;
    }
    if (const auto result = scheduler.execute_phase(compiled.value, context, aeg::pipeline::PhaseId("GpuPhase"));
        !result || gpu_state.execution_count.load() != 1) {
        return 11;
    }

    const std::thread::id main_thread = std::this_thread::get_id();
    {
        std::lock_guard lock(io_state.mutex);
        if (io_state.worker_thread == main_thread) {
            return 12;
        }
    }
    {
        std::lock_guard lock(render_state.mutex);
        if (render_state.worker_thread == main_thread) {
            return 13;
        }
    }
    {
        std::lock_guard lock(gpu_state.mutex);
        if (gpu_state.worker_thread == main_thread) {
            return 14;
        }
    }

    aeg::concurrency::CancellationSource cancellation_source;
    static_cast<void>(cancellation_source.cancel());
    aeg::pipeline::PipelineExecutionContext cancelled_context(nullptr, aeg::pipeline::PipelineScope::kEngine);
    if (const auto result = scheduler.execute_phase(compiled.value, cancelled_context,
                                                    aeg::pipeline::PhaseId("IoPhase"), cancellation_source.token());
        result.code != aeg::foundation::ResultCode::kCancelled || io_state.execution_count.load() != 1) {
        return 15;
    }

    aeg::pipeline::PipelineScheduler replacement_scheduler({1, 1, 1, 1});
    if (const auto result = replacement_scheduler.set_executor(
            aeg::pipeline::ExecutionDomain::kIoWorker,
            std::make_shared<aeg::concurrency::InlineExecutor>(aeg::concurrency::ExecutorDomain::kIoWorker));
        !result || replacement_scheduler.worker_count(aeg::pipeline::ExecutionDomain::kIoWorker) != 0) {
        return 16;
    }
    aeg::pipeline::PipelineExecutionContext replacement_context(nullptr, aeg::pipeline::PipelineScope::kEngine);
    if (const auto result =
            replacement_scheduler.execute_phase(compiled.value, replacement_context, aeg::pipeline::PhaseId("IoPhase"));
        !result || io_state.execution_count.load() != 2) {
        return 17;
    }
    {
        std::lock_guard lock(io_state.mutex);
        if (io_state.worker_thread != main_thread) {
            return 18;
        }
    }

    if (const auto result = registry.phase_registry().unregister_phase(aeg::pipeline::PhaseId("IoPhase"));
        result || result.code != aeg::foundation::ResultCode::kInvalidState) {
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
    return 0;
}
