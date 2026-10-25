# AEG FlowGraph：作者与执行契约

[返回架构总览](<../AEG FlowGraph Architecture.md>)

本文是 FlowGraph 的**规范性分册**。如果其他专题分册中的展开描述与本文冲突，以本文为准；其他分册负责术语、画布体验、代码工作流和验收场景的具体说明。

## 1. 设计目标和规范用语

为统一作者模型，原有描述容易把“有端口”“有连线”“有输入”“数据已准备”“节点已经完成”混在一起。本文把它们拆开，使编辑器、Compiler、Cooker、Runtime 和自定义节点使用同一套定义。

- **必须**：实现和文档不得违反。
- **应该**：首期按此实现；如果延期，必须显示为未支持能力。
- **可以**：可选增强，不改变核心语义。

Authoring Contract 只描述稳定的作者语义，不描述 GPU Handle、函数地址、临时编译索引或具体驱动对象。

## 2. 四阶段模型

一份 Feature 作者资产按其 profile 经过四个阶段，阶段之间不能互相冒充。Feature Graph 是
唯一作者模型；Frame/Render 可从中派生 Pipeline/Feature/Pass 执行投影，Logic/Process 使用
Feature/State/Transition 子视图，其他域使用各自的 ExecutionPlan。

```text
作者图 Author Graph
    ↓ 解析类型、绑定、子图和版本
准备图 Prepared Graph
    ↓ 分配逻辑资源版本、阶段、队列和同步计划
执行图 Execution Graph
    ↓ 绑定当前 Frame/Tick/Event 的资源、状态提交和完成事件
运行观察 Runtime Inspect
```

### 2.1 作者图

作者图保存节点实例、输入绑定、数据边、子图引用、参数、资源声明和布局。它可以保存草稿错误，但不能把没有来源的必需输入标记为可编译。

### 2.2 准备图

准备图完成类型解析、Asset/Cook 解析、子图展开、资源逻辑版本分配、Scope/Queue/Format 检查和副作用根标记。它仍然不保存具体 GPU Handle。

### 2.3 执行图

执行图把准备图映射到当前目标的 CPU/GPU 阶段、队列、资源状态转换和完成事件。它是派生物，不能在 Runtime Inspect 中编辑。

### 2.4 运行观察

Runtime Inspect 必须可以从实际节点回溯到：GraphVersion、调用实例路径、NodeId、PortId/SlotId、逻辑资源版本、模块 BuildId 和 ContractDigest。没有来源映射时显示错误，不假装是引擎自动生成。

## 3. Authoring Contract

### 3.1 节点类型身份

每个节点类型必须有：

| 字段            | 规则                                                     |
|-----------------|----------------------------------------------------------|
| TypeId          | 稳定身份，不随显示名和文件名改变                         |
| ContractVersion | 端口、参数、资源、执行和副作用语义的版本                 |
| OwnerModule     | 提供契约和运行实现的模块                                 |
| DefinitionKind  | Code、GraphAsset、System、Resource、External、Flow、StateMachine、RenderPass |
| GraphKinds      | 可出现在 Frame/Render 的 Pipeline、Feature、Pass，或 Logic/Flow 的 Feature、Flow、State、Transition 哪些作者图 |
| Domain/Scope    | 所属领域、调用范围和可见边界                             |
| Documentation   | 用途、输入来源、性能、限制、失败原因和示例               |
| SourceMap       | 工程相对文件、模块目标、符号和构建目标；禁止保存绝对路径 |

显示名相同不代表类型相同；TypeId、OwnerModule 和 ContractVersion 共同参与目录查找与迁移。

### 3.2 端口、边和来源是三个不同概念

每个端口独立声明：

1. **PortId**：端口是否存在以及稳定身份。
2. **EdgeCardinality**：允许几条普通数据边，固定端口可以是 `0..1`、`1..1`，Variadic 组可以是 `0..N`。
3. **AllowedSources**：允许 Edge、Asset、Literal、Context、Default 中哪些来源。
4. **PresenceRequirement**：来源是否必须有效，还是 `None` 也是合法业务状态。
5. **BindingSchema**：AssetId、资源类别、格式、目标变体和内容版本等外部绑定字段。
6. **ValueSchema**：稳定 SchemaId/SchemaVersion、元素布局、单位、可空性、颜色/坐标空间和序列化规则；显示名不参与类型兼容。
7. **FallbackPolicy**：是否允许用户显式切换来源；禁止引擎隐式猜优先级。

