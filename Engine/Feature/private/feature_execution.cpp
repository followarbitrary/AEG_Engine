#include "feature_execution.hpp"

#include <algorithm>
#include <bit>
#include <exception>

namespace aeg::feature {
void FeatureInvocationWork::execute() {
    callback_started.reset();
    callback_finished.reset();
    try {
        const auto start = time_source->read();
        if (!start) { result = foundation::ResultValue<StateDelta>{start.result, {}}; return; }
        const auto gate = validate_transport_timing(timing, start.value);
        if (!gate && (gate.code != foundation::ResultCode::kTimeout ||
            definition.contract.clock.timing->deadline_policy == DeadlinePolicy::kRejectCommit)) {
            result = foundation::ResultValue<StateDelta>{gate, {}}; return;
        }
        for (const auto& message : messages) {
            if (!message.source.timing) {
                result = foundation::ResultValue<StateDelta>{foundation::invalid_argument("input has no host transport timing"), {}};
                return;
            }
            if (const auto valid = validate_transport_timing(*message.source.timing, start.value); !valid) {
                result = foundation::ResultValue<StateDelta>{valid, {}}; return;
            }
        }
        callback_started = start.value;
        result = definition.execute(token, state, input, parameters);
        const auto end = time_source->read();
        if (!end) result = foundation::ResultValue<StateDelta>{end.result, {}};
        else callback_finished = end.value;
    } catch (const std::exception& error) {
        result = foundation::ResultValue<StateDelta>{
            foundation::Result::failure(foundation::ResultCode::kUnknown, error.what()), {}};
    } catch (...) {
        result = foundation::ResultValue<StateDelta>{
            foundation::Result::failure(foundation::ResultCode::kUnknown, "Feature callback threw an exception"), {}};
    }
    if (callback_started && !callback_finished) {
        const auto end = time_source->read();
        if (end) callback_finished = end.value;
        else result = foundation::ResultValue<StateDelta>{end.result, {}};
    }
}

FeatureExecution::~FeatureExecution() { shutdown(); }

bool FeatureExecution::on_owner_thread() const noexcept {
    return owner_ == concurrency::current_thread_id();
}

bool FeatureExecution::busy(FeatureInstanceId instance) const noexcept {
    return std::any_of(pending_.begin(), pending_.end(), [instance](const Pending& value) {
        return value.work->instance == instance;
    });
}

foundation::Result FeatureExecution::submit(std::shared_ptr<FeatureInvocationWork> work) {
    const auto domains = work->definition.contract.domains;
    if (std::popcount(domains) != 1 || busy(work->instance))
        return foundation::invalid_argument("async work requires one leaf domain and no in-flight invocation");
    const auto index = static_cast<size_t>(std::countr_zero(domains));
    if (index >= executors_.size()) return foundation::invalid_argument("async Feature domain is invalid");
    static constexpr std::array lanes{concurrency::ExecutorDomain::kCpuWorker,
                                      concurrency::ExecutorDomain::kCpuWorker,
                                      concurrency::ExecutorDomain::kRenderThread,
                                      concurrency::ExecutorDomain::kCpuWorker,
                                      concurrency::ExecutorDomain::kIoWorker};
    try {
        if (!executors_[index])
            executors_[index] = std::make_unique<concurrency::WorkerExecutor>(
                concurrency::WorkerExecutorDesc{lanes[index], 1, false});
        // Allocate tracking before submit; a successful task is always tracked.
        pending_.push_back(Pending{work});
        concurrency::TaskDesc task{};
        task.domain = lanes[index];
        task.cancellation = pending_.back().cancellation.token();
        task.execute = [work](const concurrency::TaskContext&) {
            work->execute();
            return work->result->result;
        };
        const auto submitted = executors_[index]->submit(std::move(task));
        if (!submitted) {
            pending_.pop_back();
            return submitted.result;
        }
        pending_.back().completion = submitted.value;
        return foundation::Result::success();
    } catch (const std::exception& error) {
        if (!pending_.empty() && pending_.back().work == work && !pending_.back().completion.is_valid())
            pending_.pop_back();
        return foundation::Result::failure(foundation::ResultCode::kUnknown, error.what());
    }
}

std::vector<std::shared_ptr<FeatureInvocationWork>> FeatureExecution::take_ready() {
    std::vector<std::shared_ptr<FeatureInvocationWork>> ready;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (!it->completion.is_complete()) { ++it; continue; }
        // Completion's mutex publishes callback writes before this owner read.
        // A cancelled queued task may never have entered the callback at all.
        if (!it->work->result)
            it->work->result = foundation::ResultValue<StateDelta>{it->completion.result(), {}};
        ready.push_back(it->work);
        it = pending_.erase(it);
    }
    return ready;
}

void FeatureExecution::cancel(FeatureInstanceId instance) noexcept {
    for (auto& pending : pending_)
        if (pending.work->instance == instance) static_cast<void>(pending.cancellation.cancel());
}

void FeatureExecution::cancel_all() noexcept {
    for (auto& pending : pending_) static_cast<void>(pending.cancellation.cancel());
}

void FeatureExecution::shutdown() {
    cancel_all();
    for (auto& executor : executors_)
        if (executor) static_cast<void>(executor->wait_idle());
    pending_.clear();
    for (auto& executor : executors_) executor.reset();
}
}
