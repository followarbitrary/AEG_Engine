# AEG Domain-Oriented Pipeline Architecture

> 文档性质：架构说明、公共 API 和 ABI 使用手册
>
> 更新时间：2026-09-07
>
> 适用范围：Pipeline、PipelineFeature、Scope、Artifact、Domain Graph、RenderGraph、RHI

## 1. 文档目的

本文档用于说明 AEG Domain-Oriented Pipeline Architecture，以及游戏代码、引擎模块和
插件如何使用这套架构。下文将其简称为“本架构”。它回答以下问题：

- 为什么引擎功能统一抽象为 Pipeline。
- Pipeline、Stage、Task、Feature、Artifact 和 Graph 分别负责什么。
- 游戏开发者如何定义、注册、绑定和执行自定义 Pipeline。
- 内置领域 Feature 如何扩展，以及跨领域功能如何使用 `CompositeFeature`。
- Engine、World、Frame 和 Editor 之间如何隔离数据和生命周期。
- 公共 API 的使用边界、对象所有权、线程安全和 ABI 兼容规则是什么。
- RenderGraph、RHI、Shader Contract 与 Engine Pipeline 如何协作。

本文档不是实现排期、任务列表或模块开发顺序。文档中的代码是接口级示例，用于说明
调用方式和契约；某些类型在当前仓库中仍属于设计目标，不代表已经全部实现。

### 1.1 实现边界与文档维护

公共接口发生变化时，必须同步修订对应的签名、示例、枚举语义、生命周期和错误处理说明。
已实现 API 的描述以仓库公共头文件及可验证的实现行为为依据；尚未实现的架构约定必须明确
标识，不得将概念接口作为可以直接调用的现有 API。

当前已实现的 Pipeline 基建包括 `PhaseRegistry`、`PipelineRegistry`、`PipelineCompiler`、
`PipelineScheduler`、`PipelineScopeHost`、`ArtifactRegistry`、`ArtifactScopeBridge`、
五个领域 Feature 基类、`CompositeFeature<Parts...>`、RenderFeature 到 RenderGraph 的
运行期适配，以及 `EngineFrameOrchestrator`。`Concurrency` 已提供通用的
`Executor`、`WorkerExecutor`、`InlineExecutor`、`ExecutorRegistry`、`Completion` 和
`CancellationToken`、`TaskGroup` 和 `TaskGraph`；Pipeline Scheduler 通过执行器注册表
驱动各执行域。通用 Host 通过 Scope、实例 ID 和非拥有的 `owner` 指针关联宿主对象；
Orchestrator 通过非拥有的 Host 注册表统一驱动这些实例。

当前还实现了 `Engine/ECS` 的 `WorldManager`、`World`、`WorldCommandBuffer`、
`WorldSnapshot`，以及 `Engine/World` 的 `WorldScope`、`WorldFrameScope` 和
`WorldFrameKey`。`EngineFrameOrchestrator` 可以自动消费 World 命令、执行
`WorldCommit` 快照发布，并为注册的 Frame View 安装 World-to-Frame Bridge。

下文的专用 Engine/World/Frame/Editor Host、
`FeatureExecutionContext`、动态模块 C ABI，以及 Simulation/Render
参考 Stage 编排属于架构约定，不表示当前已有同名公共实现。`WorldPipelineHost` 和
`FramePipelineHost` 当前由通用 `PipelineScopeHost` 分别承担，专用类型仍属于文档层抽象。
`RebuildReason` 和
`PipelineRegistry::request_rebuild()` 仍属于设计接口；当前重建由调用方显式执行
`PipelineScopeHost::rebuild(registry)`，Orchestrator 会在帧边界检测 Phase revision 并
自动执行必要的重建和 Plan 切换。这些约定说明目标语义，不构成实现排期。

本文档采用以下规范性措辞：

- “必须”表示实现或调用方若不满足该条件，行为即不符合公共契约。
- “不得”表示明确禁止的用法；实现不得以隐式降级方式改变其语义。
- “应”表示默认推荐的实现策略；只有在具备等价约束和充分诊断的情况下才可偏离。
- “可以”表示实现或调用方拥有选择权，但选择结果仍须满足本节定义的生命周期、依赖和
  ABI 约束。
- “语义锚点”表示可被外部模块稳定引用的生命周期或扩展位置；它不必对应单一线程、
  单一函数或单一 GPU 命令。

## 2. 设计结论

AEG Domain-Oriented Pipeline Architecture 采用分层管线模型：

```text
Engine Scope
    -> World Scope[WorldHandle]
        -> Frame Scope[WorldFrameKey]
            -> Editor Overlay Scope

每个 Scope：
    Pipeline
        -> Stage / FeaturePoint
            -> Task
                -> Artifact Dependency DAG
                    -> Domain Graph
                        -> RenderGraph / CPU Graph / IO Graph
                            -> RHI
```

图中的箭头表示 Host 的管理/构建关系，不表示数据读取方向；数据依赖以第 5.2 节的
Artifact 和 Bridge 规则为准。

核心结论如下：

1. 所有具有明确执行时机、输入输出和依赖关系的功能都可以表达为 Pipeline。
2. Math、Logging、Allocator、RHI Device、Resource Cache 等基础服务不伪装成业务
   Pipeline。
3. `EnginePhase` 是稳定的公共生命周期锚点；Pipeline 内部可以拥有自己的 Stage。
4. Vertex、Fragment、Compute 等 Shader 阶段不承担引擎扩展点职责。
5. Pipeline 之间通过 Artifact 和显式依赖通信，不通过隐式系统调用链通信。
6. `PipelineDefinition`、`PipelineInstance` 和 `PipelinePlan` 必须分离。
7. World Pipeline 必须绑定具体 `WorldHandle`，不得采用 Singleton 实例化策略。
8. 跨领域 Feature 使用成员组合 `CompositeFeature<Parts...>`，不使用多重继承。
9. RenderGraph 负责渲染资源、生命周期、Barrier 和 Queue Sync；RHI 负责后端无关的
   GPU 对象和命令接口。
10. Vulkan、D3D12 或其他后端类型不得越过 RHI private 边界进入公共 Pipeline API。

## 3. 为什么采用 Pipeline

### 3.1 解决隐式系统调用链的问题

传统系统组织方式通常是由一个系统直接调用另一个系统：

```text
GameplaySystem
    -> PhysicsSystem
        -> AnimationSystem
            -> RenderSystem
```

这种结构的问题是：

- 执行顺序隐藏在函数调用关系中。
- 很难判断两个系统是否可以并行。
- 数据所有权和线程边界不清晰。
- 插件必须修改核心调用链才能插入功能。
- 系统之间容易形成循环依赖。

Pipeline 让依赖显式化：

```text
GameplayPipeline  --writes--> GameplayCommandBuffer
SimulationPipeline --reads--> GameplayCommandBuffer
SimulationPipeline --writes--> PhysicsSnapshot
RenderExtract     --reads--> PhysicsSnapshot
```

调度器可以据此验证拓扑、生成并行批次、等待完成点，并且在不修改既有 Pipeline 代码的
情况下插入新的功能。

### 3.2 统一调度语义，而不是统一所有实现

Pipeline 只统一以下内容：

- 何时执行。
- 读取和写入什么。
- 依赖谁。
- 运行在哪个执行域。
- 属于哪个作用域和时间域。
- 如何报告失败和完成。

Pipeline 不统一领域算法。物理求解、音频混音、渲染资源图和 IO 解码仍然由各自领域
实现。这样既能共享调度框架，又不会将所有领域压缩为难以维护的过度聚合接口。

### 3.3 带来的收益

| 能力         | 直接收益                                       |
|--------------|------------------------------------------------|
| 显式依赖     | 编译期发现循环、缺失生产者和非法访问           |
| 并行批次     | 没有冲突的任务可以并行执行                     |
| 计划缓存     | 拓扑不变时不重复分析图结构                     |
| Scope 隔离   | 多个 World、View 和 Editor 会话互不串数据      |
| Builder 沙箱 | Feature 只能使用被授权的领域能力               |
| 后端隔离     | Vulkan 和 D3D12 可以共享上层契约               |
| 结构化诊断   | 可以定位到 Pipeline、Feature、Part 和 Artifact |
| 可插拔扩展   | 游戏侧功能无需修改引擎核心调用链               |

## 4. 术语和四个正交维度

Pipeline 节点不得仅由一个分类描述。每个节点至少包含以下四个维度：

| 维度              | 含义                     | 示例                                                         |
|-------------------|--------------------------|--------------------------------------------------------------|
| `PipelineScope`   | 数据和实例的生命周期边界 | `Engine`、`World`、`Frame`、`Editor`                         |
| `FeatureDomain`   | 功能所属的业务领域       | `Logic`、`Physics`、`Render`、`Audio`、`Stream`              |
| `ExecutionDomain` | 工作使用的执行资源       | `MainThread`、`CpuWorker`、`IoWorker`、`RenderThread`、`Gpu` |
| `TimePolicy`      | 节点被什么时间机制驱动   | `Startup`、`Frame`、`FixedTick`、`Event`、`Shutdown`         |

规范性的公共枚举如下。枚举底层类型固定为 `uint8_t`，数值是 ABI 契约的一部分；已经发布
的数值不得重新编号，新增值只能追加。

```cpp
enum class PipelineScope : uint8_t {
    kEngine = 0,
    kWorld = 1,
    kFrame = 2,
    kEditor = 3
};

enum class FeatureDomain : uint8_t {
    kLogic = 0,
    kPhysics = 1,
    kRender = 2,
    kAudio = 3,
    kStream = 4,
    kComposite = 5
};

enum class ExecutionDomain : uint8_t {
    kMainThread = 0,
    kCpuWorker = 1,
    kIoWorker = 2,
    kRenderThread = 3,
    kGpu = 4
};

enum class TimePolicy : uint8_t {
    kStartup = 0,
    kFrame = 1,
    kFixedTick = 2,
    kEvent = 3,
    kShutdown = 4
};

using PipelineScopeMask = uint32_t;
using FeatureDomainMask = uint32_t;
```

### 4.1 `PipelineScope` 枚举值

`PipelineScope` 决定实例和 Artifact 的所有权边界，不决定线程，也不决定执行频率。

| 枚举值    | 精确定义                                                                | 典型实例                                             | 约束                                                                    |
|-----------|-------------------------------------------------------------------------|------------------------------------------------------|-------------------------------------------------------------------------|
| `kEngine` | 引擎进程级或运行时级作用域，保存跨 World 共享的定义、服务和只读能力信息 | `EnginePipelineHost`、资源缓存、设备能力             | 不得读取 World、Frame 或 Editor 的业务数据；通常每个引擎进程只有一个    |
| `kWorld`  | 一个独立世界的可变状态作用域，包含 ECS、Gameplay、Physics 等状态        | `WorldPipelineHost[WorldHandle]`                     | 必须绑定有效 `WorldHandle`；不得将状态发布为跨线程可变借用              |
| `kFrame`  | 某个 World 的某个 View 在一次输出帧内的临时作用域                       | `FramePipelineHost[WorldFrameKey]`、RenderGraph 资源 | 只能读取已发布的 World Snapshot；Frame 结束并完成退休后，临时对象失效   |
| `kEditor` | 编辑器会话或工具 View 的叠加作用域                                      | `EditorOverlayHost`                                  | 只能通过 Frame 输出和 Command Channel 参与；不得直接修改 World 可变状态 |

### 4.2 `FeatureDomain` 枚举值

`FeatureDomain` 是能力和 Builder 的分类。它不等于线程、不等于 GPU 阶段，也不等于
Pipeline 的 `category`。

| 枚举值       | 精确定义                                             | 对应基类/Builder                         | 典型输出                                      |
|--------------|------------------------------------------------------|------------------------------------------|-----------------------------------------------|
| `kLogic`     | CPU 业务逻辑、状态更新，以及不直接产生像素的计算任务 | `LogicFeature` / `LogicGraphBuilder`     | Gameplay 状态、AI 结果、命令、计算快照        |
| `kPhysics`   | 刚体、碰撞、布料、流体等物理求解及其数据发布         | `PhysicsFeature` / `PhysicsGraphBuilder` | Physics Snapshot、碰撞事件、约束结果          |
| `kRender`    | 几何绘制、光照、阴影、Compute 后处理和显示输出       | `RenderFeature` / `RenderGraphBuilder`   | Buffer、Texture、Render Pass、Frame 输出      |
| `kAudio`     | 声场、混音、空间音频和音频事件处理                   | `AudioFeature` / `AudioGraphBuilder`     | 音频节点、混音数据、播放命令                  |
| `kStream`    | 资源 IO、解码、上传准备、取消和发布                  | `StreamFeature` / `StreamGraphBuilder`   | IO 请求、解码结果、资源发布事件               |
| `kComposite` | 仅表示一个 Feature 由多个具体领域 Part 组成          | `CompositeFeature<Parts...>`             | 各 Part 各自的领域 Artifact；不是独立 Builder |

`kComposite` 只能用于 `FeatureDescriptor::domain` 这样的单值分类字段。它不是一个可
单独执行的领域，`domain_mask` 应填写所有具体 Part 领域的并集，不得仅填写
`kComposite`。

### 4.3 `ExecutionDomain` 枚举值

`ExecutionDomain` 描述工作由哪类执行资源承载。它是调度约束，不是线程句柄；实际线程
和队列由 Scheduler 或领域 Host 管理。

| 枚举值          | 精确定义                            | 允许的工作                                  | 不可推断的内容                                           |
|-----------------|-------------------------------------|---------------------------------------------|----------------------------------------------------------|
| `kMainThread`   | 引擎主控制线程上的串行工作          | 生命周期控制、输入分发、非线程安全 API 调用 | 不表示所有 Engine Scope 工作都必须在主线程               |
| `kCpuWorker`    | 引擎 CPU Worker Pool 中可调度的任务 | 可并行的 CPU 计算、数据转换、图编译         | 不保证固定 Worker 线程；任务必须满足线程安全要求         |
| `kIoWorker`     | IO 或资源流送专用执行资源           | 文件、网络、解码和阻塞式外部 IO             | 不得直接录制 RHI Command List，完成后必须发布 Artifact |
| `kRenderThread` | Render Build/Command 录制执行资源   | RenderGraph 构建、Pass setup、RHI 命令录制  | 不表示工作已经在 GPU 执行                                |
| `kGpu`          | 由 GPU 编排通道准备并提交、最终在 GPU 上执行的工作 | Graphics、Compute、Copy 等 GPU 命令         | 当前 Scheduler 在 CPU 侧执行编排回调；真实 GPU 指令由 RHI Queue Submission 执行，不得把该域误认为 GPU 线程  |

`kRenderThread` 和 `kGpu` 可以同时出现在一个 Render Pass 的不同节点上：前者负责在
CPU 侧准备和录制命令，后者负责在 CPU 侧编排 GPU 工作并提交 Queue；提交完成后，真实
的 Shader 和 GPU 指令由 RHI 后端及设备执行。二者都不是“在 GPU 上运行的 C++ 函数”。

### 4.4 `TimePolicy` 枚举值

`TimePolicy` 描述节点何时被触发，不负责表达同一时间域内的先后关系；先后关系必须用
Artifact 依赖或 `runs_before`/`runs_after` 声明。

| 枚举值       | 精确定义                                  | 调用次数/触发条件              | 适用规则                                                        |
|--------------|-------------------------------------------|--------------------------------|-----------------------------------------------------------------|
| `kStartup`   | 引擎、模块或 Scope 激活时执行的一次性工作 | 对应对象激活后执行一次         | 适合初始化定义、缓存和初始 Artifact；不得假设每帧执行           |
| `kFrame`     | 每个有效输出帧执行的可变步长工作          | 每帧最多一次，使用当前帧时间   | 适合 Render、Frame Extract、UI 和可变步长逻辑                   |
| `kFixedTick` | 固定时间步长驱动的工作                    | 一帧内可能执行零次、一次或多次 | 适合 Physics 和确定性模拟；必须使用 Tick 上下文而不是渲染帧时间 |
| `kEvent`     | 由显式事件或命令触发的工作                | 事件到达时执行，可同帧多次     | 必须声明事件来源和消费语义，不得依赖轮询顺序                    |
| `kShutdown`  | Scope 或模块关闭时执行的收尾工作          | 关闭流程中执行一次             | 必须在相关 Task、IO 和 GPU Completion 满足后释放资源            |

### 4.5 枚举与 Mask 的转换规则

两个 Mask 使用对应枚举值的 bit 位表示集合，而不是把枚举底层数值直接作为 Mask 值：

```cpp
const PipelineScopeMask kWorldScopeMask =
        1u << static_cast<uint8_t>(PipelineScope::kWorld);
const FeatureDomainMask kRenderDomainMask =
        1u << static_cast<uint8_t>(FeatureDomain::kRender);
```

必须遵守以下规则：

- `0` 表示空集合，不表示“所有值”。
- 未知 bit 在编译和 ABI 校验时必须报告错误，不得静默当作已知权限。
- `PipelineScopeMask` 只允许包含 `PipelineScope` 的 bit；不得写入 Domain bit。
- `FeatureDomainMask` 只允许包含具体领域 bit；Composite 通常使用 Part 领域并集。
- “全部作用域/全部领域”必须由 API 明确定义的常量提供，不得由调用方自行推定。
- 新增枚举值只能占用未使用的数值和 bit，不得改变既有值的意义。
- 跨 ABI 序列化时同时传递底层整数和版本；遇到未知或当前实现不支持的值必须返回
  `kUnsupported` 或 `kInvalidArgument`。`Foundation::ResultCode` 当前没有专门的
  “版本不匹配”成员；版本不匹配应通过模块/契约版本字段和诊断信息表达。

所有跨模块传递的 ID、版本、计数和大小使用固定宽度整数。

这几个维度互相独立。例如：

```text
World + Physics + CpuWorker + FixedTick
Frame + Render + RenderThread + Frame
Editor + Render + RenderThread + Frame
World + Stream + IoWorker + Event
```

不得因为节点属于 `Render` 就推断其必然运行在 GPU，也不得因为节点属于 `Frame` 就
推断其必然运行在 Render Thread。

## 5. 作用域模型和数据依赖

### 5.1 四种作用域

| Scope    | 作用                        | 实例数量             | 典型数据                            |
|----------|-----------------------------|----------------------|-------------------------------------|
| `Engine` | 全局服务和共享定义          | 一个                 | 配置、资源缓存、设备能力            |
| `World`  | 一个独立世界的可变运行状态  | 多个                 | ECS、物理状态、Gameplay 状态        |
| `Frame`  | 一次渲染/输出上下文         | 每个 World/View 多个 | RenderScene、临时资源、FrameContext |
| `Editor` | 编辑器对 Frame 的叠加和命令 | 多个会话/View        | Overlay、选择结果、编辑命令         |

### 5.2 单向依赖规则

本文档使用 `A -> B` 表示 A 读取或依赖 B。允许的公共数据边为：

```text
World  -> Engine
Frame  -> WorldSnapshot
Frame  -> Engine
Editor -> FramePublishedOutput
Editor -> Engine
```

以下依赖禁止出现：

```text
Engine -> World / Frame / Editor
World  -> Frame / Editor
Frame  -> Editor
Editor -> WorldMutableState
```

`Engine` 可以控制下层生命周期、持有句柄和等待完成点，这属于控制依赖，不等于读取
下层业务数据。

### 5.3 WorldHandle

World 不是全局单例。每个 World 使用带 generation 的句柄，避免销毁后复用索引导致
悬空访问：

```cpp
struct WorldHandle {
    uint32_t index;
    uint32_t generation;
};
```

句柄只用于索引和有效性校验，不提供隐式指针转换。generation 不匹配时，读取必须失败
并产生诊断。

World Pipeline 的实例关系如下：

```text
GameWorld[W1]
    -> WorldPipelineHost[W1]
        -> GameplayPipelineInstance
        -> PhysicsPipelineInstance
        -> WorldPlan[W1]

PreviewWorld[W2]
    -> WorldPipelineHost[W2]
        -> GameplayPipelineInstance
        -> PhysicsPipelineInstance
        -> WorldPlan[W2]
```

不同 World 可以共享 Pipeline 定义、Shader、Mesh、Texture、只读配置和编译缓存；不得
共享 ECS 可变状态、物理求解器状态、World Artifact、Feature 可变成员或运行时完成点。

### 5.4 FrameKey

Frame 通过不可变快照读取 World，不得直接读取正在修改的 World 指针：

```cpp
struct WorldFrameKey {
    uint64_t frame_number;
    WorldHandle world;
    uint32_t view_id;
};
```

下面的 Frame 即使使用相同的逻辑 Artifact 名称，也必须拥有不同的运行时命名空间：

```text
Frame[100, W1, MainView]
Frame[100, W2, PreviewView]
Frame[100, W1, ShadowView]
```

### 5.5 Editor Overlay

Editor 是 Scope，不是新的 Domain。Editor 通过 Frame Overlay 读取最终输出并追加受限
绘制：

```text
Frame RenderGraph
    -> FrameOverlayBridge
        -> EditorOverlayPipeline
            -> BeforePresent
```

Editor 默认不得删除基础 Pass、重排 RenderGraph、替换主 Render Target 或直接访问 World
可变状态。需要修改 World 时，Editor 发布 `EditorCommandBuffer`，由 World 在下一次
`Input` 或 `PreUpdate` 阶段消费。

## 6. Pipeline 的三层对象

### 6.1 PipelineDefinition

静态定义描述 Pipeline 的身份、版本、能力、Stage、Artifact 和 FeaturePoint。它可以被
多个 World 或运行模式复用，不保存运行时句柄。

### 6.2 PipelineInstance

实例是 Definition 绑定到具体 Scope 后的运行对象，保存配置、Feature 绑定和领域内部
状态。World Scope 的 Instance 必须绑定 `WorldHandle`。

### 6.3 PipelinePlan

Plan 是 Compiler 根据 Definition、Instance 和 Feature 贡献生成的不可变执行计划，保存：

- 扁平化后的节点和依赖边。
- 可并行执行的批次。
- Artifact 生命周期范围。
- Scope Bridge 和完成点要求。
- 运行时函数绑定和诊断索引。

三者关系：

```text
PipelineDefinition
    +--> PipelineInstance[Scope/Owner]
    +--> PipelinePlan[Topology/Capabilities/Bindings]
```

Plan 不保存业务对象裸指针，也不保存 Vulkan、D3D12 等后端原生对象。

## 7. 公共身份、版本和句柄

### 7.1 稳定 ID

公共身份不得依赖注册顺序或内存地址：

| ID                   | 作用                    |
|----------------------|-------------------------|
| `PipelineId`         | Pipeline 类型身份       |
| `PipelineInstanceId` | 一个 Scope 中的运行实例 |
| `StageId`            | Pipeline 内部 Stage     |
| `TaskId`             | Plan 中的执行节点       |
| `FeatureId`          | Feature 类型身份        |
| `FeatureInstanceId`  | 顶层 Feature 绑定身份   |
| `FeaturePartId`      | Composite 子 Part 身份  |
| `FeaturePointId`     | Pipeline 公开扩展锚点   |
| `ArtifactId`         | 数据契约身份            |
| `ArtifactTypeId`     | 数据布局和版本身份      |
| `PlanRevision`       | Plan 版本和失效判断     |

公共 ID 应采用稳定的哈希表示，并保留调试名称用于日志、诊断和工具显示。哈希冲突
必须在注册和编译阶段显式报告，不得静默覆盖。

### 7.2 运行时句柄

句柄必须同时满足对象归属与代际有效性约束。当前 RenderGraph 的 Buffer、Texture、Pass
句柄使用 `index + generation + graph_id`：`index` 定位对应类型的记录，`generation`
标识记录代际，`graph_id` 标识创建它的图实例。不同图即使 index 和 generation 相同，
也不得互相解析句柄。Pipeline 侧的 `RenderGraphHandle<Tag>` 镜像保留这三个字段。
句柄应满足：

- 可复制、可比较。
- 不隐式拥有对象。
- 不隐式转换为裸指针。
- 只能由创建它的 Host 或 Device 解析。
- generation 失效后所有操作返回错误。

`is_valid()` 只验证字段非空，不访问对象表；必须通过所属图的 `contains_buffer()`、
`contains_texture()` 或资源操作验证实际有效性。`reset()` 使原资源句柄失效；
`clear_passes()` 保留资源句柄，但移除原 Pass。`graph_id == 0` 表示没有有效的图身份。

## 8. EnginePhase、Stage 和 FeaturePoint

### 8.1 EnginePhase 与可扩展全局 Phase

`EnginePhase` 是跨 Pipeline 共享的全局生命周期锚点，用于建立不同 Pipeline 之间的
宏观时序偏序关系。它不是功能实现类，也不是线程、队列或 GPU Shader Stage 的别名。
这些枚举值是 ABI 稳定的内置 Phase 集合；内置枚举并不构成全局 Phase 命名空间的封闭
边界。真正的全局 Phase 集合由 `PhaseRegistry` 管理，扩展 Phase 使用独立的 `PhaseId`
标识，并通过相对于既有 Phase 的 `before/after` 关系插入全局偏序图。

`StageDescriptor::phase_id` 非空时表示该 Stage 使用注册表中的自定义或内置 Phase；为空
时才从兼容字段 `StageDescriptor::phase` 推导内置 Phase。这样既保持旧代码的 ABI/API
兼容性，又允许模块注册真正参与跨 Pipeline 调度的新 Phase。公共枚举定义及逐值语义如下：

```cpp
enum class EnginePhase : uint8_t {
    kStartup = 0,
    kFrameBegin = 1,
    kInput = 2,
    kPreUpdate = 3,
    kFixedUpdate = 4,
    kVariableUpdate = 5,
    kWorldCommit = 6,
    kRenderExtract = 7,
    kRenderBuild = 8,
    kRenderExecute = 9,
    kPresent = 10,
    kFrameEnd = 11,
    kShutdown = 12
};
```

| 枚举值            | 精确定义                                             | 典型输入与输出                                                                                       | 调度与可见性约束                                                                               |
|-------------------|------------------------------------------------------|------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------|
| `kStartup`        | Engine、模块或 Scope 进入可运行状态前的引导阶段      | 输入为静态配置和模块定义；输出为 Registry、长期服务和初始能力档案                                    | 不得读取尚未建立的 World/Frame 状态；一次性初始化必须具备幂等或明确的重复调用策略              |
| `kFrameBegin`     | 建立一次输出帧的身份、时间基准和帧槽上下文           | 输入为 Engine/World 入口和外部时钟；输出为 `WorldFrameKey`、Frame Slot 和帧级上下文                  | 必须先完成上一帧所占用帧槽的 GPU Completion 检查；不得使用尚未获得的 Frame Token               |
| `kInput`          | 摄取并规范化设备输入、网络事件和 Editor 命令         | 输入为外部事件流；输出为版本化 `CommandBuffer` 或输入 Artifact                                       | 输入数据在此阶段完成格式归一化；不得绕过命令通道直接修改 World 可变状态                        |
| `kPreUpdate`      | 在模拟迭代前消费命令并建立本次更新的初始条件         | 输入为 CommandBuffer、上一提交状态和时间参数；输出为 Tick 输入及求解前状态                           | 属于 World Owner 的受控写入窗口；下游只能通过声明的 Artifact 读取结果                          |
| `kFixedUpdate`    | 按固定时间步长执行零次或多次确定性更新               | 输入为固定 Tick 上下文；输出为模拟状态、事件和中间求解 Artifact                                      | 同一渲染帧内允许重复执行；必须使用 Tick 序号和固定步长，不得以渲染帧时间替代                   |
| `kVariableUpdate` | 按当前帧可变时间步长执行一次表现和业务更新           | 输入为可变 `delta_time`、已提交状态和帧输入；输出为摄像机、表现参数和非确定性业务结果                | 结果不得被隐式解释为固定 Tick 状态；若需确定性重放，必须另行记录输入与 Tick 数据               |
| `kWorldCommit`    | 将 World 的可变状态发布为版本化、不可变的可见数据    | 输入为本帧 World 状态；输出为 `WorldSnapshot`、事件和统计快照                                        | 形成写入方到读取方的发布边界；发布后不得原地修改快照内容                                       |
| `kRenderExtract`  | 从已发布的 World Snapshot 构造 Frame 级 Render Scene | 输入为 Snapshot、Engine 资源索引和 View 参数；输出为可渲染实体、视图和 Render Artifact               | 不得访问 World 的可变容器；多个 View 可以并行执行，但必须拥有独立的 Frame 身份                 |
| `kRenderBuild`    | 以声明式方式构造并编译当前帧的 RenderGraph           | 输入为 Render Scene、Shader Contract 和运行时能力；输出为 Pass、资源、Binding、Barrier 和 Queue Plan | 在 CPU 构建域执行；不得把构建阶段误认为 GPU 执行，也不得在此阶段提交未编译拓扑                 |
| `kRenderExecute`  | 录制并提交已编译的 RenderGraph 执行计划              | 输入为不可变 Plan、Frame Context 和资源解析结果；输出为 Queue Submission 与 GPU Completion           | 只允许使用当前 Frame 的 Plan 和资源；执行期间不得修改图拓扑或跨 Scope 借用可变数据             |
| `kPresent`        | 将完成的 Frame 输出移交给 Presentation Surface       | 输入为已提交的 Swapchain Frame 和完成依赖；输出为 Present 状态、Suboptimal 或 OutOfDate 诊断         | 必须满足 `ResourceUsage::kPresent` 及全部 Queue 等待条件；不得在此阶段隐式重排基础 RenderGraph |
| `kFrameEnd`       | 结束帧生命周期并推进帧槽、统计和资源退休             | 输入为 CPU/GPU Completion；输出为退休记录、性能统计和下一帧可复用状态                                | 只有满足所有引用的 Completion 后才能回收 Frame-local 资源；不得提前销毁 GPU 仍在使用的对象     |
| `kShutdown`       | Engine、模块或 Scope 的有序静默与资源析构阶段        | 输入为关闭请求；输出为已排空的任务、回调、IO 和 GPU 使用                                             | 必须先阻止新工作，再等待既有工作完成，最后销毁依赖对象；不可与正常帧执行并发交错               |

标准客户端帧的**规范性阶段顺序**为：

```text
Startup
    -> FrameBegin
        -> Input
            -> PreUpdate
                -> FixedUpdate (zero or more iterations)
                    -> VariableUpdate
                        -> WorldCommit
                            -> RenderExtract
                                -> RenderBuild
                                    -> RenderExecute
                                        -> Present
                                            -> FrameEnd

Shutdown (outside the normal frame loop)
```

该顺序表达的是跨阶段的最低依赖约束，而非所有节点的全序。Compiler 可以在同一阶段
内部对不存在数据冲突的 Stage/Task 进行并行化；`FixedUpdate` 是唯一允许在单个渲染帧
内重复进入的标准阶段。服务器、离屏工具和编辑器模式可以省略不适用的阶段，但不得
改变保留阶段的语义或将其重新解释为其他生命周期事件。

自定义 Phase 通过锚定到既有 Phase 的相对关系进入上述生命周期，而不是替换或重编号
内置枚举。对于 `EngineFrameOrchestrator`，自定义 Phase 按其拓扑位置落入以下五个执行
区间：

