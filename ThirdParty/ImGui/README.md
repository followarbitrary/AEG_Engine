# Dear ImGui

该目录包含 AEG 固定版本的 Dear ImGui Docking 源码。Docking 和 viewport 代码只用于 UI
布局与交互，项目明确不编译任何官方平台 Backend 或渲染 Backend。

引擎负责把 Platform 事件转换为 ImGui 输入，并把 `ImDrawData` 转换为后端无关的 RHI 与
RenderGraph 操作。Dear ImGui 不拥有 AEG 的 GPU 资源、Swapchain、命令录制或同步状态。
多视口窗口由 `Engine/Platform` 抽象创建，浮动 viewport 的绘制目标和提交仍由 Application
侧的 RHI/RenderGraph 桥接负责。

版本：Dear ImGui `v1.93.0 WIP`，Docking 分支提交
`367b2c24f399988ddafc0bb4628da0106bcc09be`。
