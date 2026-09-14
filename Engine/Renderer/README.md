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
PostProcess 和 BeforePresent 的标准 Stage/FeaturePoint 拓扑。具体绘制和计算由
RenderFeature 通过 RenderGraph 声明和执行；当前还提供 Experimental 的
`OpaqueRenderFeature`，用于把已提取的 MeshRenderer 实例发出为最小的 indexed
opaque draw。

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

`RenderResourceResolver` 将 `MeshAssetId`、`MaterialInstanceId` 和视图内
`RenderInstance` 解析为后端无关的 `RenderMeshResource`、`RenderMaterialResource`、
`RenderObjectResource` 与帧内 `RenderLightingResource`。Mesh 资源包含 Vertex/Index
Buffer 及其不可变创建描述，并通过 `RenderMeshVertexLayout` 明确标准 Opaque 顶点的
Position/UV/Normal 格式和 interleaved byte offset；Material 资源选择 Graphics Pipeline；
Object 资源提供当前 View 所需的 `RenderObjectConstants` Uniform Buffer、Binding Layout
和 Binding Set；Lighting 资源提供 group 2 的帧/View-local `RenderLightingConstants`
BindingSet。所有返回值均是公共 RHI Handle；实现可以由 Resource 系统、Asset 系统或宿主
应用提供。

解析策略由 `RenderExtractionDesc::require_resolved_resources` 控制：

- `false`：提取阶段只保留资产身份，适合资源异步加载或 CPU-only 场景提取；
- `true`：每个通过视图过滤的实例都必须能解析出有效 Mesh 和 Material，否则提取失败；
  Object Binding 会在具体 RenderFeature 构图时按 View 解析。

解析器指针只在提取和当前帧 RenderFeature 构建期间借用，Orchestrator 不取得其所有权。
实现必须让返回的 RHI 资源至少存活到该帧 RenderGraph 执行结束。

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

### `OpaqueRenderFeature`

`opaque_render_feature.hpp` 提供 `register_opaque_render_feature()` 和
`bind_opaque_render_feature()`，后者把该 Experimental Feature 绑定到标准
`RenderPipeline.Opaque` 扩展点。它读取当前 `RenderSceneSnapshot`，逐个解析 Mesh、
Material 和 View-local Object Binding，并声明 `DrawOpaque` Graphics Pass。Pass 会显式
读取 Vertex/Index/Uniform Buffer，并向 `RenderView::color_target` 写入 Color Attachment；
当 `RenderView::depth_target` 有效时，还会声明并清除 Depth/Stencil Attachment。因此
Resource Resolver 提供的 Buffer 描述必须包含相应的 RHI capability，调用方提供的 Depth
Texture 必须声明 `kDepthStencil` capability。

调用方必须先将 `RenderView::color_target` 对应的外部或图内 Texture 绑定到
RenderGraphFeatureRuntime。使用 `EngineFrameOrchestrator` 时，应在
`EngineFramePreparationContext` 中导入目标并以同一稳定 `RenderResourceId` 绑定。当前
最小 Shader Contract 位于 `shaders/core/opaque/unlit_mesh.vert.hlsl` 与
`shaders/core/opaque/unlit_mesh.frag.hlsl`，固定使用 group 0 Object、group 1 Material、
group 2 Lighting。顶点 Position 为 `float4`、UV 为 `float2`、Normal 为 `float3`；顶点
Shader 输出插值 UV 和 View-space Normal。材质参数块继续提供稳定的 `float4` Tint，片元
Shader 按插值 UV 采样纹理，并计算
`albedo * tint * (ambient + color * intensity * max(dot(normal, -direction), 0))`。
当前实现只支持一个 Directional Light，不包含阴影、镜面反射、PBR 或多光源循环；没有
Directional Light 时由 `make_render_lighting_constants()` 生成环境光回退。

Lighting Uniform 必须作为当前帧/View 的新版本上传，并通过 `FrameContext` 的延迟销毁
路径管理旧 Buffer 和 BindingSet；禁止在 GPU 仍可能使用时原地修改持久 Descriptor。
Opaque Pass 会显式声明 group 2 Lighting Buffer 的 Fragment `kUniformRead`，以及材质纹理、
Tint Buffer、Object Buffer 的对应读取。缺失声明、过期 Graph Handle、法线/UV 布局不匹配
或 Buffer 范围不完整会在构图阶段失败，而不是让 Feature 继续提交不完整的绘制。

### 编辑器 Scene Viewport

编辑器的 `Scene Viewport` 使用独立的离屏 Color/Depth Texture，并在每个
`EngineFramePreparationContext` 中以稳定的逻辑资源身份导入 RenderGraph。Viewport 尺寸
变化只在帧边界重建 Texture、View 和相关图资源；正常的材质、对象和光照更新则使用
`FrameContext` 的帧槽资源，不通过每帧 `wait_idle()` 规避生命周期问题。

Opaque 写入完成后，颜色目标必须显式转换为 Fragment `kSampledRead`，ImGui 只能借用该采样
View 绘制预览。颜色目标、深度目标、材质纹理、Tint Uniform 和 Lighting Uniform 都必须在
当前帧重新取得有效 Graph Handle；编辑器不得保存跨帧 Handle，也不得把 ImGui 纹理注册表当作
GPU 资源所有者。

## 标准 RenderPipeline

`register_render_pipeline()` 注册标准 Frame 级 `RenderPipeline`。该管线只定义
Visibility、Shadow、Depth、Opaque、Lighting、Transparent、PostProcess 和
BeforePresent 的 Stage/FeaturePoint 拓扑；实际 Pass、资源和 Shader 由 RenderFeature
通过 RenderGraph 声明。

RenderPipeline 的实现可以依赖 `RenderSceneSnapshot`，但不能反向修改 World，也不能将
后端对象写入 Renderer 公共接口。资源状态转换、队列同步和物理执行仍由 RenderGraph
与 RHI 负责。