| 区间         | 拓扑范围                          | Orchestrator 的执行时机                   | 典型用途                                     |
|--------------|-----------------------------------|-------------------------------------------|----------------------------------------------|
| 启动前置区间 | `Phase < Startup`                 | `initialize()` 激活 Host 后、`Startup` 前 | 模块前置注册、静态服务准备、启动前诊断       |
| 帧前置区间   | `Startup < Phase < FrameBegin`    | `run_frame()` 开始帧后、`FrameBegin` 前   | 输入预处理、帧级调度准备、外部数据摄取       |
| 活动帧区间   | `FrameBegin <= Phase <= FrameEnd` | 当前 FrameContext 处于活动状态时          | 业务更新、World 提交、RenderGraph 构建与执行 |
| 帧后置区间   | `FrameEnd < Phase < Shutdown`     | `FrameEnd` 完成并释放当前帧状态后         | 帧后统计、回收请求、异步结果归并             |
| 关闭尾部区间 | `Phase >= Shutdown`               | `shutdown()` 的有序关闭流程中             | 模块收尾、服务停机、关闭后诊断               |

同一区间内的多个自定义 Phase 仍由 `PhaseRegistry` 的拓扑排序统一决定；稳定 ID 只用于
无额外偏序约束时的确定性并列排序。`run_frame()` 自动执行帧前置、活动帧和帧后置区间，
`initialize()` 与 `shutdown()` 分别负责启动前置和关闭尾部区间。

### 8.1.1 PhaseRegistry、PhaseId 与注册约束

`PhaseRegistry` 同时保存内置 Phase 和扩展 Phase。内置 Phase 在注册表构造时自动存在，
不能被删除；扩展 Phase 必须提供唯一的 `PhaseId`、非空名称和一个已经存在的锚点 Phase：

```cpp
const aeg::pipeline::PhaseDescriptor weather_phase{
    aeg::pipeline::PhaseId("WeatherUpdate"),
    "WeatherUpdate",
    aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kVariableUpdate),
    aeg::pipeline::PhaseOrderRelation::kAfter,
};

registry.phase_registry().register_phase(weather_phase);

const auto weather_stage = builder.add_stage({
    aeg::pipeline::StageId("WeatherStage"),
    aeg::pipeline::EnginePhase::kFrameBegin,
    aeg::pipeline::ExecutionDomain::kCpuWorker,
    aeg::pipeline::TimePolicy::kFrame,
    true,
    aeg::pipeline::PhaseId("WeatherUpdate"),
});
```

其中 `PhaseId` 是稳定哈希身份，不是字符串指针，也不携带所有权；名称仅用于诊断和
工具显示。`PhaseOrderRelation::kBefore` 表示新 Phase 必须先于锚点执行，`kAfter` 表示
新 Phase 必须后于锚点执行。注册表仍保留内置 Phase 之间的标准顺序，因此扩展 Phase 不会
破坏既有生命周期边界。

| API/字段                               | 精确定义                                                          | 生命周期与失败条件                                                                           |
|----------------------------------------|-------------------------------------------------------------------|----------------------------------------------------------------------------------------------|
| `PhaseRegistry::register_phase()`      | 将一个扩展 Phase 加入全局偏序图，并执行环检测                     | ID 重复、锚点不存在、关系非法或形成环时失败；成功后 revision 递增                            |
| `PhaseRegistry::unregister_phase()`    | 删除一个未被其他扩展 Phase 作为锚点引用的扩展 Phase               | 内置 Phase、被引用 Phase 或不存在的 ID 不能删除                                              |
| `PhaseRegistry::find_phase()`          | 查询一个 Phase 的声明元数据                                       | 返回的 `name` 是 Registry 内部存储的借用视图；Registry 销毁后或对应 Phase 删除后不得继续使用 |
| `PhaseRegistry::snapshot()`            | 在同一读锁内捕获有序 Phase 列表和 revision                        | 用于 Compiler、Orchestrator 等需要同时消费顺序和版本的调用方；返回值是独立拥有的快照         |
| `PhaseRegistry::ordered_phases()`      | 返回确定性的全局拓扑序；同等约束下按内置顺序和稳定 ID 排序        | 发现环或内部状态损坏时返回错误；调用方不得自行按注册顺序排序                                 |
| `PhaseRegistry::revision()`            | 反映 Phase 集合或顺序约束是否发生变化的单调版本                   | 每次成功注册/删除递增；`PipelinePlan::phase_revision` 记录编译时版本                         |
| `PipelineCompileInput::phase_registry` | 可选的编译注册表；为空时使用 `PipelineRegistry::phase_registry()` | 编译结果必须记录所用 revision；Stage 引用未注册 Phase 时失败                                 |

Phase 注册属于拓扑变更。`snapshot()` 将顺序和 revision 作为一个逻辑版本读取；Compiler
在构建 Plan 时记录该 revision，并在已知的并发变更情况下返回 `kOutOfDate`，调用方应
重新获取快照后重试。运行中的 Orchestrator 不在当前帧中替换 Phase 图；它会在下一
个帧边界检测 revision，重编译并切换所有受影响的 Plan。若 Registry 在一帧执行期间被
修改，当前帧必须在下一阶段边界检测到版本不一致并进入 Abort/恢复路径，不能让同一帧
同时使用两个全局 Phase 图。

自定义 Phase 的锚点可以是内置 Phase，也可以是已经注册的自定义 Phase。锚点被其他
自定义 Phase 引用时不得删除；删除操作不会隐式修改其他 Phase 的锚点，也不会静默将
受影响的 Pipeline Stage 改挂到其他 Phase。Registry 的生命周期不依赖某个具体 Pipeline，
因此模块卸载前必须显式解除相关 Stage/Definition 的使用关系，并在安全边界完成 Plan
重建或停用。

当锚点是自定义 Phase 时，新 Phase 继承该锚点所在的相邻内置 Phase 区间。例如，若
`WeatherUpdate` 位于 `VariableUpdate` 与 `WorldCommit` 之间，则锚定到
`WeatherUpdate` 的前置或后置 Phase 仍必须留在同一区间；其相对边只决定区间内部顺序，
不得越过 `WorldCommit` 或被稳定排序推迟到后续生命周期区间。位于 `Startup` 之前或
`Shutdown` 之后的开放区间采用同一继承规则。

阶段值只表示生命周期位置。Feature 仍必须声明自己的 Artifact 访问和执行域；不得因为
绑定到 `kRenderBuild` 就自动获得 RenderGraph 的全部权限。不同运行模式可以省略阶段，
但不得改变保留阶段的既有语义或在同一 Plan 中重复执行一次性阶段。

### 8.2 Pipeline Stage

`Stage` 是 Pipeline 内部的局部调度单元，也是 Artifact 依赖、执行域约束和扩展点
绑定的基本粒度。Stage 不等同于函数调用、线程或单个 GPU Pass；一个 Stage 可以由
多个 Task、多个 RenderGraph Pass 或多个 Queue Batch 组成。Stage 的稳定身份由
`StageId` 表示，外部模块只能引用 Pipeline 明确公开的 Stage。

下列两个 Pipeline 是引擎的参考性 Stage 拓扑。它们规定的是语义边界和默认依赖，不是
强制采用的具体算法，也不是图形 API 的固定流水线阶段：

```text
SimulationPipeline
    PreSolve -> Broadphase -> Narrowphase -> Solve -> PublishSnapshot

RenderPipeline
    Visibility -> Shadow -> Depth -> Opaque -> Lighting -> Transparent
        -> PostProcess -> BeforePresent
```

#### `SimulationPipeline` 的 Stage 语义

`SimulationPipeline` 面向连续状态的离散化推进。其典型执行域为 `CpuWorker`，但某些
空间查询、粒子求解或约束计算可以通过 GPU Compute 实现；执行域变化不改变 Stage 的
语义身份。默认数据依赖如下：

```text
SimulationCommandBuffer / PreviousState
    -> PreSolve
        -> BroadphasePairs
            -> Narrowphase
                -> ContactManifolds / ConstraintSet
                    -> Solve
                        -> NextState / SimulationEvents
                            -> PublishSnapshot
                                -> PhysicsSnapshot
```

| Stage             | 语义契约                                                                           | 典型输入                                                              | 典型输出                                     | 不可违反的边界                                                                       |
|-------------------|------------------------------------------------------------------------------------|-----------------------------------------------------------------------|----------------------------------------------|--------------------------------------------------------------------------------------|
| `PreSolve`        | 将外部命令、时间参数和上一状态规范化为本次求解的初始条件，并建立参与求解的代理集合 | `SimulationCommandBuffer`、上一 Tick 状态、固定步长、外力和运动学输入 | `SolveInput`、Broadphase Proxy、初始约束参数 | 不得在该阶段发布尚未完成求解的 Physics Snapshot；输入规范化必须具有确定性            |
| `Broadphase`      | 基于空间索引、层级包围体或其他加速结构筛选潜在相交对                               | Broadphase Proxy、碰撞过滤规则、空间加速结构                          | `BroadphasePairs`                            | 只保证候选关系，不得将候选对直接视为精确接触；过滤规则必须与碰撞层契约一致           |
| `Narrowphase`     | 对候选对执行精确几何相交测试并构造接触流形、接触法向和穿透信息                     | `BroadphasePairs`、几何表示、材质接触参数                             | `ContactManifolds`、`ConstraintSet`          | 输出必须引用当前 Tick 的几何版本；不得读取下一 Tick 尚未发布的状态                   |
| `Solve`           | 对接触、关节和其他约束执行迭代求解，并推进下一状态                                 | `ConstraintSet`、上一状态、积分参数                                   | `NextState`、`SimulationEvents`、求解统计    | 需要确定性结果时必须固定迭代顺序、浮点策略和归约规则；不得以注册顺序作为隐式排序依据 |
| `PublishSnapshot` | 将求解结果封装为带版本的不可变快照并建立读取方可见的发布边界                       | `NextState`、事件和 Tick 标识                                         | `PhysicsSnapshot`、版本号、完成 Artifact     | 快照发布后不得原地修改；Frame、RenderExtract 等消费者只能通过 Snapshot 读取          |

上述 Stage 通常对应 `FixedUpdate`，`PublishSnapshot` 同时承担 `WorldCommit` 的发布
职责。Stage 之间的箭头是默认数据依赖；当空间分区、岛屿求解或多 World 并行时，
Compiler 可以将单个 Stage 分解为多个分区子图，但不得改变输入输出版本和确定性约束。

#### `RenderPipeline` 的 Stage 语义

`RenderPipeline` 面向 Frame 级视觉输出。其 Stage 既包含 CPU 侧的可见性分析、资源
编排和命令录制，也包含 GPU 侧的 Graphics/Compute 执行；`RenderThread` 与 `Gpu` 是
正交的执行域，不可由 Stage 名称推导。下表定义各 Stage 的最小可观察契约：

| Stage           | 语义契约                                                                                              | 典型输入                                                          | 典型输出                                          | 不可违反的边界                                                                                             |
|-----------------|-------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------|---------------------------------------------------|------------------------------------------------------------------------------------------------------------|
| `Visibility`    | 根据 View、相机参数和场景加速结构确定本帧可见集合，并生成后续绘制所需的实例或间接参数                 | `RenderScene`、Camera、Frustum、Occlusion/HZB、LOD 策略           | `VisibleSet`、Indirect Arguments、Draw Packet     | 只能输出可见性结论，不得在未声明的情况下修改 World 状态；遮挡结果必须绑定对应深度历史版本                  |
| `Shadow`        | 为光源 View 构建阴影投影并生成 Shadow Map、Atlas 或其他阴影表示                                       | `VisibleSet`、LightSet、Shadow Policy、场景几何                   | Shadow Texture、Shadow Metadata                   | 阴影资源的分辨率、层级和生命周期必须由 RenderGraph 声明；不得把阴影图隐式视为主场景颜色                    |
| `Depth`         | 建立主视图深度表示，可包含 Depth Prepass、深度金字塔或层级 Z 缓存                                     | `VisibleSet`、Camera、Depth Configuration                         | `SceneDepth`、HZB/Depth Pyramid                   | 深度格式、采样数和反向 Z 约定必须由 Pipeline/Shader Contract 固定；后续消费者必须声明读取版本              |
| `Opaque`        | 对不透明几何执行材质评估和基础光栅化，生成 GBuffer 或不透明颜色结果                                   | `VisibleSet`、`SceneDepth`、Material/Geometry、Shadow Metadata    | GBuffer、`SceneColor` 或 Opaque Color             | 不透明写入必须显式声明颜色/深度附件及 Load/Store 语义；不得将透明材质混入该 Stage 的隐式排序               |
| `Lighting`      | 将几何、材质、光源和阴影信息合成为光照结果                                                            | GBuffer、LightSet、Shadow Texture、环境光数据                     | Lit Scene Color、Lighting Auxiliary Buffers       | 可以采用 Forward、Deferred 或 Compute 实现，但必须保持输入输出 Artifact 契约；光照结果不得绕过资源状态跟踪 |
| `Transparent`   | 对透明几何执行顺序相关或顺序无关的合成                                                                | Transparent Visible Set、`SceneDepth`、当前 Scene Color、Material | Composited Scene Color、Transparency Accumulation | 需要严格排序时必须显式声明排序键和顺序约束；加权混合等近似算法必须在 Shader Contract 中标明                |
| `PostProcess`   | 对场景输出执行时域、空域和色彩空间处理，例如 TAA、Bloom、Tone Mapping、Upscaling                      | Scene Color、Depth、Motion、Exposure、History Resources           | Post-Processed Color、History 更新、诊断输出      | History 属于跨帧持久资源，必须通过明确的导入和 Completion 管理；不得将临时中间结果暴露为长期资源           |
| `BeforePresent` | 将最终 Frame 输出转换为 Presentation Surface 所需的格式、尺寸和资源状态，并完成必要的 Overlay/Resolve | Post-Processed Color、UI/Editor Overlay、Swapchain Image          | Final Output、`ResourceUsage::kPresent` 状态      | 必须是 Present 前的最后一个可扩展语义锚点；转换完成后不得再写入呈现图像                                    |

Render Stage 的默认顺序不构成不可分解的全序。例如 `Shadow` 与部分 `Depth` 工作在
资源依赖独立时可以并行，多个 Light/Viewport 也可以形成分区子图。真正的执行顺序由
Artifact 版本、RenderGraph 资源读写、Queue Dependency 和显式顺序约束共同决定。
Stage 名称只提供稳定的语义锚点，不等价于 Vulkan/D3D12 的 RenderPass、Subpass、
Shader Stage 或硬件流水线阶段。

#### 参考 Pipeline 与 `EnginePhase` 的映射

下表给出参考 Pipeline Stage 与全局 `EnginePhase` 的默认映射。映射表示 Stage 的主要
生命周期归属，不限制 Stage 内部同时使用多个执行域，也不表示每个 Stage 必须对应一个
独立的函数或 Command List。

| Pipeline             | Stage             | 默认 `EnginePhase`                | 主要执行域                    | 说明                                           |
|----------------------|-------------------|-----------------------------------|-------------------------------|------------------------------------------------|
| `SimulationPipeline` | `PreSolve`        | `kPreUpdate`                      | `kCpuWorker`                  | 将输入命令和上一 Tick 状态转换为本次求解输入   |
| `SimulationPipeline` | `Broadphase`      | `kFixedUpdate`                    | `kCpuWorker` 或 `kGpu`        | 生成潜在相交候选对；可按空间分区并行           |
| `SimulationPipeline` | `Narrowphase`     | `kFixedUpdate`                    | `kCpuWorker` 或 `kGpu`        | 计算精确接触和约束几何信息                     |
| `SimulationPipeline` | `Solve`           | `kFixedUpdate`                    | `kCpuWorker` 或 `kGpu`        | 推进约束系统和连续状态                         |
| `SimulationPipeline` | `PublishSnapshot` | `kWorldCommit`                    | `kCpuWorker` 或 `kMainThread` | 发布带版本的不可变 Simulation/Physics Snapshot |
| `RenderPipeline`     | `Visibility`      | `kRenderExtract`                  | `kCpuWorker` 或 `kGpu`        | 从 Render Scene 生成可见集合和绘制参数         |
| `RenderPipeline`     | `Shadow`          | `kRenderBuild` / `kRenderExecute` | `kRenderThread` 和 `kGpu`     | 构建并执行阴影资源生产图                       |
| `RenderPipeline`     | `Depth`           | `kRenderBuild` / `kRenderExecute` | `kRenderThread` 和 `kGpu`     | 生产主视图深度及其派生层级表示                 |
| `RenderPipeline`     | `Opaque`          | `kRenderBuild` / `kRenderExecute` | `kRenderThread` 和 `kGpu`     | 执行不透明几何的资源编排与光栅化               |
| `RenderPipeline`     | `Lighting`        | `kRenderBuild` / `kRenderExecute` | `kRenderThread` 和 `kGpu`     | 计算光照合成结果                               |
| `RenderPipeline`     | `Transparent`     | `kRenderBuild` / `kRenderExecute` | `kRenderThread` 和 `kGpu`     | 执行透明材质的排序或顺序无关合成               |
| `RenderPipeline`     | `PostProcess`     | `kRenderBuild` / `kRenderExecute` | `kRenderThread` 和 `kGpu`     | 执行时域、空域和色彩空间处理                   |
| `RenderPipeline`     | `BeforePresent`   | `kPresent`                        | `kRenderThread` 和 `kGpu`     | 生成最终输出并转换为可呈现状态                 |

`kRenderBuild` 与 `kRenderExecute` 是 Render Pipeline 中必须区分的两个生命周期边界：
前者负责声明式图构建、Shader Contract 校验和计划编译，后者负责命令录制、Queue 提交
和 Completion 管理。一个 Render Stage 可以横跨这两个 Phase，但其构建数据与执行数据
必须通过不可变 Plan 或 Frame Artifact 连接，不得在 GPU 执行期间回读或修改构建状态。

#### Stage 的公开性和扩展规则

- Stage 只有在 Pipeline Definition 中被标记为公开，或通过公开 `FeaturePoint` 暴露
  后，外部 Feature 才能依赖其 `StageId`。
- 外部 Feature 应通过 `FeaturePoint` 声明插入位置、可见 Artifact 和权限，不得直接
  改写 Stage 的内部 Task 列表或拓扑。
- 一个 Stage 可以拥有多个 `FeaturePoint`；`kBefore`、`kInside` 和 `kAfter` 的
  可见性由该锚点的契约分别定义。
- Stage 是否存在、是否裁剪和如何分区由 Pipeline Profile 与 Artifact 消费关系决定；
  但一旦对外发布 StageId，其既有语义不得被重新解释。
- Stage 的执行域、时间策略和资源访问必须显式声明。不得因为 Stage 属于
  `RenderPipeline` 或 `SimulationPipeline` 就推断其必然运行在 GPU 或固定 Tick 上。

### 8.3 FeaturePoint

FeaturePoint 是 Feature 的扩展锚点，不一定对应一个实际执行 Pass：

```cpp
enum class FeaturePointPosition : uint8_t {
    kBefore = 0,
    kInside = 1,
    kAfter = 2
};

enum class FeaturePointFlags : uint32_t {
    kNone = 0,
    kMayCreateArtifacts = 1u << 0,
    kMayCreateGraphicsPass = 1u << 1,
    kMayCreateComputePass = 1u << 2,
    kMayCreateCopyPass = 1u << 3,
    kMayUseDedicatedQueue = 1u << 4,
    kMayWriteFinalOutput = 1u << 5
};

struct FeaturePointDescriptor {
    FeaturePointId id;
    PipelineId pipeline;
    StageId anchor_stage;
    FeaturePointPosition position;
    FeatureDomainMask allowed_domains;
    PipelineScopeMask allowed_scopes;
    FeaturePointFlags flags;
};
```

#### `FeaturePointPosition` 的值语义

| 枚举值    | 精确定义                           | 依赖可见性                     | 使用限制                                                             |
|-----------|------------------------------------|--------------------------------|----------------------------------------------------------------------|
| `kBefore` | Feature 插入到锚点 Stage 执行之前  | 可以读取锚点之前已发布的数据   | 不得读取由锚点 Stage 首次写入且未提前发布的 Artifact                 |
| `kInside` | Feature 插入到锚点提供的内部扩展槽 | 由锚点契约明确可见的输入和输出 | 只有声明支持内部扩展的 FeaturePoint 才允许使用；不得自行定义内部顺序 |
| `kAfter`  | Feature 插入到锚点 Stage 完成之后  | 可以读取锚点发布的输出         | 不得将锚点输出改写为隐式覆盖；需要新版本或显式写入契约               |

`FeaturePointPosition` 不是资源访问权限，也不是“替换 Pass”指令。当前契约不提供
隐式 `kReplace` 语义；需要替换基础功能时，应由 Pipeline 提供新的 Definition 或
明确的替代 FeaturePoint。

#### `FeaturePointFlags` 的值语义

`FeaturePointFlags` 是位掩码，可以组合使用。未设置的 bit 表示对应能力被禁止，
`kNone` 表示不授予任何额外能力。

| 标志值                   | 允许的行为                                    | 未设置时的结果                              |
|--------------------------|-----------------------------------------------|---------------------------------------------|
| `kNone`                  | 不授予 FeaturePoint 特殊权限                  | Feature 只能使用基础读取和既有输出约束      |
| `kMayCreateArtifacts`    | 可以声明新的 Artifact 或 RenderGraph 逻辑资源 | 创建新 Artifact 的调用必须失败              |
| `kMayCreateGraphicsPass` | 可以添加 Graphics Pass                        | Graphics Pass 创建必须失败                  |
| `kMayCreateComputePass`  | 可以添加 Compute Pass                         | Compute Pass 创建必须失败                   |
| `kMayCreateCopyPass`     | 可以添加 Copy/Transfer Pass                   | Copy Pass 创建必须失败                      |
| `kMayUseDedicatedQueue`  | 可以请求专用 GPU 队列或执行队列               | 必须使用 Host 分配的默认队列                |
| `kMayWriteFinalOutput`   | 可以写入 FeaturePoint 标记的最终输出          | 只能写临时或中间 Artifact，不得触碰最终输出 |

设置标志只表示“允许请求”，不保证设备或运行模式一定提供对应能力。Compiler 仍需
检查 RHI 能力、队列可用性、Scope 权限和 Artifact 写入规则。未知 bit 必须拒绝，不得
按“未来标志”静默放行。

它至少描述：

- 绑定的公共 Phase 或 Pipeline Stage。
- 插入位置：之前、之后或锚点内部。
- 允许的 Domain 和 Scope。
- 是否允许新增 Artifact。
- 是否允许创建 Graphics、Compute 或 Copy Pass。
- 是否允许使用专用队列。
- 是否允许修改最终输出。

### 8.4 顺序规则

最终执行顺序由以下信息共同决定：

1. Artifact 数据依赖。
2. 资源读写依赖。
3. 显式 `runs_before` / `runs_after` 约束。
4. 执行域和时间域限制。
5. 稳定 ID 作为拓扑等价时的确定性排序依据。

注册顺序不得改变有依赖节点的语义。`runs_before` 不得替代真实的数据契约。

## 9. Artifact 数据模型

### 9.1 Artifact 的作用

Artifact 是 Pipeline、Stage 和 Domain Graph 之间传递数据的正式契约。它不是简单的
全局变量，也不是对某个系统内部容器的别名。Artifact 描述以下信息：

- 数据的稳定身份和类型。
- 谁拥有数据以及它属于哪个 Scope。
- 数据可以存在多久。
- 数据由什么存储介质承载。
- 哪些 Scope 可以读取。
- 版本如何校验。

Pipeline 通过 Artifact 表达依赖后，Compiler 才能推导执行顺序、并行关系、生命周期和
完成点。

### 9.2 ArtifactDescriptor

```cpp
enum class ArtifactStorage : uint8_t {
    kValue = 0,
    kMutableState = 1,
    kImmutableSnapshot = 2,
    kCommandBuffer = 3,
    kCpuResource = 4,
    kGpuLogicalResource = 5,
    kCompletionToken = 6
};

enum class ArtifactLifetime : uint8_t {
    kTask = 0,
    kStage = 1,
    kFrame = 2,
    kFixedTick = 3,
    kWorld = 4,
    kEngine = 5
};

enum class OwnershipPolicy : uint8_t {
    kScopeOwned = 0,
    kGraphOwned = 1,
    kProducerOwned = 2,
    kSharedImmutable = 3,
    kExternalBorrowed = 4
};

struct ArtifactDescriptor {
    ArtifactId id;
    ArtifactTypeId type;
    PipelineScope owner_scope;
    ArtifactStorage storage;
    ArtifactLifetime lifetime;
    OwnershipPolicy ownership;
    uint32_t version;
    PipelineScopeMask visible_to;
};
```

#### `ArtifactStorage` 的值语义

`ArtifactStorage` 描述 Artifact 的承载方式，不描述它属于哪个 Scope，也不描述它何时
失效。存储类型一旦注册，生产者和消费者必须按照同一种布局契约访问。

| 枚举值                | 精确定义                                                       | 适用数据                                       | 关键限制                                                                          |
|-----------------------|----------------------------------------------------------------|------------------------------------------------|-----------------------------------------------------------------------------------|
| `kValue`              | 可复制或可移动的普通值，由 Artifact 系统直接承载               | 时间、计数、配置快照、小型结果                 | 不适合大型数组、GPU 资源或需要独占所有权的数据                                    |
| `kMutableState`       | 由一个 Scope Owner 持有、可被受控任务修改的状态                | ECS 状态、求解器状态、运行时缓存               | 不能被其他 Scope 直接借用；跨 Scope 必须转成 Snapshot、Command 或 Resource Bridge |
| `kImmutableSnapshot`  | 发布后不可变、可被多个消费者只读的数据版本                     | `WorldSnapshot`、Physics Snapshot、RenderScene | 发布后不能原地更新；新内容必须生成新版本或新快照                                  |
| `kCommandBuffer`      | 按约定格式写入并由指定消费者读取/消费的命令序列                | 输入命令、Editor 命令、网络命令                | 必须定义生产窗口、消费窗口和是否允许重复消费                                      |
| `kCpuResource`        | CPU 侧资源或字节数据，通常由 IO/解码任务产生                   | Mesh 数据、纹理压缩数据、配置文件              | 不能直接当作 GPU 资源使用；上传必须通过 Resource/RenderGraph 流程                 |
| `kGpuLogicalResource` | RenderGraph 管理的逻辑 Buffer/Texture，物理对象由 RHI 延迟解析 | Render Target、Storage Buffer、纹理            | 只能通过 RenderGraph Builder/Execution Context 访问，不能保存原生 GPU 指针        |
| `kCompletionToken`    | 表示任务、提交或 GPU 工作完成状态的同步 Artifact               | CPU Task 完成点、Queue Signal、资源退休点      | 不承载业务数据；等待和销毁必须遵守其完成语义                                      |

`kGpuLogicalResource` 的“逻辑”表示资源身份、描述和访问关系，而不是一个已经创建的
`VkImage`、`ID3D12Resource` 或其他后端对象。Compiler 可以根据生命周期和别名分析重新
分配物理资源，但不能改变公开的 Artifact 身份。

#### `ArtifactLifetime` 的值语义

生命周期表示 Artifact 至少在哪个执行边界内保持有效。实际退休时间还要加上所有消费
者和 GPU Completion；因此 `kFrame` 不等同于“Present 调用返回后立即销毁”。

| 枚举值       | 精确定义                                        | 失效边界                             | 典型用途                                               |
|--------------|-------------------------------------------------|--------------------------------------|--------------------------------------------------------|
| `kTask`      | 仅在一个 Task 的执行上下文内有效                | Task 返回且其内部异步工作完成后      | 临时计算结果、Task 私有 scratch 数据                   |
| `kStage`     | 从 Stage 开始到 Stage 完成期间有效              | Stage 完成并且没有未声明消费者后     | Stage 内多个 Task 共享的中间结果                       |
| `kFrame`     | 绑定一个 `WorldFrameKey` 的构建、执行和退休周期 | 该 Frame 的 CPU/GPU 消费全部完成后   | RenderScene、临时 RenderGraph 资源、Frame Context 数据 |
| `kFixedTick` | 绑定一个固定 Tick 序号，而不是整个渲染帧        | 该 Tick 提交或被下一个明确版本替代后 | Physics 中间状态、Tick 事件和确定性模拟结果            |
| `kWorld`     | 绑定一个 World 的完整生命周期                   | World 销毁或显式撤销 Artifact 后     | World 状态、持久实体数据、World 级缓存                 |
| `kEngine`    | 绑定 Engine Runtime 生命周期                    | Engine 关闭并完成所有引用退休后      | 全局配置、只读能力、共享资源索引                       |

生命周期选择规则：

- Artifact 的生命周期不能超过其 `owner_scope` 的生命周期。
- `kTask` 和 `kStage` 只能被当前执行图引用，不能直接暴露给其他 Scope。
- `kFixedTick` 必须带有 World 和 Tick 身份；不能用渲染帧号代替 Tick 号。
- 需要从 World 传到 Frame 的数据必须至少是 `kImmutableSnapshot`，并通过 Bridge 发布。
- GPU 资源即使逻辑生命周期结束，也必须等待最后一次 Queue Completion 后才能退休。

#### `OwnershipPolicy` 的值语义

所有权决定谁负责创建、保持有效和最终销毁；它与访问权限是两个独立概念。一个
Artifact 可以是 `kSharedImmutable`，但仍然只允许指定消费者读取。

| 枚举值              | 所有者                                       | 销毁规则                                     | 典型用途                                 |
|---------------------|----------------------------------------------|----------------------------------------------|------------------------------------------|
| `kScopeOwned`       | `owner_scope` 对应的 Host                    | Scope 退休时由 Host 销毁                     | World 状态、Engine 服务、Frame 公共输出  |
| `kGraphOwned`       | 当前 Domain Graph 或 RenderGraph             | 生命周期分析结束且完成点满足后由 Graph 退休  | Transient Buffer/Texture、Task scratch   |
| `kProducerOwned`    | 当前生产者或发布者                           | 生产者在所有消费者完成后释放或转移           | Snapshot、解码结果、Command Buffer       |
| `kSharedImmutable`  | 一个长期 Owner，消费者只读借用或持有只读引用 | Owner 在所有只读引用和 Completion 结束后销毁 | 只读配置、共享 Mesh/Texture、能力表      |
| `kExternalBorrowed` | Graph/Engine 之外的外部对象                  | 消费者绝不直接销毁；由导入方约定释放/归还    | 外部窗口、交换链图像、外部音频或工具资源 |

`kExternalBorrowed` 要求导入接口同时提供有效期和完成条件；如果外部对象需要由引擎
调用释放回调，应使用显式的 Release/Deleter 契约，而不能把它误标为
`kScopeOwned`。

访问方式不是 Artifact 的全局属性，而是某个 Stage 与 Artifact 之间的绑定属性：

```cpp
enum class ArtifactAccess : uint8_t {
    kRead = 0,
    kWrite = 1,
    kReadWrite = 2,
    kAppend = 3,
    kConsume = 4,
    kPublishSnapshot = 5
};

enum class AccessFlags : uint32_t {
    kNone = 0,
    kOptional = 1u << 0,
    kPreserveContents = 1u << 1,
    kDiscardContents = 1u << 2,
    kExclusive = 1u << 3,
    kDeterministicAppend = 1u << 4
};

struct ArtifactBinding {
    ArtifactId artifact;
    ArtifactAccess access;
    uint32_t required_version;
    AccessFlags flags;
};
```

