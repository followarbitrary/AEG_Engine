# Editor Application

`aeg_editor_application` 是由 Application 持有的最小编辑器宿主。它通过
`aeg_application_platform_win32` 创建 Win32 窗口，创建 RHI Surface 和 Swapchain，持有
`FrameContext` 与 `RenderGraph`，并将编辑器 ImGui RenderFeature 注册到标准
`RenderPipeline`。

Application 不包含 Win32 头文件。窗口消息由 `Application/Platform/private/win32` 转换为
`WindowEvent`，再提交给 `InputSystem` 和 `EditorImGuiHost`。ImGui 绘制数据被复制为
`ImGuiDrawPacket`；`ImGuiRenderer::prepare_frame()` 由受控的
`EngineFrameOrchestrator` RenderBuild Hook 在 RenderFeature 构图前调用。

当启用多视口时，`EditorImGuiHost` 通过 `Engine/Platform` 的 `IWindowSystem` 创建和管理
浮动窗口，并为每个 ImGui viewport 生成带 `WindowId` 的绘制包。Application 负责把这些
绘制包映射到各自的 Surface、Swapchain、FrameContext 和 RenderGraph；EditorFramework 不
接触 native 窗口句柄，也不直接提交 GPU 命令。

字体图集只通过 `Resource::TextureUploader` 上传一次。得到的 RHI Texture、Sampled View
和 Sampler 以借用方式注册到 `ImGuiTextureRegistry`；ImGui 永远不会接收 native GPU 句柄。

Scene Viewport 的拾取和 Transform Gizmo 属于 Application-owned 编辑器交互。拾取只读取
编辑 World 的最新 Snapshot，以 CPU 包围球生成相机射线并更新 Editor-owned Selection；
Gizmo 使用 ImGui DrawList 绘制世界坐标轴，不进入游戏 RenderPass。拖拽过程通过编辑 World
Command Buffer 排队 Transform，鼠标释放提交事务，Esc 取消并恢复事务开始时的值。

Inspector 通过 Runtime Reflection 枚举当前选中实体的公共组件。Transform、Camera、
MeshRenderer 和 Directional Light 字段使用范围校验；多选字段写入同一个编辑事务，拖拽
或直接输入不会按每帧生成历史记录。Name/Tag 属于 Scene Entity 元数据，和组件增删一样
通过 Scene Asset 快照进入 Undo/Redo 与 dirty checkpoint。

## Asset Browser 与导入索引

编辑器不会从引擎仓库或 `CMakeLists.txt` 推断游戏项目。`Project > New Project` 创建带有
`project.aegproject`、`Assets/`、`Scenes/` 和 `.aeg/` 的游戏项目；`Project > Import Project`
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

导入器维护 fast-path 与 bake-path 两条线路：RenderBuild 可以直接解码 OBJ、常见图片格式和
`.aegmat`，后台 watcher 同时把成功结果静默写入项目私有的 `.aeg/DerivedData/`（`.aegmesh`、
`.aegtex`、`.aegmatbin`、`.aegfont`）。烘焙文件带有源 Asset ID 与内容哈希，完成后在下一帧
无感替换 fast-path；项目内已有 `.aeg*` 文件优先按自有格式读取。DerivedData 不进入 Asset
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

可运行交付包中的 `aeg_editor_application.exe` 会从自身所在目录解析着色器、字体和
`DemoProject`，因此启动目录可以是任意目录。默认启动会打开内置演示项目；也可以显式指定
项目：

```text
aeg_editor_application.exe --project E:\Projects\MyGame\project.aegproject
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