其他分册和界面文案中的 `SourcePolicy` 是这一组规则的简称；Authoring Contract 的规范字段名为 `AllowedSources`，两者不能作为两份独立配置。

`EdgeOrAsset` 是一个常用的界面预设，不是把两条来源同时激活。实例必须有一个 `ActiveSource`：

| ActiveSource | 是否创建普通数据边 |             是否产生上游执行依赖 | 典型用途                                            |
|--------------|-------------------:|---------------------------------:|-----------------------------------------------------|
| `Edge`       |                 是 |           是，源结果先于目标消费 | 动态 SceneColor、Pass 输出                          |
| `Asset`      |                 否 |           否；只产生资源准备依赖 | 静态纹理、LUT、材质资产                             |
| `Literal`    |                 否 |                               否 | Float、Color、Enum 常量                             |
| `Default`    |                 否 |                               否 | 节点契约提供的默认值；与用户填写的 Literal 分开记录 |
| `Context`    |                 否 | 否；由当前 Frame/View/World 提供 | 相机、帧上下文、世界快照                            |
| `None`       |                 否 |                               否 | 仅当 PresenceRequirement 允许为空                   |

必需输入是“执行前必须得到有效来源”，不是“执行前必须有一条线”。输入状态必须分开显示：

`Unset → Bound → Pending → Ready`，失败时进入 `Invalid`。`AssetId` 已填写但仍在导入、Cook 或设备准备中，只能是 `Pending`，不能提前视为 `Ready`。

### 3.3 输入绑定数据

每个输入实例保存：

```text
InputBinding {
    PortId
    ActiveSource
    EdgeIds[]
    AssetId / ProductId / TargetVariant
    DefaultId / DefaultRevision
    LiteralValue
    ContextKey
    ContentRevision
    BindingStatus
}
```

`EdgeIds[]` 只引用真实数据边；Asset、Literal 和 Context 不伪造 NodeId 或执行边。输入从 Edge 切换到 Asset 时，必须在同一事务中解除旧边或将其放入未激活编辑缓存；运行时只读取 ActiveSource。

`Default` 读取节点契约中带版本的默认值，`Literal` 读取实例中保存的用户值；二者都不产生上游执行依赖，但默认值变化必须提高契约版本或使实例的语义摘要失效。活动来源失效时不得自动从 Edge、Asset、Default 之间切换，除非节点契约声明了可由用户确认的 `FallbackPolicy`。

来源状态的合法迁移为 `Unset → Bound → Pending → Ready`，解析或设备变化可以从 `Ready` 回到 `Pending`，失败进入 `Invalid`，用户清除绑定回到 `Unset`。`Cancelled` 只表示本次准备/执行被取消，不等于可消费的空值。

### 3.4 Variadic 和 SlotId

Variadic 不是通用加号，而是节点类型定义的输入组。每个槽位有稳定 `SlotId`，并声明：最小/最大槽位、成员端口、是否有序、空集合语义、每槽来源模式、删除行为和合并算法。

槽位显示序号可以变化，SlotId 不能随排序变化。槽位数量、数据边数量和数组元素数量分开校验；一根边传入数组不自动产生多个槽位。

## 4. 数据边、执行完成和同步

### 4.1 普通数据边的唯一含义

`A.Output → B.Input` 同时表示：

1. A 的指定结果是 B 的指定输入来源。
2. B 只能在该结果满足其完成条件后消费并执行。

系统不得创建只表示顺序、却不携带数据的作者边。旧 `Ordering` 只能作为迁移输入，不能由新编辑器写出。

### 4.2 完成点按执行域定义

“节点完成”必须声明它证明的执行域和阶段：

