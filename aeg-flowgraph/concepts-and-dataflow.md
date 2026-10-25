# AEG FlowGraph：概念与数据流模型

[返回架构总览](<../AEG FlowGraph Architecture.md>)

输入来源、默认值、执行实例、结果状态和资源版本以 [作者与执行契约](authoring-and-execution-contract.md) 为准；
目标 Feature 五域、Composite 组合和旧 Pipeline/Pass 迁移以 [Feature 五域架构设计](../AEG%20Feature%20Five-Domain%20Architecture.md) 为准；
本分册负责术语和作者侧数据流解释。

## 1. 设计目标

FlowGraph 不是把旧画布换成深色皮肤，而是把作者模型改成“可组合的数据流图”。用户的基本动作应该像拼积木：从左侧拿一个已有节点，放到画布，看到它的输入和输出，把上一块的输出拖到下一块的输入；需要多个来源时，只能连接到节点类型已经声明的 Multi-input、Array 或 Variadic 端口，或者插入节点类型明确的 Merge/Compose；不需要时删除节点，留下可诊断的断线，而不是让系统悄悄猜一个替代关系。

成功标准是：一名游戏开发者可以只理解“这个节点需要什么、输出什么、多个输入怎么合并”，就能组装一个可预览的 Feature，并能沿着数据边解释结果为何产生或为什么没有产生。

## 2. 名词重新定义

| 名词             | 在 FlowGraph 中的含义                               | 与历史模型的关键差异                                                |
|------------------|-----------------------------------------------------|---------------------------------------------------------------------|
| Feature Graph（主作者模型） | 五域 Feature、Composite 和公开资源契约组成的数据流图 | Feature 是唯一业务身份；各域可派生不同执行投影 |
| Pipeline Graph（Frame/Render 兼容 profile） | 由 Feature Graph 派生的计划/锚点图 | 保留现有 API 和诊断，不再作为全域业务根 |
| Pass Graph（Frame/Render profile） | 由 Pass、Resource、Value 和组合节点组成的执行细节图 | Pass 不再主要依靠隐式顺序边，也不承载跨帧状态 |
| Process/State Graph（Logic profile） | Feature 内部由 State、Transition、Event、Guard、Action 和 FunctionCall 组成的流程图 | ProcessInstance/StateVersion/OwnerPath 必须保留 |
| Node             | 画布上的积木实例                                    | 任何有稳定类型契约的对象都可成为节点                                |
| Node Type        | 节点类型定义，包含端口、参数、能力和编译规则        | 类型定义与实例布局/连接分离                                         |
| Port / Pin       | 节点的数据入口或出口                                | 连接的最小合法单位，不连接节点外框                                  |
| Data Edge        | 从输出端口流向输入端口的数据关系                    | 唯一常规连线；同时代表数据依赖和执行先后                            |
| Graph Input      | 子图接收父图或运行目标数据的边界节点                | 把跨层引用变成显式端口                                              |
| Graph Output     | 子图向父图暴露结果的边界节点                        | 取代任意节点连 Present 的隐式终点                                   |
| External Binding | 由资产、文件资源、默认值或运行上下文满足端口输入    | 没有上游数据边也可以是合法来源，但必须由端口契约声明                |
| Source Policy    | 输入端口允许哪些来源，以及如何判定输入已满足        | EdgeOnly、AssetOnly、EdgeOrAsset、Default、Context、Optional        |
| Value            | 标量、向量、枚举、颜色、参数等小型值数据            | 可由常量、参数或上游节点产生                                        |
| Resource         | 纹理、缓冲、加速结构、历史纹理等可读写数据          | 一等数据对象，有格式/作用域/生命周期                                |
| Artifact         | Pipeline/Feature 间传递的业务快照或结构化结果       | 可以是场景快照、可见性列表、光照数据等                              |
| ExecutionToken   | 明确表示“某个步骤已完成”的控制数据                  | 替代不携带数据的自由 Ordering 线                                    |
| Multi-input      | 节点声明的一个输入端口接收多个同类来源              | 数量、顺序和合并规则全部由节点类型声明，不是画布通用能力            |
| Variadic Port    | 节点类型明确声明的可变数量输入端口                  | 只有该端口允许增加槽位，且受节点声明的最小/最大数量和空集合语义约束 |
| Adapter          | 把一个公开类型转换成另一个合法类型的节点            | 转换成本和精度变化可被看见                                          |
| Merge/Compose    | 以明确算法合并多个来源的节点                        | 取代“多根线接到一个输入后由引擎猜”                                  |
| Macro / Subgraph | 一组节点保存成可复用的图节点                        | Feature、Flow 子图和 CompositePass 都可以是此类容器                 |
| Side Effect      | 写入外部目标或改变状态但不产生颜色结果的能力        | 必须通过 Receipt/Token 等输出参与图依赖                             |
| Dangling Edge    | 端点被删除或暂时未完成的断线                        | 可保存为草稿，不能编译运行                                          |
| Compile Plan     | 从作者图生成的执行候选计划                          | 不是作者图的第二份可编辑副本                                        |
| Runtime Inspect  | 某帧实际执行图的只读检查                            | 可显示裁剪/合并/资源复用，不反向修改作者图                          |

