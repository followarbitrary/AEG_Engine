# Feature 五域架构设计

> 文档性质：目标架构与迁移规范（非代码实现计划）
> 版本：0.2
> 更新时间：2026-10-18
> 状态：FeatureGraph-only 原Tasks1–7主线已实现并完成最终验收；历史阶段状态保留，当前结论见文末2026-10-18最终验收与逐项矩阵

## 1. 决策摘要

本方案将 `Pipeline / Feature / Pass` 三层业务模型替换为 **Feature 五域模型**：

- **Feature 是唯一的功能单元**。任何需要被作者配置、注册、复用、观测、启停、持有跨调用状态或参与依赖解析的能力，都以 Feature 身份存在。
- **五域是能力分类和契约边界**：`Logic`、`Physics`、`Render`、`Audio`、`Stream`。域不是父子层级，也不决定状态性、线程模型或时间尺度。
- **Atomic / Composite 是结构维度**。Atomic 是一个不可再拆的域契约实现；Composite 是递归组合的 Feature。二者都可以有状态，Composite 可以同域嵌套，也可以跨域嵌套。
- **Call / Process / Continuous 是执行策略**，不是新的业务类型。一次函数调用、跨帧流程和持续服务分别用这三种策略描述；状态由独立的 `StatePolicy` 声明。
- `Pipeline` 和 `Pass` 从作者业务身份降为内部执行机制。Pipeline 表示某个 Scope/Clock 的计划或调度投影；RenderGraph Pass 表示后端工作的临时节点。正式新资产只写 Feature。

本轮实现决议：用户已明确“不用兼容旧项目”。游戏 SDK、模块 ABI 和正式资产只接受新的
FeatureGraph/FeatureModule 资产与 API，不读取旧 `.aegpipe`、旧 AEGP 字节流，也不保留旧项目启动适配器。
Engine 内部为 Frame/Render projection 保留的 TypeCatalog、PipelinePlan 和 RenderGraph Pass 不属于
游戏模块的作者身份，编辑器必须把它们显示为派生投影或内部诊断。

FeatureDefinition 和 FeatureCatalogEntry 同时携带发布模块的稳定 `owner_module`。
模块作者入口在写入 Catalog 前绑定该身份，ModuleRegistry、静态 FeatureModule 和
metadata 解码都校验归属一致性；Editor 只能把通过归属校验的 Feature 条目加入可视化目录。

因此，状态机的 function 层不再被硬塞入 Pipeline/Feature/Pass 层级：普通函数保留为 Feature 或 Process 的私有实现；需要独立作者语义的计算则登记为一个 `Call` 策略 Feature；状态机本身是 Logic Feature 内部的 Process/StateMachine，而不是第四种功能层。

## 2. 问题与范围

现有三层在 Frame/Render 图中能够近似成立，但将它外推到跨帧 Logic、Physics、Audio、Stream 后会产生三类混淆：

1. `Pipeline` 既被当作计划容器，又被当作功能所有者，导致状态机状态和执行计划生命周期混在一起。
2. `Feature` 既表示能力，又被当作 Pipeline 的子节点；同一个跨域能力无法自然复用或递归组合。
3. `Pass` 是帧内执行节点，却被误当作所有 function 的通用粒度；一次伤害计算、一个状态转移和一个 RenderGraph Pass 的生命周期完全不同。

本规范重定义作者模型和运行时契约；本轮实现按上述 FeatureGraph-only 决议修改 C++ 公共接口和 ABI。当前实现状态与目标语义必须在文档中分开标注。

## 3. 正交维度

### 3.1 五域

| Domain | 关注对象 | 典型资源 | 常见时钟示例 |
|---|---|---|---|
| `Logic` | 规则、实体、状态机、玩法意图 | `WorldSnapshot`、`Intent`、`GameEvent` | tick / event |
| `Physics` | 空间约束、求解、碰撞 | `Transform`、`Force`、`CollisionEvent` | fixed tick |
| `Render` | 可见性、图形资源、提交准备 | texture、buffer、view、draw intent | frame |
| `Audio` | 播放、混音、空间声学 | sample block、voice、audio event | sample block |
| `Stream` | 网络、文件、外部流和连接 | packet、byte stream、IO result | event / readiness |

域只规定接口、资源语义、权限和验证规则。一个 Render Feature 可以保存跨帧历史，一个 Logic Feature 可以是无状态查询；不能由域名推导 `StatePolicy`。

### 3.2 结构、执行、状态与时间

| 维度 | 取值 | 作用 |
|---|---|---|
| 结构 | `Atomic` / `Composite` | 描述 Feature 是否由子 Feature 组成 |
| 执行策略 | `Call` / `Process` / `Continuous` | 描述一次调用、可暂停流程、持续服务 |
| 状态策略 | `Stateless` / `InstanceState` / `ProcessState` / `ExternalSnapshot` | 描述状态所有者和提交边界 |
| 时钟 | `Frame`、`FixedTick`、`LogicTick`、`SampleBlock`、`Event` 或注册时钟 | 描述推进和截止时间 |
| 作用域 | Engine、World、Scene、Frame、Asset、Connection 等 | 描述实例可见范围 |

这些列可以任意组合，但必须显式声明。例如 `Atomic + Logic + Process + ProcessState` 可以承载技能状态机；`Atomic + Render + Continuous + InstanceState` 可以维护历史帧重投影；`Call` 只表示没有可恢复 continuation，不表示实现内部不能使用临时局部变量。

### 3.3 A/B/C/D 语义映射

| 标识 | 用户含义 | 在目标模型中的落点 | 关键边界 |
|---|---|---|---|
| A | 功能层：相对 Frame 的全部 CPU 逻辑 | Feature 的业务外壳、实例和状态所有权；也可覆盖 Physics/Audio/Stream 的 CPU 能力 | “功能”不等于 Pipeline；只把需要独立身份/契约的能力注册为 Feature |
| B | 函数层：一次函数调用 | 私有函数或 `Call` 策略 Feature 的一次 `Invocation` | 默认不跨调用、不持有 continuation；诊断可带 `InvocationId` |
| C | 流程层：跨帧玩法流程 | Feature 持有的 `ProcessInstance`、State/Transition 和等待条件 | 可暂停、恢复、取消、重试和迁移；不展开为 Stage/Pass |
| D | 逻辑域：五域中的 Logic | `Domain=Logic` 的分类、资源权限和 Clock 合同 | 不是所有 A/C，也不是执行层级；其他四域同样可以有状态流程 |

“function”在本规范中默认指 B 的一次调用粒度。只有当该调用需要独立作者契约、权限、观测、
缓存或重试语义时，才提升为 `Call Feature`；普通 C++ 函数和状态机 Action 仍保持实现细节。

### 3.4 合法组合与 ClockContract

“正交”表示维度分开声明，不表示所有笛卡尔积都合法。编译器和运行时应按下表拒绝隐含
语义：

| ExecutionKind + StatePolicy | 允许性 | 约束 |
|---|---|---|
| `Call + Stateless` | 允许 | 调用结束即完成；输出只能通过返回值、Delta、Intent 或 Receipt 发布 |
| `Call + InstanceState` | 允许 | 只允许在调用提交边界读写显式 Owner 状态；不得等待或保存 continuation |
| `Call + ProcessState` | 禁止 | 需要 continuation 时改建 `ProcessInstance` |
| `Process + ProcessState` | 允许 | Process 必须有独立 ProcessId、StateVersion、取消代次和恢复点 |
| `Process + ExternalSnapshot` | 允许 | 状态由声明的外部 Owner 提供版本化 Snapshot/Command；不能隐式写共享对象 |
| `Process + Stateless` | 有条件允许 | 仅当 continuation 完全由显式外部调度状态表示，并声明恢复/取消协议 |
| `Continuous + Stateless` | 允许 | 每次处理只依赖输入和外部版本；不得通过隐藏静态变量保存状态 |
| `Continuous + InstanceState` | 允许 | 声明服务状态 Owner、关闭/排空和重启语义 |
| `Continuous + ProcessState` | 有条件允许 | 只能表示服务管理的子 Process；子流程身份不得与服务实例混用 |

每个 Feature 还必须声明 `ClockContract`：`ClockId`、推进频率或 readiness 来源、phase/截止
时间、最大 backlog、迟到数据策略、同一时钟内的稳定排序键，以及跨时钟边的 Snapshot/Queue/
Event/IO 适配。`Frame`、`FixedTick`、`LogicTick`、`SampleBlock` 和 `Event` 是预置 profile，
不是五个 Domain 的固有属性；注册新 Clock 必须经过版本化能力注册和实时预算审核。

## 4. Feature 模型

### 4.1 定义、节点、实例和调用身份

以下身份不可混用：

| 身份 | 生命周期 | 必须稳定的字段 |
|---|---|---|
| `FeatureDefinition` | 共享、不可变 | `FeatureDefinitionId`、`DefinitionVersion`、Domain/Contract 摘要 |
| 作者节点 | 资产生命周期 | `NodeId`、端口/参数 schema、父节点路径 |
| `FeatureInstance` | Scope 生命周期 | Definition、Scope、`OwnerPath`、实例状态 |
| `ProcessInstance` | 流程生命周期 | `ProcessId`、`ProcessRevision`、取消代次、状态版本 |
| `Invocation` | 一次执行 | `InvocationId`、`AttemptId`、Clock/输入版本 |
| `ExecutionPlan` | 编译或调度周期 | `PlanRevision`、依赖排序、临时 WorkItem |

同一个 Definition 在两个作者节点中出现时，默认创建两个实例；共享缓存必须以显式 `SharedResource`/`CacheKey` 声明，不能依赖 Definition 单例。`InvocationId` 不是业务状态身份，迟到回调必须同时核对实例、状态版本、计划代次和 AttemptId。

### 4.2 Atomic Feature

Atomic 是在当前契约边界上不可再拆的实现单元，不等于一次函数调用，也不要求无状态。它只属于一个 Domain，声明自己的输入、输出、时钟、权限、状态和副作用。Atomic 可以在内部调用普通函数、使用本域私有状态机，或展开成多个内部 WorkItem；这些实现细节不会生成新的作者业务层。

### 4.3 Composite Feature

Composite 是拥有子 Feature 引用和组合契约的 Feature。允许：

- 同域递归组合，例如 `LogicCombat` 组合多个 Logic Atomic；
- 跨域组合，例如 `GameplayPresentation` 组合 Logic、Physics、Audio 和 Render 子树；
- Composite 嵌套 Composite，直到编译器检测到结构递归环。

Composite 的 `DomainSet` 是递归叶子域的并集，不引入 `Mixed` 或 `Composite` 第六域。Composite
本身只负责结构组合和契约映射；协调状态、流程和状态机必须由子树中的明确 Atomic/Process
Feature 持有。当前公共契约没有 `CoordinatorFeature` 字段，因此禁止把 Composite 声明为
Process/Continuous 或持有 ProcessState；不得隐含一个跨五域共同写入的全局可变状态。

子 Feature 保留各自的状态所有权、时钟、权限和提交边界。父节点只声明组合依赖、数据映射、失败策略和启停策略，不取得子状态的别名写权限。

## 5. 函数层、流程层和状态机

### 5.1 Function 的三种情况

1. **私有函数**：只服务于某个 Feature/Process，未被作者单独配置，不注册为 Feature。
2. **Call Feature**：一次调用本身需要稳定契约、参数 schema、权限、观测或缓存时，登记为 `Call` 策略 Feature。调用完成后不保留 continuation。
3. **Process Feature**：需要跨帧等待、暂停、恢复、重试或子流程时，登记为 `Process` 策略 Feature，拥有独立 `ProcessInstance`。

因此“function 指代一次函数调用”只适用于第二列语义；它不是把每个 C++ 函数都提升为架构对象，也不把状态机的 transition action 误标成 Pass。

### 5.2 状态机规则

- 状态机属于拥有它的 Logic Feature 或明确声明为其他域的 Feature；状态机不是独立的第五层。
- Guard 应是无副作用读取；Action 只能产生声明性的 `StateDelta`、`Intent` 或 `Event`，由指定 Owner 在提交边界校验并应用。
- 状态提交顺序为 `Exit -> StateDelta/ProcessState -> Enter -> Outbox`。同一实例同时到达多个转移时，按显式优先级、因果序号和稳定 NodeId 决定，不依赖线程完成顺序。
- 状态机循环与结构组合循环不同：Composite 结构必须无环；跨 Tick 的状态反馈允许存在，但必须经过显式事件、队列、快照或至少一个 Clock 边界。
- 迟到的异步结果必须携带 `ProcessId + ProcessRevision + StateVersion + AttemptId + RuntimeGeneration`；任何字段不匹配都只能生成诊断，不得写入当前状态。
- 取消只废弃尚未提交的结果。已经发送的网络包、已提交的 GPU 命令、已播放的音频和外部文件写入不能伪称回滚。

### 5.3 状态与共享世界

`FeatureInstance` 独占自己的状态；Logic/Physics 的 World 是多个 Feature 通过 Command/Delta 提交后的领域聚合视图。Feature 可以读取不可变 Snapshot，不能直接取得其他 Feature 的可变状态指针。跨域共享使用 Resource Contract：Snapshot、Queue、Event、Command 或 Receipt，并声明版本、容量、背压、去重、超时和丢失策略。

同一 Owner 内的 State/Exit/Enter 可以做局部原子提交。跨域默认是局部提交加 Receipt/补偿，不能承诺五域同时生效，也不能把 ResourceBus 当作分布式事务。

## 6. 时钟、调度与执行计划

时钟是 Feature 的契约字段，不由 Domain 名称隐含决定。一个 Composite 可以连接多个时钟，但必须声明每条边的采样/排队规则：

- Snapshot 边注明读取的版本和最大允许延迟；
- Queue 边注明容量、背压、丢弃或合并策略；
- Event 边注明因果序号、去重键、顺序和超时；
- 外部 IO 边注明 readiness、取消和重试语义。

编排器为每个 Clock 生成局部 `ExecutionPlan`，在跨时钟边界插入明确的同步或数据转换 WorkItem。不存在固定的“Audio 必须先于 Stream、Stream 必须先于 Logic”全局优先级；只有资源依赖、截止时间、实时预算和显式优先级共同决定可执行顺序。实时音频线程不得等待任意 World 提交，GPU 提交也不得依赖可无限阻塞的业务锁。

引擎内部执行五域 Feature、Process 和 `ExecutionPlan` 时，线程创建、异步任务提交、完成等待、
取消和跨时钟并行必须统一通过 `aeg::concurrency`。内部运行时使用 `ExecutorRegistry`、
`WorkerExecutor`、`TaskGraph`、`TaskGroup`、`Completion` 和 `CancellationToken` 等抽象；
引擎其他模块不得直接创建 `std::thread`/`std::jthread`，也不得直接使用 `std::async`、
`std::future` 或自行维护线程池。此实现规则只约束引擎内部，不约束游戏开发者自己的线程或协程。
`Concurrency` 不属于游戏 SDK、Feature 合约或 Module ABI；引擎调度器根据声明完成并发编排，
游戏作者不接收执行器、取消令牌或完成状态类型。线程句柄、创建方式和生命周期细节不能泄漏到
游戏 Feature 代码。
普通数据结构的短临界区保护不改变任务的执行域，也不能被用来替代上述执行器、完成或取消协议。

`Pipeline` 在此模型中是计划名称或旧 API 的适配名：它可以包含多个 Feature 的执行投影，但不拥有 Feature 业务状态。计划重建、缓存失效、WorkItem 取消和完成回收都属于运行时机制；完成回收不能改变 Feature 的定义或实例身份。

## 7. Render 的特殊实现边界

Render Feature 仍可编译成 RenderGraph 的 Pass、Barrier 和 Queue 提交，但这是一种内部投影：

- 一个 Render Feature 可以产生多个 RenderGraph Pass；一个 Pass 不能反向成为通用 Feature 类别。
- 只有需要作者配置/复用/状态/观测的 Render 能力才注册为 Feature；纯内部拷贝、Barrier、资源清理保留为 WorkItem/Pass。
- `Graphics Pipeline`、Shader、PSO 等 RHI 对象的 “pipeline” 名称不受本业务模型改名影响；它们是 GPU 后端资源。
- Frame 是 Scope + Clock + transient 资源的边界。跨帧历史、缓存和异步上传由 Render Feature 或共享资源声明所有权。

示例：`DeferredRenderer` 是 Composite Render Feature，子 Feature 可以是 `ShadowFeature`、`GBufferFeature`、`LightingFeature` 和 `PostProcessFeature`；每个子 Feature 内部仍可展开多个 RenderGraph Pass。`GameplayPresentation` 则可以组合 Logic 的可见性意图、Render 的提取、Audio 的事件和 Stream 的资源准备，但不创建一个隐含的“跨域 Pass”。

## 8. 作者、编辑器与可观测性

编辑器以一个 Feature Graph 为主模型：

- 节点目录按五域分组，另以 Atomic/Composite、Call/Process/Continuous、Scope/Clock 作为筛选维度；
- StateMachine 是 Logic/Process Feature 内的一个可视化子视图，不是平行的业务层；
- 参数、端口、资源契约、权限、失败策略和状态 schema 都来自 Definition/Contract；
- 作者只编辑 Feature 节点和连接，编译后查看派生的 ExecutionPlan、RenderGraph Pass、队列和资源生命周期；
- 运行诊断同时展示 Definition、Node、Instance、Process、Invocation 和 WorkItem 的链路。

目录和资产禁止把 `Pipeline`、`Feature`、`Pass` 作为三类平行的业务身份。正式保存的资产只写 Feature 身份；旧名称不进入新目录或 schema。

## 9. 旧三层架构的迁移语义

迁移是语义转换，不是字符串重命名。资产至少分别保存：`AssetFormatVersion`、`ContractVersion`、`StateSchemaVersion`、`StateVersion`、`PlanRevision` 和 `RuntimeGeneration`。

### 9.1 当前公共模型的实现边界

FeatureModule/运行时的游戏作者入口已经删除 `ModuleDefinition` 及其
`add_pipeline/add_feature/add_render_pass` 注册模板；游戏模块只发布 `FeatureDefinition`。
Engine 内部的 `AuthoringTypeKind`、`PipelineInstanceConfiguration`、`FeatureInstanceConfiguration` 和
`RenderPassInstanceConfiguration` 仍可以作为 Frame/Render projection 的实现数据结构，但不得从
GameSDK、FeatureGraph 资产或 Feature 节点生成器导出。以下规则只约束这些内部执行机制，不代表目标模型保留三种平行业务身份：

- `PipelineDefinition`、`PipelinePlan`、Stage/Task/Artifact 的固定排序和 FeaturePoint anchor
  是 `ExecutionPlan` 的 Frame/Render profile。业务输入、输出、权限和状态必须能追溯到
  Feature；只有同步、屏障、阶段锚点等框架工作可以标记为 `profile-infra`。
- 现有 `PipelineFeatureContributionRecord` 只记录 Stage 范围，且当前 Plan node 没有 Feature
  owner provenance。目标模型必须补充“Feature owner / profile-infra”诊断规则或未来扩展字段；
  在此规则落地前，不能声称任意 Pipeline Task 都已归属于 Feature。
- `FeaturePointDescriptor` 保留为 Frame/Render 的 `ProjectionAnchor` 和权限合同：
  `allowed_domains`、`allowed_scopes`、queue/pass/output/snapshot flags 只在对应 profile
  生效，不得把 Render 权限推广为 Logic/Process 权限。
- 内部 TypeCatalog 仍支持三种 projection kind（Frame plan、Feature projection、Render work item），
  没有独立的 Flow/StateMachine kind。Flow/StateMachine 作为 Feature-owned authoring extension
  保存；若以后需要独立目录类型，必须以新的 Contract/asset schema 版本和 ABI 可选尾部引入，不能
  把 projection kind 暴露给游戏作者。
- 当前 `pipeline_configuration.hpp` 不含 `FlowDefinitionId`、`FlowInstanceId`、`OwnerPath`、
  `StatePolicy` 或 `Clock` 字段。它们属于目标 Flow profile 的新记录，不能复用
  `FeatureInstanceConfiguration::passes` 偷渡，也不能把运行时状态写入 `PipelinePlan`。

| 旧对象 | 新对象 | 保留规则 |
|---|---|---|
| Pipeline（作者业务根） | Composite Feature | 原 Scope、Clock、依赖、输出、启停和权限成为 Composite 合同；固定 Stage 不再是业务层 |
| Pipeline（运行计划） | ExecutionPlan / Pipeline 兼容投影 | 保留调度、排序、缓存和诊断语义；不迁移为 Feature 状态 |
| PipelineFeature | Atomic 或 Composite Feature | 按真实 Domain、结构和状态策略拆分；不能只按旧继承名判断 |
| RenderPass（作者可复用节点） | Atomic Render Feature 或 Render Composite 子节点 | 保留资源绑定、队列、顺序、参数、最终输出和权限约束 |
| RenderGraph Pass / Task | WorkItem / 内部 RenderGraph Pass | 保留执行和资源生命周期，不进入作者 Feature 目录 |
| 一次 function call | 私有函数或 Call Feature | 仅在需要独立契约、观测、权限或复用时提升 |
| 跨帧技能/行为/加载 | Process Feature + ProcessInstance | 保留状态、暂停/恢复、取消、重试和迁移协议 |

旧资产不参与本轮运行；新资产直接以 FeatureDefinition/FeatureGraph/FeatureInstance/ProcessInstance 编译和发布。验证失败时保留当前活动 RuntimeGeneration；旧资产或旧异步回调不进入新运行时。

## 10. 验收标准

目标架构只有在以下行为都成立时才算完成设计闭环：

1. 同域嵌套、跨域嵌套和同一定义的重复实例均合法，实例状态互不串写；结构递归环会被拒绝。
2. Logic 状态机可连续 Tick，支持暂停、恢复、并行子流程和迟到完成；旧 StateVersion/AttemptId 不能修改当前实例。
3. Logic→Physics→Logic 反馈具有显式 Snapshot/Queue/Event 边界；Audio 消费不等待 World 提交；跨域没有虚假的全局原子事务。
4. 失败、重试和取消不会产生半提交；外部副作用不会因重试重复发生；取消不会伪称撤销已发出的副作用。
5. 热迁移成功保持可迁移状态，失败保留旧代次；旧定义、计划和回调不能写入新实例。
6. 新 RenderFeature 的依赖、权限、参数、资源版本、队列约束和最终输出可验证；内部 RenderGraph Pass 仍可观测。
7. 新作者目录和新资产没有 Pipeline/Feature/Pass 三类平行业务身份；Pipeline 只作为计划或兼容投影出现。

## 11. 非目标与文档优先级

本文不规定 `aeg::concurrency` 的下一版内部线程实现，也不要求把所有内部函数包装成 Feature；
但第 6 节的五域并发边界属于稳定架构约束。本轮代码实现负责落实 FeatureGraph-only 的 C++ 接口和 ABI，
并保证引擎内部五域执行入口通过该并发抽象执行；游戏 Feature/Module 只接收声明式契约，
不接收 Concurrency 类型。

文档冲突时优先级为：本文件的目标语义与本轮 FeatureGraph-only 决议 → `5Domain.md` 的五域术语和契约 → 各模块公共头文件与实现的当前事实 → 旧 Pipeline/Pass 使用手册。旧手册中的三层图只作为待删除实现事实，不得反向修改本目标模型。

## 12. 架构审核结论（方案级，2026-10-04）

### 12.1 结论

**目标模型在概念上符合五域，但“只写 Feature、自动编排五域”必须以契约闭环为前提。**
方案已经把业务身份、域分类、结构组合、执行策略、状态所有权和时钟边界分开；仍需把这些
契约收敛为可验证的作者输入，才能支持稳定的图形编排。

| 审核问题 | 方案级结论 | 需要满足的条件 |
|---|---|---|
| 是否是五域而不是三层 | 是 | Composite 只能是结构组合，DomainSet 必须由五个叶子域递归并集得到，不能引入第六域 |
| 开发者是否只写 Feature 代码 | 有条件地是 | Feature 必须同时提供可导出的 Contract；Contract 至少描述输入、输出、完成点、状态、权限、副作用、Clock 和版本 |
| 图形编排器能否编排 Feature | 可以 | 编排器必须读取 FeatureDefinition/Contract，生成 Feature Graph，并按域和时钟派生 ExecutionPlan |
| 状态机是否属于 Feature | 是 | StateMachine 作为 Logic/Process Feature 的内部子图；ProcessInstance、StateVersion、取消和恢复不能被计划展开吞掉 |
| 五域能否独立调度 | 可以 | 每个域拥有可验证的 ClockContract；跨域连接必须显式使用 Snapshot、Queue、Event、Command 或 Receipt |
| Render Pass 是否是通用功能层 | 否 | Pass 只作为 Render Feature 的执行投影；其他域使用各自的 WorkItem/ExecutionPlan 语义 |

### 12.2 “开发者只写 Feature”应如何准确理解

目标落地后，开发者不再编写业务 Pipeline、通用 Pass 或手工 Stage 排序，但仍需提供一份
Feature Contract：Domain、端口/资源 schema、Scope、Clock、StatePolicy、权限、副作用、
失败/重试和版本迁移信息。Contract 可以由 Feature 声明自动生成，也可以由声明式资产补充；
“只写 Feature 代码”不等于“编排器可以从任意黑盒函数猜出依赖”。

目标工作流应是：

```text
Feature 实现 + Contract/Exporter
    -> TypeCatalog/FeatureDefinition
        -> Feature Graph（编辑器组合、参数和资源连接）
            -> Contract/Domain/Clock/权限校验
                -> 各时钟 ExecutionPlan
                    -> FeatureInstance/ProcessInstance 运行
```

如果 Feature 没有声明输入输出、完成点、状态所有权和副作用，编排器只能把它作为 opaque
节点，不能安全地排序、并行、重试、迁移或跨域连接。需要独立作者语义的 Call/Process 仍需
显式声明，不会因为写成 C++ 函数就自动获得图节点身份。

### 12.3 必须通过的落地门槛

在对外宣称“只写 Feature、图形编排五域 Feature”之前，至少要满足：

1. **身份与目录**：FeatureDefinition/Node/Instance/Process 有稳定 ID 和版本模型；
   Flow/StateMachine 有明确的子图或独立类型策略。
2. **Composite 组合**：同域、跨域和递归 Composite 都能展开叶子 DomainSet、OwnerPath 和
   子状态，并拒绝结构递归环。
3. **作者资产**：Feature Graph 是持久化根模型；Pipeline/Pass 只是派生执行视图。
4. **编排输入**：每个 Feature Contract 能描述 Domain、Clock、Scope、端口、完成点、权限、
   状态和副作用；编排器能输出 owner provenance 和诊断链路。
5. **运行时生命周期**：FeatureInstance、ProcessInstance、StateStore、取消/恢复/重试、
   代次和迟到回调校验与 ExecutionPlan 解耦。
6. **五域执行计划**：Logic/Physics/Render/Audio/Stream 各自有可验证的 Clock/Queue/Event/
   Snapshot 适配；跨域连接不依赖隐式全局顺序或虚假全局事务。
7. **编辑器闭环**：编辑器能从 Feature Contract 生成节点目录、组合图、校验错误、预览派生
   ExecutionPlan，并回溯到 Feature/Process/Invocation。

满足这些门槛后，产品表述才可以是“开发者编写 Feature Contract，图形编排器自动生成各域
执行计划”；否则只能称为 Feature 五域的设计方案。

### 12.4 当前实现审计（2026-10-05）

本分支已经把五域核心契约、Tagged ID、Composite 展开、Process 生命周期、FeatureGraph 编译器、
Asset 目标中的 AEGF 二进制资产、Application/Authoring 目标中的 FeatureGraph TOML 作者文档、FeatureGraph importer、RenderFeature 内部投影、
Feature-only 编辑器模型和 ABI 字段命名落到独立目标与 smoke tests 中。验证过的行为包括：

- 五个叶子域和正交的 Atomic/Composite、Call/Process/Continuous、StatePolicy、Clock、Scope 契约；
- OwnerPath、Definition/Node/Instance/Process/Invocation provenance、同 Clock 无缓冲环拒绝和跨 Clock 显式传输；
- 迟到 Invocation、旧 Attempt/StateVersion/RuntimeGeneration 拒绝，以及热替换迁移来源校验；
- FeatureGraph 节点、边、Process 子图、绑定、扩展和 ContractDigest 的 TOML/AEGF round-trip，旧 AEGP magic 拒绝；
- RenderFeature 生成带 Feature 来源的内部 WorkItem，作者模型不产生 RenderPass 身份。

