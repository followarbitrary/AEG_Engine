# Pipeline

## 目录职责

`Pipeline` 是 AEG 的领域导向执行框架，负责描述和编译 Pipeline、Feature、Stage、
Task、Artifact 及其依赖，并负责在声明的 ExecutionDomain 上调度运行时节点。

## 当前实现

- `public/pipeline_types.hpp`：公共 ID、枚举、描述符、Plan 和数据模型。
- `public/pipeline.hpp`：Builder、Feature、Registry、Compiler、Scheduler 和 Scope Host。
- `private/pipeline.cpp`：定义收集、拓扑编译、Artifact Runtime 和 Worker Pool 实现。

## 依赖边界

Pipeline 可以使用 Foundation 和通用 RHI 数据模型，但不应依赖具体 Vulkan/D3D12
类型。RenderGraph 通过受控的 Feature/Runtime 接口接入 Pipeline，不应让 Pipeline
直接持有 RenderGraph 的私有实现。
