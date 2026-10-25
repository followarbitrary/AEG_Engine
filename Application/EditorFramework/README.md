# EditorFramework

`Application/EditorFramework` 是 Application 侧的编辑器框架。它可以依赖 Engine 的公共
接口，但 Engine 模块不得依赖本目录或编辑器业务数据。

## 目录职责

该目录用于编辑器专用的框架能力，包括编辑器状态、工具窗口、选择集、场景预览、
属性面板和编辑器扩展生命周期。

## 当前状态

本目录当前为 `Experimental`。公共接口允许编辑器集成使用，但仍可能随编辑器渲染
契约演进而调整；Runtime 不得把本目录作为核心运行时的必要依赖。

`EditorImGuiHost` 负责 Dear ImGui Context、事件输入和 Panel 生命周期。项目使用带
Docking 能力的 Dear ImGui UI 核心，但不使用任何官方平台或渲染 Backend。编辑器 UI 的 GPU
资源必须通过 `ImGuiTextureRegistry` 注册 RHI View/Sampler，注册表不拥有这些资源，也不
接收 native 图形 API 句柄。

`EditorImGuiDesc::platform_services` 可绑定 `Engine/Platform` 提供的
`IPlatformServices`，`window_system` 可绑定 `IWindowSystem` 以启用引擎管理的多视口。
`ImGuiPlatformBridge` 通过这些抽象接入剪贴板、光标、IME、窗口位置/尺寸和显示指标；
EditorFramework 不直接调用 Win32 或其他操作系统 API。

启用多视口后，`EditorImGuiHost::capture_draw_packets()` 为每个有效 ImGui viewport
生成一个 `ImGuiDrawPacket`。绘制包带有 generation-checked `WindowId`，浮动窗口的创建、
显示、移动、缩放、聚焦和销毁都通过 `IWindowSystem` 与 `IWindow` 完成。失败的浮动窗口
创建只跳过该 viewport，不破坏主窗口帧。

初始化后可通过 `EditorImGuiHost::capture_font_atlas()` 获取引擎拥有的 RGBA8 字体图集
快照；上层使用 `Resource::TextureUploader` 创建带 `Sampled` capability 的 RHI Texture、
Sampled View 和 Sampler，再将这些句柄注册到 `ImGuiTextureRegistry`，最后调用
`set_font_texture()`。字体图集上传和生命周期仍由上层 Resource/RHI 负责。

编辑器字体解析使用仓库内的 FreeType 和 Dear ImGui FreeType adapter。`EditorFontInspector`
只向 Framework 暴露 family/style、字符集摘要和稳定元数据；TTF 与 CFF OTF 都在 CPU
侧构建 atlas，中文缺字由目录中的 fallback 字体补齐。用户字体目录、导入事务、偏好文件
以及 Win32 文件选择器属于 `Application/Editor` 与 `Application/Platform`；Framework
只接收复制后的后端无关 `EditorFontCatalog`。Preference 的 Preview 不改变应用的 applied
状态，Apply/Cancel 的提交和回滚由 Application Controller 完成。

`ImGuiRenderer` 将宿主捕获的 `ImGuiDrawPacket` 上传到 RHI 资源，私有固定 overlay
入口在 prepare 后向实际目标 RenderGraph 声明资源和 Graphics Pass。
实际命令只能通过 RenderGraph 的 `RenderPassExecutionContext` 录制；
EditorFramework 不得直接访问 Vulkan、D3D12 或其他 native 图形 API。

公共 `imgui_renderer.hpp` 只保留 renderer 所有者与帧准备合同；目标资源 ID 使用
`feature::RenderResourceId`。`public/imgui_render_execution.hpp` 提供固定构图与
多个 Renderer 的 prepared-frame 释放入口，由 Editor 主窗口、偏好窗口与原生视口装配
使用。旧 ImGuiRenderFeature 类、身份和注册/注销已删除。构图校验真实 Device、目标
句柄代次、格式、尺寸及 attachment capability，复用 RenderGraph 私有事务 backend。

字体图集和其他 UI 纹理由上层创建、维护和销毁。上层应将 RHI texture、sampled view、
sampler、纹理描述和初始资源状态注册到 `ImGuiTextureRegistry`；注册表只保存借用句柄。
提交中的 UI 绘制完成前不得更新或注销仍被使用的纹理绑定。

`ImGuiTextureRegistry`、`EditorImGuiHost` 和 `ImGuiRenderer` 均为
`Thread-confined`：同一实例只能在其绑定的编辑器/Render 线程顺序调用，调用方负责跨
线程传递不可变 `ImGuiDrawPacket` 或其他快照。`ImGuiRenderer::prepare_frame()` 返回的
帧级 RHI 资源由 `FrameContext` 保活至对应 GPU 提交完成；销毁渲染器前必须调用
`shutdown()`，该函数会等待设备空闲并释放持久化 Pipeline、BindingLayout 及当前帧资源。

当前覆盖范围包括顶点/索引/投影矩阵上传、采样纹理、alpha blending、scissor、
indexed draw、空绘制包、Docking 和 Engine-owned 多视口平台回调。ImGui 用户绘制回调和
官方多后端输入/渲染 Backend 不在当前公共契约内。

主窗口、Preferences 与 native viewport 按 clear、prepare、固定 overlay、readback/Present
顺序构图。空 packet 仍保留使用 Load 的有副作用 pass；UI 技术 pass 不携带 Scene 业务
Feature provenance。PreparedFrame 在 prepare 时由 FrameContext 保活，graph callback
也持有它。GPU 完成不等于 Renderer 已释放借用纹理的 descriptors；替换共享字体或
SceneColor 前，Application 必须先 drain 所有可能采样的 host、reset 它们的 graph，
再批量释放所有 Renderer 的 prepared frames。双实际 Renderer 的 GPU 测试验证只释放
主 Renderer 时 view 仍不能销毁，完整释放后 view、texture 和 sampler 均能真实退休。

## 边界

编辑器代码不得成为运行时核心模块的必要依赖。运行时构建应能在不链接该目录的
情况下完成；编辑器与 Runtime 之间应通过公开接口、反射或工具协议交互。所有窗口、GPU
资源、命令录制和同步仍必须通过 Engine 的 Platform、RHI、Resource 和 RenderGraph 公共接口完成。
