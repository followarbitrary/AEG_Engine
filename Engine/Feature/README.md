# Feature

`Engine/Feature` 是五域 Feature 的运行时契约和执行状态边界。它负责
`FeatureDefinition`、`FeatureGraph`、`FeatureRegistry`、`FeatureInstance`、`ProcessInstance`
以及跨 Feature 的声明式端口传输；五个 Domain（Logic、Physics、Render、Audio、Stream）
是能力分类，不是目录层级或执行先后。

## 公共入口

- `feature_contract.hpp`：Definition、Domain、ExecutionKind、StatePolicy、Clock、权限及 OutboxIntent 值合同。
- `feature_parameters.hpp`：版本化参数记录的统一编码、解码与校验；编码按 ParameterId 排序。
- `feature_contract_metadata.hpp`：可选类型、默认值、范围、枚举、文本/资产/向量约束与编辑提示。
- `feature_graph.hpp`：节点、边、Process 子图和编译后的 provenance。
- `feature_registry.hpp`：Definition 注册、Composite 展开和 `owner_module` 归属校验。
- `feature_runtime.hpp`：Instance/Process 生命周期、Invocation、StateDelta、迁移和执行计划。
- `feature_time.hpp`：带 Runtime 时间域的单调纳秒时刻和时长。
- `feature_resource_bus.hpp`：Snapshot、Queue、Event、Command、Receipt 的声明式传输合同。

## 边界

Feature 运行时只消费声明式合同和不透明 payload，不拥有 Editor 坐标/布局、不创建
RenderGraph Pass，也不把 Pipeline authoring 类型作为业务身份。`owner_module` 从 Definition
传播到 Graph provenance、FeatureInstance/ProcessInstance 和 Render 投影，用于诊断、热替换
和卸载安全检查。

多线程、异步任务和等待由引擎内部的 `Engine/Concurrency` 适配层提供；Feature/Module
公共契约不暴露线程、协程或标准库执行器。跨 Clock 消息使用同一 Runtime 来源的单调纳秒
时间域；candidate 与 reset 共享该来源，独立 Runtime 使用独立 domain。
在输入捕获、worker 开始和 owner 提交时重验期限。边的 `deadline_ticks` 以发布图时作者源
Clock 的周期为单位；重新编译并发布图后更新该周期快照。Receipt 继承原 Command 的期限。

Clock 必须显式声明 capability version、phase、relative deadline、deadline policy 和工作预算。
公开 `make_periodic_clock_contract` 只产生声明值；host 的版本化能力准入由私有上下文完成。
Runtime 按实际展开的重复 owner 和全部根审核自动工作量，暂停 owner 仍预留预算；Call 按
单次工作验证。候选激活、换图或替换失败保留 live generation、状态和 pending 交付。
`Observe` 记录超限，`RejectCommit` 拒绝晚到的状态、输出和 Outbox 整体提交；这属于尽力执行
与预算审核，不构成硬件实时保证。时间源可通过 `FeatureRuntimeOptions::now_ns` 注入。

执行回调为 `execute(token, state, input, parameters)`。`state` 是独立拥有字节和流程状态名的
只读快照，`input` 只包含本次输入消息，`parameters`
是当前实例的完整参数信封；显式传入输入也会收到参数。两份只读视图仅在回调期间有效，
需要保留时由 Feature 自行复制。使用 `decode_feature_parameters(parameters, contract.parameters)`
读取版本化记录，由 Feature 自己解释记录内的业务载荷。编译、编辑、发布和热替换都使用同一个
参数校验入口；新参数 schema 无法接受已激活实例时，热替换失败并保留原定义和代次。

自动重试保留本次输入、参数和已捕获消息的 MessageDeliveryId。等待期间的普通 dispatch、
invoke、手动 retry 和定义替换均拒绝；Clock 按 backoff 驱动下一次尝试。成功提交只消费本次
捕获的交付，因此等待期间到达的新 Queue 消息或替换旧值的新 Snapshot/Data 保留给下一调用。

类型化参数描述直接位于 `ParameterContract::metadata`，不再维护第二份身份/版本/必填 schema。
值复用 Foundation 的 `ParameterValue`，没有默认值以 `optional` 表示。类型、默认值和有效性约束
进入 Feature 执行摘要，展示名、说明、分组和排序不进入该摘要；Module JSON 携带完整描述。
模块包的整体 SHA 仍覆盖这些展示字段，属于完整元数据包的身份。

