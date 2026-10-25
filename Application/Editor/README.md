# Editor Application

用户操作从[五域编排上手与编辑器操作](../../aeg-flowgraph/editor-experience.md)开始；
该指南包含新建项目、Process、transport、Validate/Compile 和发布状态的完整流程。
本页侧重 Editor 宿主、实现边界和模块维护说明。

`aeg_editor_application` 是由 Application 持有的最小编辑器宿主。它通过
`aeg_application_platform` 创建宿主平台窗口，创建 RHI Surface 和 Swapchain，持有
`FrameContext` 与 `RenderGraph`。固定 `RenderPipeline` 仍提供内部 Frame 技术计划，
编辑器 UI 通过私有固定 overlay 入口构图。

Application 不包含具体平台头文件。窗口消息由 `Application/Platform/private/<platform>` 转换为
`WindowEvent`，再提交给 `InputSystem` 和 `EditorImGuiHost`。ImGui 绘制数据被复制为
`ImGuiDrawPacket`；`ImGuiRenderer::prepare_frame()` 由受控的
`EngineFrameOrchestrator` RenderBuild Hook 调用；clear、prepare、固定 overlay、
readback/Present 按此顺序加入实际 graph。

当启用多视口时，`EditorImGuiHost` 通过 `Engine/Platform` 的 `IWindowSystem` 创建和管理
浮动窗口，并为每个 ImGui viewport 生成带 `WindowId` 的绘制包。Application 负责把这些
绘制包映射到各自的 Surface、Swapchain、FrameContext 和 RenderGraph；EditorFramework 不
接触 native 窗口句柄，也不直接提交 GPU 命令。

字体图集只通过 `Resource::TextureUploader` 上传。得到的 RHI Texture、Sampled View
和 Sampler 以借用方式注册到 `ImGuiTextureRegistry`；ImGui 永远不会接收 native GPU 句柄。

替换字体或 SceneColor 纹理前，共享释放入口收集 main、Preferences 和所有原生 viewport，
先拒绝仍处于活动 Frame 的操作，再 drain 所有 FrameContext、reset 所有 RenderGraph，
最后批量释放 Renderer 的 prepared frames。只有全部成功后才替换或销毁旧纹理。
只等待 GPU 或只释放主窗口 Renderer 不能退休其他 host 持有的 sampled descriptors。

主窗口与 Preferences 的 framebuffer 回归在 overlay 后读回全部256个角落探针。
预期值来自实际 packet 的三角形/scissor、字体图集双线性采样及 alpha 混合，包含 AA
边框和 BGRA 通道顺序；未覆盖区域严格验证 clear。检查同时要求真实 GPU draw 数吻合，
不再以 UI 绘制前的纯黑读回代替最终画面。

编辑器用户字体保存到 `%LOCALAPPDATA%/AEG-Engine/Editor/Fonts`，导入先进入事务目录，
只有 Preference Controller 的 Apply 才提交文件和版本化偏好；Cancel 会恢复 applied catalog
并删除事务文件。TTF 与 CFF OTF 共用 FreeType Inspector，用户字体不改变游戏运行时字体。

Scene Viewport 的拾取和 Transform Gizmo 属于 Application-owned 编辑器交互。拾取只读取
编辑 World 的最新 Snapshot，以 CPU 包围球生成相机射线并更新 Editor-owned Selection；
Gizmo 使用 ImGui DrawList 绘制世界坐标轴，不进入游戏 RenderPass。拖拽过程通过编辑 World
Command Buffer 排队 Transform，鼠标释放提交事务，Esc 取消并恢复事务开始时的值。

Inspector 通过 Runtime Reflection 枚举当前选中实体的公共组件。Transform、Camera、
MeshRenderer 和 Directional Light 字段使用范围校验；多选字段写入同一个编辑事务，拖拽
或直接输入不会按每帧生成历史记录。Name/Tag 属于 Scene Entity 元数据，和组件增删一样
通过 Scene Asset 快照进入 Undo/Redo 与 dirty checkpoint。

## Asset Browser 与导入索引