仍不能宣称“整个引擎已经只剩 FeatureGraph”：Runtime 的 Frame/Render 执行容器、模板中的 Frame/Render
投影和部分旧 Editor 投影仍作为内部基础设施存在；它们需要继续收敛为带 Feature provenance 的
ExecutionPlan/RenderGraph 投影，而不再成为作者业务身份。当前实现已经通过本机 Visual Studio CMake
configure，之前的 Feature/Module/Project targeted CTest 20/20、最新 FeatureGraph/Runtime/Module
focused CTest 4/4，以及外部项目与游戏包 focused CTest 2/2；`aeg_application_runtime` 和
`aeg_editor_application` Debug 目标也已构建。证据还覆盖了 Module contract digest 的 canonical
metadata schema/SDK 版本边界，避免工具生成的 manifest 与静态可执行文件使用不同的契约摘要；它们不等同于
旧 Runtime/Editor 主路径已经迁移完成。跨域 Edge 的运行时
图运行时现在会把 Feature 提交的 opaque `StateDelta.output_payload` 沿声明边送入下游实例的
输入队列，按目标聚合容量拒绝无法原子提交的结果，并在 Queue 自环中只消费旧消息、保留本次
提交的新消息；这证明了图连接会影响运行结果，但还不是完整的
ResourceBus。Queue/Snapshot/Event 的 readiness、deadline、丢弃和快照版本语义、真实 RenderGraph
投影、动态模块 ABI 的 Feature 回调接入、状态载荷迁移与失败重试策略仍是后续收敛项。Project
Operation 已改为 FeatureGraph 编排和 FeatureRuntime dispatch；主 Editor 对有效 `.aegfeature`
优先使用独立 FeatureGraph 文档，已支持目录添加节点、基于 module metadata 的端口合同往返、
端口方向/Schema/版本校验的交互连边、节点/边删除、OwnerPath 级联删除、统一 Undo/Redo 和
原子保存。当前 FeatureNode 的 `parameter_blob` 仍是 Feature-owned opaque bytes，尚未提供
中性的参数 Schema/默认值/范围编辑器；因此参数图形编辑仍未完成。Scene Runtime 的视口仍通过
`.aegframe` 生成内部 Frame/Render 投影，布局持久化仍是后续工作。Editor Runtime Inspect
对已加载项目会把 live owner 的 asset UUID 映射回 `.aegfeature` 源路径；package-only Runtime
保持空来源，不读取 Editor 文档。主 Project 面板对选中的 `.aegframe` 提供
`Apply to Viewport`，失败时保留 last-good 投影并显示诊断。主 FlowGraph 空状态也会保留
malformed `.aegfeature` 的源路径和解析错误；Project Settings 使用
`Startup FeatureGraph` 作者标签，不再使用 Pipeline 身份。
Composite 在编译阶段会调用 Registry 展开以拒绝缺失子定义和递归环，运行时再物化子实例；当前
CompiledFeatureGraph 的可视化 provenance 仍以作者节点为主，尚未把每个叶子子 Feature 单独呈现在
编辑器预览中。
场景与 FeatureGraph 当前仍按独立文件顺序保存，跨文件原子提交和失败恢复尚未形成统一事务。

### 12.5 后续实现验证（2026-10-06）

本轮完成参数 `ParameterContract`、版本化参数 blob 的统一校验入口，并接入图编译、运行时发布、
ContractDigest 和 Module metadata；参数编辑器的默认值、范围和图形编辑仍需继续实现。
`Continuous + ProcessState` 服务现在可以管理独立的并行子 Process，父子关系、完成聚合、
暂停/恢复和取消传播保留各自 ProcessId、AttemptId 与 StateVersion。

FeatureGraph 的 `ProcessSubgraph` 现在显式携带 `initial_state` 与 `terminal_states`，编译阶段会
构造并校验 `ProcessStateMachine`（入口标记、状态可达性、终止状态、重复/未知转移和端口条件）。
运行时发布使用声明的入口状态，不再把声明顺序中的第一个字符串当作隐含入口；到达终止状态后，
在子 Process 已完成的前提下才提交完成状态。未填写终止列表时只派生无出边的 sink 状态，循环流程
仍需通过状态机的循环规则验证。

FeatureDefinition 和 FeatureCatalogEntry 现在携带 `owner_module`。`AEG_REGISTER_GAME_FEATURE`
在写入模块 Catalog 前绑定模块身份，静态注册、ModuleRegistry 和 metadata 解码都会拒绝归属
不一致的 Feature；该身份也纳入 ContractDigest。热替换可以更新合同和执行回调，但不能转移
模块归属；无归属的替换定义会继承当前 owner，避免热加载清除 provenance。内置 Renderer 的
`build_*_render_graph` 只保留在 `Engine/Renderer/private`，公共 Renderer 头不再把这些
RenderGraph 构建函数作为作者 API。
FeatureRuntime 在 `FeatureInstance` 和其 `ProcessInstance` 激活时快照 `owner_module`，因此
诊断和热替换期间可以从运行实例/流程回溯到发布模块，而不必依赖当前 Registry 中可能已被
替换的 Definition。

这不等于 Renderer/Runtime 的公共 ABI 已经完全脱离旧 Pipeline 类型：
`renderer.hpp`、`renderer_variants.hpp` 和 `engine_frame_orchestrator.hpp` 仍有内部
Registry/RenderGraph/Host 类型出现在公共签名中，对应 target 仍有 `aeg_pipeline` 的公共
链接传播。`pipeline_composition_runtime.hpp` 已先收回实现容器和重型 authoring include，
公共头只保留轻量的 PreparedDocument/ScopeHost 前置声明与执行身份；它仍是 Editor 内部
桥接 API，不代表 Pipeline 已从所有公共头和链接接口消失。Renderer/Frame Runtime 的
PIMPL 或内部桥接仍是下一阶段边界收口工作。

ResourceBus 已保留输出 envelope 的 schema、时间戳、序号和 transaction，Event 的通用图路由
默认启用因果序号和按路由去重；专用注册入口可以显式关闭因果追踪。Render 投影改用内部
`RenderWorkItemKind` 和确定性 `RenderWorkItemId`，并保留 `owner_module` provenance，
不再用固定 Pass 名表示操作。
这些 WorkItem 目前仍是投影数据，尚未接入真实 RenderGraph 构图/执行回调。

本机全量 Debug 构建成功；上一轮本地全量 CTest 153/153 通过，本轮针对 Feature、Module、
Editor custom node 和 Renderer 边界的 CTest 30/30 通过，其中 embed、external project 和
game project runtime 三项外部闭环 3/3 通过。参数 schema 改变 digest 后的 GameModule 模板
元数据已同步，测试覆盖了模板项目与运行时包。

本轮目录边界复核也已落实：`Engine/Asset` 只保留中立载荷解码，Renderer 顶点适配移到
`Application/Runtime`；Process 状态机语义保留在 `Engine/Feature`，坐标、颜色、Bezier 控制点
和自动布局移到 `Application/Editor`；UUID/AssetId/AssetKind 与通用参数值下沉到 Foundation，
因此 Feature、Shader、Asset 不再通过相对路径或 Pipeline authoring 类型互相反向依赖。
未接入生产目标的 `Engine/Serialization/document_io_utilities` 已删除，FeatureGraph 文档 codec 从
`Application/Project` 移到 `Application/Authoring`，保存改为
临时文件加替换，避免编辑器被中断时留下半写文件。`feature_graph_asset` 仍属于 Asset，因为它
只负责编解码 AEGF 二进制资产信封，不拥有 FeatureGraph 运行时状态。

源码目录与公共命名空间按模块职责对齐：Foundation 的共享身份/参数值、Asset 的二进制
codec、Feature 的业务语义、RenderGraph 的执行投影、Application/Authoring 的源文档 codec、
Application/Runtime 的启动/发布适配与 Application/Editor 的编辑模型各自归入对应命名空间。
同一命名空间的定义尽量集中在同一功能目录，子命名空间使用对应子目录；外部类型的前置声明、
匿名命名空间和命名空间别名不代表该目录拥有外部模块功能。

本节审计时，五域独立执行适配器、目标 Clock readiness/消费时的截止时间策略、完整 Snapshot
版本语义、动态 ABI 的执行/continuation 回调与状态载荷迁移仍是目标能力。状态载荷后续由
12.9 收口，五域异步执行由 12.10 收口；这些进展不能被表述为所有目标能力已经实现。

跨 Clock 的期限尚未闭环：当前消息和 Command 交易记录使用数值 tick，尚未携带期限所属的
Clock 身份。不同频率 Clock 的本地 tick 不得直接相减；后续必须采用声明的期限 Clock 或统一
时间基准，并在消费前判断期限，避免过期 Command 仍执行副作用。单时钟 Receipt 超时测试
不能作为跨域期限正确的证据，也不能由任意 Clock 的 tick 扫描并清理全部交易。


### 12.6 参数执行与编辑事务收口（2026-10-07）

参数信封的唯一入口位于 Engine/Feature 的 `feature_parameters.hpp/.cpp`。编码按 ParameterId
排序，解码严格消费完整信封并拒绝重复、未知、版本过期、截断、超限和缺失必填记录；底层
复用 Serialization 的 BinaryReader/Writer，作为私有链接依赖。原有参数校验复用该解码入口，
不再单独维护一份二进制解析器。

Feature 执行回调统一为 `execute(token, input, parameters)`。输入消息和实例参数是独立只读
快照，生命周期限定在回调内；移除了空输入时把参数拼接到消息前缀的隐式行为，显式调用也
提供当前实例的参数。每个作者节点保持独立参数。EditorFeatureGraphDocument 的
`set_parameters` 通过版本匹配的完整 Catalog 合同校验，并作为单次事务支持撤销/重做。
这为图形参数控件提供模型入口，尚未完成类型化控件、默认值/范围和 Module 富元数据通路。

自动重试冻结输入、参数和消息交付身份，普通调用不能绕过等待中的重试。提交只消费冻结的
MessageDeliveryId，即使 Snapshot 或容量为 1 的 Data 在等待期间替换了同内容同时间戳的
旧消息，新交付仍保留。热替换前必须验证所有受影响实例的参数；不兼容的 schema 或等待中
的自动重试会阻止发布，原定义和运行时代次保留。

本节初审时 `feature_contract_metadata` 尚未接入 Definition/Catalog/Module metadata，旧类型化值
codec 存在本机字节布局、Int32/Float 编码宽度和资产 UUID 字节丢失问题，且值类型与
Foundation 参数值重复。这些参数通路问题由后续 12.7 收口；本节参数信封入口当时不依赖该路径。
五域执行适配器、真实
RenderGraph 投影、动态 ABI 执行回调和跨 Clock 期限仍按前述审计列为未完成能力。

本轮独立审查发现并修复了显式输入缓冲区被调用者复用后改变自动重试数据的问题，覆盖了
零退避和非零退避。修改后全量 Debug 编译成功，完整 CTest 153/153 通过，包括 embed、
external project、game project runtime 三项外部构建和编辑器恢复测试；未提交。


### 12.7 类型化参数与图形编辑（2026-10-07）

唯一 schema 仍为 ParameterContract：id/version/max_size/required 只保留在这里，可选 metadata
直接携带类型、默认值、约束和 UI 提示。删除了未使用的 FeatureContractMetadata/base_contract
副本、重复的参数身份/必填字段和 Feature 私有 ParameterValue；默认值改为
optional<Foundation::ParameterValue>。Definition/Registry/Catalog 都复制这一份合同，Module JSON
完整往返 typed metadata。类型、默认值和有效性约束参与 Feature ContractDigest；显示文案和
布局提示不参与该执行摘要，模块包整体 SHA 仍包含完整元数据内容。

Typed codec 复用私有 Serialization，采用固定宽度小端整数/浮点、长度前缀字符串、完整
16 字节 UUID 和按类型固定维数的 float64 向量。拒绝错误类型、溢出、非有限数、错误维数、
无效范围/步长/枚举/正则、非法默认值、截断和尾随字节。Float 边界和格点按照 float32 线格式
验证，避免 0.1 等合法默认值因二进制舍入而被误拒绝；完整 UUID 替代旧首字节编码。
资产种类无法由 UUID 推断，须待资产目录解析上下文接入，不在当前纯参数合同中宣称支持。

信封编解码补齐缺失的 typed 默认值，执行回调前统一物化参数。因此显式输入、空输入、作者图
和 Cook 后的图使用同一默认语义；必填且没有默认的参数仍在编译/发布时拒绝缺失。已有显式
记录保持节点独立性。参数 schema 或约束变化时，现有热替换校验仍保留 last-good generation。

FeatureGraph 面板支持参数草稿、分组/排序、高级参数显示、类型控件、默认值和可选值清空。
Apply 复用 EditorFeatureGraphDocument::set_parameters 的单次事务，验证失败不改变图/历史，
成功可撤销/重做。节点切换、文档修订或 Catalog 语义摘要变化会重新载入草稿；未应用的值
不会进入保存图。底层值控件与内部 Frame 编排视图共用，保留原有非空向量维数和 opaque
参数记录，避免再写一套相同控件。

本节尚不代表五域独立执行器、真实 RenderGraph 执行投影、动态 ABI callback 和跨 Clock
期限完成；这些仍按前述依赖推进。本轮独立审查发现 Float 多步格点误拒绝和共享空向量控件
不显示分量两处 P2，均有 RED→GREEN 回归。完整 Debug ALL_BUILD 成功；全量 CTest 首跑
152/153 通过，唯一失败为 Editor 身份测试。该测试只按 PID/标题匹配，可能匹配同名启动画面
或隐藏主窗；补充真实 Win32 窗口回归后确认旧判据误匹配，修正为显示的非工具窗口并增加
退出诊断，未改变生产退出逻辑或放宽时间预算。修正后连续三次身份复验通过，最终相关
CTest 8/8 通过。原全量失败未记录 child exit，故其确切原因不作确定性归因；本节不宣称
修正后的完整 153 项再次全跑通过。日志和细节记录在实现 ledger，未提交。


### 12.8 五域异步执行的前置状态闭环（2026-10-07 审计）

本节审计时 StateStore 只保存 version/commit_count/payload_revision，没有持久化 Feature 的业务状态
字节；execute 只读取 token/input/parameters，StateDelta 的 output_payload 用于端口输出。
因此已有版本冲突和状态机跃迁校验不能作为“业务状态快照→修改→提交”已经完成的证据。
本节审计时 tick/dispatch 仍直接调用 callback 并同步 complete，Runtime 容器也没有跨线程访问协议。

后续最小实施边界应先补齐不透明状态快照和 StateDelta 状态载荷，再用项目内部 Concurrency
执行拥有 token/state/input/parameters/callback 快照的任务。完成结果由 Runtime 所属线程收件箱
回收并统一 complete；worker 不得直接修改 Runtime 或 ResourceBus。必须明确每实例在飞策略、
取消/重试/换代后的迟到结果和关闭收敛，状态与输出沿用同一原子提交边界。

Concurrency 的 Main/CPU/IO/Render/GPU 是通用执行 lane，并非 Logic/Physics/Render/Audio/Stream
五域。五域 owner 需要内部 FeatureDomain→执行适配器映射；多个 CPU 域不能因此合并为同一
所有者。游戏开发者继续只声明 Feature 合同和状态读写，不暴露 Concurrency 类型。

验收先覆盖两个同定义实例的独立状态累计、Process 后续调用读取已提交状态、长任务期间
另一 Clock 可推进、迟到结果不改状态或消费新消息、非法状态/输出原子拒绝，以及关闭任务
收敛和 GameSDK 边界；随后扩展五域映射。真实 RenderGraph、动态 ABI 和跨 Clock 期限仍为
独立后续边界。本节记录源码审计和实施顺序；真实状态载荷由后续 12.9 收口，异步调度由 12.10 收口。


### 12.9 真实状态载荷与 Owner 提交（2026-10-07）

回调更新为 execute(token, state, input, parameters)。StateSnapshot 独立拥有 payload 与
current_state，携带本次 StateVersion 和业务 schema 版本，不借用 Runtime 容器。按 StatePolicy
选择字节所有者：InstanceState 在 FeatureInstance，ProcessState 在 ProcessInstance；Process +
InstanceState 的 continuation 仍由 ProcessRevision/current_state/status 校验。Stateless 与
ExternalSnapshot 不持有本地业务载荷，显式空更新也拒绝。外部状态通过声明的输入/资源契约读取。

StateDelta 的可选 state_payload 缺省表示保留，有效空 vector 表示清空。唯一 FeatureContract
声明 StatePayloadContract 的 schema_version/max_size（默认 1.0.0 / 1 MiB，Feature 可覆盖）；
二者进入共享 ContractDigest 并在 Module/Catalog 往返，状态字节本身不写入作者图或 AEGF。
初始字节为空，由 Feature 自己解释；没有新增通用状态值、状态 codec 或第二份 schema。
Process 子图的 state_schema_version 描述状态机拓扑，业务载荷版本迁移不再重新标记它。

StateDelta 载荷、Process 跃迁、输入消费和端口输出复用同一私有暂存事务。提交前校验版本、
字节上限及可选纯 validate_state_payload；任何失败都不改变状态或队列。校验器/迁移器执行期间
Runtime 拒绝重入写操作，异常转换为失败；执行回调本身持有 Definition/callable 的副本，完成时
仍核对当前 generation/版本。真实状态存储尚不意味着完成了任意业务世界或跨域全局事务。

重试冻结 StateSnapshot、输入、参数和交付身份，同时核对原 generation/状态版本/输入版本及
current_state。暂停/恢复未改变载荷版本和状态名时，可以生成新 ProcessRevision/AttemptId；
后续失败核对当前 attempt，避免第三次重试被旧 ProcessRevision 中断。其他提交或显式跃迁
改变版本时，不再回放旧状态；旧 Invocation 的完成仍按原 revision/version 拒绝。

热替换先对所有相关 owner 的私有快照执行可选 migrate_payload，并按新合同/校验器检查，
全部成功后才替换定义、状态与 RuntimeGeneration。跨 schema 的非空载荷必须显式转换；
同 schema 缺省保留字节，空初始载荷可在新校验器接受时迁移 schema。失败保留原状态和 Clock
计划；有活跃实例时禁止变更状态所有者、结构或生命周期种类。激活前将整个 ExpandedFeature
树与 live Registry 展开/共享摘要核对，拒绝旧根或子节点，且不消耗实例身份或更改计划。

激活实例还保留由唯一合同摘要派生的 definition_digest；直接修改 Registry 不执行迁移，
摘要与活跃绑定不匹配时 invoke/complete 拒绝。合法 Runtime 替换在同一暂存事务中更新绑定，
避免新 schema/callback 读取旧存储；调用者也可明确重新发布图重建实例。

本轮独立审查发现并复现上述多次重试与旧子展开两处 P2，单次修复后 Runtime 专项通过；
收尾时也复现并阻止直接 Registry 替换绕过活跃状态迁移。最终 Debug ALL_BUILD 通过，
专项 CTest 12/12、完整 CTest 153/153（0 失败，506.13 秒）通过，覆盖 Module 元数据、
SDK、资产/回调、编辑器生成器与独立外部/游戏模板构建。模板默认图为空，外部包启动不作为
实际状态累计的证据；真实字节读写由资产到 RuntimeHost 的两轮执行与 Runtime 回归覆盖。
验证日志及边界记于实现 ledger，本轮未提交。
在本节状态载荷阶段，五域独立执行与 owner 完成收件箱仍为后续能力，后续进展见 12.10。
真实 RenderGraph、动态 ABI 和跨 Clock 期限仍未完成。

### 12.10 五域异步执行与 Owner 完成收件箱（2026-10-07）

引擎内部的 FeatureExecution 位于 Engine/Feature/private，与 Feature Runtime 同属 aeg::feature。
它复用 Concurrency 的 WorkerExecutor、CancellationSource 与 Completion，按需为五域各自创建
一个 worker：Logic/Physics/Audio 映射 CPU lane，Render 映射 RenderThread lane，Stream 映射
IO lane。通用 lane 不承担 Feature 身份，多个 CPU 域也不共享同一个 executor owner。
这是第一阶段执行配置；RenderThread lane 仍是 CPU 侧执行入口，不代表真实 RenderGraph/GPU 执行。

RuntimeBootstrap 显式启用异步策略；确定性工具与测试保留默认 inline 策略，二者共享
invoke 的快照准备、异常包装和 finish/complete 提交路径。游戏开发者继续实现 Feature 的
execute(token, state, input, parameters)，无需编写线程调度代码；GameSDK、GameRuntime 与
Module ABI 不暴露 Concurrency 类型，也不限制游戏自行使用的线程/协程实现。

每个工作项独立拥有 Definition/callable、InvocationToken、原始 token、StateSnapshot、输入、
参数与消息交付身份。worker 不访问 Runtime/ResourceBus 容器。Completion 的终态发布建立
结果读取的同步边界；所属线程的 pump_completions 回收结果，核对 generation、合同绑定、
AttemptId、ProcessRevision、StateVersion 与输入版本，再复用原子状态/输出事务。
排队取消而未进入 callback 的工作也由 Completion 结果回收，不留下永久在飞槽位。
Runtime 的读取与写入入口均检查 owner 线程；FeatureRegistry 的修改仍由引擎 owner 协调。

每实例至多一个自动在飞调用，显式 invoke/dispatch/retry 也不能绕过这一约束。同 Clock 的
Data/Command/Receipt 依赖从现有路由表派生，不维护第二份业务依赖图；下游在上游提交后
才捕获输入。忙碌 Clock 对新触发执行 Drop（合并）、Reject（背压失败）或 Queue；Queue
最多保留 max_backlog 个待执行计划，完成泵推进已接受的计划而不制造新的物理 Clock tick。
五域拥有独立执行器，长任务不会占用其他域的 worker；同 Clock 的依赖仍按提交顺序推进。

零与非零 backoff 重试均冻结原状态、输入、参数和交付身份。每次泵只回收入口时已完成的
集合，新提交的零 backoff 尝试由后续泵回收，避免单次泵无限重试。暂停/恢复保留原状态未变
时的重试语义；显式状态转换会清理旧重试并取消旧排队任务。取消上游或遇到暂停/失效节点，
整条本轮延迟依赖链撤销，不能继续执行缺少本轮输入的下游。进入终态不会遗留 retry_pending
而阻塞同 Clock 的独立实例。已提交的输出和副作用不因取消撤销。

迁移和图换代取消旧排队任务，迟到的旧结果只做回收，不改变新状态或消费消息。关闭先禁止
新提交，取消排队工作并等待所有运行回调返回，再清理收件箱和释放游戏模块、上下文及
Frame 基础设施。普通 Feature callback 没有强制中断机制；回调必须返回才能完成关闭。

独立审查确认两处 P2，三个确定性回归分别复现暂停依赖链、取消 backoff 和终态阻塞问题；
单次修复后异步专项通过。最终 Debug ALL_BUILD、专项 CTest 26/26、完整 CTest 154/154
（0 失败，484.98 秒）通过；两个未注册 CTest 的 Runtime 回调/帧时钟程序也直接验证通过。
完整测试包含外部项目、游戏模板运行包、独立 embed 构建和串行编辑器恢复；实际异步行为由
拥有同步闸门的 Runtime 回归验证。验证日志记录于实施 ledger，本轮未提交。
本阶段不声称完成真实 RenderGraph 执行、实时音频适配、动态 ABI 执行/continuation 或
跨 Clock readiness/deadline；这些仍是后续实施边界。

### 12.11 RenderIntent 到真实 RenderGraph 的首个执行闭环（2026-10-07）

Render Feature 使用现有 `execute(token, state, input, parameters)`，通过可选
`StateDelta::render_intent` 返回拥有数据的类型化意图；不增加作者回调、注册表或
GameSDK 的 RHI/Builder/Pass API。必须在唯一 FeatureContract 中声明 `Render.Intent`
v1 的 intent/owner-boundary 副作用和能力权限。只有 Atomic Render Feature 可提交；
Composite 保持组合身份，其原子叶子各自拥有结果。

owner 在原有状态、消息和输出事务中共同接受意图。省略保留前次已接受结果，有效空意图
明确停止工作；非法意图不会部分更新状态。已发布的执行计划按依赖顺序捕获拥有的快照，
包括 Composite 作者 Node、叶子 Instance、Module、Invocation 和 RuntimeGeneration。
取消、显式跃迁、迁移和图换代退休旧结果，旧快照仍可被已经提交的 GPU 工作保留。

v1 支持每实例局部单 mip/layer RGBA8 transient 纹理、颜色清屏、纹理区域复制和最终输出。
外部 RGBA8/BGRA8 颜色引用使用稳定 ID 与版本，宿主按共享或实例专属归属提供实际绑定。
RenderGraph 验证绑定、能力、格式、采样数和区域，拒绝同一底层纹理的复制别名，复用原有
backend 与完整构图回滚事务；各实例的同名局部资源不会共享。每个实际内部 Pass 携带
Feature/Module/Node/Instance/Invocation/Frame 来源，并沿用 Graph 的依赖、裁剪和同步。
清屏按资源能力使用 attachment 或 transfer 路径；最后一次交换链访问后的 Present
转换、acquire 和提交由原有 RenderGraph 处理。

RuntimeBootstrap 在现有 post-build 边界捕获结果，使用 preparation 实际解析的纹理绑定，
构图后继续原有 readback 回调。owned host 的颜色资源 ID 为 `Renderer.SceneColor`。
Feature worker 不访问 RHI；GPU 执行保留意图快照直到 FrameContext 退休，Graph reset
不会提前释放所有者。CPU owner 提交和 GPU 构图分别使用各自事务，不能宣称跨系统原子。

当前权限是每 Feature 的能力位和宿主绑定归属，尚未细化为资源端口合同。Draw/Shader/PSO
意图、完整内置 Renderer 迁移、实时音频适配、动态 ABI callback
和跨 Clock readiness/deadline 仍未完成；本节是 Task 5 的首个真实执行切片。

独立审查确认并修复三处 P2：复制采样数未校验、局部写入丢弃其他区域/子资源、不同图句柄
导入同一原生纹理绕过跨操作状态跟踪。分别复现失败后，单次修复使回归通过。真实 GPU
回读验证多 Pass 红/蓝输出、实例隔离以及局部复制后未覆盖像素和其他 mip/layer 保留；
隐藏原生窗口验证默认 ColorAttachment-only 交换链在宿主 Present 后追加 Feature 清屏，
最终仍完成呈现并保持原生 Present 状态。Native Runtime 启动图验证真实 Feature 来源，
GameSDK 编译边界拒绝 RHI/Pipeline/Concurrency 头泄漏，Vulkan validation ERROR 会使
GPU gate 失败。最终 Debug ALL_BUILD、专项 CTest 27/27、完整 CTest 155/155
（0 失败，489.60 秒）通过；未注册 CTest 的 Runtime 回调和帧时钟程序也直接通过。
验证覆盖外部工程、游戏模板运行包、独立 embed 构建和串行编辑器恢复。本轮未提交。

### 12.12 实例所有的跨帧 GPU history（2026-10-07）

作者在原有 RenderTextureDeclaration 中选择 transient 或 history 生命周期，history
需要独立的 kCreateHistory 权限。仍由同一个 Feature execute 返回意图，GameSDK 不增加
RHI、Builder、Pass 或 Concurrency 入口。v1 history 为单 mip/layer、单采样 RGBA8。

RenderGraph 所属存储按 Instance/局部资源 ID 保存最近一次成功提交的内容。构图只暂存
新纹理；局部写入复制旧内容后修改，完整写入可直接覆盖新分配。历史写入作为裁剪根，
Graph 成功执行后发布，新分配在失败时退休且不替换旧内容。相同版本必须保持尺寸，
改尺寸需新版本并成功初始化；无内容或错误版本的读取、局部写入、导出会拒绝构图。
不自动轮换 previous/current，也不自动退回清屏；首帧初始化由作者安排。

Runtime 另行捕获 live resource owners，不以“本帧没有意图”判断实例结束。资源 epoch
独立于全局 RuntimeGeneration：取消、显式状态转移及受影响迁移退休自己的资源，无关
定义迁移保留原历史。有效空意图在成功执行后释放该实例未保留的历史声明，省略意图
沿用原结果。Bootstrap 在 Graph reset 后的 post-build 边界协调存储和意图快照。

原生所有权复用原有纹理分配所有者及 FrameContext 租约。导入纹理的图视图持有底层
分配，GPU 工作退休前 Graph reset/资源取消不能提前释放它。关闭先结束 Feature 工作、
停止编排并清理 Graph，再 drain 宿主帧租约后销毁存储/设备；只有 history 的借用宿主
也执行这一屏障。包含历史事务的图成功或失败后需 reset/重建，不允许重放已接受分配。

