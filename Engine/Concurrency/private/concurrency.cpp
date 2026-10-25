/**
 * @file concurrency.cpp
 * @brief Implements platform-independent task execution and completion primitives.
 */
#include "../public/concurrency.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace aeg::concurrency {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::out_of_memory;

[[nodiscard]] foundation::Result cancelled() {
    return foundation::Result::failure(foundation::ResultCode::kCancelled, "task execution was cancelled");
}

[[nodiscard]] bool is_valid_domain(ExecutorDomain domain) noexcept {
    return static_cast<uint8_t>(domain) <= static_cast<uint8_t>(ExecutorDomain::kGpu);
}

[[nodiscard]] std::size_t domain_index(ExecutorDomain domain) noexcept {
    return static_cast<std::size_t>(domain);
}

[[nodiscard]] std::chrono::nanoseconds to_duration(uint64_t timeout_nanoseconds) noexcept {
    using Rep = std::chrono::nanoseconds::rep;
    constexpr uint64_t kMaxDuration = static_cast<uint64_t>(std::numeric_limits<Rep>::max());
    return std::chrono::nanoseconds(static_cast<Rep>(std::min(timeout_nanoseconds, kMaxDuration)));
}

[[nodiscard]] uint64_t remaining_timeout_nanoseconds(const std::chrono::steady_clock::time_point& deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
    return remaining.count() <= 0 ? 1 : static_cast<uint64_t>(remaining.count());
}

[[nodiscard]] foundation::Result execute_task(const TaskDesc& task, std::stop_token stop_token = {}) {
    if (stop_token.stop_requested() || task.cancellation.is_cancellation_requested()) {
        return cancelled();
    }
    try {
        return task.execute(TaskContext{task.cancellation, task.user_data, stop_token});
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "executor task threw an unknown exception");
    }
}
}  // namespace

uint64_t allocate_unique_identity() noexcept {
    static std::atomic<uint64_t> next_id{1};
    auto candidate = next_id.load(std::memory_order_relaxed);
    for (;;) {
        // Reserve the maximum as the exhausted sentinel; never wrap to zero.
        if (candidate == (std::numeric_limits<uint64_t>::max)()) std::terminate();
        if (next_id.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed))
            return candidate;
    }
}

ThreadId current_thread_id() noexcept {
    thread_local const ThreadId id{allocate_unique_identity()};
    return id;
}

void yield_current_thread() noexcept {
    std::this_thread::yield();
}

foundation::Result sleep_for(uint64_t duration_nanoseconds, CancellationToken cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return cancelled();
    }
    if (duration_nanoseconds == kWaitIndefinitely) {
        return invalid_argument("sleep_for requires a finite duration");
    }
    if (cancellation.is_valid()) {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        static_cast<void>(condition.wait_for(lock, cancellation.native_stop_token(),
                                             to_duration(duration_nanoseconds), [] { return false; }));
    } else {
        std::this_thread::sleep_for(to_duration(duration_nanoseconds));
    }
    return cancellation.is_cancellation_requested() ? cancelled() : foundation::Result::success();
}

struct CancellationToken::State final {
    struct PropagateStop final {
        std::weak_ptr<State> target{};

        void operator()() const noexcept {
            if (const std::shared_ptr<State> state = target.lock(); state != nullptr) {
                static_cast<void>(state->source.request_stop());
            }
        }
    };

    using ParentStopCallback = std::stop_callback<PropagateStop>;

    std::stop_source source{};
    std::unique_ptr<ParentStopCallback> parent_a_callback{};
    std::unique_ptr<ParentStopCallback> parent_b_callback{};
};

CancellationToken::CancellationToken(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

bool CancellationToken::is_valid() const noexcept {
    return state_ != nullptr;
}

bool CancellationToken::is_cancellation_requested() const noexcept {
    return state_ != nullptr && state_->source.stop_requested();
}

std::stop_token CancellationToken::native_stop_token() const noexcept {
    return state_ == nullptr ? std::stop_token{} : state_->source.get_token();
}

foundation::ResultValue<CancellationToken> CancellationToken::combine(const CancellationToken& first,
                                                                      const CancellationToken& second) {
    if (!first.is_valid()) {
        return {foundation::Result::success(), second};
    }
    if (!second.is_valid() || first.state_ == second.state_) {
        return {foundation::Result::success(), first};
    }
    try {
        auto combined_state = std::make_shared<State>();
        const std::weak_ptr<State> weak_state = combined_state;
        combined_state->parent_a_callback =
            std::make_unique<State::ParentStopCallback>(first.native_stop_token(), State::PropagateStop{weak_state});
        combined_state->parent_b_callback =
            std::make_unique<State::ParentStopCallback>(second.native_stop_token(), State::PropagateStop{weak_state});
        return {foundation::Result::success(), CancellationToken{std::move(combined_state)}};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("combined cancellation state allocation failed"), {}};
    }
}

