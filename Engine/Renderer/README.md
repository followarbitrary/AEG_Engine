# Renderer

## 目录职责

该目录用于承载面向渲染域的高层策略和渲染管线，例如 RenderView、可见性策略、
材质绘制策略、渲染特性组合和视图级输出编排。

## 当前状态

目录目前仅作为架构预留。当前帧内资源图和 Pass 编译由 `RenderGraph` 负责，底层
GPU 对象由 `RHI` 负责；未来 Renderer 应在两者之上提供领域语义，而不是重复实现
资源状态跟踪。

## 边界

Renderer 可以消费 World 的渲染快照、Shader Contract、Resource 和 RenderGraph，
但不应直接访问 RHI 后端私有对象，也不应把通用 Pipeline 调度逻辑复制到本目录。
