<p align="center">
  <img src="Assets/Branding/aether-engine-graph-logo-final.png" alt="AEG Engine logo" width="420">
</p>

# AEG Engine

AEG Engine 是一个使用 C++20 构建的模块化游戏引擎与编辑器项目。它以明确的生命周期、显式依赖和
后端无关的系统边界组织 World、Scene、ECS、Pipeline、RenderGraph、RHI、资源与编辑器能力。

## 当前能力

- Domain-oriented Pipeline、Stage、Task、Feature 与 Artifact 依赖编排
- ECS World、不可变快照、Scene 文档、层级和序列化
- 后端无关 RHI、Vulkan 后端、RenderGraph 与资源上传
- ImGui 编辑器工作区、Scene Viewport、Hierarchy、Inspector、Assets 与 Pipeline 编辑
- C++ 模块注册、反射、日志、数学、并发与平台适配

## 构建

Windows 构建需要 CMake 3.25+、C++20 编译器、Vulkan SDK 和 DXC。第三方源码随仓库提供。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target aeg_editor_application
```

启动编辑器：

```powershell
.\build\AetherEngine.exe
```

也可以显式打开游戏项目：

```powershell
.\build\AetherEngine.exe --project E:\Projects\MyGame\project.aegproject
```

## 文档

- [引擎开发契约](AEG%20Engine%20Contracts.md)
- [Domain-Oriented Pipeline Architecture](AEG%20Domain-Oriented%20Pipeline%20Architecture.md)
- [编辑器启动与演示](Application/Editor/README.md)

Logo 与品牌资源位于 [`Assets/Branding`](Assets/Branding/)。
