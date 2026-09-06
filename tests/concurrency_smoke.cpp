/**
 * @file concurrency_smoke.cpp
 * @brief Exercises completion, cancellation, executor, and task-group contracts.
 */
#include <atomic>
#include <concurrency.hpp>
#include <memory>
#include <thread>

namespace {
    using aeg::foundation::Result;
    using aeg::foundation::ResultCode;

    [[nodiscard]] Result failure_result() {
        return Result::failure(ResultCode::kInvalidState, "intentional task failure");
    }
}  // namespace

int main() {
    using aeg::concurrency::CompletionSource;
    using aeg::concurrency::ExecutorDomain;
    using aeg::concurrency::ExecutorRegistry;
    using aeg::concurrency::TaskDesc;
    using aeg::concurrency::TaskGraph;
    using aeg::concurrency::TaskGroup;
    using aeg::concurrency::TaskHandle;
    using aeg::concurrency::WorkerExecutor;
    using aeg::concurrency::WorkerExecutorDesc;

    CompletionSource completion_source;
    const aeg::concurrency::Completion completion = completion_source.completion();
    if (!completion.is_valid() || completion.is_complete() || completion.result().code != ResultCode::kNotReady ||
        completion.wait(0).code != ResultCode::kTimeout) {
        return 1;
    }
    if (!completion_source.complete() || !completion.is_complete() || !completion.result()) {
        return 2;
    }
    if (completion_source.complete().code != ResultCode::kInvalidState) {
        return 3;
    }

    WorkerExecutor executor({ExecutorDomain::kCpuWorker, 2, false});
    if (executor.worker_count() != 2 || executor.domain() != ExecutorDomain::kCpuWorker) {
        return 4;
    }

    const std::thread::id main_thread = std::this_thread::get_id();
    std::atomic<uint32_t> completed_tasks{0};
    std::atomic<uint32_t> worker_tasks{0};
    TaskGroup successful_group;
    for (uint32_t index = 0; index < 8; ++index) {
        TaskDesc task{};
        task.execute = [&](const aeg::concurrency::TaskContext&) {
            completed_tasks.fetch_add(1, std::memory_order_relaxed);
            if (std::this_thread::get_id() != main_thread) {
                worker_tasks.fetch_add(1, std::memory_order_relaxed);
            }
            return Result::success();
        };
        if (!successful_group.submit(executor, std::move(task))) {
            return 5;
        }
    }
    if (!successful_group.wait(5'000'000'000ull) || completed_tasks.load() != 8 || worker_tasks.load() != 8) {
        return 6;
    }
    if (executor.wait_idle(5'000'000'000ull).code != ResultCode::kSuccess) {
        return 7;
    }

    std::atomic<uint32_t> cancelled_callback_count{0};
    aeg::concurrency::CancellationSource cancellation_source;
    static_cast<void>(cancellation_source.cancel());
    TaskDesc cancelled_task{};
    cancelled_task.cancellation = cancellation_source.token();
    cancelled_task.execute = [&](const aeg::concurrency::TaskContext&) {
        cancelled_callback_count.fetch_add(1, std::memory_order_relaxed);
        return Result::success();
    };
    const auto cancelled_submission = executor.submit(std::move(cancelled_task));
    if (!cancelled_submission || cancelled_submission.value.wait(5'000'000'000ull).code != ResultCode::kCancelled ||
        cancelled_callback_count.load() != 0) {
        return 8;
    }

    WorkerExecutor deterministic_executor({ExecutorDomain::kIoWorker, 1, true});
    TaskGroup failing_group;
    std::atomic<bool> allow_failure{false};
    TaskDesc failing_task{};
    failing_task.execute = [&](const aeg::concurrency::TaskContext&) {
        while (!allow_failure.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return failure_result();
    };
    if (!failing_group.submit(deterministic_executor, std::move(failing_task))) {
        return 9;
    }

    std::atomic<uint32_t> cancelled_after_failure{0};
    TaskDesc queued_task{};
    queued_task.execute = [&](const aeg::concurrency::TaskContext&) {
        cancelled_after_failure.fetch_add(1, std::memory_order_relaxed);
        return Result::success();
    };
    if (!failing_group.submit(deterministic_executor, std::move(queued_task))) {
        return 10;
    }
    allow_failure.store(true, std::memory_order_release);
    const Result failing_result = failing_group.wait(5'000'000'000ull);
    if (failing_result.code != ResultCode::kInvalidState || cancelled_after_failure.load() != 0) {
        return 11;
    }

    ExecutorRegistry registry;
    auto registered_executor =
        std::make_shared<WorkerExecutor>(WorkerExecutorDesc{ExecutorDomain::kRenderThread, 1, true});
    if (!registry.register_executor(registered_executor)) {
        return 12;
    }
    const auto found_executor = registry.find_executor(ExecutorDomain::kRenderThread);
    if (!found_executor || found_executor.value != registered_executor) {
        return 13;
    }
    TaskDesc registry_task{};
    registry_task.execute = [](const aeg::concurrency::TaskContext&) { return Result::success(); };
    const auto registry_submission = registry.submit(ExecutorDomain::kRenderThread, std::move(registry_task));
    if (!registry_submission || !registry_submission.value.wait(5'000'000'000ull) ||
        !registry.unregister_executor(ExecutorDomain::kRenderThread, 5'000'000'000ull) ||
        registry.find_executor(ExecutorDomain::kRenderThread).result.code != ResultCode::kNotFound) {
        return 14;
    }

    ExecutorRegistry graph_registry;
    if (!graph_registry.register_executor(
            std::make_shared<WorkerExecutor>(WorkerExecutorDesc{ExecutorDomain::kCpuWorker, 1, true}))) {
        return 15;
    }
    std::atomic<uint32_t> graph_state{0};
    TaskGraph graph;
    TaskDesc graph_source{};
    graph_source.domain = ExecutorDomain::kMainThread;
    graph_source.execute = [&](const aeg::concurrency::TaskContext&) {
        graph_state.store(1, std::memory_order_release);
        return Result::success();
    };
    const aeg::foundation::ResultValue<TaskHandle> source_handle = graph.add_task(std::move(graph_source));
    if (!source_handle) {
        return 16;
    }
    TaskDesc graph_consumer{};
    graph_consumer.domain = ExecutorDomain::kCpuWorker;
    graph_consumer.execute = [&](const aeg::concurrency::TaskContext&) {
        if (graph_state.load(std::memory_order_acquire) != 1) {
            return Result::failure(ResultCode::kInvalidState, "task graph dependency was not enforced");
        }
        graph_state.store(2, std::memory_order_release);
        return Result::success();
    };
    const aeg::foundation::ResultValue<TaskHandle> consumer_handle = graph.add_task(std::move(graph_consumer));
    if (!consumer_handle || !graph.add_dependency(source_handle.value, consumer_handle.value) || !graph.validate() ||
        !graph.execute(graph_registry) || graph_state.load() != 2) {
        return 17;
    }

    TaskGraph cyclic_graph;
    TaskDesc cyclic_task{};
    cyclic_task.execute = [](const aeg::concurrency::TaskContext&) { return Result::success(); };
    const auto cyclic_first = cyclic_graph.add_task(cyclic_task);
    const auto cyclic_second = cyclic_graph.add_task(std::move(cyclic_task));
    const Result cyclic_first_dependency = cyclic_graph.add_dependency(cyclic_first.value, cyclic_second.value);
    const Result cyclic_second_dependency = cyclic_graph.add_dependency(cyclic_second.value, cyclic_first.value);
    if (!cyclic_first || !cyclic_second || !cyclic_first_dependency || !cyclic_second_dependency ||
        cyclic_graph.validate().code != ResultCode::kInvalidArgument) {
        return 18;
    }

    return 0;
}
