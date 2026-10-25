# Concurrency

## 目录职责

该目录承载引擎内部并发基础设施，负责将逻辑执行域映射到可替换的执行器，并提供
任务提交、完成传播、协作式取消和批次收敛能力。Runtime、Editor、Tooling、ECS、Pipeline
和 Frame/Render 内部实现可以依赖这些原语，但游戏作者编写的 Feature、Module 和 Runtime
启动代码不属于该 API 的使用者，也不会从 `AEG::GameSDK` 或 `AEG::GameRuntime` 获得它的
头文件或编译使用要求；最终程序仍链接引擎内部实现所需的库。

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
- `ThreadId` / `current_thread_id()`：可跨平台比较的进程内线程身份，用于 owner 亲和性检查；
  不暴露 OS 线程句柄。
- `yield_current_thread()` / `sleep_for()`：统一的让出和阻塞等待入口；`sleep_for()` 支持
  `CancellationToken`，被取消时返回 `kCancelled`。它是线程阻塞工具，不是协程定时器。

`PipelineScheduler` 已经通过 `ExecutorRegistry` 使用本目录的执行器，不再持有
Pipeline 私有线程池，也不再以调度器级全局互斥锁串行化不同的 Pipeline 执行。
默认的 `kRenderThread` 和 `kGpu` 执行器仍是 CPU 侧编排入口；真正的 GPU 指令和
`GpuSyncPoint` 必须由 RHI/RenderGraph 的 Queue Submission 产生。

`Engine/Feature/private/feature_execution.*` 复用 WorkerExecutor、CancellationSource 和
Completion，按需创建五个独立执行器。Logic/Physics/Audio 使用 CPU lane，Render 使用
RenderThread lane，Stream 使用 IO lane；通用 lane 相同不意味着共享 Domain 所有者。
完成状态发布 worker 的快照结果，由 Runtime 所属线程回收，不增加第二套线程或锁实现。
Feature 与 Runtime 的游戏公共入口不包含 Concurrency 类型或使用要求。

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
- `WorkerExecutor` 必须由任务回调之外的生命周期 owner 持有，并从非 worker 线程释放；
  回调不得销毁或释放最后一个 executor owner，也不得从自身 worker 回调中等待其析构。
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

### 第一方边界

`tests/cmake/concurrency_boundary.cmake` 会扫描引擎拥有的 Engine、Editor、Runtime、Tooling、
Platform、测试和 tools 源码，禁止第一方直接创建标准线程、使用标准异步/协程句柄或调用原生
线程 API。游戏模板和 DemoProject 只接受“不得引用引擎内部 Concurrency 类型”的表面检查，
不会被标准线程实现策略约束。
`Engine/Concurrency` 自身是唯一的标准线程和协程实现边界；第三方日志后端、OS 子进程等待、
GPU 同步和数据保护锁不属于任务执行器的替代范围。
Win32 窗口 focus 所用的 native 线程 ID 继续由 Platform 管理，因为 `AttachThreadInput`
要求 OS 线程身份；它不创建或调度线程。

`allocate_unique_identity()` 复用 `current_thread_id()` 的同一内部原子签发器，供引擎 host 的
Runtime 会话代次等瞬时身份使用。并发签发保持正数且从不复用；保留 `UINT64_MAX` 为
耗尽哨兵，耗尽时终止而不回绕。该身份不持久化、不代表 OS 句柄，也不承担数据发布
同步；任务完成仍使用既有 Completion。此 API 不属于 GameSDK 或游戏作者接口。
