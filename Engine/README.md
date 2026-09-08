# Engine

## 目录职责

`Engine` 是 AEG 的引擎模块源码根目录。一级目录按照运行时职责和依赖边界划分，
模块通过稳定的 `public` 接口向上层暴露能力，通过 `private` 保存实现细节。

## 一级模块

| 目录              | 职责                                                 | 状态     |
|-------------------|------------------------------------------------------|----------|
| `Foundation`      | 结果类型、错误传播和最低层通用定义                   | 已实现   |
| `Logging`         | 统一日志、日志级别和 Quill 适配                      | 已实现   |
| `Math`            | 标量/SIMD 数学与图形变换基础                         | 已实现   |
| `Pipeline`        | Pipeline、Feature、Artifact、Compiler 和 Scheduler   | 已实现   |
| `RHI`             | 后端无关的 GPU 资源、命令和同步抽象                  | 已实现   |
| `RenderGraph`     | 帧内资源图、Pass、Barrier 和队列同步                 | 已实现   |
| `Resource`        | Mesh、Texture、Material、Render Target 和上传        | 已实现   |
| `Shader`          | Shader Asset、Reflection、Contract 和布局信息        | 已实现   |
| `Runtime`         | Engine 生命周期与统一帧编排                          | 已实现   |
| `Renderer`        | RenderExtract、RenderSceneSnapshot 和 RenderPipeline | 基础实现 |
| `ECS`             | World 数据组织和实体组件系统                         | 已实现   |
| `World`           | ECS 与 World Scope Pipeline 的数据流适配             | 基础实现 |
| `Concurrency`     | 通用任务系统、线程协调和并发原语                     | 基础实现 |
| `Platform`        | 操作系统、窗口、文件和平台适配                       | 预留     |
| `Diagnostics`     | 断言、性能分析、崩溃和运行时诊断                     | 预留     |
| `Reflection`      | 类型元数据和运行时反射                               | 预留     |
| `Serialization`   | 配置、存档和资产序列化                               | 预留     |
| `EditorFramework` | 编辑器专用框架与工具扩展                             | 预留     |
| `Tooling`         | 离线工具、代码生成和构建辅助                         | 预留     |

## 目录约定

- `public` 只放跨模块可依赖的声明和稳定数据模型。
- `private` 放实现、内部辅助类型和不属于公共 ABI 的代码。
- 后端专有实现必须留在对应模块的 `private` 子目录中。
- 预留目录可以先保存架构文档，但在形成稳定 API 前不应被其他模块依赖。

模块依赖应保持从基础设施到领域服务的单向关系；上层模块不得通过私有路径绕过
公共边界，也不得把平台或图形后端类型泄漏到通用模块。