CPU 意图接受和 GPU 历史发布仍是独立事务。异步 worker 可能在上一份意图实际渲染前
产出后续意图，CPU 状态步数不能证明历史已就绪；跨 Clock/GPU readiness receipt、
显式 temporal 角色、Draw/Shader/PSO 和完整内置 Renderer 迁移继续保留为后续项。

本切片最终 Debug ALL_BUILD 通过；专项 CTest 24/24、完整 CTest 156/156（0 失败，
344.21 秒）通过，另顺序执行 Runtime 回调和帧时钟程序均通过。真实 GPU 回读覆盖跨
Graph reset/帧槽的实例隔离、局部写入保留、版本替换和定向退休；独立审查发现的
Present 失败未结束事务已由真实原生呈现边界故障注入复现并修复。完整验证包含外部
工程、游戏模板运行包、独立 embed 构建、SDK/Concurrency 边界和串行编辑器恢复。
本轮未提交；Task 5 仍保留上述未完成范围。

### 12.13 宿主绑定场景的真实绘制切片

Render.Intent v1 增加 RenderDrawScene 和 kDrawScene 权限。作者只提交场景槽的稳定
ID/版本、外部颜色目标及显式 Load/Clear 和清屏颜色；不持有 Renderer/RHI 对象、
Builder、Pass 名称或回调。Load 还要求读取目标的权限。场景槽版本表示绑定协议，
当前帧内容由宿主不可变 RenderSceneSnapshot 提供，不能把跨帧 World revision 当作
固定绑定版本。

本阶段仅接受 2D、单 mip/layer、X1 RGBA8/BGRA8 颜色目标，目标尺寸须匹配 View；
材质 PSO 必须声明匹配的颜色格式/采样数且无深度附件。场景必须有效、属于当前
FrameContext 帧，并通过槽版本、实例所有者和唯一性检查。宿主解析器复用既有网格、
反射 Shader、材质、对象和灯光资源；本阶段执行固定材质 ABI，不提供任意 Shader/PSO
作者选择。

中立值及 CPU 权限校验属于 Feature；RenderGraph 提供引擎内部 typed lowering seam，
Renderer 实现该 seam 并复用同一 Opaque 原生构建/录制 helper。中立入口直接创建内部
Pass，不依赖旧作者 PassRegistry；一个操作展开的每个 Pass 均携带 Feature 来源并
接入操作顺序。构图仍复用整图事务，成功的场景 owner 经 Graph 租约保留到 GPU 帧退休，
解析器只在构图时借用，录制时使用已拥有的 draw packet。

惰性资源上传也属于该 Draw 操作的展开，须有相同来源并遵守操作顺序。内部上传
回执区分当前帧仍存活的 Pass 录制与已经成功提交的 GPU 批次；它不表示 GPU 已完成，
也不替代跨 Clock readiness 协议。构图回滚/reset/clear_passes 丢失未提交上传时，
解析器用原 GPU 分配和新帧暂存重新录制，材质描述符保持指向同一原生视图；不能
仅凭缓存句柄把未初始化的资源当作已上传。已提交批次即使后续呈现失败仍可复用，
帧租约和现有 RHI 状态/同步负责其在途生命周期。该机制只属于引擎内部。

Runtime 的 World view 可独立注册和提取场景，不需要构造空 Pipeline。post-build 边界
提供 owning snapshot span 和宿主解析器。恰好一个场景时发布 Runtime.Scene 槽；缺少
或多个场景时，该默认槽绘制被拒绝，宿主可显式发布多个命名槽。无 Draw 的清屏、复制
和历史意图不要求场景。CPU 意图接受与 GPU 提交仍是不同事务。

Task 5 继续保留 Shadow/GBuffer/Lighting/PostProcess 的中立迁移、旧 Renderer 作者
目录/Pass 注册删除、任意 Shader/PSO 合同，以及跨 Clock/GPU readiness 和 temporal
角色等后续项。最终 Debug 整项目构建通过；专项 CTest 27/27、完整 CTest 161/161
通过（0 失败，312.09 秒），另顺序执行 Runtime 回调和帧时钟程序均通过。真实原生回读
覆盖 Load/Clear、冷资源上传和构图回滚后的网格/纹理重试、灯光上传重录；独立审查的
三项 P2 均已修复并验证。完整验证包括外部工程、游戏模板运行包、独立 embed 构建、
SDK/Concurrency 边界与编辑器恢复。本轮未提交，详细证据见执行 ledger。

### 12.14 场景深度附件迁移

RenderDrawScene 可显式附带外部深度目标及 Load/Clear、clear_depth。与颜色附件共享
RenderAttachmentLoad 值；作者仍只持有稳定 ID/版本。首阶段限定 D32Float、2D、单
mip/layer、X1，尺寸须匹配颜色目标和 View。clear_depth 必须有限且属于 [0,1]。
深度目标要求外部写权限，Load 还需外部读权限；版本、实例所有者、句柄与别名沿用
同一绑定检查。颜色清屏、复制、导出仍只接受颜色格式，局部/history 深度另行扩展。

Renderer 复用同一 Opaque draw packet、原生资源导入与 indexed draw 录制。引擎按
颜色/采样/深度附件合同唯一选择材质 PSO；主 PSO 和可选深度附件变体由宿主持有。
Runtime 复用既有 Renderer.SceneDepth 槽和材质 Shader/layout，保留颜色 PSO，并创建
颜色+D32 深度测试/写入变体；其退休继续使用原宿主屏障。空场景也遵守附件 Load/Clear。
这不增加作者 Pass 注册、独立深度回调或第二份绘制实现，也不提供任意 Shader/PSO 选择。

验收使用既有 UV/depth 原生夹具转换为 Feature 执行入口，真实回读证明近物遮挡和
交换深度后的正确赢家，以及 Load 保留已有深度、Clear 覆盖旧深度。此阶段的验证
结果记录于执行 ledger；Task5 的深度单独绘制、Shadow/GBuffer/PostProcess 等剩余
profile 和旧作者注册删除继续保留。

本切片全量 Debug 编译通过，完整 CTest 164/164 通过。实际深度变体、遮挡/深度写入、
Load/Clear、24 类非法输入回滚，以及材质变体热更新不重复创建资源均已验证。
独立审查无重要问题；当时缺少空场景指定深度值的直接回读断言，12.15 已以同一空场景
颜色+深度 Clear 后接独立深度 Load 的原生回读覆盖该路径。

### 12.15 独立场景深度绘制

RenderDrawDepthScene 显式指定场景和 RenderDepthAttachment，不要求颜色目标；用于同一
Frame 的 Z-prepass。复用 kDrawScene 权限、外部写权限及 Load 的外部读权限。外部 D32Float、
2D/X1/单 mip/layer、View 尺寸及绑定所有权/版本/唯一性限制与 12.14 一致。
深度目标不可作为颜色 final_output；可选 depth_output 明确导出外部 D32 目标，沿用
ExportOutput/外部写权限，确保独立深度工作成为图根。宿主消费或后续 Draw 的 Load
也可形成实际依赖；省略输出不把可裁剪工作强制标成副作用。

Renderer 复用既有 depth-only draw packet、网格/对象导入和 indexed draw 录制；中立入口
直接创建内部 WorkItem，不依赖作者 PassRegistry，显式控制 Clear/Load。材质 depth_pipeline
由宿主提供，depth_pipeline_contract 使用既有附件合同描述无颜色、X1、D32 ABI。
Runtime 沿用现有深度 Shader/PSO、SceneDepth 和场景提取边界；不增加另一份 GPU 缓存。
空场景也录制附件操作，来源与 owning 场景快照随 GPU 帧退休。

验收转换既有原生 depth fixture，回读近物/远物的实际深度、Load 保留、Clear 覆盖与空
场景指定值；错误绑定/PSO 在同一构图事务中回滚。Shadow/GBuffer/Lighting/PostProcess
及局部/history 深度仍是后续范围。本节验证状态记录于实施 ledger。

本切片全量 Debug 编译通过，相关回归 27/27、完整 CTest 165/165 通过。原生夹具覆盖
深度图根、近远遮挡、Load/Clear、空场景指定值、38 类非法输入拒绝及整图事务回滚。
验证期间发现并修正了 Win32 最小化状态/事件把零尺寸误报为 1×1 的既有适配问题，
平台最小化/恢复与四种 owned-host 启动测试各连续通过三次。
独立审查保留一项低优先级测试增强：浮点深度回读比较尚未显式拒绝 NaN；不视为已验证该断言。

### 12.16 方向光阴影深度生成

RenderDrawDepthScene 的 RenderDepthSceneProjection 明确选择相机深度或方向光阴影。
Feature 作者仍声明场景、外部 D32 附件及 Clear/Load；权限和资源事务沿用 12.15。
引擎通过既有 make_directional_shadow_view 计算光空间视图，并使用相机剔除前的
shadow_casters（未提供时沿用 instances），复用深度录制和宿主 PSO。
无有效投影光或无 caster 时，只执行声明的附件操作，不回退为相机几何绘制。
对象常量使用实际传入的视图；相机与阴影视图在同帧保有独立对象绑定。

Owned Runtime 按意图需求绑定独立的 Renderer.ShadowDepth 外部槽，复用现有深度
分配/绑定函数，避免覆盖 Renderer.SceneDepth；当前阴影尺寸仍匹配场景 View。
此切片仅完成深度 producer；阴影采样、光照合成、独立分辨率及其余 Renderer profile
继续保留为后续范围。实施和验证状态记录于 ledger。

本切片全量 Debug 编译通过，相关回归 30/30、完整 CTest 166/166 通过。原生深度
夹具共 16 次执行、45 类非法输入拒绝；新增九个阴影场景验证倾斜方向光深度、
相机视野外 caster、无光/关闭/空阴影的 Clear/Load，以及生产 Runtime 对象常量的
同帧相机/阴影视图隔离。新增阴影回读显式拒绝非有限值；12.15 的旧相机 NaN
断言增强仍延后。独立审查无新增严重/重要问题，本轮未提交。

### 12.17 方向光阴影采样与场景光照

RenderDrawScene 可选声明 directional_shadow 外部纹理输入，同一场景快照的方向光
深度 producer 在前、消费者在后；实际 sampled-read 建立图依赖，颜色输出锚定工作。
作者只提供稳定 ID/版本及外部读权限，不接触阴影描述符、Shader、PSO 或 Pass。
采样输入沿用外部 D32、2D/X1/单 mip/layer/场景 View 尺寸限制，另需 Sampled 能力；
不能与颜色或相机深度附件别名。实际原生绑定和形状在同一构图事务中验证。

Renderer 复用现有方向光拟合、ShadowResources 帧常量/描述符和 Opaque 的 group 3
采样录制，直接展开内部工作。宿主发布 shadow_pipeline 的实际附件合同，选择时
验证颜色/采样/深度一致；Runtime 复用既有颜色+D32 阴影 PSO 和帧资源服务。
无有效投影光时采用现有无阴影场景光照路径。直接光受阴影衰减，环境光保留。
不新建 Shader/PSO/cache，不调用会创建另一份阴影图的完整 Forward 投影。
独立分辨率、多光源/级联、Deferred GBuffer 输入及任意 Shader/PSO 仍待后续实现。
本节已接入中立消费入口、共享资源准备和宿主合同。两个消费者及同意图重复 Draw
通过真实像素回读，覆盖方向光阴影、视野外 caster、关闭/空场景和环境光；26 类非法
输入验证整图回滚、来源与帧退休。共享原生缓冲导入现在共用图身份与依赖/状态历史，
相容性按声明初态校验，不重置执行后的状态；跨队列整数回读已有回归。
本切片全量 Debug 编译、相关 49/49、完整 CTest 169/169 通过，独立审查无新增问题。
宿主专项为空场景，打包后的非空阴影组合链路尚未端到端验证；不据此宣布完整
Renderer profile 迁移完成。实际验证命令与限制以 ledger 为准。

### 12.18 GBuffer 中立 producer

RenderDrawGBufferScene 只声明场景和 albedo、normal、material 三个命名颜色附件及
depth；附件通过稳定 ID/版本引用宿主资源，各自明确 Load/Clear。所有目标必须是外部
引用，Draw/Write 权限必需，每个 Load 另外需要 Read；颜色清除值有限，深度值有限且
在 [0,1]。四个附件不能有逻辑或原生别名。颜色附件验证与现有场景绘制共用。

当前固定 ABI 是按语义顺序排列的 RGBA16Float、RGBA16Float、RGBA8Unorm 及
D32Float，全部为 View 尺寸、2D/X1/单 mip/layer。浮点绑定仅对 GBuffer 对应角色开放；
Clear/Copy/final_output 的原有颜色约束不放宽。producer 不强制成为图根，由后续
实际 attachment Load、sampled-read 或读回消费者建立依赖。局部/history GBuffer 未接入。

Renderer 复用已有 GBuffer PSO、网格/材质/对象导入和 indexed draw 录制；中立入口
直接展开内部工作，上传和绘制保留 Feature 来源与 owning 场景租约。每个附件的
Load/Clear 对空场景也生效。宿主 PSO 合同复用 RenderMaterialAttachmentContract，
有序追加附件 1/2 的格式，未使用项为 Unknown；GBuffer 实际合同随 live 句柄一起刷新。
宿主需求扫描复用同一个 transient 附件分配/绑定函数，不建立第二份资源缓存。

原生 MRT/深度内容、近物遮挡、空 Clear/Load、非法输入回滚和帧退休已通过专项验证，
本切片全量 Debug 编译、完整 CTest171/171 通过，独立审查无新增问题。完整 Deferred Lighting/PostProcess 消费、任意 Shader/PSO
及 temporal/旧作者层删除仍是原计划未完成项，不能据此宣布完整 Renderer profile 完成。

### 12.19 DeferredLighting 中立 consumer

RenderDeferredLightingScene 显式读取 albedo、normal、depth 三个外部输入，向 hdr
颜色附件写入，并可选读取 directional_shadow。所有资源仍由稳定 ID/版本引用，
作者只编写 Feature。当前 shader 未读取 material，不能为其制造 sampled-read 依赖。
固定输入 ABI 为 RGBA16Float、RGBA16Float、D32Float；HDR 为 RGBA16Float，均为
View 尺寸、2D/X1/单 mip/layer。采样输入需要 Read 权限和 Sampled 能力，输出需要
Write 权限及 ColorAttachment 能力；HDR Load 另外需要 Read。所有输入/输出不得别名。
HDR 仅在此显式角色开放，不放宽 LDR Clear/Copy/final_output 的合同。

Renderer 从现有 Deferred profile 提取并共用同一个全屏录制器、资源服务和 shader/PSO，
中立入口直接展开内部工作，不调用会再次生成 GBuffer/阴影/后处理的完整 profile。
宿主随真实 base/shadow PSO 句柄发布实际附件合同；所有绑定/形状/合同验证纳入构图
事务，来源和 owning 场景租约延续到 GPU 帧退休。全屏 consumer 即使场景 instances
为空也执行，因为几何内容由 GBuffer producer 提供，consumer 场景只提供视图/光照。
Load/Clear 是附件装载语义；全屏 shader 仍覆盖像素，背景 depth>=1 输出透明黑。

现有光照常量缓存以帧编号、View ID 及完整已验证常量为键，上传收据使用同一身份，
同帧不同场景/光照不重写已有 GPU 常量；同一身份重用已有资源，并在未提交上传被
清空时重录。延续宿主显式退休策略，不在此切片新增第二份缓存或宣称有界逐帧回收。
当前 shader 支持环境光与一盏方向光及可选方向光阴影；局部/多光源、PostProcess、
temporal、完整 Renderer profile/Editor 迁移及旧作者层删除继续作为原计划未完成项。
原生专项已验证真实 HDR、同帧双消费者/重复 Draw、空 consumer、有/无阴影、环境光、
背向光与空 GBuffer；101 类非法输入回滚、实际执行失败/帧退休与失效回调也通过。
CPU owner 原子提交、SDK 与 owned-host 按需 HDR 绑定通过专项。owned-host 是空场景且
HDR 不作 final_output，该测试只证明构图/合同/绑定，不证明打包非空 HDR 渲染。
本切片全量 Debug 编译及完整 CTest173/173 通过，独立审查无 Critical/Important；
一项阴影 consumer 前提的文档 Minor 按 Native 规则延期，详见 ledger。此验证仅关闭
上述固定合同切片，完整 Tasks5/6/7 及前述未完成项继续实施。

### 12.20 PostProcess 中立 consumer 与非空打包链路

RenderPostProcessScene 读取稳定 ID/版本的外部 hdr、depth，向 output 颜色附件写入。
HDR/depth 固定 RGBA16Float/D32Float，output 沿用 LDR 格式合同，全部为 View 尺寸、
2D/X1/单 mip/layer。输入需要 ReadExternal/Sampled，输出需要 WriteExternal/
ColorAttachment，Load 另外需要 ReadExternal；三资源逻辑 ID 及原生绑定不得别名。
全屏操作不要求场景可见实例；场景提供 camera、extent 和默认环境。可选 settings
仅覆盖此操作的一份临时 View，不改变不可变场景快照。Feature 所有的设置类型只含
RGB 数组、exposure/gamma、天空/色调映射开关；颜色与曝光有限非负，gamma 正有限且
倒数有限。设置和校验从 Renderer 移到 Feature，当前项目调用方同步迁移，无重复类型
或旧命名空间兼容别名。

Renderer 复用唯一 PostProcess recorder、既有 shader/PSO 和 PostProcessResources。
真实输出附件合同从 PSO 创建描述发布。各次调用沿用帧退休常量缓冲与新描述符；
通过已存在的资源命名/import 函数和 Graph 原生去重按实际缓冲身份导入，不另建缓存。
读取范围为 shader 实际使用的208字节。中立工作没有强制副作用，final_output、
真实消费者或读回决定存活；Load/Clear 只表示装载合同，全屏覆盖像素不承诺保留旧值。
所有校验进入现有整图事务，Feature/Node/调用/操作索引/Frame 来源与 owning 场景和
意图租约延续到 GPU 帧退休。

专项已验证独立 Feature 数据边 GBuffer→Lighting→PostProcess、同帧两个参数消费者、
重复 Load 操作、默认环境/曝光/Gamma/天空开关、阴影/环境光/空 GBuffer 到最终 LDR。
实际常量导入去重/实例隔离、非法输入回滚、执行失败后新帧重建和失效回调覆盖其中。
owned-host 专项真实打包 mesh/material/texture、UUID 重定位、Scene 和 FeatureGraph，
从非空几何运行 GBuffer→HDR→PostProcess→final_output；中心绿色和红色天空分别
读取实际最终像素，且不存在独立绿色清屏。作者执行函数只返回 Feature 意图。
本切片全量 Debug 编译、相关62/62和完整 CTest175/175（304.13s）通过，直接回调/
帧时钟通过；独立审查无 Critical/Important/新增 Minor。审查后的26文件与测试后
冻结快照一致，证明与细节见 ledger。此前两项 Minor 按 Native 规则继续延期。

此处仅补齐当前固定 ABI 后处理及非空打包链，完整 profiles/clustered/局部与多光源、
任意 Shader/PSO、temporal、跨 Clock readiness/deadline、Audio/动态 ABI、Editor 主
入口和旧作者层删除仍是原计划未完成项。Task5/6/7 不据此关闭。

### 12.21 独立光列表编排的可信输入前置合同

LightCulling 与 clustered Forward 必须是可独立编排的 Feature。跨实例光列表引用必须
同时证明生产者已提交该声明、消费者实际消费了匹配的同 Clock DataEdge；仅匹配作者
填写的 id/version 或 owner 数字不足以授权访问。

Runtime 在消息提交时记录真实来源实例、完整 InvocationToken、ContractDigest、资源
epoch 和生产者 Clock，覆盖作者填写的来源字段；Router 分配实际 delivery_id 和路由
目标。Invocation 和 retry 保存完整已消费消息，替代仅保存 delivery_id 的旧记录，
以免 Snapshot 替换后丢失原来源和 payload。成功提交的 RenderIntent 带上这些不可由
StateDelta 提供的消费证据。显式调用输入不消费图队列，也不取得图消息的授权；失败
提交保留旧意图及其证据。捕获值拥有消息数据，不依赖原 Runtime 的存续。

后续 RenderGraph 每次 build 使用局部登记表和 engine-only typed owning bundle，
producer 只准备一次 constants/lights/clusters 及回调，多个消费者共享同一个 bundle。
引用校验包含实际生产者声明、消费 payload、owner/digest/epoch/invocation、Scene
snapshot、camera depth、frame 和 build identity；回滚后的旧句柄不能借数字复用取得
新 bundle。不能把组合操作等同于独立 producer/consumer 已完成。

GPU 列表大小除 CPU uint64 溢出检查，还必须满足 shader 的 uint32 字节地址范围。
当前 for_frame 已在上传分配前拒绝超过 2^32 字节的列表；RHI 已公开 Vulkan 实际
maxStorageBufferRange，列表和灯光输入还须在分配前满足该限制（见 §12.22）。可选
阴影将使用同一光照源码的真实无阴影入口和匹配 PSO/
descriptor 布局，计算阶段不制造阴影纹理读取。上述登记表、降低路径和
shader 变体仍在后续实施范围；本节只记录已补齐的可信消息与寻址前置条件。

本轮 Debug 全量编译通过，相关58/58、完整 CTest175/175（302.88s）及直接回调/
帧时钟通过；测试后13文件冻结快照一致。Native 全分支最终审查仍待原计划整体完成。
异步执行可能暂时出现新 producer 意图与旧 consumer 意图并存，后续必须按同 Clock
readiness 和完整波次/last-good 发布策略捕获引用一致的输出，不能用第二缓存掩盖。

### 12.22 独立光列表的逻辑执行计划与计算基础设施

Feature 提供独立的 `RenderLightCullingScene` 和 `RenderDrawClusteredScene` 意图，以及
`Render.LightList.v1` 端口值。引用由 id、version、producer invocation 和 generation
组成；沿用已有二进制读写器编码为四个 little-endian uint64，拒绝非法零值、截断和
尾随数据。作者不填写原生缓冲区、描述符或可信 owner；SDK 不暴露 RHI、Pipeline、
Pass、Serialization 或内部 Concurrency。

Owner commit 验证独立 Cull/ReadLightList 权限、场景和输入版本、声明端口/schema、
同实例重复列表 id（包括不同 version）、附件别名与深度 Load。失败意图保留原状态、
原已接受输出及其来源，仍可用同一个有效 token 提交正确结果。图预检再以真实消费
的 DataEdge payload 与完整 MessageSource 选定实际生产者，拒绝伪造、缺失、旧代次、
跨 Clock 和不匹配场景/深度/阴影。同名甚至完全相同的公开引用数值仍由消息来源区分。

RenderGraph 根据所有同 Clock DataEdge 和列表依赖进行稳定拓扑排序，在任何 GPU
降低前拒绝环；重新排序仍保留原始 output/operation 到 WorkItem 的归属。原输入顺序
的负面对照会使消费者先读取未生产的目标，真实 GPU 回归验证生产者先执行并得到
正确像素。这里的逻辑 producer binding 尚不是 GPU bundle。

现有 LightCulling 录制代码共用一个 setup/execute 主体。无 PassRegistry 的显式上下文
生成内部 Compute pass；上传资源复用既有 native identity/import helpers，可在同一图
容纳多套列表，声明准确的 constants（176B）、lights 和 clusters 范围。是否使用独立
Compute queue 遵循引擎内部 builder 权限；没有该权限时在 Graphics queue 执行计算，
不新增作者队列开关。现有 for_frame 仍是唯一灯光打包、分配、描述符及退休入口。

RHI `max_storage_buffer_bytes` 来自设备实际 maxStorageBufferRange，零表示不可用。
for_frame 在 retention/upload 前检查 clusters 与 lights 范围；合法的 16384²/1023 灯光
会生成可寻址的 2^32B 列表，但超过本机设备描述符范围，因此被明确拒绝。GPU 测试
还验证了两套 Point/Spot/空列表、空间与深度排除和帧结束后回调拒绝。

本节完成逻辑校验、执行顺序和共享计算基础设施。图内 typed owning bundle、camera
depth/snapshot 证明、build identity/回滚保护、独立 Feature 间的原生 bundle 连接
及异步完整波次发布仍须实现。集群 PSO 与共享 recorder 的后续进展见 §12.23。新意图当前在 native
降低处明确返回 NotReady；不能把 API 声明或底层计算测试计为独立 Feature GPU 编排完成。

验证：Debug ALL_BUILD 通过；相关 59/59、完整 CTest 175/175（355.63s）及直接回调/
帧时钟通过。完整回归后 13 个源码/测试文件的冻结 SHA256 均一致；本轮没有提交。

### 12.23 原生集群管线与无阴影输入

Renderer 的 `create_clustered_forward_pipelines` 复用宿主现有 ShaderManager/layout cache，
统一为 Runtime 和 Editor 创建有阴影、无阴影两个集群 PSO；附件契约来自各 PSO 实际
创建参数（当前基线为 HDR RGBA16Float、X1、D32），不能借用普通 RGBA8 PSO 的契约。
Runtime 材质解析在已有材质缓存上更新、移除、恢复这些元数据；不复制材质资源或缓存。
Opaque recorder 先选择实际集群变体，再校验该变体的附件契约。

两个 fragment 入口共用一份光照计算。无阴影入口的真实编译反射没有 group3/b0
阴影纹理，其余资源绑定保持一致。既有 for_frame 是唯一光源打包、分配与退休入口；
其不可变 shadow mode 决定 forward layout 和三/四项 descriptor 写入。
Compute 必须接收相机深度；无阴影 Forward 不接收阴影纹理；有阴影 Forward 必须
接收阴影纹理。错误模式、帧结束后的回调、缺失或不匹配的 PSO 都明确拒绝。

共享 native recorder 的 Vulkan 测试以一份列表驱动两个 HDR consumer，回读真实
directional、Point、Spot、无贡献、空列表和空几何六组像素。阴影 sentinel producer
被裁剪；空几何的 Opaque 工作仅保留目标附件依赖，其未消费的光照计算被裁剪。
缺失 PSO、颜色/采样数/深度/MRT 契约错误
在已有 producer 与第一个 consumer 后拒绝，并恢复图的 pass/resource/cull counts。

图编译器的存活判定只沿内容 writer 与显式依赖回溯；WAR 和只读状态排序约束保留
工作之间的顺序，不使未消费的 reader 存活。保留的工作重新计算资源 hazard 与屏障，
避免死 reader 留下虚假的 Compute queue transfer。WAW 仍保守保留已有 writer。
独立 imported-buffer GPU 测试同时验证读者裁剪、显式依赖/副作用保留与真实回读。

本节证明引擎内部原生基础设施；测试 adapter 不代表作者 Feature GPU 编排。
独立作者意图的 typed owning bundle、深度/snapshot 与构建回滚保护随后接入，见 §12.24。
完整波次发布以及其余五域迁移要求继续实施。

验证：最终 Debug ALL_BUILD 通过；相关 52/52、完整 CTest 177/177（349.13s）
及直接回调/帧时钟测试通过。完整回归后 22 个源码/测试/构建文件的冻结 SHA256
保持一致；本段无提交，整项迁移未宣布完成。

### 12.24 独立灯光列表的原生连接与构建来源（2026-10-08）

`RenderLightCullingScene` 与 `RenderDrawClusteredScene` 已连接到 Renderer 的既有
for_frame、Compute 和 Opaque recorder。作者仍只提交逻辑场景、附件和 LightList
引用；资源包、RHI、回调与构建身份均为引擎内部类型。没有新的上传器、资源缓存或录制器。

RenderGraph 按已验证的执行计划排序，以**原始 output/operation 索引**保存强类型
LightList lease。Renderer 的私有具体包持有一份 canonical snapshot、完整
RenderLightCullingResource、clusters 和相机/方向阴影深度来源。一个生产者的所有
消费者共享该包，不重新解析或分配光照资源。graph 的既有 execution owner 与
FrameContext 的既有延迟退休管理执行期寿命；不从 shared<void> 取回或转换资源包。

每次构建尝试创建独立且强持有的 identity，同时校验 FrameContext、Device、帧号、
graph id、完整 producer invocation/owner 以及原始生产索引。回滚后数值资源 handle
即使复用，仍不能重新接纳失败尝试留下的 lease。

深度来源按实际图纹理维护，只认可本次构建成功降级返回的 canonical snapshot：

| 当前写入 | 供后续 Cull/Clustered 使用的证明 |
| --- | --- |
| Camera Depth Clear / GBuffer Depth Clear | 建立该场景快照的相机投影证明，空几何同样有效 |
| Camera/GBuffer Depth Load | 仅保持已存在且场景、引用、纹理、投影、snapshot 身份均匹配的证明 |
| DirectionalShadow Depth | 建立或按相同 Load 规则保持方向阴影证明，不能充当相机深度 |
| 普通 DrawScene 深度及其他未被明确跟踪的写入 | 使对应物理纹理的证明失效 |
| Clustered Depth Load | 必须与生产者和当前最新相机证明一致；成功消费保持该证明 |