#### `ArtifactAccess` 的值语义

访问值描述一个 Stage 对一个 Artifact 的操作方式。它不是 C++ 的读写指针权限，而是
Compiler 用来建立数据依赖、生产版本和并行限制的正式声明。

| 枚举值             | 精确定义                                            | 生产/消费语义                              | 并行和版本规则                                                    |
|--------------------|-----------------------------------------------------|--------------------------------------------|-------------------------------------------------------------------|
| `kRead`            | 读取已经存在的指定版本，不修改内容                  | 必须有已发布生产者                         | 多个只读者可以并行；不会产生新版本                                |
| `kWrite`           | 创建或完整替换一个输出版本，不读取旧内容            | 当前 Stage 成为该版本生产者                | 默认需要唯一写者；与 `kDiscardContents` 常一起使用                |
| `kReadWrite`       | 先读取当前版本，再产生修改后的新版本                | 同一 Stage 同时是消费者和生产者            | 通常要求独占写权限；不能与其他写者无序并行                        |
| `kAppend`          | 向可追加序列或集合写入新元素                        | 追加元素而不是替换整个 Artifact            | 多写者必须声明合并顺序；需要稳定结果时设置 `kDeterministicAppend` |
| `kConsume`         | 读取并消费一个 Artifact，通常伴随所有权或可用性转移 | 通常只允许一个最终消费者                   | 消费完成后不能再声明普通 `kRead`，除非 Artifact 契约允许共享副本  |
| `kPublishSnapshot` | 将当前 Owner 状态发布为一个不可变版本               | 发布者生成 Snapshot/版本并开放给目标 Scope | 发布后不可原地修改；跨 Scope 必须有对应 Bridge                    |

访问组合的硬性规则：

- 同一个 Stage 对同一个 Artifact 只能声明一种主访问模式；重复声明必须可合并且结果
  唯一，否则编译失败。
- `kReadWrite` 不等于两个互不相关的节点；它形成一个先读后写的版本边。
- `kAppend` 不自动提供顺序；顺序必须由依赖、合并器或确定性 Append 标志提供。
- `kConsume` 不表示“读取后删除物理资源”；实际销毁仍由 Ownership 和 Completion 决定。
- `kPublishSnapshot` 只能由拥有或被授权发布源状态的 Scope 使用。

#### `AccessFlags` 的值语义

`AccessFlags` 是位掩码，可以与一个 `ArtifactAccess` 组合。标志只补充访问契约，不会
改变主访问值的基本语义。

| 标志值                 | 语义                                           | 允许组合                           | 互斥/错误情况                                         |
|------------------------|------------------------------------------------|------------------------------------|-------------------------------------------------------|
| `kNone`                | 没有额外访问要求                               | 所有访问类型                       | 与其他标志组合时只作为空值；显式设置不是必要条件      |
| `kOptional`            | 输入可能不存在，缺失时 Task 按约定走降级路径   | `kRead`、`kConsume`                | 不能用于必须产出的 `kWrite` 或 `kPublishSnapshot`     |
| `kPreserveContents`    | 写入前必须保留旧内容，允许资源状态和数据被读取 | `kReadWrite`、部分 `kWrite`        | 不能与 `kDiscardContents` 同时设置                    |
| `kDiscardContents`     | 写入前旧内容无意义，可跳过保留和加载           | `kWrite`、部分 Render 写入         | 不能与 `kPreserveContents` 同时设置；不能用于只读访问 |
| `kExclusive`           | 要求该访问在声明范围内拥有独占权限             | `kWrite`、`kReadWrite`、`kConsume` | 若存在未声明的并发读写，Compiler 必须拒绝             |
| `kDeterministicAppend` | Append 结果必须与线程调度和注册顺序无关        | `kAppend`                          | 需要元素排序键或显式合并策略，否则编译失败            |

`ArtifactBinding::required_version` 表示精确要求的 Artifact Schema/数据版本，必须与
`ArtifactDescriptor::version` 匹配；`0` 保留为无效值。需要兼容多个版本时，应注册版本
适配器或显式转换 Task，不能把 `0` 当作“任意版本”。`visible_to` 只表示可见 Scope
权限，不会自动创建 Bridge，也不会授予写权限。

例如，`WorldSnapshot` 是不可变快照，多个 Frame 可以读取它；`GameplayCommandBuffer`
是命令 Artifact，World 在受控阶段消费它；`SceneColor` 是 Frame-owned 的 GPU 逻辑资源，
只能由当前 Frame 的 Render Graph 管理。

### 9.3 运行时 Artifact Key

逻辑名称不得直接作为全局运行时键。实际键必须加入实例身份：

```text
Engine: (ArtifactId)
World:  (WorldHandle, ArtifactId)
Frame:  (FrameNumber, WorldHandle, ViewId, ArtifactId)
Editor: (EditorSessionId, ViewId, ArtifactId)
```

因此 Game World 和 Preview World 可以同时拥有名为 `SceneColor` 的资源而不发生冲突。

### 9.4 Artifact 访问规则

```text
读取：只能读取已由生产者显式发布的 Artifact。
写入：只能写当前 Scope 所拥有的 Artifact。
跨 Scope 写入：必须使用 Bridge、Snapshot 或 Command Channel。
多写者：必须声明合并器、顺序约束或独占所有权。
```

禁止通过以下方式绕过 Artifact 契约：

- 全局可变单例。
- 保存另一个 Scope 的裸指针。
- 将 ECS 内部容器直接传给 Render Thread。
- 用注册顺序暗示资源生产顺序。
- 通过日志、线程局部变量或隐式缓存传递业务数据。

## 10. Pipeline 公共 API

### 10.1 PipelineDescriptor

`PipelineDescriptor` 是 Pipeline 类型的静态元数据：

```cpp
enum class PipelineCategory : uint8_t {
    kCore = 0,
    kGameplay = 1,
    kSimulation = 2,
    kRendering = 3,
    kAudio = 4,
    kStreaming = 5,
    kTooling = 6,
    kCustom = 7
};

enum class Capability : uint8_t {
    kParallelCpuExecution = 0,
    kGpuExecution = 1,
    kGraphicsQueue = 2,
    kComputeQueue = 3,
    kCopyQueue = 4,
    kRayTracing = 5,
    kBindlessResources = 6,
    kShaderReflection = 7,
    kAsyncPipelineCompilation = 8
};

using CapabilityMask = uint64_t;

struct PipelineDescriptor {
    PipelineId id;
    std::string_view name;
    Version version;
    PipelineCategory category;
    PipelineScopeMask supported_scopes;
    FeatureDomainMask domains;
    ExecutionDomain default_execution_domain;
    TimePolicy default_time_policy;
    ModuleId owner_module;
    CapabilityMask required_capabilities;
};
```

#### `PipelineCategory` 的值语义

`PipelineCategory` 是用于注册、检索、诊断和工具展示的分类标签。它不会自动授予
Builder 权限，也不会改变 Pipeline 的 Scope、Domain 或执行线程。

| 枚举值        | 精确定义                                              | 典型内容                           | 不代表什么                           |
|---------------|-------------------------------------------------------|------------------------------------|--------------------------------------|
| `kCore`       | 引擎运行所需的核心生命周期和基础调度 Pipeline         | 初始化、帧边界、公共服务协调       | 不表示可以读取所有 Scope 数据        |
| `kGameplay`   | 面向游戏规则和业务状态的 Pipeline                     | 角色逻辑、AI、任务、脚本           | 不表示一定运行在主线程               |
| `kSimulation` | 对连续状态进行求解或推进的 Pipeline                   | Physics、Animation、粒子模拟       | 不表示只能使用固定 Tick              |
| `kRendering`  | 生成视觉输出或构建渲染工作的 Pipeline                 | RenderExtract、Render、PostProcess | 不表示所有节点都在 GPU               |
| `kAudio`      | 生成或处理音频输出的 Pipeline                         | 声场、混音、播放调度               | 不表示可以直接访问音频设备句柄       |
| `kStreaming`  | 负责外部数据读取、解码和资源发布的 Pipeline           | Mesh/Texture/Shader 流送           | 不表示 IO 结果可以绕过 Artifact 发布 |
| `kTooling`    | 编辑器、诊断、录制和分析用途的 Pipeline               | Editor Overlay、Profiler、Capture  | 不表示可以修改运行时 World 状态      |
| `kCustom`     | 不适合内置分类但遵守公共 Pipeline 契约的用户 Pipeline | 游戏专用管线、插件管线             | 不会放宽任何编译或权限校验           |

一个 Pipeline 只能有一个 `category` 值；一个 Pipeline 需要多个业务领域时，使用
`domains` Mask 表达，而不是把多个 Category 拼接到一个整数中。

#### `Capability` 和 `CapabilityMask` 的值语义

`Capability` 的枚举值是 bit 位索引，`CapabilityMask` 的实际值通过
`1ull << static_cast<uint8_t>(capability)` 构造。空 Mask 为 `0`，表示没有额外能力要求。
这些名称是后端无关的能力类别；Vulkan、D3D12 等后端由 RHI Capability Query 映射到
它们，不能把后端扩展名直接写进 Pipeline Descriptor。

| 枚举值                      | 精确定义                                     | Compiler/RHI 校验内容                                  |
|-----------------------------|----------------------------------------------|--------------------------------------------------------|
| `kParallelCpuExecution`     | 需要 CPU Worker 并行执行能力                 | Scheduler 是否提供满足线程安全约束的 Worker Pool       |
| `kGpuExecution`             | 至少需要一个可提交 GPU 工作的设备            | RHI Device 是否成功初始化并支持目标资源/命令类型       |
| `kGraphicsQueue`            | 需要执行 Graphics/Render Pass 的队列         | 是否存在可用 Graphics Queue 和对应 Command 能力        |
| `kComputeQueue`             | 需要提交 Compute 工作的队列                  | 是否存在 Compute Queue；可与 Graphics Queue 共享或独立 |
| `kCopyQueue`                | 需要独立或可用的 Copy/Transfer 队列          | 资源上传和转移路径是否可用                             |
| `kRayTracing`               | 需要光线追踪管线、加速结构或相关 Shader 能力 | RHI 和 Shader Contract 是否同时满足要求                |
| `kBindlessResources`        | 需要大规模或无固定槽位的资源绑定             | Binding/Descriptor 系统是否提供对应能力                |
| `kShaderReflection`         | 需要从 Shader Contract 获取反射布局          | Shader 编译产物是否包含可验证的 Reflection             |
| `kAsyncPipelineCompilation` | 允许或要求在后台编译 Pipeline/Shader         | 编译线程、缓存和生命周期是否支持异步结果发布           |

能力缺失时，Compiler 必须返回明确的 `kUnsupported` 或能力诊断。不能仅因为后端是
独立显卡、某个 API 名称或某个设备类型就推断能力一定存在。

字段用途：

| 字段                       | 用途                               |
|----------------------------|------------------------------------|
| `id`                       | 稳定机器身份，用于查找、依赖和缓存 |
| `name`                     | 日志、诊断和工具显示               |
| `version`                  | 契约兼容和模块版本检查             |
| `category`                 | 区分生命周期、业务、渲染或工具用途 |
| `supported_scopes`         | 指定可以创建实例的 Scope           |
| `domains`                  | 指定 Pipeline 会贡献的领域图       |
| `default_execution_domain` | 未单独指定时的默认执行域           |
| `default_time_policy`      | 未单独指定时的默认时间策略         |
| `owner_module`             | 标识提供定义的模块                 |
| `required_capabilities`    | 声明所需的引擎或设备能力           |

### 10.2 `Pipeline`

`Pipeline` 是静态定义入口。它描述拓扑，不直接执行某一帧的业务逻辑：

```cpp
namespace aeg::pipeline {

class Pipeline {
public:
    virtual ~Pipeline() = default;

    [[nodiscard]] virtual const PipelineDescriptor& descriptor()
            const noexcept = 0;

    // 仅声明 Stage、Artifact、FeaturePoint 和依赖。
    [[nodiscard]] virtual foundation::Result build(
            PipelineBuilder& builder) const = 0;
};

}  // namespace aeg::pipeline
```

`build()` 的使用约束：

- 只在定义收集或重新编译期间调用。
- 必须是确定性的、可重复调用的声明过程。
- 不创建 GPU 资源，不启动线程，不访问 World 可变状态。
- 不保存 `PipelineBuilder` 引用。
- 不依赖 Vulkan、D3D12 或其他后端原生对象。

### 10.3 StageDescriptor 和 Task

Stage 是 Pipeline 内部的逻辑调度节点，Task 是 Stage 中可执行的工作：

```cpp
enum class TaskFlags : uint32_t {
    kNone = 0,
    kParallelSafe = 1u << 0,
    kOptional = 1u << 1,
    kSideEffect = 1u << 2,
    kMustRun = 1u << 3,
    kMayBlock = 1u << 4,
    kExportsCompletion = 1u << 5
};

struct StageDescriptor {
    StageId id;
    EnginePhase phase;       // 兼容字段：仅在 phase_id 为空时生效。
    ExecutionDomain execution_domain;
    TimePolicy time_policy;
    bool parallelizable;
    PhaseId phase_id;        // 非空时引用 PhaseRegistry 中的内置或自定义 Phase。
};

struct TaskDescriptor {
    TaskId id;
    StageId stage;
    TaskFlags flags;
    TaskExecutionFunction execute;
};
```

#### `TaskFlags` 的值语义

`TaskFlags` 是 Task 级位掩码。标志描述调度器可以采用的优化和必须遵守的约束，不能
替代 Artifact 依赖、资源访问声明或执行域声明。

| 标志值               | 精确定义                                            | Scheduler 行为                                 | 组合限制                                                    |
|----------------------|-----------------------------------------------------|------------------------------------------------|-------------------------------------------------------------|
| `kNone`              | 没有额外 Task 属性                                  | 按普通依赖和执行域调度                         | 可与空标志以外的标志组合                                    |
| `kParallelSafe`      | Task 不访问未声明的共享可变状态，可与同批次任务并行 | 在依赖满足时允许并行提交                       | 仍受 Stage 的 `parallelizable`、Artifact 冲突和队列能力限制 |
| `kOptional`          | 当其输出没有消费者且无外部副作用时可以裁剪          | Compiler 可删除该 Task                         | 不能与不可裁剪的外部副作用语义混用                          |
| `kSideEffect`        | Task 有不能完全由输出 Artifact 表达的外部效果       | Compiler 必须保留并谨慎排序                    | 必须额外声明顺序或完成依赖，不能依赖注册顺序                |
| `kMustRun`           | 即使输出未被消费也必须执行                          | 禁止 Dead-Task Elimination                     | 与 `kOptional` 同时设置是编译错误                           |
| `kMayBlock`          | Task 可能等待外部资源或执行阻塞操作                 | Scheduler 可以据此避免阻塞主线程/Render Thread | 不得在不允许阻塞的执行域使用；GPU Task 不适用               |
| `kExportsCompletion` | Task 会发布可供下游等待的 Completion Artifact       | Compiler 为其建立完成点出口                    | 必须声明 Completion 类型、发布时机和所有权                  |

`StageDescriptor::parallelizable` 是 Stage 级上限：值为 `false` 时，Stage 内 Task 不得
仅凭 `kParallelSafe` 并行；值为 `true` 时，也只有标记为安全且不存在依赖冲突的 Task
才能并行。任何需要固定顺序的行为都必须使用 Artifact 边或显式顺序约束表达。

Stage 负责表达依赖和调度边界，Task 负责实际工作。Task 不应自行等待未声明的任务，
也不应直接修改其他 Task 的数据。

`StageDescriptor::phase_id` 是 Stage 的实际全局 Phase 引用。旧代码可以继续填写
`phase`，Builder 会将其规范化为对应的内置 `PhaseId`；新代码若使用自定义 Phase，必须
填写 `phase_id`，并确保该 ID 已存在于编译所使用的 `PhaseRegistry`。`phase` 与
`phase_id` 同时填写时，以 `phase_id` 为准，但 `phase` 仍应填写一个合法的内置枚举值，
以保持结构化诊断和旧 ABI 消费者的可解释性。

### 10.4 `PipelineBuilder`

`PipelineBuilder` 是定义阶段的受控写接口：

```cpp
class PipelineBuilder {
public:
    [[nodiscard]] PipelineStageHandle add_stage(
            const StageDescriptor& descriptor);
    [[nodiscard]] PipelineTaskHandle add_task(
            const TaskDescriptor& descriptor);
    [[nodiscard]] FeaturePointHandle add_feature_point(
            const FeaturePointDescriptor& descriptor);
    [[nodiscard]] ArtifactHandle declare_artifact(
            const ArtifactDescriptor& descriptor);

    [[nodiscard]] foundation::Result bind_task(
            PipelineTaskHandle task, PipelineStageHandle stage);
    [[nodiscard]] foundation::Result reads(
            PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result writes(
            PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result reads_writes(
            PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result runs_before(
            PipelineStageHandle stage, StageId other);
    [[nodiscard]] foundation::Result runs_after(
            PipelineStageHandle stage, StageId other);
    [[nodiscard]] foundation::Result expose_artifact(
            ArtifactId artifact, PipelineScope target_scope);
};
```

关键规则：

- `add_*()` 返回的 Handle 只在当前 Definition 构建事务中有效。
- `reads()`、`writes()` 声明的是契约，不会复制或立即分配数据。
- `runs_before()` 和 `runs_after()` 只补充顺序信息，不替代 Artifact 读写关系。
- `expose_artifact()` 必须明确目标 Scope，不能隐式广播到所有 Scope。
- Builder 失败时，当前 Definition 不提交，调用方得到带来源信息的 `Result`。

### 10.5 `PipelineInstance`

实例绑定到一个具体 Scope 和 Owner：

```cpp
class PipelineInstance {
public:
    virtual ~PipelineInstance() = default;

    [[nodiscard]] virtual PipelineInstanceId id() const noexcept = 0;
    [[nodiscard]] virtual PipelineScope scope() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result activate(
            const PipelineActivationContext& context) = 0;
    [[nodiscard]] virtual foundation::Result deactivate(
            const PipelineDeactivationContext& context) = 0;
};
```

实例可以保存配置、统计数据和领域内部状态，但不能把自己的可变状态作为公共 Artifact
直接暴露。对外数据必须通过 Artifact Registry 发布。

## 11. PipelineFeature API

### 11.1 基类关系

`PipelineFeature` 是所有 Feature 的多态根接口，负责提供统一的注册、描述、构建贡献
和诊断入口。内置领域基类固定为五个；它们分别形成独立的 Builder 能力边界。具体
Feature 应直接选择一个主领域基类。只有当一个功能在语义上同时产生两个或多个领域
的 Artifact，且这些 Artifact 之间存在明确的跨域数据流时，才使用
`CompositeFeature<Parts...>`。

```text
PipelineFeature
├── LogicFeature
├── PhysicsFeature
├── RenderFeature
├── AudioFeature
├── StreamFeature
└── CompositeFeature<Parts...>
```

上图表示公共继承关系和能力边界，不表示运行时执行顺序。`AnimationFeature`、
`AiFeature`、`ParticleFeature`、`UiFeature` 等是业务侧的具体 Feature 类型，不构成新的
基础继承层级；它们必须根据主要数据语义选择上述领域之一。领域名称不能仅凭功能名称
推断，最终分类以 Feature 的输入、输出和 Builder 使用方式为准。

| 类型                         | 领域标识                                                   | Builder 能力边界                     | 语义职责                                                                | 不应承载的职责                                                       |
|------------------------------|------------------------------------------------------------|--------------------------------------|-------------------------------------------------------------------------|----------------------------------------------------------------------|
| `PipelineFeature`            | 无具体领域                                                 | 仅提供统一元数据、贡献收集和诊断入口 | 作为 Registry 的类型擦除接口和 Feature 生命周期契约                     | 不直接创建领域 Task，不暴露聚合所有领域能力的统一 Builder            |
| `LogicFeature`               | `FeatureDomain::kLogic`                                    | `LogicGraphBuilder`                  | CPU 业务逻辑、状态变换，以及不以像素输出为主要结果的 GPU Compute 工作   | 不直接构造 Render Pass 或持有 World 外部可变指针                     |
| `PhysicsFeature`             | `FeatureDomain::kPhysics`                                  | `PhysicsGraphBuilder`                | 刚体、碰撞、约束、布料和流体等连续状态求解                              | 不直接写入 RenderGraph 资源或绕过 Tick/快照发布协议                  |
| `RenderFeature`              | `FeatureDomain::kRender`                                   | `RenderGraphBuilder`                 | 几何绘制、光照、阴影、后处理和 Frame 输出的声明式构建                   | 不直接操作后端原生对象或维护全局 Image Layout                        |
| `AudioFeature`               | `FeatureDomain::kAudio`                                    | `AudioGraphBuilder`                  | 空间化、混音、DSP 处理、声场和音频事件调度                              | 不直接修改 World 状态或依赖 Render Thread 的隐式时序                 |
| `StreamFeature`              | `FeatureDomain::kStream`                                   | `StreamGraphBuilder`                 | IO、解码、资源上传准备、取消和资源发布                                  | 不在 IO Worker 中录制 RHI Command List，不把未完成资源发布为可用版本 |
| `CompositeFeature<Parts...>` | `FeatureDomain::kComposite`，具体领域由 `domain_mask` 表示 | 按 Part 静态派发到各领域 Builder     | 将多个单领域 Part 组合为一个外部 Feature 身份，并保留各 Part 的领域隔离 | 不作为独立执行领域，不通过多重继承合并多个领域接口                   |

Feature 的继承关系只解决“可以声明哪些类型的贡献”，不决定 Stage 的先后关系。所有
先后、并行、资源可见性和完成条件仍必须通过 Artifact、FeaturePoint、显式顺序约束和
Execution Domain 声明表达。

#### 基类选择和继承约束

- `PipelineFeature` 是类型擦除和生命周期管理所需的抽象根；游戏侧的具体 Feature 不应
  直接继承它，否则其领域能力无法在编译期得到约束。
- 单领域 Feature 必须且只能选择一个领域基类。该限制是能力隔离规则，而不是命名约定。
- `LogicFeature` 中的 GPU Compute 仅限于其主要结果属于逻辑或计算 Artifact 的情形；若
  任务直接生产颜色、深度、阴影或其他 RenderGraph 输出，应选择 `RenderFeature`。
- `PhysicsFeature` 的状态推进必须服从 Tick、版本和快照发布协议；渲染线程不得读取其
  可变求解器状态。
- `RenderFeature` 的所有 GPU 资源访问必须通过 `RenderGraphBuilder` 声明，不能以类成员
  保存跨帧的后端原生资源状态。
- `AudioFeature` 和 `StreamFeature` 的执行结果分别通过 Audio Artifact 和 Resource
  Artifact 发布；二者不能借助全局队列或隐式线程局部状态向其他领域传递数据。
- 多领域职责必须拆分为 Part，并由 `CompositeFeature<Parts...>` 进行组合；不得使用
  多重继承规避领域边界。

### 11.2 `PipelineFeature`

```cpp
class PipelineFeature {
public:
    virtual ~PipelineFeature() = default;

    [[nodiscard]] virtual const FeatureDescriptor& descriptor()
            const noexcept = 0;
    [[nodiscard]] virtual FeatureDomain feature_domain() const noexcept;
    [[nodiscard]] virtual FeatureDomainMask feature_domain_mask() const noexcept;
    [[nodiscard]] virtual foundation::Result collect_contributions(
            FeatureBuildContext& context) const = 0;
    [[nodiscard]] virtual foundation::Result build_render_graph(
            RenderGraphBuilder&) const {
        return foundation::Result::success();
    }
};
```

`collect_contributions()` 必须是非模板虚入口，因为 Registry 只持有类型擦除后的
`PipelineFeature`。领域 Builder 的静态匹配只在内部实现中进行，不能让模板函数成为
Registry 的唯一入口。

`build_render_graph()` 是运行期渲染图声明入口，默认不产生渲染贡献。RenderFeature 在此
创建逻辑资源和 Pass；`collect_contributions()` 则用于 Pipeline 编译时收集 Stage、Task
和 Artifact 契约。两者不是同一次调用，也不得混用各自的 Builder 能力。

### 11.3 五个领域基类

五个领域基类是受控能力接口，而不是仅用于运行时分类的标签类。每个基类固定一个
Builder 类型，并把通用的 `collect_contributions()` 入口适配到领域专属的 `setup()`。
该设计使 Registry 可以通过 `PipelineFeature` 进行类型擦除，同时使领域 Compiler 只
看到本领域允许的声明操作。

领域基类的选择规则如下：

1. 若 Feature 的主要输出是 CPU 状态、命令或非视觉 Compute Artifact，选择
   `LogicFeature`。
2. 若 Feature 的主要输出是物理状态、约束结果或模拟快照，选择 `PhysicsFeature`。
3. 若 Feature 产生绘制、图像处理、渲染资源或 Presentation 输出，选择
   `RenderFeature`。
4. 若 Feature 的主要输出是音频节点、混音结果或空间声学数据，选择 `AudioFeature`。
5. 若 Feature 的主要工作是外部数据摄取、解码、上传准备或资源发布，选择
   `StreamFeature`。
6. 若同一功能同时满足多个领域条件，使用 `CompositeFeature`，并将每一类职责拆分
   为独立 Part；不得通过多重继承把多个 Builder 接口叠加到一个类上。

所有领域基类都必须满足以下共同约束：

- `setup()` 只负责声明节点、Artifact、资源访问和依赖，不执行运行时业务。
- `setup()` 不得保存 Builder、Context 或其内部临时句柄；回调结束后这些对象即失效。
- 领域基类不得向子类暴露其他领域的 Builder；跨领域数据必须通过 Artifact/Bridge
  形成显式版本边。
- `collect_contributions()` 的失败必须携带当前 Feature、Part、Builder 类型和来源位置，
  以便 Compiler 生成可定位诊断。

```cpp
class LogicFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kLogic;
    using BuilderType = LogicGraphBuilder;

    [[nodiscard]] foundation::Result collect_contributions(
            FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(
            BuilderType& builder) const = 0;
};

class PhysicsFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kPhysics;
    using BuilderType = PhysicsGraphBuilder;

    [[nodiscard]] foundation::Result collect_contributions(
            FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(
            BuilderType& builder) const = 0;
};

class RenderFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kRender;
    using BuilderType = RenderGraphBuilder;

    [[nodiscard]] foundation::Result collect_contributions(
            FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(
            BuilderType& builder) const = 0;
};

class AudioFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kAudio;
    using BuilderType = AudioGraphBuilder;

    [[nodiscard]] foundation::Result collect_contributions(
            FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(
            BuilderType& builder) const = 0;
};

class StreamFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kStream;
    using BuilderType = StreamGraphBuilder;

    [[nodiscard]] foundation::Result collect_contributions(
            FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(
            BuilderType& builder) const = 0;
};
```

具体 Feature 只需实现 `descriptor()` 和对应领域的 `setup()`。Builder 的解析、权限
注入、Artifact Registry 访问和底层 Device 绑定由引擎内部完成；Feature 不得自行查找
其他领域 Builder，也不得直接依赖 RHI 后端对象。

### 11.4 FeatureDescriptor

```cpp
struct FeatureDescriptor {
    FeatureId id;
    std::string_view name;
    FeatureDomain domain;
    FeatureDomainMask domain_mask;
    PipelineScopeMask supported_scopes;
    Version version;
    ModuleId owner_module;
};
```

对于普通领域 Feature，`domain` 和 `domain_mask` 都指向一个领域；对于 Composite，
`domain` 为 `kComposite`，`domain_mask` 是全部 Part 领域的并集。

### 11.5 CompositeFeature

`CompositeFeature<Parts...>` 是组合适配器，不是新的业务领域。它通过成员组合承载多个
单领域 Part，并在编译期根据 Part 的 `kDomain` 与 `BuilderType` 完成静态派发。外部
Registry 只看到一个 Composite Feature 身份；内部每个 Part 仍保持独立的领域契约、
FeaturePoint、Artifact 命名空间和诊断来源。

```cpp
template <typename... Parts>
class CompositeFeature : public PipelineFeature {
public:
    static_assert(sizeof...(Parts) > 0);

    [[nodiscard]] foundation::Result collect_contributions(
            FeatureBuildContext& context) const final;

    template <typename Part>
    [[nodiscard]] Part& part() noexcept;

    template <typename Part>
    [[nodiscard]] const Part& part() const noexcept;

protected:
    std::tuple<Parts...> parts_;
};
```

`part<Part>()` 是组合对象内部访问具体 Part 的类型安全入口；它不改变 Part 的领域
身份，也不授予该 Part 访问其他 Builder 的权限。若 Part 需要运行时多态，应将对应
成员替换为 `std::unique_ptr<Domain>`，并由 Composite 明确拥有其生命周期。

推荐的静态派发过程如下：

```cpp
for_each_part(parts_, [&](auto& part) {
    using PartType = std::remove_cvref_t<decltype(part)>;
    using BuilderType = typename PartType::BuilderType;

    if constexpr (builder_is_compatible_with<BuilderType>) {
        auto domain_context = context.for_domain(PartType::kDomain);
        return part.setup(domain_context.builder<BuilderType>());
    }
});
```

上例中的 `for_each_part` 和 `builder_is_compatible_with` 是概念性伪代码，表达的不是
对外 ABI，而是以下不可变的行为契约：

1. 每个 Part 只能向其声明的领域 Builder 收集贡献；不进行运行时 RTTI 猜测。
2. 派发顺序只用于构建期的确定性遍历，不构成最终执行顺序。
3. 任一 Part 返回失败时，整个 Composite 的贡献收集事务必须回滚，不得发布部分图。
4. Part 的 `FeaturePartId`、诊断路径和 Artifact 声明必须保留父 Feature 身份。
5. Composite 的 `domain` 固定为 `kComposite`，`domain_mask` 必须是全部 Part 具体领域的
   并集；不得仅填写 `kComposite`。
6. Composite 不得嵌套为另一个 Composite 的 Part，除非实现同时定义明确的嵌套身份、
   事务和诊断规则；默认实现应拒绝这种用法。

派发过程由引擎内部完成：

```text
CompositeFeature::collect_contributions()
    -> 遍历 tuple 中的 Part
        -> 读取 Part::kDomain 和 Part::BuilderType
            -> 创建该领域的受限 Context/Builder
                -> 调用 Part::setup()
```

成员存储和注册规则：

1. 具体 Part 类型已知且无需运行时替换时，优先使用 `std::tuple<Parts...>` 的值语义。
2. 如果模板参数是抽象领域基类，使用 `std::unique_ptr<Domain>` 保存具体实现，并由
   Composite 独占其生命周期。
3. `std::shared_ptr` 仅在确有跨对象共享所有权需求时采用，不作为默认存储策略。
4. tuple 顺序只决定收集阶段的稳定遍历顺序，不决定 Runtime Plan 的拓扑顺序。
5. 每个 Part 都拥有由父 Feature 限定的 `FeaturePartId`，但不拥有独立的顶层注册身份。
6. 只有顶层 CompositeFeature 具有外部 `FeatureInstanceId`、启停状态和 Registry 绑定。
7. 任意 Part 构建失败时，Composite 的全部贡献均不得提交。
8. 每个 Part 可以绑定不同的 FeaturePoint、Artifact 和 Execution Domain；跨 Part 通信
   必须使用版本化 Artifact 或 Bridge。

