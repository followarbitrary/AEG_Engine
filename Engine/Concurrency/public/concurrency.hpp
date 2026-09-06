/**
 * @file concurrency.hpp
 * @brief Platform-independent task execution and completion primitives.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "result.hpp"

namespace aeg::concurrency {
inline constexpr uint64_t kWaitIndefinitely = std::numeric_limits<uint64_t>::max();
inline constexpr uint32_t kInvalidTaskIndex = std::numeric_limits<uint32_t>::max();

/** @brief Logical execution lane used to select an executor. */
enum class ExecutorDomain : uint8_t {
    kMainThread = 0,
    kCpuWorker = 1,
    kIoWorker = 2,
    kRenderThread = 3,
    kGpu = 4,
};

/** @brief Scheduling priority for one submitted task. */
enum class TaskPriority : uint8_t {
    kLow = 0,
    kNormal = 1,
    kHigh = 2,
    kCritical = 3,
};

/** @brief Cooperative cancellation state observed by task callbacks. */
class CancellationToken final {
public:
    CancellationToken() noexcept = default;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] bool is_cancellation_requested() const noexcept;
    [[nodiscard]] static foundation::ResultValue<CancellationToken> combine(const CancellationToken& first,
                                                                            const CancellationToken& second);

private:
    struct State;

    explicit CancellationToken(std::shared_ptr<const State> state) noexcept;

    std::shared_ptr<const State> state_{};

    friend class CancellationSource;
    friend class TaskGroup;
};

/** @brief Owns and publishes a cooperative cancellation request. */
class CancellationSource final {
public:
    CancellationSource();
    ~CancellationSource();

    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;
    CancellationSource(CancellationSource&&) noexcept;
    CancellationSource& operator=(CancellationSource&&) noexcept;

    [[nodiscard]] CancellationToken token() const noexcept;
    [[nodiscard]] bool cancel() noexcept;
    [[nodiscard]] bool is_cancellation_requested() const noexcept;

private:
    std::shared_ptr<CancellationToken::State> state_{};
};

/** @brief Read-only view of the completion state of one asynchronous task. */
class Completion final {
public:
    Completion() noexcept = default;
    ~Completion();

    Completion(const Completion&) noexcept = default;
    Completion& operator=(const Completion&) noexcept = default;
    Completion(Completion&&) noexcept = default;
    Completion& operator=(Completion&&) noexcept = default;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] bool is_complete() const noexcept;
    [[nodiscard]] foundation::Result wait(uint64_t timeout_nanoseconds = kWaitIndefinitely) const;
    [[nodiscard]] foundation::Result result() const;

private:
    struct State;

    explicit Completion(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_{};

    friend class CompletionSource;
};

/** @brief Completes a Completion exactly once. */
class CompletionSource final {
public:
    CompletionSource();
    ~CompletionSource();

    CompletionSource(const CompletionSource&) = delete;
    CompletionSource& operator=(const CompletionSource&) = delete;
    CompletionSource(CompletionSource&&) noexcept;
    CompletionSource& operator=(CompletionSource&&) noexcept;

    [[nodiscard]] Completion completion() const noexcept;
    [[nodiscard]] bool is_complete() const noexcept;
    [[nodiscard]] foundation::Result complete(foundation::Result result = foundation::Result::success());

private:
    std::shared_ptr<Completion::State> state_{};
};

/** @brief Context supplied to a task callback by its executor. */
struct TaskContext final {
    CancellationToken cancellation{};
    void* user_data{nullptr};
};

using TaskFunction = std::function<foundation::Result(const TaskContext&)>;

/** @brief Declarative submission data for one executor task. */
struct TaskDesc final {
    TaskFunction execute{};
    ExecutorDomain domain{ExecutorDomain::kMainThread};
    TaskPriority priority{TaskPriority::kNormal};
    CancellationToken cancellation{};
    void* user_data{nullptr};
};

/** @brief Stable index of a task stored in a TaskGraph. */
struct TaskHandle final {
    uint32_t index{kInvalidTaskIndex};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != kInvalidTaskIndex; }

    friend constexpr bool operator==(TaskHandle, TaskHandle) = default;
};

/** @brief Abstract execution resource used by task graphs and schedulers. */
class Executor {
public:
    virtual ~Executor() = default;

    [[nodiscard]] virtual ExecutorDomain domain() const noexcept = 0;
    [[nodiscard]] virtual uint32_t worker_count() const noexcept = 0;
    [[nodiscard]] virtual foundation::ResultValue<Completion> submit(TaskDesc task) = 0;
    [[nodiscard]] virtual foundation::Result wait_idle(uint64_t timeout_nanoseconds = kWaitIndefinitely) = 0;
};

