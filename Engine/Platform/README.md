# Platform

## 目录职责

该目录用于承载操作系统和运行环境适配，包括窗口与 Surface 创建、文件系统、线程
属性、时间源、动态库加载和平台事件转换。

## 公共契约

`public/window.hpp`、`public/window_types.hpp` 和 `public/window_event.hpp` 提供窗口
身份、窗口状态、Surface 绑定和平台事件的引擎语义。具体操作系统实现必须放在
`Application/Platform/private/<platform>`，不得将系统头文件加入本目录公共头文件。

`WindowSurfaceBinding` 中的句柄是不透明值，仅由 Application 平台实现填充；RHI 的
Surface Bridge 负责将其转换为后端需要的 Surface 描述。

`public/platform_services.hpp` 定义剪贴板、光标、IME 和窗口显示指标的后端无关服务。
`IWindowSystem::platform_services()` 返回这些服务的实现；Engine 和 EditorFramework
只能调用该接口，不得依赖操作系统头文件或原生句柄。

`IWindow` 还提供位置、尺寸、显示、聚焦和关闭请求等窗口生命周期操作。这些操作用于
编辑器的 Docking 多视口，但只表达语义状态，不暴露 HWND、X11 Window 或其他 native
句柄。窗口由 Application 平台实现持有，Engine 公共接口只保存带 generation 的
`WindowId`。

## 边界

Platform 是 RHI、Runtime 和 Tooling 的适配基础，但不应包含渲染算法、游戏逻辑或
具体资源格式解析。