跨领域布料功能的使用方式：

```cpp
class ClothPhysicsPart final : public PhysicsFeature {
protected:
    [[nodiscard]] foundation::Result setup(
            PhysicsGraphBuilder& builder) const override;
};

class ClothRenderPart final : public RenderFeature {
protected:
    [[nodiscard]] foundation::Result setup(
            RenderGraphBuilder& builder) const override;
};

class ClothFeature final
        : public CompositeFeature<ClothPhysicsPart, ClothRenderPart> {
public:
    [[nodiscard]] const FeatureDescriptor& descriptor()
            const noexcept override;
};
```

`ClothFeature` 对外只有一个 Feature 身份，对内分别向 PhysicsGraph 和 RenderGraph
贡献节点。Physics Part 发布的 `ClothPoseSnapshot` 必须通过 Artifact/Bridge 供 Render
Part 读取，而不是通过 Part 之间保存裸指针传递。

## 12. FeatureBuildContext 和 Builder 沙箱

### 12.1 FeatureBuildContext

Context 自动携带当前构建位置和权限：

```cpp
class FeatureBuildContext {
public:
    [[nodiscard]] PipelineScope scope() const noexcept;
    [[nodiscard]] PipelineId pipeline_id() const noexcept;
    [[nodiscard]] FeatureInstanceId feature_id() const noexcept;
    [[nodiscard]] FeaturePartId part_id() const noexcept;
    [[nodiscard]] FeaturePointId feature_point() const noexcept;
    [[nodiscard]] const ArtifactRegistry& artifacts() const noexcept;
    [[nodiscard]] DiagnosticSink& diagnostics() const noexcept;

    [[nodiscard]] FeatureBuildContext for_domain(
            FeatureDomain domain) const;
};
```

Context 的作用：

- 自动限定当前 Pipeline、Scope、Feature 和 Part。
- 拒绝读取不属于当前 Scope 的 Artifact。
- 把诊断定位到完整的来源路径。
- 为 Composite 的每个 Part 提供隔离的领域视图。
- 不暴露 World 可变指针、Frame 内部容器或后端原生句柄。

### 12.2 领域 Builder

领域 Builder 相互独立，且不提供聚合所有领域能力的统一接口：

| Builder               | 允许声明的内容                                     |
|-----------------------|----------------------------------------------------|
| `LogicGraphBuilder`   | 逻辑任务、命令、CPU Artifact、逻辑依赖             |
| `PhysicsGraphBuilder` | 求解任务、碰撞数据、固定 Tick 依赖                 |
| `RenderGraphBuilder`  | Render Pass、Buffer/Texture、View、Shader Contract |
| `AudioGraphBuilder`   | 音频节点、声场数据、混音依赖                       |
| `StreamGraphBuilder`  | IO 请求、解码任务、资源发布和取消策略              |

如果一个 Feature 需要两个领域的 Builder，应使用 CompositeFeature，而不是把两个
Builder 同时注入单一领域类。

### 12.3 Context 的有效期

Builder 和 Context 都是短生命周期对象：

- 只在 `Pipeline::build()` 或 Feature 收集回调期间有效。
- 不允许保存到成员变量或异步任务中。
- 不允许跨线程调用。
- 回调返回后，所有临时 Handle 都只能作为已编译 Definition 的内部索引使用。

## 13. Registry、Host、Compiler 和 Scheduler

### 13.1 PipelineRegistry

Registry 保存类型定义、工厂和绑定关系。它不直接改写正在执行的 Plan：

```cpp
enum class RebuildReason : uint8_t {
    kPipelineRegistered = 0,
    kPipelineUnregistered = 1,
    kFeatureBindingChanged = 2,
    kArtifactSchemaChanged = 3,
    kCapabilityProfileChanged = 4,
    kExecutionPolicyChanged = 5,
    kShaderContractChanged = 6,
    kManual = 7
};

class PipelineRegistry {
public:
    [[nodiscard]] foundation::Result register_pipeline(
            PipelineFactory factory);
    [[nodiscard]] foundation::Result unregister_pipeline(
            PipelineId id);

    [[nodiscard]] foundation::Result register_feature(
            FeatureFactory factory);
    [[nodiscard]] foundation::Result unregister_feature(
            FeatureId id);

    [[nodiscard]] foundation::Result bind_feature(
            const FeatureBindingDescriptor& binding);
    [[nodiscard]] foundation::Result unbind_feature(
            FeatureInstanceId feature);

    [[nodiscard]] PhaseRegistry& phase_registry() noexcept;
    [[nodiscard]] const PhaseRegistry& phase_registry() const noexcept;
};
```

`PipelineRegistry::phase_registry()` 返回与该 Registry 绑定的全局 Phase 注册表。Pipeline
定义、Feature 绑定和 Phase 集合分别受各自的注册操作管理；Registry 不会因为新的 Phase
或绑定注册而隐式修改已发布的 `PipelinePlan`。需要把拓扑变更反映到运行时，必须通过
`PipelineScopeHost::rebuild()`，或者交由 `EngineFrameOrchestrator` 在下一帧边界完成。

当前 `PipelineRegistry` 不提供 `request_rebuild()` 公共方法。下表中的 `RebuildReason`
是面向未来重配置系统的结构化原因约定，调用方可以在自己的重配置管理器中使用，但不得
把它当作当前 Registry 已实现的调用接口。

#### `RebuildReason` 的值语义

`RebuildReason` 是 Plan 失效原因的结构化提示。它帮助 Registry 合并重建请求和帮助
诊断定位，但不允许调用方用它绕过 Compiler 的完整校验。

| 枚举值                      | 触发条件                                                          | 通常重建的内容                               | 不会自动发生的行为                      |
|-----------------------------|-------------------------------------------------------------------|----------------------------------------------|-----------------------------------------|
| `kPipelineRegistered`       | 新 Pipeline Definition 注册并可能被当前 Profile 选中              | 目标 Scope 的候选 Pipeline 集合、拓扑和绑定  | 不会自动创建 World 实例，仍需显式绑定   |
| `kPipelineUnregistered`     | 已使用的 Definition 被撤销或模块准备卸载                          | 依赖该 Definition 的所有受影响 Plan          | 不能在旧 Plan 仍执行时立即卸载模块      |
| `kFeatureBindingChanged`    | Feature 新增、移除、启停、移动 FeaturePoint 或优先级改变          | Feature contribution、依赖边、裁剪结果和批次 | 不会修改 Feature 自己的运行时状态       |
| `kArtifactSchemaChanged`    | Artifact 类型、版本、访问方式、Owner、生命周期或可见性改变        | 生产者/消费者连接、Bridge 和生命周期分析     | 不能把旧布局静默当成新布局              |
| `kCapabilityProfileChanged` | RHI 设备、队列、Shader 能力或运行模式能力集合改变                 | 能力验证、Domain 子图、队列依赖和后端计划    | 不能仅按设备名称判断能力是否存在        |
| `kExecutionPolicyChanged`   | 执行域映射、线程策略、队列策略或调度配置改变                      | 批次划分、线程/队列分配和 Completion 关系    | 不改变 Artifact 的业务版本              |
| `kShaderContractChanged`    | Shader Reflection、Binding Layout、Pipeline Layout 或变体哈希改变 | RenderFeature 资源绑定和 RHI Pipeline 计划   | 不改变 EnginePhase 或业务 Artifact 语义 |
| `kManual`                   | 外部缓存失效、调试强制刷新或工具明确要求重新编译                  | 由 Compiler 重新判断的完整 Plan              | 不能作为跳过依赖检查的快捷方式          |

多个原因可以在一个安全窗口内合并；合并后必须保留所有原因的诊断信息。Reason 只表示
“为什么请求重建”，最终是否真的改变 Plan 由 Definition、Feature、Capability 和
Artifact 的内容决定。

Registry 操作分为两类：

| 操作     | 含义                                                      |
|----------|-----------------------------------------------------------|
| 类型注册 | 提供如何创建 Pipeline/Feature 的定义和工厂                |
| 实例绑定 | 将某个 Feature 绑定到具体 Pipeline、Scope 和 FeaturePoint |

注册顺序不应改变最终计划；Compiler 使用稳定 ID 和显式约束形成确定结果。

### 13.2 Scope Host

当前使用 `PipelineScopeHost : public PipelineInstance` 管理一个独立实例的绑定、
ArtifactRegistry、Active/Pending Plan 和退休列表。其主要调用接口为：

```cpp
PipelineScopeHost(PipelineScope scope, PipelineInstanceId instance = {},
                  void* owner = nullptr);
foundation::Result bind_pipeline(PipelineId pipeline);
foundation::Result rebuild(const PipelineRegistry& registry,
                           CapabilityMask available_capabilities = 0);
PipelinePlanHandle active_plan_snapshot() const noexcept;
foundation::Result apply_pending_plan();
foundation::Result collect_retired_plans();
foundation::Result execute(PipelineScheduler& scheduler,
                           PipelineExecutionContext& context) const;
foundation::Result execute_phase(PipelineScheduler& scheduler,
                                 PipelineExecutionContext& context,
                                 PhaseId phase,
                                 PipelinePlanHandle pinned_plan = {}) const;
```

上述为签名摘录。宿主负责 `owner` 的有效期；World 必须分别创建 Host，不能共用可变
ArtifactRegistry。`active_plan()` 返回借用指针，不适合跨重配置长期保存；需要保留
Plan 时应使用 `active_plan_snapshot()`。具体热切换语义见第 20 节。

以下专用 Host 是架构层封装示意，尚不是当前可直接实例化的公共类：

```cpp
class WorldPipelineHost {
public:
    [[nodiscard]] WorldHandle world_handle() const noexcept;
    [[nodiscard]] const PipelinePlan* active_plan() const noexcept;

    [[nodiscard]] foundation::Result bind_pipeline(
            PipelineId pipeline_id);
    [[nodiscard]] foundation::Result bind_feature(
            const FeatureBindingDescriptor& binding);
    [[nodiscard]] foundation::Result rebuild();
    [[nodiscard]] foundation::Result execute(
            const WorldExecutionContext& context);
};
```

对应的 Host 类型：

| Host                 | 所属实例            | 主要职责                         |
|----------------------|---------------------|----------------------------------|
| `EnginePipelineHost` | Engine              | 全局定义、服务和 Engine Pipeline |
| `WorldPipelineHost`  | `WorldHandle`       | Gameplay、Physics 和 World 状态  |
| `FramePipelineHost`  | `WorldFrameKey`     | 快照消费、RenderExtract 和帧图   |
| `EditorOverlayHost`  | Editor Session/View | Overlay 绘制和编辑命令           |

Host 不把所有作用域合并成一个全局对象。这样可以让多个 World 并行运行，并让 Preview
World 与 Game World 拥有完全独立的可变状态。

`execute_phase()` 是 Orchestrator 使用的帧内入口。调用方传入的 `pinned_plan` 若非空，
Host 会在本次调用中使用该不可变快照，而不会读取可能已经切换的 Active Plan；这保证同一
帧的各个 Phase 使用同一个 Plan。未传入快照时，Host 使用当前 Active Plan，并在必要时
尝试应用 Pending Plan。直接使用该接口时，调用方必须按照 `PhaseRegistry::snapshot()`
返回的全局顺序驱动 Phase，并自行保证帧边界和跨作用域 Bridge 已就绪。

### 13.2.1 当前 World/ECS Scope 适配

当前仓库使用 `aeg::ecs::WorldManager` 管理 World 实例，使用
`aeg::world::WorldScope` 将一个具体的 ECS World 绑定到 `PipelineScope::kWorld`：

```cpp
aeg::ecs::WorldManager world_manager;
const auto handle = world_manager.create_world({"GameWorld", true});
const auto state = world_manager.find(handle.value);

aeg::world::WorldScope world_scope(
        state.value,
        pipeline_registry,
        {
                .pipeline = aeg::pipeline::PipelineId("GameplayPipeline"),
        });

if (const auto result = world_scope.initialize(); !result) {
    return result;
}
```

`WorldScope` 的 `std::shared_ptr<ecs::World>` 构造函数是唯一公共构造路径。它保证
Scope、`WorldManager::find()` 和 World Pipeline 访问同一个可变 ECS 实例。`WorldHandle`
仅用于标识和校验，不携带可变 World 状态，不能据此重新构造 World。

组件注册和命令输入如下：

```cpp
world_scope.state().register_component<Transform>(
        aeg::ecs::ComponentTypeId("Transform"), "Transform");
const auto entity = world_scope.state().create_entity();
world_scope.state().emplace<Transform>(entity.value, Transform{});

world_scope.command_buffer().enqueue([entity = entity.value](aeg::ecs::World& world) {
    auto* transform = world.get<Transform>(entity);
    if (transform == nullptr) {
        return aeg::foundation::Result::failure(
                aeg::foundation::ResultCode::kNotFound, "entity is missing");
    }
    transform->position.x += 1.0f;
    return aeg::foundation::Result::success();
});
```

直接修改 ECS 的 API 只能在 World owner thread 使用。命令缓冲本身支持跨线程生产；
`EngineFrameOrchestrator` 在 `PreUpdate` 自动消费 `WorldScope::command_buffer()`，
因此输入、网络和 Editor 不应通过裸指针直接写入组件。

`WorldScope::commit()` 会生成 `WorldSnapshot` 并写入标准
`WorldSnapshot` Artifact。Orchestrator 已注册该 World 时，会在 `WorldCommit` 自动执行
同一操作；调用方通常不需要在普通帧中手工调用 `commit()`。

Frame View 的连接方式如下：

```cpp
aeg::pipeline::PipelineScopeHost frame_host(
        aeg::pipeline::PipelineScope::kFrame,
        aeg::pipeline::PipelineInstanceId("GameWorldMainView"));
frame_host.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline"));

orchestrator.register_world_frame(world_scope, frame_host, /*view_id=*/0);
orchestrator.initialize();
orchestrator.run_frame({});
```

`register_world_frame()` 会为该 View 创建 `WorldFrameKey` 和 World-to-Frame
`ArtifactScopeBridge`。`WorldCommit` 完成后，Bridge 捕获不可变快照；Frame Pipeline
在 `RenderExtract` 中通过声明的 `ArtifactBinding` 读取它。Frame Pipeline 永远不会
获得 `ecs::World&`，也不能绕过 Artifact 合约访问 World 可变存储。

执行上下文中的两个不透明指针具有不同语义：`EngineFrameDesc::user_data` 会原样传递
给本帧执行的每个 `PipelineExecutionContext`，用于承载宿主上下文；
`PipelineExecutionContext::scope_data()` 则由编排器注入当前 Scope 的运行时对象。
World Pipeline 的 `scope_data()` 是 `WorldScope*`，Frame Pipeline 的 `scope_data()` 是
对应 View 的 `WorldFrameScope*`。Task 自身的 `TaskDescriptor::user_data` 仍通过
`PipelineTaskContext::user_data()` 取得。三者均为非拥有借用，调用方必须保证其生命周期
覆盖所有可能的执行与异步完成点。

### 13.2.2 EngineFrameOrchestrator

`aeg::engine::EngineFrameOrchestrator` 是当前实现的统一帧编排入口，位于
`Engine/Runtime/public/engine_frame_orchestrator.hpp`。它以非拥有方式登记多个
`PipelineScopeHost`，负责初始化、Phase 同步、帧内 Plan 固定、Pipeline Runtime 执行、
RenderFeature 构图、RenderGraph 执行、Present 和 Shutdown。

```cpp
aeg::engine::EngineFrameOrchestrator orchestrator(
        registry, scheduler, frame_context, render_graph);
orchestrator.register_pipeline(engine_host);
orchestrator.register_pipeline(world_host);
orchestrator.register_pipeline(frame_host);

if (auto result = orchestrator.initialize(); !result) {
    return result;
}

aeg::engine::EngineFrameDesc frame_desc{};
frame_desc.user_data = frame_user_data;
if (auto result = orchestrator.run_frame(frame_desc); !result) {
    return result;
}

return orchestrator.shutdown();
```

Orchestrator 不拥有传入的 Registry、Scheduler、FrameContext、RenderGraph 或 Host；这些
对象必须在 Orchestrator 整个生命周期内保持有效。Host 在 `begin_frame()` 时按 Scope、
PipelineId 和 InstanceId 进行稳定排序，并固定每个 Active Plan；帧执行期间 Registry
或 Host 的拓扑变化不会替换已固定的 Plan，而是在边界处被检测并要求重建。
World 命令在 `PreUpdate` 由 World Owner 线程消费；`WorldCommit` 为每个已登记 World
生成单调递增 revision 的不可变 `WorldSnapshot`，随后为所有已登记 View 捕获同一提交
边界上的快照。Frame Pipeline 只能经由其 `WorldFrameScope` 的 Bridge 读取该快照。

### 13.3 PipelineCompiler

Compiler 把静态定义和运行时绑定编译为不可变 Plan：

```cpp
class PipelineCompiler {
public:
    [[nodiscard]] foundation::ResultValue<PipelinePlan> compile(
            const PipelineCompileInput& input);
};
```

Compiler 负责：

1. 收集目标 Scope 的 Pipeline、Stage 和 Feature。
2. 展开 Composite 的 Part，并记录来源路径。
3. 验证 ID、版本、Scope、Domain 和能力要求。
4. 验证 Artifact 类型、Owner、访问方式和生产者。
5. 建立 Data、Ordering、Resource 和 Completion 依赖。
6. 检查循环、多写者和跨 Scope 非法访问。
7. 生成稳定拓扑、并行批次和生命周期范围。
8. 保存领域贡献及其实现引用；`PipelineCompiler::compile()` 只生成 Pipeline Plan，
   不直接编译或提交 RenderGraph。统一的 `EngineFrameOrchestrator` 在
   `RenderBuild`/`RenderExecute` 边界调用运行期适配器完成连接。
9. 从 `PhaseRegistry::snapshot()` 同时读取有序 Phase 列表和 revision；如果已知的
   编译期间 Phase 版本发生变化，则返回 `kOutOfDate`，不发布与新旧版本都不一致的 Plan。
10. 输出完整诊断；失败时不发布部分 Plan。

### 13.4 PipelinePlan

当前 Plan 保存 `revision`、Pipeline/Scope 身份、定义版本、编译时
`phase_revision`、`nodes`、`dependencies`、`topological_order`、`batches`、`artifacts`
和 `exposures`。`PipelineNode::phase_id` 是节点实际所属的全局 Phase；其旧的
`EnginePhase phase` 字段仅保留用于兼容和诊断。完整字段定义位于
`Engine/Pipeline/public/pipeline_types.hpp`。

| 字段或类型                            | 使用语义                                                                                     |
|---------------------------------------|----------------------------------------------------------------------------------------------|
| `feature_contributions`               | 记录 Feature 实例、Composite Part、领域、挂载点、Stage 范围及挂载点权限                      |
| `feature_implementations`             | 将每项贡献关联到 `shared_ptr<const PipelineFeature>`，使已编译贡献不受后续 Registry 撤销影响 |
| `definition_owner` / `feature_owners` | 延长编译时定义对象与 Feature 对象的生命周期                                                  |
| `PipelinePlanHandle`                  | `shared_ptr<const PipelinePlan>`；用于获取和持有不可变计划快照                               |
| `phase_revision`                      | 记录该 Plan 所依据的 `PhaseRegistry::revision()`；不匹配时必须在帧边界重编译                 |

通用 Completion 列表、结构化诊断列表及 `PipelinePlanKey` 不属于当前 Plan 的字段。

Plan 是只读对象。执行线程只读取紧凑的索引数组和函数绑定，不在普通帧中修改图结构。

### 13.5 PipelineScheduler

Scheduler 执行 Plan，不重新分析拓扑。它为五种 `ExecutionDomain` 提供实际调度通道：
`kMainThread` 在调用线程串行执行，其余执行域通过 `Concurrency::ExecutorRegistry`
解析到独立的 `Executor`。默认实现使用 `WorkerExecutor`，但 Render Thread 和 GPU
提交域可以在后续由宿主替换为专用执行器。`kGpu` 的 Task 回调是 CPU 侧的 GPU
编排/提交入口；真正的 Shader 和 GPU 指令在 RHI 提交后由设备执行，不能把 C++ 回调
误认为在 GPU 线程上运行。

```cpp
struct PipelineSchedulerDesc {
    uint32_t cpu_worker_count;
    uint32_t io_worker_count;
    uint32_t render_thread_worker_count;
    uint32_t gpu_worker_count;
};

class PipelineScheduler {
public:
    [[nodiscard]] foundation::Result execute(
            const PipelinePlan& plan, PipelineExecutionContext& context);
    [[nodiscard]] foundation::Result execute_phase(
            const PipelinePlan& plan, PipelineExecutionContext& context,
            PhaseId phase);
    [[nodiscard]] uint32_t worker_count(ExecutionDomain domain) const noexcept;
    [[nodiscard]] foundation::Result set_executor(
            ExecutionDomain domain,
            std::shared_ptr<concurrency::Executor> executor);
    [[nodiscard]] foundation::Result wait_idle(
            uint64_t timeout_nanoseconds = concurrency::kWaitIndefinitely) const;
};
```

一个批次中的节点只有在不存在数据、资源或完成点冲突时才可以并行。Task 不应自行等待
未声明的节点，也不应依赖当前实现偶然形成的执行顺序。`execute_phase()` 只筛选指定
全局 Phase 的节点，但仍按原 Plan 的批次和依赖顺序执行；因此它适合由 Orchestrator
逐 Phase 驱动，而不是替代 Compiler。一个合法 Phase 可能没有任何属于当前 Plan 的
节点，此时执行结果仍为成功；调用方不得把“本 Plan 没有节点”解释为“Phase 未注册”。

调度器通过 `ExecutorRegistry` 获取每个非主线程执行域的执行器，并按已编译批次的域
分组提交：`kCpuWorker`、`kIoWorker`、`kRenderThread`、`kGpu` 均提交
`TaskExecutionFunction`，随后在调用线程执行 `kMainThread` 节点。当前 Pipeline Plan
仍按域的固定顺序收敛，不构成跨域并行保证；同一域内只有同时满足 Batch、Stage 和
Task 并行安全条件的节点才会并行。`kGpu` 的 C++ 回调是 GPU 工作的编排/提交入口，
不是运行在 GPU 上的函数；真实 GPU 执行由 RenderGraph/RHI 的 Queue Submission 完成。

`Concurrency::Completion` 负责暴露异步任务的最终 `Result`，`TaskGroup` 负责批量等待、
首个非取消失败传播和失败后的协作式取消。`Completion::result()` 在任务未结束时返回
`kNotReady`，等待超时返回 `kTimeout`，取消返回 `kCancelled`。Scheduler 不以线程
连接或隐式共享状态表示跨域依赖；节点之间的数据顺序仍必须由 Plan 的 Artifact 和
拓扑约束表达。通用 `Concurrency::TaskGraph` 同样只通过显式前驱依赖推进任务；它
适合 World、Streaming 和 Tooling 的通用任务图，不替代 Pipeline Compiler 对领域
Artifact DAG 的编译与优化。

`PipelineSchedulerDesc` 中计数为 `0` 时使用默认策略：CPU Worker 使用硬件并发度的
至少一个线程，IO、RenderThread 和 GPU 编排域各保留至少一个 Worker。调用方可以在
初始化或安全切换边界通过 `set_executor()` 注入专用执行器；替换不会改变已编译 Plan
的拓扑，但新提交会使用新的执行器，旧执行器由其共享所有权保持到既有任务结束。
Scheduler 的执行接口允许不同 Context 并发调用；同一个 `PipelineExecutionContext`
不得被并发共享。Task 不得递归调用同一个 Scheduler 的执行方法，也不得依赖未声明的
共享可变状态。`wait_idle()` 是生命周期边界操作，会以独占执行闸门阻止新的 Scheduler
执行，等待已进入的执行退出，再等待当前执行器集合空闲；因此适用于模块卸载、执行器
替换和 Runtime 关闭，不应从一个正在运行的 Task 回调中调用。

## 14. ScopeBridge、快照和命令通道

### 14.1 ScopeBridge

跨作用域数据必须通过显式 Bridge：

```cpp
enum class BridgeKind : uint8_t {
    kSnapshot = 0,
    kCommand = 1,
    kResource = 2,
    kCompletion = 3
};

struct ScopeBridgeDescriptor {
    PipelineScope source_scope;
    PipelineScope destination_scope;
    ArtifactId source_artifact;
    ArtifactId destination_artifact;
    BridgeKind kind;
    Version version;
};

class ScopeBridge {
public:
    virtual ~ScopeBridge() = default;

    [[nodiscard]] virtual foundation::Result publish(
            const BridgePublishContext& context) = 0;
    [[nodiscard]] virtual foundation::Result acquire(
            const BridgeAcquireContext& context) = 0;
};
```

#### `BridgeKind` 的值语义

`BridgeKind` 描述跨 Scope 数据边的传输协议。它不表示传输方向；方向由
`source_scope` 和 `destination_scope` 明确给出。

| 枚举值        | 精确定义                                    | 典型方向                             | 必须保证                                          |
|---------------|---------------------------------------------|--------------------------------------|---------------------------------------------------|
| `kSnapshot`   | 发布不可变的数据版本供目标 Scope 读取       | `World -> Frame`                     | 发布后只读、版本可校验、消费者完成前保持有效      |
| `kCommand`    | 将调用方意图编码为目标 Scope 的命令输入     | `Editor -> World`、`Input -> World`  | 命令有稳定类型和版本，由目标 Owner 在受控阶段消费 |
| `kResource`   | 在 Scope 之间传递或导入资源使用权/资源视图  | `Engine -> Frame`、`Frame -> Editor` | 明确借用或所有权转移、物理资源有效期和释放规则    |
| `kCompletion` | 传递 CPU Task、IO 操作或 GPU 提交的完成条件 | `Render -> FrameEnd`                 | 明确谁可以等待、何时满足、何时可以退休相关资源    |

选择规则：

- 数据内容跨 Scope 只读共享时应使用 `kSnapshot`；不得以 `kResource` 代替快照。
- 源 Scope 状态需要修改时应使用 `kCommand`；不得通过 Bridge 传递可变指针。
- `kResource` 只描述资源使用权或资源句柄契约，不允许借此绕过 RHI/RenderGraph。
- `kCompletion` 可以附着在其他 Bridge 上，但不能替代实际的数据或资源依赖。
- 一个 Bridge 只能声明一种主 `kind`；如果同时存在数据和完成条件，应建立两个可诊断
  的 Bridge，或使用明确的复合协议类型，而不是让接收方猜测。

Bridge 负责：

- 确认生产者和消费者 Scope。
- 校验 Artifact 类型和版本。
- 表达所有权转移或只读借用。
- 表达 CPU、IO 或 GPU 完成条件。
- 在诊断中提供跨 Scope 的完整路径。

Bridge 不负责隐藏共享可变状态。常见 Bridge 如下：

```text
WorldSnapshotBridge
    World -> Frame

RenderTargetBridge
    Frame Render -> Editor Overlay

EditorCommandBridge
    Editor -> 下一次 World Input/PreUpdate

GpuCompletionBridge
    Render/RHI -> FrameEnd/Resource Retirement
```

### 14.2 WorldSnapshot

Frame 读取 World 时，生产者必须先发布不可变快照：

```text
World mutable state
    -> WorldCommit
        -> WorldSnapshot
            -> RenderExtract
                -> RenderScene
                    -> RenderGraph
```

快照的特点：

- 发布后只读。
- 可以被多个 Frame/View 并行消费。
- 不包含指向 World 可变容器的裸引用。
- 可以包含帧间插值所需的前后状态。
- 其生命周期由引用计数、帧槽或明确的完成点管理。

当前实现要求通过 `WorldScope` 发布的快照属于该 Scope 的 World，且其 revision 必须
等于 World 当前 revision；已发布 revision 不能向后回退。该约束防止旧 Frame 或异步
生产者把过期状态重新写入 World Artifact。快照发布成功后，World Scope 的
`latest_snapshot()` 与 World Artifact 指向同一逻辑版本；Frame Bridge 在 `capture()`
时复制的是 `ArtifactView`，因此后续 World Commit 不会改变已捕获 View 的内容。
每次 `WorldScope::initialize()` 都建立新的 Scope 活跃代际；`shutdown()` 使当前代际
失效并清空尚未消费的命令。已经完成捕获的 `WorldFrameScope` 仍可读取其持有的不可变
Snapshot，但关闭后的 Scope 不得再次 `capture()`，重新初始化也不会使旧 Frame Scope
恢复可捕获状态。一个 World 可以同时绑定多个 View，每个 View 必须使用独立的
`WorldFrameScope` 和 Bridge；Orchestrator 在 Frame 结束时恢复 Frame Host 原先的
Bridge，避免临时绑定泄漏到下一帧或其他 World。

### 14.3 CommandBuffer

命令通道用于把上层意图传回拥有者，而不是直接写入拥有者的数据：

```text
Input / Network / Editor
    -> CommandBuffer
        -> World Input 或 PreUpdate
            -> World mutable state
```

命令应包含稳定类型 ID、版本和执行时所需的值数据，不应包含跨线程长期保存的对象指针。

## 15. RenderFeature、RenderGraph 和 RHI 的使用边界

### 15.1 RenderFeature 的职责

RenderFeature 是 Render Domain 的扩展单元，负责声明渲染功能，不直接承担图形后端调用：

1. 选择 RenderPipeline 暴露的 FeaturePoint。
2. 声明需要读取和写入的 Frame Artifact。
3. 通过 `RenderGraphBuilder` 创建一个或多个 Pass。
4. 使用 Shader Contract 获取 Binding Layout 和 Pipeline Layout。
5. 把资源使用表达成 Usage、View 和访问类型。
6. 让 RenderGraph 生成资源生命周期、Barrier 和 Queue Dependency。

一个功能可以同时使用 Compute、Vertex、Fragment 或 Ray Tracing Shader，仍然只对应一个
RenderFeature 身份。

### 15.2 RenderGraphBuilder

当前 `aeg::pipeline::RenderGraphBuilder` 是 Feature 使用的受限适配器，声明于
`Engine/Pipeline/public/pipeline.hpp`。它有两种模式：Pipeline 编译时使用 Stage/Task/Artifact
接口；`build_render_graph()` 调用期间使用资源和 Pass 接口。`is_runtime()` 区分这两种模式。

| API                                                                        | 作用与调用要求                                                                                 |
|----------------------------------------------------------------------------|------------------------------------------------------------------------------------------------|
| `create_buffer(id, desc, lifetime)` / `create_texture(id, desc, lifetime)` | 使用 RHI 描述符声明资源，返回 `ResultValue<RenderGraphBuffer/Texture>`；可省略 ID 创建匿名资源 |
| `import_buffer()` / `import_texture()`                                     | 将外部 RHI 资源及初始状态导入图，不转移底层资源所有权                                          |
| `import_swapchain_image()`                                                 | 导入已获取的交换链图像，使用 `FrameAcquireResult`、纹理描述符和初始状态                        |
| `find_buffer(id)` / `find_texture(id)`                                     | 按 `RenderResourceId` 查询已登记资源；不声明任何 Pass 读写关系                                 |
| `export_buffer(handle)` / `export_texture(handle)`                         | 将逻辑资源标记为外部可观察输出，参与存活分析；不返回物理资源                                   |
| `add_graphics_pass()` / `add_compute_pass()` / `add_copy_pass()`           | 接收名称、Setup、Execute 和 Queue，返回 `ResultValue<RenderGraphPass>`                         |
| `feature_point_flags()` / `allows()`                                       | 查询当前挂载点授予的能力                                                                       |
| `user_data()`                                                              | 取得宿主借用的数据指针；不自动提供 Artifact 访问或类型检查                                     |

