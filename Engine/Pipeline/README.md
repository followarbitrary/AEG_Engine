# Pipeline

## 目录职责

`Pipeline` 是 AEG 的领域导向执行框架，负责描述和编译 Pipeline、Feature、Stage、
Task、Artifact 及其依赖，并负责在声明的 ExecutionDomain 上调度运行时节点。

## 当前实现

- `public/pipeline_types.hpp`：公共 ID、枚举、描述符、Plan 和数据模型。
- `public/pipeline.hpp`：Builder、Feature、Registry、Compiler、Scheduler 和 Scope Host。
- `private/pipeline.cpp`：定义收集、拓扑编译、Artifact Runtime 和执行器编排。

## Feature 扩展入口

`PipelineFeature` 用于 Registry 的类型擦除和生命周期管理。业务 Feature 的推荐扩展入口是
`LogicFeature`、`PhysicsFeature`、`RenderFeature`、`AudioFeature`、`StreamFeature` 或
`CompositeFeature<Parts...>`。这是 AEG Domain-Oriented Pipeline Architecture 的开发
约定：当前不会通过构造令牌、模板约束或 Registry 的运行时 RTTI 检查强制禁止直接继承
`PipelineFeature`。直接派生在 C++ 层面合法，但会绕过领域基类固定的 Builder 能力边界和
领域元数据；是否遵守该约定由开发人员负责。

`CompositeFeature` 采用成员组合。Part 按开发约定应选择一个领域基类，不作为独立顶层
Feature 注册，也不嵌套另一个 `CompositeFeature`。当前模板仅强制拒绝嵌套组合；其他
领域隔离规则由开发人员维护。该约定维持领域能力的可推导性，并避免以多重继承绕开
Builder 边界。

## 依赖边界

Pipeline 可以使用 Foundation、Concurrency 和通用 RHI 数据模型，但不应依赖具体
Vulkan/D3D12 类型。`PipelineScheduler` 通过 Concurrency 的 `ExecutorRegistry` 映射
执行域；RenderGraph 通过受控的 Feature/Runtime 接口接入 Pipeline，不应让 Pipeline
直接持有 RenderGraph 的私有实现。
