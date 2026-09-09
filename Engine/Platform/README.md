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

## 边界

Platform 是 RHI、Runtime 和 Tooling 的适配基础，但不应包含渲染算法、游戏逻辑或
具体资源格式解析。
