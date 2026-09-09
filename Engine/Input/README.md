# Input

## 目录职责

该目录负责将 `Engine/Platform` 产生的窗口事件转换为帧级输入状态，并为逻辑、编辑器
和工具系统提供确定性的事件序列与快照。

## 边界

Input 不包含 Win32、X11、SDL 或图形 API 类型。平台实现只负责产生
`platform::WindowEvent`；输入系统负责事件排队、帧边界提交和状态折叠。具体窗口的
所有权仍属于 Application。
