# RenderGraph

## 目录职责

`RenderGraph` 管理帧内 GPU 资源和 Pass 的声明式有向无环图，负责依赖分析、资源
生命周期、显式 Barrier、跨队列同步、Transient 资源分配以及执行提交。

## 当前实现

- `public/render_graph.hpp`：资源、Pass、Plan、Barrier 和执行接口。
- `public/frame_context.hpp`：帧槽、命令提交、完成跟踪和延迟销毁。
- `public/render_feature_runtime.hpp`：将 Pipeline Plan 中的 RenderFeature 展开到图。
- `RenderPassCommandContext`：执行回调只能解析已准备资源并录制命令，不能取得 `Device`
  或创建/销毁 RHI 资源；attachment view 和声明的 sampled-read view 在回调前由 Graph 准备。
- 图创建的纹理与视图通过 FrameContext 保留到 GPU 完成。重建图时可立即释放没有 descriptor
  引用的句柄；仍被当前帧 descriptor 引用的资源在 descriptor 退休后释放，导入纹理本体仍由宿主拥有。
- 宿主替换导入纹理或退出时，可在无活动帧的边界调用 `FrameContext::drain()`：等待已提交帧，
  先清理所有命令引用，再退休 descriptor 和共享所有权。帧编号与暂存分配内容保留到正常帧槽复用。
- `private`：图编译、运行时状态、帧上下文和 Feature Runtime 的实现。

## 依赖边界

RenderGraph 只管理 Render Domain 的图，不承担通用业务 Pipeline 的阶段调度。它
通过 RHI 的后端无关接口提交工作，不能在公共接口中暴露 Vulkan 或 D3D12 类型。
# Custom RenderPass contract

游戏模块中的 RenderPass 继承 `aeg::pipeline::RenderPass`，由 `RenderPassRegistry` 按稳定
`RenderPassTypeId` 创建。Pass 的 setup/execute 回调只能使用 `RenderPassBuilder`、
`RenderPassExecutionContext` 和当前 `RenderPassConfiguration`；资源状态、依赖和同步由
RenderGraph 统一管理。Pass 不得保存 Device、原生命令缓冲或跨帧 Graph Handle。
