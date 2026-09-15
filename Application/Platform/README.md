# Application Platform

`aeg_application_platform` owns the selected native implementation without exposing platform types to Engine or
Application callers. Platform implementations live in separate `private/<platform>` directories. Alongside window
creation, clipboard, cursor, IME, system-font discovery, native dialogs, and process-interrupt translation, it provides
an RAII startup splash loaded from a caller-supplied image path. Splash creation validates the image and window extent,
displays a centered borderless per-pixel transparent window, and remains non-owning with respect to the editor lifecycle;
closing or destroying the returned `IStartupSplash` releases all native resources.

该目录承载 Application 所拥有的具体平台实现。`Engine/Platform` 只定义窗口、Surface
绑定和事件语义；Win32 的 `HWND`、窗口类、消息循环和 `WndProc` 只能出现在本目录的
private 实现中。

`aeg_application_platform` 通过不暴露具体平台类型的工厂
`aeg::application::create_window_system()` 提供窗口系统。Application 持有返回的
`IWindowSystem`，Engine 和 EditorFramework 只使用 `IWindowSystem`、`IWindow` 和
`WindowEvent`。

窗口系统不会把 `WM_QUIT` 隐式转换成全局 Engine 状态，也不会自动创建 RHI Surface、
Swapchain 或其他 GPU 资源；Application/Runtime 根据抽象窗口事件编排后续生命周期。