| 执行域              | 完成条件                                      | 后继可以依赖什么             |
|---------------------|-----------------------------------------------|------------------------------|
| CPU 值计算          | CPU 结果写入不可变结果对象并发布              | 结果对象和 CPU 数据依赖      |
| GPU 同队列          | 命令录制并由图计划插入资源状态/可见性依赖     | 对应 GPU 资源版本            |
| GPU 跨队列          | 源队列 signal、目标队列 wait 和资源可见性成立 | 目标队列可读取的资源版本     |
| GPU→CPU Readback    | 拷贝完成事件/fence 已满足，CPU 映射合法       | Readback 结果对象            |
| Asset/Cook          | ProductId、ContentRevision 和目标格式准备完成 | 只读外部资源版本             |
| External SideEffect | 外部系统返回声明的 Receipt，且调用身份匹配    | Receipt 代表的具体副作用结果 |

`ExecutionToken`、`WriteReceipt` 和 `ReadbackReady` 必须带 `CompletionDomain`、`CompletionStage`、调用代次和结果身份。不能把“CPU 已经录制命令”自动解释为“GPU 已完成”，也不能把“文件路径存在”解释为“纹理已经可采样”。

每个可被连线消费的输出端口或 `ResourceValueVersion` 也必须带自己的完成域、完成阶段和结果状态；不能只在节点级别记录一个完成点，因为同一节点可能同时产生 CPU 值、GPU 资源和异步 Readback。

### 4.3 数据依赖和内存依赖

数据边产生作者层的先后依赖；Compiler 必须根据资源访问生成实际执行依赖和内存可见性依赖。对于 GPU 资源，执行先后本身不足以保证写入可见；必须同时处理队列交接、访问范围和资源状态。作者图不暴露屏障节点，但 Runtime Inspect 要显示由哪条数据边、哪种 Access 和哪个资源版本产生了同步。

### 4.4 执行实例、结果状态和取消

同一 GraphAsset 可能在不同 View、World、Tick、Event 或调用路径执行。每次实际调用必须
生成稳定的 `ExecutionInstanceId`，由 `GraphAssetId + GraphVersion + InstancePath +
InvocationDomain(Frame/Tick/Event) + InvocationOrdinal + TargetVariant` 组成；Frame profile
可附带 `FrameId + ViewId + WorldId`，Logic/Flow profile 必须附带 `WorldId/TickId` 或
`EventId`。跨帧流程另有稳定的 `FlowInstanceId + StateVersion`，不能用
`ExecutionInstanceId` 代替。一次调用的每次准备/执行重试另有递增 `AttemptId`；重试不能改变
逻辑调用身份，异步结果必须同时匹配 ExecutionInstanceId、AttemptId、GraphVersion、
SemanticDigest，并在 Flow 场景核对 FlowInstanceId/StateVersion。`ResourceValueVersion`、
完成事件、异步请求和 Runtime Inspect 都必须带正确身份，不能只用 NodeId 或帧号。缓存使用
单独的 `CacheScopeKey`：只有节点声明纯度、作用域和输入允许跨调用共享时，才可以省略
`InvocationOrdinal`；有状态或副作用节点必须使用完整调用身份。

运行结果必须区分：

| ResultStatus | 语义                         | 下游行为                                   |
|--------------|------------------------------|--------------------------------------------|
| `Ready`      | 结果已满足声明的完成点       | 可以消费                                   |
| `Invalid`    | 节点失败或结果校验失败       | 默认 fail-fast；依赖它的节点变为 `Skipped` |
| `Cancelled`  | 本次准备/执行被取消          | 不得当作 Default、None 或旧结果            |
| `Skipped`    | 因条件、裁剪或上游失败未执行 | 只有端口契约明确允许时才可产生默认输出     |
| `Partial`    | 节点声明了可部分消费的结果   | 必须带分片/范围和明确的消费规则            |

预览、准备和应用都有取消代次。取消后，尚未提交的资源和副作用必须释放或标记为待回收；已提交的外部副作用不能假装回滚。重试策略由节点契约声明，默认不自动重试。

## 5. 逻辑资源版本和写入规则

### 5.1 逻辑结果不是单一 ResourceId

资源身份分两层：

- `LogicalResourceId`：作者可理解的 SceneColor、BloomTexture、ExternalTarget。
- `ResourceValueVersion`：某个生产者在某次图调用中产生的逻辑结果，如 `SceneColor@current`。