`RenderResourceId` 是渲染资源命名空间中的 ID，不等同于 `ArtifactId`。同一次 Runtime
构建中，命名 Buffer 与 Texture 共用 ID 唯一性约束；重复 ID 返回 `kInvalidState`，
查询未登记 ID 返回 `kNotFound`。读写声明应置于 Setup 回调，通过
`RenderPassBuilder::read()`、`write()`、`read_write()` 或附件接口完成。

`RenderGraphResourceLifetime` 的值定义如下：

| 值            | 语义                                                           |
|---------------|----------------------------------------------------------------|
| `kTransient`  | 图管理的临时逻辑资源，参与瞬态分配；默认值                     |
| `kPersistent` | 图管理的持久资源，不按临时资源生命周期复用                     |
| `kImported`   | 外部拥有的资源，必须通过 import 接口登记，不能传给 create 接口 |

`RenderGraphPassType::kGraphics`、`kCompute`、`kCopy` 分别表示绘制、计算和复制声明类别。
当前类别用于选择挂载点权限，不能视为命令录制的完整隔离机制。三个专用 add 接口的默认
Queue 分别为 `kGraphics`、`kCompute`、`kCopy`；后两者请求独立队列时还需要
`kMayUseDedicatedQueue`，否则必须显式选择获准的 Queue。

创建和导入资源需要 `kMayCreateArtifacts`；创建 Pass 需要对应的
`kMayCreateGraphicsPass`、`kMayCreateComputePass` 或 `kMayCreateCopyPass`；导出资源及
声明 Present 使用需要 `kMayWriteFinalOutput`。权限不足返回 `kUnsupported`。
该权限目前不等同于对某个指定最终输出纹理的所有写操作进行对象级授权。

这些接口只声明逻辑资源和访问关系，不返回 `VkImage`、`VkBuffer` 或
`ID3D12Resource*`。Execute 回调使用 `RenderPassExecutionContext` 解析 RHI Buffer、Texture
及 View，通过 `device()` 和 `command_list()` 录制后端无关命令。该 Context 不提供
`bind_shader(path)` 或直接的 `dispatch()` 便利接口；Shader/布局须由 Shader Contract
及 RHI Pipeline 绑定流程提供。

当前仓库的 `RenderGraph`、`PassBuilder`、`PassExecutionContext`、`FrameContext` 和
`RenderGraphPlan` 已经提供了这类适配器所需的底层能力：

- 逻辑 Buffer/Texture 创建和外部资源导入。
- Pass 读、写、读写及附件声明。
- Transient 生命周期分析。
- `RenderGraphBarrierPlan` 和 `RenderGraphQueueDependency`。
- Frame Slot、Command List、提交、呈现和 GPU Completion。

### 15.2.1 RenderGraphFeatureRuntime

`aeg::render_graph::RenderGraphFeatureRuntime` 定义于
`Engine/RenderGraph/public/render_feature_runtime.hpp`，连接 Pipeline Plan 与具体图。
依赖方向为 `aeg_render_graph -> aeg_pipeline`，Pipeline 不反向依赖 RenderGraph 实现。

宿主创建 Runtime 时提供 `RenderGraph&`、`const PipelinePlan&`、
`const PipelineRegistry&` 和可选 `RenderGraphFeatureRuntimeDesc`，随后调用 `build()`。
一次性调用可使用 `build_render_graph_features(graph, plan, registry, desc)`。

| 输入                    | 生命周期与含义                                                                                     |
|-------------------------|----------------------------------------------------------------------------------------------------|
| `buffers` / `textures`  | 借用的绑定 span，将公开 `RenderResourceId` 关联到当前图已有句柄；绑定数组须保持有效直到 build 返回 |
| `user_data`             | 非拥有指针；构图期间必须有效，若被 Execute 回调捕获则须持续到相关执行完成                          |
| Graph / Plan / Registry | 借用引用，必须覆盖 Runtime 构建调用；不要直接解引用临时 Plan 快照后立即丢弃其持有者                |

Runtime 优先使用 Plan 保存的 Feature 实现；记录缺失时才尝试 Registry 查询。
Pass 回调持有 Feature 实现引用，因此 Registry 撤销不会立即销毁仍被图引用的对象。
这并不自动保证动态库代码或捕获的外部资源存活。

`build()` 只展开逻辑声明，不调用 `RenderGraph::compile()` 或 `execute()`。成功后不得
在同一个 Runtime 上再次 build，重复调用返回 `kInvalidState`。失败时回滚本次新增的资源、
Pass、导出标记及编译计划状态，可以重试；没有图变更时保留原编译计划。
该事务不撤销 Feature 对外部对象的修改，也不涵盖直接访问图进行 reset、compile、execute
或并发修改；调用方必须在构图期间排除这些操作。失败构建返回过的临时句柄必须全部丢弃，
不得跨重试保留或使用。

跨图或不再存在的资源绑定返回 `kNotFound`；字段不完整的绑定返回 `kInvalidArgument`。
回滚不销毁导入的外部 RHI 资源。低层 API 的调用方在 `build()` 成功后仍必须显式调用
`RenderGraph::compile()` 和 `RenderGraph::execute()`；这是便于工具、离屏渲染和特殊
提交策略使用的原始控制路径。

常规 Engine 帧应使用 `aeg::engine::EngineFrameOrchestrator`。Orchestrator 在
`RenderBuild` 阶段自动以帧内固定的 `PipelinePlanHandle` 调用该 Runtime，在
`RenderExecute` 阶段自动调用 `RenderGraph::execute()`；调用方不应在同一帧再次手动
展开相同 Plan，否则会重复添加 Pass 或破坏图的单次构建事务。需要自定义提交、离屏
多图或工具模式时，才直接使用本节的低层 Runtime 与 RenderGraph API。

### 15.3 RenderGraph 的职责

RenderGraph 负责：

- Pass 拓扑和裁剪。
- 逻辑 Buffer/Texture 资源。
- 资源生命周期和临时资源复用。
- Usage 状态推导。
- 显式 Barrier 计划。
- 跨队列依赖和所有权转移。
- 命令录制顺序。

RenderGraph 不负责 Gameplay、Physics、Audio、网络或插件生命周期。

### 15.4 RHI 的职责

RHI 负责提供后端无关的 GPU 能力：

```text
rhi::Device
    -> Buffer / Texture
    -> BufferView / TextureView / Sampler
    -> BindingLayout / BindingSet
    -> Graphics / Compute Pipeline
    -> Command List
    -> Queue Submit
    -> GpuSyncPoint / GpuCompletionSet
```

`rhi::Device` 的公共接口不能出现 Vulkan、D3D12 或其他后端类型。后端专有对象只能存在
于 `Engine/RHI/private/<backend>` 中。

#### RHI 示例中的 `Format` 枚举值

渲染示例中的 `aeg::rhi::Format::kRgba16Float` 表示一个未压缩的 RGBA 纹理格式：

| 枚举值         | 通道布局                    | 每通道类型                    | 每像素大小        | 数值解释                                             |
|----------------|-----------------------------|-------------------------------|-------------------|------------------------------------------------------|
| `kRgba16Float` | `R`、`G`、`B`、`A` 四个通道 | 16 位浮点数（half precision） | 64 bit，即 8 byte | 线性浮点值，不是 UNORM/SNORM，也不自动执行 sRGB 转换 |

该格式的名称只描述数据布局，不保证在当前设备上一定支持所有用途。创建资源前仍需
查询 RHI Format Capability，分别确认是否支持 Sampled、Storage、Color Attachment、
Transfer Source 或 Transfer Destination。把它用于颜色附件时，写入和采样的色彩空间
约定必须由 RenderFeature/Shader Contract 明确，不能根据格式名称猜测。

### 15.5 Shader Contract

Shader Contract 负责 Shader 与资源绑定之间的契约：

```text
HLSL source
    -> compiled shader package
        -> ShaderReflection
            -> ShaderContract
                -> BindingLayout / PipelineLayout
                    -> RHI Pipeline
```

当前 Shader 层的 `ShaderProgram`、`ShaderReflection`、`ShaderContract`、
`ShaderPipelineLayout` 和 `ShaderManager` 属于这一层。Shader Contract 不决定 Feature
在哪个 EnginePhase 执行，也不读取 World 数据。

## 16. ABI 和模块边界

### 16.1 两种 ABI 场景

AEG 的 ABI 需要区分两种使用场景：

| 场景           | 推荐接口          | 说明                                             |
|----------------|-------------------|--------------------------------------------------|
| 引擎与静态模块 | C++ 公共头文件    | 模块使用同一编译器、标准库和构建配置             |
| 动态插件/DLL   | 版本化 C ABI 门面 | 避免跨 DLL 传递 STL、异常和未固定布局的 C++ 对象 |

静态模块可以使用 `Pipeline`、`PipelineFeature`、`PipelineBuilder` 等 C++ 抽象类；动态
插件对外应使用不依赖编译器布局的模块入口，再由宿主适配为内部 C++ 对象。

### 16.2 公共 C++ API 规则

公共 C++ 头文件必须遵守：

- 不包含后端私有头文件。
- 不在公共类型中保存 `Vk*`、`ID3D12*` 或平台窗口对象。
- 使用固定宽度整数表达跨模块的 ID、版本和容量。
- 非拥有引用必须明确生命周期，拥有资源使用 Host/Device 管理。
- 不用异常表达普通运行时失败，使用 `aeg::foundation::Result`。
- 不把注册顺序、内存地址或 vtable 地址当作稳定身份。
- 公共结构体新增字段时只能追加，并同步更新版本规则。

### 16.3 动态模块 ABI

动态模块的入口应采用固定布局和显式大小字段：

```cpp
struct ModuleApiVersion {
    uint32_t major;
    uint32_t minor;
};

struct AbiResult {
    uint32_t code;
    const char* message;
};

struct ModuleApi {
    uint32_t struct_size;
    ModuleApiVersion version;
    const char* name;

    AbiResult (*register_types)(void* registry);
    AbiResult (*unregister_types)(void* registry);
    void (*destroy)(void* module);
};

extern "C" const ModuleApi* aeg_get_module_api();
```

这是 ABI 门面的概念形态，实际导出名和平台可见性宏由模块系统统一定义。规则如下：

- `struct_size` 允许宿主判断结构体是否包含新字段。
- `major` 不兼容时拒绝加载；`minor` 增加时保持向后兼容。
- 字符串、数组和回调必须注明借用或拥有关系。
- 插件不能释放宿主分配的内存，宿主也不能释放插件内部对象。
- 不跨边界传递 `std::string`、`std::vector`、`std::function`、异常或 RTTI 对象。
- 回调执行线程和重入规则必须写入 ABI 契约。

### 16.4 ABI 中的所有权

采用“创建者销毁”规则：

```text
Host creates -> Host destroys
Plugin creates -> Plugin destroys
Registry owns registered definitions
Scope Host owns Pipeline instances
Device owns RHI handles
FrameContext owns frame-local allocations until retirement
```

如果一个 API 返回借用指针或 span，必须同时说明：

- 有效起止时间。
- 是否允许跨线程读取。
- 哪个操作会使其失效。
- 是否需要调用方复制。

### 16.5 ABI 版本和契约版本

模块 ABI 版本与 Pipeline/Feature/Artifact 版本是不同层级：

```text
Module ABI Version       -> 能否加载模块
Pipeline API Version     -> 能否理解 Pipeline 接口
Pipeline Definition Version -> 能否复用定义
Feature Version          -> 能否绑定 Feature
Artifact Schema Version  -> 能否连接生产者和消费者
Plan Revision            -> 当前运行时拓扑版本
```

升级一个 Artifact Schema 不应静默接受旧布局；必须通过兼容适配器、版本转换或明确报错
处理。

### 16.6 公共头文件和私有实现

公共头文件只描述跨模块必须理解的契约，不承担后端实现细节。模块应按以下边界拆分：

```text
public/    -> 调用方可包含的类型、接口、枚举、版本和错误码
private/   -> 模块内部实现、平台对象、锁、缓存和后端适配器
abi/       -> 动态模块入口、固定布局结构体和 C 函数表
```

公共头文件的使用规则如下：

- `public/` 不包含 `private/`，也不通过传递包含间接暴露后端头文件。
- 公共接口优先使用值类型、句柄、span-like 视图和显式上下文句柄。
- 需要扩展的 ABI 结构体将 `struct_size` 放在首字段，新增字段只追加到末尾。
- 私有实现可以使用 STL、智能指针、互斥量和后端原生对象，但这些类型不能出现在
  动态 ABI 的参数、返回值或成员中。
- C++ 虚函数接口只用于同一编译配置下的静态模块；插件边界使用 C 函数表和不透明句柄。

调用方不应通过公共头文件推断对象的内存布局、引用计数方式或后端资源类型。对象的
创建、使用、退休和销毁必须通过所属 Host、Device 或 ABI 函数完成。

### 16.7 ABI 回调和字符串生命周期

ABI 中的回调和字符串必须在接口定义中明确所有权、有效期、线程和重入语义。推荐使用
以下规则：

| 数据                    | 默认语义                   | 调用方行为                              |
|-------------------------|----------------------------|-----------------------------------------|
| `const char*` 输入      | 借用，只在当前调用期间有效 | 不保存指针，需要持久化时复制            |
| `const char*` 输出      | 由提供方借用               | 在约定的模块/对象仍存活期间读取，不释放 |
| `const uint8_t* + size` | 借用只读字节序列           | 只在回调期间读取，不异步保存            |
| `void* user_data`       | 调用方拥有的不透明上下文   | 回调原样传回，不由被调用方释放          |
| `destroy_*` 函数        | 创建模块提供销毁实现       | 必须由同一模块执行销毁                  |

回调契约至少要说明：

- 回调发生在哪个线程或执行域。
- 回调期间是否允许重入 Registry、Host 或模块 API。
- 回调返回后，提供方是否仍可能再次使用 `user_data`。
- 取消、卸载和错误路径中哪些回调保证不会再发生。
- 回调失败如何映射为 `AbiResult`，以及错误消息的有效期。

错误消息默认是借用字符串，不保证跨下一次 ABI 调用仍有效。若调用方需要异步显示或
写入日志，必须在当前调用返回前复制内容。需要由调用方提供输出缓冲区时，接口同时
接收指针、容量和实际写入长度，并明确截断规则；不返回由另一模块分配、却要求本模块
释放的字符串。

卸载模块前，宿主必须先停止新回调、等待已排队回调完成，再调用模块的销毁函数。模块
不能在回调中直接卸载自己，也不能让回调捕获跨卸载边界仍会执行的 C++ 对象或函数地址。

## 17. Pipeline 使用方法

### 17.1 定义一个 Pipeline

游戏代码通过继承 `Pipeline` 声明自己的功能拓扑。定义阶段只描述节点和契约，不执行
实际业务：

```cpp
class WeatherPipeline final : public aeg::pipeline::Pipeline {
public:
    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor()
            const noexcept override;

    [[nodiscard]] aeg::foundation::Result build(
            aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
                .id = aeg::pipeline::StageId("WeatherUpdate"),
                .phase = aeg::pipeline::EnginePhase::kVariableUpdate,
                .execution_domain = aeg::pipeline::ExecutionDomain::kCpuWorker,
                .time_policy = aeg::pipeline::TimePolicy::kFrame,
                .parallelizable = true,
        });

        if (!stage.is_valid()) {
            return aeg::foundation::Result::failure(
                    aeg::foundation::ResultCode::kInvalidArgument,
                    "WeatherUpdate stage could not be created");
        }

        if (const auto result = builder.reads(
                    stage, {aeg::pipeline::ArtifactId("TimeState"),
                            aeg::pipeline::ArtifactAccess::kRead,
                            1,
                            {}});
                !result) {
            return result;
        }

        if (const auto result = builder.reads(
                    stage, {aeg::pipeline::ArtifactId("WorldSnapshot"),
                            aeg::pipeline::ArtifactAccess::kRead,
                            1,
                            {}});
                !result) {
            return result;
        }

        return builder.writes(
                stage, {aeg::pipeline::ArtifactId("WeatherState"),
                        aeg::pipeline::ArtifactAccess::kWrite,
                        1,
                        {}});
    }
};
```

上面的代码表达了四件事：

1. 创建一个属于 `VariableUpdate` 的 Stage。
2. 声明它读取 `TimeState` 和 `WorldSnapshot`。
3. 声明它生成 `WeatherState`。
4. 让 Compiler 根据这些关系决定它与其他节点的顺序和并行关系。

如果需要明确约束，可以继续声明：

```cpp
builder.runs_before(stage, aeg::pipeline::StageId("RenderExtract"));
```

但如果 `RenderExtract` 本身读取 `WeatherState`，通常无需额外声明 `runs_before`；数据
依赖已经足以生成正确顺序。只有无法通过数据表达的纯顺序关系才使用显式约束。

### 17.2 注册 Pipeline 类型

模块在初始化时向 Registry 注册类型：

```cpp
class GameModule final : public aeg::module::Module {
public:
    [[nodiscard]] aeg::foundation::Result register_types(
            aeg::pipeline::PipelineRegistry& registry) override {
        return registry.register_pipeline(
                [] { return std::make_unique<WeatherPipeline>(); });
    }
};
```

注册表示“该类型具备可创建性”，不表示它已经绑定到某个 World。注册失败时模块不得继续
发布依赖该类型的实例。

### 17.3 创建 World Pipeline 实例

创建 World 时，由 `WorldPipelineHost` 根据 `WorldHandle` 创建实例：

```cpp
const aeg::pipeline::WorldHandle world_handle = world.handle();

if (const auto result = world.pipeline_host().bind_pipeline(
            aeg::pipeline::PipelineId("WeatherPipeline"));
        !result) {
    return result;
}
```

同一个 Pipeline 类型可以绑定到多个 World，但每个 Host 都会得到独立的
`PipelineInstance`、Artifact namespace 和运行状态。

### 17.4 注册和绑定 Feature

Feature 类型先注册，再绑定到 Pipeline 公开的 FeaturePoint：

```cpp
registry.register_feature(
        [] { return std::make_unique<WeatherRenderFeature>(); });

registry.bind_feature({
        .feature = aeg::pipeline::FeatureId("WeatherRenderFeature"),
        .pipeline = aeg::pipeline::PipelineId("RenderPipeline"),
        .scope = aeg::pipeline::PipelineScope::kFrame,
        .point = aeg::pipeline::FeaturePointId("PostProcess"),
        .priority = 0,
        .enabled = true,
});
```

绑定失败的常见原因：

- Feature 的 Domain 不在 FeaturePoint 的允许集合中。
- Feature 的 Scope 不在 Pipeline 支持的范围内。
- FeaturePoint 不属于指定 Pipeline。
- Feature 读取了未公开的 Artifact。
- 绑定会造成多写者冲突且没有合并规则。

### 17.5 启动时编译

所有定义和绑定准备完成后，Host 请求编译：

```cpp
const auto plan_result = pipeline_compiler.compile({
        .scope = aeg::pipeline::PipelineScope::kWorld,
        .world = world.handle(),
        .profile = runtime_profile,
        .registry = registry.snapshot(),
});

if (!plan_result) {
    log_pipeline_compile_error(plan_result.result);
    return plan_result.result;
}

world.pipeline_host().install_plan(plan_result.value);
```

安装成功后，运行时只使用已编译的 Plan；普通帧数据变化无需重新调用 Compiler。

## 18. Feature 使用方法

### 18.1 单领域 Feature

单领域 Feature 只继承一个领域基类：

```cpp
class WindFeature final : public aeg::pipeline::LogicFeature {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor()
            const noexcept override;

protected:
    [[nodiscard]] aeg::foundation::Result setup(
            aeg::pipeline::LogicGraphBuilder& builder) const override {
        const auto task = builder.add_task("UpdateWindState");
        return builder.writes(
                task, aeg::pipeline::ArtifactId("WindState"));
    }
};
```

Feature 只负责贡献自己的节点和契约，不负责决定其他 Feature 的执行顺序，也不直接
调用其他 Feature 的方法。

### 18.2 CompositeFeature

跨领域功能使用成员组合：

```cpp
class ClothPhysicsPart final : public aeg::pipeline::PhysicsFeature {
protected:
    [[nodiscard]] aeg::foundation::Result setup(
            aeg::pipeline::PhysicsGraphBuilder& builder) const override;
};

class ClothRenderPart final : public aeg::pipeline::RenderFeature {
protected:
    [[nodiscard]] aeg::foundation::Result setup(
            aeg::pipeline::RenderGraphBuilder& builder) const override;
};

class ClothFeature final
        : public aeg::pipeline::CompositeFeature<
                  ClothPhysicsPart, ClothRenderPart> {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor()
            const noexcept override;
};
```

引擎会自动遍历 tuple 中的 Part，根据 `Part::kDomain` 创建相应的 Builder 视图。游戏侧
代码无需采用多重继承，也无需对同一 Feature 执行重复注册。

### 18.3 Feature 的构建时和运行时

当前 Feature 调用分为三层：

1. `setup()` 在 Pipeline 编译期间声明 Stage、Task、Artifact、访问依赖与执行策略；
   此时不能使用运行期 RenderGraph 资源接口。
2. `build_render_graph()` 在宿主显式执行 Runtime 构图时创建渲染资源和 Pass；
   Pass Setup 同步声明资源访问，不能在此保存局部对象的引用供后续 Execute 使用。
3. Task Execute 使用 `PipelineTaskContext` 访问 Artifact，Pass Execute 使用
   `RenderPassExecutionContext` 解析资源并录制 RHI 命令。逻辑句柄应按值捕获。

下列 `FeatureExecutionContext` 是尚未实现的统一上下文设计，不应替代当前两个执行接口：

```cpp
class FeatureExecutionContext {
public:
    [[nodiscard]] PipelineScope scope() const noexcept;
    [[nodiscard]] const ArtifactView& read(ArtifactId id) const;
    [[nodiscard]] ArtifactWriter write(ArtifactId id);
    [[nodiscard]] const CompletionSet& dependencies() const noexcept;
};
```

不得在 `setup()` 中保存运行时指针，也不得在执行回调中再次修改拓扑。

## 19. 一帧的使用流程

### 19.1 顶层 Pipeline 调度

常规 Engine 帧的推荐入口是 `EngineFrameOrchestrator`。它将 Pipeline Runtime 和
RenderGraph 绑定到同一个帧状态机中，调用方不再需要分别驱动每个 Host、手动固定 Plan
或在 RenderBuild/RenderExecute 之间传递临时所有权：

```cpp
// Registry、Scheduler、FrameContext、RenderGraph 和各 Host 的生命周期
// 必须覆盖 Orchestrator 的整个生命周期。
aeg::engine::EngineFrameOrchestrator orchestrator(
        registry, scheduler, frame_context, render_graph);
orchestrator.register_pipeline(engine_host);
orchestrator.register_pipeline(world_host);
orchestrator.register_pipeline(frame_host);
orchestrator.register_pipeline(editor_overlay_host);

if (auto result = orchestrator.initialize(); !result) {
    return result;
}

aeg::engine::EngineFrameDesc frame_desc{};
frame_desc.user_data = frame_user_data;
frame_desc.acquire_swapchain_image = true;
frame_desc.swapchain = swapchain;
frame_desc.present_acquired_frame = true;
if (auto result = orchestrator.run_frame(frame_desc); !result) {
    return result;
}

return orchestrator.shutdown();
```

`initialize()` 完成 Phase Registry 同步、所有已登记 Host 的必要重建和激活，并执行一次
`Startup`。`run_frame()` 完成以下不可分割的帧内操作：

1. 调用 `FrameContext::begin_frame()`，等待当前帧槽可复用。
2. 复制调用方提供的绑定 span，并在帧边界固定每个 Active `PipelinePlan`。
3. 按 `PhaseRegistry::snapshot()` 的全局顺序执行 `FrameBegin` 至 `FrameEnd` 之间的
   所有 Phase；落在该区间内的自定义 Phase 会自动参与执行。
4. 在 `RenderBuild` 后自动展开当前固定 Plan 中的 RenderFeature。
5. 在 `RenderExecute` 后自动编译并执行 RenderGraph，提交 Queue Batch 和 GPU Completion。
6. 在 `Present` 或 `FrameEnd` 处理 acquired frame，并结束 FrameContext 生命周期。

`present_acquired_frame == true` 时，Orchestrator 在 `Present` Phase 尝试呈现；为 `false`
时则将呈现或取消处理延迟到 `FrameEnd`。如果图已经以 `submits_frame` Batch 提交了
acquired frame，Orchestrator 会确保该帧最终被解析；没有 Present 输出时则允许取消未提交
的 acquired frame。`reset_render_graph` 为真会重置逻辑资源和 Pass；为假只清除 Pass，
适合保留 Persistent/Imported 资源记录并更新当前 Swapchain Image。

需要逐阶段插入调试、工具或自定义控制时，可以使用活动帧区间的低层入口：

```cpp
if (auto result = orchestrator.begin_frame(frame_desc); !result) {
    return result;
}
const auto phase_snapshot = registry.phase_registry().snapshot();
if (!phase_snapshot) {
    return phase_snapshot.result;
}

const auto& phases = phase_snapshot.value.ordered_phases;
std::size_t frame_begin_rank = phases.size();
std::size_t frame_end_rank = phases.size();
for (std::size_t rank = 0; rank < phases.size(); ++rank) {
    if (phases[rank] == aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kFrameBegin)) {
        frame_begin_rank = rank;
    }
    if (phases[rank] == aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kFrameEnd)) {
        frame_end_rank = rank;
    }
}
if (frame_begin_rank > frame_end_rank || frame_end_rank == phases.size()) {
    return aeg::foundation::Result::failure(
            aeg::foundation::ResultCode::kInvalidState,
            "invalid frame phase interval");
}
for (std::size_t rank = frame_begin_rank; rank <= frame_end_rank; ++rank) {
    if (auto result = orchestrator.execute_phase(phases[rank]); !result) {
        return result;
    }
}
```

该手动形式必须保留全局顺序，并且必须让 `FrameEnd` 作为最后一个普通帧 Phase 执行。
帧前置和帧后置自定义 Phase、启动区间及关闭区间由 `run_frame()`、`initialize()` 和
`shutdown()` 自动处理；正常业务不应同时调用 `run_frame()` 和逐 Phase 接口。任一阶段
失败都会进入 Abort 路径；如果底层 GPU 等待或取消仍未完成，Orchestrator 会保留活动帧
状态，调用方必须先完成恢复或再次执行 `shutdown()`，不得开始下一帧。

### 19.2 RenderGraph 执行顺序（低层显式路径）

以下为不使用 Orchestrator 时的低层调用方式。示例函数接收已创建的图、已编译 Pipeline
Plan、Registry、FrameContext 及宿主纹理；FrameContext 必须已进入可执行帧状态，纹理
必须属于该图：

```cpp
aeg::foundation::Result build_and_execute(
        aeg::render_graph::RenderGraph& graph,
        const aeg::pipeline::PipelinePlan& plan,
        const aeg::pipeline::PipelineRegistry& registry,
        aeg::render_graph::FrameContext& frame,
        aeg::render_graph::RenderGraphTexture scene_color) {
    const aeg::render_graph::RenderGraphFeatureTextureBinding binding{
        aeg::pipeline::RenderResourceId("SceneColor"), scene_color};
    aeg::render_graph::RenderGraphFeatureRuntimeDesc desc{};
    desc.textures = {&binding, 1};
    if (auto result = aeg::render_graph::build_render_graph_features(
                graph, plan, registry, desc); !result) {
        return result;
    }
    if (auto result = graph.compile(); !result) {
        return result;
    }
    return graph.execute(frame).result;
}
```

Feature 只声明资源使用和绘制行为；资源状态转换、Barrier、跨队列等待和物理句柄解析
由 RenderGraph/RHI 完成。本例不自动呈现，也不等待 GPU 完成。生产调用方应保留
`execute()` 返回的完成点，按 FrameContext 规则退休资源。该函数用于一次构图执行；
重复构图前须由宿主管理既有声明，不能逐帧向同一图无限追加 Feature 资源和 Pass。

当 RenderGraph 的构图来源是 `PipelineScopeHost::active_plan_snapshot()` 时，应优先使用
`PipelinePlanHandle` 重载，使 Pipeline Plan 的所有权与本次图执行建立明确关联：

```cpp
auto plan_snapshot = host.active_plan_snapshot();
if (!plan_snapshot) {
    return aeg::foundation::Result::failure(
            aeg::foundation::ResultCode::kInvalidState, "No active pipeline plan");
}
if (auto result = aeg::render_graph::build_render_graph_features(
        graph, plan_snapshot, registry, desc); !result) {
    return result;
}
if (auto result = graph.compile(); !result) {
    return result;
}
return graph.execute(frame).result;
```

该重载在构图事务成功时将 `PipelinePlanHandle` 转移为 RenderGraph 的待执行所有权；
`execute()` 开始时把该所有权登记到当前 `FrameContext` 的活动帧槽，随后 RenderGraph
释放自身的待执行引用。帧槽只有在其全部 GPU Completion 满足后才释放该引用，因此
`PipelineScopeHost` 可以在后续安全边界生成并切换新 Plan，而旧 Plan 不会在当前帧的
GPU 工作完成前退休。构图失败时，事务回滚同时撤销待执行所有权。

`const PipelinePlan&` 重载仍然是借用语义，不会替调用方建立上述所有权链；如果该 Plan
可能在执行期间被 Host 退休，可以通过显式 `RenderGraphExecutionLease` 将共享所有权
附加到本次执行：

```cpp
const aeg::render_graph::RenderGraphExecutionLease plan_lease(plan_snapshot);
aeg::render_graph::RenderGraphExecuteDesc execute_desc{};
execute_desc.execution_leases = {&plan_lease, 1};
return graph.execute(frame, execute_desc).result;
```

Lease 只在 `execute()` 调用期间借用其 `span`，RenderGraph 会将其中的共享所有权
复制到活动 `FrameContext` 帧槽；帧槽回收前，调用方无需继续持有 Lease 对象。对于不
属于 RenderGraph 执行的其他 CPU/GPU 对象，仍可直接使用
`FrameContext::retain_until_complete()`。RenderGraph 本体、Pass 回调捕获的 Feature
实现、Imported 资源以及动态库模块的生命周期仍须分别管理；Plan 保活不等价于这些对象
的自动保活。同一 RenderGraph 若跨多个 Frame 重复执行，宿主必须为每个 Frame 重新建立
计划保活关系，不能假定一次构图的待执行所有权会自动覆盖后续 Frame。

### 19.3 当前底层 RenderGraph 的对应关系

架构层概念与当前仓库 API 的对应关系：