编辑器不会从引擎仓库或 `CMakeLists.txt` 推断游戏项目。`File > New Project...` 创建带有
`Project.aegproj`、`Assets/`、`Source/`、`Build/` 和 `.aeg/` 的游戏项目；`File > Import Project...`
只接受已有项目文件和 `Assets/` 目录的项目根。只有当前显式打开的游戏项目才会被扫描，未打开
项目时 Asset Browser 只显示内置编辑器资源，刷新操作会给出可恢复错误。

`Application/Editor/private/editor_asset_database.*` 维护当前游戏项目 `Assets/` 目录的编辑器索引。
索引以稳定路径哈希作为初始 Asset ID，并在 manifest 中保存相对路径、类型、导入状态、文件
大小、修改时间、内容哈希和依赖 Asset ID。文件重命名或移动时优先按类型和内容哈希保留身份；
删除的记录保留为 `Missing`，因此场景和 Inspector 可以继续给出可诊断的失效引用。

Asset Browser 支持刷新、搜索、类型过滤、双击预览、重导入、重命名和删除。拖拽 payload 只
包含资源类别和稳定 ID；Inspector 和 Scene Viewport 在接收时校验类别、导入状态和 Edit/Play
模式。资源替换通过编辑 World Command Buffer 进入 Undo/Redo 与 dirty checkpoint，不保存
GPU 句柄或裸指针。

## FeatureGraph 编排

当前分支已提供 `EditorFeatureGraphModel`、FeatureCatalog 过滤、Process 子图和
FeatureGraph TOML 读写。主 FlowGraph 面板会优先打开项目中的有效 `.aegfeature`，在
FeatureGraph 快照上显示模块 FeatureCatalog，并支持添加节点、基础端口连边、节点/边删除、
OwnerPath 级联删除、Undo/Redo 和原子保存；没有有效 FeatureGraph 时显示源文档诊断与创建提示。
连边会读取 metadata 中的端口方向、Schema、版本和时钟，并在事务前
校验 transport 条件；参数以 Feature-owned blob 保存，由 Feature 类型化契约和值控件编辑。
不能复用旧 Pipeline authoring API。

FeatureGraph 是唯一作者入口。Project Settings 使用有序的 `startup_feature_graphs`
列表；FlowGraph 保留选择、编辑、Load Module、Apply 和 Save。Frame/Render 区只读显示
真实 Preview Feature ExecutionPlan 和最近成功帧的 RenderGraph，包括每个来源的
asset、generation、Clock、Definition、Node、Instance、Invocation 和 Module。

默认 Scene Preview 使用独立的 Engine FeatureRuntime 和
`Renderer.Scene.Opaque/Forward/Deferred` 定义；它不修改 Game 的目录摘要或 Play 时钟。
旧 `.aegframe` 文件选择、`Apply to Viewport`、Pipeline sidecar 编辑和启动字段已删除。
旧扩展和旧项目字段会明确诊断；坏 FeatureGraph 保留 last-good 来源和像素。

游戏工程的 `Assets/Features/*.aegfeature` 是独立于场景的 TOML 源文档。通用 Editor 只读取
游戏模块导出的 `FeatureCatalog` 元数据来建立五域 Feature 节点和资源目录；它不会扫描或编译
游戏源码来猜测节点类型。图编辑器始终使用稳定 Definition/Node/Port、版本和 binding ID，
因此显示标题相同也不会产生二义性。Composite、Process 子图使用当前作者快照；
只读 ExecutionPlan 使用已发布的 Runtime generation，RenderGraph 使用 Scene 最后一次成功
执行帧的拥有快照。Pipeline、Stage 和内部 Pass 不进入 Feature 作者目录。
当项目包含多张有效 `.aegfeature` 时，FlowGraph 顶部的 `Select FeatureGraph` 选择器可以
切换当前编辑文档；当前草稿 dirty 时会先拒绝切换，避免未保存编辑被另一张图覆盖。
选择器在 FlowGraph 窗口内部绘制，保留宿主的原生窗口参数；面板绘制返回后才应用
切图请求，确保当前文档在本帧绘制期间保持有效。