物理 GPU 内存可以复用，但只有在所有旧版本读者结束后才能复用。内存复用不能改变作者图中的逻辑版本。

### 5.2 访问冲突

每次资源使用都声明：`Read`、`Write`、`ReadWrite`、`Consume`、范围、队列、阶段和所有权。

- Read-after-Write：必须有生产版本和可见性依赖。
- Write-after-Read：旧读者结束前不得覆盖，除非显式声明新版本或复制。
- Write-after-Write：必须有明确顺序或报冲突。
- 多个只读消费者：可以共享，但各自仍引用同一逻辑版本。
- 原地修改：必须由节点契约声明输入版本和输出新版本；不能用同一个 ResourceId 隐式覆盖。

首期按整张纹理/整段 Buffer 做范围检查；mip、layer、buffer range 的细分属于后续能力，但字段不能被设计成无法扩展的单一布尔值。

### 5.3 外部资源

外部纹理、Swapchain、History、导入 Buffer 必须声明：来源所有者、读写权限、初始状态、每帧/跨帧生命周期、目标变体和交接方式。静态 Asset 默认只读；外部写入必须有 SideEffectClass 和 WriteReceipt。外部资源不能仅凭同名或路径自动合并。

History 资源还必须有 `HistoryKey`、生产帧、消费帧和失效原因。图版本、目标、尺寸、格式、世界或 View 改变时，必须声明保留、清空或重新初始化策略；不能把上一版本的 History 当作当前帧的普通 Ready 结果。

## 6. 图层、子图和执行边界

### 6.1 编辑层与执行层分开

在 Frame/Render profile 中，Pipeline、Feature、Pass 是作者视图层；Stage、Phase、FeaturePoint、Part、ExecutionDomain、Queue 是执行约束层。一个 Render Feature 黑盒可以展开为多个 Pass，但不会自动创建新的全局调度器。Logic/Flow profile 的作者对象是 Feature、Flow、State、Transition、Event 和 FunctionCall；FlowInstance、StateStore、OwnerPath 不能在展开时丢失，FunctionCall 也不能被当作独立 Pass。

### 6.2 子图接口

Graph Asset 使用 `GraphAssetId + GraphVersion`。每个公开接口端口还有稳定 `InterfacePortId`、方向、ValueSchema 和兼容迁移规则；父图节点保存公开端口的快照和调用实例路径，不能按显示名或数组位置猜测接口。内部 NodeId 不能直接跨边界引用。父图只能接 Graph Input/Output 暴露的接口。

编辑态另有单调递增的 `DraftRevision`；布局变化只增加 `LayoutRevision`。只有通过校验并发布的语义快照才产生不可变 `GraphVersion`。Prepared/Applied 快照必须绑定 GraphVersion 和 DraftRevision，不能把未发布草稿或只改布局的版本混用。

展开必须维护调用栈，拒绝自身递归和环形资产引用。发布版本不可变；修改共享子图生成新 GraphVersion，调用实例通过迁移预览升级。缓存键必须包含完整的调用版本路径。

### 6.3 跨领域和跨阶段

连接合法性必须同时检查 Domain、Stage、Part、Scope、Queue 和资源完成点。首期 Render profile 只开放同一 Render Pipeline 内可证明的跨 Pass/Feature 数据流；其他领域通过版本化 Artifact 或明确的 CPU/GPU Bridge 连接。Logic/Flow 的 Transition、Guard、Action、Cancel 和 Resume 使用带身份的 Event/Receipt/StateDelta 语义，不能用普通 DataEdge、Pass 顺序或空 Ordering 线替代。不能因为两个端口的 DataType 名称相同就跨领域直连。

### 6.4 旧 Ordering 迁移

作者模型区分：

- `DataEdge`：FlowGraph 新语义，携带数据和先后。
- `FixedConstraint`：引擎阶段、Part、Queue 等固定约束，不是作者普通连线。
- `LegacyOrdering`：只读旧资产迁移记录，新图禁止创建。

旧 Ordering 有合法业务数据时迁移为 DataEdge；没有业务数据但节点声明 Completion/Receipt 时迁移为显式完成数据；无法表达时保留迁移诊断并阻断新编译。保存成功的新 FlowGraph 文档不得继续写出纯 Ordering 边。