| Pipeline 层概念                | 当前底层对象                                 |
|--------------------------------|----------------------------------------------|
| `pipeline::RenderGraphBuilder` | `RenderGraphFeatureRuntime` 提供的后端适配器 |
| Render Pass                    | `RenderGraph::add_pass()`                    |
| 资源读取/写入                  | `PassBuilder::read()` / `write()`            |
| 帧执行上下文                   | `render_graph::FrameContext`                 |
| 编译后渲染计划                 | `RenderGraphPlan`                            |
| GPU 完成点                     | `GpuSyncPoint` / `GpuCompletionSet`          |
| 执行期所有权 Lease             | `RenderGraphExecutionLease`                  |
| 后端设备                       | `rhi::Device`                                |
| 统一帧编排                     | `engine::EngineFrameOrchestrator`            |

## 20. 动态重配置和卸载

### 20.1 启停 Feature

当前 Feature 的绑定变更与 Plan 重建必须显式衔接。下例中 Host 已完成绑定、初始编译和激活，
调用方负责选择不与该实例执行冲突的重配置边界，并检查每项 Result：

```cpp
if (auto result = registry.bind_feature(binding); !result) {
    return result;
}
if (auto result = host.rebuild(registry); !result) {
    return result;
}
return host.apply_pending_plan();
```

禁用使用 `unbind_feature(instance)` 后重建。Registry 变更本身不会更新已编译 Plan。
Host 未激活时，`rebuild()` 安装 Active Plan；已激活时只暂存 Pending Plan。
`execute()` 在执行前也会尝试应用 Pending Plan，应用失败则返回错误，不继续执行旧计划。

使用 `EngineFrameOrchestrator` 时，调用方不需要在每个 Phase 手动调用
`apply_pending_plan()`：Orchestrator 在初始化以及每个帧边界同步 Phase Registry 时，会
为所有已登记 Host 检查 Active Plan 的 `phase_revision`，必要时调用 `rebuild()` 并应用
Pending Plan。随后 `begin_frame()` 固定新的 Plan 快照，当前帧不会观察到中途切换。
若 Registry 在帧执行过程中发生变化，Orchestrator 在下一阶段边界返回状态错误并中止
当前帧；它不会把新 Phase 拓扑部分混入旧帧。

```text
Registry change -> rebuild -> Pending Plan
                            -> apply_pending_plan -> Active Plan
Old Active Plan -> Retired Plans -> collect_retired_plans
```

| 状态或操作                                | 当前行为                                                              |
|-------------------------------------------|-----------------------------------------------------------------------|
| 没有 Pending Plan                         | `apply_pending_plan()` 返回 `kNotReady`；execute 将此视为无待应用变更 |
| 旧 Plan 仍有外部快照且新契约不兼容        | 拒绝切换，返回 `kInvalidState`，保留 Pending Plan                     |
| Artifact 契约替换受活动 Writer 等约束阻止 | 返回错误，不将 Pending Plan 发布为 Active                             |
| 成功切换                                  | 替换 Artifact 契约并更新计划引用，将旧计划放入退休列表                |
| `collect_retired_plans()`                 | 仅尝试清理无外部 Plan 引用的退休项，同时调用 Artifact 退休逻辑        |

Plan 锁协调 Host 的计划切换与 CPU 执行，引用计数负责对象生命周期；两者均不表示 GPU
完成。当前 Host 不轮询或等待 GPU Completion。调用方必须单独管理仍在执行的
RenderGraph、资源、模块代码和完成点，再释放相关持有者。`request_rebuild()` 自动通知、
通用 CPU/GPU 完成门控与动态库卸载管理仍属于架构约定。

### 20.2 卸载模块

#### 帧槽关联的共享所有权

当前 `FrameContext::retain_until_complete(std::shared_ptr<const void> owner)` 将共享对象
登记到活跃帧槽。它可直接接收 `PipelinePlanHandle`，也可用于资源集合或模块生命周期
对象，不引入 Pipeline 到 RHI 的反向依赖。调用方必须在相关提交之前登记并检查 Result：

```cpp
auto snapshot = host.active_plan_snapshot();
if (!snapshot) {
    return aeg::foundation::Result::failure(
            aeg::foundation::ResultCode::kInvalidState, "No active pipeline plan");
}
if (auto result = frame.retain_until_complete(snapshot); !result) {
    return result;
}
// Build and execute the graph using this same snapshot.
```

`frame` 必须已经成功 begin_frame。必须使用保活的同一个快照构图，不能在登记后重新获取
可能已经切换的 Active Plan。帧槽持有的引用阻止旧 Plan 被 `collect_retired_plans()` 提前
清理；槽位释放引用后，调用方仍需再次调用 collect 完成 Host 退休列表清理。

| 场景            | 语义                                                                   |
|-----------------|------------------------------------------------------------------------|
| 无活跃帧        | 返回 `kInvalidState`，不登记对象                                       |
| 没有共享控制块  | 返回 `kInvalidArgument`；具有控制块的空 alias 指针可正常保活           |
| 登记分配失败    | 返回 `kOutOfMemory`；调用方不得继续提交依赖该对象的工作                |
| `end_frame()`   | 不释放登记对象，也不等待 GPU                                           |
| 帧槽复用        | 等待该槽全部已记录队列完成点，重置命令列表并处理延迟销毁后释放引用     |
| `abort_frame()` | 等待已提交工作并清理命令和延迟资源后释放引用；中途失败则保留引用       |
| 正常析构        | 在设备等待及资源清理之后释放剩余引用；设备必须比 FrameContext 存活更久 |

登记采用追加容器，不进行逐项去重；重复登记会增加共享引用计数，但不会重复销毁对象。
容器在槽位回收后保留容量，减少重复分配。该接口与其他 FrameContext 修改操作一样，必须
由调用方串行使用，不提供并发登记保证。对象析构在回收调用线程执行，析构函数不得抛出
异常或重入当前 FrameContext。

本机制只覆盖经当前 FrameContext 提交并记录的工作，不覆盖直接调用 Device 提交、其他
FrameContext 的工作或未纳入持有对象的裸指针。Plan 保活不自动保活 RenderGraph 对象、
外部资源和 DLL；需要分别持有实际所有者。发生设备丢失时不能将析构路径解释为 GPU
成功执行证明；调用方应按设备故障恢复策略处理。

模块卸载前必须满足：

1. 停止接受新的实例和 Feature 绑定。
2. 从 Registry 撤销类型。
3. 等待所有相关 Scope 的 Task、Frame 和 GPU 工作完成。
4. 销毁 PipelineInstance、FeatureInstance 和 Domain Graph 状态。
5. 最后卸载模块代码和资源。

不能在模块仍有执行回调或 GPU 相关延迟销毁项时卸载 DLL。

## 21. 哪些内容应该设计为 Pipeline

### 21.1 适合使用 Pipeline 的功能

一个功能通常在满足以下条件时适合设计为 Pipeline：

- 有稳定的功能边界。
- 有明确的输入和输出 Artifact。
- 有确定的执行时机或时间策略。
- 需要和其他工作建立先后或完成依赖。
- 可能需要并行、裁剪、统计或独立启停。
- 需要被不同运行模式选择或替换。

典型例子：

```text
Gameplay Pipeline
Simulation/Physics Pipeline
Animation Pipeline
RenderExtract Pipeline
Render Pipeline
Audio Pipeline
Streaming Pipeline
Networking Pipeline
Editor Pipeline
Game-specific Pipeline
```

### 21.2 不应伪装为 Pipeline 的基础服务

以下对象为 Pipeline 提供能力，但本身不是业务调度节点：

```text
Math
Logging
Result
Allocator / Memory Resource
Task Scheduler
RHI Device
Resource Cache
Shader Manager
Descriptor Allocator
```

把基础服务包装成 Pipeline 会增加无意义的拓扑节点，并让生命周期、配置和依赖变得
模糊。基础服务可以被 Pipeline 使用，但不应因为“所有东西都管线化”而被强行纳入
业务图。

### 21.3 必需、默认和可选

“是否为 Pipeline”与“是否必需加载”是两个独立问题：

| 类别             | 说明                                                                   |
|------------------|------------------------------------------------------------------------|
| Runtime Core     | Registry、Compiler、Scheduler、Artifact Registry 和 Host，属于框架基础 |
| Profile Required | 某个运行模式必须具备的职责，例如客户端的 Render Pipeline               |
| Default Module   | 标准发行版默认提供，但可以替换或关闭的模块，例如 Physics               |
| Optional Module  | 按项目需求加载的模块，例如 Navigation、Cutscene、Terrain               |
| User Pipeline    | 游戏项目或插件自行注册的功能                                           |

例如，服务器可以不加载 Render 和 Present Pipeline，但仍然使用同一套 Registry、
Artifact 和 Scheduler；编辑器可以创建多个 Preview World，并分别绑定不同的 Render
Pipeline 实例。

## 22. 生命周期、线程和所有权

### 22.1 Definition 生命周期

```text
创建 Definition
    -> 注册到 Registry
        -> 编译为 PipelinePlan
            -> 被多个 Scope Instance 使用
                -> Registry 撤销
                    -> 等待引用结束
                        -> 销毁 Definition
```

Definition 在注册后应视为不可变。需要修改拓扑时创建新版本，而不是在旧 Definition
上原地修改。

### 22.2 Instance 生命周期

```text
Host 创建 Instance
    -> activate(context)
        -> 绑定 Feature 和 Artifact
            -> 执行若干 Frame/Tick
                -> deactivate(context)
                    -> 等待完成点
                        -> Host 销毁 Instance
```

World Instance 的生命周期由 `WorldHandle` 限定；Frame Instance 的生命周期由
`WorldFrameKey` 和 Frame Slot 限定；不能将一个 Scope 的 Instance 保存到另一个 Scope。

### 22.3 Plan 生命周期

Plan 采用只读和版本化策略：

- Active Plan 可以被多个只读执行者使用。
- Pending Plan 可以在后台编译。
- 安全点替换 Artifact 契约并切换 Plan 引用，不修改旧 Plan 内容。
- 当前 `collect_retired_plans()` 根据外部引用与 Artifact 状态清理计划，不自动等待 GPU。
  涉及 Render Command、GPU 资源和模块代码的生命周期仍须由调用方关联完成点管理。

### 22.4 线程模型

| 对象                 | 默认线程规则                                       |
|----------------------|----------------------------------------------------|
| Registry 写操作      | 配置线程或安全重配置线程                           |
| Definition/Plan 读取 | 可被多个只读线程并行访问                           |
| Builder/Context      | 只在构建回调所属线程使用，不跨线程保存             |
| World 可变状态       | 由 World Owner 线程或其任务图写入                  |
| WorldSnapshot        | 发布后可被多个消费者只读                           |
| RenderGraph 构建     | 在 Render Build 所属线程或明确的构建线程完成       |
| RHI Command List     | 遵守 Device/Queue 的线程规则，由 FrameContext 管理 |
| GPU 资源退休         | 由 Completion/Frame Slot 确认后执行                |

Pipeline 之间优先使用不可变 Snapshot、CommandBuffer 和 CompletionToken，不以全局
互斥锁保护整个 World 或 RenderScene。

### 22.5 指针和引用规则

公共 API 不返回跨 Scope 的长期裸指针。允许的短期引用必须明确：

- 只在当前回调中有效。
- 不允许保存或异步使用。
- 不拥有所引用对象。
- 对象失效后不得继续解析。

资源、World、Frame 和 Graph 对象应使用句柄、Host 或 Device 进行生命周期管理。

## 23. 错误处理和诊断

### 23.1 Result 规则

公共 API 使用现有的 `aeg::foundation::Result` 和 `ResultValue<T>`：

```cpp
foundation::Result result = operation();
if (!result) {
    return result;
}

const auto value_result = create_value();
if (!value_result) {
    return value_result.result;
}
use(value_result.value);
```

`ResultCode` 表示机器可判断的错误类别，`message` 用于诊断，不应被当作稳定错误 ID。
稳定错误分类应通过 `ResultCode` 或专门的诊断 ID 表达。

#### `ResultCode` 的值语义

Foundation 当前公共定义提供以下稳定错误值。底层类型是 `uint8_t`，当前序号一旦进入
ABI 或持久化诊断，不得重新编号；新增错误只能追加。`AbiResult::code` 可以使用相同的
数值，但由于 ABI 字段是更宽的整数，适配层必须明确转换。

```cpp
enum class ResultCode : uint8_t {
    kSuccess = 0,
    kInvalidArgument = 1,
    kInvalidState = 2,
    kUnsupported = 3,
    kNotFound = 4,
    kOutOfMemory = 5,
    kTimeout = 6,
    kDeviceLost = 7,
    kBackendError = 8,
    kNotReady = 9,
    kOutOfDate = 10,
    kSurfaceLost = 11,
    kUnknown = 12,
    kCancelled = 13
};
```

| 枚举值             | 精确定义                                    | 调用方通常如何处理                                 |
|--------------------|---------------------------------------------|----------------------------------------------------|
| `kSuccess`         | 操作已完成，返回值和输出参数有效            | 继续使用返回对象；不得把非空错误消息当作失败       |
| `kInvalidArgument` | 参数为空、范围错误、格式错误或组合不合法    | 修正调用参数后重试；不得对完全相同的请求重复重试   |
| `kInvalidState`    | 对象存在，但当前生命周期/状态不允许该操作   | 按正确顺序调用，或先激活/结束当前操作              |
| `kOutOfMemory`     | CPU、GPU、Descriptor 或其他分配资源不足     | 释放/裁剪资源或降低配置；重试不保证成功            |
| `kUnsupported`     | 当前实现或后端不提供该操作                  | 选择替代路径或裁剪功能                             |
| `kNotFound`        | 请求的对象或稳定 ID 不存在                  | 检查注册、绑定和命名；不得将缺失对象当作空对象     |
| `kTimeout`         | 在约定期限内未完成等待或外部操作            | 记录上下文并决定取消、重试或故障处理               |
| `kDeviceLost`      | GPU Device 或其执行上下文不可继续使用       | 停止提交，进入设备恢复或引擎重启流程               |
| `kBackendError`    | 无法进一步归类的后端执行错误                | 保留后端诊断，不依赖错误文本分类                   |
| `kNotReady`        | 对象存在，但异步结果尚未准备好              | 等待对应 Completion 或稍后重试                     |
| `kOutOfDate`       | 外部状态已变化，当前对象/配置需要刷新或重建 | 重建交换链、缓存或相关 Plan                        |
| `kSurfaceLost`     | Presentation Surface 已失效                 | 停止获取/呈现并重建 Surface/Swapchain              |
| `kUnknown`         | 无法映射到已知错误类别的外部或旧版本错误    | 只能作为兜底诊断，不能被调用方细分解释             |
| `kCancelled`       | 操作被显式或级联取消，未表示参数或后端错误  | 停止消费结果；根据上游状态决定丢弃、重排或重新提交 |

`Result` 的布尔转换只有 `kSuccess` 为真；所有其他值都必须视为失败。调用方可以根据
`ResultCode` 决定恢复策略，但不能依赖 `message` 的具体文本。`ResultValue<T>` 只有在
`result.code == kSuccess` 时才允许读取 `value`。

### 23.2 编译错误

Compiler 必须拒绝以下情况：

- 重复 Pipeline、Stage、Feature、Part 或 Artifact ID。
- 未注册的依赖目标。
- Artifact 类型或版本不匹配。
- 缺少生产者。
- 多写者没有合并、顺序或独占规则。
- 循环依赖。
- Scope 读取和跨 Scope 写入违反矩阵。
- FeaturePoint 与 Feature Domain 不匹配。
- 时间域和执行域冲突。
- 插件引用其他模块的私有 Stage。
- 后端原生类型越过公共 ABI 边界。

### 23.3 诊断内容

每条诊断建议包含：

```text
PipelineId / Pipeline name
FeatureId / FeaturePartId
StageId / TaskId / FeaturePointId
ArtifactId / ArtifactTypeId
Scope / Domain / ExecutionDomain / TimePolicy
Dependency path
Source module and version
Suggested correction
```

Composite 构建失败时，应保留父 Feature、失败 Part 和对应 Builder 类型，避免只显示一条
无法定位的通用错误。

### 23.4 日志和可观测性

日志使用统一 Logging 系统，应按以下类别记录：

```text
PipelineRegistry
PipelineCompiler
PipelineScheduler
ArtifactRegistry
ScopeHost
RenderPipeline
RenderFeature
PluginLoader
```

高频帧内数据进入 tracing/profiling 缓冲区，默认不逐 Task 输出同步日志。调试工具至少
应能查看：

- Pipeline 和 Stage 拓扑。
- Feature/Part 的挂载位置。
- Artifact 生产者和消费者。
- 实际执行批次和被裁剪节点。
- Scope Bridge 和 Completion 关系。
- RenderGraph Pass、Barrier 和 Queue Dependency。

## 24. 性能和缓存契约

### 24.1 编译与执行分离

拓扑分析、循环检查、批次生成和生命周期分析只在定义或绑定变化时执行。普通帧只更新
Artifact 内容，不重新分析图结构。

### 24.2 计划缓存

Plan Cache 的逻辑键至少包含：

```text
Pipeline Definition versions
Feature binding set
Artifact schema versions
Scope kind and instance policy
Runtime profile
Normalized device capability class
RenderGraph topology
Shader Contract hashes
```

设备能力使用 RHI 提供的通用能力分类，不把 Vulkan 扩展名或 D3D12 对象指针暴露到
Pipeline Cache。

### 24.3 热路径约束

- Plan 执行使用紧凑索引，不在热路径按字符串查找节点。
- 已发布 Plan 只读，避免执行期间锁住拓扑容器。
- Ready 节点按批次提交，减少调度器唤醒次数。
- Composite 的领域匹配在收集/编译阶段完成，执行阶段不遍历所有 Builder。
- World 到 Frame 使用快照和所有权转移，避免锁住整个 ECS World。
- Transient Artifact 依据生命周期复用内存和 GPU 资源。
- 延迟销毁由 Frame Slot 和 GPU Completion 驱动，不调用全局设备空转。

### 24.4 结果确定性

在依赖等价的情况下，Compiler 使用稳定的 PipelineId、StageId、FeatureId 和 ArtifactId
排序。注册顺序、线程调度顺序和对象地址不能改变 Plan 的可观察拓扑。

## 25. 与现有模块的关系

当前仓库中已经存在的模块与本架构的关系如下：

| 模块                 | 架构角色                                                | 公共边界                              |
|----------------------|---------------------------------------------------------|---------------------------------------|
| `Engine/Foundation`  | Result、通用基础类型和错误                              | 不依赖任何图形后端                    |
| `Engine/Logging`     | 统一日志和诊断输出                                      | 供 Runtime/模块使用                   |
| `Engine/Math`        | SIMD 数学、变换和图形基础计算                           | 不参与 Pipeline 拓扑                  |
| `Engine/RHI`         | Buffer、Texture、View、Binding、Pipeline、Command、Sync | 公共接口后端无关                      |
| `Engine/RenderGraph` | FrameContext、资源图、Barrier、Queue Sync               | 只管理 Render Domain 图               |
| `Engine/Shader`      | HLSL 产物、Reflection、Shader Contract、Layout          | 不决定 EnginePhase                    |
| `Engine/Resource`    | Mesh、Texture、上传和 Render Target                     | 通过 RHI/RenderGraph 使用 GPU         |
| `Engine/Concurrency` | Executor、Worker、Completion、Cancellation 和任务批次   | 平台中立；不持有领域状态              |
| `Engine/Pipeline`    | Pipeline、Feature、Registry、Compiler、Scheduler        | 本文档描述的扩展层                    |
| `Engine/Runtime`     | EngineFrameOrchestrator 与统一 Engine 帧生命周期        | 连接 Pipeline Runtime 与 RenderGraph  |
| `Engine/World`       | WorldHandle、WorldScope、ECS 快照和命令                 | World Scope 数据所有者                |

当前专用 Scope Host、Renderer 和部分模块化运行时仍属于架构目标。`WorldScope` 已提供
World/ECS 与通用 `PipelineScopeHost` 的基础适配，但尚未引入独立的
`WorldPipelineHost`/`FramePipelineHost` 公共类型。本文档不会把目标 API 误写成当前已经
存在的头文件；实现时应优先保持这里定义的调用方向和所有权规则。通用 Concurrency
基建已经存在，但 GPU 队列提交适配器仍由 RHI/RenderGraph 负责，不能将默认
WorkerExecutor 视为真实 GPU 执行线程。

## 26. API 快速参考

### 26.1 开发者通常需要实现的内容

| 开发者需求        | 应实现/调用                                                                               |
|-------------------|-------------------------------------------------------------------------------------------|
| 新增功能调度单元  | 继承 `Pipeline`，实现 `descriptor()` 和 `build()`                                         |
| 新增单领域功能    | 继承 `LogicFeature`、`PhysicsFeature`、`RenderFeature`、`AudioFeature` 或 `StreamFeature` |
| 新增跨领域功能    | 继承 `CompositeFeature<Parts...>`，组合具体领域 Part                                      |
| 声明输入输出      | 使用 `PipelineBuilder`/领域 Builder 声明 ArtifactBinding                                  |
| 接入已有 Pipeline | 注册 Feature 类型并绑定到公开 FeaturePoint                                                |
| 使用多个 World    | 由 `WorldManager` 创建独立 World，并为每个 World 创建独立 `WorldScope` 和 Host           |
| 修改运行拓扑      | 通过 Registry 请求重建，在安全点切换 Plan                                                 |
| 增加渲染能力      | 实现 `RenderFeature`，使用 `RenderGraphBuilder`                                           |
| 增加编辑器绘制    | 使用 `FrameOverlayBuilder`，不直接修改基础 RenderGraph                                    |
| 增加动态插件      | 实现版本化 Module ABI，不跨 DLL 传递 STL/异常                                             |

### 26.2 禁止用法

```text
不得在 Pipeline 中直接调用另一个 Pipeline 的 execute。
不得以注册顺序替代 Artifact 依赖。
不得使 Frame 读取 World 可变指针。
不得使 Engine 读取 World/Frame 业务数据。
不得将 Composite 的 Part 再次注册为顶层 Feature。
不得在公共 API 暴露 Vk*、ID3D12* 或平台对象。
不得在 RenderFeature 中手写全局 Image Layout 状态。
不得在执行期间修改当前 PipelinePlan。
不得跨 ABI 边界传递 std::vector、std::string 或异常。
```

## 27. 架构不变量

1. Pipeline 统一的是调度语义，不是所有领域的算法实现。
2. 基础服务不因为形式统一而包装成业务 Pipeline。
3. EnginePhase 是稳定公共锚点，Pipeline Stage 是局部细分，FeaturePoint 是扩展入口。
4. Shader Stage 不是 EnginePhase；Vertex、Fragment、Compute 等只表示 GPU Shader 语义。
5. Pipeline 之间通过 Artifact 和显式依赖通信，不通过隐式函数调用通信。
6. `PipelineDefinition`、`PipelineInstance`、`PipelinePlan` 三者生命周期和职责分离。
7. Engine、World、Frame、Editor 遵守严格的单向数据依赖。
8. World Pipeline 必须绑定 `WorldHandle`，不得采用 Singleton 实例化策略。
9. Frame 只能读取发布后的 World Snapshot，不得读取 World 可变状态。
10. Editor 只能通过 Frame Overlay 和 Command Channel 参与，不得直接改写 World。
11. 所有内置领域 Feature 都继承 `PipelineFeature`。
12. 跨领域功能使用成员组合 `CompositeFeature<Parts...>`，不使用多重继承。
13. Composite 的 Part 不具有独立顶层注册身份。
14. tuple 顺序不代表执行顺序，执行顺序由契约和约束决定。
15. 多写者、跨 Scope 写入和资源所有权转移必须显式声明。
16. 拓扑变化才触发重编译，普通帧数据变化只更新 Artifact 内容。
17. 已发布 Plan 只读，Plan 切换必须等待相关 CPU/GPU Completion。
18. RenderGraph 负责资源生命周期、Barrier 和 Queue Sync；RHI 负责后端无关 GPU 接口。
19. 公共 Pipeline/RHI/Shader API 不得暴露 Vulkan、D3D12 或其他后端专属类型。
20. 诊断必须能够定位到 Pipeline、Feature、Part、Stage 和 Artifact 来源路径。

这套架构的使用关系可以概括为：

```text
注册类型
    -> 创建 Scope Instance
        -> 绑定 Feature
            -> Builder 声明 Artifact 和依赖
                -> Compiler 生成 Plan
                    -> Scheduler 执行 Domain Graph
                        -> RenderGraph / CPU Graph / IO Graph
                            -> RHI
```

本文档是公共架构和使用契约。具体领域功能、实现文件组织和模块加载策略可以演进，
但不能破坏上述 API 边界、数据所有权和依赖方向。

## 28. 公共枚举值索引

本节汇总当前公共头文件和本文档设计 API 中开发者会直接接触的枚举值。前文已经对
Pipeline 层枚举给出了完整语义；本节补充 Foundation、Logging、RHI 和 RenderGraph
的值语义，作为查表入口。

### 28.1 统一使用规则

- 普通枚举表示互斥选择，同一字段只能填写一个值；不能把两个普通枚举值按位或合并。
- 标记型枚举使用位掩码；只有明确启用 bitmask operators 的类型才能使用 `|`、`&`、
  `~` 等运算。
- `kNone` 表示空集合，`kAll`/`kAllCommands` 表示明确的全范围请求，二者不能混淆。
- `kCount`、`kUnknown` 以及本节明确标注的其他哨兵通常不是可提交给执行 API 的有效
  业务值；每个类型的具体规则见对应表格。当前公共枚举没有统一的“无效值”成员，不能
  自行假设某个类型存在这样的成员，也不能把其他枚举的哨兵跨类型复用。
- 普通枚举的数值顺序不是业务语义。跨 ABI 或持久化时必须固定数值，并对未知值返回
  `kUnsupported` 或 `kInvalidArgument`。当前 `ResultCode` 没有专门的版本不兼容成员；
  版本不兼容由版本字段和诊断 ID 表达。
- 不同枚举即使底层类型相同也不能互相转换；例如 `ResourceUsage`、`PipelineStage` 和
  `ShaderStage` 分别描述资源状态、执行阶段和 Shader 可见性。

### 28.2 Foundation：`ResultCode`

`ResultCode` 的完整定义和恢复策略见第 23.1 节。当前值的核心区别如下：

| 值                 | 重点语义                     |
|--------------------|------------------------------|
| `kSuccess`         | 操作成功，输出值有效         |
| `kInvalidArgument` | 调用参数或组合不合法         |
| `kInvalidState`    | 对象状态不允许当前操作       |
| `kUnsupported`     | 当前实现不提供该能力         |
| `kNotFound`        | 请求的对象或稳定 ID 不存在   |
| `kOutOfMemory`     | 分配失败                     |
| `kTimeout`         | 等待或外部操作超时           |
| `kDeviceLost`      | GPU Device 不可继续使用      |
| `kBackendError`    | 无法进一步归类的后端错误     |
| `kNotReady`        | 异步结果尚未准备好           |
| `kOutOfDate`       | 外部状态变化，需要刷新或重建 |
| `kSurfaceLost`     | Presentation Surface 已失效  |
| `kUnknown`         | 兜底未知错误                 |
| `kCancelled`       | 操作被显式或级联取消         |

### 28.3 Logging：`Level` 和 `Category`

```cpp
enum class Level : uint8_t {
    kTrace,
    kDebug,
    kInfo,
    kNotice,
    kWarning,
    kError,
    kCritical,
    kOff
};

enum class Category : uint8_t {
    kCore,
    kRhi,
    kRenderGraph,
    kResource,
    kApplication,
    kShader,
    kAsset,
    kTest,
    kCount
};
```

#### `Level` 的值语义

Level 按严重程度从详细到严重排列。运行时阈值设为某个值时，通常保留该值及更严重的
消息；`kOff` 禁止该类别的运行时输出。编译期宏过滤仍可能在运行时阈值之前删除消息。

| 枚举值      | 精确定义                     | 适用场景                         |
|-------------|------------------------------|----------------------------------|
| `kTrace`    | 最细粒度的执行轨迹           | 高频调试路径、资源状态和单步诊断 |
| `kDebug`    | 面向开发调试的详细信息       | 参数、拓扑、缓存命中和调试分支   |
| `kInfo`     | 正常运行的重要状态           | 初始化完成、模块加载、关键配置   |
| `kNotice`   | 值得关注但不表示异常的事件   | 发生降级、重建或非默认选择       |
| `kWarning`  | 可恢复的异常或风险           | 缺失可选资源、性能降级、兼容回退 |
| `kError`    | 当前操作失败，但进程可能继续 | 单个资源、Pass 或请求失败        |
| `kCritical` | 影响核心运行能力的严重错误   | Device Lost、核心服务不可用      |
| `kOff`      | 关闭该类别的运行时日志       | 性能测试或明确禁止输出时使用     |

#### `Category` 的值语义

| 枚举值         | 日志归属                                        | 不应记录的内容                           |
|----------------|-------------------------------------------------|------------------------------------------|
| `kCore`        | Engine 生命周期、Registry、Scheduler 和基础服务 | 领域私有细节应使用更具体类别             |
| `kRhi`         | Device、资源、Command List、Queue 和同步        | 不记录 Gameplay 状态                     |
| `kRenderGraph` | Pass、资源图、Barrier、批次和裁剪               | 不替代高频 GPU Trace                     |
| `kResource`    | Mesh、Texture、上传、缓存和资源流送             | 不记录底层 RHI 每条命令                  |
| `kApplication` | 游戏应用和宿主层事件                            | 不用于引擎内部错误分类                   |
| `kShader`      | Shader 编译、Reflection、Contract 和变体        | 不输出完整 Shader 源码或敏感路径         |
| `kAsset`       | Asset 解析、依赖和版本                          | 不把内容解析失败伪装成 RHI 错误          |
| `kTest`        | 测试、验证和 Smoke 场景                         | 正式运行时不应依赖该类别                 |
| `kCount`       | 类别数量哨兵，用于数组容量计算                  | 不能传给 `set_category_level()` 或日志宏 |

`kDefaultRuntimeLevel` 不是枚举值：Debug 构建默认使用 `kDebug`，Release 构建默认使用
`kInfo`。类别覆盖值存放在 `[0, kCount)` 的槽位中。

### 28.4 RHI：Surface、Backend 和 Adapter

#### `SurfaceType`

| 枚举值      | 精确定义                             | 创建要求                                                   |
|-------------|--------------------------------------|------------------------------------------------------------|
| `kWindow`   | 与窗口系统关联、可用于呈现的 Surface | 至少提供后端能识别的非空窗口句柄；句柄只在创建调用期间借用 |
| `kHeadless` | 不依赖窗口系统的离屏 Surface         | 两个 Native Handle 都应为空；不能直接调用 Present          |

#### `BackendType`

当前仓库公共定义为：

```cpp
enum class BackendType : uint8_t {
    kVulkan
};
```

`kVulkan` 表示选择 Vulkan 后端实现，不表示公共 API 可以暴露 Vulkan 类型。未来增加
`kD3D12` 等后端时，应追加新的枚举值并复用同一组 `Device`、资源、同步和错误契约；
不得在 `rhi.hpp` 或 `rhi_struct.hpp` 增加 `Vk*`/`ID3D12*` 字段。

#### `AdapterType`