FlowGraph 的 Five-domain orchestration 区域固定显示 Logic、Physics、Render、Audio、Stream
五条泳道。一个 Composite 节点可以投影到多个声明域，但所有投影共享原始 NodeId；点击任一
投影都会更新同一个只读 Contract Inspector。Inspector 从当前版本匹配的 FeatureCatalog
契约和作者快照读取 Domain、Structure、Execution、StatePolicy、Scope、Clock、owner path、
端口、Composite children、Process 摘要以及参数/状态 payload 元数据。版本不匹配时会显示
stale contract，而不会把新契约误套到旧节点上。

泳道下方的 transport diagnostics 会列出每条边的 Local、Cross-clock 或 Cross-domain 关系、
transport、两端 Clock/Domain、capacity、deadline 和生产校验器的诊断。跨域或跨 Clock 的 Data
边会保留在视图中并由同一契约校验规则报告。各 transport 按其契约验证；其中 Queue 要求
正数 capacity，Snapshot 要求正数 deadline。

未发布草稿可以在没有 executable module、DLL 或 Feature callback 时使用 `Validate draft` 和
`Compile preview`。预览明确标注 `Authoring preview — not published`，并显示 Clock groups、
authoring provenance 和五域投影；成功结果按源路径、文档身份、draft revision 与 catalog
digest 保存为 last-good。图或目录改变后旧结果标记为 stale，失败的 Compile 只更新定位诊断并
保留旧预览；切换或重新打开文档会清除该缓存。`Apply` 仍然要求真实 executable module，
authoring preview 不创建 Runtime generation、FeatureInstanceId 或执行所有权。

打开图只刷新当前草稿的泳道和诊断；只有点击 `Compile preview` 成功才建立 last-good，
`Validate draft` 不会创建编译结果。非法草稿仍显示自己的节点和边，旧成功结果单独显示在
Compiled authoring preview 区域，并标明 stale。泳道和画布共享选择，跨域投影有独立控件身份；
从泳道选择节点后，画布的删除和聚焦操作作用于该节点，点击画布空白处会清除 Inspector 选择。

面板顶部提供添加节点、连边、校验、编译和发布的操作说明，以及 Node catalog、Check draft、
Process editor 跳转。新项目的默认图包含一个 Logic/Process 节点和 Ready → Running → Done
状态图；离线目录支持立即校验和预览。该示例不绘制场景物体，运行 callback 需要另行构建并
加载模块；首次加载会读取保存的启动图，要采用当前作者草稿时再 Apply。
尚未加载 executable module 时，主面板 Apply 按钮禁用。
Frame/Render 检查默认折叠；Feature definitions 通过显式入口打开，并可独立关闭。
画布、泳道、目录和 Inspector 使用现有 Editor 合同旁车中的可读名称，搜索支持名称和完整 ID。
展示信息必须与 canonical contract 的 ID、owner、版本和摘要一致；同名节点仍以完整 ID 区分。

作者目录扫描、Definition 编辑/生成和实现导航遵循所选 `.aegproj` 的 `paths.source`，
例如配置为 Native 时只从该源码目录读取合同，不混入旧 Source 目录。主 UI 显式转交所选
项目文件；多个项目共享根目录时不能猜测其归属。底层 root-only Service/scanner 接口仍采用
固定 Source 约定，调用者需要配置目录时应提供显式项目文件或 source directory。

参数控件的未 Apply 草稿绑定拥有共享所有权的不可变文档快照。即使两张图使用相同
NodeId、revision 和 catalog，或在同一 document 对象中重开相同文件，Apply 也只使用
当前快照的参数；同一快照重复绘制保留草稿，Undo 后重新加载对应参数。

FeatureCatalog 的 Domain、Execution、Clock 筛选取交集。Clock 下拉来自实际 catalog，
以完整 uint64 ClockId 选择；All clocks 是独立的无筛选状态，catalog 重排保留选择，
被移除的时钟回到 All。筛选不修改文档或 Undo 历史。原生 FlowGraph 把作者控件与
检查视图置于可用鼠标滚轮滚动的内容子窗口，窗口 chrome 和根选择器保持固定。

