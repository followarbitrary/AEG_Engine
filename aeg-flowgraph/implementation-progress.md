# AEG FlowGraph 实施进度

> 2026-10-18 重新核查：以下为旧 Pipeline 作者阶段的历史记录，不能代表现版完整状态。
> 当前五域实现及实际缺口见 [实现核查与补齐记录](implementation-audit.md)。原计划尚未全部完成，正在补齐。

计划：[AEG FlowGraph 实施计划](<../AEG FlowGraph Implementation Plan.md>)。

工作目录：`E:/Codex Projects/AEG-Engine`。
分支：`master`。本次实施不创建提交。

## 实施裁决

- 用户已确认架构并要求开始实现，本会话直接执行全部计划；计划记录放在项目根目录，专题进度放在 `aeg-flowgraph`，遵循已确认的文档位置和正式命名。
- 用户要求直接在本地 master 工作，不使用 worktree，也不提交；设计文档、计划和实现改动均保留在当前主目录。
- 现有输入契约不含来源字段；通过显式 `InputPortContract` 扩展既有 TypeCatalog，不创建与之并行的节点注册系统。旧资产的边语义只能通过明确迁移进入新语义。
- 三层文档引用 Task 1 契约；Task 2 编码保存同一字段；Task 3 读取同一来源生成数据 DAG；Task 4 由配置和来源映射执行；Task 5/6 只通过事务修改作者字段，接口依赖已核对。

## 任务状态

当前结论：首期“节点目录 → FlowGraph 作者图 → 显式数据流 → 参数/来源配置 → 保存/校验 → 预览/应用 → 基础 Runtime Inspect”闭环已经接通并有回归证据。完整架构方案中的跨领域桥接、子图接口迁移、多 View 执行隔离、GPU/Readback 完成协议、逐资源运行采集、模块契约差异回流和全量验收仍属于后续阶段，当前不会伪装成已经完成。

| 任务                       | 状态                             | 证据                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
|----------------------------|----------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1 节点输入契约与作者校验   | 已完成                           | `aeg_flowgraph_input_contract_smoke` 通过；显式来源、必填无连线 Asset、可选 None、默认版本、状态、SlotId、槽位顺序、边上限、资源节点外部 Asset 输入以及“每条显式输入数据边必须有 Edge 绑定”均有行为测试                                                                                                                                                                                                                                                                                                                                                                       |
| 2 持久化、Cook 编码与摘要  | 已完成                           | TOML/AEGP/模块元数据 JSON 往返测试通过；输入绑定、SlotId、槽位参数、版本化来源和契约摘要均纳入，TOML 保存含临时文件完整性校验与 `.bak` 备份                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| 3 数据流编译、接口与迁移   | 基础完成，持续补齐               | `PreparedPipelineDocument::DataDependency` 已加入；普通数据边参与 Pass 拓扑排序，资源中继派生 Pass 依赖；Legacy Ordering 唯一端口迁移和反例测试已加入，跨域/子图/完整完成协议仍待实现                                                                                                                                                                                                                                                                                                                                                                                         |
| 4 执行身份与预览运行       | 基础完成，持续补齐               | `ExecutionInstanceId`、`ExecutionAttemptId`、状态/完成点和 Runtime stage/apply/stop 代次已接入；候选失败会丢弃 pending 状态并保留 last-good，应用过期草稿会被拒绝；多 View、GPU/Readback 完成协议仍待实现                                                                                                                                                                                                                                                                                                                                                                     |
| 5 编辑事务与节点目录       | 基础完成，持续补齐               | 增删、连线、断开、来源切换、参数命令、复制、布局、SlotId 排序和槽位增删均复用 `EditorCompositionDocument` 历史事务；连接只校验当前端口契约，不再因其他未完成节点阻断草稿编辑；空节点及其持久化标记现在是一个可撤销事务；只读运行图与跨图复制仍待实现                                                                                                                                                                                                                                                                                                                          |
| 6 FlowGraph Editor 界面    | 基础完成，持续补齐               | FlowGraph 已成为唯一作者编排入口；Pipeline 图显示 Pipeline/Feature 节点和数据边，Feature/Pass 图显示所属资源节点及其内建端口；支持节点拖动、稳定布局保存、端口兼容连线和拖线预览、边选中/断开、节点复制、空白区菜单、类型搜索、面包屑返回、Feature → Pass 双击/右键导航、输入来源选择、AssetId 编辑、资源 Asset 输入、Variadic 槽位增删/排序、诊断定位、节点定义面板、自定义脚手架入口和契约参数编辑；面板提供 Validate/Save Draft/Apply to Viewport/Restore Default 操作；已接入只读 Runtime Inspect，可查看实际活动计划的版本、节点、依赖入出度和合成节点；画布已支持 Escape 取消连线、Ctrl/Cmd+Z/Y 撤销重做、Ctrl/Cmd+S 保存、Ctrl/Cmd+D 复制、Delete 删除或断边、F 聚焦选中节点和 A 聚焦全部节点；运行态的逐资源状态、队列屏障与来源回溯仍待实现 |
| 7 契约导出、代码节点与 IDE | 基础完成，持续补齐               | 自定义节点定义面板现在可从项目根目录生成 Pipeline/Feature/Pass 源码、模块注册入口和契约 JSON，并显示实现位置；路径越界、覆盖策略、原子写入、外部 IDE 启动有测试；构建回流、契约差异迁移仍待实现                                                                                                                                                                                                                                                                                                                                                                               |
| 8 全范围交付核验           | 首期闭环已核验，扩展能力持续补齐 | 首期作者编排、保存、校验、预览/应用、基础 Runtime Inspect 和示例启动均有证据；跨领域、GPU/Readback、完整迁移和逐资源运行采集仍未宣称完成                                                                                                                                                                                                                                                                                                                                                                                                                                      |