CancellationSource::CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

CancellationSource::~CancellationSource() = default;

CancellationSource::CancellationSource(CancellationSource&&) noexcept = default;

CancellationSource& CancellationSource::operator=(CancellationSource&&) noexcept = default;

CancellationToken CancellationSource::token() const noexcept {
    return CancellationToken{state_};
}

bool CancellationSource::cancel() noexcept {
    return state_ != nullptr && state_->source.request_stop();
}

bool CancellationSource::is_cancellation_requested() const noexcept {
    return state_ != nullptr && state_->source.stop_requested();
}

struct Completion::State final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool complete{false};
    foundation::Result result{};
    std::vector<std::function<void(const foundation::Result&)>> continuations{};
};

Completion::Completion(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

Completion::~Completion() = default;

bool Completion::is_valid() const noexcept {
    return state_ != nullptr;
}

bool Completion::is_complete() const noexcept {
    if (state_ == nullptr) {
        return false;
    }
    std::lock_guard lock(state_->mutex);
    return state_->complete;
}

foundation::Result Completion::wait(uint64_t timeout_nanoseconds) const {
    if (state_ == nullptr) {
        return invalid_state("completion is not initialized");
    }

    std::unique_lock lock(state_->mutex);
    if (!state_->complete) {
        if (timeout_nanoseconds == kWaitIndefinitely) {
            state_->condition.wait(lock, [this] { return state_->complete; });
        } else if (!state_->condition.wait_for(lock, to_duration(timeout_nanoseconds),
                                               [this] { return state_->complete; })) {
            return foundation::Result::failure(foundation::ResultCode::kTimeout, "completion wait timed out");
        }
    }
    return state_->result;
}

foundation::Result Completion::result() const {
    if (state_ == nullptr) {
        return invalid_state("completion is not initialized");
    }
    std::lock_guard lock(state_->mutex);
    if (!state_->complete) {
        return foundation::Result::failure(foundation::ResultCode::kNotReady, "completion has not finished");
    }
    return state_->result;
}

Completion::Awaiter Completion::operator co_await() const& {
    return Awaiter{*this};
}

Completion::Awaiter Completion::operator co_await() && {
    return Awaiter{std::move(*this)};
}

bool Completion::register_continuation(std::function<void(const foundation::Result&)> continuation) const {
    if (state_ == nullptr || !continuation) {
        return false;
    }

    std::lock_guard lock(state_->mutex);
    if (state_->complete) {
        return false;
    }
    state_->continuations.push_back(std::move(continuation));
    return true;
}

bool Completion::Awaiter::await_ready() const noexcept {
    return completion_.is_complete();
}

foundation::Result Completion::Awaiter::await_resume() const {
    return completion_.result();
}

CompletionSource::CompletionSource() : state_(std::make_shared<Completion::State>()) {}

CompletionSource::~CompletionSource() = default;

CompletionSource::CompletionSource(CompletionSource&&) noexcept = default;

CompletionSource& CompletionSource::operator=(CompletionSource&&) noexcept = default;

Completion CompletionSource::completion() const noexcept {
    return Completion{state_};
}

bool CompletionSource::is_complete() const noexcept {
    return state_ != nullptr && completion().is_complete();
}

foundation::Result CompletionSource::complete(foundation::Result result) {
    if (state_ == nullptr) {
        return invalid_state("completion source is not initialized");
    }

    const std::shared_ptr<Completion::State> state = state_;
    std::vector<std::function<void(const foundation::Result&)>> continuations;
    {
        std::lock_guard lock(state->mutex);
        if (state->complete) {
            return invalid_state("completion source has already completed");
        }
        state->result = std::move(result);
        state->complete = true;
        continuations.swap(state->continuations);
    }
    state->condition.notify_all();
    for (std::function<void(const foundation::Result&)>& continuation : continuations) {
        continuation(state->result);
    }
    return foundation::Result::success();
}

struct CoroutineTask::State final : std::enable_shared_from_this<CoroutineTask::State> {
    using Handle = std::coroutine_handle<promise_type>;

    struct RequestCancellation final {
        std::weak_ptr<State> target{};

        void operator()() const noexcept {
            if (const std::shared_ptr<State> state = target.lock(); state != nullptr) {
                state->request_cancellation();
            }
        }
    };

    using CancellationCallback = std::stop_callback<RequestCancellation>;

    explicit State(Handle coroutine_handle) noexcept : coroutine(coroutine_handle) {}

    ~State() {
        if (coroutine) {
            if (!coroutine.promise().completion().is_complete()) {
                coroutine.promise().publish_completion(cancelled());
            }
            coroutine.destroy();
        }
    }

    [[nodiscard]] foundation::ResultValue<Completion> schedule(Executor& target_executor, TaskPriority task_priority,
                                                               CancellationToken task_cancellation) {
        if (!coroutine) {
            return {invalid_state("coroutine task has no coroutine state"), {}};
        }
        const Completion task_completion = coroutine.promise().completion();
        {
            std::lock_guard lock(mutex);
            if (scheduled) {
                return {invalid_state("coroutine task has already been scheduled"), {}};
            }
            scheduled = true;
            cancellation = std::move(task_cancellation);
            keep_alive = shared_from_this();
        }

        std::unique_ptr<CancellationCallback> callback{};
        try {
            if (cancellation.is_valid()) {
                callback = std::make_unique<CancellationCallback>(cancellation.native_stop_token(),
                                                                  RequestCancellation{weak_from_this()});
            }
        } catch (const std::bad_alloc&) {
            const foundation::Result result = out_of_memory("coroutine cancellation callback allocation failed");
            complete_without_resume(result);
            return {result, {}};
        }

        {
            std::lock_guard lock(mutex);
            if (!completed) {
                cancellation_callback = std::move(callback);
            }
        }
        if (task_completion.is_complete()) {
            return {foundation::Result::success(), task_completion};
        }

        TaskDesc resume_task{};
        resume_task.domain = target_executor.domain();
        resume_task.priority = task_priority;
        resume_task.cancellation = cancellation;
        const std::shared_ptr<State> state = shared_from_this();
        resume_task.execute = [state](const TaskContext& context) {
            if (context.is_stop_requested()) {
                state->request_cancellation();
                return foundation::Result::success();
            }
            if (const foundation::Result result = state->bind_executor_stop_token(context.stop_token); !result) {
                state->complete_without_resume(result);
                return result;
            }
            state->resume();
            return foundation::Result::success();
        };

        const foundation::ResultValue<Completion> submitted = target_executor.submit(std::move(resume_task));
        if (!submitted) {
            complete_without_resume(submitted.result);
            return {submitted.result, {}};
        }
        if (const foundation::Result observed = observe_dispatch(submitted.value); !observed) {
            complete_without_resume(observed);
            return {observed, {}};
        }
        return {foundation::Result::success(), task_completion};
    }

    [[nodiscard]] foundation::Result observe_dispatch(const Completion& dispatch_completion) {
        const std::weak_ptr<State> weak_state = weak_from_this();
        try {
            if (dispatch_completion.register_continuation([weak_state](const foundation::Result& result) noexcept {
                    if (const std::shared_ptr<State> state = weak_state.lock(); state != nullptr && !result) {
                        state->complete_without_resume(result);
                    }
                })) {
                return foundation::Result::success();
            }
        } catch (const std::bad_alloc&) {
            return out_of_memory("coroutine dispatch continuation allocation failed");
        } catch (const std::exception& exception) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
        } catch (...) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "coroutine dispatch continuation allocation failed");
        }

        const foundation::Result result = dispatch_completion.result();
        if (result.code == foundation::ResultCode::kNotReady) {
            return invalid_state("coroutine dispatch completion changed without publishing a result");
        }
        if (!result) {
            complete_without_resume(result);
        }
        return foundation::Result::success();
    }

    [[nodiscard]] foundation::Result bind_executor_stop_token(std::stop_token stop_token) {
        if (!stop_token.stop_possible()) {
            return foundation::Result::success();
        }

        std::unique_ptr<CancellationCallback> callback;
        try {
            callback = std::make_unique<CancellationCallback>(stop_token, RequestCancellation{weak_from_this()});
        } catch (const std::bad_alloc&) {
            return out_of_memory("coroutine executor stop callback allocation failed");
        }

        {
            std::lock_guard lock(mutex);
            if (!completed && executor_stop_callback == nullptr) {
                executor_stop_callback = std::move(callback);
            }
        }
        return foundation::Result::success();
    }

    void resume() noexcept {
        {
            std::lock_guard lock(mutex);
            if (completed) {
                return;
            }
            if (running) {
                resume_pending = true;
                return;
            }
            if (cancellation_requested || cancellation.is_cancellation_requested()) {
                cancellation_requested = true;
            } else {
                running = true;
            }
        }

        while (true) {
            bool should_cancel = false;
            {
                std::lock_guard lock(mutex);
                should_cancel = cancellation_requested && !running;
            }
            if (should_cancel) {
                complete_without_resume(cancelled());
                return;
            }

            coroutine.resume();
            if (coroutine.done()) {
                finish(std::move(coroutine.promise().result_));
                return;
            }

            bool resume_again = false;
            {
                std::lock_guard lock(mutex);
                if (cancellation_requested || cancellation.is_cancellation_requested()) {
                    cancellation_requested = true;
                    running = false;
                    should_cancel = true;
                } else if (resume_pending) {
                    resume_pending = false;
                    resume_again = true;
                } else {
                    running = false;
                }
            }
            if (should_cancel) {
                complete_without_resume(cancelled());
                return;
            }
            if (!resume_again) {
                return;
            }
        }
    }

    void request_cancellation() noexcept {
        bool cancel_suspended = false;
        {
            std::lock_guard lock(mutex);
            if (completed) {
                return;
            }
            cancellation_requested = true;
            cancel_suspended = scheduled && !running;
        }
        if (cancel_suspended) {
            complete_without_resume(cancelled());
        }
    }

    void complete_without_resume(foundation::Result result) noexcept {
        {
            std::lock_guard lock(mutex);
            if (completed || running) {
                return;
            }
            completed = true;
        }
        coroutine.promise().publish_completion(std::move(result));
        release_keep_alive();
    }

    void finish(foundation::Result result) noexcept {
        {
            std::lock_guard lock(mutex);
            if (completed) {
                return;
            }
            completed = true;
            running = false;
        }
        coroutine.promise().publish_completion(std::move(result));
        release_keep_alive();
    }

    void release_keep_alive() noexcept {
        std::shared_ptr<State> lifetime_guard{};
        {
            std::lock_guard lock(mutex);
            lifetime_guard = std::move(keep_alive);
        }
    }

    std::mutex mutex{};
    Handle coroutine{};
    CancellationToken cancellation{};
    std::unique_ptr<CancellationCallback> cancellation_callback{};
    std::unique_ptr<CancellationCallback> executor_stop_callback{};
    std::shared_ptr<State> keep_alive{};
    bool scheduled{false};
    bool running{false};
    bool resume_pending{false};
    bool completed{false};
    bool cancellation_requested{false};
};