格式正确的宿主 D32 纹理、同值的 snapshot 副本、不同 owner 映射出的另一纹理、
第一次 Load，以及被后续阴影写入覆盖的相机深度，均不能替代生产证明。可选阴影
引用也要求同一 snapshot 的方向阴影生产；实际 fit disabled 时仍选无阴影 shader/layout，
不会给 Forward 增加阴影纹理访问。

for_frame 唯一签发私有 origin，严格连接校验其 FrameContext、Device、实际帧号、
snapshot 指针及 native handles/描述/尺寸/mode。既有直接 primitive 调用保持自己的
scene 使用方式；严格 Feature seam 要求 scene.frame_number 为当前帧。callback 是
可信引擎 resolver 的忠实转发边界：origin 校验不声称能够认证任意 std::function 的函数体。

独立 Feature GPU 回归使用真实 Runtime 的一个 producer 与两个跨 owner consumer、
同钟 DataEdge 和 canonical 32-byte payload；反转捕获次序后仍选中原始生产槽。
六组 HDR 像素复用 §12.23 的 directional/Point/Spot/无贡献/无灯/空几何 oracle，
非空几何只有一次 Compute descriptor preparation，两个消费者使用新且不同的
Forward descriptor。另有 Clear/Load/GBuffer/disabled-shadow 构建控制、深度与
snapshot/native-origin 拒绝，以及同一 active frame/graph 回滚后的旧 lease 注入。
数值 handle 复用由测试 witness buffer 实证，不声称观察到了私有 clusters handle。
此回归尚未覆盖独立 Clustered Feature 的 enabled-shadow GPU 像素链。

新连接还暴露并修复 HostWrite 首次使用的队列所有权问题：未被 GPU 使用过的上传
缓冲区不会因 initial_queue 提示生成源队列 release；已使用后部分 CPU 重写仍保持
原 GPU owner，并用合法 GPU scope 完成跨家族 release。Host stage 不能与跨家族
转移组合；提交前已 flush 的 HostWrite 由 queue submission 提供设备可用性。
依据 [Vulkan 同步规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)。
现有 imported-buffer GPU 回归覆盖 Graphics/Copy/Compute 首次使用、部分重写、
真实回读和首次使用无虚假完成点；未增加第二份同步实现。

原生连接三项回归通过，最终 Debug ALL_BUILD 成功；完整 CTest 178/178 通过
（334.38s），Runtime 回调与帧时钟直接专项均通过。验证后 11 个冻结源码/测试/构建
文件哈希全部一致，diff-check 通过。没有中途提交或阶段审查。
当时完整异步波次发布、Editor 主入口、旧层次删除、其余五域迁移和最终整分支审查仍待完成；波次与 Owned 接入的后续进展见 §12.25–12.26。


### 12.25 同 Clock 的完整 CPU 渲染批次（2026-10-08）

纯逻辑意图计划器归属 Feature 目录和命名空间；RenderGraph 投影器委托同一个实现。
FeatureRuntime 每个 Clock 只保留一份已接受的不可变 CPU 意图批次。第一次完整批次
尚未就绪时 capture 成功返回空；有完整批次后，异步、DataEdge 等待和重试退避期间
继续返回整份旧批次。调度循环、owner completion、deferred continuation 均完成后
验证候选批次，并在启动下一条排队波次之前整体发布；无需等待 Clock 出现空闲帧。
显式 Call 也进入同一就绪判定，不能逐个 owner 替换已经发布的批次。

若 consumer 仍引用上次实际接受的 producer，计划器验证完整消息来源及 scene、
depth、shadow 等身份后报告 NotReady，并保留完整旧批次。错误的端口 payload、
来源、版本、场景或循环仍是实际错误，不以 NotReady 掩盖。LightList 导出必须由
同一次 Invocation 声明的 culling operation 产生，验证在 StateDelta/ResourceBus
提交前完成；无声明或挪用另一 Invocation 的引用不能推进状态或发出消息。
物理 lowering 只接收已经接受的完整批次，每个当前帧重新生成 native owning bundle，
不缓存或重放上一帧 GPU 图。

接受的 owner 关联 definition digest、resource epoch、Runtime generation 和提交后
Process revision。suspend/resume/cancel、定义替换或图重发导致身份退休时，整份
受影响批次退休；另一个 owner 完成不能使旧 Process 意图复活。Composite 发布前
使用展开后的叶子计划收集 owner，仍保留作者根节点和独立 leaf instance；复用
同一个 owner 捕获函数，不增加另一份 Composite 遍历逻辑。

确定性三叶子 CPU 测试覆盖首批空、逐步完成、排队下一波前发布、无 worker 的重试
退避、不同 attempt 的成功恢复、idle consumer 保留旧意图、显式导出事务和新 generation。
Process 生命周期及 Composite 叶子回归与六组当前 scene 下的旧 CPU 批次 GPU
重建、随后完整 B 波次切换共同验证。专项已通过；最终 Debug ALL_BUILD、Runtime 回调/帧时钟直接测试以及完整 CTest 180/180（361.55s）通过，15 个冻结源码哈希一致。
这仍不是全部五域迁移或完整 profiles、跨 Clock readiness/deadline 的完成结论。 Process
生命周期专项当时使用两个独立的空意图叶子；其结果不外推到跨 owner LightList。
后续真实 producer 退休的 CPU 与独立 GPU 证据见 §12.29，Owned 退休组合仍待验证。

### 12.26 Owned Runtime 的独立 Clustered 打包帧（2026-10-08）

Owned Runtime 复用现有需求扫描，为 LightCulling/Clustered 请求真实 scene，按
规范 external identity 为 Clustered HDR 和可选 directional shadow 请求已有宿主
附件。已有 bind_owned_render_target、asset cache、Renderer culling/forward/PP
lowerers 继续承担分配、解析和记录；不新增 fallback 调度器、GPU 缓存或 allocator。
HDR 仍不是 RGBA8 final_output，作者最终输出通过 PP 到 SceneColor。

真实 cooked fixture 复用已有 mesh/material/texture/scene dependency closure，并
通过反射序列化 PointLight。producer 返回 camera depth/cull 和 canonical LightList；
同 Clock 两个 consumer 从真实 DataEdge 解码，最后一个以 HDR Load 消费前一个的
颜色生产，再 PP 到 swapchain。第一份完整批次没有 PP，单独验证 Clustered 的 HDR
需求解析，允许其无外部消费的 GPU 绘制被正常裁剪；后续像素证据只来自 live 的
三个 Runtime owner 和真实非空 native 图，不把裁剪的第一批次当作 GPU 计算证据。

测试 clock 固定时间，零 delta 帧仍 pump。引擎测试内部 Concurrency gate 阻塞末位
consumer：首批 pending 的实际读回为默认黑帧；完整 A 建立后，B 的 producer 与快
consumer 完成仍连续两帧读回 A，释放后全体 work token 与像素一起切换 B。
中心 2×2 覆盖两个 cluster tile，校验真实局部光的非饱和结果；四角同时校验 HDR
生产者 clear 与 PP exposure。oracle 从场景/灯光/后处理公式计算，并按真实交换链
UNORM/sRGB 格式编码，逐通道容差 2。不向 GameSDK 暴露 gate、Runtime 或 Concurrency。

该路径发现缓存 mesh 重导入的状态缺口：上传已完成、绘制可能被裁剪，GPU buffer
仍处于 CopyDestination/Copy；声明的 VertexInput/Graphics 不能替代实际状态。
RenderGraph 编译器在已有 imported-state 刷新入口同时读取 buffer 全部分段，变化时
废弃旧屏障计划；compile/execute 共用一份完整覆盖校验，buffer 的重复 import
声明契约保持独立。双图回归先缓存无转换 reader，再由外部 Copy producer 只写半个
buffer；不修改 reader 声明，仅重新 compile 即获得真实 range/usage/queue 转换并
读回两字，保留未写半段。已有首次/部分 HostWrite 的多队列回归同时保留。

Owned 与相关专项 15/15（11.55s）、最终 Debug ALL_BUILD 和完整 CTest 180/180（361.55s）通过；Runtime 回调/帧时钟直接测试通过，冻结源码一致，diff-check 为 0。
Editor 主入口、enabled-shadow 独立 Clustered GPU 链、旧公共层删除和其余原始范围
继续实施，不据此勾选 Task5 全部完成或提前执行整分支审查。

### 12.27 Module 作者注册上下文仅注册 Feature（2026-10-08）

直接收窄现有 ModuleRegistry 与 ModuleRegistrationContext：删除旧 phase、pipeline、
pipeline feature、render pass、bind feature 注册族和裸 Pipeline/Pass registry getter，
同时删除对应 ownership 容器与工厂逻辑。构造函数必须接收 FeatureRegistry，可选反射
服务沿用原接口；不存在旧构造重载或兼容分支。五处构造方均改用同一个 Feature
registry，Runtime 的其他待迁移 Frame/Renderer 内部服务不由 Module 作者上下文提供。

FeatureDefinition 的 module owner 检查、模块依赖和 register/unregister 生命周期继续
复用已有实现。启动失败回滚及正常卸载共享唯一 Feature/反射清理实现，保留 Composite
依赖的重试删除顺序。删除旧模块测试中的 Pipeline/LogicFeature 作者类，替换为真实
FeatureRuntime callback/commit、所有权、依赖阻止卸载和成功卸载验证；失败模块先
成功注册自己的 Feature 与反射类型，再故意失败，两个注册都必须撤销。公共边界测试
先在旧实现上实际失败，再验证旧方法及构造方式不可用、新 Feature 构造方式可用。

专项 14/14（7.02s）、最终 Debug ALL_BUILD、Runtime 回调/帧时钟直接测试及完整 CTest 180/180（363.06s）通过；21 个源码与测试冻结哈希一致，diff-check0。这只关闭 Task7 的 Module
注册上下文子闭包。Module bundle 的旧 catalog、动态 DLL 执行式 Feature ABI、Editor
主入口和其余旧模型删除继续实施，不据此宣布游戏作者公共面或 Tasks5/6/7 全部完成。


### 12.28 Feature 元数据与类型化编辑器草稿（2026-10-08）

ModuleMetadata 删除旧 TypeCatalog、catalog 字段、descriptor 别名及 Module 的公开 Pipeline
依赖。正式 schema/ABI/SDK major 为 2；旧 catalog、minimal header 和其他 schema 拒绝。
Feature 的纯语义 JSON codec 从 Module 移至 Feature，通用 ID/版本/ParameterValue helper
在 Serialization private 复用。Module 只持有描述符、模块容器、owner 校验和摘要。

NodeDefinitionDraft 直接持有 FeatureDefinition，展示标签和 retired IDs 位于编辑器 envelope。
草稿和正式 validator 共用实现，仅草稿允许 Composite 暂缺 children；Generate 保持严格。
StatePayload、类型化参数、时钟、权限、副作用和 Composite 映射完整保存并进入生成的 C++
定义。定义面板复用原生参数控件和同一服务，真实 compiled ID 决定只读；不另建 DTO 或 codec。
旧 Frame 面板的 Pipeline/Pass 作者创建、拖放创建和定义编辑对话框删除。

Save Draft 不发布目录或图。Generate 先准备每份受影响图和目录，再执行已有生成文件事务；
同一个事务负责草稿 sidecar 退休。实际文档身份/edge index 决定断边，不把别的定义的同值
PortId 当作受影响边。未打开的引用图阻止破坏性更新；图与目录共同进入 Undo/Redo。
模块 bundle 和 DynamicModuleLoader 删除 metadata-only/无 DLL 兼容路径，加载时重验正式
manifest。动态生命周期测试使用真实 DLL 证明握手、租约和关闭，动态 Feature callback
执行仍待实现。另一个只链接 GameSDK 的目标实际编译六组生成实现/注册文件，并核对完整
契约和 Unsupported stub；它不替代 Editor→Cook→Runtime→Render 全链 E2E。

专项12/12（0.31s）、最终 Debug ALL_BUILD、Runtime 回调/帧时钟直接测试及完整 CTest
182/182（313.54s）通过；53 个源码/测试冻结哈希一致，diff-check0，无中途提交或阶段审查。
Task5/6/7 整体、Main Editor Apply/Inspect、DemoProject 旧示例及其他原始范围继续推进。


### 12.29 LightList Process 生产者退役与独立消费者提交（2026-10-08）

同一 Clock 的真实 P→C1/C2 DataEdge 已接受完整 A，或者 P 已提交尚未消费的 B 时，
suspend/cancel/定义替换会改变 P 的有效生命周期。消费者自己的提交和已经路由的
真实 delivery 保留；省略 render_intent 也保留此前意图与已消费 receipt。不能因此
发布没有当前生产者的 Clustered 图，也不能把这种合法等待一律判为伪造来源。

在现有 Runtime owner 提交事务中，typed export 通过唯一的 declaration validator 后，
PortMessage 携带一次分配、跨 delivery 共享的不可变 CPU source declaration。它记录
真实 owner/node/contract/token/digest/epoch、匹配的 cull 操作和提交后的 Process revision；
Runtime 覆盖 callback 提供的 evidence。源声明没有嵌套 input_messages、回调或 GPU 句柄，
不形成 receipt 链，也不进入资产/消息 payload 序列化。它按现有队列和提交输出的
所有权释放，不另设 bus、缓存、allocator 或清理线程。

Runtime 将 source 的 generation/digest/epoch/Process revision 与 live owner 对照，
仅在私有 planner 入口提供已证实退役的声明。规划器仍共用唯一的 DataEdge/delivery/
port/schema/token/payload 和 scene/depth/shadow 校验；全部合法但 source 退役返回
NotReady，capture 成功空，不产生伪依赖索引或物理 bundle。公开 planner 对缺失或
未经授权的 producer 继续严格拒绝，即使输入含有真实 opaque receipt。伪造 reference、
scene/depth/shadow 不匹配仍为 InvalidArgument，不由生命周期吞掉。

CPU 原案例先实际 RED（退役后的 fresh tick 报 missing/stale source），修复后验证
suspend/cancel、未消费 B、定义替换、恢复后当前 receipt、明确空消费者以及六类错误
控制。Concurrency gate 同时覆盖在途 P 的迟到结果仅回收，以及在途 C 的独立提交
仍有效但不能发布 orphan。Process/消费者原有状态提交边界和调度器继续复用。

独立 native GPU 模式复用既有 P/两个 C、场景/材质/资源解析、两路 HDR、Graph 和
FrameContext。实际 A 提交后产生真实 pending B，再退役 P；B delivery 正常消费，
新帧成功空构图并执行，pass/resource 数均为零、不调用 culling resolver。A 的场景和
输出在 GPU drain 前仍持有，之后弱租约过期；suspend 恢复后只接受当前 token 与
真实 receipt，六个场景的两路 HDR 数值通过。cancel 独立 fixture 使用实际非空 A。

同一 cull 通过显式消息分别发送到不同 target port 也只能是一个生产者。该实物
回归先 RED（被误算为多个退役 producer），再在原提交事务以已验证的操作地址
共享同一 source declaration；没有跨事务 cache，不同 delivery ID/目标端口仍保留。

目标端口误判实际 RED→GREEN 后，最终相关专项18/18（8.90s）、Debug ALL_BUILD、
Runtime 回调/帧时钟直接测试及完整 CTest184/184（306.40s）通过。78个冻结源码/测试/
构建文件哈希一致，diff-check0；无中途提交或阶段审查。
本节不宣称 Owned lifecycle 组合通过：Bootstrap 目前没有已有的内部生命周期控制入口，
不得为测试向 GameSDK 暴露 Runtime/Concurrency。enabled-shadow 独立 Clustered、多
producer 同 public ID、Editor 主入口、动态 Feature ABI 和其余原始范围继续实施。

### 12.30 独立 Feature 的方向阴影与 Clustered 消费（2026-10-08）

真实 Runtime 的 P Feature 返回相机深度、DirectionalShadow 深度和 LightCulling；
同一 typed DataEdge 输出由两个独立 C Feature 消费，各自返回 Clustered HDR 意图。
公共 List/schema/receipt、当前 InvocationToken 和 owner 保持唯一来源，捕获顺序反转
后仍由既有意图计划器安排物理工作。全部复用同一 Renderer native allocator、
Compute dispatch、阴影拟合、对象/材质解析和 Opaque recorder，没有新的作者 Pass、
缓存、Shader、PSO 工厂或执行器，也没有向 GameSDK 暴露内部并发。

六组真实 GPU 回读覆盖关闭阴影、中心 caster、相机视野外 caster、再次关闭、空几何
及无灯光。两个独立 HDR 附件的全部 half 值有限、像素一致，并核对暗区、亮区和
背景的解析数值。有效阴影时两份 fresh Forward descriptor set 使用同一真实阴影
View，该 View 与 Compute 的相机深度 View 不同。最终编译图保留两个深度、剔除和
两个消费操作的完整 Feature 来源，核对实际 Compute 读/写阶段与 Fragment 阴影读。
中立 Builder 仍按已有许可选择队列；Compute dispatch 不等同于必须使用专用队列。

测试实际发现剔除后的同投影 Clear 替换未被拒绝：旧校验分别匹配 scene/texture/
projection/snapshot，却未比较内容所属的那次生产。修复复用现有强 build scope 和
build 内局部 contents_revision：每次 Clear 取得新版本，完全匹配的 Load 延续旧版。
Renderer 在既有 camera/shadow bundle 校验中比较保留证明与最新证明的 scope/版号，
不新增全局计数、marker 分配、cache 或 validator。强 scope 防止跨回滚 build 的
数字重复被误认。两个真实 C 回调分别在消费后返回 shadow Load 和 camera Load，
随后另一个 C 的成功 GPU 回读证明合法保留路径仍有效。

有效阴影负例覆盖 camera/shadow 消费间替换、缺失/错误投影/首次 Load、阴影专用
snapshot clone、附件 alias、不同 owner 的 shadow 绑定、六项实际选中 Clustered PSO
错误及保留旧 bundle 的强 build ABA。失败必须恢复原图的 pass/resource/culled 数量。
删除未选中的普通 shadow PSO 和 unshadowed variant 后，有效阴影的实际 GPU 正例
仍成功，不能靠另一种材质路径通过。场景和输出随既有 Graph/FrameContext 持有，
正常 drain 前存活、之后释放；退休 native callback 拒绝使用。

最终 Debug ALL_BUILD、Runtime 回调/帧时钟串行检查、相关专项19/19（11.99s）及
完整 CTest185/185（337.45s）通过；78个冻结源码/测试/构建文件哈希一致，diff-check0。
无中途提交或阶段审查。本节不替代完整波次/Process/Owned 的有效阴影组合、
多生产者同 public List ID、完整渲染能力迁移或 Task5/6/7 其余门槛。

### 12.31 同公共 LightList ID 的独立生产者（2026-10-08）

独立原生夹具直接复用现有 P/C 两个 Feature 定义，发布四个真实实例 PA→CA 和
PB→CB。P 的 List ID/version、场景槽和相机深度逻辑引用完全同名；真实 Invocation、
owner 和 DataEdge delivery 各自独立。宿主用既有 owner-qualified 绑定为每对实例
提供同一个 canonical snapshot 和一份 D32，两个消费者写各自的 HDR。此项不创建
另一套 allocator、resolver、recorder、Shader 或生产 registry。

真实消息必须匹配完整 MessageSource、digest/epoch/clock、端口/schema/version 和
当前 producer token；消费者的引用来自实际 decode 的 payload。先按 PA/PB/CA/CB
输入构图，再在 fresh frame 反转为 CB/CA/PB/PA。每帧最终编译图验证两份相机深度、
两份剔除和两份消费工作都存活，保留实际定义、节点、module、实例、token 和原始
操作索引。两份真实 owning bundle 属于同一强 build scope，却各自匹配自己的
RenderLightListProduction，没有按公共 ID 合并。

同一个 native for_frame/回调观察器按 canonical snapshot 分桶，只忠实转发现有
Compute/Forward 准备函数。每对实例实际 resolve/Compute/Forward 各一次，native
constants/lights 和 fresh descriptor set 分开，每个 Forward 使用自己 Compute 的
clusters。PA 的 red Point 与 PB 的 blue Spot 产生两幅不同 HDR；中心、交叉位置和
背景分别匹配既有解析 oracle，全部 half 值有限、alpha 为 1。Graph 有实际 draw、
queue completion 和 submit，再按正常 FrameContext drain 退休。

编译拓扑与真实 callback 句柄共同判断瞬态存活：第一个排列实际重叠，camera 与
clusters 物理句柄不同；反向排列实际非重叠并复用这两类物理资源，两个正确像素
结果仍保留。逻辑来源隔离不能等同于禁止合法瞬态复用，也不能仅用 textures_alias
推断 transient pool 的物理分配。

负例保留真实 receipt，仅把 CA 引用换成 PB 的 canonical 引用，须在 native 准备前
以 InvalidArgument 拒绝。不同 camera 绑定、CA 的 equal-value snapshot clone、
返回或消费另一个 producer 的真实 bundle、PB 借用 PA 的真实 native origin，以及
强旧 bundle/数字句柄重现均由既有校验拒绝并完整回滚。origin 用例同时证明 PA
准备成功、PB 才失败，不能把一般 stale-frame 错误当作 snapshot 隔离证据。两个
snapshot、两个 bundle 和输出在正常 GPU drain 前存活、之后释放；两份退休 native
callback 都拒绝使用。

初始正例已由现有生产能力通过；clone 负例曾错误转发原始绑定，修正测试注入后
全部控制通过，不把它宣称为生产缺陷修复。最终 Debug ALL_BUILD、Runtime 回调/
帧时钟串行检查、相关专项20/20（11.76s）及完整 CTest186/186（345.25s）通过，
78个冻结源码/测试/构建文件哈希一致、diff-check0。无中途提交或阶段审查。
此节是直接独立原生链的同名来源证明，
不替代有效阴影/Process/Owned 的双生产者组合、Owned lifecycle、完整渲染迁移或
Task6/7 的主入口与全链路门槛。


### 12.32 内部 Host 检查与 Owned Process 控制

FeatureGraphRuntimeHost 是作者 asset/node 到实际 Runtime owner 的唯一查询与控制入口。
inspect_node 先检查已发布 generation，再复用现有 asset namespacing 查 published
provenance 根，沿真实 children 返回 owning snapshots。嵌套 Composite 根、子 Composite
与 Atomic 叶子保留各自 Instance、Definition、Module、Scope、OwnerPath、实际 contract、
参数与 state；Process 值仅属于实际 Process owner，不借用叶子的身份表示 Composite 根。
同资产不同根和不同资产的同 NodeId/Definition 不混用。所有 descendant 的资源归属
node_id 沿用实际 published 作者根，digest/epoch/Instance 独立。查询不依赖 Render
输出存在，旧副本在换代后仍可读取，但不授予新执行权限。

suspend/resume/cancel 命令必须匹配 generation、作者 asset/node、该根下的实际 owner
Instance/node/digest/resource_epoch、所属 ProcessId 和当前 ProcessRevision，之后仅转发
现有 FeatureRuntime 生命周期函数。普通 state commit 不改变命令所检查的 lifecycle
revision；状态切换后必须重新查询。错误归属、旧 revision/generation/epoch 和未知 action
不产生状态变化。无效 staging 保留 last-good，成功 republication 产生新身份。

Bootstrap 的 RuntimeFeatureControl 是 Application/Runtime 下同 namespace 的无状态内部
转发桥；通过 private friend 访问已经拥有的 Host，启动前/停止后返回 NotReady。GameSDK
不暴露此头文件、Runtime getter、执行器或 Concurrency。调用限定在 Runtime owner
thread，不增加其他线程、控制器、instance map、allocator 或缓存。当前 snapshot 中
render_output 是真实已提交的 Render invocation/intent/输入消息；其他域的一般 invocation
诊断尚未提供，不从 compiler 的占位字段或 commit_count 推造。

Host 测试使用真实 Logic Process + Audio 叶子的嵌套 Composite，两个资产同作者 NodeId
及同资产另一根。查询在首次执行前生效，控制错误归属被拒绝；暂停只跳过所选叶子，
其他同定义 Process 和不同域兄弟继续 commit。恢复/取消、last-good、换代及旧副本
行为实际执行验证。

新增 Owned 模式复用 Cooked 启动图、实际 PointLight 和 P→两个 C 的 typed DataEdge，
先达到实际非空 GPU wave 2（含 PostProcess 与解析像素），再从内部服务暂停 P。
两个 C 在没有新 input 时独立 commit，保留真实旧 demand/receipt；Runtime 退休已接受
旧 cohort，后续 tick 成功、捕获为空、publication NotReady，新的 GPU 图无任何 Feature
work（包括 dead entries），仍保留 Present/readback 并输出默认黑。恢复不会立刻复活旧
cohort；实际新 P wave 3 与两个 C 全部完成后，逐 owner 校验 GPU token、真实 MessageSource
和 List 引用，并验证新曝光/背景/PointLight 像素。C 的 StateVersion 因空 commit 独立
推进，不强求与 P 相等。独立取消模式验证 terminal/epoch 改变、同样的成功空帧及
拒绝恢复。GPU native lease 退休时机仍由既有 FrameContext 控制，这两个模式不新增
对立即销毁的保证。

以上是原 Task5/Gate10 Owned lifecycle 和 Task6 共享服务的前置闭包，初次 Host/SDK
2项与 Owned 两模式已通过；最终全量证据记录在执行 ledger。Main Feature Apply/Inspect、
实际可执行 Module 集成、完整 Render profiles/temporal、动态 Feature 回调 ABI、旧业务
模型删除与全链路门槛仍按原方案执行，不把此服务等同于完整 Inspector 或 Task6 完成。


### 12.33 有效阴影的完整波次、Process 与 Cooked Owned 时序

原生 GPU 夹具新增三个有限时序模式，复用同一个 P→两个 C 的 typed DataEdge、回调、
唯一 native allocator/resolver/recorder 与已有几何/oracle。三个模式在每个参与的真实
GPU 帧固定启用中央 caster 方向阴影，不把原 iteration0 或 wave-final iteration6 的
关闭阴影用例当作组合证明。P 的 camera→directional→cull 布局从真实最终 cull 操作
读取规范输出，不再把旧两操作布局的 index1 解释成 cull。

完整波次模式先提交 CPU A，真实 async B 的 P/C1 已 commit、C2 在 Concurrency gate
等待；两个新物理帧仍捕获整份 A，publication NotReady，各帧重新创建有效阴影的
GPU bundle/graph/descriptors。释放 C2 后第三物理帧整体接受 B，canonical List、真实
consumed MessageSource 与全部 WorkItem token 一起切换，两个 HDR 的背景从
(.2,.3,.4)切为(.4,.2,.6)。三个帧都有独立的新强 build identity/frame index；两个 A
帧保留相同 CPU invocation，而 B invocation 改变。

Process 模式在真实非空有效阴影 A 已提交 GPU 后生成下一 P 输出与两个不同的真实
delivery，再 suspend 或 cancel P；C 实际消费这些消息并保留 demand，捕获仍成功空、
readiness NotReady。正常退休后，新的原生帧无任何 pass/resource/Feature work 或
culling resolve，执行成功。resume 不立刻复活旧 cohort，只有真实新 P 与两 C 完成
后才重建有效阴影、匹配当前每 owner 的 token/receipt。cancel 保持 terminal。

已有观察 lowerer 只转发唯一 native lowerer，同时弱观察实际 typed bundle：output/op/
owner/invocation 对应真实 P cull，两个 C 消费同一个 bundle。持有前一帧的强 build
identity 本身不持有 GPU/scene；丢弃 graph/output/snapshot 后 bundle 与两者在 drain
前仍存活，drain 后过期，退休 native callback 拒绝执行。实际五个 live WorkItem
(camera、directional、Compute、两 Forward)保留完整来源；Compute 的 camera read/
clusters write、Fragment 的 shadow read、有效 shadow view 与 camera view 分离，
one Compute/two fresh descriptors 均检查。两幅有限 HDR/alpha 输出相同，x54 暗区
为(.1,.1,.1,1)，x90 亮区为(.1+.8/sqrt(1.25),相同,相同,1)，背景按实际 A/B 区分。