/** @brief Configuration for a worker-backed executor. */
struct WorkerExecutorDesc final {
    ExecutorDomain domain{ExecutorDomain::kCpuWorker};
    uint32_t worker_count{0};
    bool deterministic{false};
};

/** @brief Priority-aware executor backed by a dedicated worker set. */
class WorkerExecutor final : public Executor {
public:
    explicit WorkerExecutor(const WorkerExecutorDesc& desc = {});
    ~WorkerExecutor() override;

    WorkerExecutor(const WorkerExecutor&) = delete;
    WorkerExecutor& operator=(const WorkerExecutor&) = delete;
    WorkerExecutor(WorkerExecutor&&) = delete;
    WorkerExecutor& operator=(WorkerExecutor&&) = delete;

    [[nodiscard]] ExecutorDomain domain() const noexcept override;
    [[nodiscard]] uint32_t worker_count() const noexcept override;
    [[nodiscard]] foundation::ResultValue<Completion> submit(TaskDesc task) override;
    [[nodiscard]] foundation::Result wait_idle(uint64_t timeout_nanoseconds = kWaitIndefinitely) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Synchronous executor useful for main-thread work and deterministic tests. */
class InlineExecutor final : public Executor {
public:
    explicit InlineExecutor(ExecutorDomain domain = ExecutorDomain::kMainThread) noexcept : domain_(domain) {}

    [[nodiscard]] ExecutorDomain domain() const noexcept override { return domain_; }
    [[nodiscard]] uint32_t worker_count() const noexcept override { return 0; }
    [[nodiscard]] foundation::ResultValue<Completion> submit(TaskDesc task) override;
    [[nodiscard]] foundation::Result wait_idle(uint64_t timeout_nanoseconds = kWaitIndefinitely) override;

private:
    ExecutorDomain domain_{ExecutorDomain::kMainThread};
};

/** @brief Thread-safe mapping from logical execution lanes to executors. */
class ExecutorRegistry final {
public:
    ExecutorRegistry();
    ~ExecutorRegistry();

    ExecutorRegistry(const ExecutorRegistry&) = delete;
    ExecutorRegistry& operator=(const ExecutorRegistry&) = delete;
    ExecutorRegistry(ExecutorRegistry&&) = delete;
    ExecutorRegistry& operator=(ExecutorRegistry&&) = delete;

    [[nodiscard]] foundation::Result register_executor(std::shared_ptr<Executor> executor,
                                                       bool replace_existing = false);
    [[nodiscard]] foundation::Result unregister_executor(ExecutorDomain domain,
                                                         uint64_t timeout_nanoseconds = kWaitIndefinitely);
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<Executor>> find_executor(ExecutorDomain domain) const;
    [[nodiscard]] foundation::ResultValue<Completion> submit(ExecutorDomain domain, TaskDesc task) const;
    [[nodiscard]] foundation::Result wait_idle(uint64_t timeout_nanoseconds = kWaitIndefinitely) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Owns a set of submitted tasks and propagates first failure or cancellation. */
class TaskGroup final {
public:
    explicit TaskGroup(CancellationToken parent = {});
    ~TaskGroup();

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;

    [[nodiscard]] CancellationToken token() const noexcept;
    [[nodiscard]] bool cancel() noexcept;
    [[nodiscard]] foundation::ResultValue<Completion> submit(Executor& executor, TaskDesc task);
    [[nodiscard]] foundation::Result wait(uint64_t timeout_nanoseconds = kWaitIndefinitely);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    CancellationSource cancellation_source_{};
    CancellationToken parent_token_{};
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Topological task graph for domain-independent engine work. */
class TaskGraph final {
public:
    TaskGraph();
    ~TaskGraph();

    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;
    TaskGraph(TaskGraph&&) = delete;
    TaskGraph& operator=(TaskGraph&&) = delete;

    [[nodiscard]] foundation::ResultValue<TaskHandle> add_task(TaskDesc task);
    [[nodiscard]] foundation::Result add_dependency(TaskHandle prerequisite, TaskHandle dependent);
    [[nodiscard]] foundation::Result validate() const;
    [[nodiscard]] foundation::Result execute(const ExecutorRegistry& registry,
                                             CancellationToken cancellation = {}) const;
    [[nodiscard]] std::size_t task_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};
}  // namespace aeg::concurrency