CoroutineTask CoroutineTask::promise_type::get_return_object() {
    const auto coroutine = std::coroutine_handle<promise_type>::from_promise(*this);
    auto state = std::make_shared<State>(coroutine);
    state_ = state;
    return CoroutineTask{std::move(state)};
}

std::suspend_always CoroutineTask::promise_type::initial_suspend() const noexcept {
    return {};
}

std::suspend_always CoroutineTask::promise_type::final_suspend() const noexcept {
    return {};
}

void CoroutineTask::promise_type::return_value(foundation::Result result) noexcept {
    result_ = std::move(result);
}

void CoroutineTask::promise_type::unhandled_exception() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        result_ = out_of_memory("coroutine task allocation failed");
    } catch (const std::exception& exception) {
        result_ = foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    } catch (...) {
        result_ =
            foundation::Result::failure(foundation::ResultCode::kUnknown, "coroutine task threw an unknown exception");
    }
}

Completion CoroutineTask::promise_type::completion() const noexcept {
    return completion_source_.completion();
}

void CoroutineTask::promise_type::publish_completion(foundation::Result result) noexcept {
    static_cast<void>(completion_source_.complete(std::move(result)));
}

std::function<void(const foundation::Result&)> CoroutineTask::promise_type::make_completion_continuation(
    std::coroutine_handle<promise_type> coroutine) const {
    const std::weak_ptr<State> weak_state = state_;
    return [weak_state, coroutine](const foundation::Result&) noexcept {
        const std::shared_ptr<State> state = weak_state.lock();
        if (state == nullptr) {
            return;
        }
        static_cast<void>(coroutine);
        state->resume();
    };
}