## 3. 数据的六个维度

节点端口不能只写“颜色”“纹理”这种显示名，至少需要以下六个维度：

| 维度       | 示例                                         | 用来判断什么                    |
|------------|----------------------------------------------|---------------------------------|
| Domain     | Render、Physics、Audio、Logic、Stream        | 数据在哪个业务领域可解释        |
| Value Type | Float、Color、Camera、LightList、SceneDepth  | 端口是否类型匹配                |
| Storage    | Scalar、Texture2D、Buffer、Snapshot、History | 需要什么存储和读写方式          |
| Scope      | Frame、View、World、Asset、Engine            | 生命周期和所有者是否可见        |
| Lifetime   | Transient、Imported、Persistent、History     | 可以存活多久、何时释放或清理    |
| Access     | Read、Write、ReadWrite、Consume              | 生产者/消费者、同步和多写入规则 |

例如 `SceneDepth` 可以描述为：`Render / Texture2D / Frame / Transient or Imported / Read`。两个端口即使显示名相同，只要 Scope、格式或 Access 不兼容，也不能直接连线。

## 4. 图的三个层次

本节的三种图是作者资产的视图，不是全域业务层级。`Pipeline Graph → Feature Graph →
Pass Graph` 只在 Frame/Render profile 中展开；Logic/Flow profile 的 canonical 对象是
`Feature → Flow/State → Transition/Event → FunctionCall`，其跨帧状态和 OwnerPath 必须
保留。FunctionCall 不是独立 Pipeline/Feature/Pass，RenderPass 也不能承载 Flow 状态。

### 4.1 根图 Root Graph（Frame/Render profile）

根图由一个 Pipeline 资产承载。它可以接收 `WorldSnapshot`、`View`、`FrameContext` 等系统输入，向目标暴露 `ColorOutput`、`DepthOutput`、`DebugOutput` 或业务 Artifact。根图可以包含 Feature 节点和系统适配节点，但不能直接引用 Feature 内部私有 Pass。

### 4.2 复用图 Reusable Graph

Feature 或 CompositePass 把一组节点封装为一个可复用图。封装操作要求用户为边界端口命名并选择方向、类型、来源模式、默认值和是否必须有可用来源。图内节点布局保存为子图布局；父图只显示黑盒摘要和公开端口。

### 4.3 执行图 Execution Graph（Frame/Render profile）

Pass 图展开后形成执行候选图。执行图包含资源访问、队列、阶段、屏障和实际实例化信息，但仍通过稳定来源映射回作者节点。用户不能在执行图中拖动或连线；需要修改时定位到对应作者图。

## 5. 多源数据模型

### 5.1 多个不同输入

节点可以有多个命名输入，例如：

```text
Bloom Pass
  SceneColor ───────────────>
  BrightnessMask ───────────>
  CameraExposure ───────────>
```

每个端口类型不同，连接到错误端口立即拒绝。用户不需要把多个来源先装进一个模糊的“输入数组”。

### 5.2 同类型多个输入

对节点类型明确声明为 Variadic/Array 的输入，端口显示为集合：

```text
Layer Compose
  Layers[0] <──────── Pass A
  Layers[1] <──────── Pass B
  Layers[2] <──────── Pass C
```

集合端口的数量下限、上限、顺序是否有意义、空集合如何处理和每项数据类型都由节点类型声明。若顺序有意义，端口列表支持在节点允许的范围内拖动排序；若顺序无意义，编译器按节点规则稳定排序，不按画布位置猜测。普通节点不会因为用户想接第二根线就自动获得集合端口。

### 5.3 多源合并

Merge、Join、Compose、Blend、Select 等节点把“多个来源如何变成一个结果”写在节点类型里。系统不自动把多个输出接到单值输入，也不使用“最后连接覆盖前一个”规则。

### 5.4 多个消费者

一个输出默认可以分叉到多个消费者，但每个消费者仍会声明是否允许共享、是否需要复制、是否影响资源生命周期。输出端口禁止分叉时，第二次连接提示插入 Copy/Adapter 或创建新资源，而不是静默复制数据。

