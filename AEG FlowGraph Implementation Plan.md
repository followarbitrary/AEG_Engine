# AEG FlowGraph 实施计划

> 2026-10-18 现版核查：本计划尚未全部完成。原 Pipeline 作者文件已由五域 FeatureGraph
> 架构取代，产品能力逐项映射与本轮补齐证据见 [实现核查与补齐记录](aeg-flowgraph/implementation-audit.md)。
> 下文旧文件职责和 master 分支说明保留为历史计划，不据此恢复已淘汰的作者 API。

**Goal:** 将已确认的 AEG FlowGraph 方案接入现有引擎、作者资产、工具链和编辑器，交付可增删、连接、保存、编译和预览的数据流编排系统。

**Architecture:** 扩展现有 `TypeCatalog`、`PipelineDocument`、`EditorCompositionDocument` 和 RenderGraph 接口。作者层使用显式输入来源与稳定端口/槽位；数据边同时产生结果绑定和执行依赖；准备、执行与运行观察保留完整来源身份。实现直接在本会话推进，逐任务验证并记录进度。

**Tech Stack:** C++20、CMake 3.25+、MSVC、现有 Foundation/Concurrency/Pipeline/RenderGraph/RHI、ImGui、仓库内 JSON/TOML/FlatBuffers。

**Spec:** [架构总览](<AEG FlowGraph Architecture.md>)、[作者与执行契约](aeg-flowgraph/authoring-and-execution-contract.md)、[验收与交付范围](aeg-flowgraph/validation-and-delivery.md)。

## 全局约束

- 普通作者边必须传递真实结果，并限制消费者在结果满足完成条件后执行；新图不能写出纯 Ordering。
- 输入来源、必需性、边数、槽位数分别建模；只有节点声明的 Variadic 组允许增减输入。
- Asset、Literal、Default、Context、None 均不伪造上游节点；活动来源唯一，禁止隐式回退。
- 文档只保存稳定身份和作者语义，不保存函数地址、编译索引、绝对源码路径或 GPU Handle。
- 维持 Pipeline、FeaturePoint、Part、Stage 与 RenderGraph 的现有边界。
- 按设计首期支持 Render Domain、单项目、单目标，跨域、History、Readback 等只在能力与完成协议成立时开放。
- 构建/预览失败保留可恢复作者状态；运行切换、代码重载和文件保存分别使用事务。
- 界面、运行计划和 Cook 产品共享节点、槽位、图版本、契约摘要及来源映射。
- 项目实现直接保留在本地 `master` 工作目录；本轮不创建 worktree、不提交，设计、计划和进度采用稳定名称。

## 验证重点

1. 必需 Texture 输入使用 Asset 时没有边仍合法；缺失、Pending、Invalid、Cancelled 不得当作 Ready。
2. 删除/排序 Variadic 槽位保持 SlotId；一根数组边不改变槽位数量，超限和未知槽位拒绝。
3. 两个相同类型节点、两个 View 和倒序异步结果不能串用输出、History、缓存或运行回调。
4. 默认值变化、类型契约变化、编译目标变化必须使摘要与准备快照失效；布局变化不改变语义摘要。
5. 旧 Ordering、Present、固定结构和未知字段可诊断迁移；崩溃、并发保存不能静默丢数据。

## 任务与文件职责

### Task 1: 节点输入契约与作者校验

**Files:** `Engine/Pipeline/public/pipeline_authoring_types.hpp`、`pipeline_document.hpp`、`pipeline_authoring.hpp`、`Engine/Pipeline/private/flowgraph_input.cpp`、`pipeline_authoring.cpp`、`pipeline_document.cpp`、`tests/flowgraph_input_contract_smoke.cpp`、`CMakeLists.txt`。

**Interfaces:** 产生 `InputSourceKind`、`InputPortContract`、`InputBindingDocument`、`DocumentEndpoint::slot_id` 与 `VariadicInputSlotDocument`；`validate_flowgraph_inputs(const PipelineDocument&, const TypeCatalog&)` 返回带节点、端口和 SlotId 的结构化诊断。现有无扩展契约的节点继续按旧格式读取；显式 FlowGraph 契约决定来源和连接规则。

