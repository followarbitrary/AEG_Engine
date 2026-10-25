/**
 * @file concurrency_smoke.cpp
 * @brief Exercises completion, cancellation, executor, and task-group contracts.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concurrency.hpp>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultCode;

[[nodiscard]] Result failure_result() {
    return Result::failure(ResultCode::kInvalidState, "intentional task failure");
}

[[nodiscard]] bool wait_for_value(const std::atomic<uint32_t>& value, uint32_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (value.load(std::memory_order_acquire) != expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        aeg::concurrency::yield_current_thread();
    }
    return true;
}

aeg::concurrency::CoroutineTask await_completion(aeg::concurrency::Completion completion,
                                                 std::atomic<uint32_t>& state) {
    state.store(1, std::memory_order_release);
    const Result result = co_await completion;
    if (!result) {
        co_return result;
    }
    state.store(2, std::memory_order_release);
    co_return Result::success();
}

aeg::concurrency::CoroutineTask record_coroutine_start(std::atomic<uint32_t>& start_count) {
    start_count.fetch_add(1, std::memory_order_relaxed);
    co_return Result::success();
}

bool test_identity_issuer_is_unique_across_workers_and_preserves_thread_identity() {
    using namespace aeg::concurrency;
    const auto owner = current_thread_id();
    std::array<std::array<uint64_t, 128>, 16> issued{};
    std::array<CompletionSource, 4> started{};
    CompletionSource release;
    WorkerExecutor executor({ExecutorDomain::kCpuWorker, 4, false});
    TaskGroup group;
    for (std::size_t batch = 0; batch < issued.size(); ++batch) {
        TaskDesc task{};
        task.execute = [&, batch](const TaskContext&) {
            const auto worker = current_thread_id();
            if (batch < started.size()) {
                static_cast<void>(started[batch].complete());
                if (!release.completion().wait(5'000'000'000ull)) return failure_result();
            }
            for (auto& id : issued[batch]) {
                id = allocate_unique_identity();
                if (id == 0 || id == owner.value || id == worker.value || current_thread_id() != worker)
                    return failure_result();
            }
            return Result::success();
        };
        if (!group.submit(executor, std::move(task))) return false;
    }
    for (const auto& worker : started)
        if (!worker.completion().wait(5'000'000'000ull)) return false;
    static_cast<void>(release.complete());
    if (!group.wait(5'000'000'000ull) || !executor.wait_idle(5'000'000'000ull) || current_thread_id() != owner)
        return false;
    std::vector<uint64_t> identities;
    for (const auto& batch : issued) identities.insert(identities.end(), batch.begin(), batch.end());
    std::sort(identities.begin(), identities.end());
    return identities.front() != 0 && std::adjacent_find(identities.begin(), identities.end()) == identities.end();
}

bool test_executor_lanes_cancellation_and_inline_replacement() {
    using namespace aeg::concurrency;
    const auto owner = current_thread_id();
    // These are existing Concurrency lanes. kGpu selects its technical executor,
    // and does not introduce another Feature domain or claim a GPU shader ran.
    constexpr std::array domains{ExecutorDomain::kCpuWorker, ExecutorDomain::kIoWorker,
                                 ExecutorDomain::kRenderThread, ExecutorDomain::kGpu};
    struct LaneState final {
        std::atomic<uint32_t> count{0};
        ThreadId thread{};
    };
    std::array<LaneState, domains.size()> states{};
    std::array<std::shared_ptr<WorkerExecutor>, domains.size()> executors{};
    ThreadId main_execution{};
    ExecutorRegistry registry;
    const auto main = registry.find_executor(ExecutorDomain::kMainThread);
    if (!main || main.value->domain() != ExecutorDomain::kMainThread || main.value->worker_count() != 0) return false;
    TaskDesc main_task;
    main_task.execute = [&](const TaskContext&) {
        main_execution = current_thread_id();
        return Result::success();
    };
    const auto main_submission = registry.submit(ExecutorDomain::kMainThread, std::move(main_task));
    if (!main_submission || !main_submission.value.wait(5'000'000'000ull) || main_execution != owner) return false;

    const auto make_task = [&](std::size_t index) {
        TaskDesc task;
        task.domain = domains[index];
        task.user_data = &states[index];
        task.execute = [&, index](const TaskContext& context) {
            if (context.user_data != &states[index]) return failure_result();
            states[index].thread = current_thread_id();
            states[index].count.fetch_add(1, std::memory_order_relaxed);
            return Result::success();
        };
        return task;
    };
    for (std::size_t index = 0; index < domains.size(); ++index) {
        executors[index] = std::make_shared<WorkerExecutor>(WorkerExecutorDesc{domains[index], 1, true});
        if (!registry.register_executor(executors[index])) return false;
        const auto executor = registry.find_executor(domains[index]);
        const auto submitted = registry.submit(domains[index], make_task(index));
        if (!executor || executor.value != executors[index] || executor.value->domain() != domains[index] ||
            executor.value->worker_count() != 1 || !submitted || !submitted.value.wait(5'000'000'000ull) ||
            states[index].count.load() != 1 || !states[index].thread.is_valid() || states[index].thread == owner) {
            std::cerr << "Direct Concurrency lane execution failed: " << index << '\n';
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous)
            if (states[previous].thread == states[index].thread) return false;
    }
    CancellationSource cancellation;
    static_cast<void>(cancellation.cancel());
    auto cancelled_task = make_task(1);
    cancelled_task.cancellation = cancellation.token();
    const auto cancelled = registry.submit(ExecutorDomain::kIoWorker, std::move(cancelled_task));
    if (!cancelled || cancelled.value.wait(5'000'000'000ull).code != ResultCode::kCancelled ||
        states[1].count.load() != 1) return false;

    auto inline_io = std::make_shared<InlineExecutor>(ExecutorDomain::kIoWorker);
    if (registry.register_executor(inline_io).code != ResultCode::kInvalidState ||
        registry.find_executor(ExecutorDomain::kIoWorker).value != executors[1] ||
        !registry.wait_idle(5'000'000'000ull) || !registry.register_executor(inline_io, true)) return false;
    const auto replacement = registry.find_executor(ExecutorDomain::kIoWorker);
    const auto inline_submission = registry.submit(ExecutorDomain::kIoWorker, make_task(1));
    if (!replacement || replacement.value != inline_io || replacement.value->worker_count() != 0 ||
        !inline_submission || !inline_submission.value.wait(5'000'000'000ull) || states[1].count.load() != 2 ||
        states[1].thread != owner || !registry.wait_idle(5'000'000'000ull)) return false;
    return current_thread_id() == owner;
}
}  // namespace

int main() {
    if (!test_identity_issuer_is_unique_across_workers_and_preserves_thread_identity()) return 32;
    if (!test_executor_lanes_cancellation_and_inline_replacement()) return 33;
    using aeg::concurrency::CompletionSource;
    using aeg::concurrency::ExecutorDomain;
    using aeg::concurrency::ExecutorRegistry;
    using aeg::concurrency::TaskDesc;
    using aeg::concurrency::TaskGraph;
    using aeg::concurrency::TaskGroup;
    using aeg::concurrency::TaskHandle;
    using aeg::concurrency::TaskPriority;
    using aeg::concurrency::WorkerExecutor;
    using aeg::concurrency::WorkerExecutorDesc;

    const auto owner_id = aeg::concurrency::current_thread_id();
    if (!owner_id.is_valid() || owner_id != aeg::concurrency::current_thread_id()) {
        return 28;
    }
    aeg::concurrency::CancellationSource delay_cancellation;
    CompletionSource delay_started;
    WorkerExecutor delay_executor({ExecutorDomain::kIoWorker, 1, true});
    aeg::concurrency::ThreadId delay_thread{};
    TaskDesc delay_task{};
    delay_task.execute = [&](const aeg::concurrency::TaskContext&) {
        delay_thread = aeg::concurrency::current_thread_id();
        if (delay_thread == owner_id || delay_thread != aeg::concurrency::current_thread_id()) {
            return failure_result();
        }
        static_cast<void>(delay_started.complete());
        return aeg::concurrency::sleep_for(5'000'000'000ull, delay_cancellation.token());
    };
    const auto delay = delay_executor.submit(std::move(delay_task));
    if (!delay || !delay_started.completion().wait(5'000'000'000ull) ||
        delay.value.wait(10'000'000ull).code != ResultCode::kTimeout) {
        return 29;
    }
    static_cast<void>(delay_cancellation.cancel());
    if (delay.value.wait(500'000'000ull).code != ResultCode::kCancelled ||
        aeg::concurrency::sleep_for(0, delay_cancellation.token()).code != ResultCode::kCancelled ||
        !aeg::concurrency::sleep_for(0)) {
        return 30;
    }
    const auto delay_start = std::chrono::steady_clock::now();
    if (!aeg::concurrency::sleep_for(1'000'000ull) ||
        std::chrono::steady_clock::now() - delay_start < std::chrono::milliseconds(1)) {
        return 31;
    }

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

    const aeg::concurrency::ThreadId main_thread = aeg::concurrency::current_thread_id();
    std::atomic<uint32_t> completed_tasks{0};
    std::atomic<uint32_t> worker_tasks{0};
    TaskGroup successful_group;
    for (uint32_t index = 0; index < 8; ++index) {
        TaskDesc task{};
        task.execute = [&](const aeg::concurrency::TaskContext&) {
            completed_tasks.fetch_add(1, std::memory_order_relaxed);
            if (aeg::concurrency::current_thread_id() != main_thread) {
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

    CompletionSource awaited_source;
    std::atomic<uint32_t> coroutine_state{0};
    const auto coroutine_submission = await_completion(awaited_source.completion(), coroutine_state).schedule(executor);
    if (!coroutine_submission || !wait_for_value(coroutine_state, 1) || coroutine_submission.value.is_complete()) {
        return 8;
    }
    if (!awaited_source.complete() || !coroutine_submission.value.wait(5'000'000'000ull) ||
        coroutine_state.load(std::memory_order_acquire) != 2) {
        return 9;
    }

    CompletionSource failed_await_source;
    std::atomic<uint32_t> failed_coroutine_state{0};
    const auto failed_coroutine_submission =
        await_completion(failed_await_source.completion(), failed_coroutine_state).schedule(executor);
    if (!failed_coroutine_submission || !wait_for_value(failed_coroutine_state, 1) ||
        !failed_await_source.complete(failure_result()) ||
        failed_coroutine_submission.value.wait(5'000'000'000ull).code != ResultCode::kInvalidState ||
        failed_coroutine_state.load(std::memory_order_acquire) != 1) {
        return 10;
    }

    CompletionSource cancellation_gate;
    aeg::concurrency::CancellationSource suspended_cancellation;
    std::atomic<uint32_t> suspended_coroutine_state{0};
    const auto suspended_coroutine_submission =
        await_completion(cancellation_gate.completion(), suspended_coroutine_state)
            .schedule(executor, TaskPriority::kNormal, suspended_cancellation.token());
    if (!suspended_coroutine_submission || !wait_for_value(suspended_coroutine_state, 1) ||
        !suspended_cancellation.cancel() ||
        suspended_coroutine_submission.value.wait(100'000'000ull).code != ResultCode::kCancelled ||
        suspended_coroutine_state.load(std::memory_order_acquire) != 1) {
        return 11;
    }

    aeg::concurrency::CancellationSource coroutine_cancellation;
    static_cast<void>(coroutine_cancellation.cancel());
    std::atomic<uint32_t> cancelled_coroutine_start_count{0};
    const auto cancelled_coroutine_submission =
        record_coroutine_start(cancelled_coroutine_start_count)
            .schedule(executor, TaskPriority::kNormal, coroutine_cancellation.token());
    if (!cancelled_coroutine_submission ||
        cancelled_coroutine_submission.value.wait(5'000'000'000ull).code != ResultCode::kCancelled ||
        cancelled_coroutine_start_count.load(std::memory_order_relaxed) != 0) {
        return 12;
    }

    aeg::concurrency::Completion stopped_worker_completion;
    std::atomic<uint32_t> stopped_worker_state{0};
    {
        WorkerExecutor stopped_executor({ExecutorDomain::kCpuWorker, 1, true});
        TaskDesc stopped_task{};
        stopped_task.execute = [&](const aeg::concurrency::TaskContext& context) {
            stopped_worker_state.store(1, std::memory_order_release);
            while (!context.is_stop_requested()) {
                aeg::concurrency::yield_current_thread();
            }
            stopped_worker_state.store(2, std::memory_order_release);
            return Result::success();
        };
        const auto stopped_worker_submission = stopped_executor.submit(std::move(stopped_task));
        if (!stopped_worker_submission || !wait_for_value(stopped_worker_state, 1)) {
            return 13;
        }
        stopped_worker_completion = stopped_worker_submission.value;
    }
    if (stopped_worker_state.load(std::memory_order_acquire) != 2 ||
        !stopped_worker_completion.wait(5'000'000'000ull)) {
        return 14;
    }

    aeg::concurrency::Completion stopped_coroutine_completion;
    std::atomic<uint32_t> stopped_coroutine_state{0};
    {
        WorkerExecutor stopped_executor({ExecutorDomain::kCpuWorker, 1, true});
        CompletionSource stopped_coroutine_gate;
        const auto stopped_coroutine_submission =
            await_completion(stopped_coroutine_gate.completion(), stopped_coroutine_state).schedule(stopped_executor);
        if (!stopped_coroutine_submission || !wait_for_value(stopped_coroutine_state, 1)) {
            return 26;
        }
        stopped_coroutine_completion = stopped_coroutine_submission.value;
    }
    if (stopped_coroutine_completion.wait(100'000'000ull).code != ResultCode::kCancelled ||
        stopped_coroutine_state.load(std::memory_order_acquire) != 1) {
        return 27;
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
        return 15;
    }

    WorkerExecutor deterministic_executor({ExecutorDomain::kIoWorker, 1, true});
    TaskGroup failing_group;
    std::atomic<bool> allow_failure{false};
    TaskDesc failing_task{};
    failing_task.execute = [&](const aeg::concurrency::TaskContext&) {
        while (!allow_failure.load(std::memory_order_acquire)) {
            aeg::concurrency::yield_current_thread();
        }
        return failure_result();
    };
    if (!failing_group.submit(deterministic_executor, std::move(failing_task))) {
        return 16;
    }

    std::atomic<uint32_t> cancelled_after_failure{0};
    TaskDesc queued_task{};
    queued_task.execute = [&](const aeg::concurrency::TaskContext&) {
        cancelled_after_failure.fetch_add(1, std::memory_order_relaxed);
        return Result::success();
    };
    if (!failing_group.submit(deterministic_executor, std::move(queued_task))) {
        return 17;
    }
    allow_failure.store(true, std::memory_order_release);
    const Result failing_result = failing_group.wait(5'000'000'000ull);
    if (failing_result.code != ResultCode::kInvalidState || cancelled_after_failure.load() != 0) {
        return 18;
    }

    ExecutorRegistry registry;
    auto registered_executor =
        std::make_shared<WorkerExecutor>(WorkerExecutorDesc{ExecutorDomain::kRenderThread, 1, true});
    if (!registry.register_executor(registered_executor)) {
        return 19;
    }
    const auto found_executor = registry.find_executor(ExecutorDomain::kRenderThread);
    if (!found_executor || found_executor.value != registered_executor) {
        return 20;
    }
    TaskDesc registry_task{};
    registry_task.execute = [](const aeg::concurrency::TaskContext&) { return Result::success(); };
    const auto registry_submission = registry.submit(ExecutorDomain::kRenderThread, std::move(registry_task));
    if (!registry_submission || !registry_submission.value.wait(5'000'000'000ull) ||
        !registry.unregister_executor(ExecutorDomain::kRenderThread, 5'000'000'000ull) ||
        registry.find_executor(ExecutorDomain::kRenderThread).result.code != ResultCode::kNotFound) {
        return 21;
    }

    ExecutorRegistry graph_registry;
    if (!graph_registry.register_executor(
            std::make_shared<WorkerExecutor>(WorkerExecutorDesc{ExecutorDomain::kCpuWorker, 1, true}))) {
        return 22;
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
        return 23;
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
        return 24;
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
        return 25;
    }

    return 0;
}