编解码使用固定宽度小端格式，Int32/UInt32/Float 为 4 字节，Int64/UInt64/Double 为 8 字节，
资产引用保留完整 16 字节 UUID，向量分量为 float64。拒绝类型错配、非有限数、错误宽度/维数、
非法约束和默认值、截断及尾随字节。Float 的范围和步长按实际 float32 线格式判断。
缺失且有默认值的参数由共享信封入口补齐，回调始终收到物化后的参数；必填且无默认值的
参数在图编译/发布时仍须显式提供。没有 typed metadata 的 payload 继续由 Feature 自己解释。
`AssetConstraint::kind` 可声明资产种类；Editor、Cook 和 Runtime host 使用实际目录或包解析器
验证 UUID 存在及种类。纯合同校验保持独立，运行会话保留接受时的不可变资产目录。

Editor 的参数草稿支持分组、高级参数、默认值、清空可选值和类型控件，Apply 作为单次可撤销
事务写入。共享值控件在 FeatureGraph 和内部 Frame 编排视图间复用；用户未应用的草稿不改变
运行图。更换节点或外部文档版本变化会重新载入参数草稿。

真实状态载荷由 `StatePolicy` 决定所有者：InstanceState 使用实例 StateStore，ProcessState 使用
Process StateStore；Process 的 current_state/revision/status 始终独立保护 continuation。
Stateless/ExternalSnapshot 不持有本地业务字节，任何 state_payload 更新（含清空）均拒绝。
`StateDelta::state_payload == nullopt` 保留载荷，有效空 vector 明确清空；初始载荷为空。
StatePayloadContract 在唯一 FeatureContract 中声明 schema_version 和 max_size，默认 schema
1.0.0、上限 1 MiB；二者进入共享执行摘要并通过 Module JSON 往返。Process 子图的
state_schema_version 仅描述状态机拓扑，不由业务载荷迁移重新标记。

完整状态/输出提交沿用私有暂存事务，包含字节上限和可选纯业务校验器；非法状态、输出、
outbox、跃迁或容量均不改变已提交载荷、消息队列和 pending journal。校验器与迁移器不能
重入修改 Runtime。
自动重试冻结状态快照和原版本；暂停/恢复在载荷版本和 current_state 未变时允许生成新的
ProcessRevision/AttemptId，旧结果仍拒绝。状态被其他提交或显式转移改变时，不回放旧载荷。

StateMigration 的可选纯 migrate_payload 按所有相关 owner 暂存转换并校验，全部成功后发布
定义、状态与 RuntimeGeneration。跨 schema 的非空业务载荷必须显式转换；同 schema 默认
保留。空初始载荷可以在新校验器接受的前提下迁移 schema。活跃实例不允许变更状态所有者或
执行生命周期。activate 先核对整个展开树的当前合同，再分配实例，拒绝热替换后的旧展开。
RuntimeBootstrap 启用私有五域异步执行策略。Logic、Physics、Audio 各自持有 CPU Worker，
Render 持有 RenderThread Worker，Stream 持有 IO Worker；共享通用 lane 不合并五域所有者。
工具和确定性测试可使用默认 inline 策略；两种策略共用快照准备、回调包装和提交事务。

worker 只访问拥有的 Definition、token、状态、输入和参数快照；Runtime 所属线程通过
`pump_completions()` 回收 Completion 并提交。每实例同时至多一个任务，同 Clock 的
Data/Command/Receipt 依赖在上游提交后才捕获输入。忙碌 Clock 按 Drop/Reject/Queue
处理新触发，Queue 以 max_backlog 限制待执行计划。取消或失效的依赖链不再执行本轮下游；
显式状态转移清理旧重试，进入终态不会永久阻塞同 Clock 的其他实例。

Runtime 非所属线程的读取和修改入口拒绝访问。取消、迁移和图换代后的迟到结果只回收，
不提交状态或消费消息。关闭取消排队任务，等待运行回调返回，然后清理完成收件箱；
普通回调不能被强制中断，游戏作者无需使用 Concurrency API。SampleBlock 与真实 Stream IO
readiness、取消/重试及 World 等待时 Audio 独立提交已有专项；具体音频设备适配按宿主实现。

Process 的 Guard、候选选择和 Action 可保持 Feature 私有实现。作者按 priority 降序、因果序号
升序、稳定 NodeId 升序选择一个转移，在候选 Delta 中依序准备 Exit、State、Enter，再写 Outbox。
Runtime 在唯一 owner 边界原子校验并提交，实际 consumer 读取已提交状态后交付命令。
`aeg_feature_process_transition_order_smoke` 通过真实 Process、worker gate 和 private consumer
验证该接缝及坏载荷、后段非法命令、过期和取消的非空 journal 保留语义。