- [ ] 写真实 TypeCatalog/文档用例：必需 Asset 无边、None 必需性、禁止来源、重复活动来源、Default 版本、Literal 类型、Pending/Cancelled、Variadic 槽位和边数、未知端口与方向。
- [ ] 构建并运行测试，记录功能缺失造成的失败。
- [ ] 实现输入契约、绑定校验和现有文档校验入口；新输入不能退回到“required = 必须连线”。
- [ ] 运行新测试及现有目录、文档校验测试，确认失败路径和旧格式行为。
- [ ] 对照 G01/G09/G21/G22 核对差异，保留本地 master 未提交状态并记录测试结果。

### Task 2: 文档持久化、Cook 编码与规范摘要

**Files:** `Application/Authoring/private/pipeline_document_io.cpp`、`Engine/Module/private/module_metadata_json.cpp`、`Engine/Pipeline/private/pipeline_document.cpp`、对应 I/O、metadata 和摘要测试。

**Interfaces:** Task 1 字段通过 TOML、canonical metadata JSON 和 AEGP 二进制完整往返；`document_semantic_digest(const PipelineDocument&)` 对语义字段进行规范排序与编码。新增格式显式编号，旧格式读取保留迁移信息。

- [ ] 写绑定/SlotId/DefaultRevision 的保存重开和 Cook 往返测试；写重排不变、布局不变、参数/来源/版本变化使摘要改变的测试。
- [ ] 运行确认字段丢失或摘要错误，随后扩展编码、严格解析、未知字段保留和规范摘要。
- [ ] 验证重复键、越界枚举、截断、尾随数据与非有限数值拒绝；旧 payload 读取仍有证据。
- [ ] 增加原子保存、完整性、备份和外部修改冲突验证；按 S11 完成恢复行为。
- [ ] 运行文档/codec/metadata/Cook 相关检查，保留本地 master 未提交状态。

### Task 3: 数据流编译、接口与迁移

**Files:** `Engine/Pipeline/public/pipeline_configuration.hpp`、`pipeline_authoring_types.hpp`、`pipeline_document.hpp`、`Engine/Pipeline/private/pipeline_document.cpp`、新的 FlowGraph 编译/迁移实现和测试。

**Interfaces:** 准备结果提供数据 DAG、每输入解析来源、逻辑资源版本、Graph Output、SourceMap 和诊断；继续产生现有 `PipelineInstanceConfiguration` 供运行层消费。

- [ ] 写手工确定的多源 DAG 顺序、无边资产准备、循环、跨 Part/Stage/Domain、类型/Scope/Lifetime/格式冲突测试。
- [ ] 实现普通数据边的生产消费计划、公开 Graph Input/Output、固定约束和安全资源版本。
- [ ] 写 LegacyOrdering/Present 迁移反例；无法映射时保留原信息并阻断新编译。
- [ ] 实现不可变 GraphVersion、公开 InterfacePortId、调用路径、子图递归拒绝和默认结构物化来源。
- [ ] 验证 S1/S4/S5/S6/S7 的编译部分，保留本地 master 未提交状态。

### Task 4: 执行身份、结果与预览运行

**Files:** `Engine/Runtime/public/pipeline_composition_runtime.hpp`、对应 `.cpp`、Pipeline 配置/执行接口、RenderGraph 来源与资源计划、`Application/Editor/private/composition/editor_composition_preview.*`、运行测试。

**Interfaces:** `ExecutionInstanceId`、`AttemptId`、`ResultStatus`、输出完成点与 ResourceValueVersion；运行观察记录作者映射、保留/裁剪原因、队列和资源版本。

- [ ] 先验证相同图多 View/多调用、失败传播、取消后回调、倒序尝试和候选准备失败。
- [ ] 实现 CPU、GPU 同/跨队列、Asset Prepare 完成条件；Readback/外部副作用只在实际完成事件存在时开放。
- [ ] 数据依赖落实到 RenderGraph 的访问、屏障和队列计划；隐式原地写和外部写竞争拒绝。
- [ ] 完成 Prepared/Applied/Last-good 事务、取消代次、发布安全点和运行观察。
- [ ] 验证 S5/S8/S9/S10，以及裁剪根、缓存和副作用提交，保留本地 master 未提交状态。

