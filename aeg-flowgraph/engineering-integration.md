# AEG FlowGraph：工程衔接与架构约束

[返回架构总览](<../AEG FlowGraph Architecture.md>)

本分册把现有 AEG 工程能力与 FlowGraph 方案对齐，说明哪些是已有基础、哪些需要新增契约或工具链能力。当前工程基线和交付证据以 Frame/Render profile 为主；Logic/Flow 的 FlowInstance、StateStore、Transition 事务和 Tick/Event 运行入口仍属于目标语义，不能从已有 Pipeline/RenderPass 基建推断为已实现。问题、反例和关闭条件都按工程责任归入本分册，便于实现、测试和交付逐项核对。

## 1. 现有工程基础

当前仓库已经提供了部分可复用基础：

| 工程位置                                              | 已有事实                                                                         | 对 FlowGraph 的意义                                                                 |
|-------------------------------------------------------|----------------------------------------------------------------------------------|-------------------------------------------------------------------------------------|
| `Engine/Pipeline/public/pipeline_authoring_types.hpp` | 有 TypeCatalog、Pipeline/Feature/RenderPass 类型、PortDescriptor、参数和资源描述 | 作为目录和契约的起点；还需要来源、ValueSchema、槽位和完成点字段                     |
| `Engine/Pipeline/public/pipeline_document.hpp`        | 保存 Pipeline/Feature/Pass/Resource、边、布局和扩展                              | 只能作为旧作者格式基础；需要新增输入绑定、GraphVersion、实例路径、SlotId 和资源版本 |
| `Engine/Pipeline/README.md`                           | 区分 Pipeline、FeaturePoint、RenderPass、Part 和阶段职责                         | FlowGraph 作者层必须经过这些执行边界，不能用图层名称代替运行域                      |
| `tools/module_metadata_tool.cpp`                      | 读取 metadata JSON，计算契约/内容摘要和 manifest                                 | 现有工具不是 C++ 契约自动导出器；首期新增 Contract Exporter host target             |
| 游戏工程 CMake 模板                                   | 源码清单和 metadata 作为模块构建输入                                             | 代码节点向导必须显示构建目标和生成文件变更，不能静默改写手写清单                    |
| `Engine/Module/private/dynamic_module.cpp`            | 有 lease、retiring 和卸载超时保护                                                | 可作为模块代次和引用排空基础；完整候选发布事务仍需补齐                              |
| `Engine/Module/public/module_export.hpp`              | 有模块 ABI 握手宏                                                                | 不能因此假定存在契约导出回调表；需要 Host-safe ContractRegistry                     |
| `Engine/RenderGraph/public/render_graph.hpp`          | 有不可裁剪副作用标记                                                             | 可作为 SideEffect 根的运行落实点，但作者契约仍要记录保活原因和 Receipt              |

## 2. 必须冻结的作者契约

实现者不能从显示名、C++ 类名或现有数组位置猜语义。每个节点类型必须提供：

- TypeId、ContractVersion、OwnerModule、GraphKinds 和源码映射；
- 固定命名端口、ValueSchema、Domain、Scope、Lifetime、Access；
- AllowedSources、PresenceRequirement、BindingSchema、FallbackPolicy；
- Single 或 Variadic 的连接基数、稳定 SlotId、顺序和空集合语义；
- 参数默认值、接口变体、准备/执行阶段、输出完成域和 ResultStatus；
- SideEffectClass、MustExecute、ReceiptType、CommitPolicy、RetryPolicy；
- Purity、Determinism、CachePolicy、MergePolicy 和隐式依赖；
- 支持的目标、队列、平台能力、诊断和迁移规则。

`SourcePolicy` 是旧分册的易读简称，规范字段是 `AllowedSources`。`Optional` 描述 PresenceRequirement，不是一个来源类型。`Default` 与实例 Literal 分开保存。`Cancelled` 是本次工作结果，不是可替代的输入来源。

## 3. 26 项一致性要求

以下要求合并前期问题和第二轮复查，作为实现前的工程基线：