## 7. 持久化、摘要和资产状态

### 7.1 作者文档最小字段

作者文档必须能稳定保存：

| 类别     | 必需身份/内容                                                                                         |
|----------|-------------------------------------------------------------------------------------------------------|
| 图资产   | GraphAssetId、GraphVersion、DraftRevision、LayoutRevision、GraphVariantId、SchemaVersion、OwnerModule |
| 调用实例 | InstancePath、父资产版本、局部 NodeId                                                                 |
| 节点     | TypeId、ContractVersion、NodeId、参数、启用状态                                                       |
| 端口     | PortId、ActiveSource、InputBinding、公开接口映射                                                      |
| Variadic | GroupId、SlotId、SlotOrder、槽位参数                                                                  |
| 边       | EdgeId、源/目标 InstancePath + NodeId + PortId/SlotId                                                 |
| 资源     | LogicalResourceId、ValueVersion、约束、所有权、读写声明                                               |
| 资产绑定 | AssetId、ProductId、TargetVariant、ContentRevision、绑定状态                                          |
| 布局     | 坐标、折叠、注释、Reroute；不进入语义摘要                                                             |
| 扩展     | 未知字段原文和拥有者，不静默丢弃                                                                      |

NodeId 在同一图内唯一；全局诊断定位使用 `GraphAssetId/GraphVersion + InstancePath + NodeId + PortId/SlotId`。不能用数组下标、显示名、文件路径或内存地址替代身份。

### 7.2 语义摘要

`SemanticDigest` 只包含影响运行的字段：节点类型/契约版本、参数、ActiveSource、资产内容版本、数据边、槽位顺序、资源访问、GraphVersion、GraphVariantId、目标、CapabilityProfileId、CompilerVersion、PlanSchemaVersion、NodeCatalogDigest、AdapterCatalogDigest 和固定约束。布局变化不改变摘要；绑定切换、资源内容变化、契约变化、目标/能力变化、编译器变化和图变体变化会使 Prepared/Applied 快照失效。Canonical 序列化必须固定字段顺序、集合排序、数值格式和浮点特殊值表示，不能使用本地化文本作为摘要输入。

### 7.3 Asset 生命周期

外部资产状态至少为：

```text
Discovered → Imported → Cooked → DeviceReady
     ↘ Invalid / Missing / Unsupported
```

作者只保存稳定 AssetId；路径属于 Asset Database。移动、重命名、重新导入保留 AssetId 时，ContentRevision 增加；删除或导入失败进入 Invalid，不把端口当作无来源而悄悄替换默认值。不同 TargetVariant 有独立 ProductId 和准备状态。

作者文件写入必须是崩溃安全事务：先写临时文件并校验 SchemaVersion、SemanticDigest 和完整性摘要，再 flush/关闭并原子替换；保留上一个可恢复版本和必要的写入日志。外部修改、写入中断或摘要不匹配时，编辑器进入恢复/冲突状态，不能把半写文件当作新作者图编译。

## 8. 代码节点和 TypeCatalog 统一来源

### 8.1 单一契约来源

第一阶段仍采用“模块 C++ 声明/注册代码为唯一真源、TypeCatalog 为导出结果、编辑器只读 TypeCatalog”。手写 `module.metadata.json` 不能继续被当作和 C++ 同等权威的第二份契约。

### 8.2 推荐导出流程

```text
节点声明/注册代码
    → Host-safe metadata export
    → TypeCatalog canonical JSON
    → ContractDigest
    → DLL/metadata/manifest bundle
    → Editor offline catalog / Runtime verified bundle
```

导出必须是 Host-safe 的纯元数据路径，不创建 GPU 资源、不执行游戏帧、不保存 C++ 对象。本项目首期选定“每个模块提供独立 Contract Exporter host target，由同一份 C++ 注册函数写入最小 ContractRegistry”的路径：Exporter 只链接契约声明和无设备依赖的类型，不加载游戏运行时 DLL；运行模块也调用同一注册函数生成运行目录。不能让 Editor 扫描 C++ 源码猜节点，也不能把手写 JSON 当作第二份真源。