激活实例保留经接受的 definition_digest，派生自唯一合同摘要。直接修改 Registry 不能绕过
活跃状态迁移：摘要不匹配时 invoke/complete 拒绝，要求 Runtime 迁移或重新发布图。
合法 Runtime 替换在同一暂存事务中更新绑定；此缓存不是第二份业务合同来源。

## 已提交命令 outbox

`StateDelta::outbox` 是拥有字节的 `OutboxIntent { effect, version, payload }` 数组。
唯一 `SideEffectContract` 必须以相同 ID/version 声明 `kCommand + kOutbox`；这对 ID/version
就是命令 payload 的类型版本标识，不新增另一套 schema 字段。现有 Feature 合同摘要和
JSON 已包含该副作用声明。Feature callback 只提供命令值，不提供交付来源或执行句柄。

`private/feature_outbox.hpp` 与 `private/feature_runtime.cpp` 实现 Runtime 所有的 pending
journal。新命令在同一个暂存事务中与 State、Process、ResourceBus 提交，复制 payload，
附加成功尝试的完整 InvocationToken 和该 Delta 内独立的 entry_index。连续提交按追加顺序
保留每条命令，空 outbox 不清除旧 pending；后续 Render publication 失败也不撤销已接受的命令。
公共 `kMaxOutboxIntentsPerCommit` 为 1024，`kMaxOutboxIntentBytes` 为 1 MiB；私有 journal
同时限制 1024 条 pending 和 16 MiB pending payload。达到边界合法，超限拒绝整个源提交。
终态交付立即释放 pending 预算，外部仍持有 handle 不继续占用该预算。

Engine-private `detail::FeatureOutboxAccess` 提供 bind/capture/consume/reject。
`FeatureOutboxBinding` 与 `FeatureOutboxHandle` 封装真正 canonical owner/entry 的共享所有权，
调用方只能默认构造无效值或复制已有 binding/handle，不能拼装 source 或 payload 取得权限。
bind 使用 Runtime 实际发布的全域 root 和 descendant 来源快照，保留 Module、Definition、
Node、Instance、owner path、Clock、Domain、generation 和定义摘要；修改导出的编译图不会
改变此来源。root 使用作者 parent path，descendant 保留完整展开 ancestry 及其作者 root Node。
直接 activate 而未进入真实图发布的实例不能取得 binding。

capture 只返回该 binding 的 Pending 命令。consume 在应用前核对 owner thread、Runtime/
binding/generation、协议与 canonical pending entry，拒绝 foreign、无效 handle、重放、
空函数和重入；失败预检不调用应用函数，也不移除命令。调用方必须先完成 typed decode 和
全部目标/数值/lifecycle 检查，再提供 `noexcept` 应用函数。Runtime 在同一重入 guard 内
调用它，随后无分配地记录 Applied receipt 并确认交付；这里没有可失败的通用 World batch
callback。reject 只接受有效的非 Success ResultCode，生成终态 Rejected receipt。
receipt 保留完整 token、entry_index、状态和错误码，默认无效 handle 不具有消费权限。

已接受命令不依赖最新 invocation、当前 StateVersion 或 Process 状态，也不要求 Registry
仍持有定义；Process 暂停、取消、完成不会丢掉已提交的 pending。pending 期间 graph
publication 和 definition replacement 返回 NotReady，保留 last good owner/generation；排空后
允许换代，旧 binding 随之失效。shutdown/destructor 为剩余命令明确记录 Cancelled receipt，
捕获 handle 继续拥有原 payload 和 receipt；未提交的迟到 callback 结果仍按原机制丢弃。

Engine Runtime 的 private `feature_world_commands` 已消费固定 `World.Transform` v1 协议，
通过真实发布的 Feature owner 显式绑定 World 代次和实体，在 snapshot commit 前应用。
命令不读取 latest StateStore，不使用隐藏 Feature endpoint 或普通 ECS command batch
替代交付事务。journal 不构成跨进程持久化或跨系统原子提交；其他组件协议需要各自的
typed adapter，不能把任意 bytes 当作组件内存。

## 已提交 Render 意图

Render Feature 通过同一个 execute 返回 `StateDelta::render_intent`。这是拥有自身数据的
类型化 `RenderIntent`，不包含 RHI 句柄、Builder、执行回调或作者 Pass 身份。提交必须声明
`Render.Intent` v1 的 owner-boundary intent 副作用和各项能力权限；只有 Atomic Render
Feature 可提交。CPU owner 在状态/消息暂存事务中共同接受它，非法意图不会部分改变状态。
省略意图保留上次接受的结果；有效空意图明确停止之前的工作。