`Show ExecutionPlan` 展示所选已发布根的实际 Clock 顺序和实例、Process、Invocation、
展开域、模块及作者/runtime OwnerPath；pending 或失败 Apply 保留已发布视图。
`Show RenderGraph` 展示成功帧的实际 pass 顺序、batch、barrier、queue dependencies 和
Feature work 的 invocation/attempt、operation、模块及源归属。当前根、当前 generation
的源行可以选择原始作者 NodeId；其他根或历史 generation 保留归属但禁用源导航。
检查本身不调用 Feature callback，也不复制 GPU 所有权或可变状态 payload。
帧失败保留同项目的最后一次成功捕获；新建、导入或关闭项目清除当前检查入口，调用者
已经取得的拥有快照继续有效。

Process 子图的 `ProcessEditorLayout` 与 `auto_layout_state_machine` 只属于 Editor
实现层，位于 Editor private include surface；Runtime、GameSDK 和其他外部消费者不能通过
`aeg_editor_feature_model` 的公开 usage requirements 取得该布局状态。

Process 子图编辑器支持显式设置入口状态和终态；这些操作与状态、转移编辑共用同一套
FeatureGraph Undo/Redo 事务，并在源文档中保留 `initial_state`/`terminal_states`。
Process owner 的状态图提供状态卡片、转移箭头、输出点连线、拖动、缩放、平移和自动布局；
未连接的新状态也能在作者草稿中显示。拖动预览在释放时保存一次，Escape、换根或重开
会取消旧交互。布局以完整 owner ID 保存在源文档的 `aeg.editor.process-layout.v1:`
扩展中，只改变 `layout_revision`；删除状态、转移或 owner 时在同一事务中清理布局。
正式 Tooling Cook 移除该编辑器扩展命名空间并清零布局版本，布局变动不会改变运行时 AEGF payload。

FeatureGraph 的 `EditorFeatureGraphDocument`、面板、节点定义/合同服务和参数控件同样
属于 Editor composition 实现层，保留在 `Application/Editor/private/composition`。
`aeg_editor_feature_model` 不再通过 PUBLIC include requirements 传播这组头文件；只有
Editor 自身和明确 opt-in 的 focused smoke 才取得 private include path。

所有编辑命令写入 `FeatureGraphDocument` 快照并进入统一 Undo/Redo；当前连接使用共享端口 schema
校验，节点参数使用 Feature 的类型化契约和值控件。保存通过独立的原子源文档 codec 完成，只有替换
成功才推进 saved checkpoint；源文档保留布局和未知扩展。纯布局事务保持 graph_version、
draft_revision、contract_digest 与编译后 ExecutionPlan 不变。
FeatureGraph 源文档只接受新的 `.aegfeature` schema；格式错误会保留当前已加载文档并显示
带节点、端口和 OwnerPath 的诊断。若项目没有可编辑的有效图，主 FlowGraph 面板会逐项
显示 malformed 文件的源路径和解析错误；即使另一个有效图正在编辑，这些诊断也会继续
显示，不会被隐藏成空目录。

导入器维护 fast-path 与 bake-path 两条线路：RenderBuild 可以直接解码 OBJ、常见图片格式和
`.aegmat`，Editor 内部后台导入与 watcher 通过一个双 worker 的 `aeg::concurrency::WorkerExecutor`
`kIoWorker` 执行，并把成功结果静默写入项目私有的 `.aeg/DerivedData/`（`.aegmesh`、
`.aegtex`、`.aegmatbin`、`.aegfont`）。烘焙文件带有源 Asset ID 与内容哈希，完成后在下一帧
无感替换 fast-path；项目内已有 `.aeg*` 文件优先按自有格式读取。该执行器只属于 Editor
实现，不是游戏 Feature/Module 的作者契约。DerivedData 不进入 Asset
Browser，也不会改变场景中的稳定资源引用。

OBJ 会上传真实顶点、索引和包围盒；图片通过 vendored `stb_image` 解码为 RGBA8，支持 PNG、
JPEG、BMP、TGA、GIF、PSD、PIC、PNM 和 HDR；`.aegmat` 的 Texture/texture 引用与 Tint
会参与材质绑定。缺失或首次导入失败才使用 fallback；已有 GPU 版本在重导入失败时保持
last-good，并保留诊断信息。资源退休通过 FrameContext 延迟销毁，避免 GPU 提交仍在使用
时提前释放。