导出的 canonical JSON 由工具规范化排序；C++ 注册得到的运行目录、导出的 JSON 和 manifest 中的 ContractDigest 必须一致。旧 metadata 文件如果仍用于开发 fixture，必须显示 `SourceRevision/DirectoryStale`，不能标记为经过构建验证。

Contract Exporter 和运行模块都属于受信任的项目构建代码，首期不承诺进程内沙箱。Editor 只执行用户授权项目和插件的 exporter target；模块 bundle 必须带 OwnerModule、SourceRevision、BuildId 和可选签名/信任状态，未知来源或摘要不一致的 DLL 不得自动加载或进入 Active。该边界必须在项目设置和诊断中明确显示。

### 8.3 代码回流与发布代次

每次候选构建产生：

```text
BuildId + SourceRevision + ContractDigest + ModuleGeneration + TargetVariant
```

候选目录先验证，再迁移/准备受影响图，最后在安全点与运行快照一起 Active。旧模块、旧回调、旧 CPU/GPU 工作和旧资源在所有引用释放前保留；卸载超时维持旧 Active，不强制关闭 DLL。异步结果必须同时匹配请求代次、GraphVersion、SemanticDigest 和目标代次。

如果首期不支持新旧模块并存，则使用“停止预览 → 排空引用 → 重载 → 重新准备”的明确流程，停止后不承诺旧预览继续工作。模块升级时，节点运行状态必须声明 `Reset`、`Migrate` 或 `Reject`；没有状态迁移协议时只能 Reset，不能把旧代次的状态对象传给新实现。

## 9. SideEffect、裁剪和缓存

### 9.1 副作用契约

节点至少声明：

- `SideEffectClass`：None、ExternalWrite、Present、Capture、HistoryWrite、Readback、Tooling 等。
- `MustExecute`：是否可以被裁剪。
- `Idempotency`：重复执行是否安全。
- `ExternalVisibility`：需要哪些 Graph Output、外部绑定或运行观察根。
- `ReceiptType`：完成后输出什么 Receipt，以及它证明的完成阶段。
- `CommitPolicy`：副作用何时提交、是否可撤销以及提交后的可见性。
- `RetryPolicy`：是否允许重试、最多次数、退避和幂等前提。

编译器从 Graph Output、外部导出、显式保活资源和 MustExecute 副作用根反向标记；普通未消费节点可以裁剪。Runtime Inspect 显示节点被保留或裁剪的根原因。

### 9.2 缓存和合并

节点声明 `Purity`、`Determinism`、`CachePolicy`、`MergePolicy` 和隐式依赖。缓存键至少包含契约、参数、输入逻辑版本、资源 ContentRevision、目标能力、Frame/View/World Scope、Shader/资产版本以及 7.2 列出的编译和目录版本。External Read、随机数、时间、SideEffect 默认不可缓存；合并不能改变副作用次数、输出版本或诊断来源。

没有数据边关系的纯节点可以并行；不应以节点位置或稳定排序伪造业务先后。非幂等或不可交换的 SideEffect 必须通过真实 Receipt/DataEdge、同一外部目标的冲突诊断或固定引擎约束表达顺序。稳定排序只能让计划可复现，不能解决数据竞争。

### 9.3 条件和启用

首期只允许 Compile-time/Prepare-time 条件改变图；运行时条件使用 `Select/Switch` 等显式节点，不动态增删端口。`Enabled=false` 的语义必须由节点类型声明：移除执行但保留输出、输出默认值、或使输出无效，三者不能混用。影响拓扑的参数生成版本化 Graph Variant，并在切换时提供迁移预览。

## 10. 兼容性矩阵和诊断

每条候选边经过兼容矩阵，结果只有三种：

| 结果              | 行为                                                                              |
|-------------------|-----------------------------------------------------------------------------------|
| `Compatible`      | 直接建立 DataEdge，并生成执行/内存计划                                            |
| `AdapterRequired` | 推荐明确 Adapter 节点，显示资源、精度和性能代价                                   |
| `Forbidden`       | 拒绝连接，说明 Domain/Scope/Lifetime/Access/Format/Queue/ValueSchema 中的具体冲突 |

