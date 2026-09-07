# Concurrency

## 目录职责

该目录承载引擎级并发基础设施，负责将逻辑执行域映射到可替换的执行器，并提供
任务提交、完成传播、协作式取消和批次收敛能力。Pipeline、World、Resource 和
Renderer 可以依赖这些原语，但不得在各自模块内重复实现线程池或完成状态。

## 当前状态

当前提供以下公共类型：

- `CancellationSource` / `CancellationToken`：发布和观察协作式取消请求。
- `CompletionSource` / `Completion`：以 `Result` 表示异步任务的最终结果。
- `CoroutineTask`：可由 `Executor` 启动、可直接 `co_await Completion` 的惰性协程任务。
- `Executor`：执行器抽象；实现可以是 Worker、Render Thread 或 RHI 提交适配器。
- `WorkerExecutor`：带优先级队列和专属 `std::jthread` 工作线程的通用执行器。
- `InlineExecutor`：同步执行器，用于主线程路径和确定性测试。
- `ExecutorRegistry`：线程安全的逻辑执行域到执行器映射。
- `TaskGroup`：批量等待、首个失败传播和失败后的协作式取消。
- `TaskGraph`：基于显式前驱依赖的通用拓扑任务图；同一拓扑层可跨执行域并行提交。

`PipelineScheduler` 已经通过 `ExecutorRegistry` 使用本目录的执行器，不再持有
Pipeline 私有线程池，也不再以调度器级全局互斥锁串行化不同的 Pipeline 执行。
默认的 `kRenderThread` 和 `kGpu` 执行器仍是 CPU 侧编排入口；真正的 GPU 指令和
`GpuSyncPoint` 必须由 RHI/RenderGraph 的 Queue Submission 产生。

## 边界

本目录不定义游戏业务任务，不持有 World 或 RenderGraph 的领域状态，也不依赖
具体图形后端。`ExecutorDomain::kGpu` 只表示 GPU 编排/提交执行域，不意味着存在
可以运行 C++ 函数的 GPU 线程。

### 生命周期语义

- `Completion` 可复制，只读观察同一个共享完成状态；未完成时 `result()` 返回
  `kNotReady`。
- `Completion::wait()` 超时返回 `kTimeout`，不会取消任务。
- `CancellationToken` 基于 `std::stop_source` / `std::stop_token`。执行器只保证尚未
  开始的任务在取消后不再调用回调；已经运行的普通任务必须周期性调用
  `TaskContext::is_stop_requested()`。
- `WorkerExecutor` 析构时向全部 `std::jthread` 请求停止，取消尚未开始的任务，并等待
  正在执行的任务完成协作式收敛。
- `TaskGroup` 的析构会请求取消并等待已提交任务收敛，调用方不得让任务回调反向等待
  正在析构的 `TaskGroup`。

### 协程语义

- `CoroutineTask` 采用惰性启动模型：创建协程不会执行函数体，首次调用 `schedule()`
  后才通过目标 `Executor` 开始执行。
- 每个 `CoroutineTask` 只能调度一次。任务完成后，`schedule()` 返回的 `Completion`
  保存协程最终 `Result`；被等待的 `Completion` 失败时，该失败结果由协程显式传播。
- `co_await Completion` 仅登记延续并挂起协程，不占用 Worker，也不会创建监视线程。
- 当前延续在完成 `CompletionSource` 的线程上恢复。因此，`co_await` 后的代码不得假定
  仍具有原 Worker 或 Render Thread 的线程亲和性；需要亲和性的任务应在恢复后显式提交
  到对应 `Executor`。
- 显式 `CancellationToken` 与首次执行协程的 `std::jthread` 停止请求都会终止处于挂起
  状态的协程。协程正在运行时仍采用协作式语义，在下一次挂起或返回时收敛。
- 未调度的 `CoroutineTask` 在析构时不会执行函数体，并以 `kCancelled` 完成其
  `Completion`；已经调度的任务由内部状态保持存活，调用方可以只保留 `Completion`。

### 线程安全语义

- `CancellationSource`、`CancellationToken` 和 `Completion` 可跨线程使用。
- `Executor` 的 `submit()` 和 `wait_idle()` 必须支持并发调用，具体实现可以限制自身
  的重入方式。
- `PipelineExecutionContext` 仍然是一次执行的上下文，不得被多个并发 Scheduler
  调用共享；不同执行调用应使用不同 Context。
- `PipelineScheduler::wait_idle()` 是生命周期边界操作：它会阻止新的 Scheduler 执行，
  等待既有执行退出，再等待已注册执行器空闲。
- `TaskGraph` 的拓扑结构在 `execute()` 期间必须保持不变；图可以被重复执行，但每次
  执行都会创建独立的 indegree 和完成状态。