Cooked Owned 也新增三个模式，沿原 AssetPackage/SceneSerializer/Module/FeatureGraph/
Host 与原始 shader/resource 工厂执行同样的 publication、suspend/resume、cancel。
同一 quad/green material/texture 供 receiver 与 caster 使用，caster 仅新增自己 UUID
的 mesh/material 两条 relocation，不重复资产或依赖。反射场景真实加入 casts_shadow
的 DirectionalLight，direction 字段直接设世界方向(.5,0,-1)、intensity .8；当前 extractor
读取该字段，不能仅旋转 Transform 代替。caster(-.30,0,-1.4)*scale(.15,.15,1)的阴影
覆盖 receiver 中心，自身位于相机左侧。现有 PointLight 仍通过真实 local list 绘制。

P 输出显式 directional depth/cull shadow，两个 C 读取同一 typed List 与 shadow，C2
沿已有 PP 输出最终320×240图像。实际 GPU work 必须含 P 的 directional 操作；
中心159/160×119/120是完全暗区，195×119/120是亮区，同一个 pixels_match 复用
Point 距离/衰减公式并加入 .8/sqrt(1.25)*visibility，实际验证 wave2/wave3 的 exposure
1/2、背景与设备 UNORM/sRGB 编码。受控等待仍须成功 tick、捕获空/NotReady、无
任何 live 或 dead Feature work 与默认黑；恢复继续逐 owner 比对实际消息和 GPU token，
不强求独立空 commit 后的 C StateVersion 等于 P。首批 pending、慢 C 期间保留旧完整
批次与最终全部切换同样实际执行。Owned readback tick 内已 drain，精确 drain 前后
bundle/scene/output 生命周期证明来自上述原生夹具，不能用 tick 后 Host 副本替代。

初次原生三个模式和 Owned 三个模式均实际通过，追加 typed bundle/强 scope/弱 lease
控制也通过；生产能力已经支持该组合，未修改生产实现或虚构 RED。最终 Debug
ALL_BUILD、Runtime 串行、相关专项与完整194项证据以执行 ledger 为准。
此节补齐已命名的有效阴影波次/Process/Owned 时间行为；不外推双生产者组合全部
乘积，不关闭完整 Render profiles、多方向光、任意 Shader/PSO、temporal、Task6 Main
Apply/Inspect、动态 Feature callback ABI、旧业务模型删除和原全链路门槛。


### 12.34 Module 作者 ABI 的渲染入口收敛

Module 固定宽度 C ABI 删除 `AegAbiRenderGraphApi`、`AegAbiPassSetupApi`、
`AegAbiPassCommandApi` 及对应表槽；未被调用的 `module_render_api.h` 和
`module_render_resources.hpp` 一并删除。旧 Render/RenderPrograms 能力位退休。
Render Feature 继续声明中立 RenderIntent，由引擎负责已有 RenderGraph/RHI 的
资源、构图、命令与 GPU 退休。

能力协商在原 `validate_module_api()` 中完成，正式能力集合由 `AEG_CAP_ALL`
唯一声明。模块要求的能力必须同时属于该集合和宿主支持集合；即使调用方提供全部
支持位，旧 Pipeline/作者 RenderGraph/Pass 位及未知位也不能通过。通用
context/result/value、metadata、Feature registration/graph/process 与 value reflection
表保留。当前 major2 是本轮 Feature-only 迁移，继续拒绝旧 major。

实际检查覆盖旧 C++ 表成员及头文件消失、GameSDK 旧 provider 头文件不可见、C 头文件
编译、能力位拒绝，以及真实 DLL 握手和租约生命周期。最终构建与回归证据记录于
实施 ledger。标准导出宏仍只提供握手；真实动态 Feature 回调、完整 StateDelta/
RenderIntent 传递与自动回调保活仍待实现，Main 可执行 Module/Apply/Inspect 及原
Tasks5/6/7 其余门槛保持开放。


### 12.35 动态 Feature 执行与代码/帧生命周期

标准 Module 导出绑定 DLL 内真实 FeatureModuleDefinition 工厂，统一 Feature execution
v1 替代此前未实现的 registration/graph/process 表。Logic 及其状态机仍是五域 Feature，
不引入独立 Function executor 或 Pipeline/Pass 作者入口；退休独立 logic 能力位，
使用 AEG_CAP_FEATURES。作者只编写已有 Feature callbacks，ABI bridge 由 Module 提供。

固定 C invocation 含完整 InvocationToken、StateSnapshot 的版本/schema/状态标签与
payload，以及只读 input/parameters。跨 DLL 响应为宿主 ByteSink 复制的 owning
AFR1 packet，包含完整 Foundation 结果诊断、StateDelta、九种 RenderOperation 和全部
authored message 字段。缺失/engaged-empty 状态及 intent 保留区别。总响应16MiB，
已有状态 owner 的1MiB上限不替代此协议限额。字段遍历共用一套 Reader/Writer，复用
Serialization primitives、LightList codec；结构校验不复制 Runtime 的语义验证。
私有 delivery/source/render evidence 不序列化，真实 Runtime owner 继续盖章。

协商入口先用共享固定前缀检查拒绝不支持的版本、空输出和短输出，再初始化实际工厂。
DLL 所有 endpoint 共用原 checked context table，在进入作者 execute/validator 或
写 sink 前检查 cookie、slot、generation、kind、reserved。错误上下文、坏视图和
异常均受控返回。DLL 从实际 Feature definitions 生成 metadata；宿主通过原 metadata
codec 拷贝与解码，按 sidecar 条目顺序对齐 actual entries，再使用原完整 Module
contract SHA 比较实际 descriptor/依赖/所有契约及 schema/ABI/SDK。静态注册、导出
和导入共用原 catalog validator，不建立另一套 Registry。

宿主 callable 自动共享原 DynamicModuleLease。Registry、同步回调副本和真实 Async
work 保留代码借用；旧工作尚在 DLL 栈内时卸载返回 Timeout，换代后的旧结果收集丢弃，
最后借用释放后可关闭旧 DLL。RenderFeatureOutput 是宿主深拷贝值，DLL 可以在 native
RenderGraph 编排/GPU执行之前关闭；帧输出仍由既有 FrameContext 保留至正常 drain。

真实 DLL 夹具沿现有 ModuleRegistry/FeatureRuntime 执行两组 Logic→Snapshot→Render，
验证真实输入 receipts、独立状态与 DLL validator 对非法状态的事务拒绝。两版 DLL
使用同一源码；测试 gate 在 DLL 作者 callback 栈内调用宿主 Concurrency Completion，
GameSDK 不暴露线程封装。只有旧 Async work 的借用阻止卸载，v2 接管后旧结果不提交，
v2 产生不同状态并正常 stop/join/unregister。捕获的原始 Clear/Copy intent 在 DLL
关闭后经过现有唯一 GPU helper，验证四个 Feature work 的 provenance、六个实际 pass、
两幅4×4红蓝像素和 drain 前后输出弱引用寿命。整个 ModuleRegistry bundle 热换代、
Application 启动动态加载、Main Apply/Inspect 和完整非空全链路尚未由这些测试证明。

C/C++布局、完整 codec/坏包、实际 endpoint 的五类 context 与六类 invocation 负例、
反序 catalog 正例、八个正式有效但 DLL 声明不符的 bundle 负例、SDK 和相关 Runtime/
Render 回归的最终证据以 ledger 为准。原 Task5 profiles/多方向光/任意ShaderPSO/temporal、
Task6 Main服务和 Task7旧业务模型删除/全链路以及剩余原门槛继续开放。


### 12.36 通用 Invocation Inspect 的真实 owner 边界

Task6 的 Invocation 观察不依赖 RenderIntent。Engine FeatureRuntime 在现有
invocations_ 与 owner StateStore/ProcessInstance 上提供 owning 查询，既有
FeatureGraphRuntimeHost 的节点 owner 快照直接携带它；不增加 Editor registry、
Controller、回调调度器或第二份 Token validator，GameSDK 不因此取得 Concurrency。

观察字段独立：open attempts（可以多个，附同一提交校验得到的 committable）、
last issued、last failed（owning Result + submission/callback/commit/publication 阶段）、
last committed（真实 Token + 提交后 StateVersion/ProcessRevision）、pending retry
（真实 origin/next ordinal/due tick）。issued/open 不证明 callback 进入；等待 retry
或 dispatch 成功不证明状态提交。工作线程不写 Runtime 容器，owner 消费既有 Completion
后更新事实。失败仅记录 authentic current attempt，伪造 Token、旧 generation/Process
revision 的迟到 cleanup 不覆盖当前历史。

提交记录随现有 State/Process/ResourceBus staged transaction 一起发布。状态提交后
Render publication 仍可能失败，需同时显示已提交记录及 publication failure，不能
标为回滚。显式 Process transition 与 StateMigration 虽可增加 commit_count，也不
创造 Invocation。直接 complete 的 Delta 被拒不会为了诊断改变既有可重试 Token 语义。
历史每类最多一个，只持有值，不持有 callback/native module handle/GPU lease；
replacement/shutdown 保留原 generation 的历史，图发布的新 owner 起始为空。

真实非 Render callback 的成功/失败/backoff、手工多个 Token、Process pause/transition、
迁移与旧结果、实际五域 gated worker、Composite 的无虚构记录与 Host 两 asset/独立 owner
以及提交后真实 orphan LightList publication failure均沿既有 Runtime 测试验收。
这只完成共享诊断前置能力；Main executable bundle selection、实际 Apply/Inspect UI、
Scene delta/Render viewport/session 生命周期、完整非空全链路及原 Tasks5/6/7范围仍开放。

### 12.37 Runtime 会话重建的身份隔离

RuntimeGeneration 作为现有 Token、Host Inspect 和 Process 命令的共同校验字段，
必须在同一引擎 host 内跨 Runtime 对象、图发布、定义替换与关闭保持不复用。仅给
构造函数分配种子再局部加一仍可与并存会话碰撞。现有 Concurrency 的 ThreadId 原子
签发器抽为一个内部 `allocate_unique_identity()` 原语；所有这些签发点共用它。
发布保留候选 Runtime 构造时取得的新 generation，失败候选不改变 last-good owner。
Host 初始代次来自它所持有的 Runtime。局部 Instance/Process/Invocation/Attempt
计数器无需因此增加另一身份类型。

真实 Host 回归捕获旧会话的 pending Token 与 Suspend 命令，关闭并销毁后重建同图；
旧操作均须返回 InvalidState，新 Process 保持 Ready/零提交，新 Token 可以正常提交。
并存会话在另一 Runtime 连续发布、替换和关闭时也不能出现重复 generation。内部
WorkerExecutor 并发签发回归验证身份唯一及线程身份稳定。原语不承担数据发布同步，
不持久化、不暴露到 GameSDK；64 位空间耗尽时终止，禁止回绕为零或旧身份。

这项隔离只是 Task6 生命周期前提，尚不表示完成 Main Apply/Inspect、Scene DLL/GPU
所有权、完整 Play/Stop 流程或模块到最终 Render 的端到端验收。

### 12.38 显式项目模块选择与 Cook 匹配

正式 module manifest 验证只证明 bundle 内的二进制、sidecar、版本和摘要相互一致；
它不能证明该 bundle 属于当前项目。既有 ProjectOperationService 的 `select_module`
接收明确 project file、module manifest 与 configuration，复用 ProjectFileCodec、
ProjectPaths 和 validate_module_bundle，并返回现有 owning ModuleBundle。相对 manifest
按项目根解析，显式绝对路径允许自定义构建目录；实际 build ID 与摘要来自被选中的
正式 manifest，不按目录修改时间、模块名、dev 或默认 Debug 猜测。

Tooling 私有的唯一项目匹配检查拒绝空 symbolic module ID，并将 ModuleId(project.module.id)、
project.target 平台及显式 configuration 分别与已验证 metadata 对照。module.target 是
CMake 编译目标，不能用于平台比较。现有 cook_project 复用这一检查，在启动资产查询
与目录/产物写入前返回 cook.module_project 诊断，避免把别的模块标成当前项目交付。

回归复用现有 GameSDK 构建的实际 DLL 与唯一测试 bundle 写入器，bundle 始终有效而只
改变项目 module ID、平台或请求配置，验证选择与 Cook 拒绝，以及匹配项目通过这道检查。
选择还验证与当前工作目录无关的相对路径、自定义绝对输出、缺失/空输入、路径越界和
二进制摘要变化；返回的 bundle 可经既有 Loader 导入实际 Feature 执行定义。匹配项目
缺失启动场景的控制仅证明这道检查通过，不表示成功 Cook、Main Apply 或端到端完成。

既有 Loader 在重新验证后、打开 DLL 前统一核对请求 bundle 的路径、module ID、
实际 build/platform/config、内容摘要与 contract 事实；正式 manifest 被换成另一有效
构建也必须拒绝，不能静默切换所选构建。回归用同一实际 DLL 重写为另一有效 build ID，
验证旧选择被拒绝且重新显式选择后可加载。Scene 接入复用这一检查，仍须持有实际
ModuleRegistry、Runtime 与 Host 并在真实帧边界应用，不能用目录声明代替可执行性。


### 12.39 Main FeatureGraph 到实际 Scene 执行会话

Main 面板的 Load Module 使用明确 manifest/configuration，调用既有项目选择服务和
Loader，再把真实 DLL Feature 定义注册到 Scene 拥有的既有 Registry。metadata 不能
单独生成可执行实例。候选使用隔离的 ModuleRegistry、FeatureRuntime 与 Runtime Host，
完整启动根 staging 全部成功后，在下一实际 Scene 帧发布；失败销毁候选并保留 last-good。
模块选择限于 Edit，不能把此接缝当成完整跨版本模块热替换。

Apply 在现有 active_documents 中替换所选 asset 或加入新根，然后重新 stage 完整集合。
复用 Host 的整批失败清理与 apply_pending，不建立第二调度器或 pending 状态缓存。
同一 asset/node 的 Inspect 直接取 Host owning snapshot；跨图同名 Node ID 保持隔离。
面板显示真实 Invocation 的 open/committable、committed、failed 与 retry，Process 控制
从当前快照构造完整 generation/owner/process/revision 命令。

Scene render_frame 接收独立 measured elapsed_seconds，所有测试显式提供 fixture 时间。
Main 的 UI delta clamp 不覆盖 Feature delta；非有限或负时间在候选发布前拒绝。Play
按契约频率和 backlog 策略推进；Edit/Pause 不累积游戏时钟，Pause 仍收集之前发行的
工作。Resume 保留未满周期余量。Stop 在现有 Concurrency cancel/join 后更换 Runtime
代次和 Host，重置运行状态；旧 owning Inspect 值仍可读，旧命令不能进入新代次。
关闭项目、切换项目和 shutdown 先退休 DLL 执行与注册所有权，再释放 World/资源。

真实 GameSDK DLL 经 Main 同款 FeatureGraphPanel 的 ImGui 输入框/按钮进入同一
Scene owner，覆盖 Load、Apply、selected-node Inspect、完整两根、失败候选、pending
按钮抑制、measured delta 与 Pause/Resume。现有 DLL probe/Concurrency gates 验证
真实后台回调在 Stop 时结束，旧完成不提交到新会话；共享 probe 不进入 GameSDK。

这轮证据是实际执行与 UI 服务接缝。Main 可执行文件编译不等于真实主窗口交互验证；
Dynamic.Logic 的 Continuous 夹具也不证明成功 DLL Process 命令。Scene 的 GPU
RenderIntent/history 接缝见 12.40；GPU/设备失败恢复、完整 Render profiles 与正式动态
Cook/Runtime 交付仍未证明。原 Tasks5/6/7 与完整非空端到端验收继续开放。


### 12.40 Scene Feature 输出投影与最终视口读回

Scene 和 Runtime 复用 Renderer 内唯一的 history reconcile、原生 draw lowerer 与
RenderGraph Feature build 流程。Scene 从当前实际执行图一次捕获全部 owning outputs
和存活 RenderResourceOwner；多个根共享当前帧的 Scene binding/resolver。没有新输出
仍核对完整 owner 集合。纹理 handle、resolver 和 span 仅在当前帧构建期间借用；既有
Graph/FrameContext 的输出、快照和原生资源 lease 保持录制与 GPU 完成所需的所有权。

Scene 绑定实际 SceneColor/SceneDepth，使用实际 FrameContext 的 frame_number，不能
把另一纹理别名为 SceneColor，或把 RGBA8 视口当作 HDR/GBuffer/Shadow attachment。
Device 绑定的 history 由执行会话持有。普通 Apply 使用完整 owner reconcile 退休旧
实例资源；Stop 成功 cancel/join 后重建 Runtime 并 clear history，项目/模块会话退休
也 clear。clear 不提前销毁仍被既有 Frame lease 持有的 GPU 分配。

最终读回复用唯一 viewport allocation/copy/read 路径。Fallback 无 Present 清屏先执行；
旧 Frame 终端合成与全部 Feature 投影后，才对当前帧绑定的真实 SceneColor 声明读回。
读回保留视口后续 sampled usage，避免较早 state_input 复制掩盖最终投影或产生反向依赖。

真实 GameSDK DLL 两个 Logic→Snapshot→Render 根经同一 Scene 生成 4×4 红色像素。
首值缺失时作者回调不生成呈现；Snapshot 不增加同 tick 等待。后续实际 DLL 提交只有
history copy、没有 clear，仍读出全部 16 个红色像素；Pause 继续呈现，Stop 后不重放
旧 Render 输出。此证据覆盖真实 clear/copy/history 投影及最终像素，不替代全部 draw
profiles、设备失败恢复、实际主窗口操作、成功 DLL Process 控制或完整正式交付验收。

### 12.41 Cooked GameBuildManifest 与动态 Feature 模块交付

Shipping 可执行文件只携带 Runtime 启动入口；Feature 源码和模块实现由 Cook 阶段
生成的 bundle 交付。`ProjectCooker` 先复用既有 `validate_module_bundle()` 校验
metadata、DLL、契约和内容摘要，再把三份已验证文件按原 manifest 的相对路径复制到
`<delivery>/module/`，并二次校验复制后的 manifest。二进制 `GameBuildManifest` 记录
`module_manifest_name` 与 manifest 的 SHA-256；包、文本 manifest、二进制 manifest 和
整个 `module/` 目录构成可运行交付物。Cook 先把这些内容写入新的
`versions/<publication>/`，验证 package、两个 game manifest 和 module bundle 后，再用
原子文件替换更新输出根的 `current.toml`；失败发布保留旧 selector。写入沿用现有包/manifest
编码，不能把 Editor、Source 或 Tooling 目录当作运行时依赖。

动态入口调用同一个 `RuntimeBootstrap`，传入空的静态模块定义。Runtime 读取二进制
manifest，核对 build/target/configuration、包摘要、模块 ID、模块契约摘要以及 module
manifest 摘要；随后要求模块 manifest 是发布根下 `module/` 的相对路径，拒绝绝对路径、
`..` 越界和其它目录。通过同一个 `validate_module_bundle()` 后才用
`DynamicModuleLoader` 加载 DLL、生成 FeatureModuleDefinition，并注册到既有
`ModuleRegistry`。静态测试仍可传入 FeatureModuleDefinition，但不再把游戏模块源码编进
Shipping EXE，避免同一 Feature 出现两个执行所有者。

外部工程验收覆盖 Cook 后的 selector 和完整版本目录、发布目录在删除 Source/Editor 后运行 120 帧，
以及删除 `module/GameModule.dll` 的失败启动。后者必须返回模块 bundle/manifest/Feature
模块诊断；这证明可执行文件确实依赖 Cooked 动态模块。当前证据覆盖正式动态交付和启动
边界，不替代 Module bundle 热替换、完整动态 callback ABI、失败恢复和原 Tasks5/6/7
的其余门槛。

### 12.42 非空 Editor→Cook→Runtime→Render 验收

aeg_feature_end_to_end_smoke 创建真实项目，使用现有 EditorFeatureGraphDocument 保存
Assets/Features/Default.aegfeature 及 .meta，再重新打开并核对四个 Node、两个 Snapshot
Edge 和同一 FeatureRegistry contract digest。Cook 复用正式 module bundle validator，
将真实 GameSDK DLL、metadata 和 manifest 发布到版本目录的 module/ 下，然后由动态
Runtime selector 验证并加载同一个 DLL，不传入静态模块定义。

两组 Logic→Render 根通过合法跨域 Snapshot 连接，使用独立的 Channel=4/5 参数。
其中 `Channel=4` 的 Logic 是真实 Process Feature，图中保存显式 `Idle/Active` Process
子图；Runtime 快照在首波提交后通过同一 generation/owner/process/revision 命令执行
Suspend→Resume。这样成功的 DLL Process 控制与普通 Continuous/InstanceState Feature
保持区分，不能靠 `commit_count` 推断 Process 存在。
测试注入 Runtime 时钟，明确推进首个逻辑周期并用现有 Concurrency completion pump
等待真实 worker 波次；第二个周期消费已提交的 Snapshot。验收覆盖首值缺失时的无呈现
状态和下一周期的真实 input receipt，不将连续快速调用 tick 误当作帧推进。两份输出均
保留实际 definition digest、instance/node provenance、有效 invocation、最终 intent 和
routed message。

Runtime shutdown 关闭 DLL 执行所有权后，捕获的 owning RenderFeatureOutput 仍作为值
交给既有 render_feature_test::execute_pixels_and_retire_owners。RenderGraph 执行真实
clear/copy passes，验证 Feature work 来源与 4×4 红蓝读回，并在 FrameContext drain 后
释放输出所有权。该门验证动态交付、Snapshot 时序、Runtime 输出值和 GPU 编排的连接；
旧 Editor Pipeline 作者入口、完整 Renderer profiles、模块热替换和其余 Task5/6/7 门槛
继续实施。

### 12.43 Editor Feature 作者边界与旧 Runtime 组合器清理（2026-10-10）

项目作者模型统一命名为 `EditorProjectFeatureAuthoring`。项目发现仍可读取 Frame/Render
资产，但 `.aegframe` 只形成 `EditorProjectFrameProjection` 不可变快照；Frame catalog
仅作为 Scene 内部激活依赖，不再通过 Scene Runtime accessor 暴露给编辑器作者层。Present
连接修改入口已删除，编辑器不能在已经加载的 Frame projection 上直接改写 Renderer
连接。

旧的 EditorCompositionDocument/Commands/Storage/Preview/View、旧 FlowGraph panel 及其
专用 smoke 已删除。FeatureGraph model、Feature definition/process 编辑器、节点定义与
生成服务继续保留；`Application/Tooling` 下的 `EditorCompositionCatalog` 仍保留，因为
它是 FeatureCatalog 的合并视图，不是旧 Pipeline 作者模型。生产代码没有再引用旧私有
作者文件，静态 identity smoke 固定了这些边界。

Runtime 中未被生产路径使用的 `PipelineCompositionRuntime` 公共包装器、实现和专用测试
也已删除。FeatureRuntime/Frame projection 是唯一的运行时作者输出接缝；RenderGraph/RHI
内部仍可保留 GPU pipeline/pass 名称。此次删除不等同于完成整个 Pipeline 内部迁移，旧
Engine/Pipeline 业务类型、其余 Renderer profiles、Main 失败恢复、动态热替换以及
Task 5/6/7 的完整验收仍需继续。

### 12.44 RenderGraph Present 可达性私有桥（2026-10-13）

RenderFeature 的 Present 可达性过滤只属于 Engine 内部的 RenderGraph 投影。其对已编译
PipelinePlan 的节点、依赖和 EnginePhase 遍历位于 RenderGraph/private；public RenderGraph
include surface 不再提供 render_feature_flow.hpp，因此 Feature/Runtime 消费者不能借该
查询依赖 Pipeline 拓扑类型。Editor Scene Runtime 仍通过 RenderGraph/private include
调用相同的 header-only helper，语义和执行图不变。


### 12.45 RenderGraph State Flow typed builder 私有桥（2026-10-13）

RenderStateLinkOverride、RenderStateBlendOverride 与 RenderStateFlow 是 RenderGraph 的值
合同；但基于编译 PipelinePlan 的 state-flow 遍历属于 Engine 内部 projection。其
build_render_state_flow 声明位于 RenderGraph/private/render_state_flow_pipeline.hpp，
public render_state_flow.hpp 不再提供 Pipeline-plan typed builder。Editor Scene、Runtime
和 Viewport 仍使用同一实现，只有 include surface 收口，state-link 与 Present 可达性语义不变。


### 12.46 RenderGraph 上传回执 opaque 边界（2026-10-13）

上传 Pass 的提交回执只用于 Engine Runtime 判断录制批次是否已提交或仍属于当前帧；
它不是 Feature 作者合同，也不代表 GPU 完成。完整 receipt 位于 RenderGraph/private，
RenderGraph、Resource uploader 和 Runtime resolver 之间只传递 opaque shared ownership。
Runtime 在 private seam 检查 frame readiness，GameSDK/Resource public header 不再依赖
RenderGraph 内部 receipt 类型。

### 12.47 Renderer LightCulling provenance private bridge（2026-10-13）

`RenderLightCullingResource` 的 frame/device/scene provenance 属于 Renderer profile
资源所有者的内部校验，不属于五域 Feature 或 Renderer public value contract。公共资源值
只携带 opaque `shared_ptr<const void> origin_token`；不可变的 `LightCullingOrigin` 及
其 frame、设备、场景、描述和 native handle 比对位于 Renderer/private。Light-culling
资源生产和 Clustered draw 消费都经过同一个 private validator，缺少 token、跨帧、跨
设备、跨场景或 native payload 改写都会拒绝；Feature/Runtime 仍只看到稳定 profile
resource value。

### 12.48 EngineFrame PipelineScopeHost registration private adapter（2026-10-13）

`PipelineScopeHost` 是 Engine 内部执行 host，不属于 Runtime public 编排合同。`EngineFrameOrchestrator`
的注册入口通过 Runtime-private `EngineFramePipelineRegistration` 借用 host；只有 private
实现解包并保存 typed host，Runtime Bootstrap、Editor 和 Native viewport 仍负责 host 的
稳定生命周期。World/Frame registration 的执行语义、重复绑定检查和回滚路径保持不变，
只是移除了 public 注册调用方对 Pipeline host 类型的直接依赖。


### 12.49 RenderGraph submission observer opaque boundary（2026-10-13）

RenderGraph 的 Feature history submission observer 是 Engine 内部事务参与者，不属于 public
RenderGraph value contract。public graph 只保留 opaque `shared_ptr<const void>` owner；
`RenderGraphSubmissionObserver` 的接口和 prepare/submitted/failed 回调只位于
`Engine/RenderGraph/private/render_graph_submission.hpp` 与 `render_graph.cpp`。这样保留
history snapshot 的单次接收、提交和失败回滚语义，同时移除 public header 对 observer
类型及 typed retention helper 的依赖。

### 12.50 RenderFeatureHistory opaque owner bridge（2026-10-13）

Feature history storage is a RenderGraph-private temporal service, not a public runtime
configuration type. `RenderGraphFeatureRuntimeDesc` carries only a borrowed `const void*`
owner; `build_render_graph_features` is the sole private seam that restores
`RenderFeatureHistory*` and invokes the existing history-key transaction path. The history
class, transaction observer, GPU leases and rollback behavior remain private and unchanged,
while public RenderGraph headers no longer name the storage type.

### 12.51 EngineFramePassRegistry host injection（2026-10-13）

Authored RenderPass expansion uses a Runtime-private registry adapter, but the orchestrator's
public lifecycle contract does not own or name that adapter. `EngineFramePipelineHost` now borrows
both Pipeline services and the optional `EngineFramePassRegistry`; private orchestrator code
resolves the typed RenderPassRegistry and execution epoch only inside RenderGraph build. Runtime
Bootstrap, Editor Scene Runtime, and RenderFeature smoke preserve the same pass registry lifetime,
while public Runtime callers depend only on the opaque Pipeline host.

### 12.52 RenderFeature current FrameContext opaque owner（2026-10-13）

The active FrameContext is an Engine frame-lifecycle owner, not part of the public Feature
projection contract. `RenderGraphFeatureRuntimeDesc` now transports only a borrowed
`const void* current_frame_owner`; RenderGraph-private runtime code restores the typed
FrameContext for build scopes and light-list validation. Renderer and profile paths preserve
active-frame, frame-number, device matching, depth production, and shadow/light-list execution
semantics without exposing the frame owner through public RenderGraph headers.

### 12.53 Renderer stale FrameContext declaration cleanup（2026-10-13）

`renderer.hpp` no longer forward-declares RenderGraph `FrameContext`, because no Renderer public
value or function uses it. `RenderFeatureContext` remains intentionally forward-declared: the
Runtime orchestrator public layout owns a unique pointer to that Renderer-private implementation
contract. The cleanup therefore removes an accidental dependency without breaking the existing
Runtime ownership boundary.

### 12.54 RenderGraph Feature runtime access bridge（2026-10-13）

