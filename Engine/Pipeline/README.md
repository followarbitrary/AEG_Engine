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
Feature 注册，也不嵌套另一个 `CompositeFeature`。这些规则只用于设计评审、Editor 元数据
审查和运行时诊断，不增加额外的代码 trait、构造令牌或 Registry 硬性拒绝；组合实现由游戏
项目自行维护，避免把口头领域约束误写成引擎核心的类型限制。

## 依赖边界

Pipeline 可以使用 Foundation、Concurrency 和通用 RHI 数据模型，但不应依赖具体
Vulkan/D3D12 类型。`PipelineScheduler` 通过 Concurrency 的 `ExecutorRegistry` 映射
执行域；RenderGraph 通过受控的 Feature/Runtime 接口接入 Pipeline，不应让 Pipeline
直接持有 RenderGraph 的私有实现。
# Authoring contracts

游戏代码中的 Pipeline、Feature 和 RenderPass 必须继承对应的 Pipeline 专有基类，并通过
`TypeCatalog` 提供 Editor 所需的稳定类型、参数、端口和声明元数据。`.aegpipe` 文档只保存
稳定 ID、参数、连接和布局，不保存函数地址、编译索引或 GPU Handle。

`RenderPassRegistry` 负责把自定义 Pass 适配到现有 `RenderGraphBuilder`；Pass 回调只能通过
`RenderPassBuilder` 与 `RenderPassExecutionContext` 访问图资源。CompositeFeature 的组合规则
当前仅作为文档和注册审查约定，不由额外的代码 trait 强制执行。

Render Pass 需要 Shader 或其他 GPU 程序时，声明阶段使用
`RenderPassBuilder::declare_render_program()`，执行阶段使用
`RenderPassExecutionContext::bind_render_program()`。两者只传递稳定的
`RenderProgramRequest`、参数块和逻辑资源绑定；程序提供器返回的
`PreparedRenderProgram::owner` 负责把模块代码和帧内资源保活，Pass 本身不接触 Device、
原生命令对象或 Vulkan 创建函数。

模块声明可使用 `PipelineBuilderBackendAdapter` 转发到当前 `PipelineBuilder`。适配器只在
声明回调期间借用 Builder；模块不得保存适配器、句柄或资源绑定供异步回调使用，跨阶段数据
必须通过已发布的不可变 Pipeline 配置传递。

在 Pass 的 `.cpp` 中使用 `REGISTER_RENDER_PASS(MyPass)` 完成静态注册。宏参数应为当前
命名空间中的类型名；注册发生在 `main()` 前，运行时通过 `global_render_pass_registry()`
按稳定 `RenderPassTypeId` 查找。
