# Runtime

## 目录职责

`Runtime` 负责把引擎生命周期、Pipeline Runtime 和帧内 RenderGraph 连接成统一的
执行入口。它是 Engine Scope 与 Frame Scope 之间的运行时编排层。

## 当前实现

当前目录包含 `EngineFrameOrchestrator`，负责 Phase Registry 同步、Pipeline Plan
固定、Frame Begin/End、RenderBuild、RenderExecute、Present、Abort 和 Shutdown。

## 依赖边界

Runtime 可以依赖 Foundation、Pipeline 和 RenderGraph，但不应依赖具体 RHI 后端。它
负责调用这些模块的公共接口，不应重新实现 Pipeline 编译或 RenderGraph Barrier 分析。