## 构建与验证记录

- CMake 配置已成功：MSVC 19.44，Visual Studio 17 2022，Vulkan SDK 1.4.357，Editor/Tests/Tools 开启。
- 系统 PATH 没有 cmake/ctest，使用原项目 CMakeCache 记录的 CLion bundled CMake 绝对路径。
- Task 1 目标和现有相关目标构建成功；CMake 输出保留既有 C4456 与 metadata C4244 警告。CTest 运行 9/9 通过，包含新增输入契约测试。
- 本轮验证：`aeg_flowgraph_input_contract_smoke`、`aeg_pipeline_document_io_smoke`、`aeg_pipeline_asset_codec_smoke`、`aeg_module_metadata_smoke`、`aeg_renderer_authoring_smoke`、`aeg_pipeline_composition_runtime_smoke`、编辑器 composition 相关 smoke、`aeg_editor_custom_node_workflow_smoke`、`aeg_generated_flowgraph_nodes_smoke` 均通过；生成的 Pipeline/Feature/Pass 头文件、实现文件、注册文件和契约 JSON 均参与 CMake 增量输出声明并完成编译；`aeg_editor_application` 60 帧回归通过。
- 编辑器主程序目标 `aeg_editor_application` 构建成功；FlowGraph 是唯一作者编排入口，旧 Pipeline 面板及其工作区注册已删除。运行计划和内置管线观察已并入 FlowGraph 的只读 Runtime Inspect 入口，当前展示计划级摘要。
- 本轮补全：连接命令允许保存未完成草稿但严格校验源/目标端口方向、类型、BindingId、资源类别和单输入上限；来源切换会在一个事务中移除旧数据边；复制节点会复制参数、非边来源和 Variadic 槽位但不会复制外部数据边；节点定义面板可生成自定义节点代码脚手架。
- 本轮 FlowGraph 回归：`ctest --test-dir build -C Debug -R "aeg_(editor_composition|editor_custom_node_workflow|generated_flowgraph_nodes|flowgraph_input_contract)" --output-on-failure` 共 11/11 通过；编辑器一帧启动回归退出码为 0。
- Runtime Inspect 投影回归已迁移到当前 Feature/Frame 边界：编辑器只保留 FeatureGraph 作者模型，Frame projection 作为不可变快照进入 Runtime Inspect；旧 `aeg_editor_composition_view_smoke` 已删除。
- FlowGraph 的 Add Node 现在提供“Create Empty Node”：按当前 Pipeline、Feature 或 Pass 图生成可撤销的空白类型目录项和节点实例；空节点没有实现或端口，必须通过节点定义工作流补充契约并重新加载模块后才能进入可执行运行态。
- 自定义节点脚手架测试覆盖三种节点类别、重复文件拒绝、显式覆盖、源目录越界和非法 C++ 类型名；生成操作不启动外部 IDE，返回精确文件/行号/符号定位供 IDE 适配器使用。
- 资源节点现在作为 Feature/Pass 图的一等节点显示；未连接的外部 Texture/Buffer 也能先配置 AssetId，再通过端口建立数据流。普通连线仍是唯一的数据流与执行先后语义，资源节点不会被当作隐式顺序边。
- 空节点的实例与 `@flowgraph:empty-type` 草稿扩展在同一个编辑历史项内，创建后一次 Undo 即可完整撤销；右键节点、双击节点定义和画布空白区菜单均使用独立的 ImGui ID/作用域。
- FlowGraph 面板已接入编辑器的保存草稿、应用到视口和恢复默认视口事务；应用前要求文档诊断为空，应用失败保留作者草稿并继续使用当前可用视口。
- FlowGraph 面板已增加只读 Runtime Inspect 入口；它消费 `EditorSceneRuntime` 的活动计划快照，按来源展示计划版本、节点稳定 ID、依赖入出度和合成节点标记，不允许从运行图反向修改作者图。
- FlowGraph 入口现在使用独立原生视口：点击主窗口左侧 Flow Graph 时保留 Scene/Hierarchy/Inspector 的主布局，并打开或聚焦 FlowGraph；多视口不可用时才回退到原停靠面板。独立窗口首帧使用 1680×945 的作者工作区尺寸，窗口内容直接从图页签开始，不重复绘制窗口标题栏。作者界面按效果稿实现为图页签、面包屑、Node Palette、Canvas、Inspector 和 Compiler Diagnostics 六个区域；原有输入源、参数和可变槽位编辑集中在右侧 Inspector，底部诊断面板始终可见并提供 Compiler Diagnostics、Find Results、Graph Stats 三个页签。Canvas 采用明确的三栏固定宽度预算，离屏节点不会把 Inspector 推出窗口。
- 演示工程的 Deferred 作者文件已补齐资源输入的显式 Edge 绑定并移除未注册的示例空 Pipeline 节点；一帧编辑器启动回归现在无 Pipeline fallback 警告。
