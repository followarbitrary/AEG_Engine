# Runtime

## 目录职责

`Runtime` 负责把引擎生命周期、Pipeline Runtime 和帧内 RenderGraph 连接成统一的
执行入口。它是 Engine Scope 与 Frame Scope 之间的运行时编排层。

## 当前实现

当前目录包含 `EngineFrameOrchestrator`，负责 Phase Registry 同步、Pipeline Plan
固定、Frame Begin/End、RenderBuild、RenderExecute、Present、Abort 和 Shutdown；同时
负责已注册 World 的命令消费、`WorldCommit` 快照发布，以及 World-to-Frame Bridge
的帧内安装与清理。对于绑定 World View 的 Frame Pipeline，
`PipelineExecutionContext::scope_data()` 在执行期间指向该 View 的
`world::WorldFrameScope`；对于 World Pipeline，该字段指向 `world::WorldScope`。
每个 World View 在每帧拥有独立的 `WorldFrameScope` 和 Bridge；Frame 结束时恢复
宿主原有的 Bridge。World Scope 关闭或重新初始化后，旧 Frame Scope 不得重新捕获，
但已捕获的不可变 Snapshot 仍可在其所有权有效期间读取。
`PipelineExecutionContext::user_data()` 始终原样保留 `EngineFrameDesc::user_data`，
不会被 World 或 Frame Scope 覆盖；Task 的 `TaskDescriptor::user_data` 仍通过
`PipelineTaskContext::user_data()` 访问。

## 依赖边界

Runtime 可以依赖 Foundation、Pipeline、World 和 RenderGraph，但不应依赖具体 RHI
后端。它负责调用这些模块的公共接口，不应重新实现 Pipeline 编译或 RenderGraph
Barrier 分析，也不应直接持有 World 的可变容器。