CoroutineTask::CoroutineTask(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

CoroutineTask::~CoroutineTask() = default;

CoroutineTask::CoroutineTask(CoroutineTask&&) noexcept = default;

CoroutineTask& CoroutineTask::operator=(CoroutineTask&&) noexcept = default;

bool CoroutineTask::is_valid() const noexcept {
    return state_ != nullptr;
}

Completion CoroutineTask::completion() const noexcept {
    return state_ == nullptr || !state_->coroutine ? Completion{} : state_->coroutine.promise().completion();
}

foundation::ResultValue<Completion> CoroutineTask::schedule(Executor& executor, TaskPriority priority,
                                                            CancellationToken cancellation) {
    if (state_ == nullptr) {
        return {invalid_state("coroutine task is not initialized"), {}};
    }
    return state_->schedule(executor, priority, std::move(cancellation));
}

struct WorkerExecutor::Impl final {
    struct QueuedTask final {
        TaskDesc task{};
        CompletionSource source{};
        uint64_t sequence{0};
    };

    struct QueueCompare final {
        [[nodiscard]] bool operator()(const std::shared_ptr<QueuedTask>& left,
                                      const std::shared_ptr<QueuedTask>& right) const noexcept {
            const auto left_priority = static_cast<uint8_t>(left->task.priority);
            const auto right_priority = static_cast<uint8_t>(right->task.priority);
            if (left_priority != right_priority) {
                return left_priority < right_priority;
            }
            return left->sequence > right->sequence;
        }
    };

    explicit Impl(const WorkerExecutorDesc& executor_desc)
        : domain(executor_desc.domain), deterministic(executor_desc.deterministic) {
        if (!is_valid_domain(domain) || domain == ExecutorDomain::kMainThread) {
            throw std::invalid_argument("worker executor requires a worker execution domain");
        }

        const uint32_t hardware_count = std::max(1u, std::thread::hardware_concurrency());
        const uint32_t requested_count = executor_desc.worker_count == 0
                                             ? (domain == ExecutorDomain::kCpuWorker ? hardware_count : 1u)
                                             : executor_desc.worker_count;
        const uint32_t resolved_count = deterministic ? 1u : std::max(1u, requested_count);
        workers.reserve(resolved_count);
        try {
            for (uint32_t index = 0; index < resolved_count; ++index) {
                workers.emplace_back([this](std::stop_token stop_token) { worker_loop(stop_token); });
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    ~Impl() { stop(); }

    void stop() noexcept {
        std::vector<std::shared_ptr<QueuedTask>> pending_tasks;
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            stopping = true;
            while (!queue.empty()) {
                pending_tasks.push_back(queue.top());
                queue.pop();
            }
        }

        for (std::jthread& worker : workers) {
            static_cast<void>(worker.request_stop());
        }
        for (const std::shared_ptr<QueuedTask>& pending_task : pending_tasks) {
            static_cast<void>(pending_task->source.complete(cancelled()));
        }
        work_available.notify_all();
        idle.notify_all();
        for (std::jthread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void worker_loop(std::stop_token stop_token) {
        while (true) {
            std::shared_ptr<QueuedTask> queued_task;
            {
                std::unique_lock lock(mutex);
                work_available.wait(lock, stop_token, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    if (stopping || stop_token.stop_requested()) {
                        return;
                    }
                    continue;
                }
                queued_task = queue.top();
                queue.pop();
                ++active_count;
            }

            const foundation::Result result = execute_task(queued_task->task, stop_token);
            static_cast<void>(queued_task->source.complete(result));

            {
                std::lock_guard lock(mutex);
                --active_count;
                if (queue.empty() && active_count == 0) {
                    idle.notify_all();
                }
            }
        }
    }

    ExecutorDomain domain{ExecutorDomain::kCpuWorker};
    bool deterministic{false};
    mutable std::mutex mutex;
    std::condition_variable_any work_available;
    std::condition_variable idle;
    std::priority_queue<std::shared_ptr<QueuedTask>, std::vector<std::shared_ptr<QueuedTask>>, QueueCompare> queue;
    std::vector<std::jthread> workers;
    uint64_t next_sequence{0};
    uint32_t active_count{0};
    bool stopping{false};
};

WorkerExecutor::WorkerExecutor(const WorkerExecutorDesc& desc) : impl_(std::make_unique<Impl>(desc)) {}

WorkerExecutor::~WorkerExecutor() = default;

ExecutorDomain WorkerExecutor::domain() const noexcept {
    return impl_ == nullptr ? ExecutorDomain::kCpuWorker : impl_->domain;
}

uint32_t WorkerExecutor::worker_count() const noexcept {
    return impl_ == nullptr ? 0 : static_cast<uint32_t>(impl_->workers.size());
}

foundation::ResultValue<Completion> WorkerExecutor::submit(TaskDesc task) {
    if (impl_ == nullptr) {
        return {invalid_state("worker executor is not initialized"), {}};
    }
    if (!task.execute) {
        return {invalid_argument("executor task callback is empty"), {}};
    }

    CompletionSource source;
    Completion completion = source.completion();
    if (task.cancellation.is_cancellation_requested()) {
        static_cast<void>(source.complete(cancelled()));
        return {foundation::Result::success(), std::move(completion)};
    }

    try {
        auto queued_task = std::make_shared<Impl::QueuedTask>();
        queued_task->task = std::move(task);
        queued_task->source = std::move(source);
        bool executor_is_stopping = false;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->stopping) {
                executor_is_stopping = true;
            } else {
                queued_task->sequence = impl_->next_sequence++;
                impl_->queue.push(std::move(queued_task));
            }
        }
        if (executor_is_stopping) {
            static_cast<void>(queued_task->source.complete(invalid_state("worker executor is stopping")));
            return {invalid_state("worker executor is stopping"), {}};
        }
    } catch (const std::bad_alloc&) {
        return {out_of_memory("executor task submission allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what()), {}};
    }
    impl_->work_available.notify_one();
    return {foundation::Result::success(), std::move(completion)};
}

foundation::Result WorkerExecutor::wait_idle(uint64_t timeout_nanoseconds) {
    if (impl_ == nullptr) {
        return invalid_state("worker executor is not initialized");
    }
    std::unique_lock lock(impl_->mutex);
    const auto idle_predicate = [this] { return impl_->queue.empty() && impl_->active_count == 0; };
    if (idle_predicate()) {
        return foundation::Result::success();
    }
    if (timeout_nanoseconds == kWaitIndefinitely) {
        impl_->idle.wait(lock, idle_predicate);
        return foundation::Result::success();
    }
    if (!impl_->idle.wait_for(lock, to_duration(timeout_nanoseconds), idle_predicate)) {
        return foundation::Result::failure(foundation::ResultCode::kTimeout, "worker executor did not become idle");
    }
    return foundation::Result::success();
}

foundation::ResultValue<Completion> InlineExecutor::submit(TaskDesc task) {
    if (!task.execute) {
        return {invalid_argument("executor task callback is empty"), {}};
    }
    CompletionSource source;
    Completion completion = source.completion();
    const foundation::Result result = execute_task(task);
    static_cast<void>(source.complete(result));
    return {foundation::Result::success(), std::move(completion)};
}

foundation::Result InlineExecutor::wait_idle(uint64_t) {
    return foundation::Result::success();
}

struct ExecutorRegistry::Impl final {
    mutable std::mutex mutex;
    std::array<std::shared_ptr<Executor>, 5> executors{};
};

ExecutorRegistry::ExecutorRegistry() : impl_(std::make_unique<Impl>()) {
    impl_->executors[domain_index(ExecutorDomain::kMainThread)] = std::make_shared<InlineExecutor>();
}

ExecutorRegistry::~ExecutorRegistry() = default;

foundation::Result ExecutorRegistry::register_executor(std::shared_ptr<Executor> executor, bool replace_existing) {
    if (impl_ == nullptr || executor == nullptr) {
        return invalid_argument("executor registry received an empty executor");
    }
    const ExecutorDomain domain = executor->domain();
    if (!is_valid_domain(domain)) {
        return invalid_argument("executor domain is invalid");
    }

    std::lock_guard lock(impl_->mutex);
    std::shared_ptr<Executor>& slot = impl_->executors[domain_index(domain)];
    if (slot != nullptr && !replace_existing) {
        return invalid_state("executor domain is already registered");
    }
    slot = std::move(executor);
    return foundation::Result::success();
}

foundation::Result ExecutorRegistry::unregister_executor(ExecutorDomain domain, uint64_t timeout_nanoseconds) {
    if (impl_ == nullptr || !is_valid_domain(domain)) {
        return invalid_argument("executor domain is invalid");
    }

    std::shared_ptr<Executor> executor;
    {
        std::lock_guard lock(impl_->mutex);
        executor = impl_->executors[domain_index(domain)];
    }
    if (executor == nullptr) {
        return not_found("executor domain is not registered");
    }
    if (const foundation::Result result = executor->wait_idle(timeout_nanoseconds); !result) {
        return result;
    }

    std::lock_guard lock(impl_->mutex);
    std::shared_ptr<Executor>& slot = impl_->executors[domain_index(domain)];
    if (slot == executor) {
        slot.reset();
        return foundation::Result::success();
    }
    return invalid_state("executor domain changed while unregistering");
}

foundation::ResultValue<std::shared_ptr<Executor>> ExecutorRegistry::find_executor(ExecutorDomain domain) const {
    if (impl_ == nullptr || !is_valid_domain(domain)) {
        return {invalid_argument("executor domain is invalid"), {}};
    }
    std::lock_guard lock(impl_->mutex);
    const std::shared_ptr<Executor>& executor = impl_->executors[domain_index(domain)];
    if (executor == nullptr) {
        return {not_found("executor domain is not registered"), {}};
    }
    return {foundation::Result::success(), executor};
}

foundation::ResultValue<Completion> ExecutorRegistry::submit(ExecutorDomain domain, TaskDesc task) const {
    const foundation::ResultValue<std::shared_ptr<Executor>> executor = find_executor(domain);
    if (!executor) {
        return {executor.result, {}};
    }
    return executor.value->submit(std::move(task));
}

foundation::Result ExecutorRegistry::wait_idle(uint64_t timeout_nanoseconds) const {
    if (impl_ == nullptr) {
        return invalid_state("executor registry is not initialized");
    }

    std::array<std::shared_ptr<Executor>, 5> executors;
    {
        std::lock_guard lock(impl_->mutex);
        executors = impl_->executors;
    }

    if (timeout_nanoseconds == kWaitIndefinitely) {
        for (const std::shared_ptr<Executor>& executor : executors) {
            if (executor != nullptr) {
                if (const foundation::Result result = executor->wait_idle(); !result) {
                    return result;
                }
            }
        }
        return foundation::Result::success();
    }

    const auto deadline = std::chrono::steady_clock::now() + to_duration(timeout_nanoseconds);
    for (const std::shared_ptr<Executor>& executor : executors) {
        if (executor == nullptr) {
            continue;
        }
        if (const foundation::Result result = executor->wait_idle(remaining_timeout_nanoseconds(deadline)); !result) {
            return result;
        }
    }
    return foundation::Result::success();
}

struct TaskGroup::Impl final {
    mutable std::mutex mutex;
    std::vector<Completion> completions{};
};

TaskGroup::TaskGroup(CancellationToken parent) : parent_token_(std::move(parent)), impl_(std::make_unique<Impl>()) {}

TaskGroup::~TaskGroup() {
    static_cast<void>(cancel());
    static_cast<void>(wait());
}

CancellationToken TaskGroup::token() const noexcept {
    return cancellation_source_.token();
}

bool TaskGroup::cancel() noexcept {
    return cancellation_source_.cancel();
}

foundation::ResultValue<Completion> TaskGroup::submit(Executor& executor, TaskDesc task) {
    if (impl_ == nullptr) {
        return {invalid_state("task group is not initialized"), {}};
    }
    if (!task.execute) {
        return {invalid_argument("executor task callback is empty"), {}};
    }

    const foundation::ResultValue<CancellationToken> group_token =
        CancellationToken::combine(cancellation_source_.token(), task.cancellation);
    if (!group_token) {
        return {group_token.result, {}};
    }
    const foundation::ResultValue<CancellationToken> combined_token =
        CancellationToken::combine(parent_token_, group_token.value);
    if (!combined_token) {
        return {combined_token.result, {}};
    }
    task.cancellation = combined_token.value;

    TaskFunction function = std::move(task.execute);
    task.execute = [this, function = std::move(function)](const TaskContext& context) mutable {
        const foundation::Result result = function(context);
        if (!result) {
            static_cast<void>(cancellation_source_.cancel());
        }
        return result;
    };

    foundation::ResultValue<Completion> submitted = executor.submit(std::move(task));
    if (!submitted) {
        return submitted;
    }
    try {
        std::lock_guard lock(impl_->mutex);
        impl_->completions.push_back(submitted.value);
    } catch (const std::bad_alloc&) {
        static_cast<void>(cancel());
        static_cast<void>(submitted.value.wait());
        return {out_of_memory("task group completion tracking allocation failed"), {}};
    }
    return submitted;
}

foundation::Result TaskGroup::wait(uint64_t timeout_nanoseconds) {
    if (impl_ == nullptr) {
        return invalid_state("task group is not initialized");
    }

    std::vector<Completion> completions;
    try {
        std::lock_guard lock(impl_->mutex);
        completions = impl_->completions;
    } catch (const std::bad_alloc&) {
        return out_of_memory("task group wait allocation failed");
    }

    foundation::Result first_failure = foundation::Result::success();
    bool saw_cancellation = false;
    if (timeout_nanoseconds == kWaitIndefinitely) {
        for (const Completion& completion : completions) {
            const foundation::Result result = completion.wait();
            if (!result) {
                if (result.code == foundation::ResultCode::kCancelled) {
                    saw_cancellation = true;
                } else if (first_failure) {
                    first_failure = result;
                }
            }
        }
    } else {
        const auto deadline = std::chrono::steady_clock::now() + to_duration(timeout_nanoseconds);
        for (const Completion& completion : completions) {
            const foundation::Result result = completion.wait(remaining_timeout_nanoseconds(deadline));
            if (!result) {
                if (result.code == foundation::ResultCode::kTimeout) {
                    return result;
                }
                if (result.code == foundation::ResultCode::kCancelled) {
                    saw_cancellation = true;
                } else if (first_failure) {
                    first_failure = result;
                }
            }
        }
    }

    if (!first_failure) {
        return first_failure;
    }
    return saw_cancellation ? cancelled() : foundation::Result::success();
}

std::size_t TaskGroup::size() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->completions.size();
}

struct TaskGraph::Impl final {
    struct Node final {
        TaskDesc task{};
        std::vector<uint32_t> successors{};
        uint32_t indegree{0};
    };

    mutable std::mutex mutex;
    std::vector<Node> nodes{};
};

TaskGraph::TaskGraph() : impl_(std::make_unique<Impl>()) {}

TaskGraph::~TaskGraph() = default;

foundation::ResultValue<TaskHandle> TaskGraph::add_task(TaskDesc task) {
    if (impl_ == nullptr) {
        return {invalid_state("task graph is not initialized"), {}};
    }
    if (!task.execute || !is_valid_domain(task.domain)) {
        return {invalid_argument("task graph task has an invalid callback or domain"), {}};
    }
    try {
        std::lock_guard lock(impl_->mutex);
        const uint32_t index = static_cast<uint32_t>(impl_->nodes.size());
        if (index == kInvalidTaskIndex) {
            return {out_of_memory("task graph exceeded the task index limit"), {}};
        }
        impl_->nodes.push_back(Impl::Node{std::move(task), {}, 0});
        return {foundation::Result::success(), TaskHandle{index}};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("task graph task allocation failed"), {}};
    }
}

foundation::Result TaskGraph::add_dependency(TaskHandle prerequisite, TaskHandle dependent) {
    if (impl_ == nullptr) {
        return invalid_state("task graph is not initialized");
    }
    std::lock_guard lock(impl_->mutex);
    if (!prerequisite.is_valid() || !dependent.is_valid() || prerequisite.index >= impl_->nodes.size() ||
        dependent.index >= impl_->nodes.size()) {
        return invalid_argument("task graph dependency references an invalid task");
    }
    if (prerequisite == dependent) {
        return invalid_argument("task graph task cannot depend on itself");
    }
    std::vector<uint32_t>& successors = impl_->nodes[prerequisite.index].successors;
    if (std::find(successors.begin(), successors.end(), dependent.index) != successors.end()) {
        return invalid_state("task graph dependency is already declared");
    }
    try {
        successors.push_back(dependent.index);
        ++impl_->nodes[dependent.index].indegree;
    } catch (const std::bad_alloc&) {
        return out_of_memory("task graph dependency allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result TaskGraph::validate() const {
    if (impl_ == nullptr) {
        return invalid_state("task graph is not initialized");
    }

    std::lock_guard lock(impl_->mutex);
    std::vector<uint32_t> indegrees;
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
    try {
        indegrees.reserve(impl_->nodes.size());
        for (uint32_t index = 0; index < impl_->nodes.size(); ++index) {
            const Impl::Node& node = impl_->nodes[index];
            if (!node.task.execute || !is_valid_domain(node.task.domain)) {
                return invalid_argument("task graph contains an invalid task");
            }
            indegrees.push_back(node.indegree);
            if (node.indegree == 0) {
                ready.push(index);
            }
        }
        std::size_t processed = 0;
        while (!ready.empty()) {
            const uint32_t index = ready.top();
            ready.pop();
            ++processed;
            for (const uint32_t successor : impl_->nodes[index].successors) {
                if (successor >= indegrees.size() || indegrees[successor] == 0) {
                    return invalid_state("task graph dependency indegree is inconsistent");
                }
                if (--indegrees[successor] == 0) {
                    ready.push(successor);
                }
            }
        }
        if (processed != impl_->nodes.size()) {
            return invalid_argument("task graph contains a dependency cycle");
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("task graph validation allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result TaskGraph::execute(const ExecutorRegistry& registry, CancellationToken cancellation) const {
    if (impl_ == nullptr) {
        return invalid_state("task graph is not initialized");
    }
    if (const foundation::Result result = validate(); !result) {
        return result;
    }

    std::vector<Impl::Node> nodes;
    try {
        std::lock_guard lock(impl_->mutex);
        nodes = impl_->nodes;
    } catch (const std::bad_alloc&) {
        return out_of_memory("task graph execution snapshot allocation failed");
    }
    if (nodes.empty()) {
        return foundation::Result::success();
    }
    if (cancellation.is_cancellation_requested()) {
        return cancelled();
    }

    std::vector<uint32_t> indegrees;
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
    try {
        indegrees.reserve(nodes.size());
        for (uint32_t index = 0; index < nodes.size(); ++index) {
            indegrees.push_back(nodes[index].indegree);
            if (nodes[index].indegree == 0) {
                ready.push(index);
            }
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("task graph execution state allocation failed");
    }

    std::size_t processed = 0;
    while (!ready.empty()) {
        if (cancellation.is_cancellation_requested()) {
            return cancelled();
        }
        TaskGroup group(cancellation);
        std::vector<uint32_t> current_layer;
        try {
            current_layer.reserve(ready.size());
            while (!ready.empty()) {
                current_layer.push_back(ready.top());
                ready.pop();
            }
        } catch (const std::bad_alloc&) {
            return out_of_memory("task graph layer allocation failed");
        }
        for (const uint32_t index : current_layer) {
            const foundation::ResultValue<std::shared_ptr<Executor>> executor =
                registry.find_executor(nodes[index].task.domain);
            if (!executor) {
                static_cast<void>(group.cancel());
                static_cast<void>(group.wait());
                return executor.result;
            }
            if (const foundation::ResultValue<Completion> submitted = group.submit(*executor.value, nodes[index].task);
                !submitted) {
                static_cast<void>(group.cancel());
                static_cast<void>(group.wait());
                return submitted.result;
            }
        }
        if (const foundation::Result result = group.wait(); !result) {
            return result;
        }

        try {
            for (const uint32_t index : current_layer) {
                ++processed;
                for (const uint32_t successor : nodes[index].successors) {
                    if (successor >= indegrees.size() || indegrees[successor] == 0) {
                        return invalid_state("task graph execution indegree is inconsistent");
                    }
                    if (--indegrees[successor] == 0) {
                        ready.push(successor);
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            return out_of_memory("task graph next-layer allocation failed");
        }
    }

    return processed == nodes.size() ? foundation::Result::success()
                                     : invalid_argument("task graph contains a dependency cycle");
}

std::size_t TaskGraph::task_count() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->nodes.size();
}
}  // namespace aeg::concurrency