RenderGraph's Feature projection helpers are implementation access, not public graph API. The
public `RenderGraph` type now friends only the RenderGraph-private
`detail::RenderGraphFeatureGraphAccess`; its typed wrappers cover transaction scope, owner
retention, Feature work attribution, resource description/alias checks, and commit/rollback.
`RenderGraphFeatureRuntime` remains entirely private, while public graph values and execution
semantics stay unchanged.

### 12.55 RenderGraph upload receipt private access bridge（2026-10-13）

上传回执属于 Resource-private 的构图生命周期操作，不是作者可调用的 RenderGraph API。
public `RenderGraph` 不再包含 `track_pass_submission` 名称；Resource 上传器通过
`RenderGraph/private/render_graph_upload_access.hpp` 中的
`detail::RenderGraphUploadAccess` 请求 opaque `shared_ptr<const void>` 回执，真正的
`RenderGraphPassSubmission` 和录制寿命仍由 RenderGraph-private 持有。帧号一致性、重复
请求复用、事务回滚、`clear_passes`/reset 失效和 Runtime readiness 语义保持不变。

### 12.56 Renderer material resource contract（2026-10-13）

`RenderMaterialResource` is a stable host-owned material value contract, separate from the
scene extraction aggregate and from private shader/PSO factories. Its public definition now
lives in `renderer_material_resource_contract.hpp`; `renderer.hpp` only aggregates that narrow
header for existing resolver consumers. The contract carries opaque-to-authors GPU handles,
graph-local resource handles, attachment/profile/shader metadata and variant cache epoch, while
`Renderer::is_valid()` implementation remains private. No ShaderManager, ShaderProgram or PSO
factory type crosses the public boundary.

### 12.57 RenderFeatureHistory private access bridge（2026-10-13）

RenderFeatureHistory remains a RenderGraph-private temporal owner. The Feature runtime no longer
friends the storage class directly; `RenderFeatureHistoryRuntimeAccess` is the only bridge for
same-device validation and keyed transaction staging. Public descriptors continue to carry only
the opaque history owner, and history-key isolation, GPU lease lifetime, submission and rollback
semantics remain unchanged.

### 12.58 Pipeline RenderPass execution private bridge（2026-10-13）

`add_registered_render_pass` is an execution helper for the Engine-owned authored-pass
expansion path, not a public Pipeline authoring contract. The declaration now lives in
`Engine/Pipeline/private/render_pass_execution.hpp`; Pipeline implementation, builtin Renderer
passes and RenderGraph Feature runtime use that private bridge. Public `RenderPassRegistry`,
authoring descriptors and registration macros remain available, while pass lookup, expansion,
ordering and execution-epoch semantics stay unchanged.

### 12.59 Renderer builtin registry host（2026-10-13）

The complete `register_builtin_rendering(PipelineRegistry&, RenderPassRegistry&)` operation
belongs to the Runtime/Editor Engine host, not the public Renderer identity/catalog contract.
Its declaration now lives in `renderer_variants_internal.hpp`; public `renderer_variants.hpp`
keeps stable pipeline/Feature IDs and authoring catalog registration only. Runtime and Editor
continue to own registry lifetime and invoke the private assembly seam, with builtin pipeline,
Feature, Pass registration and duplicate/error behavior unchanged.

### 12.60 RenderFeatureProjector private lowering boundary（2026-10-13）

`RenderWorkItem` and its provenance identity are stable RenderGraph value contracts, but the
`RenderFeatureProjector` class is a typed execution lowering adapter. Its declaration now lives
in `RenderGraph/private/render_feature_projector.hpp`; only RenderGraph runtime and focused
projection tests use it. Public projection values still retain Feature/Module/Node/Instance/
Invocation/Frame provenance, operation indexing and validation behavior without publishing the
runtime projector implementation.

### 12.61 Feature RenderIntentPlanner private lowering boundary（2026-10-13）

`RenderIntentExecutionPlan` and `RenderLightListUse` remain neutral public planning values.
The typed `RenderIntentPlanner`, including retired-source evidence validation and FeatureRuntime
friend access, now lives in `Engine/Feature/private/render_intent_planner.hpp`. FeatureRuntime,
RenderGraph projection and focused tests use the private planner seam; LightList provenance,
DataEdge ordering and NotReady behavior remain unchanged without exposing the planner helper to
author contracts.

### 12.62 RenderGraph Feature helper members private bridge（2026-10-13）

Feature transaction and attribution operations are RenderGraph-private implementation access, not
public graph authoring API. `RenderGraphFeatureGraphAccess` now reaches the Graph `Impl` directly
for transaction checkpoints, owner/observer retention, WorkItem attribution, dependency chaining,
texture metadata/alias checks and commit/rollback. Public `RenderGraph` retains only resource,
Pass, compile and execute contracts; existing error, lifetime and rollback semantics remain
unchanged.

### 12.63 Runtime profile Shader/PSO cache epoch validation（2026-10-13）

Runtime profile resource callbacks borrow a host-owned Shader/PSO variant cache. The resolver now
checks DeferredLighting primary/optional shadow PSO contracts, LightCulling shader contracts and
PostProcess PSO contracts against the host `variant_cache_epoch` before accepting them. A retired
callback result becomes `NotReady`, preserving profile contract values while preventing stale
pipeline/layout state from entering the RenderGraph.

### 12.64 ShaderPipelineLayout manager lifetime/access bridge（2026-10-13）

`ShaderPipelineLayout` remains a public value/API for layout descriptions and cache metadata, but
its ShaderManager owner and mutable storage are Shader-private. `shader_pipeline_layout_access.hpp`
now owns the bridge to opaque owner/epoch/hash/vector fields, while ShaderManager publishes a weak
lifetime token. Layout values that outlive the manager fail closed through `is_valid()` without
accessing a dangling manager; cache retirement and rebuilt-epoch semantics remain unchanged.

### 12.65 Runtime non-material resource host generation（2026-10-13）

Runtime mesh, texture, object and lighting caches now participate in the same explicit host
lifecycle as material Shader/PSO resources. `RuntimeAssetUploadHost::resource_cache_epoch` plus
borrowed device/frame/RenderGraph identity is captured by `RuntimeAssetResolver`; any in-place
host generation mutation with live GPU resources fails closed, while retirement still permits a
fresh generation to bind. Cache ownership and resource destruction remain host-controlled.

### 12.66 Editor ImGui execution assembly private boundary（2026-10-13）

`Application/EditorFramework/public/imgui_renderer.hpp` 保留 ImGuiRenderer 的资源所有者、
帧准备与关闭合同；目标资源使用 canonical `feature::RenderResourceId`，与原 Pipeline
别名保持同一类型。具体 ImGuiRenderFeature、Pipeline registry 注册/注销以及 typed
RenderGraphBuilder 入口移到 `private/imgui/imgui_render_execution.hpp`。主窗口、偏好窗口
与原生视口显式借用该 private adapter；PreparedFrame 的 owning callback 捕获与 GPU
退休语义不变。真实公共消费者的三项 static_assert 在迁移前编译失败，迁移后编译通过。
此边界不声称整个 EditorFramework 的传递依赖已移除 Pipeline。

### 12.67 Editor FeatureGraph parameter draft snapshot isolation（2026-10-13）

同一 FeatureGraph 面板可切换多个作者根，但参数草稿不能只按 NodeId/revision/catalog
索引：不同根和重新打开的文档可能共享这些值。EditorFeatureGraphDocument 暴露现有
不可变快照的 owning 引用，面板保留该引用并将它纳入草稿身份；保活上一快照同时防止
地址复用把新文档误识别为旧会话。正常重绘保留未 Apply 草稿，Undo 与文档替换刷新参数。
真实 ImGui Apply 回归覆盖 A/B/A、同对象同版本同内容重开和不同内容替换；不新增身份
issuer、状态库或另一条编辑事务。Process 可视化子视图的后续接入见 §12.68。

### 12.68 Process 作者状态图、布局与 Cook 隔离（2026-10-13）

Task 6 的 Process 子视图现在由实际 FeatureGraph 面板调用：显示状态卡片与转移箭头，
支持输出点连线、拖动、缩放、平移及自动布局。未连接的作者草稿仍可显示，不以 Runtime
状态机的完整可达性校验阻止编辑。入口与终态修改继续调用既有 Process 文档事务。

布局使用 Editor-private `ProcessEditorLayout`，以每 owner 一个
`aeg.editor.process-layout.v1:` JSON 扩展保存到 `.aegfeature` 源文档。完整 uint64 owner
与精确状态名参与存储；不存在 Runtime 布局合同或第二套业务状态机。纯布局修改只推进
layout_revision，不改变 graph_version、draft_revision、contract_digest 或编译后的计划。
拖动释放、自动布局及状态/转移/owner 清理都复用同一 Undo/Redo；预览与失败不入历史。

Tooling 的正式 FeatureGraph importer 在调用既有 AEGF codec 前剔除
`aeg.editor.process-layout.` 命名空间并清零 layout_revision，保留其他扩展与全部运行时
字段。真实 TOML→Cook→AEGF 回归验证布局变更前后运行时 payload 相同，源文件字节未改。
画布通过 owning snapshot 取消跨根和重开的旧交互；覆盖窗口、重叠卡片及包含 `###` 的
状态名均有实际 ImGui 回归。文本状态/转移控件也改用完整 owner 与元素索引作为 ID。
本节完成这一主线切片；完整 Task 5/6/7 与分支全量验收仍开放。

### 12.69 Main FlowGraph 导航窗口与文档生命周期（2026-10-13）

FeatureGraph 根选择器在实际 FlowGraph Begin 之后绘制，保留 EditorImGuiHost 准备的
原生窗口 class、DockID 和尺寸。此前 Begin 前的 BeginCombo 清空这些 NextWindowData，
使包含有效作者图的恢复项目无法脱离 dock；实际 missing_present 回归两次失败证实该路径。
Main 现在通过面板的同步绘制 callback 排队选择，并在整个面板返回后执行根替换，保证
参数、Process 和 definition 子视图借用的旧文档在绘制期间有效。失败切换继续保留原根。
窗口 class ImGui 回归与实际损坏/缺失 Frame projection 的 Native/Dock 恢复均已定向通过。

### 12.70 Clock palette 与原生作者内容滚动（2026-10-13）

Task 6 的 palette 现在按 Domain mask、Execution、完整 ClockId 取交集。公共作者
catalog 的 Clock 合同继续拒绝零 ID；Editor filter 的 optional 区分 All 与显式零值，
不使用低 32 位或列表索引作为选择身份。实际 ImGui 下拉来自 catalog，重排保留所选
ClockId，移除该 clock 回到 All，过滤操作不改变作者快照、dirty 或历史。

原生 FlowGraph 的 chrome 与根选择器保持在根窗口，全部作者内容、参数、Process 和
派生检查视图进入可滚动子窗口。真实鼠标滚轮回归覆盖溢出内容和返回顶部，避免把
键盘导航的自动滚动当作鼠标路径验收。Docked presentation 保持原有窗口行为。

### 12.71 已发布计划与成功帧的只读派生视图（2026-10-13）

Application-private `FeatureGraphRuntimeHost::inspect_execution` 返回所选已发布根的
拥有值：实际 Clock/instance 顺序、完整 descendant 与模块/域/Process/Invocation 归属。
作者 NodeId/OwnerPath 与 namespaced runtime ID/OwnerPath 分开保存，避免多根同 NodeId
冲突。它读取实际 issued token，不通过运行 callback、StateStore 或大 payload 构造视图。
pending/失败发布保留原 generation，退休 definition/owner 失败关闭。

Editor 在 Scene 帧成功执行后通过既有 RenderGraph-private bridge 复制计划、编译计数
及对应 published source 归属，保存为 shared owning const snapshot。派生视图明确区分
当前作者草稿、已发布计划与成功帧；实际 live pass 保留 Feature work 的完整 invocation/
attempt/operation、模块、源路径与作者 NodeId。无 Feature work 的 pass 标为帧基础设施。
同根同 generation 的源行可选择作者节点，跨根或历史 generation 不改变当前选择。

失败帧不替换同项目 last-good capture；成功新建/导入项目以及关闭项目清除当前捕获，
外部已持有的 owning snapshot 保持有效。Scene GPU 回归从真实 DLL 双根 Render commits
读回红色像素，并在实际 ImGui RenderGraph 行导航到原作者节点，不以 mock 图代替执行。
本节只完成派生检查与 Clock 主线切片，Task 5 profiles/attachments 与 Task 7 最终验收仍开放。

### 12.72 Depth-only Shadow profile 的实际 Shader/PSO 来源（2026-10-13）

Shadow/Z-prepass 共享的实际 depth-only PSO 现在发布 `RendererProfile::kShadow` 合同。
中立附件合同允许没有任何颜色且使用合法 depth format 的形状；Shadow graphics profile
只接受 depth-only，其他 graphics profile 继续要求 primary color，未知 profile enum
失败关闭。正式 Feature depth 路径仍使用 D32/X1 的既有目标校验，不借中立类型扩展 GPU
操作范围或增加另一套 PSO factory。

Runtime/Editor 从创建真实 depth PSO 的同一 graphics 描述与反射 layout 发布附件、hash
和 epoch，并保活 layout。Runtime-private upload host 的新字段不能被 GameSDK 或公共
Runtime 消费者取得。暖材质 resolve 校验已发布 Shadow profile、同一 variant epoch、
新旧附件一致性以及实际 borrowed layout 的当前 owner/hash/epoch；没有 depth PSO 时
返回的 depth attachment/profile 均为空，新 cache generation 使用真实重建的 layout/PSO。

Renderer 对每个实际 depth draw 在导入绘制资源前校验 profile/version、resource/Shader/
PSO epoch 与目标附件。真实 GPU fixture 覆盖相机/方向光两种投影、后续非法操作的完整
构图回滚，并保留原近远遮挡、Clear/Load、独立阴影尺寸、暗/亮像素与退休断言。空 caster
无需 PSO，仍走现有清屏操作。此切片不关闭 GBuffer profile 或原 Task 7 作者层删除门禁。
仍活跃的内部 Shadow/Z-prepass 无 explicit options 路径也比较固定 D32/X1 目标；
两份 metadata 均为合法 D24/X4 时仍拒绝，而不是只检测非法 profile。profile/目标检查
先于 mesh/object resolver，负例不创建这些对象资源；恢复后的合法帧继续通过原像素 oracle。

### 12.73 GBuffer ordered MRT 与实际 Shader/PSO profile（2026-10-13）

GBuffer PSO 通过 `gbuffer_profile_contract` 发布独立 `RendererProfile::kGBuffer` 身份。
中立附件值的 GBuffer 形状要求完整三槽合法未压缩颜色与合法 depth，不把 Device 支持
或具体 target ABI 混入该值层；Shadow depth-only 和其他 profile 的既有规则保持。
Feature 与内部 Deferred 降低继续按实际 RGBA16/RGBA16/RGBA8 + D32/X1 的完整 ordered
目标合同校验，合法的非 canonical MRT/MSAA/depth metadata 也不能绕过真实目标匹配。

Runtime/Editor metadata 来自同一实际 PSO graphics 描述、反射 layout hash 与 cache epoch。
Runtime owner 保活 GBuffer layout，私有 host/resolver 在 GPU 上传或暖材质返回前核对
profile/version、host variant epoch、新旧附件与实际 live layout/hash/epoch。无句柄时
返回 attachment/profile 一起清空，不读取无用 borrowed layout。真实新代次从新的
ShaderManager cache 构建 layout/PSO，不给旧 metadata 重盖 epoch；兼容同代次 PSO
替换与 graph reset 保留 descriptor/参数缓冲的既有生命周期。

GBuffer recorder 先验证材质/profile/目标再调用 mesh/object resolver。后续非法操作与
内部 Deferred 的非法有效形状均使用原图事务回滚；原 MRT 像素、近远遮挡、Load/Clear、
裁剪、HDR/后处理和 GPU 退休断言保留。该切片不代表原 Task 5 全部时序/程序范围或
Task 7 作者 schema/目录/registration 删除与完整分支验收已经完成。

### 12.74 普通 Forward 所选材质 Shader/PSO 合同（2026-10-13）

普通 Opaque/Forward 消费既有材质 `shader_contract`。选中的色深变体须与它的完整
Shader layout 合同一致，且 concrete variant epoch 非零并匹配 resource 的 host epoch。
ShaderManager 反射代次与 concrete PSO 缓存代次独立；同 layout 创建的新 PSO 可以保留
Shader 代次而推进 PSO 代次。未选中的 optional 色深或 Shadow 元数据不参与当前 draw
选择校验。共享单颜色附件形状要求合法未压缩颜色、无额外 MRT、合法可选 depth/samples。

方向阴影采样 PSO 通过中立 `shadow_shader_contract` 与 `shadow_pipeline_cache_epoch`
发布独立 full layout 和 concrete 代次。其 group-three 使 full layout hash 与普通材质
不同；不要求两者 hash 相等。Runtime/Editor 从实际创建 PSO 的 graphics 描述及反射
layout 发布元数据，并保活 Shadow layout。Runtime upload host 的 borrowed layout
仍是私有实现边界，不增加游戏 Shader 选择/反射/PSO 工厂 API。

Runtime 在冷上传或暖 descriptor 返回前检查 host 整体发布合同，包括 actual live
material/Shadow layout 的 owner/hash/epoch、单颜色形状以及 optional 色深/Shadow 的
concrete 代次；不重盖色深 variant epoch。撤销 Shadow handle 后清空全部 Shadow 元数据，
不读取无用 borrowed layout。Recorder 的选中合同检查先于 mesh/object 解析，后续坏
操作依旧使用完整图事务回滚。真实 GPU 像素、Load/Clear、缓存替换与退休语义保留。

该切片不关闭原 Task 5/7 的作者 Pipeline/Pass 身份删除与完整分支验收门禁。

### 12.75 作者删除前置：执行合同与宿主注册（2026-10-13）

RuntimeBootstrap 只保留实际使用的 FeatureRegistry/FeatureRuntime 与内部执行工厂，
不再创建或填充未被读取的 Renderer TypeCatalog。技术 Pass 由各 Engine 宿主持有的
registry 显式注册；全局 registry、静态 registrar 与旧游戏 Pass 注册宏已删除。
两个 registry 的工厂、失败注册、注销、执行 epoch 与 retained callback owner 相互隔离。

RenderProgramRequest/Provider、RenderPassConfiguration 和内部 Frame 配置直接消费
Foundation 的 AssetReference/ParameterBlock。参数值语义和 PreparedRenderProgram.owner
保活合同保留，执行头文件不再借作者 schema 头传播这些中性值。内置 Pass 的实际
descriptor/factory 路径保留，无调用的 TypeCatalogEntry/PassAuthoringDeclaration 工厂
及验证 helper 已删除，Renderer execution profile 头去掉作者目录的传递依赖。

这是实际删除前置，不是全部作者身份删除：Renderer 的11项活跃目录、可增 Pass 的
schema、Editor PipelineDocument/.aegframe 准备与视口选择链仍需一起迁移。不能删注册
后依靠缺类型触发默认预览；也不能删掉旧测试的排序、碰撞、空图和失败保留 oracle。

### 12.76 默认 Scene Preview 的真实 Feature 根（2026-10-13）

Renderer-private 的 `Renderer.Scene.Opaque`、`Renderer.Scene.Forward` 和
`Renderer.Scene.Deferred` 是完整场景能力的 Atomic Render Features，不把每个技术 Pass
重新注册为作者 Feature。定义、三项目录和默认 FeatureGraph 同源；回调只提交 owning
RenderIntent。Forward 的私有 LightList 使用真实 invocation/generation，未公开 export port。
实际 GPU 操作仍由既有 Renderer lowerer 和 Shader/PSO 合同执行。

Editor 无旧作者选择的 Scene Preview 使用独立 registry/runtime/FeatureGraph host，默认
Opaque 图真实 compile/publish/tick/capture 后投影，旧 Frame host 不参与其业务绘制。
Preview 每个渲染帧推进 Frame clock；Game session 仍按 Play/Pause 的原时钟规则运行。
两个目录保持各自 contract digest，不能重盖游戏资产摘要来接入 Engine 定义。HDR、
ordered MRT 和独立 ShadowDepth 由宿主按实际 View 创建。空图不复活隐藏绘制或清屏，
保留已有成功像素；默认恢复和跨项目重置通过候选 Feature host 的事务提交完成。
Scene 绘制计数直接读取实际 Orchestrator 结果，未打开项目也不强制为零；回归使用
有几何的正控及空图零 draw 的负控，避免统计作用域隐藏实际绘制。

不同 Runtime 的局部 Instance ID 和 resource owner 可以相同，故 Preview 与 Game 保留
独立 history。RenderGraph 用私有 observer 的 weak State 控制块身份区分存储，允许不同
存储各提交一份完整快照；同一存储的第二份快照仍拒绝，包括不同 View key。原执行前
prepare-all、失败不提交、成功全部提交和禁止重放语义保留，弱身份不延长存储生命周期。

成功帧快照保留每个源的真实 generation；顶层 generation 仅在所有来源相同时有效。
来源导航按对应源的 generation、asset UUID 和 authored node 判断，不把不同 Runtime
的元数据改写成一个代次。CPU/真实 GPU/ImGui 回归覆盖三 profile、完整操作来源、
几何像素、空图零 draw、恢复、碰撞 owner 的红蓝 history、失败和退休回滚。

Renderer 的11项旧作者目录、Editor `.aegframe` 和 `.aegscene.pipeline` 选择/编辑链、
项目 schema、Demo 资产与正式交付迁移仍开放。当前 Engine preset 根是 Editor 私有预览
来源；若后续项目图引用它们，须正式处理各目录作用域及 Cook/Runtime 交付，不能把此
预览切片当成旧作者链删除或原五域方案已经完成。


## 12.77 Renderer 作者链删除与实际 Frame/Render 检查（2026-10-13）

Renderer 的 11 项旧作者目录、公开 renderer_variants.hpp 和注册实现已删除。
原生 profile ID 与装配函数只保留在 Renderer private 技术设施。Editor 项目目录不再
暴露 PipelineDocument/frame_catalog/FrameProjection；旧 .aegframe/.aegpipe 只按来源
诊断，不解析或迁移其 payload。改名 .aegfeature 的旧 payload 由实际 Feature reader 拒绝。

Scene 永远通过独立 Preview 与 Game FeatureRuntime capture 投影 RenderIntent；旧
FrameHost/native fallback、StateLink/blend/ordering 作者链与 compositor 资源已删除。
默认恢复在 Edit 帧边界准备候选 FeatureHost/history 后交换；Engine 与 Game 目录、
摘要、时钟、history 和 generation 保持各自真实身份。三个 Engine preset 是私有
预览来源；不为保留旧 fixture 创造另一个启动字段、preset 资产或 Cook 旁路。

Main 保留实际 FeatureGraph 选择、编辑、Load Module、Apply、Save 和 Runtime Inspect。
Frame/Render 只读区直接读取已发布 Preview EP 与最近成功 RG；无 Game 作者图时也可
检查实际来源。旧文件 Apply、scene.pipeline 保存/恢复、ordering/blend 编辑和未实现
profile 选择的 Project Hub 卡片均删除。Native 区在填充画布前显示可滚动检查内容。

ProjectConfig 删除 startup_frame_projections。Codec load/save(retained_source)按精确
build 字段 presence 拒绝两个旧启动键，包括空数组、错误类型及 format 1/2；启动
Feature roots 的旧扩展按 ASCII case 拒绝，无关 vendor 字段和有序多个 roots 保留。
Editor 保留具体 Unsupported 诊断，禁止 removed key 经旧 header parser 静默 restamp。
Scene Open/Save As 拒绝旧扩展和 sidecar；失败 New Project 先验证候选 manager，保留
当前真实 Game session、来源、场景和像素。Demo 四份实际旧作者资产精确删除，备份
字节保留在工作区并排除旧 .aegpipe.bak 安装。

真实 GPU fixtures 保留 Forward/Deferred 的方向、Shadow、Lighting、Environment、
驻留网格和 Play/Pause/Stop oracle，并验证实际 Feature token/Node/Instance/Module 与
活跃 WorkItem。Main recovery 的五个拒绝 case 和一个正例同时验证真实项目/Scene、
Opaque EP、成功 RG 来源、Native/Dock 切换和原文件 SHA；不以 exit0/警告替代来源证明。
夹具恢复原项目使用 binary COPY_FILE，避免 CMake 文本写回把 LF 改成 CRLF。

此批删除的是活跃 Renderer/Editor 作者链。全局旧 Pipeline authoring/document/IO 死层、
AEG_PROJECT header 自动转换与 project.aegproject fallback，以及原方案最终验收仍开放。
Foundation 参数值、现代 Feature/Module/PortRoute schema 和技术 World/Frame/ImGui/PSO
调度设施不能与旧作者序列化混删。

本批最终根 ALL_BUILD Debug 通过；完整 CTest206/206（367.68秒），包含三项冷构建与
实际发布/负例、六场景Main来源/恢复/SHA验证。23份完整源空白扫描与全仓diff-check通过。
完整原目标继续开放，后续按上述剩余作者层与兼容链推进。

## 12.78 全局作者文档删除与现代项目边界（2026-10-13）

Application/Authoring 的 Pipeline 文档 IO 与 Engine/Pipeline 的旧作者 schema、
PipelineDocument、catalog 和 FlowGraph ordering 转换实现已删除；CMake 同步删除来源
与旧验证目标。Authoring 的独立消费者只得到 Feature/Foundation 合同，不继承 Pipeline
include 或链接边。Editor 零消费者的 ordering/layout/blend helper 和旧 debug 开关已删除。
一般图验证迁至真正 FeatureGraph，保留合法拓扑顺序、真实定义及 owner provenance，并
明确覆盖 input→output、input→input、output→output 拒绝与缺端点/端口/schema/version。

FeatureGraphCompiler 的失败值现在填充既有 owning diagnostic：真实坏节点、已知 owner
及实际坏端口，不返回部分 plan/provenance。节点级错误没有端口时保持空 port，不借任意
相邻边制造位置；诊断副本在原图清空后仍有效。既有 ResultCode/message 保持，未增加
另一个作者身份或失败执行计划。

ProjectFileCodec 只接受明确整数 format_version=2。缺省版本、旧版本、错误类型与未知
版本在 load、typed save 和 retained-source save 中明确拒绝；不重盖原文版本。旧
.aegproject 扩展按 ASCII case 拒绝，AEG_PROJECT header 即使藏在现代文件名下也拒绝。
根目录只选择 canonical Project.aegproj；旧文件单独存在仅提供 Unsupported 诊断，不
fallback/转换。显式旧文件不能因同目录存在现代文件绕过拒绝。

保存还在真正写入前校验既有目标的格式与已退休字段，禁止现代 typed/retained 配置覆盖
旧 TOML 或损坏目标而形成隐式转换。该校验不映射旧配置，不合并目标字段，也不盖过
候选源自己的具体错误；不存在的合法现代目标可创建，已有版本 2 配置可正常更新。
28 项实际目标负例及源/目标/临时文件字节保护已验证 RED→GREEN。

EditorProjectManager 统一使用正式 discovery/codec，不再解析 header、写 canonical
转换文件或保留 legacy 标记。成功选择的现代文件路径归会话持有，设置更新写回该文件；
失败导入保留 descriptor、路径、recent scene/list 及全部源字节。Main 和 Demo 使用现代
Project.aegproj，精确删除旧 Demo project.aegproject。配置路径用 UTF-8 编解码，实际
Unicode 路径、多 roots 顺序与 vendor 字段保持。现代 Feature/Module/Scene payload 的
schema 版本不属于这些旧项目格式，不能一起删除。

Project 的 TOML 读取使用原生文件流与 UTF-8 来源名称，避免路径重载为诊断来源调用
本地编码的 string()，从而拒绝可用的 emoji 文件名。原生文件名/根目录/两者组合的
load、discovery、typed 与 retained 更新，以及坏目标的真实来源诊断已单独取得 RED→GREEN。

Recent 项目记录保存实际选中文件，按文件去重，Main 的卡片身份、显示及重开动作都使用
该文件；同目录 canonical 与 Vendor 项目分别保留。UI 最近路径用 UTF-8 二进制存储；
不猜测只有目录的旧记录。路径解码失败只跳过坏 Recent 行，坏 Scene 缓存返回既有
optional 空路径，保留原缓存字节、项目 descriptor 和源文件；不转换旧编码。实际
MSVC 解码抛 system_error/1113，最终捕获只围住解码，分配和业务执行在捕获之外。
精确文件重开、Unicode 文件/root/Scene、坏行与坏 Scene 缓存均独立验证 RED→GREEN。