Runtime `TextRenderer` 在 Scene Viewport 以世界空间绘制 ASCII 字形；当前 fast-path 使用
仓库内的 `imstb_truetype` 生成图集并复用同一套纹理、网格、材质上传和 RenderGraph 路径。

使用 `--frames 1` 可执行单帧 smoke；不带参数时，编辑器窗口会保持运行直到关闭。

## 启动与演示

可运行交付包中的 `AetherEngine.exe` 会从自身所在目录解析着色器、字体和
`DemoProject`，因此启动目录可以是任意目录。默认启动会打开内置演示项目；也可以显式指定
项目：

```text
AetherEngine.exe --project E:\Projects\MyGame\Project.aegproj
```

启动失败会显示可恢复错误，并将详细日志写入系统临时目录
`%TEMP%\AEG-Engine\Editor\logs\aeg-editor.log`。项目配置损坏、项目目录缺失或
场景加载失败不会覆盖当前已打开的项目和场景。

最小三分钟演示流程：

1. 在 Assets 面板刷新并导入资源；拖入视口创建实体。
2. 在 Hierarchy 调整父子关系和顺序，在 Inspector 修改 Transform 与材质。
3. 使用 Undo/Redo 对比修改前后状态，然后保存场景。
4. 点击 Play，观察演示网格旋转；Play 使用运行时 World 副本和运行相机。
5. 点击 Stop，确认编辑场景、选择集和 Inspector 数值恢复。
6. 关闭并重新打开项目，确认层级、组件值和项目内相对资源引用保持一致。

New、Open、Save、Save As、切换项目和关闭窗口遇到未保存修改时都会提供
Save / Don't Save / Cancel。保存失败会保留 dirty 状态并阻止后续关闭或切换。


## Feature 定义编辑

Feature Definitions 面板直接编辑 FeatureDefinition 和五域契约；参数约束、端口、状态
payload、时钟、权限、副作用、子 Feature 和 Composite port mapping 使用原生控件。
它复用已有参数值控件，不要求作者编辑 JSON。已编译模块的定义按真实模块目录标记只读。
独立草稿均可在定义列表中恢复，新建 ID 会避开目录和已有草稿的身份。

Save Draft 仅原子写入编辑器 envelope，保留 revision hash、retired IDs 和展示标签，
不发布目录或改变图的历史。Save and Generate 严格校验后更新源文件与目录；草稿 sidecar
退休属于同一个文件事务，失败保留所有原文件和图状态。受影响的真实 FeatureGraph 边按
文档身份和 edge index 处理，未打开的引用图阻止破坏性发布；图和目录共用 Undo/Redo。
生成的六组五域/Composite 源码在测试中只链接 GameSDK，实际编译和注册；execute 仍是
明确返回 Unsupported 的待实现 stub，不能作为游戏行为执行完成的证明。

内部 Frame 视图的旧 Pipeline/Pass 作者创建及定义编辑对话框已移除。动态 Feature
执行 ABI 已提供实际 DLL 回调；Main FeatureGraph 面板的 Load Module、Apply 和
Runtime Inspect 复用同一 Scene owner。模块 manifest 与 configuration 明确输入，
项目匹配、摘要和执行定义由现有选择服务与 Loader 校验；Source metadata 只参与作者编辑。

候选启动根或替换后的完整 active batch 在下一 Scene 帧发布。失败保留 last-good；
pending 时阻止重复 staging。Main 将实际帧间时间单独传入 FeatureRuntime，Play 累积
契约时钟，Pause 只收集已发行工作，Stop 通过 Concurrency cancel/join 后重建代次。
Inspect 直接显示 owning Feature/Node/Instance/Process/Invocation 快照，Process 命令
包含实际 generation、owner、Process ID 和 revision。已打开项目的 Inspect 快照还携带
对应 `.aegfeature` 源路径；项目设置更新后会立即刷新 authoring catalog，避免运行时 owner
继续指向旧的或缺失的编辑器来源。项目退出先退休这些执行所有权。