v1 支持每实例局部的 RGBA8 transient/history 纹理、颜色清屏、纹理区域复制和可选最终输出。
外部引用使用稳定 ID 与版本，由宿主提供共享或实例专属绑定；GPU 句柄属于 RenderGraph。
`capture_render_outputs` 从已发布执行计划捕获实际原子叶子，保留 Composite 的作者 Node
以及叶子的独立 Instance/Invocation。取消、显式跃迁、代次替换和图换代均使旧意图退休。
RenderGraph 构图单独验证实际绑定并使用自身回滚事务，CPU 状态提交与 GPU 构图不是
跨系统原子事务。history 需要 `kCreateHistory` 权限，代表最近一次成功 GPU 提交的内容；
未初始化或未匹配版本的读取/局部写入拒绝构图。引擎通过独立的
`capture_render_resource_owners` 维护资源所有者，空输出不表示实例结束。实例资源 epoch
在取消、显式跃迁和受影响的定义迁移时退休，无关定义迁移保留资源。GPU 句柄、存储、
帧保留和提交事务位于 RenderGraph，游戏作者只声明 Feature 意图。
RenderDrawScene 已支持宿主槽 ID/版本和外部颜色目标、明确的 Load/Clear；需要 kDrawScene、
外部写权限，Load 还需外部读权限。原生绑定、固定材质 ABI 和附件检查由 Renderer 执行。
可选 RenderDepthAttachment 使用同一个 RenderAttachmentLoad，当前仅接受外部 D32Float、
2D、X1、单 mip/layer，且尺寸匹配 View；深度 Load 也需要读权限，清除值须有限且在 [0,1]。
颜色清屏、复制和 final_output 保持颜色格式约束；可选 depth_output 明确导出外部 D32
图根，要求导出和外部写权限。RenderDrawDepthScene 携带场景、深度附件及
RenderDepthSceneProjection，选择相机或方向光阴影；复用 kDrawScene 及附件权限，
不要求颜色目标。阴影光空间和相机剔除前的 caster 由引擎推导，无有效光或 caster
时仅执行声明的 Clear/Load。未知投影值在 owner 合同验证时拒绝。
RenderDrawScene 可选 directional_shadow 外部 D32 输入，要求外部读权限且与颜色/深度
附件身份不同。引擎根据同一场景推导方向光常量，校验原生绑定并复用阴影采样绘制；
作者通过 Feature 数据边传递场景和纹理引用，不编写 Pass 或描述符代码。
局部/history 深度、多光源阴影与独立阴影分辨率仍待后续扩展。
内置固定 Renderer profiles 已迁移并通过主线验收；任意 Shader/PSO 作者选择和额外生命周期治理后置。

RenderDrawGBufferScene 使用具名 albedo/normal/material 颜色附件及 depth，每个附件独立
声明宿主槽 ID、版本与 Load/Clear。作者只返回 Feature 意图；引擎固定投影为
RGBA16Float/RGBA16Float/RGBA8Unorm + D32Float，View 尺寸、2D、X1、单 mip/layer。
需要 Draw/外部写权限，每个 Load 还需外部读权限；颜色清除值须有限，深度须在 [0,1]。
四个逻辑身份及原生绑定不能别名。普通 Clear/Copy/final_output 的 LDR 格式约束不变。
GBuffer 工作依靠实际附件消费者或显式 depth_output 成为图根；无消费者时参与裁剪。

RenderDeferredLightingScene 消费外部 albedo/normal/depth 并写入 HDR 附件，可选读取方向光
阴影。输入为 RGBA16Float/RGBA16Float/D32Float，HDR 为 RGBA16Float，形状同 View；
所有输入/输出须独立，输入需 Sampled/读权限，HDR 需 ColorAttachment/写权限。
consumer 场景提供视图和光照，instances 为空也执行全屏消费；背景 depth>=1 输出透明黑。
HDR Load/Clear 是附件装载语义，全屏 shader 仍覆盖像素。当前 shader 不读取 material；
环境光/一盏方向光是固定合同，多光源/temporal 与任意程序仍是后续范围。

RenderPostProcessScene 通过外部 HDR/depth 引用消费 RGBA16Float/D32Float，并写入现有 LDR
输出格式。三者为 View 尺寸、2D/X1/单 mip/layer且不得别名；输出支持 Load/Clear，采样
输入需要 Read/Sampled，输出需要 Write/ColorAttachment，Load 额外需要 Read。Feature 值
合同 RenderPostProcessSettings 只含数组、标量和开关，天空颜色/exposure有限非负，gamma
正有限且倒数有限。可覆盖此操作的设置；缺省使用不可变场景 View 的环境值。作者无需
Renderer、Math或RHI类型。全屏覆盖像素，Load不承诺保留原像素；输出消费者决定图存活。