本批已经取得独立作者消费者、源删除、Project/Manager 行为和失败诊断的真实 RED，再
运行 GREEN；最新根 ALL_BUILD Debug 通过，最终定向 CTest22/22（34.55秒）与完整
CTest206/206（379.73秒）通过，三项冷构建及实际交付/Main 恢复均保留。
原五域方案仍未完成：公开 PipelineFeature 五域继承、FeaturePoint/Registry/ScopeHost、
WorldScope 的原始 Pipeline 注入和 RenderState 作者覆盖字段仍需迁移。任务/执行节点
还需真实 FeatureDefinition/Invocation provenance 或明确技术设施分类，不能把旧裸任务
统一标作基础设施。Module 已只注册 FeatureDefinition，技术 WorldCommit、快照、多 View
与 Frame/ImGui 调度继续保留。

## 12.79 World 公开注入删除与强类型 Frame 快照（2026-10-13）

WorldScope 只接受已有 ECS World 的共享所有权，持有命令缓冲、聚合状态和快照发布。
旧 WorldScopeDesc、Registry/Host、Pipeline 绑定/重建及计划启停入口删除；Orchestrator
的 Engine 私有注册路径也明确拒绝原始 World Pipeline，防止公开入口删除后仍能绕回
同一业务执行。Frame 技术观察器继续注册，失败 World 注册不污染同 ID 的 Frame 注册。

World/Frame 的公共头不传播 Pipeline/Artifact。Frame read_snapshot 返回拥有共享不可变
SnapshotData 的 WorldSnapshot 值，不分配新的读租约。canonical Artifact 身份和桥接
仅由私有 WorldSnapshotExecution 提供，World 内部独立 ArtifactRegistry 发布标准合同。
每次 initialize 使用新存储与活跃代次，Frame 保源存储且活动 token 为弱引用；shutdown/
reinitialize 使旧 capture 失效，已捕获值及读出的快照继续有效。世界/当前 revision/禁止
倒退校验保留。aeg_world 对 Pipeline 改为私有链接，真正 World-only 消费者验证边界。

Runtime/Editor 使用聚合构造，Loader 不接受业务 Pipeline 描述；Runtime 从未填充的
pipeline_hosts 及 raw World execute 路径删除，保留命令应用、WorldCommit、Scene 引用
重定位、多 View RenderExtract、实际 Feature 渲染、Play 隔离、时钟和资源退役。

World-frame 回归把原裸任务 ECS5→7 替换为实际编译的 Logic Call FeatureGraph，执行
dispatch_call，提交版本化 StateDelta 后由夹具私有的 World owner 校验真实 provenance、
generation、digest、schema、owner/world、已提交 token 和 state version 再应用。callback
没有可变 World 指针，保 command→callback→delta→唯一 revision1 WorldCommit→Frame
顺序，并拒绝坏 schema、foreign World/owner、stale token 和 replay。两 World/三 View/
两帧及 sentinel 桥恢复、旧值保活、重初始化拒绝均保留。

当前生产尚无通用 Feature→ECS 投影桥；夹具私有消费者是本批回归，不能用于声称生产
World 业务已全部 Feature 化。Editor demo rotation 和 Runtime callbacks 的业务/host
归属仍需审计，公开 PipelineFeature 五域层次及任意 Task/Plan owner/profile-infra 规则
仍开放，原完整五域目标继续推进。

本批最终 ALL_BUILD Debug 通过，定向 CTest40/40（54.54秒）与完整 CTest208/208
（397.44秒）通过。三项冷构建、实际发布负例、源 SHA 与 Main 恢复守卫保留；23份
完整源空白检查及全仓 diff-check 通过。本批验证不会关闭上述剩余原方案要求。

### 12.80 Pipeline 物理公开边界与中立 RenderGraph 值迁移

Engine public consumer fresh build 成功、direct exit1 得到四个真实 RED：公开 Pipeline
执行层次/声明 backend、内部 StateFlow、Frame link/blend author overrides，以及 Renderer
第二套 Pipeline graph handles。本批删除零消费者 pipeline_execution.hpp 与模块声明 backend
整层；五个执行/配置/Pass/Program 头物理移至 Pipeline/private，没有公开 shim。

Renderer 三个公开头只用 Foundation stable IDs、Feature resource IDs、同一组原生 Graph
handles。Snapshot Artifact 常量与 RenderFeatureContext 留在 Renderer/private。Orchestrator
公开布局转 PImpl，所有 pin、host、phase、bridge 和 context owner 在 cpp；实际 preparation
context 仍指向外层 owner，abort/release/deactivate/clear 顺序保持。World/ECS snapshot API
和原 View 回滚不改变。RenderState link/blend overrides 类型、字段、参数及应用循环删除，
StateFlow 与 composite 输入私有化；无消费者的 enabled/composer 通道本批暂留技术合同。

Pipeline CMake 全部 usage 私有；Renderer/RenderGraph/Engine 的 Pipeline link 不传播，
Tooling 完全移除不使用的 include/link，ImGui 设施依赖显式私有。内部执行与选定 native
fixtures 显式声明 Feature/RHI/Concurrency/private 编译合同，公开消费者不继承旧路径。
RuntimeAssetResolver 的中立值直接复用 Graph handles，删除四个恒等转换 helper。

Ruling：物理公开边界封闭不等于原旧类已经在所有内部设施消失；任意 Task 的可信来源、
legacy Scene render contribution 的真实 Feature owner、Tooling Module/commit provenance、
正式生产 World typed output 聚合和 Demo/callback 业务归属仍开放。原 Tasks1–7 不缩小。
模块声明夹具直接验证实际 builder 的配置/排序/权限，不再验证已删除的录制转发 seam。
源码删除边界 fresh build/direct0；统一 ALL_BUILD 与定向/full CTest 最终证据后补。
统一 ALL_BUILD Debug 保留 references exit0，定向 CTest54/54、5.20秒通过。构建暴露的
私有 lowerer、Runtime/ImGui deps 和两纯值 test residual 已实际修正，失败日志保留。
完整209项将在新空fixture目录执行，同原用例/properties/assertions/defaultcwd，不删除
原目录；source/test/docs在full cold全过程冻结，最终证据待terminal。
本批最终证据：ALL_BUILD0、focused54/54。完整首跑208/209、381.27秒，三项实际cold
build242.25/379.19/381.13秒通过；唯一恢复夹具因新末级目录名不符合原安全guard，在
product启动前拒绝。保guard，改generated TEST_ROOT为新父目录下原basename后补跑
1/1、23.13秒通过，原六case/marker/60frame/nativeviewport/SHA断言不减。全部209项已有
通过覆盖，记录为208+1，不冒称单次full209/209；失败日志保留。45源whitespace与全仓
检查通过。测试退出后才补文档，cold期间所有源/测试/文档冻结。

后续默认方向是退休0生产消费者的raw Task CPU callback，保技术Stage/phase/Artifact/
planpin与nativeRG；业务测试迁真实Feature invocation+typed输出。任意infra label不是
业务授权。GPU thread lane属于Concurrency设施，五域Feature mapping没有GPU domain。
formal World生产投影、legacyRender owner、Tooling Module/token等仍原完整目标待办。
### 12.81 退休 Pipeline raw CPU Task 通道

真实新 Frame RED freshbuild0/direct42：Task userdata持World并直接将ECS5写7，唯一
WorldCommit发布7，没有Feature invocation owner。两个fixture编译attempt（latest_snapshot
ResultValue与explicit stable-ID构造）保留，不当作RED。

Production 没有 add_task 调用，故删除 TaskDescriptor/TaskExecutionFunction/Task IDs/Flags/
handles/records、PipelineTaskContext、add_task/bind_task、Node execute/userdata/synthetic，
以及无生产消费者的Scheduler worker/executor/cancel facade。不引入任意infra callback
factory。结构计划一Stage一Node，保阶段/phase/domain/time/artifact bindings/source依赖/
贡献区间/plan pin。visited_nodes每调用清空，成功一次发布phase选择的拓扑访问；索引/
覆盖/重复/metadata/dependency失败无部分记录。ScopeHost automatic applypending和explicit
pinned phase不切计划、Artifact contracts/lease/bridge/type/version/immutable semantics保留。

Runtime 私有 opt-in owning trace无 callback：固定记录实际Stage、phase、host、Frame、
userdata地址身份、WorldFrameKey与当前typed snapshot、实际Frame host Artifact bridge读。
默认不累计；活跃帧切换拒且已记录值保留，capture copies在shutdown/disable后保旧值。
World两World/三View/两帧/currentScope/地址/userdata/sentinel与phaseStartup→PostFrame→
Shutdown顺序保持。诊断不能执行业务，Frame不再借rawTask收集这些证明。

四旧业务fixtures换真正Logic CallDefinition/Module/compiled graph/DataEdges/dispatch_call/
StateDelta commits。ScalarProjection只消费actual lastcommit，验证actual compiled provenance/
完整ownerpath/definition digest/module/schema/generation/token/state/input/replay。1→2→3
业务来自反向authored graph的真实数据输入与消息stamp；Artifact42与snapshot7来自实际
committed payload，不使用call后写常量。schema/foreignowner/otheractualtoken/stale/replay
拒绝无Artifact mutation；hotSwap的pending/active/pinned/retired/writer阻version与注销
nativeimpl/definition寿命保留。该helper明确test-only，不能充作生产World投影完成证明。

线程Main/CPU/IO/Render/GPU、预取消、拒非replace保持旧executor、实际IO Inline替换转
Concurrency设施测试，原1–32覆盖不删。GPU仅ExecutorDomain，五域Feature没有GPU worker。

本批Engine target withrefs、RawFrame0、sourcegate0、observer3/3和phase/concurrency2/2已实证；
统一ALL_BUILD Debug /m4 references exit0，定向56/56、8.77秒通过。完整210项回归待final。
原Tasks1–7继续：legacy RenderContribution/内部跨域hierarchy、Tooling Module/token、formal
World typed outbox与Demo/callback归属等仍待完成，rawTask退休不替代它们。

本批最终完整 CTest210/210、364.24秒、exit0；embed220.59秒、external339.62秒、实际
game runtime341.42秒和六case Main recovery22.74秒通过。统一ALL_BUILD0、focused56/56
与单次full210/210均已结束。Generated registry只换四个TEST_ROOT父目录，保原basename、
全部用例/properties/defaultcwd、实际发布负例/缺DLL/源码SHA与恢复守卫；不删旧目录。
所有源码/测试/文档在cold全过程冻结，terminal后才补结果。27源完整空白与全仓检查通过。

下一边界是内部virtual Pipeline.build/collect与legacy RenderGraph contribution：前者仍可
在Frame注册/编译时执行无Feature身份的任意callback，后者实际Scene draw pass只有C++
plan owner而无RenderWorkItem。旧非Render五域类没有生产消费者，应实际退休，不以私有
改名延续。保真实Feature Call/Output正例、Stage/phase/Artifact技术值，以及原生Scene draw
像素/深度/PSO epoch；固定native draw helper应脱离旧类。ImGui三host的upload/overlay/
Present设施按固定私有adapter保资源至GPU完成。生产World输出、Tooling Module/token、
Demo业务归属和原Tasks1–7继续，任意infra label与测试绿色不能替代业务来源证明。

### 12.82 Pipeline 固定声明值与编译期回调退休

真实 Frame 注册旁路 RED freshbuild0，factory/build/collect 三种直接持 World 写 ECS5→7，
WorldCommit 均发布7、direct42，没有现代 Feature invocation。名字 RED 在 caller backing
string 仍存活时修改首字节，实际 builder definition 名称跟着改变，direct42，无悬垂读取。

Registry 只收 owning PipelineDefinition / PipelineFeatureDeclaration 值。Pipeline virtual
build、两 factory、collect/setup、FeatureBuildContext、DomainGraphBuilder 和非 Render
五域旧类/Composite 实际删除，不做 private 改名。Pipeline、Feature 与 point 名称拥有
std::string，标准/Variant 模板本地固定函数产生完成值，消除局部 stage name 借用。
纯注册重放所有 Stage/Artifact/point/binding/ordering/exposure 验证后入表；编译固定
展开局部索引、anchor 顺序、Artifact 访问与权限。Variant pipeline/scope/point 与 authored
type/queue/空参数/无资源重绑规则转声明限制表，依旧由 compiler 固定校验。

计划 pin 真正不可变 definition/声明值，native Render implementation 仅保实现寿命。
旧 CPU fixtures 保真实 Module/Call/DataEdge/StateDelta 业务；配置观察改实际计划字段与
身份检查。HotSwap pending/active/pinned/retired、版本1→2→3、ArtifactWriter阻切换、
ordering 和注销后 owner 存活/最后计划释放后过期均保持。World多 View、两帧、实际
snapshot/bridge/userdata/sentinel 证明保持，无旧编译期观察 callback。

新聚合边界另实证 discard、不同 Feature 配置共用实例 ID、冲突 binding version/合并
flags RED42；固定纯校验拒绝、registry 原 owner 不变、失败无部分 plan，均 GREEN0。
两 local Stage、snapshot publish/read version7、cross-index ordering、Editor exposure、
兼容 duplicate flags 合并和实际声明 lease 正例通过。Render null implementation 拒绝，
非 Render 结构数据的 native lookup 返回 NotFound，不将 success/null 当可执行实现。

Factory/build/collect/name 四模式 GREEN0，Engine withrefs build0，定向11/11、2.24秒。
根统一 ALL_BUILD、GPU/Editor 定向与完整212项测试待终态验证，cold全程 source/test/docs
冻结后才运行。旧 RenderFeature/PipelineFeature 的每帧 native 图回调与旧 Scene work 来源
仍需迁真实 RenderIntent/RenderWorkItem；native PassRegistry epoch/commands/lease 和
像素深度不能删。ImGui 应固定技术 overlay adapter。Tooling Module/commit、正式生产
World outbox 与 Demo/callback 归属等原Tasks1–7要求继续，本批不宣称全部五域完成。

根ALL_BUILD Debug /m4 references exit0；核选67项后定向CTest67/67、9.45秒、exit0。
首次无用例的filter语法attempt保留，不能以exit0当验证通过。29份完整源空白检查、
全仓diffcheck与保留bak SHA通过。完整212 registry保持全部commands/properties/
defaultcwd，只更换4 TEST_ROOT父目录并保原basename，旧目录不删。Cold source/test/
docs从运行前冻结至全部exec terminal，最终full事实后补。

最终单次完整CTest212/212、355.22秒、exit0；三冷构建211.72/329.57/331.36秒与Main
六case恢复23.77秒通过，源码SHA、发布负例、缺DLL与所有原断言保留。Cold全过程
source/test/docs冻结，wrapper terminal后才补本事实，ALL0/focused67/67/full212/212。

只读审计另确认未覆盖的authored配置死接缝：runtime仍提供实际Feature实例配置，但
native Renderer consumer查询恒null的旧declaration getter，因而instance/source未进入
实际PassRegistry配置，Variant authored集合分支不可达。原GPU测试的count/pixels不足
证明它；下一批需实查实际setup/execute config与finaloutput行为的RED，然后在真实
Feature操作下接固定native dispatch，不复活旧作者Pass模型。旧SceneDraw实际geometry
pass缺真实FeatureWork也仍开放，不能把本批完整绿色当这些语义或原五域全量完成证明。

### 12.83 真实 Render operation 的固定原生分派

authored配置与真实SceneDraw分别取得行为RED：实际Registry配置的instance/source错误，
以及Feature invocation/像素成功但原生setup/execute为零。当前Feature实例配置只为
存量private native序列提供技术数据；不恢复旧作者Pass模型。实例、队列、参数、绑定、
source与ordering按实际配置校验，并用真实Graph pass handle建立依赖。

现代Render投影从当前已提交operation复制完整RenderWorkItem、resource owner、View与
depth projection，固定映射现有七个原生Pass类型。Source与inspection均为private值，
公开Feature没有Pass名字、注册或队列输入。Opaque绘制使用固定free recorder，实际业务
归因仍由原projector checkpoint完成；selected snapshot指针与depth/light生产证据保持。

显式Registry host必须有效且epoch当前，不能回退direct。prepared commands保不可变
配置/source，并与执行配置全部字段匹配。同次查得的实际Pass与配置由Graph留到Frame
完成，注销/reset不损失在途GPU所有权；失败构建回滚新增owner。Runtime所有View与
Editor preview/用户Feature现在传各自实际host。固定Cull按native合同使用Compute，
dedicated queue权限仅由CullLights能力授予。定向/完整验证结果记于handoff§120。

旧RenderFeature graph adapter、legacy Scene fixtures、ImGui overlay、正式World outbox、
Tooling实际Module/commit与Demo归属仍须继续。当前原生分派不代表原五域完整目标完成。

当前single ALL_BUILD Debug references与定向87/87、13.80秒已通过。七role真captured
source、其它owner/另operation prepared交换、已resolved注销后执行和Frame退休均保。
额外真实WorldFrame双View经过Renderer提取，逐View原Feature投影/像素成立，samework.id
使用不同nativeinstance，跨View prepared交换拒绝，snapshot/output/commands寿命到GPU
完成。完整214 fresh-parent suite待终态；cold source/test/docs全冻结，终态后记结果。

最终完整214/214、381.20秒、exit0：三冷构建/发布负例/缺DLL与源码SHA守卫通过，Main
六case恢复24.39秒通过。所有commands/properties/defaultcwd保持，仅四TEST_ROOT换新
父目录并保原basename/oldroots；全程源码/测试/文档冻结，wrapper终态后才补本结果。
ALL0/定向87/87/full214/214支持本批边界和执行语义迁移；旧Scene/Render适配器等原目标
仍开放，下一步普通opaque/multiframe必须以真实geometry与committed token取得RED。

### 12.84 Opaque Scene 成功路径的真实调用来源

普通opaque/Forward/Deferred/backlit与multiframe以真实GPU geometry、成功材质pixels和
实际pass handle取得缺FeatureWork的RED。成功入口现在实际执行Scene Feature的
compile/publish/tick/capture→当前Renderer View投影→固定原生Registry，不增加另一
旧入口或假owner。多帧逐次提交新token，保原三帧与恢复帧像素、实际材质换版/defer、
非法合同/缺纹理/rollback；native prepared source与actual投影完整WorkID和View相符。

旧OpaqueRenderFeature类、register/bind与私有注册头删除，builtin旧身份查找NotFound；
固定原生绘制器与七种Pass设施保留。两个仅观察snapshot的旧虚对象改真正owning观测，
核发布Artifact同对象、版本/Frame/World和释放后存活，不给它们编造geometry业务身份。

实际GPU帧退休另实证跨槽descriptor/view依赖：双槽提交后，按单槽释放先销毁的view仍
被后槽set引用，RED42。FrameContext在所有completion/command清理后按资源依赖种类
跨所有选中槽释放；保失败句柄与原错误以便重试。单槽abort/reuse范围保持。外部未退
descriptor导致的真实first-drain错误、释放外部owner后的retry与实际handle退休均验证。
统一ALL_BUILD Debug references通过；定向87/87、10.07秒，完整214/214、355.94秒，
均exit0且所有handles终态。完整suite核214名称/命令/属性/工作目录等价，仅四个夹具
父目录换新，保原basename与所有守卫；cold全过程冻结22份本批source/test/docs SHA，
终态一致。详情见handoff§121。shadow GPU六个旧成功模式与VariantFeature、ImGui、
formal World/Tooling/Demo及原五域完整验收仍开放，不以本批绿色关闭原目标。

### 12.85 Renderer VariantFeature 业务身份退休

shadow GPU的Forward、local-light、postprocess与Deferred两个模式取得真实成功像素后，
原live Scene pass无FeatureWork的RED。现执行production Scene定义的compile/publish/
tick/capture与当前实际View/native epoch；unshadowed路径保真实producer→两个独立HDR
consumer、六帧pixels、六组PSO晚期失败回滚、unusedShadow与emptyScene无LightList依赖。
实际native setup/execute与图中work核完整Projector ID/token/owner/operation/frame/View。

生产旧VariantFeature类、两旧FeatureId及四个register/bind函数删除。实际builtin registry
的声明和实现查询均拒旧Forward/Deferred身份；固定6/7阶段技术计划与七种native pass保留。
固定native sequence用私有无Feature/point/part身份的配置值，复用唯一真实backend的RAII
事务session。图测试保reorder/shorter/disabled/order、配置篡改、晚期非空回滚、epoch/
注销保活、像素、drain/reset寿命，不再借旧Feature注册来调用技术设施。业务profile另经
真实World commit/Renderer提取与Feature capture验证，整图技术helper不能冒充一条来源。

首定向24/24与空场景Cull实际plan存在且culled验证已通过；最终统一ALL_BUILD Debug
references、广定向87/87（9.45秒）通过，完整suite结果以handoff§122终态为准。
完整CTest214/214（352.43秒）exit0；全部214注册命令/属性/工作目录等价，仅四个
夹具父目录换新、原basename与旧目录及所有守卫保持。Cold期间22份本批源码/测试/
文档SHA一致，所有handles终态后才补事实，详见handoff§122。
ImGui三host、通用旧设施、正式World outbox、
Tooling实际Module/commit及Demo/callback归属仍开放，原Tasks1–7目标不缩减。

### 12.86 ImGui 固定 overlay 与共享纹理退休

Editor 的 UI overlay 使用私有固定构图入口，旧 ImGuiRenderFeature 类、业务身份与注册/
注销已删除；不为技术 UI pass 制造 Scene Feature provenance。入口校验真实 Device、
graph/代次与 attachment 描述，通过同一 NativeGraphBuildSession backend 事务录制。
空 packet 仍有使用 Load 的 live side-effect pass，PreparedFrame 在 prepare 时保活到
Frame GPU completion，graph callback 同时持有真正 buffers/descriptors/geometry。

共享字体与 SceneColor 只能在所有采样 host 的 Frame 已 drain、Graph 已 reset、Renderer
prepared frames 已批量释放后替换。单 host release 或 device wait_idle 均不能退休其他
Renderer 的 descriptors。双真实 GPU host 在旧 main-only 释放顺序下 destroy view 报
InvalidState；完整释放组之后真实 view/texture/sampler 均能销毁。实际旧身份先得到
successful lookup 的 RED，迁移后要求 declaration/implementation 双 NotFound。

三个 Editor host 均按 clear→prepare→overlay→readback/Present 构图；原检查在 UI 前读回
clear 的缺陷因此暴露。真实 BGRA8 Unorm 四角62/52/27/255与实际窗口边框色逐通道吻合，
旧纯黑角落判据不能验证最终 UI。保留256探针，按真实 packet 的 scissor/三角形顺序、
顶点RGBA/UV、实际font atlas双线性采样及alpha混合预测最终像素；RGB仅允许1量化差，
alpha与未覆盖clear精确，非字体纹理覆盖直接失败，各角须有真实绘制且GPU draw数吻合。
应用main/preferences/native/missing-project已4/4（16.44秒）通过，统一ALL_BUILD Debug
references与广定向88/88（10.14秒）、完整215/215（354.98秒）均exit0，所有handles终态。
完整215注册名称/命令/属性/工作目录逐项等价，仅四个夹具父目录换新、原basename/
旧目录和守卫保持。Cold全过程647份source/test/docs SHA一致，终态验证后才补事实；
8份本批完整源码whitespace0、全仓diffcheck0、保留bak SHA不变，详见handoff§123。
具体派生入口退休后，
通用旧 Render executable callback 链仍需下一批取得实际 RED 后删除；真正 Feature
projector/static lowering、Frame 技术 Stage/Artifact lease与 native事务设施保留。
正式 World outbox、实际 Tooling Module/commit、Demo/callback及原Tasks1–7完整验收继续。

### 12.87 通用旧 Render executable 链退休

旧 PipelineFeature/RenderFeature 虚执行对象、implementation字段/record/vector与查询API
已物理删除。Registry只保非Render结构声明与实际不可变owner；旧Render声明在任何入表
前返回InvalidArgument，要求使用已提交Feature输出。固定技术Render Stage/Artifact仍由
PipelineDefinition提供，不能静默接受已经无执行路径的旧作者配置。

Orchestrator不再遍历pinned Frameplan执行旧instance Runtime；真正Scene post_build的
capture/projector/native lowering保持。六个旧Runtime/Plan bridge/PassHost/composer/
state-flow文件删除；现代lowering通过已有free API，函数体只将空buffer span绑定helper
换为texture-only初始化，归一化逐字相等。NativeSession/真实native pass与configuration
lease、Frame提交/退休、history和完整操作来源保持。public Desc的旧buffer/state/
configuration/userdata/featuredata transport退休，Shader/PSO/RHI host所有者边界未扩张。

四个旧state字段没有存活producer；Renderer旧StateSeed/隐式输出与Load分支随之删除，
实际Feature StateStore、history、显式附件与draw options保持。技术测试直接用原preparation/
NativeSession；不重新创建业务callback或假Scene Feature来源。编译期验证旧类、字段与
lookup API缺席；旧身份在实际registry查询descriptor/declaration均NotFound。

实际RED为原Orchestrator全合同后旧callback_count=1/direct73，以及两轮真GPU culling
全部原readback/descriptor/stale/退休后仍找到旧declaration+implementation/direct73。
首次迁移测试的Render旧声明被接受负例暴露静默空执行风险，显式拒绝后首定向30/30
（6.03秒）通过，包含20个原ShadowGPU模式。统一ALL_BUILD Debug references、广定向
88/88（10.59秒）、完整215/215（355.56秒）均exit0且全部handles终态。完整215名称/
命令/属性/工作目录逐项等价，仅四夹具父目录换新，原basename/旧目录/全部守卫保持。
Cold全过程641份source/test/docs SHA一致；24份完整源码whitespace0、全仓diffcheck0、
保留bak SHA不变，六旧文件缺席与现代lowering归一化一致均终态复核，详见handoff§124。
旧结构作者元数据、正式World outbox、实际Tooling Module/commit与
Demo/callback仍按原Tasks1–7继续，不以本批绿色宣布完整五域目标完成。

### 12.88 旧结构作者链退休与 native 权限分离

Pipeline 的旧结构声明、FeaturePoint/Binding、register/find/bind/unbind、contribution与
instance configuration链已物理删除。Compiler直接读取Registry拥有的完整不可变
PipelineDefinition，保原Stage/Artifact重放验证、Phase/scope/capability/版本/可见性、
拓扑和失败不发布部分计划；definition_owner及pin/pending/active/retired/ArtifactWriter/
World技术桥/执行lease继续存在。真实五域Feature身份与contract.domains仍属Engine/Feature。
三个固定Renderer定义删除无业务消费者的空挂点，Stage顺序与Snapshot Artifact保持。

真正native权限从旧挂点拆出为六位RenderGraphBuildFlags，资源权限改名
kMayCreateResources，Snapshot发布仍由Artifact合同验证。RenderGraphBuildContext仅含
flags/context_data，旧作者身份、scope/config/userdata getters删除；实际资源/Pass backend、
NativeSession事务、NativePassSequence/Registry epoch、真正pass/config lease与GPU退休保留。
无调用的contribution Present helper删除，现代lowering体只变更权限/context命名。

实际RED在原owner-boundary World/无ECS写入/编译/shutdown成功后，旧Logic注册/绑定/
展开仍成功，direct73。GREEN测试直接用完整Definition，验证真实Stage、Artifact、owner/
weak lease、失败保旧计划和编译期旧API缺席；ImGui/culling原GPU验证后在已注册的三个
真实Scene Feature定义中查旧身份不存在。三README与handoff§125/ledger同步；构建与
CTest终态事实随后追加，不能据本切片关闭原Tasks1–7验收。formal World outbox、实际
Tooling Module/commit、Demo/callback仍开放，Shader/cache支线依用户指令后置。
统一ALL_BUILD Debug references已exit0，广定向88/88（9.88秒）通过；完整215项使用
四个fresh夹具父目录，原basename、旧目录、命令/属性/工作目录和原负例/恢复守卫保持。
source/test/docs从冷构建前冻结至所有handles终态，完整结论与SHA事实随后追加。

最终完整CTest **215/215（413.18秒）exit0**，所有本批build/direct/CTest handles均终态。
639份冻结source/test/docs inventory与SHA保持至终态，31份完整源码whitespace0、全仓
diffcheck0，两旧header缺席、四原夹具目录和bak SHA保持；现代lowering与前终态仅作
权限/context命名替换后逐字相等。核完后才追加事实，详见handoff§125与final-guards.json。
原Tasks1–7继续active；下一主线是明确绑定的正式World typed outbox生产消费，现fixture
私有consumer不能替代此集成；Tooling Module/commit、Demo/callback和最终验收仍开放。

### 12.89 Feature outbox journal 与 typed command 传输

StateDelta追加OutboxIntent{id/version/owning bytes}，协议复用现SideEffectContract的
kCommand+kOutbox与已有digest，不以隐藏Feature或latest State代替跨owner命令。
Runtime在State/Process/ResourceBus同次提交保留所有pending entry；实际published全域
root/descendant及真实Invocation签发canonical来源，private Binding/Handle不接受caller
重新拼装的token/payload/compiled provenance作授权。空delta和后续commit不覆盖旧项。