| 枚举值        | 精确定义                        | 选择策略注意                                   |
|---------------|---------------------------------|------------------------------------------------|
| `kOther`      | 无法归入其他类别的适配器        | 不能直接判定为低性能或不可用                   |
| `kIntegrated` | 与 CPU 共享或紧密关联的集成 GPU | 可能具有较低独立显存，但统一内存访问可能更高效 |
| `kDiscrete`   | 独立 GPU 适配器                 | 只是评分因素之一，不能单独决定选择结果         |
| `kVirtual`    | 虚拟化 GPU 或虚拟设备           | 需要结合能力、内存和队列能力评估               |
| `kCpu`        | CPU 软件实现或 CPU 适配器       | 通常用于兼容、测试或无 GPU 环境                |

适配器选择必须综合 `AdapterType`、内存容量、队列、交换链、所需 Feature 和设备评分；
不能只按 `kDiscrete` 选择。

#### `DeviceFeature` 位值

| 标志值                     | 精确定义                      | 影响                                           |
|----------------------------|-------------------------------|------------------------------------------------|
| `kNone`                    | 没有可选设备 Feature          | 只使用核心 RHI 契约                            |
| `kTimelineSynchronization` | 支持单调递增 Timeline 同步    | 可以使用 `GpuSyncPoint` 和精确的资源退休       |
| `kBindlessResources`       | 支持大规模/无固定槽位资源绑定 | 可以使用 Bindless Binding 和 Unbounded Array   |
| `kGpuVirtualAddress`       | 支持 GPU 可见虚拟地址         | 可以使用 Device Address 和相关 GPU-driven 路径 |

### 28.5 RHI：格式、资源能力和资源状态

#### `Format`

`Format` 是跨后端的数据布局标识。它同时适用于 Buffer Typed View 和 Texture，但实际
用法必须满足资源 Capability 与设备格式能力查询。

| 枚举值            | 数据布局和数值类型                       | 常见用途                                                         |
|-------------------|------------------------------------------|------------------------------------------------------------------|
| `kUnknown`        | 未指定格式；不是可读写的实际布局         | 让 API 推断或表示没有 Typed Format；创建需要格式的资源时必须拒绝 |
| `kR8Unorm`        | 单通道 8 位无符号归一化整数              | 灰度、遮罩、单通道颜色                                           |
| `kRg8Unorm`       | RG 两通道各 8 位无符号归一化整数         | 法线分量、双通道数据                                             |
| `kRgba8Unorm`     | RGBA 四通道各 8 位无符号归一化整数       | 普通线性颜色和颜色附件                                           |
| `kBgra8Unorm`     | BGRA 四通道各 8 位无符号归一化整数       | 窗口/交换链常见颜色格式                                          |
| `kRgba8Srgb`      | RGB 为 sRGB 8 位，A 为 8 位线性分量      | 需要硬件 sRGB 转换的颜色                                         |
| `kRgba16Float`    | RGBA 四通道各 16 位浮点数                | HDR、中间渲染和高动态范围数据                                    |
| `kRgba32Float`    | RGBA 四通道各 32 位浮点数                | 高精度计算和调试输出                                             |
| `kR32Uint`        | 单通道 32 位无符号整数                   | ID、索引、整数纹理和 Typed Buffer                                |
| `kR32Float`       | 单通道 32 位浮点数                       | 深度以外的标量、权重和计算数据                                   |
| `kRg32Float`      | RG 两通道各 32 位浮点数                  | 向量、速度和二维坐标                                             |
| `kD24UnormS8Uint` | 24 位深度归一化值加 8 位模板整数         | 常规深度/模板附件                                                |
| `kD32Float`       | 32 位浮点深度，无模板                    | 高精度深度附件                                                   |
| `kD32FloatS8Uint` | 32 位浮点深度加 8 位模板整数             | 高精度深度/模板附件                                              |
| `kBc1RgbaUnorm`   | BC1 4x4 压缩块，RGBA UNORM，低成本 alpha | 低内存颜色纹理，alpha 精度有限                                   |
| `kBc3RgbaUnorm`   | BC3 4x4 压缩块，RGBA UNORM，独立 alpha   | 带平滑透明度的压缩颜色纹理                                       |
| `kBc5Unorm`       | BC5 4x4 压缩块，两个 UNORM 通道          | 法线 XY、双通道数据                                              |
| `kBc7RgbaUnorm`   | BC7 4x4 压缩块，RGBA UNORM，高质量       | 高质量颜色纹理压缩                                               |

格式名称不隐含 Sampled、Storage、Attachment 或 Transfer 能力；每种用途都必须查询
RHI 的格式能力。`kRgba8Srgb` 也不等于“所有 Shader 计算都在 sRGB 空间”，只表示
对应资源视图/附件路径可以采用 sRGB 转换规则。

#### `SampleCount`、`TextureDimension` 和 `MemoryClass`

| 类型和值                    | 精确定义                               | 约束                                |
|-----------------------------|----------------------------------------|-------------------------------------|
| `SampleCount::kX1`          | 每个像素一个样本，无 MSAA              | 可用于普通纹理和非多重采样附件      |
| `SampleCount::kX2`          | 每个像素两个样本                       | 设备和格式必须支持 2x MSAA          |
| `SampleCount::kX4`          | 每个像素四个样本                       | 常用 MSAA 配置，仍需能力查询        |
| `SampleCount::kX8`          | 每个像素八个样本                       | 带宽和存储开销较高                  |
| `SampleCount::kX16`         | 每个像素十六个样本                     | 只在设备明确支持时使用              |
| `SampleCount::kX32`         | 每个像素三十二个样本                   | 兼容性和成本都较高                  |
| `SampleCount::kX64`         | 每个像素六十四个样本                   | 主要用于特定硬件或测试路径          |
| `TextureDimension::kTex1D`  | 一维纹理，坐标只有宽度维               | 不能填写二维或三维深度              |
| `TextureDimension::kTex2D`  | 二维纹理，通常用于颜色、深度和数组切片 | `array_layers` 表示数组层           |
| `TextureDimension::kTex3D`  | 三维体纹理，深度属于纹理尺寸           | 不能解释为数组层，View 维度必须匹配 |
| `MemoryClass::kAuto`        | RHI 根据用途和访问模式选择内存         | 不保证 CPU 可见或 GPU 本地          |
| `MemoryClass::kDeviceLocal` | 优先 GPU 本地、CPU 不直接写入的内存    | 上传应经 staging/Upload 资源进行    |
| `MemoryClass::kUpload`      | 优先 CPU 写入并供 GPU 读取的内存       | 适合动态常量和上传中转              |
| `MemoryClass::kReadback`    | 优先 GPU 写入并由 CPU 读取的内存       | 读取前必须等待 GPU Completion       |

#### `ResourceFlags` 位值

| 标志值            | 精确定义                           | 使用限制                          |
|-------------------|------------------------------------|-----------------------------------|
| `kNone`           | 没有额外创建属性                   | 默认值                            |
| `kAllowAlias`     | 允许在不重叠生命周期内复用物理内存 | 不能在重叠访问时强行别名          |
| `kMutableFormat`  | 允许使用兼容格式创建 View          | 兼容格式必须经过 RHI 校验         |
| `kCubeCompatible` | 允许满足条件的 2D 资源解释为 Cube  | 通常要求六层及相容 View Dimension |
| `kExternal`       | 资源来自或需要交给外部互操作路径   | 必须定义所有权、同步和有效期      |

#### Buffer/Texture Capability 位值

| 类型和值                                               | 精确定义                             |
|--------------------------------------------------------|--------------------------------------|
| `BufferCapability::kNone` / `TextureCapability::kNone` | 不授予额外操作                       |
| `kTransferSource`                                      | 可作为 Copy/Transfer 的源            |
| `kTransferDestination`                                 | 可作为 Copy/Clear/Transfer 的目标    |
| `BufferCapability::kVertex`                            | 可绑定为 Vertex Buffer               |
| `BufferCapability::kIndex`                             | 可绑定为 Index Buffer                |
| `BufferCapability::kUniform`                           | 可通过 Uniform/Constant Buffer 读取  |
| `BufferCapability::kStorage`                           | 可作为 Storage Buffer 读写           |
| `BufferCapability::kIndirect`                          | 可提供 Indirect Draw/Dispatch 参数   |
| `BufferCapability::kTypedView`                         | 可创建 Typed Buffer View             |
| `BufferCapability::kDeviceAddress`                     | 可获得 GPU Device Address            |
| `BufferCapability::kAccelerationInput`                 | 可作为加速结构构建输入               |
| `TextureCapability::kSampled`                          | 可创建 Sampled View 并被 Shader 采样 |
| `TextureCapability::kStorage`                          | 可创建 Storage View 并被 Shader 读写 |
| `TextureCapability::kColorAttachment`                  | 可作为颜色附件                       |
| `TextureCapability::kDepthStencil`                     | 可作为深度/模板附件                  |

创建资源时必须一次性声明整个生命周期可能用到的 Capability；后续不能把缺少的能力
当作隐式升级。Buffer 专属值不能用于 Texture，反之亦然。

#### `Aspect` 位值

| 标志值     | 精确定义           |
|------------|--------------------|
| `kColor`   | 选择颜色数据子资源 |
| `kDepth`   | 选择深度子资源     |
| `kStencil` | 选择模板子资源     |

`Aspect` 可以组合，例如深度/模板格式的 View 可以使用 `kDepth | kStencil`。颜色格式
不能同时声明深度或模板 Aspect。`SubresourceRange` 未显式填写时默认是 `kColor`，
因此创建深度/模板 View 时必须主动改写 Aspect。

#### `ResourceUsage`

`ResourceUsage` 是 RHI 和 RenderGraph 共用的抽象资源状态。它描述资源在某个操作中的
语义用途；后端再结合 `PipelineStageMask`、`ShaderStageMask` 和 `QueueClass` 推导原生
Layout、Access Mask、Barrier 和 Queue Ownership。它不是内存类型，也不是 Shader 阶段。

| 枚举值                             | 精确定义                              | 使用规则                                       |
|------------------------------------|---------------------------------------|------------------------------------------------|
| `kUndefined`                       | 没有可读取的已知内容或状态            | 只用于初始状态或明确丢弃后的转换，不能直接读取 |
| `kCopySource`                      | Copy/Transfer 源                      | 资源作为复制读取源，不表示 Shader 可读取       |
| `kCopyDestination`                 | Copy/Clear/Transfer 目标              | 资源接收复制或清除写入                         |
| `kVertexInput`                     | Vertex Buffer 输入读取                | Buffer 必须有 `kVertex` Capability             |
| `kIndexInput`                      | Index Buffer 输入读取                 | Buffer 必须有 `kIndex` Capability              |
| `kIndirectArguments`               | GPU 间接绘制或派发参数读取            | Buffer 必须有 `kIndirect` Capability           |
| `kUniformRead`                     | Uniform/Constant Buffer 只读访问      | Buffer 必须有 `kUniform` Capability            |
| `kSampledRead`                     | 通过 Sampled View 和 Sampler 读取纹理 | Texture 必须有 `kSampled` Capability           |
| `kTypedRead`                       | 通过 Typed View 读取格式化数据        | 资源和 View 的 Format 必须匹配                 |
| `kStorageRead`                     | Storage Resource 只读访问             | 必须有 Storage Capability 和只读 View          |
| `kStorageWrite`                    | Storage Resource 写入访问             | 必须有 Storage Capability，旧内容默认不保留    |
| `kStorageReadWrite`                | Storage Resource 同时读写             | 需要读写状态和相应同步依赖                     |
| `kColorAttachmentRead`             | 作为颜色附件读取已有内容              | 常用于 Load、Blend 或输入附件路径              |
| `kColorAttachmentWrite`            | 作为颜色附件写入颜色结果              | 必须有 Color Attachment Capability             |
| `kDepthStencilRead`                | 读取深度/模板附件                     | 必须选择正确的 Depth/Stencil Aspect            |
| `kDepthStencilWrite`               | 写入深度/模板附件                     | 必须有 Depth/Stencil Capability                |
| `kResolveSource`                   | 多重采样资源的 Resolve 源             | 源和目标的样本数、格式和范围必须兼容           |
| `kResolveDestination`              | Resolve 目标资源                      | 通常是单采样且可写的资源                       |
| `kPresent`                         | 交给 Presentation Surface 的最终状态  | Present 后不能继续写入                         |
| `kHostRead`                        | CPU 从资源读取                        | 资源必须 CPU 可见且已等待 GPU Completion       |
| `kHostWrite`                       | CPU 向资源写入                        | 资源必须 CPU 可见，之后要声明 GPU 后续状态     |
| `kAccelerationStructureRead`       | Shader 读取加速结构                   | 需要 AS Handle 和对应 RHI 能力                 |
| `kAccelerationStructureBuildRead`  | 构建加速结构时读取输入                | 输入必须保持有效到构建完成                     |
| `kAccelerationStructureBuildWrite` | 构建加速结构时写入目标                | 目标不能与未声明读写重叠                       |

同一资源在相邻操作间改变 `ResourceUsage` 时，RenderGraph 必须生成对应 Barrier；即使
Usage 没变，只要存在跨队列或可见性依赖，也可能需要 Memory Dependency。

#### `QueueClass`

| 枚举值      | 精确定义                                 | 约束                                   |
|-------------|------------------------------------------|----------------------------------------|
| `kAuto`     | Scheduler/RHI 根据能力和依赖选择逻辑队列 | 不能表达特定队列的强制要求             |
| `kGraphics` | 支持 Graphics/Render 命令的逻辑队列      | 通常也可承载通用 Compute               |
| `kCompute`  | 面向 Compute 的逻辑队列                  | 不能录制不支持的 Graphics/Present 命令 |
| `kCopy`     | 面向 Copy/Transfer 的逻辑队列            | 只执行允许的传输和清除命令             |
| `kPresent`  | 面向 Presentation 的逻辑队列             | 只用于呈现路径                         |

逻辑队列不等同于物理队列索引。多个逻辑类别可以映射到同一物理队列，映射结果应写入
诊断信息和 `RenderGraphQueueDependency`。

#### `ShaderStage` 位值

| 标志值                    | 精确定义                                   |
|---------------------------|--------------------------------------------|
| `kNone`                   | 没有 Shader 阶段可见性                     |
| `kVertex`                 | Vertex Shader 阶段                         |
| `kTessellationControl`    | Tessellation Control/Hull Shader 阶段      |
| `kTessellationEvaluation` | Tessellation Evaluation/Domain Shader 阶段 |
| `kGeometry`               | Geometry Shader 阶段                       |
| `kFragment`               | Fragment/Pixel Shader 阶段                 |
| `kCompute`                | Compute Shader 阶段                        |
| `kRayTracing`             | Ray Tracing Shader 阶段                    |
| `kMesh`                   | Mesh/Task 相关几何生成阶段                 |
| `kAll`                    | 所有当前公开 Shader 阶段的宽泛可见性请求   |

`ShaderStage` 是位掩码，`kAll` 不能与普通阶段值当作互斥枚举使用。资源 Binding 的
可见性和 `set_inline_constants()` 都使用该 Mask；它不表示资源在这些阶段一定实际访问。

### 28.6 RHI：同步和图形管线状态

#### `PipelineStage` 位值

`PipelineStage` 是 Barrier、Wait 和资源可见性使用的执行阶段 Mask。它描述操作发生在
哪个 GPU/Host 阶段，不等于 `ShaderStage` 的 Shader 可见性。

| 标志值            | 精确定义                                             |
|-------------------|------------------------------------------------------|
| `kNone`           | 没有指定执行阶段；只适合表示空值，不适合实际资源访问 |
| `kCopy`           | Copy/Transfer 命令阶段                               |
| `kVertexInput`    | 顶点/索引输入读取阶段                                |
| `kVertexShader`   | Vertex Shader 执行阶段                               |
| `kFragmentShader` | Fragment/Pixel Shader 执行阶段                       |
| `kEarlyDepth`     | 早期深度测试阶段                                     |
| `kLateDepth`      | 晚期深度/模板测试和写入阶段                          |
| `kColorOutput`    | 颜色附件输出和混合阶段                               |
| `kCompute`        | Compute Shader 执行阶段                              |
| `kRayTracing`     | Ray Tracing Shader/相关 GPU 阶段                     |
| `kHost`           | CPU/Host 对资源的可见访问阶段                        |
| `kAllCommands`    | 无法精确表达时请求覆盖所有命令的保守同步范围         |

`kAllCommands` 是明确的宽范围请求，不是已列 bit 的简单并集。热路径应尽量使用精确
阶段，以减少 Barrier 和 Wait 的同步范围。

#### `PipelineType`

| 枚举值      | 精确定义                                | 必需的 Shader 阶段                   |
|-------------|-----------------------------------------|--------------------------------------|
| `kGraphics` | 可绑定 Graphics Pipeline 并执行 Draw    | 至少包含 Vertex；通常还包含 Fragment |
| `kCompute`  | 可绑定 Compute Pipeline 并执行 Dispatch | 必须包含 Compute                     |

Graphics Pipeline 的 `graphics` 状态只对 `kGraphics` 生效；Compute Pipeline 不应填写或
依赖图形固定功能状态。

#### `PrimitiveTopology`

| 枚举值                        | 精确定义                                  |
|-------------------------------|-------------------------------------------|
| `kPointList`                  | 每个顶点独立形成一个点                    |
| `kLineList`                   | 每两个顶点形成一条独立线段                |
| `kLineStrip`                  | 相邻顶点连续形成折线                      |
| `kTriangleList`               | 每三个顶点形成一个独立三角形              |
| `kTriangleStrip`              | 相邻顶点复用形成三角形带                  |
| `kTriangleFan`                | 首顶点与后续相邻顶点形成三角形扇          |
| `kLineListWithAdjacency`      | 独立线段并附带邻接顶点                    |
| `kLineStripWithAdjacency`     | 折线并附带邻接顶点                        |
| `kTriangleListWithAdjacency`  | 独立三角形并附带邻接顶点                  |
| `kTriangleStripWithAdjacency` | 三角形带并附带邻接顶点                    |
| `kPatchList`                  | 固定数量控制点组成一个 Tessellation Patch |

Adjacency 拓扑要求 Vertex Shader 输入布局提供相应邻接数据；`kPatchList` 的控制点数
由 Tessellation 配置和后端限制共同决定，不能只看拓扑名称推断。

#### `VertexInputRate`、`PolygonMode`、`CullMode` 和 `FrontFace`

| 类型和值                       | 精确定义                                       |
|--------------------------------|------------------------------------------------|
| `VertexInputRate::kVertex`     | 每处理一个顶点，Vertex Buffer binding 前进一次 |
| `VertexInputRate::kInstance`   | 每处理一个实例，Vertex Buffer binding 前进一次 |
| `PolygonMode::kFill`           | 光栅化填充多边形内部                           |
| `PolygonMode::kLine`           | 只光栅化多边形边线                             |
| `PolygonMode::kPoint`          | 以点表示多边形顶点                             |
| `CullMode::kNone`              | 不剔除任何面                                   |
| `CullMode::kFront`             | 剔除正面                                       |
| `CullMode::kBack`              | 剔除背面                                       |
| `CullMode::kFrontAndBack`      | 剔除正面和背面，不产生可见三角形               |
| `FrontFace::kCounterClockwise` | 逆时针顶点绕序被视为正面                       |
| `FrontFace::kClockwise`        | 顺时针顶点绕序被视为正面                       |

`PolygonMode::kLine` 和 `kPoint` 可能受设备 Feature 限制。Cull 和 FrontFace 共同决定
最终哪一面被剔除，不能单独根据其中一个值判断。

#### `CompareOp`

| 枚举值            | 比较条件             |
|-------------------|----------------------|
| `kNever`          | 永远失败             |
| `kLess`           | 源值小于目标值       |
| `kEqual`          | 源值等于目标值       |
| `kLessOrEqual`    | 源值小于或等于目标值 |
| `kGreater`        | 源值大于目标值       |
| `kNotEqual`       | 源值不等于目标值     |
| `kGreaterOrEqual` | 源值大于或等于目标值 |
| `kAlways`         | 永远通过             |

比较方向以 RHI 文档的“源值与目标值”定义为准；Depth Test、Stencil Test 和其他使用者
必须明确各自的源/目标含义，不能把 `kLess` 固定理解为“只适合深度”。

#### `BlendOp`

| 枚举值             | 计算语义                                                    |
|--------------------|-------------------------------------------------------------|
| `kAdd`             | `source * source_factor + destination * destination_factor` |
| `kSubtract`        | `source * source_factor - destination * destination_factor` |
| `kReverseSubtract` | `destination * destination_factor - source * source_factor` |
| `kMin`             | 取源和目标的逐分量较小值，通常忽略 factor                   |
| `kMax`             | 取源和目标的逐分量较大值，通常忽略 factor                   |

#### `ColorComponent` 位值

| 标志值  | 精确定义               |
|---------|------------------------|
| `kNone` | 不写入任何颜色通道     |
| `kR`    | 允许写入 R 通道        |
| `kG`    | 允许写入 G 通道        |
| `kB`    | 允许写入 B 通道        |
| `kA`    | 允许写入 A 通道        |
| `kAll`  | 允许写入 RGBA 全部通道 |

#### `BlendFactor`

Blend Factor 是混合公式中的乘数。令 `S` 为当前片元源值、`D` 为附件已有目标值、`Sa`
和 `Da` 为对应 alpha、`C`/`A` 为常量值、`S1` 为双源混合的第二源值：

| 枚举值                      | 因子语义                              |
|-----------------------------|---------------------------------------|
| `kZero`                     | `0`                                   |
| `kOne`                      | `1`                                   |
| `kSourceColor`              | `S`                                   |
| `kOneMinusSourceColor`      | `1 - S`                               |
| `kDestinationColor`         | `D`                                   |
| `kOneMinusDestinationColor` | `1 - D`                               |
| `kSourceAlpha`              | `Sa`                                  |
| `kOneMinusSourceAlpha`      | `1 - Sa`                              |
| `kDestinationAlpha`         | `Da`                                  |
| `kOneMinusDestinationAlpha` | `1 - Da`                              |
| `kConstantColor`            | `C`                                   |
| `kOneMinusConstantColor`    | `1 - C`                               |
| `kConstantAlpha`            | `A`                                   |
| `kOneMinusConstantAlpha`    | `1 - A`                               |
| `kSourceAlphaSaturate`      | `min(Sa, 1 - Da)`，通常用于源颜色因子 |
| `kSourceOneColor`           | 双源混合的第二源颜色 `S1`             |
| `kOneMinusSourceOneColor`   | `1 - S1`                              |
| `kSourceOneAlpha`           | 双源混合的第二源 alpha                |
| `kOneMinusSourceOneAlpha`   | `1 - S1a`                             |

`kSourceOne*` 需要双源输出和设备支持；普通单源 Fragment Shader 不能使用它们。颜色
和 alpha 通道可以分别设置 Factor/Op，最终写入还要受 `ColorComponent` Mask 限制。

#### `ComponentSwizzle`

| 枚举值      | View 输出分量来源        |
|-------------|--------------------------|
| `kIdentity` | 保持该分量的原始对应通道 |
| `kZero`     | 固定输出 `0`             |
| `kOne`      | 固定输出 `1`             |
| `kR`        | 取源纹理 R 通道          |
| `kG`        | 取源纹理 G 通道          |
| `kB`        | 取源纹理 B 通道          |
| `kA`        | 取源纹理 A 通道          |

Swizzle 只改变 View 读取出的分量映射，不修改底层资源内容，也不改变资源的 Format。

### 28.7 RHI：交换链、View、Sampler 和 Binding

本节覆盖 `Engine/RHI/public/rhi_struct.hpp` 中与交换链、资源 View、采样器和资源绑定
有关的枚举。除 `BindingFlags` 外，这些类型都是普通枚举，同一个字段只能填写一个值；
不能把两个值按位或，也不能把枚举底层整数当作另一个枚举使用。

#### `PresentMode`

`PresentMode` 描述应用希望采用的呈现节奏，而不是对所有平台强制使用某个原生呈现
模式。Surface 可能不支持请求值，RHI 可以根据本节规定选择有效的近似值，并必须在
`SwapchainInfo::present_mode` 中报告最终值。

| 枚举值           | 精确定义                                         | 时序和画面特征                                                         | 不支持时的处理                                                                   |
|------------------|--------------------------------------------------|------------------------------------------------------------------------|----------------------------------------------------------------------------------|
| `kImmediate`     | 尽快把最新完成的图像交给显示系统，不等待显示刷新 | 延迟通常较低，但可能撕裂；不保证每个提交都显示                         | 选择支持的安全模式，并通过 `SwapchainInfo` 报告；当前 Vulkan 优先回退到 `kVsync` |
| `kLowLatency`    | 优先显示最新完成的图像，允许呈现队列丢弃较旧图像 | 适合交互和低延迟场景；不等于绝对无撕裂，也不保证设备一定有额外队列容量 | 当前 Vulkan 映射为 Mailbox；不可用时回退到 FIFO 或 Surface 的第一个可用模式      |
| `kVsync`         | 以显示刷新为节奏进行排队和呈现                   | 通常避免撕裂；生产者可能因队列满而等待；是 `SwapchainDesc` 默认值      | 应优先保留该语义；若 Surface 连 FIFO 都不提供，只能选择实际可用模式并报告结果    |
| `kAdaptiveVsync` | 正常时按刷新同步，生产者落后时允许放宽同步       | 尽量减少撕裂和卡顿的折中模式；属于可选能力                             | 当前 Vulkan 映射为 FIFO Relaxed；不可用时按普通回退规则处理                      |

以下规则对所有呈现模式都成立：

- `PresentMode` 不改变 `ResourceUsage::kPresent` 的资源状态要求；呈现前仍必须完成
  所有写入和必要的跨队列等待。
- `kImmediate`、`kLowLatency` 和 `kAdaptiveVsync` 都不是“必须支持”的标志。调用者
  不能因请求成功就假设实际模式未被替换，必须读取 `get_swapchain_info()`。
- `SwapchainInfo::needs_recreate` 或 `ResultCode::kOutOfDate` 表示外部 Surface 状态
  已改变，不是呈现模式本身的失败。

#### `BufferViewKind`

`BufferViewKind` 决定 `BufferViewDesc` 如何解释一个 Buffer 的字节范围。它不改变
Buffer 的容量，也不自动补充创建时缺失的 `BufferCapability`。

| 枚举值        | 精确定义                                                   | `BufferViewDesc` 要求                                                                                | 绑定/Usage 约束                                                                                                        |
|---------------|------------------------------------------------------------|------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| `kUniform`    | 按 Uniform/Constant Buffer 规则读取一段连续字节            | `format` 应为 `kUnknown`；`offset` 和有效范围必须满足设备对 Uniform 对齐的要求                       | Buffer 必须有 `BufferCapability::kUniform`；通常与 `BindingType::kUniformBuffer` 和 `ResourceUsage::kUniformRead` 配对 |
| `kStorage`    | 按 Storage Buffer 规则访问一段连续字节，不声明固定元素格式 | `format` 应为 `kUnknown`；`structure_stride` 可以为 0                                                | Buffer 必须有 `BufferCapability::kStorage`；可与 `BindingType::kStorageBuffer` 配对                                    |
| `kRaw`        | 按原始字节地址访问 Storage 数据                            | `format` 应为 `kUnknown`；调用者负责在 Shader 中遵守字节对齐和布局                                   | Buffer 必须有 `BufferCapability::kStorage`；不能因此推断它可作为 Typed View                                            |
| `kStructured` | 将范围解释为固定步长的结构体数组                           | `structure_stride` 必须大于 0；`format` 应为 `kUnknown`                                              | Buffer 必须有 `BufferCapability::kStorage`；结构步长必须与 Shader Contract 的结构布局一致                              |
| `kTyped`      | 将范围解释为固定格式的元素数组                             | `format` 不能为 `kUnknown`；格式的元素大小、范围和对齐必须有效；`structure_stride` 不参与 Typed 解释 | Buffer 必须有 `BufferCapability::kTypedView`；与 `ResourceUsage::kTypedRead` 或 Typed Buffer Binding 配对              |

所有 Buffer View 都必须满足 `offset <= buffer.size`，且有限 `size` 不能越过 Buffer
末尾；使用 `UINT64_MAX` 的 `size` 表示“从 offset 到末尾”。无效句柄通常返回
`kNotFound`，范围、Capability、格式或对齐错误返回 `kInvalidArgument`。当前 Vulkan
后端对非 Typed View 保存 RHI 元数据并使用 Buffer Range 绑定，Typed View 才创建原生
格式化 View；调用者不应依赖这种实现细节来改变公共语义。

#### `TextureViewKind`

| 枚举值             | 精确定义                                      | 必需的 Texture Capability             | 合法的主要 Binding/Usage                                                           |
|--------------------|-----------------------------------------------|---------------------------------------|------------------------------------------------------------------------------------|
| `kSampled`         | 通过采样器或采样指令读取纹理 View             | `TextureCapability::kSampled`         | `BindingType::kSampledTexture`、`ResourceUsage::kSampledRead`                      |
| `kStorage`         | 通过 Storage Image/Texture 进行读写或只读访问 | `TextureCapability::kStorage`         | `BindingType::kStorageTexture`、`kStorageRead`/`kStorageWrite`/`kStorageReadWrite` |
| `kColorAttachment` | 作为颜色渲染附件写入或读取                    | `TextureCapability::kColorAttachment` | `ColorAttachmentDesc`、`kColorAttachmentRead`/`kColorAttachmentWrite`              |
| `kDepthStencil`    | 作为深度或模板附件访问                        | `TextureCapability::kDepthStencil`    | `DepthStencilAttachmentDesc`、`kDepthStencilRead`/`kDepthStencilWrite`             |

View Kind 与 Format 必须相容：深度/模板格式不能创建 `kColorAttachment` View，颜色
格式不能创建 `kDepthStencil` View。`kSampled` 和 `kStorage` 不代表格式一定支持该
访问，实际格式能力仍由 RHI/设备验证。

#### `TextureViewDimension`

该枚举只改变 View 的坐标解释和数组/立方体形状，不改变底层 Texture 的分配形状。
`TextureDesc::dimension` 是资源创建时的物理维度，`TextureViewDimension` 是一次 View
创建时的逻辑维度；二者必须满足下表的兼容关系。

| 枚举值        | 精确定义                                               | 源 Texture 和子资源约束                                                            |
|---------------|--------------------------------------------------------|------------------------------------------------------------------------------------|
| `kAuto`       | 根据源 Texture 维度和选中的 layer 数自动推导 View 类型 | 1D 资源推导为 1D/1D Array；2D 资源推导为 2D/2D Array；3D 资源推导为 3D             |
| `kTex1D`      | 单层一维 View                                          | 源必须为 `TextureDimension::kTex1D`，`layer_count` 必须为 1                        |
| `kTex1DArray` | 一维数组 View                                          | 源必须为 1D 纹理；选中的 layer 范围解释为数组层                                    |
| `kTex2D`      | 单层二维 View                                          | 源必须为 `TextureDimension::kTex2D`，`layer_count` 必须为 1                        |
| `kTex2DArray` | 二维数组 View                                          | 源必须为 2D 纹理；选中的 layer 范围解释为数组层                                    |
| `kCube`       | 一个立方体 View，由连续六层组成六个面                  | 源必须为 2D 纹理，必须有 `ResourceFlags::kCubeCompatible`，`layer_count` 必须为 6  |
| `kCubeArray`  | 多个立方体组成的数组 View                              | 源必须为 2D 纹理，必须有 Cube Compatible，`layer_count` 至少为 6 且必须是 6 的倍数 |
| `kTex3D`      | 三维体纹理 View                                        | 源必须为 `TextureDimension::kTex3D`；不能把 3D 深度改解释为数组层                  |

