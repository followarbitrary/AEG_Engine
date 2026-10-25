# Pipeline

`Pipeline` 保留 Engine 内部的技术投影编译与阶段观测实现。游戏作者使用 `Engine/Feature`
的 FeatureDefinition、类型化 StateDelta 和 `.aegfeature` FeatureGraph。

所有 Pipeline 头均位于 `private/`，没有公开头或兼容 shim。`pipeline.hpp` 与
`pipeline_types.hpp` 提供技术 Definition、Stage、Artifact、Registry、ScopeHost 和编译输入。
逻辑资源句柄复用 RenderGraph 的实际 `render_graph_types.hpp` 定义；Pipeline 内部别名
不提供另一套公开句柄。`render_pass.hpp`、`render_program.hpp` 及其 Registry 和程序
绑定契约同样仅供 Engine 内部使用。

`PipelineRegistry::register_pipeline()` 接收拥有字符串与记录的 `PipelineDefinition`，
完整重放 Stage、Artifact、访问绑定、ordering 和 exposure 验证后才入表。编译直接读取
该不可变 Definition，保留 scope/capability、Phase、Artifact 版本与可见性校验，生成
一 Stage 一节点的依赖、拓扑顺序及批次；编译失败不发布部分计划。

`PipelinePlan::definition_owner` 保留注册表中的实际 Definition。ScopeHost 继续管理
active、pending、retired 计划，支持帧内 pin、安全边界切换与退休；外部 plan 引用、
Artifact 合同及尚未结束的 Writer 仍约束替换。Registry 注销不使已持有的 Definition
或计划失效。Artifact 的值、共享不可变快照、发布、跨 scope 桥和执行 lease 继续保留。
WorldScope 的旧 Pipeline 注入已删除，World 快照使用其私有技术 Artifact 桥。

`PipelineScheduler` 只按编译批次遍历或筛选 Phase；`PipelineExecutionContext` 提供
本次成功调用的只读节点索引，失败后索引为空。业务执行由真正的 FeatureRuntime
承担；技术 Stage 不分发业务 callback。执行域与时间策略是技术计划元数据，业务
并发、worker lane 和取消行为由 Feature/Concurrency 提供。

旧结构作者链已物理删除：`PipelineFeatureDeclaration`、FeaturePoint、Feature/Part/
Point 身份、`register_feature` / `find_feature_*` / `bind_feature` / `unbind_feature`、
贡献展开与 `feature_owners` 均不再存在。`pipeline_configuration.hpp` 及其中的 Pipeline/
Feature/RenderPass instance 配置、ScopeHost 配置入口和编译配置链已删除。
`PipelineDescriptor` 不再携带旧绑定使用的 domains；五域业务合同仍属于 `Engine/Feature`。
此前的 Pipeline/RenderFeature 虚执行层、factory/build/collect、raw Task callback、旧模块
声明 backend、FlowGraph DTO、composer 和 state input/output/layer 通道也已删除。

私有 native `RenderGraphBuilder` 使用独立的 `RenderGraphBuildFlags`：
`kMayCreateResources`、`kMayCreateGraphicsPass`、`kMayCreateComputePass`、
`kMayCreateCopyPass`、`kMayUseDedicatedQueue`、`kMayWriteFinalOutput`。这些六位权限
控制真实资源与 Pass 操作，不再来自 FeaturePoint；Snapshot 发布仍由技术 Artifact 合同校验。
`RenderGraphBuildContext` 只包含 `flags` 与借用的 `context_data`，Builder 提供
`build_flags()`、`context_data()`、`allows()` 和 `is_runtime()`，不携带旧作者身份或实例配置。

NativeGraphBuildSession 与 committed Feature 投影继续复用同一资源/Pass backend、
事务和命令录制路径。Renderer 的业务工作来自实际提交的 RenderIntent 与 RenderWorkItem；
ImGui 使用固定私有 overlay adapter。内部 `RenderPassRegistry`、native setup/execute、
`RenderPassConfiguration` 与 `NativePassSequence` 仍服务真实技术执行，后两者不属于已删除
的旧实例配置链。native 排序使用实际注册的 Pass handle；`retain_execution_owner`
保活实际解析的 Pass 和配置，直到 Graph 提交的 GPU 帧退休。

Pipeline 使用 Foundation、Feature、RenderGraph 中性值和通用 RHI 数据，不持有
RenderGraph 私有实现或具体 Vulkan/D3D12 类型。AssetReference、ParameterBlock 和
ParameterValue 直接使用 Foundation `parameter_value.hpp`。
