# Renderer

该目录承载渲染域的高层语义和视图级数据流，包括 `RenderView`、World-to-Render
提取、可见性过滤、`RenderSceneSnapshot`、标准 `RenderPipeline` 以及渲染特性所需的
后端无关上下文。视锥体属于 Math 的通用几何能力，Renderer 只负责传递相机矩阵和
`ClipSpaceConvention`，并将 Math 的测试结果纳入视图级可见性统计。这里不实现 Vulkan、
D3D12 等后端细节，也不复制 Pipeline Scheduler 或 RenderGraph 的通用调度逻辑。

## 目录职责

该目录用于承载面向渲染域的高层策略和渲染管线，例如 RenderView、可见性策略、
材质绘制策略、渲染特性组合和视图级输出编排。

## 当前状态

当前实现提供 `renderer.hpp` 中的 `RenderView`、`RenderCamera`、
`RenderSceneSnapshot`、`Renderer` 和 `RenderPipeline`。`Renderer` 仅消费不可变的
`WorldSnapshot`，通过注册的提取回调构造不可变场景快照，并可将快照发布为 Frame
Scope 的 `RenderSceneSnapshot` Artifact。`EngineFrameOrchestrator` 在
`RenderExtract` Phase 自动执行该发布流程；没有配置 Renderer 时不会改变原有帧流程。

`RenderPipeline` 提供 Visibility、Shadow、Depth、Opaque、Lighting、Transparent、
PostProcess 和 BeforePresent 的标准 Stage/FeaturePoint 拓扑。具体绘制和计算仍由
RenderFeature 通过 RenderGraph 声明和执行。

## 边界

Renderer 可以消费 World 的渲染快照、Shader Contract、Resource 和 RenderGraph，
但不应直接访问 RHI 后端私有对象，也不应把通用 Pipeline 调度逻辑复制到本目录。
`RenderSceneSnapshot` 只保存后端无关的资产 ID、变换、视图和光照数据；GPU 资源句柄
解析与同步仍由 Resource、RenderGraph 和 RHI 负责。

## 核心数据流

`WorldSnapshot` 是唯一的场景输入。`Renderer` 根据 `RenderView` 和已注册的
`RenderExtractFunction` 生成一个不可变的 `RenderSceneSnapshot`，随后由
`EngineFrameOrchestrator` 将其发布到对应的 Frame Scope Artifact，并在 RenderBuild
阶段把快照放入 `RenderFeatureContext`。RenderFeature 通过
`render_feature_context(builder)` 读取该上下文，再使用 `RenderGraphBuilder` 声明资源、
Pass 和执行依赖。

```text
WorldSnapshot + RenderView
        |
        v
Renderer::extract()
        |
        v
RenderSceneSnapshot (immutable, view-local)
        |
        +--> Frame Scope: RenderSceneSnapshot Artifact
        |
        +--> RenderFeatureContext
                    |
                    v
             RenderGraphFeatureRuntime
```

## 公共接口

### `RenderView`

`RenderView` 描述一个 World/View 对。`world` 与 `view_id` 决定其来源，`id` 是稳定的
视图身份，`camera` 和 `output_extent` 描述视图变换及目标尺寸，
`camera.clip_space_convention` 描述投影矩阵的 Y 轴和深度映射约定，`visibility_mask`
用于层级过滤，`frustum_culling` 控制基于 `math::Frustum` 的包围球裁剪。`color_target` 和 `depth_target`
是 RenderGraph 资源身份，不是后端句柄。

`RenderView::is_valid()` 只校验身份、尺寸和相机数值的语义完整性，不要求输出资源
必须已经导入 RenderGraph。`Renderer::prepare_view()` 可为缺失的 `id` 推导稳定身份，
并为单视图 Swapchain 帧补入默认 Color Target。

### `RenderSceneSnapshot`

该结构是 RenderExtract 的帧内不可变产物，包含：

- 当前帧号、World 修订号和所属 World；
- 已通过 Visibility Mask 和可选 Frustum Culling 的 `RenderInstance`；
- 已通过相同过滤流程的 `RenderLight`；
- `RenderVisibilityStats`，记录候选、可见、Mask 排除和视锥裁剪数量；
- 当前 View 的完整副本。

快照只保存资产身份、变换、边界和光照语义，不持有 Vulkan/D3D12 原生对象，也不负责
GPU 资源生命周期。

### `RenderResourceResolver`

`RenderResourceResolver` 将 `MeshAssetId`、`MaterialInstanceId` 解析为后端无关的
`RenderMeshResource` 和 `RenderMaterialResource`。返回值中的 Buffer、Binding Layout
和 Binding Set 均为公共 RHI Handle；实现可以由 Resource 系统、Asset 系统或宿主应用
提供。

解析策略由 `RenderExtractionDesc::require_resolved_resources` 控制：

- `false`：提取阶段只保留资产身份，适合资源异步加载或 CPU-only 场景提取；
- `true`：每个通过视图过滤的实例都必须能解析出有效 Mesh 和 Material，否则提取失败。

解析器指针只在提取和当前帧 RenderFeature 构建期间借用，Orchestrator 不取得其所有权。

### `RenderExtractContext` 与 `RenderSceneSnapshotBuilder`

自定义提取器通过 `RenderExtractContext` 访问只读 `WorldSnapshot`、当前 `RenderView`、
资源解析器和快照 Builder。Builder 负责：

- 校验对象/光源身份、数值和变换；
- 拒绝重复身份；
- 执行 Visibility Mask 和可选 Frustum Culling；
- 维护可见性统计；
- 在 `finish()` 时按稳定身份排序并转移为共享不可变快照。

提取器不应保存 `RenderExtractContext` 或 Builder 引用；它们只在回调期间有效。

### `RenderFeatureContext`

RenderFeature 在运行时通过以下接口取得 Renderer 上下文：

```cpp
const auto* context = aeg::renderer::render_feature_context(builder);
if (context == nullptr || !context->is_valid()) {
    return foundation::Result::failure(
        foundation::ResultCode::kInvalidState, "render context is unavailable");
}
const RenderSceneSnapshot& scene = *context->scene_snapshot;
```

`scene_snapshot` 指向当前 Frame/View 的不可变快照，`resource_resolver` 可用于在图构建
期间解析 GPU 资源，`user_data` 是宿主透传指针。上下文由
`EngineFrameOrchestrator` 持有到当前 RenderGraph 完成执行之后；Feature 不得延长或
释放这些指针的生命周期。

## 标准 RenderPipeline

`register_render_pipeline()` 注册标准 Frame 级 `RenderPipeline`。该管线只定义
Visibility、Shadow、Depth、Opaque、Lighting、Transparent、PostProcess 和
BeforePresent 的 Stage/FeaturePoint 拓扑；实际 Pass、资源和 Shader 由 RenderFeature
通过 RenderGraph 声明。

RenderPipeline 的实现可以依赖 `RenderSceneSnapshot`，但不能反向修改 World，也不能将
后端对象写入 Renderer 公共接口。资源状态转换、队列同步和物理执行仍由 RenderGraph
与 RHI 负责。