### Task 5: 编辑事务与节点目录

**Files:** `Application/Editor/private/composition/editor_composition_commands.*`、`editor_composition_document.*`、`editor_composition_view.*`、目录实现和编辑器模型测试。

**Interfaces:** 来源切换、增删槽位、连接/断开、复制/替换、布局、子图入口均成为一个可撤销事务；图视图消费同一 InputPortContract。

- [ ] 先写来源切换同步移除旧边、删除恢复、复制重建身份、SlotId 排序/撤销、非法连线不修改历史的测试。
- [ ] 实现新增命令并让固定结构、未知类型和只读运行图有可解释的限制。
- [ ] 目录按端口兼容、图层、模块、能力过滤；定义位置和实例配置分别导航。
- [ ] 验证 S1/S2/S2A/S3/S4/S7 的操作模型，保留本地 master 未提交状态。

### Task 6: FlowGraph Editor 界面

**Files:** 编辑器现有 Pipeline 绘制入口、图布局与 Workspace、Inspector、Diagnostics，必要时分出专用绘制文件。

**Interfaces:** Task 5 操作模型连接 ImGui 画布、节点目录、端口与输入来源检查器；Runtime Inspect 消费 Task 4 来源映射。

- [ ] 核对现有绘制入口和交互测试，先写会捕获误连、错误来源/槽位展示和只读越界的回归。
- [ ] 实现三层画布、普通数据端口、拖线候选、按组增项、输入来源设置、增删复制替换和错误定位。
- [ ] 实现搜索、面包屑、定义导航、快捷操作、布局保存和运行只读视图。
- [ ] 构建启动真实编辑器，对照效果参考与 S1–S8 验证，记录实际运行证据并保留本地 master 未提交状态。

### Task 7: 单一契约导出、代码节点与 IDE

**Files:** Module 契约注册/metadata、`tools/module_metadata_tool.cpp`、`EngineTools.cmake`、游戏模板、ProjectOperation、节点定义导航和测试 fixtures。

**Interfaces:** 同一 Host-safe 注册函数生成 exporter 和运行目录；canonical JSON/ContractDigest、BuildId/SourceRevision/ModuleGeneration 形成一致 bundle。

- [ ] 先写同一契约导出/运行摘要一致、旧 metadata 不可进入 Active、脚手架生成不覆盖源码和 IDE 项目解析测试。
- [ ] 实现纯元数据 Contract Exporter host target、构建清单、代码节点模板和类型源码映射。
- [ ] 实现双击打开定义工作区、外部 IDE 打开项目/符号、构建回流、接口差异与迁移预览。
- [ ] 实现停止预览、排空 lease、重载、重新准备流程，状态 Reset/Migrate/Reject 明确。
- [ ] 验证自定义节点工作流和 S12，保留本地 master 未提交状态。

### Task 8: 全范围交付核验

**Files:** 演示工程/测试资产、所有 FlowGraph 相关测试、`aeg-flowgraph/implementation-progress.md` 和正式说明。

**Interfaces:** 最终验收使用正式编辑器、作者文件、Cook 产品和运行结果。

- [ ] 逐条核对首期必须能力、条件开放门槛、G01–G26 与 S1–S12/S2A；标出每项实际证据。
- [ ] 构建所需目标，运行完整适用 CTest 集合和演示回归，报告既有失败及实际影响。
- [ ] 执行代码差异检查，修复影响语义、保存、运行和资源安全的问题。
- [ ] 完成架构/API/用户文档的实际能力说明；仅在所有首期要求经证据确认后完成整体目标。

## 执行记录

任务状态与运行命令记录在 [实施进度](aeg-flowgraph/implementation-progress.md)。设计批准已覆盖实施；本计划是执行分解，执行中依据正式契约作具体裁决并记录，不重复请求设计批准。
