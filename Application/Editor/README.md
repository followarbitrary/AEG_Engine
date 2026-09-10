# Editor Application

`aeg_editor_application` is the minimal Application-owned editor host. It creates the
Win32 window through `aeg_application_platform_win32`, creates the RHI surface and
swapchain, owns the `FrameContext` and `RenderGraph`, and registers the editor ImGui
RenderFeature with the standard `RenderPipeline`.

The application does not include Win32 headers. Window messages are converted by
`Application/Platform/private/win32` into `WindowEvent` values, which are submitted to
`InputSystem` and `EditorImGuiHost`. ImGui draw data is copied into an
`ImGuiDrawPacket`; `ImGuiRenderer::prepare_frame()` is called from the controlled
`EngineFrameOrchestrator` RenderBuild hook before the RenderFeature graph is built.

The font atlas is uploaded once through `Resource::TextureUploader`. The resulting RHI
texture, sampled view, and sampler are borrowed by `ImGuiTextureRegistry`; ImGui never
receives a native GPU handle.

Run with `--frames 1` for a one-frame smoke run. The normal invocation keeps the editor
window open until it is closed.
