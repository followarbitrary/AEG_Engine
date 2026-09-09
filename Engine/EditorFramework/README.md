# EditorFramework

## 目录职责

该目录用于编辑器专用的框架能力，包括编辑器状态、工具窗口、选择集、场景预览、
属性面板和编辑器扩展生命周期。

## 当前状态

`EditorImGuiHost` 负责 Dear ImGui Context、事件输入和 Panel 生命周期。项目只集成
ImGui Core，不使用官方平台或渲染 Backend。编辑器 UI 的 GPU 资源必须通过
`ImGuiTextureRegistry` 注册 RHI View/Sampler，注册表不拥有这些资源，也不接收 native
图形 API 句柄。

后续 ImGui RenderFeature 将消费 `ImDrawData`，并在 `RenderPipeline.BeforePresent`
阶段通过受限 Render Pass 接口录制绘制命令。

## 边界

编辑器代码不得成为运行时核心模块的必要依赖。运行时构建应能在不链接该目录的
情况下完成；编辑器与 Runtime 之间应通过公开接口、反射或工具协议交互。