| 编号 | 必须达成的规则                                                  | 失败后果                                     |
|------|-----------------------------------------------------------------|----------------------------------------------|
| G01  | 端口必需性、来源、边数和槽位数量分开判断                        | Asset 输入被误报缺边，或空值被误认为合法资源 |
| G02  | 资源身份与 ResourceValueVersion 分离，写入不能隐式覆盖          | 分叉读写结果不确定                           |
| G03  | CPU、GPU、跨队列、Readback、Asset Prepare 各有完成点            | CPU 录制完成被误当作 GPU 可读                |
| G04  | Sequence 只有真实内部步骤和完成握手才可存在                     | Token 分叉被误认为下游串行                   |
| G05  | 作者层、执行领域、阶段、队列和 Part 分轴建模                    | 子图嵌套越过现有调度边界                     |
| G06  | 作者文档、语义摘要、Cook 产品和运行观察共享稳定身份             | 重开或 Cook 后实例、缓存和诊断串线           |
| G07  | C++ 契约、Exporter、运行注册和 manifest 使用同一 ContractDigest | Editor 目录与 DLL 端口不一致                 |
| G08  | 候选模块、图迁移、资源和运行快照统一提交或保持 last-good        | 新目录与旧回调/旧资源混用                    |
| G09  | TypeCatalog 和作者文档承载完整来源契约与 SlotId                 | Asset、Default 被塞进字符串或假边            |
| G10  | Asset 状态覆盖 Imported、Cooked、DeviceReady、Invalid 等        | 编辑器显示可用但目标设备不能读取             |
| G11  | SideEffect、外部可见性、保活和裁剪根分开声明                    | Capture、Present 或外部写入被裁剪            |
| G12  | Enabled、条件和拓扑变体有唯一语义                               | 运行时偷偷增删端口或资源                     |
| G13  | 缓存、合并、纯度、确定性和隐式依赖进入契约                      | Preview、Play、Cook 结果不一致               |
| G14  | Scope、Lifetime、Queue、Format、ValueSchema 有公开兼容矩阵      | 同名类型跨领域错误连接                       |
| G15  | 子图版本、调用栈、实例缓存和递归拒绝明确                        | 循环展开或新旧子图混用                       |
| G16  | LegacyOrdering 只读迁移，作者新图只写 DataEdge                  | 新旧编译器对同一 JSON 得到不同顺序           |
| G17  | 构建目标、生成文件、并行编辑、取消和回滚有用户模型              | IDE 回流覆盖源码或写入半成品目录             |
| G18  | Runtime Inspect 有完整来源、资源版本、队列和采集成本            | 优化后无法解释实际行为                       |
| G19  | ExecutionInstanceId 隔离每个 View、World、帧和调用路径          | 多次调用串用资源、History 或异步结果         |
| G20  | ResultStatus、AttemptId、取消和重试传播一致                     | 旧异步结果覆盖新尝试或副作用重复             |
| G21  | DefaultId/Revision 与用户 Literal 分开                          | 默认值更新未使旧准备计划失效                 |
| G22  | ValueSchema 和 InterfacePortId 稳定且版本化                     | 单位、颜色空间或接口重排造成错误连接         |
| G23  | 摘要覆盖编译器、能力、图变体和目录版本                          | 编译器升级仍复用旧计划                       |
| G24  | 作者文件原子写入、备份、完整性和语义冲突阻断                    | 崩溃后半写文件或静默覆盖他人语义             |
| G25  | 非幂等副作用声明提交、重试和顺序边界                            | 稳定排序掩盖外部数据竞争                     |
| G26  | Contract Exporter 路径和模块状态 Reset/Migrate/Reject 已冻结    | 目录、运行状态和模块代次不能安全切换         |

## 4. 编译和运行边界

编译器至少经过：解析作者身份 → 解析来源和资产 → 展开版本化子图 → 校验 ValueSchema/Scope/Lifetime/Access → 建立数据 DAG → 分配逻辑资源版本 → 生成队列与内存同步计划 → 检查副作用根和裁剪 → 绑定目标能力。

作者图只规定可解释的数据依赖。物理屏障、队列信号、资源别名、Pass 合并和并行录制由执行图决定，必须能回溯到作者边、资源访问或固定约束。无数据边的 Asset、Default、Literal 和 Context 来源只能产生准备条件，不能伪造上游执行节点。

## 5. 代码节点与模块回流

首期采用：

```text
C++ 契约注册函数
    → Contract Exporter host target
    → canonical TypeCatalog
    → ContractDigest / manifest
    → Editor 离线目录与 Runtime 验证目录
```

Exporter 不加载游戏运行时 DLL，不创建 GPU 对象，不执行游戏帧；Editor 不扫描 C++ 源码猜节点。模块 bundle 必须包含 OwnerModule、SourceRevision、BuildId、ContractDigest、ModuleGeneration、TargetVariant 和信任状态。摘要不一致或未经授权的模块不能进入 Active。

首期不支持新旧模块并存时，明确停止预览、排空 lease、卸载/重载、重新准备、重新应用。状态对象没有迁移协议只能 Reset；构建或重载失败保持 last-good。源码、TypeCatalog、作者图和运行快照不得由一次局部保存互相覆盖。

## 6. 现阶段不应开放

- 任意跨 Domain、Stage、Part 或 Queue 的直接连线；
- 没有 ResourceValueVersion 的隐式原地读写；
- 没有真实完成事件的 GPU→CPU 数据边；
- 无 GraphVersion 的共享子图就地修改；
- Editor 自动扫描 C++、自动加载任意 DLL 或自动把相似资产连入新端口；
- 未声明 Partial、Fallback、Retry 或 CommitPolicy 的隐式容错；
- 只依靠画布位置或稳定排序表达业务副作用先后。

## 7. 工程关闭条件

在实现前，至少要有以下可复现证据：

1. 同一输入分别使用 Edge、Asset、Literal、Default、Context、None 和 Cancelled 时，保存、重开、Compile、Preview 结果唯一。
2. 多 View、多次调用、History、异步 Readback 和重试都能按 ExecutionInstanceId/AttemptId 回溯并隔离。
3. 五类完成点、读写冲突、资源版本和物理复用均有计划与诊断。
4. 子图递归、接口版本、非法跨域和 LegacyOrdering 迁移均能拒绝或生成可复核迁移。
5. 作者文件在崩溃、外部修改和合并冲突后只恢复完整版本，不静默丢语义。
6. Contract Exporter、运行注册、metadata 和 manifest 的摘要一致，模块状态切换符合 Reset/Migrate/Reject。
7. 副作用、裁剪、缓存、合并和运行观察能够从实际结果回溯到契约声明。
