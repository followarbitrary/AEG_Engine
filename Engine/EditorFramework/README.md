# EditorFramework

## 目录职责

该目录用于编辑器专用的框架能力，包括编辑器状态、工具窗口、选择集、场景预览、
属性面板和编辑器扩展生命周期。

## 当前状态

本目录当前为 `Experimental`。公共接口允许编辑器集成使用，但仍可能随编辑器渲染
契约演进而调整；Runtime 不得把本目录作为核心运行时的必要依赖。

`EditorImGuiHost` 负责 Dear ImGui Context、事件输入和 Panel 生命周期。项目只集成
ImGui Core，不使用官方平台或渲染 Backend。编辑器 UI 的 GPU 资源必须通过
`ImGuiTextureRegistry` 注册 RHI View/Sampler，注册表不拥有这些资源，也不接收 native
图形 API 句柄。

初始化后可通过 `EditorImGuiHost::capture_font_atlas()` 获取引擎拥有的 RGBA8 字体图集
快照；上层使用 `Resource::TextureUploader` 创建带 `Sampled` capability 的 RHI Texture、
Sampled View 和 Sampler，再将这些句柄注册到 `ImGuiTextureRegistry`，最后调用
`set_font_texture()`。字体图集上传和生命周期仍由上层 Resource/RHI 负责。

`ImGuiRenderer` 将宿主捕获的 `ImGuiDrawPacket` 上传到 RHI 资源，并由
`ImGuiRenderFeature` 在 `RenderPipeline.BeforePresent` 阶段声明 RenderGraph 资源和
Graphics Pass。实际命令只能通过 RenderGraph 的 `RenderPassExecutionContext` 录制；
EditorFramework 不得直接访问 Vulkan、D3D12 或其他 native 图形 API。

字体图集和其他 UI 纹理由上层创建、维护和销毁。上层应将 RHI texture、sampled view、
sampler、纹理描述和初始资源状态注册到 `ImGuiTextureRegistry`；注册表只保存借用句柄。
提交中的 UI 绘制完成前不得更新或注销仍被使用的纹理绑定。

`ImGuiTextureRegistry`、`EditorImGuiHost` 和 `ImGuiRenderer` 均为
`Thread-confined`：同一实例只能在其绑定的编辑器/Render 线程顺序调用，调用方负责跨
线程传递不可变 `ImGuiDrawPacket` 或其他快照。`ImGuiRenderer::prepare_frame()` 返回
的帧级 RHI 资源由 `FrameContext` 保活至对应 GPU 提交完成；销毁渲染器前必须调用
`shutdown()`，该函数会等待设备空闲并释放持久化 Pipeline、BindingLayout 及当前帧资源。

当前覆盖范围包括顶点/索引/投影矩阵上传、采样纹理、alpha blending、scissor、
indexed draw，以及空绘制包。ImGui 用户绘制回调和多后端平台输入/渲染 Backend 不在
当前公共契约内。

## 边界

编辑器代码不得成为运行时核心模块的必要依赖。运行时构建应能在不链接该目录的
情况下完成；编辑器与 Runtime 之间应通过公开接口、反射或工具协议交互。