`TextureViewDesc::format == Format::kUnknown` 表示沿用源 Texture Format。指定不同
Format 只有在源资源带有 `ResourceFlags::kMutableFormat` 且该 Format 出现在
`compatible_view_formats` 中时才合法；这不是格式转换。`SubresourceRange` 的
`base_mip/base_layer` 必须在资源范围内，`kRemaining` 才能表示“从起点到末尾”。

#### `Filter`

| 枚举值     | 精确定义                                                 | 影响的 `SamplerDesc` 字段                               |
|------------|----------------------------------------------------------|---------------------------------------------------------|
| `kNearest` | 选择最接近采样坐标的单个样本，不做线性插值               | `min_filter`、`mag_filter` 或 `mip_filter` 各自独立生效 |
| `kLinear`  | 对相邻样本进行线性插值；在 mip 过滤中表示线性选择 mip 层 | 同上；不自动启用各向异性                                |

`min_filter` 用于纹理缩小，`mag_filter` 用于纹理放大，`mip_filter` 用于 mip 层之间
的选择。三个字段可以分别取不同值。`Filter` 不表示颜色空间转换，也不改变纹理
Format；各向异性由 `anisotropy_enable/max_anisotropy` 单独控制。

#### `AddressMode`

| 枚举值            | 精确定义                   | 坐标超出 `[0, 1]` 时的结果                                                                                                                              |
|-------------------|----------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|
| `kRepeat`         | 周期性重复纹理             | 使用坐标的小数周期，`1.25` 等价于 `0.25`                                                                                                                |
| `kMirroredRepeat` | 周期性重复并在相邻周期镜像 | 奇数周期反转坐标方向，减少接缝方向变化                                                                                                                  |
| `kClampToEdge`    | 将坐标限制到纹理边缘       | 使用最近边缘的纹素，不采样边界外颜色                                                                                                                    |
| `kClampToBorder`  | 边界外使用固定边界颜色     | 当前 `SamplerDesc` 没有 `border_color` 字段，公共契约规定固定使用不透明白色 `(1, 1, 1, 1)`；后端不能自行选择其他颜色，无法表示时必须返回 `kUnsupported` |

当前 RHI 使用归一化纹理坐标；没有公开未归一化坐标模式。`address_u/v/w` 分别控制
三个坐标轴，2D 纹理的 `w` 值仍必须填写一个合法枚举值但不会产生第三维采样。

#### `BindingType`

`BindingType` 是 Shader Contract 中一个 `(group, binding)` 位置的资源类别。写入
`BindingSet` 时，`BindingWrite::resource.type` 必须与布局中的 `BindingLayoutEntry::type`
完全相同，并且只能使用该类型对应的资源字段。

| 枚举值                   | 精确定义                                  | `BindingResource` 有效字段      | 资源/View/Usage 要求                                                                            |
|--------------------------|-------------------------------------------|---------------------------------|-------------------------------------------------------------------------------------------------|
| `kSampler`               | 只绑定采样状态对象，不绑定纹理内容        | `sampler`                       | 必须是有效 `SamplerHandle`；通常与 `kSampledTexture` 在不同 binding 配对                        |
| `kSampledTexture`        | 绑定可被 Shader 采样的纹理 View           | `texture_view`                  | View Kind 必须为 `kSampled`，Texture 必须有 `kSampled` Capability，声明 `kSampledRead`          |
| `kStorageTexture`        | 绑定可被 Shader Storage 读写的纹理 View   | `texture_view`                  | View Kind 必须为 `kStorage`，Texture 必须有 `kStorage` Capability，按实际读写声明 Storage Usage |
| `kUniformBuffer`         | 绑定只读 Uniform/Constant Buffer 范围     | `buffer_range` 或 `buffer_view` | Buffer 必须有 `kUniform` Capability；View 若存在必须是 `BufferViewKind::kUniform`               |
| `kStorageBuffer`         | 绑定可读写或只读的 Storage Buffer 范围    | `buffer_range` 或 `buffer_view` | Buffer 必须有 `kStorage` Capability；View 可为 `kStorage`、`kRaw` 或 `kStructured`              |
| `kReadOnlyTypedBuffer`   | 绑定按 Format 解释、只读的 Typed Buffer   | `buffer_view`                   | View 必须为 `BufferViewKind::kTyped`，Buffer 必须有 `kTypedView`，Usage 为 `kTypedRead`         |
| `kReadWriteTypedBuffer`  | 绑定按 Format 解释、可读写的 Typed Buffer | `buffer_view`                   | View 必须为 Typed；底层状态按 `kStorageReadWrite` 处理，并需设备支持写入 Typed Buffer           |
| `kAccelerationStructure` | 绑定加速结构句柄供光线追踪或查询使用      | `acceleration_structure`        | 需要相应设备 Feature 和实现；当前 Vulkan 后端返回 `kUnsupported`                                |

`buffer_range` 和 `buffer_view` 是二选一的表达方式：前者直接绑定连续字节范围，后者
使用已创建 View 的范围和格式。调用者不能在同一写入中依赖“无效字段恰好被后端忽略”
来表达另一种类型。无效句柄返回 `kNotFound`，类型或 View Kind 不匹配返回
`kInvalidArgument`。

#### `BindingArrayMode`

该枚举决定一个 binding 有多少数组元素以及元素数量是否固定。`array_count` 的取值
规则是公共 Shader Contract 的一部分，而不是后端私有约定。

| 枚举值       | 精确定义                                              | `array_count` 规则                                         | 写入规则                                                                     |
|--------------|-------------------------------------------------------|------------------------------------------------------------|------------------------------------------------------------------------------|
| `kSingle`    | 一个且只有一个资源槽位                                | 必须为 `1`                                                 | 只能写 `array_index == 0`                                                    |
| `kFixed`     | 固定数量的资源数组                                    | 必须大于 `0`                                               | 合法索引为 `[0, array_count)`；是否必须全部填充由 `kPartiallyPopulated` 决定 |
| `kVariable`  | 每个 Binding Set 可使用不超过运行时声明容量的可变数组 | 公共 Shader Contract 中必须为 `0`，容量由实现/布局扩展提供 | 需要支持可变数组；当前 Vulkan 后端尚未实现                                   |
| `kUnbounded` | 无固定上限的 Bindless/Unbounded 数组                  | 必须为 `0`                                                 | 需要 Bindless 设备能力和分配策略；当前 Vulkan 后端尚未实现                   |

当前 Shader 包解析器和 Vulkan Binding Layout 创建路径都执行以下硬校验：`kSingle`
且 count 不是 1、`kFixed` 且 count 为 0、`kVariable/kUnbounded` 且 count 不是 0，
都返回 `kInvalidArgument`。`kVariable`、`kUnbounded` 或任何 Bindless/部分填充请求在
当前 Vulkan 实现中返回 `kUnsupported`，不会静默降级为固定数组。

#### `BindingFlags`

`BindingFlags` 是可组合位掩码。未列出的 bit 是未知扩展，不能忽略。

| 标志值                | 精确定义                                                    | 合法组合和当前支持状态                                                                                          |
|-----------------------|-------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------|
| `kNone`               | 不启用额外绑定行为                                          | 可以与所有普通 BindingType/ArrayMode 配合                                                                       |
| `kBindless`           | 该 binding 允许通过大规模资源索引访问，无需传统固定槽位更新 | 通常与 `kVariable`/`kUnbounded` 配合；需要 `DeviceFeature::kBindlessResources`；当前 Vulkan 返回 `kUnsupported` |
| `kPartiallyPopulated` | 数组允许存在未写入的槽位                                    | 只能对数组型 binding 有意义；读取空槽的 Shader 行为必须由 Contract 定义；当前 Vulkan 返回 `kUnsupported`        |
| `kDynamicOffset`      | Buffer 的实际基址在绑定命令时通过 dynamic offset 提供       | 只适用于 Uniform/Storage Buffer；offset 顺序必须由布局稳定定义；当前 Vulkan 返回 `kUnsupported`                 |
| `kImmutable`          | 绑定内容在创建后不可通过普通更新路径替换                    | 需要创建时可表达初始资源的 API；当前公共结构没有该初始资源字段，当前 Vulkan 返回 `kUnsupported`                 |

不允许把未知 bit、`kDynamicOffset` 用在纹理/采样器上，或把 `kImmutable` 当作普通
“性能提示”。这些情况应返回 `kInvalidArgument` 或 `kUnsupported`，不能改变绑定语义。

#### `BindingSetLifetime`

| 枚举值        | 精确定义                              | 生命周期和所有权                                                                                |
|---------------|---------------------------------------|-------------------------------------------------------------------------------------------------|
| `kPersistent` | Binding Set 可跨多个 Frame Slot 使用  | 由调用者显式销毁；资源和 Pipeline 使用完成前不得销毁；适合材质、静态场景和长期共享数据          |
| `kFrameLocal` | Binding Set 只属于创建它的 Frame Slot | 该 Slot 回收后不能继续使用；适合每帧常量、临时 RenderGraph 绑定；通常由 `FrameContext` 延迟回收 |

Lifetime 只控制 Descriptor 分配和 Set 对象的有效期，不改变被绑定 Buffer/Texture 的
所有权，也不允许绕过 GPU Completion。当前 Vulkan 为两种 Lifetime 分开维护 Descriptor
Pool 页面；Frame Local 页面在对应 Set 全部释放后可以整体重置。

### 28.8 RHI：附件、绘制、索引和命令状态

#### `LoadOp`

`LoadOp` 定义动态渲染开始时对附件既有内容的处理方式。它不是资源初始内存清零
操作；清除值只在 `kClear` 时被使用。

| 枚举值      | 精确定义                                                              | 对既有内容的要求                                        | 清除值是否生效                                     |
|-------------|-----------------------------------------------------------------------|---------------------------------------------------------|----------------------------------------------------|
| `kLoad`     | 保留附件在进入 Rendering 前的内容，并使本次渲染可以基于该内容继续工作 | 前一使用者必须已经产生有效内容，且 Barrier/Usage 已完成 | 否，`clear_color/clear_depth/clear_stencil` 被忽略 |
| `kClear`    | 在 Rendering 开始时用描述中的 Clear 值初始化附件                      | 旧内容无需有效；对 transient 资源是常用的首次写入方式   | 是                                                 |
| `kDontCare` | 明确放弃既有内容，不保证开始时的值                                    | Pass 不能读取未重新写入的内容                           | 否；初始值未定义                                   |

颜色附件使用 `ColorAttachmentDesc::load_op`，深度和模板当前共用
`DepthStencilAttachmentDesc::depth_load_op`。公共结构没有分别设置 depth/stencil load
策略的字段，因此不能假设两者可以独立配置。

#### `StoreOp`

| 枚举值      | 精确定义                                      | Rendering 结束后的保证                                               |
|-------------|-----------------------------------------------|----------------------------------------------------------------------|
| `kStore`    | 将本次 Rendering 的最终附件内容保留给后续使用 | 后续 Pass 可以在正确的资源状态下读取或继续写入                       |
| `kDontCare` | 明确允许实现丢弃附件内容                      | 后续不能把该内容当作有效输入；若要继续使用必须先完整写入或重新初始化 |

`StoreOp::kDontCare` 不是“自动清零”，也不是“稍后可能仍然保留”的提示。选择它即
表示调用者放弃内容保留语义；RenderGraph 可以据此减少存储带宽或启用别名优化。

#### `IndexFormat`

| 枚举值    | 每个索引元素的宽度           | 地址和绘制约束                                                                                         |
|-----------|------------------------------|--------------------------------------------------------------------------------------------------------|
| `kUint16` | 16 bit，即 2 byte 无符号索引 | 绑定 offset 必须满足 2 byte 对齐；索引值范围为 `0..65535`，再加 `vertex_offset` 后仍需得到合法顶点访问 |
| `kUint32` | 32 bit，即 4 byte 无符号索引 | 绑定 offset 必须满足 4 byte 对齐；适合超过 16 位索引范围的网格                                         |

`bind_index_buffer()` 的 Format 只解释后续索引读取，不会转换 Buffer 中的字节。Buffer
必须在创建时声明 `BufferCapability::kIndex`，并在命令列表中先绑定有效 Index Buffer
再调用 `draw_indexed()`。索引数据长度和 `first_index/index_count` 必须落在 Buffer
范围内；RHI 不支持通过 `IndexFormat` 表达 8 bit 索引。

#### `BarrierMode`

| 枚举值      | 精确定义                                                             | 调用方责任                                                                                        | 错误表现                                                             |
|-------------|----------------------------------------------------------------------|---------------------------------------------------------------------------------------------------|----------------------------------------------------------------------|
| `kImplicit` | RHI 根据命令使用的资源和已跟踪状态自动插入必要的本地状态转换         | 仍必须声明正确的资源用途、队列和跨提交等待；不能把自动转换当作跨队列依赖                          | 参数非法、资源缺失或队列依赖无法满足时仍返回错误                     |
| `kExplicit` | RHI 不替调用方补齐资源状态转换；调用方必须先提交精确的 `BarrierDesc` | `before` 必须匹配跟踪状态，`after` 必须覆盖后续命令的 Usage/Stage/Queue；跨队列必须有对应提交等待 | 未声明 Barrier、before 状态不匹配或 Usage 不匹配返回 `kInvalidState` |

RenderGraph 执行使用 `kExplicit`，因为 Graph Compiler 已经拥有完整的资源依赖和
生命周期信息。直接使用 RHI 的独立 Command List 可以选择 `kImplicit`，但一个命令
列表缓存不能在复用时改变其 BarrierMode；否则返回 `kInvalidState`。

### 28.9 RenderGraph：资源生命周期、访问和 Barrier

RenderGraph 枚举与 RHI 枚举处在不同抽象层：`ResourceAccess` 描述 Pass 对逻辑资源
的读写关系，`rhi::ResourceUsage` 描述物理资源在 GPU/Host 上的具体语义用途。前者
不能替代后者，后者也不能单独表达 Pass 是否产生新版本。

#### `ResourceLifetime`

| 枚举值        | 精确定义                                                 | 物理资源所有权                                                      | 首次使用和编译约束                                                                       |
|---------------|----------------------------------------------------------|---------------------------------------------------------------------|------------------------------------------------------------------------------------------|
| `kTransient`  | 只在当前 Graph 编译/执行所覆盖的生命周期内存在的逻辑资源 | Graph 创建和销毁物理 Buffer/Texture，可在不重叠区间复用或别名       | 必须在第一次使用时以 `ResourceAccess::kWrite` 写入并丢弃旧内容；不能直接作为外部资源导入 |
| `kPersistent` | 跨多次 `clear_passes()`/编译仍保留的 Graph 逻辑资源      | Graph 持有物理资源，直到 Graph `reset()` 或资源记录被销毁           | 可以先读后写；初始状态和跨帧状态必须由描述或上一轮执行状态提供                           |
| `kImported`   | 已由 Graph 外部创建、每次执行时借给 Graph 使用的资源     | 调用者/外部 Owner 持有和销毁；Graph 绝不销毁 imported Native Handle | 必须使用 `import_buffer/import_texture/import_swapchain_image`；必须提供有效初始状态     |

`kTransient` 与 `ResourceFlags::kAllowAlias` 的含义不同：前者是 Graph 逻辑生命周期，
后者是 RHI 是否允许底层资源内存别名。Transient 资源通常可以由 Compiler 自动选择
复用槽位，但实际别名仍受描述、Capability、Format、样本数和生命周期区间限制。

#### `ResourceAccess`

| 枚举值       | 精确定义                                 | 版本和依赖语义                                               | 适用情况                                          |
|--------------|------------------------------------------|--------------------------------------------------------------|---------------------------------------------------|
| `kRead`      | 读取已经存在的资源版本，不产生新内容     | 必须有可见的生产者或合法导入初始状态；多个只读 Pass 可以并行 | 采样、顶点读取、Copy Source、读取已有深度         |
| `kWrite`     | 产生或完整替换一个资源版本，不依赖旧内容 | 建立写入版本边；旧内容不属于该 Pass 的输入                   | Clear、完整渲染目标、计算输出、Transient 首次写入 |
| `kReadWrite` | 读取旧版本并在同一 Pass 中产生新版本     | 同时建立读边和写边，通常需要唯一写者或明确的顺序             | 原地 Storage 更新、深度保留后继续写入、累积计算   |

`ResourceAccess` 只允许三种值，不能用 `kRead | kWrite` 表达读写；需要读写时必须
使用 `kReadWrite`。同一 Pass 对同一逻辑资源重复声明时，Compiler 必须能够合并成
唯一访问语义；无法确定先后或内容保留关系时返回 `kInvalidArgument`。

#### `RenderGraphBarrierPhase`

| 枚举值        | 精确定义                                                       | 执行位置                                 | 典型用途                                               |
|---------------|----------------------------------------------------------------|------------------------------------------|--------------------------------------------------------|
| `kBeforePass` | 在目标 Pass 的 execute 回调和第一条资源命令之前应用 Barrier    | 当前 Pass Command List 中，Pass 回调之前 | 前一 Pass 的输出转为当前 Pass 的输入/附件/Storage 状态 |
| `kAfterPass`  | 在目标 Pass 的 execute 回调结束并关闭 Rendering 后应用 Barrier | 当前 Pass Command List 中，Pass 回调之后 | 发布最终 Usage、释放队列所有权、为后续 Batch 准备状态  |

Barrier Phase 不是 CPU 调度阶段，也不是 Shader Stage。一个 Pass 可以同时拥有两种
Phase 的 Barrier；例如先在 `kBeforePass` 转为颜色附件，再在 `kAfterPass` 转为
`kSampledRead`。执行器会先处理 Before，再执行回调，最后处理 After。

#### `RenderGraphBarrierKind`

| 枚举值              | 精确定义                                                            | 需要关注的字段                                                                              | 是否一定产生同一种原生指令                            |
|---------------------|---------------------------------------------------------------------|---------------------------------------------------------------------------------------------|-------------------------------------------------------|
| `kTransition`       | 资源的 Usage、Pipeline Stage、Shader 可见性或其他状态发生改变       | `before`、`after`、范围和 `discard_contents`                                                | 否；后端可以用一条或多条状态转换实现                  |
| `kMemoryDependency` | 资源的抽象状态可以保持不变，但前一写入必须对后一读取/写入可见且有序 | `requires_memory_dependency`、前后资源范围和 Queue                                          | 否；可能只需要内存可见性/执行顺序依赖                 |
| `kQueueTransfer`    | 资源在不同逻辑 Queue 之间转移使用权或可见性                         | `before.queue`、`after.queue`、`requires_queue_transfer`、对应 `RenderGraphQueueDependency` | 否；通常还需要提交等待/信号，不能只录一条本地 Barrier |

`kind` 是 Compiler 对 Barrier 原因的分类，不是调用方可替换的执行命令。Queue
Transfer 可能同时包含 Usage Transition 和 Memory Dependency；这时 `kind` 仍表示
主要审计原因，而 `requires_memory_dependency`/`requires_queue_transfer` 表示需要
执行的附加同步。调用方不能因为 `before == after` 就删除 `kMemoryDependency`。

#### `RenderGraphResourceKind`

| 枚举值     | 精确定义                 | `RenderGraphBarrierPlan` 中有效的范围字段 | 额外约束                                                                    |
|------------|--------------------------|-------------------------------------------|-----------------------------------------------------------------------------|
| `kBuffer`  | Barrier 指向逻辑 Buffer  | `buffer_range`                            | `resource_index/generation` 必须指向 Buffer 表；`subresources` 不参与匹配   |
| `kTexture` | Barrier 指向逻辑 Texture | `subresources`                            | Aspect、mip 和 layer 必须落在 Texture 描述范围内；`buffer_range` 不参与匹配 |

`resource_kind`、`resource_index` 和 `resource_generation` 在所属图的 Plan 内共同标识
逻辑资源，不能只比较 index。Barrier 记录不携带跨图身份，不得复制到其他图执行；
公开 Buffer/Texture/Pass 句柄另外携带 `graph_id`。Graph `reset()` 后资源代际变化，
新建图则由不同 `graph_id` 区分，旧 Barrier/Handle 即使
index 相同也必须视为过期并返回 `kInvalidState`。

### 28.10 非枚举哨兵、容量常量和“空值”语义

以下值不是枚举成员，但它们经常出现在公共结构中，必须与 `kNone`、`kUnknown` 和
`kCount` 区分。它们的数值和语义同样属于公共 ABI 契约。

| 名称                                                      | 数值/位置                                   | 精确定义                                                                | 可以作为有效业务值吗                                                     |
|-----------------------------------------------------------|---------------------------------------------|-------------------------------------------------------------------------|--------------------------------------------------------------------------|
| `render_graph::kInvalidHandleIndex`                       | `UINT32_MAX`                                | RenderGraph Handle、Plan index 或 batch index 的“没有对象/没有索引”状态 | 不能；任何执行 API 收到它都应返回 `kInvalidArgument` 或 `kInvalidState`  |
| `rhi::kRemaining`                                         | `UINT32_MAX`                                | `SubresourceRange::mip_count/layer_count` 的“从 base 到末尾”展开请求    | 只能用于支持该语义的 range count 字段；不能当作实际层数或无效 Handle     |
| `Category::kCount`                                        | `Category` 最后一个枚举成员                 | Logging 类别数量的边界哨兵                                              | 不能传给日志宏或 `set_category_level()`                                  |
| `logging::kCategoryCount`                                 | `static_cast<size_t>(Category::kCount)`     | 为类别级配置数组分配槽位的容量                                          | 可以用于数组大小，不能当作有效 Category                                  |
| `render_graph::kQueueCount`                               | 当前为 `3`                                  | FrameContext 维护的队列槽位数量：Graphics、Compute、Copy                | 只能用于内部/数组容量；不等于 `QueueClass` 枚举值数量                    |
| `GpuCompletionSet::kCapacity`                             | 当前为 `8`                                  | 一个 Completion Set 最多保存的不同 Timeline 数量                        | 超过容量时 `add()` 返回 `false`；同一 Timeline 会合并为较大 value        |
| `DeviceCreateDesc::preferred_adapter_index == UINT32_MAX` | `numeric_limits<uint32_t>::max()`           | 请求综合评分自动选择 Adapter                                            | 是一种控制请求，不是实际 Adapter index；其他值才表示枚举结果中的明确索引 |
| `Handle::value == 0`                                      | 所有 RHI Handle 的默认值                    | 无效/未绑定对象                                                         | 不能提交给创建以外的对象操作                                             |
| `generation == 0`                                         | RenderGraph Handle 和 `FrameToken` 的默认值 | 没有建立有效代际关系                                                    | 不能作为有效资源或帧 Token                                               |
| `FrameToken::frame_slot/image_index == kRemaining`        | Frame Token 默认槽位/图像索引               | 尚未获得 Swapchain Image 或 Token 无效                                  | 不能提交或呈现                                                           |
| `BufferRange::size == UINT64_MAX`                         | Buffer Range 默认大小                       | 从 `offset` 延伸到 Buffer 末尾                                          | 只在 Buffer Range/View 范围解析中有该含义                                |

`render_graph::kInvalidHandleIndex` 与 `rhi::kRemaining` 当前可能具有相同的底层数值，
但它们的语义完全不同：前者表示“没有索引”，后者表示“展开剩余范围”。任何跨
模块序列化都必须同时记录字段类型，不能只记录这个整数。

当前 `FrameContext::queue_index()` 的公开映射规则如下：

| `QueueClass` |         Frame Queue Slot | 是否可直接录制 Command List                                                         |
|--------------|-------------------------:|-------------------------------------------------------------------------------------|
| `kAuto`      | `0`，先规范化为 Graphics | 是，按 Graphics 处理                                                                |
| `kGraphics`  |                      `0` | 是                                                                                  |
| `kCompute`   |                      `1` | 是                                                                                  |
| `kCopy`      |                      `2` | 是                                                                                  |
| `kPresent`   |       `0` 的呈现归属语义 | 否；Present 不是 Command List 录制队列，`FrameContext::begin_command_list()` 会拒绝 |

因此 `kQueueCount == 3` 不意味着可以创建第四个 Present Command List；Present 是
Swapchain 生命周期操作，必须通过 `submit_frame()` 和 `present()` 完成。

### 28.11 枚举查阅、组合、错误和演进规则

#### 开发者查阅一个枚举字段的固定步骤

遇到任何公共枚举字段，必须按以下顺序判断，不能只看枚举名字：

1. 先确认字段所属抽象层：Pipeline、RHI 还是 RenderGraph。
2. 确认它是普通枚举还是位掩码；只有标注为可组合并启用 Bitmask Operator 的类型
   才能使用 `|`/`&`。
3. 查该字段的默认值，区分默认业务值、空集合、自动选择、未知格式、范围展开和
   无效句柄。
4. 查资源类型和 Capability 约束，再查 View Kind、Binding Type、Usage、Stage 和
   Queue 是否相容。
5. 查当前后端支持矩阵；“语义合法”不等于“当前设备实现了该能力”。
6. 根据失败原因选择 `ResultCode`，不能通过检查 `Result::message` 文本来判断类型。

#### 关键保留词的精确定义

| 词/值          | 统一语义                                      | 常见误用                                        |
|----------------|-----------------------------------------------|-------------------------------------------------|
| `kNone`        | 空集合或没有可选能力                          | 当作“全部允许”                                  |
| `kUnknown`     | 当前字段没有确定的实际格式/映射               | 当作任意格式或自动格式                          |
| `kAuto`        | 请求 Host/RHI/Scheduler 按公共选择策略决定    | 当作通配符，或要求每个后端选择不同语义          |
| `kAll`         | 对该 Mask 定义范围内的全部已知 bit 的宽泛请求 | 当作未来新增 bit 已自动包含，或当作精确同步阶段 |
| `kAllCommands` | Pipeline Stage 的保守全命令同步范围           | 当作 `ShaderStage::kAll` 或最优性能设置         |
| `kCount`       | 数量/数组边界哨兵                             | 作为实际业务类别                                |
| `kRemaining`   | 从指定起点展开到资源结尾                      | 作为无效索引或实际数量                          |
| `kDontCare`    | 明确放弃旧内容或结束内容保留                  | 当作清零、加载或“实现自行决定但调用者仍可读取”  |

#### 错误码选择

| 情况                                      | 应返回的 `ResultCode`                | 说明                                                                                      |
|-------------------------------------------|--------------------------------------|-------------------------------------------------------------------------------------------|
| 枚举值或组合在字段语法上不合法            | `kInvalidArgument`                   | 例如 `kFixed` 配 `array_count == 0`、未知 bit、错误 Aspect                                |
| 值在语义上合法但当前设备/后端未实现       | `kUnsupported`                       | 例如当前 Vulkan 的 Bindless、动态 Offset、Acceleration Structure                          |
| Handle、generation 或对象记录不存在       | `kNotFound` 或 `kInvalidState`       | 句柄不存在通常是 `kNotFound`；对象已过期或状态跟踪不一致通常是 `kInvalidState`            |
| 对象存在但调用时序不允许                  | `kInvalidState`                      | 例如未 Begin 的 Command List、显式 Barrier before 状态不匹配                              |
| 外部 Surface/Swapchain 状态发生变化       | `kOutOfDate` 或 `kSurfaceLost`       | 调用方应重建相关对象，不应把它当作普通参数错误                                            |
| 版本字段与当前模块/Shader Contract 不兼容 | `kUnsupported` 或 `kInvalidArgument` | 当前 `ResultCode` 没有专门的版本不兼容成员；具体版本号和诊断 ID 必须保留在错误载荷/消息中 |

`ResultCode::kSuccess` 是唯一成功值；`ResultValue<T>` 只有在成功时其 `value` 才有
意义。未知枚举值不能被静默映射到默认值，除非该字段在本节明确规定了回退策略；当前
明确允许回退的主要是 Swapchain 的有效呈现配置，并且必须向调用者报告最终值。

#### ABI、序列化和后端演进

- 公共枚举的底层类型、已发布数值和既有 bit 不能重新编号、复用或改变含义。新增
  普通枚举值只能追加未使用数值；新增 Mask bit 只能占用未使用 bit。
- 当前源码中没有给所有普通枚举逐项写出显式整数，但源代码声明顺序已经形成当前
  ABI 数值；后续修改应显式保留旧值，不能通过插入新成员改变旧成员的数值。
- 插件、Shader 包或磁盘数据跨模块传递枚举时，必须同时带有数据版本和字段类型。
  读取方先检查版本，再检查已知值/未知 bit；不能仅凭底层整数猜测其属于哪个枚举。
- C++ ABI 边界不得传递依赖编译器布局的原生后端枚举、`Vk*`、`ID3D12*` 或带有
  后端指针的结构；公共枚举只作为固定宽度的语义字段传递。
- 后端可以把同一公共值映射到不同原生 API 值，但必须保持本节的可观察语义。若无法
  保持，应返回 `kUnsupported`，不能静默改变诸如 Load/Store、Binding Type、Queue
  Ownership 或 Address Mode 的含义。
- 诊断中应同时记录枚举类型名、枚举成员名、底层整数、所在字段、资源/Binding 身份、
  后端和版本。这样未知值才可以被定位，而不是退化成“参数错误”。

#### 当前 Vulkan 后端的能力边界速查

| 功能类别      | 当前可用值/组合                                                                     | 当前明确返回 `kUnsupported` 的情况                        |
|---------------|-------------------------------------------------------------------------------------|-----------------------------------------------------------|
| Present Mode  | 请求值若存在则使用；否则按 FIFO/可用模式回退并报告有效值                            | Surface 没有任何呈现模式，或请求的图像 Usage 不受支持     |
| Buffer View   | Uniform、Storage、Raw、Structured、Typed（Typed 需 Format）                         | 加速结构相关 Buffer Capability                            |
| Texture View  | Sampled、Storage、ColorAttachment、DepthStencil，遵守 Format/Aspect/Capability 约束 | 不支持的格式、维度或 Capability 组合会被拒绝              |
| Binding Array | Single、Fixed（Fixed count 非零）                                                   | Variable、Unbounded、Bindless、PartiallyPopulated         |
| Binding Flags | `kNone`                                                                             | DynamicOffset、Immutable，以及当前实现无法支持的未知组合  |
| Binding Type  | 除 Acceleration Structure 外的已实现 Buffer/Texture/Sampler 类型                    | `kAccelerationStructure`                                  |
| Sampler       | Nearest/Linear、四种 Address Mode、可选 Anisotropy                                  | 各向异性 Feature 未启用；Border Mode 无法保持固定边界颜色 |
| Barrier Mode  | Implicit、Explicit；RenderGraph 使用 Explicit                                       | 显式状态与跟踪状态不匹配、跨队列依赖缺失                  |

这张表只描述当前后端实现，不改变前面各枚举的公共语义。新增 D3D12 或其他后端时，
应复用同一公共枚举和错误规则，并单独维护后端能力矩阵；不得为了迎合某个后端而把
公共值重新解释成后端专属概念。
