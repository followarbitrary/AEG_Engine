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

Windows 构建需要 CMake 3.25+、C++20 编译器、Vulkan SDK 和 DXC。第三方源码与离线归档
随仓库提供，详见 [`ThirdParty/README.md`](ThirdParty/README.md)。构建不会从系统安装位置
解析 nlohmann/json、toml11 或 FlatBuffers。

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

## 游戏工程扩展

游戏工程通过 `EngineTools.cmake` 以内联源码方式接入引擎。开发者在 `Source/Render/`
下实现继承自专有基类的 Pipeline、领域 Feature 和 RenderPass，并在模块声明中注册
`TypeCatalog` 元数据。通用 Editor 从 `module.metadata.json` 建立三层 Pipeline/Feature/Pass
节点目录，因此识别新游戏代码不需要重新编译 Editor。

```powershell
$engine = (Resolve-Path .).Path
cmake -S MyGameProject -B MyGameProject/Build/Debug "-DENGINE_DIR=$engine"
cmake --build MyGameProject/Build/Debug --config Debug --target MyGameExecutable
```

Cooker 会把 `.aegpipe` 和启动场景写入 `game.aegbin`，静态游戏启动时校验 Build ID、模块
契约摘要、包摘要和所有启动 Pipeline 的语义。第三方依赖已随仓库放在 `ThirdParty/`，
无需从系统目录或网络临时下载。

## 文档

- [引擎开发契约](AEG%20Engine%20Contracts.md)
- [Domain-Oriented Pipeline Architecture](AEG%20Domain-Oriented%20Pipeline%20Architecture.md)
- [编辑器启动与演示](Application/Editor/README.md)

## 资产目录边界

`Engine/Asset` 只放运行时必须共享的资产 ABI：Asset UUID/类型、Cooked envelope
与 FlatBuffers schema、`.aegbin` Reader、句柄和 payload 解码。`Application/Tooling`
放源资产 `.meta`、Importer、Cook cache、依赖遍历和 package Writer。Tooling 可以调用
Engine 的编解码接口，但 Engine 不依赖 Tooling；因此“资产由 Application 生成”不等于
“资产运行时契约属于 Application”。

通用向量、矩阵、四元数、变换、投影和几何算法统一位于
[`Engine/Math`](E:/Codex%20Projects/AEG-Engine/Engine/Math)，由 `aeg_math` 提供；上层
模块不得复制这些基础类型。

Logo 与品牌资源位于 [`Assets/Branding`](Assets/Branding/)。

发布回归会在隔离目录运行 120 帧，并检查包外没有 Source 或 Editor 依赖：

```powershell
ctest --test-dir build -C Debug --output-on-failure -R "^aeg_game_project_runtime$"
```