每commit/pending1024条、单intent1MiB、pending总16MiB；非法合同或超限拒整个源提交。
consume核canonical pending后调用已完成外部typed预检的noexcept应用函数，无分配写回
Applied receipt；错误预检保Pending，明确reject为Rejected。Process终态不清已接受项；
pending阻止publish/replace并保lastgood，shutdown/destructor明确Cancelled且capture值保活。
命令消耗不依赖latest state/registry；本地journal不表示磁盘持久化或跨域全局原子性。

Module AFR1 magic保留，wire与feature execution API升2，既有serializer追加typed outbox
并共享count/payload界限，整包仍16MiB。旧协议与future版本明确拒绝；C ABI表布局不变。
实际API编译RED后实现Core；新增DTO与原codec重编后实际decoded0/expected2、direct73，
再迁codec。Module2/2、核心4/4与新增真实publication失败guard1/1均通过；后者证明源
owner提交后Render publication错误不撤销/重放pending命令。统一最终ALL/113相关/216完整
结果待终态追加，详见handoff§126及实施依赖计划。

此批是正式World生产consumer的必要前置。真实Game.Transform typed adapter、显式
World/Feature/Scene entity绑定、World实际weak生命周期代次及两种Runtime接线仍待实施；
旧测试私有投影不作生产完成证据。Tooling/Demo及原Tasks1–7最终验收继续，Shader/cache
依用户指令后置。
最终统一ALL_BUILD Debug references exit0，相关113/113（11.05秒）通过；完整216项即将
使用四个fresh夹具父目录，原basename、旧目录、命令/属性/cwd和全部冷构建/负例/恢复守卫
保持。source/test/docs从此冻结至所有full handles终态，最终SHA/完整事实再追加。

首轮完整216终态为215/216（436.73秒、exit8）：唯一失败是新增测试直接使用std::thread，
触发Concurrency边界门禁。642份冻结文件inventory/SHA保持至终态，原日志与守卫记录保留；
不放宽门禁，仅以现有WorkerExecutor改写真实异线程探针后重新构建和验证，结果随后追加。
修正测试中的局部变量重名后fresh构建exit0，真实outbox与Concurrency边界2/2（0.69秒）
通过；生产代码和边界规则保持，线程ID不同/拒绝后Pending/零apply/重试/replay仍有实测。
修复后最终ALL_BUILD Debug references exit0，相关114/114（13.92秒）通过。完整216使用
另一组fresh父目录并重新冻结source/test/docs，首轮失败日志/目录保留，终态后再记最终事实。

修复后完整CTest **216/216（436.82秒）、exit0**。四个TEST_ROOT仅换fresh父目录，精确
registry的216名称/命令/属性/default cwd等价；原basename与负例/恢复守卫保持。全部
handles终态后实核642份冻结文件inventory/SHA不变、9份完整源码whitespace0、全仓diff0、
private outbox边界/两旧header缺席/四原及两轮各四夹具目录/原bak SHA保持。首轮失败
记录保留，核完后才更新事实，详见handoff§126与repaired-final-guards.json。
本批journal/Module传输完成，正式World typed consumer和两种Runtime接线尚未实现；
下一步取得真实typed codec/显式binding/World代次的RED后继续。原Tasks1–7仍active，
实际Tooling Module/commit、Demo/callback和完整五域验收开放，Shader/cache支线后置。

### 12.90 正式 World typed command 生产消费

继journal/Module传输之后，World.Transform/1固定typed adapter位于Engine Runtime private。
World只提供既有actual ECS owner与active token的private weak生命周期校验；无新代次
counter。复用真实Game.Transform/schema1反射对象与Serialization，严格canonical record、
metadata及finite值校验；World/entity由显式route封存。每entry先校验owner/schema/entity/
代次/revision/expected值，再noexcept赋值并确认Feature Applied receipt；失败保Pending，
合法旧pending可继续消费，snapshot后续失败不能重放Applied。固定协议不代表任意组件
事务或跨域原子batch，Feature仍是唯一作者功能身份。

启动纯值binding按graph asset/node/authored instance path/Scene entity/protocol显式配置。
GraphHost投影稳定作者路径，不复制namespaced ID算法；通过实际SceneInstance UUID映射
真实Entity后绑定canonical来源，全部配置成功才安装服务，空列表不消费。Orchestrator
用private借用consumer在每World commit前消费；无renderer路径在external apply之后、
World commit之前复用同一实现。shutdown按Orchestrator→consumer→Scene退休，保owner/
active-frame/drain失败重试。Engine-only FrameHost可借用透传既有preparation hook，
std::cref保apply_frame_desc noexcept；GameSDK边界不增加World/RHI/Concurrency执行接口。

API compile RED后才实现typed codec、作者路径与private hook。真实configured无renderer
启动在producer committed1后仍mutable5/snapshot5，direct73行为RED后才接生产消费。
renderer fixture曾违反externalcolor合同的诊断明确不算RED，原validator未放宽。最终
真实两Runtime路径configured均5→7、未配置保持5，renderer actual extract1；Composite
精准leaf、19组坏配置、异线程拒绝、同handle重新初始化、snapshot失败Applied不重放
和原World/View/Artifact守卫均通过。旧float-only/fixture-private consumer已退休。

最终ALL_BUILD Debug WITH references exit0；相关CTest **139/139（18.62秒）、exit0**，
fresh完整CTest **218/218（383.30秒）、exit0**，全部build/direct/CTest handles终态。
精确218名称/命令/属性/default cwd等价，仅四个TEST_ROOT换fresh父目录，原basename与
冷构建/发布负例/缺DLL/源码SHA/恢复守卫保留。终态后、文档解冻前实核649份冻结文件
inventory/SHA不变、19份完整源码whitespace0/globaldiff0、621份范围外文件不变，两个
旧header缺席、private消费边界/原及两轮各四夹具目录/受保护bak SHA保持。详见handoff
§127、world-feature-commands-final-guards.json和对应实施计划；原失败/诊断证据保留。

本批固定typed消费与两Runtime接线完成，原Tasks1–7继续active。下一实际Tooling缺口是
ProjectOperation的Module归属/commit观测及Main Build/Cook连接；Scene Load/Apply/Inspect
已经接通真实Module/GraphHost，不能重复记作缺失。Demo固定步长直接World mutation、
公开业务callback和旧Demo作者Source仍需迁移；中立命令作者encoder仅完成只读建议，
尚无下一批RED或实现。完整五域验收继续，Shader/cache支线依用户指令后置。

### 12.91 Tooling 真实 Module/Process 观测与 Main Build/Cook

ProjectOperation通过正式ModuleRegistry注册静态FeatureModule，五个Logic Process具有
Engine.Tooling.ProjectOperations归属。Build选择Validation/Configure/Compile；Cook只
Validation/Cook/Publication验证，不启动CMake；BuildAndCook执行五步。退休空manifest
静默Build-only兼容模式，Build缺Engine目录或CMakeLists明确失败，工具路径可显式指定。

状态持有实际Runtime复制的Node/Instance/Process/owner Module、generation、definition
digest、合同副作用和InvocationSnapshot；stage名仅在实际本次commit匹配issued token后
追加。每owner边界将owning快照送Service互斥保护，不跨线程访问Runtime。CMake/Cook
为ExternalIo/ExternalOwner，取消和State提交失败不能撤销外部IO；Cook唯一selector
发布仍由cook_project实现，Publication Process仅验证其实际结果。

cancel请求可先显示Cancelled；execution_complete仅后台实际收尾才true。wait等待执行
空闲后返回真实最终状态，关闭/切换项目与退出都cancel/wait/join，不依赖操作窗口可见。
已接受的Module、Process、commit与已发布产物事实不被取消清空。queued task保原必达
收尾机制，Runtime owner仍只在Tooling worker线程执行与观测。

Main菜单Build/Cook窗口与--project-operation命令行共用Editor-private controller和真实
Service；项目来自准确ProjectManager文件，target来自module.target。Engine/CMake/
configuration/module manifest/output明确输入；操作使用已保存文件，不隐式混用draft，
不猜最新bundle。原Scene Load Module/Apply/Inspect已经接通真实Loader/Registry/Host，
本批复用其既有职责，不新增作者Pipeline/Pass或向GameSDK暴露Engine执行能力。

缺manifest实际direct73、观测/准确项目/wait/controller的API RED及旧Main合法Cook无
selector的行为RED均保留；Core/UI/Scene与Main Cook已转GREEN。另取得缺Engine/CMake
成功及运行中无commit三个真实oracle的direct73后才修生产，三oracle最终全1，取消保
同commit。source-only CXX项目初始无bundle，由Main实际编译新DLL、metadata与Cook，
Runtime加载该新DLL并完成Process commit，单项1/1（55.30秒）通过，源SHA保持。
最终ALL_BUILD Debug WITH refs exit0，相关 **187/187（117.16秒）、exit0**；14份完整
源码whitespace0/globaldiff0、原bak SHA保持。fresh完整 **220/220（429.72秒）、exit0**；
全部handles终态后实核654份冻结source/test/docs inventory与SHA不变、634份范围外
文件不变，两个退休header缺席、private边界/五原+五fresh+四前轮夹具目录/原bak SHA
保持。220名称/命令/属性/default cwd等价，仅五个TEST_ROOT换父目录，保原冷构建/
发布负例/缺DLL/源码SHA/恢复守卫；终态核完才解冻文档，详见handoff§128、Tooling
实施计划与tooling-project-operations-final-guards.json。原RED/诊断日志保留。

原Tasks1–7与完整五域验收继续active。同项目新DLL在Main Load/Apply/Inspect的联合
验收、生成业务stub、Demo的直接World mutation/旧作者Source及任意业务callback仍需
继续迁移；不以本批工具操作闭环替代整个作者工作流。Shader/cache支线继续后置。

### 12.92 Main 作者工作流联合验收与显式保存批次

前节的同一 Main 新 DLL 联合验收已在 handoff§129 关闭。仅测试构建包含的 ImGui 输入
驱动借用实际 Main Scene/document/operation，经真实 UI 和已有 workspace action 完成
源码 Build/Cook、Load Module、Inspect、Play/Pause、Process 状态/转移编辑、Save、Apply
和再次 DLL commit。Inspect 来源是 Main 实际 callback，按钮 pending 在 UI 后核实，
新代次在下一实际 Scene 帧后核实；Save 不更换 active，旧 owning 快照保活。Cook 早于
作者编辑，其原图包独立 Runtime 启动/commit，不宣称 Save 自动更新已有 Cook 包。

合同升级另由显式 SavedStartup 策略处理：准确项目文件的全部保存 startup roots 构成
新的候选批次，空列表拒绝、不合并旧根、不回退、不放宽 digest/version；坏根保留旧批次。
默认 PreserveActive 继续使用已发布图并严格拒绝合同不符。新 Load Saved Project 动作
经 Main 连接该策略，不替换作者 draft/catalog/Undo。真实 Scene＋Panel V1/V2 DLL 测试
证明 First+Third 替换 First+Second、保存参数与 dirty 参数隔离、同一新代次及实际 V2
状态1→11。Main 联合测试运行首次构建 DLL，未冒称同一 Main 内的完整二次构建热替换。
发布仍为 Scene 帧边界；不承诺 Renderer/GPU 后续失败时回滚旧 session。

策略/Panel API compile RED、保旧实现 stale digest 的行为 RED、空 startup 的错误接受
RED、旧 Main Build/Cook 成功却缺联合工作流的行为 RED均保留。首次空列表 NoRefs
SegFault 未到 oracle，不计有效 RED；刷新依赖后准确到达空列表断言，随后才改实现。
定向2/2（53.30秒）与最终 ALL_BUILD Debug WITH refs exit0；fresh完整221/221
（484.04秒）exit0。仅六个TEST_ROOT父目录变化，名称/命令/属性/default cwd等价。
全部handles终态后核658冻结文件SHA、642范围外文件SHA、本批10源码空白/私有边界/
退休header/旧与fresh夹具/原bak SHA，全部保持；核完才解冻文档，证据见
editor-module-workflow-final-guards.json。未暂存、提交或新增 worktree。

原Tasks1–7、Demo业务 callback、生成stub与完整五域交付继续开放；同路径作者 catalog
变化仍需显式合同 diff/impact/accept。下一主线应先证明实际 World revision/before
Transform/elapsed 输入投影，再迁持续 Motion，不能用固定初值或 Feature input_version
冒充 World 观察，不能向 GameSDK 暴露 World 或 Runtime-private codec。Shader/cache后置。

### 12.93 World owner 观察输入与持续 Motion 的 Runtime 闭环

World 数据 owner 通过显式输入 port向 Logic Process 提供不可变值观察。公开中立协议
World.Transform.Observation/1 包含实际 World revision、before Transform、逻辑 clock
quantum、当前 admission 的 host frame delta和原 Channel bytes；direct elapsed为0，
retry沿用原 owning bytes。World.Transform/1 命令沿原 canonical reflected wire，
schema/finite/canonical/budget校验不变。唯一 Transform schema factory由独立协议库复用；
GameSDK获中立值与link-only codec，host admission与World consumer保持private。

首次 invocation admission在token发行前调用私有 projector，ready=false保原 Channel、
不发行attempt；deferred/backlog会在实际发行时再投影。busy/suspended先拒绝，marker lease
按真实owner/gen/digest/port/schema校验，过期拒绝、不静默回退，旧代或已销毁Runtime无回调。
逻辑elapsed不能使用整个host delta重复积分，也不能以Feature input_version代替World revision。

显式 Runtime绑定仍精确选择graph/node/authored path和稳定Scene实体。consumer先预检
整个scope真实committable open/retry，观察未退休前不提交World；canonical Pending与本批
cohort阻止新观察。全scope成功才释放cohort，含空outbox；partial failure保所有latch及真实
Applied/Pending receipts。World实体/代次/schema/expected-value校验保持；外部命令在观察后
造成冲突时保Pending，不自动改写before或放宽规则。

Renderer/headless沿既有WorldCommit消费。只有明确的consumer observation wait被Runtime
接受为未完成帧，Renderer须已退出actual frame；其他错误仍返回。frame identity来自实际
backend，completed-frame预算单独统计；run_game等待不消耗max_frames，并使用Concurrency
让出执行。此规则不承诺Renderer/GPU后续失败时回滚已经提交的World。

实际package、Clock、World、异步worker与Renderer的两波测试证明5→7、owner改为41后再到43，
revision1→2→3→4、正确输入decode和旧owning快照保活；普通路径无external failure gate。
实际入口loop核两帧和第二次before7/revision2。独立测试覆盖Channel、deferred/backlog、
retry原输入、bind前Pending、多路partial failure、lease、SDK与Concurrency边界。

API/缺输入/提前World提交/Renderer frame错位/max_frames耗尽/direct多余投影的RED保留。
首次完整226在Concurrency接口和合成测试tick处失败，保持gate/预算/断言后修复。
最终定向9/9（1.95秒）和ALL_BUILD Debug WITH refs exit0。第二完整225/226（454.45秒）
唯一失败在Main的MSVC compiler probe，记录MSB6003缺tlog目录，尚未进入Module配置；
未证明MAX_PATH越界。源码保持不变，仅该case以短fresh父目录冷重验1/1（57.92秒）
exit0。全226 case在同一冻结源得到通过结果，不冒称第二轮单次完整CTest exit0。
666冻结文件SHA到全部终态不变，18源码空白、644范围外文件、私有边界/退休header/
原与两轮fresh及独立复验夹具/原bak守卫保持，详见handoff§130及repaired-final-guards.json。

原Tasks1–7继续active。下一步是Editor显式绑定、Play/Pause/Stop/rePlay/resize生命周期及
Play中Pending＋Apply的边界排序；当前Edit组件查询不能当作Play World证据。随后迁Demo/
业务callback并退休旧直接writer；Shader/cache支线继续后置。

## 2026-10-17 Demo Motion 迁移

Demo 的 `GameModule` 现在以 `com.aeg.demo` 注册真实 Logic/Process Feature，并由
`Assets/Features/Motion.aegfeature` 作为 SavedStartup root。模块从显式绑定的
`World.Transform.Observation/1` 输入读取 Transform 与 revision，更新模块私有 phase，
再通过 `World.Transform/1` Outbox 发出带 expected revision 的写命令。Editor 旧的
`advance_runtime_demo` 直接写入路径已删除，避免与 Feature callback 双写。

独立 Demo CMake module 构建和实际 DLL 动态加载 smoke 均通过；Editor Play 改变运行态
Transform、Stop 后 Edit 快照保持不变。显式 Scene binding 目前由 Editor-private API
配置，作者可编辑/保存的路由 UI 仍待 Task 6 完成。完整 Tasks 1–7 验收未完成；下一步先做
同路径作者 catalog 的显式 diff/impact/accept，再继续 Task 5/6/7。Shader/cache 治理后置。

## 2026-10-17 Module catalog 审阅

Editor 加载 Module 前可对比当前活动 bundle catalog 与 manifest 候选 catalog。差异按稳定
Feature Definition ID 分类，并标出字段、被引用的已保存 FeatureGraph、node 与 incident edge。
存在差异时先审阅再显式接受加载；加载前复查摘要，过期审阅不会发布候选。活动 Runtime
session 与 Source authoring refresh 保持分离，图内容和 Undo 历史不会被审阅操作改写。

接受沿用既有 SavedStartup/Scene 帧边界发布与严格编译校验；审阅不自动迁移参数、删除节点或
重连边。完整 Tasks 1–7 验收和后续 graph migration UX 仍开放，Shader/cache 治理继续后置。

## 2026-10-18 Editor Scene World route 作者入口

FeatureGraph 面板现在把当前已选 authored node 与 Hierarchy 中已选 Scene entity 接到一起，显示
现有 World route，并允许作者显式绑定或移除。绑定只接受唯一的已发布 owner，且其 contract 必须
声明 `World.Transform/1` command Outbox；canonical `World.Transform.Observation/1` input port
会从 contract 读取。多 owner 展开节点当前明确拒绝，不能靠猜测选择子实例。route 修改只允许在
Edit 模式。Asset 共享纯 source/contract validator 与既有 Editor live validator 校验稳定 graph/node/owner/entity 身份。

route 元数据保存在相邻的 `.aegworldbindings` Editor sidecar，而不是 Engine `SceneAsset` 或
FeatureGraph 作者格式。Scene Save / Save As 同步当前 route 集；打开时先校验 sidecar 版本、长度、
Scene identity、Transform target 与活动模块 contract，再替换当前 Scene。新场景覆盖保存会写入
空 route 集；另存副本保留原 route。route 编辑/移除立即写 sidecar，Save Scene 同时再次同步。
sidecar 是作者数据；共享 Asset codec 使用 AGWBIND1/schema2，Scene/entity 保存 canonical UUID 字节，graph 保留原字符串。
Cook 每次包含 warm cache 都读取准确 startup Scene 邻接 sidecar，先验作者 Scene ID，再将输出 Scene ID 重定位为 product UUID；
按最终 package 的 Scene/entity/Transform 与完整 graph/node/owner/contract 验证，ScenePayload 和 Transform 版本/载荷复用正式协议校验。
成功后写入严格 schema3 的 GameBuildManifest；失败保 selector/last-good/已有 immutable cache。
Bootstrap 只接受一次 manifest 快照，私有转换到现中立 Runtime DTO；manifest 与 host 路由双非空拒绝，双空仍 Pending。
完整绑定在首次 Game start/producer callback 前完成；失败同对象可直接恢复，复用既有 consumer。路由共同预算16MiB/4096条/owner256/graph4096 bytes，manifest总64MiB。

测试从实际 ImGui 面板点击 Bind/Remove，验证非 Edit 门禁、scene 切换回读、Save As 保留 route、
同路径新 Scene 保存清除旧 route；Demo Motion、World binding 生命周期、Scene module/catalog
和 project authoring 定向 CTest 共 4/4 通过。仅唯一 published owner 已有作者入口；嵌套 Composite
多 owner 选择是后续UX；Shipping packaging现已通过真实UI Save→Cook→隐藏源树/独立交付→空request→动态DLL目标5→7、旁观23及before/revision验收。
本切片完整232/232、606.37秒、exit0，700份冻结文件SHA保持；原Task5/6/7剩余要求仍开放。Scene与sidecar分别原子替换，没有两文件单一事务提交承诺。

## 2026-10-18 参数资源种类与消费验证

AssetConstraint 可选择 Foundation AssetKind；Any 放宽种类，但在 project/package resolver 存在时
仍验证 UUID 实际存在。合同纯 schema 校验不依赖文件系统，host 校验处理存在性、kind、null 和
resolver 异常。kind 进入语义 digest，旧无 kind 的 JSON 与 digest 保持；UUID payload 不变。

Editor 的作者动作以所选项目配置的资产目录及 `.meta` product UUID 为权威，Apply、Definition
保存与 Graph Save 校验实际引用，并覆盖 Composite defaults。Save Draft 允许 required 缺值；
失败保持原文档、历史和文件。运行 FeatureSession 使用创建时接受的不可变目录，普通 Refresh
不隐式改变它；新 session 重建，显式运行目录迁移仍待完成。

FeatureRuntime 在候选发布、callback/retry 和 replacement 前验证有效参数，包含 descendants 和
默认值，失败保留 live generation/retry。RuntimeBootstrap 从 verified package 解析真实种类。
Cook 在 warm/cold cache 后统一校验，并把所有有效 AssetReference 加入正式依赖闭包，不改
immutable cache；打包或类型失败不替换 last-good 交付。

当前20项定向回归通过，含真实 ImGui、Main、package/Bootstrap 恢复和既有 World/Demo。Task 6
仍需安全的参数 type/schema payload 迁移、固定宽度与范围/初值边界；Tasks 1–7 全量验收继续开放。

## 2026-10-18 参数 schema 显式迁移与作者初值

Definition Generate 现在先用旧 schema 解码，再执行作者明确选择的 Convert / Reset，最后以新 schema 编码和验证。
type 或 typed/opaque 切换要求 Parameter version 增加与 Definition major 增加；opaque schema 换版要求 Reset，版本后退拒绝。
移除参数要求显式 Discard（执行 Reset 丢弃旧记录）与 Definition major 增加；同 type 的兼容值继续保留。
所有受影响 loaded documents 的旧定义 version/digest 与 node definition_version 必须一致；缺失或 partial/summary-only baseline 拒绝。
breaking edit 继续扫描配置资产目录；大小写扩展名一致识别，未打开的引用图拒绝，不能借 uppercase 文件绕过用途检查。
codec 解码会物化旧 defaults，因此保留的是旧有效值；Undo/Redo 同时恢复 graph/blob/catalog，不回滚生成文件或 active Runtime。

固定 wire 最小宽度即使没有 default 也验证；整数范围先检查可表示性，避免初值转换溢出。
Unit vector 仅用 norm 上下界证明部分区间无解，不声称完成带 step 的离散可行性判定；String/Enum 的 wire 最小宽度覆盖长度前缀，不证明完整约束可满足性。
作者控件提供安全 typed seed 与有界自动初值 API；自动搜索返回 Unsupported 时保留手填路径，不把搜索失败当 schema 无效。
非空 vector 编辑不截断已有分量；不兼容维度由正式 validator 拒绝，不能靠控件静默裁剪修复。

最终第二轮完整 CTest 为 230/230、536.90 秒、exit0，日志 `build/parameter-schema-final-full-ctest.log`。
此前 ALL_BUILD exit0（`build/parameter-schema-final-all-build.log`）；随后仅修 fixture Concurrency 并重建两个消费者（`build/parameter-schema-concurrency-targets-build.log`）。
修复后定向 9/9、6.86 秒、exit0；第一轮完整 229/230、550.78 秒、exit8，唯一失败为 Concurrency gate。
fixture 改用项目 sleep 封装并补 private link，未放宽 gate；首轮失败证据保留。
全量终态后、文档更新前，692 个冻结文件与 13 份实现 SHA 全保持，archive SHA 保持，diffcheck0 / staged0。
最终结果与冻结守卫见 `build/parameter-schema-final-full-result.json`；这些结果代表本轮冻结源码，不能关闭原 Tasks 1–7。

下一优先验证真实 UI 首项目的 Feature owner / Generate → 编译 → metadata → 加载；空/Builtin catalog owner 当前仅静态审计，尚无行为证据。
手动 CMake / 注册属于已有设计，不能直接认定为 bug；随后贯通 `.aegworldbindings` → Cooked manifest → Shipping，复用既有精确 consumer。
原 Tasks 1–7 仍开放；无 stage / commit / PR / new worktree，Shader/cache 生命周期支线继续后置。

## 2026-10-18 首项目 Feature owner 与生成 DLL 回流

New Feature 与保存动作均 fresh 读取明确选定项目的 `module.id`，先拒空 id；不从空/foreign catalog 或 CMake target 猜 owner。
项目扩展名大小写统一识别；多项目文件无明确选择、跨根、项目失效和窗口打开后身份变更均 fail closed，保留旧文件与作者状态。
项目发现逻辑与资产目录解析共用；没有 module 的合法资产 lookup 仍保留，不将 Definition owner 门禁扩散到纯资产查询。
真实空/foreign catalog 的 owner 行为 RED 已取得并修复，实际 ImGui 回归通过。
生成仍只安装四份输出；开发者显式接入 CMake、调用注册 helper、维护 source metadata 与构建模块属于现有设计。
新增 cold 夹具从真实 UI 输出接入 GameSDK DLL，逐值验证作者合同、metadata、实际 DLL 的 ID/version/owner/digest 一致。
实际 DLL stub 返回 Unsupported；hash 有效但合同错误的独立负包被拒绝，这证明接线与合同一致性，不代表游戏业务实现完成。

ALL_BUILD exit0：`build/feature-owner-final-all-build.log`；定向 10/10、3.76 秒、exit0。
最终完整 CTest 231/231、600.13 秒、exit0（`build/feature-owner-final-full-ctest.log`），新增 UI→cold SDK DLL 集成为 47.54 秒、exit0。
全量终态后、文档更新前，694 个冻结文件与 7 份实现 SHA 全保持，archive SHA 保持，diffcheck0 / staged0；见 `build/feature-owner-final-full-result.json`。

`.aegworldbindings` → Cooked manifest → Shipping已接通并完成232项全量；共享codec已统一writer/reader预算与版本，Demo经正式codec重编码。
下一主线核configured source刷新、Structure/Scope筛选与跨域约束展示、实际Render资源合同一致性；静态候选仍须先行为RED。固定Source/FlowGraph契约目录保持。
原 Tasks 1–7 仍开放；无 stage / commit / PR / new worktree，Shader/cache 生命周期支线后置。

## 2026-10-18 原主线最终验收

原 Tasks1–7 范围现已完成。上文逐次进度、失败与待办保留为历史；当前逐项结果见
[最终验收矩阵](docs/superpowers/plans/2026-10-18-five-domain-final-acceptance.md)。

最后六收尾包括精确配置source刷新、Structure/Scope筛选、已保存跨域边约束、实际附件一致性、
Clustered concrete owner epoch和Call continuation拒绝。随后补齐显式Clock版本/phase/deadline/
policy/budget、host能力准入、实际重复owner/combined roots预算、同源单调期限、Receipt原期限、
phase整数调度与真实IO/World等待时Audio独立推进。公开声明不授予host能力，预算不承诺硬实时。
metadata/GameSDK contract major3；Module ABI major2、Feature execution API/AFR1 v2。

边期限保留发布图时作者source Clock的周期快照，重新编译发布更新。Process的候选仲裁与
Exit→State→Enter保持Feature私有纯prepare；唯一owner原子提交后才由真实consumer消费Outbox。
原§5.2三层确定性选择、完整64位身份、异步提交及四类非空journal回滚已有11真实Runtime用例。

最终 normal Debug ALL_BUILD通过；关键11项各3次通过（15.16秒）。最终完整238/238、556.92秒、
exit0、零跳过，七组SDK/项目使用fresh短根，完整registry名称/命令/属性/cwd/桌面互斥等价。
2,682份source/test/docs/reports在原句柄49298终态前SHA及库存保持；archive/bak保持、
diffcheck0/staged0，核实后才更新本节。证据`build/clock-mainline-final-full-result.json`。

首完整236/237的唯一失败是UI夹具在LogFinish清空后取日志；实际六控件均已渲染，最小调整
读取顺序后fresh UI→SDK DLL与最终完整验收均通过。旧失败记录保留。取消不强杀callback/IO，
跨文件/五域不承诺全局事务。任意Shader/cache生命周期、多owner Bind UX和活动资产目录迁移
按已接受范围后置；无stage/commit/PR/new worktree。