## 契约 JSON 和作者草稿

`feature_contract_json.hpp` 提供不暴露 JSON 库的字符串编解码接口，完整保留五域、
执行/状态、状态 payload、时钟、端口、类型化参数、权限、副作用及 Composite 映射。
Module 元数据和 Editor 草稿共用同一个 Feature codec；通用 ID、版本和 ParameterValue
字段编码只定义在 Serialization private，不在 Module 与 Editor 中重复实现。

结构解码与发布校验分开。`validate_feature_contract_draft()` 和正式 validator 共用同一
实现，仅允许草稿中的 Composite 尚未填入 children。其他契约约束仍须成立；Generate、
FeatureCatalog 和 Runtime 的正式入口继续使用严格校验。执行 callback 属于模块，绝不写入 JSON。


## 跨 owner LightList 的 Process 生命周期

`RenderIntentExecutionPlan` 与 `RenderLightListUse` 是中性规划值；具体的
`RenderIntentPlanner` 校验与依赖规划属于 Feature-private 实现，位于
`private/render_intent_planner.hpp`。RenderGraph projector、FeatureRuntime 和 focused
planner tests 通过该内部头使用 planner，作者契约不暴露 Feature runtime 的退役证据
或规划 helper。

Runtime 在现有 typed-export 提交事务给真实 PortMessage 附加共享的不可变 source
声明；同一 cull 的多个目标消息在该提交事务内共享，delivery IDs/目标端口保持独立。
callback 不能提供该证据。它包含匹配的 cull 与 CPU provenance，不持有 GPU
句柄、回调或嵌套 receipt，不进入 wire payload/资产序列化，生命周期沿用现有消息队列
及提交意图。声明定义和 validator 位于 Feature private。

只有 Runtime 私有 planner 入口依据 live generation/digest/epoch/Process revision
确认退役后，允许合法 receipt 等待当前生产者：capture 成功空、readiness NotReady。
公共 planner 和同一个验证循环仍拒绝 missing/unauthorized producer 及 scene/depth/
shadow 错配。未消费 B 的真实 delivery 和消费者独立提交不回滚；在途退役生产者的
迟到结果仅清理，恢复必须获得新的当前 receipt。既有 GPU 帧按 FrameContext drain
保留和释放；不另建调度器、缓存或资源管理器。


## 通用 Invocation 运行观察

`FeatureRuntime::inspect_invocations()` 是引擎 owner 线程上的 owning 查询，五域共用。
开放尝试从实际 `invocations_` 取得，逐一沿提交入口的同一 Token/契约/状态/Process
校验计算 `committable`；它不表示 worker 已进入 callback。手工 invoke 可以保留多个
Token，查询按真实 InvocationId 排序，不把多个尝试折成一个 Running 标志。

每个真实 owner 分别保留最后发出的 Token、最后失败的 Result 与阶段、最后实际提交的
Token 和提交后的 StateVersion/ProcessRevision；历史不持有 callable、DLL lease、状态
payload 或 GPU 资源。失败阶段区分 submission、callback、commit validation 和提交后
publication。提交事实写入现有状态/Process/ResourceBus/outbox 事务，publication 后续失败
不撤销已接受状态、消息或命令；显式 transition 与 migration 不伪造 Invocation。非法/伪造/退役 Token
不污染当前失败历史，直接 complete 的合法 Token 在 Delta 被拒后仍可提交有效 Delta。

等待重试另显示实际 origin、下一 ordinal 和 due tick；成功调度重试不是状态提交。
替换/关闭后历史保留原 generation，开放尝试立即反映退役并在既有 owner pump/join
清理；图重新发布的新 owner 从空历史开始。现有 FeatureGraphRuntimeHost 将这些值
复制到同一节点的每个真实 owner 快照，Composite 不生成虚构执行记录。Main UI 已通过真实
bundle 加载、帧边界 Apply、Inspect/Play/Pause/Save 联合流程与冷工程验收。

RuntimeGeneration 是引擎 host 内从不复用的会话/发布身份。构造、成功图发布、定义替换
和关闭使用内部 Concurrency 的同一签发器；图发布直接保留候选 Runtime 的新身份。
同图在另一 Runtime 或关闭后重建时，旧 InvocationToken 和 Process 命令仍被拒绝，
不能因本地 instance/process/attempt 计数重新起算而命中新 owner。generation 不使用
局部加一，不写入作者资产，失败图发布保持原活动代次。