未列入矩阵的组合默认为 Forbidden。矩阵随 ContractVersion、SchemaVersion 和 AdapterCatalogDigest 版本化，不能用端口显示名猜兼容性。

诊断至少区分：来源未设置、来源 Pending、来源 Cancelled、资产 Invalid、Edge 类型冲突、资源版本冲突、读写冲突、完成点不匹配、跨阶段非法、上游失败导致的 Skipped、旧版本迁移阻断、模块目录过期和运行快照代次不匹配。

## 11. 首期可执行范围

首期收敛为：

1. 单一 Render Domain、单一目标平台、单一当前项目。
2. Pass 图优先，支持 Read-only Texture Asset、Edge/Asset 二选一、Frame/View Scope。
3. 不开放隐式原地写、跨领域直连、GPU→CPU 无完成事件、动态运行时拓扑和新旧模块并存。
4. 支持真实的 AssetId/ContentRevision、逻辑资源版本、兼容矩阵和 Runtime Inspect。
5. Feature 在父图中先作为有版本的黑盒子图，在其定义资产内部仍可编辑 Pass/子 Feature；跨领域 Feature 通过 Artifact Bridge。
6. 代码节点脚手架必须接入选定的 Contract Exporter、ContractDigest、BuildId 和 last-good 发布流程。

首期可执行范围是 Render/Frame profile 的交付范围，不表示 Pipeline/Feature/Pass 是全域业务层级。
Logic Flow profile 作为架构契约保留，开放前必须补充 FlowInstance、StateVersion、Transition
事务、暂停/取消/恢复/迁移和多实例隔离的可复现实验证据。

## 12. 规范关闭标准

方案只有在以下证据全部具备时，才从“设计阶段”进入实现：

- Edge、Asset、Literal、Default、Context、None、Cancelled 的同一端口状态机可保存、重开、Compile 和 Preview。
- CPU、GPU 同队列、跨队列、GPU→CPU Readback、Asset Prepare 都有明确完成条件和失败诊断。
- 逻辑资源 ValueVersion 能解释所有读写冲突和物理内存复用。
- 子图版本、调用路径、递归拒绝、跨 Stage/Part/Domain 规则可验证。
- 作者文档、SemanticDigest、Cook 产品和 Runtime Inspect 的身份一致。
- C++ 目录、canonical metadata、运行注册和 manifest 的 ContractDigest 一致。
- 候选模块失败、重载超时、旧回调未释放和异步倒序完成都保持 last-good。
- 裁剪、缓存、合并和副作用均能从 Runtime Inspect 回溯到契约声明。
- 多 View/多次调用不会串用 ResourceValueVersion、History、缓存或异步结果；ExecutionInstanceId 可完整回溯。
- 上游失败、取消、跳过、部分结果和外部副作用重试都有确定的传播和提交结果；AttemptId 不会让旧异步结果覆盖新尝试。
- 崩溃或并发保存不会产生半写作者文件；未知字段和冲突能够保留并阻断不安全编译。

## 13. 方案调研依据与取舍

本方案借鉴 UE Blueprint 的节点、端口、拖拽连线和调试定位体验，但没有照搬它把执行引脚和数据引脚分开的语义。Epic 的官方文档将两者分别定义为执行流和数据传递；本项目已经确定普通连线必须同时代表数据流与先后，因此把“完成”建模成 `ExecutionToken`、`WriteReceipt` 或 `ReadbackReady` 等有类型的数据结果，避免出现用户看见一条线却没有实际数据来源的情况。[Epic：Nodes in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/nodes-in-unreal-engine)

资源同步部分采用“作者层数据依赖 + 编译器生成执行/内存依赖”的分层。Vulkan 规范明确指出，执行依赖本身不能保证写入值对消费者可见，读写冲突还需要可用性、可见性、访问范围和队列交接等内存依赖；因此作者与执行契约不允许把“节点先执行”直接等同于“GPU 结果已经可读”。[Vulkan：Synchronization and Cache Control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)

这些资料只作为外部设计依据；最终实现以本项目的 Authoring Contract、兼容矩阵和首期范围为准。外部资料中的执行引脚、隐式同步或 API 对象不能直接成为作者文档字段。
