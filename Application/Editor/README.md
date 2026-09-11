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

使用 `--frames 1` 可执行单帧 smoke；不带参数时，编辑器窗口会保持运行直到关闭。