真实 ImGui 输入/按钮到同一 Scene 的 DLL 回归覆盖 Load/Apply/Inspect、整批根隔离、
时间余量、Pause/Resume、旧代次拒绝与阻塞回调 Stop；模块生命周期测试共用 DLL probe。
Scene 与 Runtime 共用 Renderer 的 Feature 输出投影，使用当前帧实际 SceneColor/Depth
和 resolver；完整 owner 集合维护会话 history，Stop 成功重建后清理。视口唯一读回在
Preview 与 Game Feature 写入之后复制真实 SceneColor。实际 DLL 两根的 4×4 像素回归覆盖
首帧写入、后续 copy-only history、Pause 与 Stop 退休输出。

完整 Module→Editor→Cook→Runtime→Render 综合验收、Demo 与业务 callback 迁移继续实施，
整个五域方案尚未完成。

默认 Scene Preview 现在由真实的 Engine Render FeatureGraph 驱动，提供 Opaque、
Forward、Deferred 三种完整场景能力。Preview 使用独立 Frame clock，Edit/Pause 仍可
绘制，而游戏时钟遵循原 Play/Pause 规则。HDR/MRT/Shadow 目标由当前 View 宿主准备；
空 Preview 图没有隐藏 draw，默认恢复及项目切换重新发布实际 Opaque Feature 根。

Preview 和项目 Feature 的成功帧可以包含不同 Runtime generation。RenderGraph Inspect
保留各源的真实 token/instance/module，按对应源定位作者节点；两个 history 存储独立，
在同一图成功提交时各自提交，失败、退休及重放维持原拒绝语义。旧 `.aegframe`、
Renderer sidecar 和 11 项目录入口已删除。通用 Pipeline 作者文档、旧 UI ordering/layout/
blend helper 与旧项目 header 自动转换链也已删除；Project 只接受明确的版本 2 TOML。
显式现代项目文件按实际选择路径保存，拒绝旧输入时保留当前项目、来源和最近记录。
最近项目记录持有精确文件身份，同目录多个现代项目各自重开；记录和最近场景路径使用
UTF-8。没有精确文件身份的旧 UI 记录不猜 canonical 文件，损坏的路径缓存不阻止有效
项目导入，也不写回或转换旧缓存。
全方案最终验收继续实施。

Build 菜单的 Build / Cook 窗口复用真实 `ProjectOperationService`。它读取当前精确
`.aegproj` 文件和 `module.target`，明确输入 Engine、CMake、配置、模块 manifest 与产物
目录；操作使用已保存文件，不隐式保存内存 draft，也不扫描最新 bundle。窗口显示
实际 Tooling Module/Process/Invocation 的提交、失败和诊断；运行中锁定请求，Cancel
后继续轮询实际执行退休。切换/关闭项目和退出时 cancel、等待最终状态并 join，行为
不依赖操作窗口是否可见。现有 Load Module / Apply / Inspect 继续复用同一 Scene owner。

Main 也支持 `--project <file> --project-operation build|cook|build-and-cook`，可用
`--engine-dir`、`--cmake`、`--target`、`--configuration`、`--module-manifest` 与
`--output-directory` 明确输入；默认 Engine/CMake 来自当前 Editor 的构建配置，target
来自准确项目文件。命令行和按钮共用 start/poll/cancel；失败或有界帧预算结束前未收尾
返回非零，缺项目或非法 operation 在打开窗口前拒绝。Cook 必须给明确模块 manifest。

Flow Graph 的 Load Module 默认保留当前 active graphs；合同升级时可明确选择
Load Saved Project，读取精确项目文件中的完整 startup FeatureGraph 列表。任一根失败
或列表为空会拒绝整个候选并保留原运行批次，成功候选在下一 Scene 帧边界替换全部根。
打开的作者 draft、catalog 与 Undo 保留，加载不会隐式保存或覆盖它们。

仅测试构建包含 Main 工作流输入驱动，联合验收通过真实 ImGui 与工作区动作执行
Build/Cook、Load、Inspect、Play/Pause、Process 编辑、Save 和 Apply。正常命令行操作
仍在完成后退出；测试 flag 可延长同一会话以验证发布边界及真实 DLL 提交。
