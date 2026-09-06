# RenderGraph

## 目录职责

`RenderGraph` 管理帧内 GPU 资源和 Pass 的声明式有向无环图，负责依赖分析、资源
生命周期、显式 Barrier、跨队列同步、Transient 资源分配以及执行提交。

## 当前实现

- `public/render_graph.hpp`：资源、Pass、Plan、Barrier 和执行接口。
- `public/frame_context.hpp`：帧槽、命令提交、完成跟踪和延迟销毁。
- `public/render_feature_runtime.hpp`：将 Pipeline Plan 中的 RenderFeature 展开到图。
- `private`：图编译、运行时状态、帧上下文和 Feature Runtime 的实现。

## 依赖边界

RenderGraph 只管理 Render Domain 的图，不承担通用业务 Pipeline 的阶段调度。它
通过 RHI 的后端无关接口提交工作，不能在公共接口中暴露 Vulkan 或 D3D12 类型。