### 5.5 没有上游连线但输入已满足

“必需输入”表示节点执行前必须获得一个合法值或资源，不表示画布上一定要存在一条上游数据边。输入端口必须声明自己的 `SourcePolicy`（规范字段 `AllowedSources`），常见模式如下：

| 来源模式        | 是否需要上游数据边 | 合法来源                       | 典型用途                         |
|-----------------|-------------------:|--------------------------------|----------------------------------|
| `EdgeOnly`      |                 是 | 上游节点输出                   | SceneColor、动态计算结果         |
| `AssetOnly`     |                 否 | 项目 AssetId/导入资源          | 固定纹理、LUT、采样器配置        |
| `EdgeOrAsset`   |             二选一 | 上游输出或项目资产             | 可接动态纹理，也可直接选文件资产 |
| `EdgeOrDefault` |             二选一 | 上游输出或契约默认值           | Exposure、阈值、可选参数         |
| `Context`       |                 否 | Frame/View/World/Graph Context | 当前视图、相机、帧上下文         |
| `Optional`      |                 否 | 无来源也合法                   | 可选 Mask、调试开关              |

`EdgeOrAsset` 必须在实例上明确当前采用的来源模式；不能同时保留一条边和一个资产绑定后让引擎猜优先级。没有上游边时，不创建虚假的执行顺序；资产导入、Cook 或 Prepare 只形成资源依赖和准备条件。若开发者需要在画布上看见该来源、复用它或追踪它的生产关系，应放置 `Texture Asset`/`Resource` 节点，用数据边接入消费者。

文件路径不能直接作为长期运行时身份保存。编辑器选择文件后应导入并解析为稳定 AssetId/资源绑定；源路径只保留在资产元数据中。文件不存在、导入失败或格式不兼容时，端口属于“来源未满足”，而不是“没有连线所以合法”。

## 6. 数据流与执行顺序

每一条普通数据边都同时表示数据流和执行先后：A 的结果输出接入 B 的源数据输入，表示 A 先产生这份结果，B 再执行并消费它。FlowGraph 不提供普通的“只表达先后、不携带数据”的 Ordering 线。原因是这种线很容易让游戏开发者误以为数据已经传递，或者让引擎在无数据来源时偷偷改变执行关系。

Frame/Render profile 需要表达顺序时使用以下方式：

1. Pass A 输出 `ExecutionToken`，Pass B 将其作为必填输入。
2. 只有当 `Sequence` 自身声明了内部步骤、并为每个步骤产生可追踪的完成数据时才允许使用；普通 Token 分叉不能保证多个下游串行完成。首期不提供没有内部完成握手的 Sequence。
3. 使用 `Join` 节点等待多个输入完成后产生一个 Token。
4. 有状态写入输出 `WriteReceipt`，后继节点消费该 Receipt。

这些仍然是数据流，只是数据类型表达的是完成、提交或状态结果。普通 Pass 没有 Token/Receipt 输出时，不应被用户用空线“强行排在前面”。

Logic/Flow profile 不使用上述 Pass 顺序替代状态机语义。StateMachine 必须使用带类型和身份
的 Transition/Event/Guard/Action 边；Action 内的 FunctionCall 只产生声明性 StateDelta、
Intent 或 Receipt，由 Flow owner 在 commit 边界原子提交。普通 DataEdge、ExecutionToken
和 RenderPass 的拓扑不能表达跨帧暂停、取消、恢复或迁移。

## 7. 节点可以删除，但结果必须诚实

删除节点的语义：

- 删除节点自身和连接到它的边。
- 上游节点保留，若它没有消费者则标记为孤立节点。
- 下游端口重新计算来源状态：没有边且没有合法外部绑定的必需端口变成“来源未满足”错误；可选端口变成未连接提示；已有合法资产/默认/上下文绑定的端口保持就绪。
- 不自动把上游输出接到下游，也不自动恢复默认节点。
- 撤销恢复节点身份、参数、布局、边和资源绑定。

系统节点如 `Frame Input`、`Graph Output`、必需的 `ExecutionContext` 可以被折叠或替换绑定，但不能删除；固定节点在画布上显示锁定原因。用户创建的普通节点可以删除，哪怕删除后图暂时不能编译。

## 8. 术语解释入口

每个端口、节点和诊断都提供“解释”动作。解释至少回答：它产生/接收什么数据、数据从哪里来、谁会消费、是否会分配资源、是否会影响主输出、删除它会断开哪些路径。高级字段显示稳定 TypeId、Scope、Queue 和 Lifetime，但不会用这些内部名称替代用户可理解的用途说明。
