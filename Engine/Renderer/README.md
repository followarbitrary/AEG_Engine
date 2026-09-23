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

## Deferred 与 Forward 管线

Renderer 同时注册两个可由项目 `.aegpipe` 选择的内置管线：

- `Renderer.DeferredRenderPipeline`：`RenderExtract`、`Shadow`、`GBuffer`、
  `DeferredLighting`、`SkyboxPostProcess`、`BeforePresent`；
- `Renderer.ForwardRenderPipeline`：`RenderExtract`、`Shadow`、`ZPrepass`、
  `LightCulling`、`ForwardOpaque`、`SkyboxPostProcess`、`BeforePresent`。

七个 RenderPass 类型通过 `register_builtin_render_passes()` 注册，
`register_builtin_rendering()` 负责装配运行时；`register_renderer_authoring_types()`
把同一组管线、Feature、Pass、端口和资源约束加入 Editor 的 `TypeCatalog`。

当前 Pass 先形成可执行的 RenderGraph 拓扑：Deferred 创建 ShadowMap 和三张 GBuffer，
其中 GBuffer 在提供 `RenderMaterialResource::gbuffer_pipeline` 时会解析网格、材质、
对象资源并发出 indexed draw；Deferred Lighting 采样 Albedo、编码 Normal 和 Depth，
执行共享方向光与环境光契约的全屏绘制。Forward 的 `ForwardOpaque` 复用同一套资源解析和绘制路径，
并声明对 Z-prepass 深度、ShadowMap 与 LightClusters 的依赖。外部 SceneColor/SceneDepth
由 FrameContext 绑定，临时资源由 RenderGraph 管理。

Z-prepass 通过 `RenderMaterialResource::depth_pipeline` 执行几何体深度绘制，ForwardOpaque
加载其深度结果。宿主通过 `DeferredLightingResources` 创建全屏光照 pipeline，并在
`resolve_deferred_lighting()` 中返回当前帧的绑定服务；GBuffer 的临时采样视图由执行上下文
解析，descriptor 每次执行单独分配并交给 FrameContext 延迟销毁。有几何体却缺失这些
资源时构图返回错误；空场景允许只清屏。光照参数复用 `RenderLightingResource` 的 uniform
范围，和 GBuffer 视图一起绑定到全屏 shader 的 group 0；Opaque 的 group 2 契约不变。

Shadow 已执行方向光视图的深度绘制，前向与延迟 shader 均可采样 ShadowMap。
Forward 的 LightCulling 在计算队列按 16×16 像素分块，使用 Z-prepass 深度和视空间
光源范围生成点光源/聚光灯列表；ForwardOpaque 通过 `clustered_pipeline` 消费列表，
叠加距离衰减与光锥衰减，并保留方向光阴影。宿主通过 `LightCullingResources` 准备当前帧
资源，借用同一 ShaderManager 的 group 3 布局；帧结束后旧绑定不可重用。
最多支持 1024 个有效局部光源，超出时显式报错。空场景不派发计算。
Editor 的 Add Component 菜单可添加 Point Light 和 Spot Light。组件支持颜色、强度、世界单位
范围、可见性掩码和启用状态；Spot Light 另有局部方向及内外光锥余弦，方向随 Transform
旋转。组件可随场景保存、重开，并通过快照提取进入前向光源列表。
Editor 对象法线使用模型—视图矩阵的逆转置，方向光也转换到视空间，保持两条管线的光照一致。
Skybox/PostProcess 现在执行全屏绘制：光照先写入 `Renderer.SceneLighting`（RGBA16F），
末尾 pass 读取 HDR 颜色和深度，在背景处按世界空间视线绘制程序化天空，再应用曝光、
可选 Reinhard 色调映射与 Gamma，输出到 `Renderer.SceneColor`。前向 `clustered_pipeline`
与延迟光照 pipeline 必须使用 `kSceneLightingFormat`；最终输出格式由 `PostProcessResources`
初始化时指定。有无几何体都需要宿主提供 `resolve_post_process()`，缺失时明确报错。
Editor 可添加 Scene Environment 组件，设置天顶、地平线、地面颜色及后处理参数，并随
场景保存。每个视图最多消费一个启用的环境；没有环境组件时采用关闭天空、曝光/Gamma
为 1 的兼容设置。此实现提供程序化天空，不包含环境贴图资产导入。
七个内置 Pass 均通过宿主 `RenderPassRegistry` 的 `setup/execute` 执行。Feature 负责解析和导入
帧资源，然后将不可变的帧命令交给注册的 Pass；Pass 通过受限 RenderGraph 上下文声明资源并录制命令。
宿主须提供 `RenderFeatureContext::render_passes`（使用帧编排器时设置 `EngineFrameDesc::render_passes`），
与编译项目文档使用同一注册表。图保留解析出的 Pass 和帧配置，注册表后续变动不影响已构建的帧。
缺少注册表、Pass 类型或已准备的帧命令会明确失败；帧命令不会序列化到 `.aegpipe` 文档。

Editor 的 Pipeline 面板选中项目内置管线文档后，可用 `Apply to Viewport` 应用到场景视口。
候选文档先验证并编译，成功后在 Edit 模式的帧边界替换 Frame Host；编译或项目保存失败
会保留原来的管线。选择写入项目的 `startup_pipelines`，保留其他项目管线，重新打开项目
时自动恢复。`Use Default Renderer` 恢复兼容管线并移除内置管线选择。
两种新 Feature 均输出 Render State，经现有 Present 合成流程输出到视口。
内置 Feature 要求 authored Feature 保留完整默认 Pass 拓扑；禁用、缺失、改变队列或使用不支持的
资源重绑定都会在编译时返回结构化错误，不会忽略配置后继续绘制。

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
兼容 Opaque 实现只支持一个 Directional Light，不包含阴影、镜面反射、PBR 或多光源循环；没有
Directional Light 时由 `make_render_lighting_constants()` 生成环境光回退。

Deferred/Forward 变体支持该方向光的真实阴影：`casts_shadow` 控制是否从光源视角绘制
深度，并在光照中进行 3×3 PCF 深度比较，只衰减直接光。`RenderSceneSnapshot::shadow_casters`
保留通过可见性掩码但被相机视锥裁掉的物体，避免画面外投影物体丢失。阴影视图按世界包围体
拟合，采用独立 View ID；宿主必须按 `(View ID, Object ID)` 提供不会互相覆盖的对象常量。
当前为单张方向光阴影图，尚无级联阴影或点光源阴影。

`ShadowResources` 在宿主侧创建每帧常量及描述符，通过 `FrameContext` 延迟回收。
前向材质提供 `shadow_pipeline`，其 group 3 的 BindingLayout 传给该资源服务；该 Layout
必须保持到所有使用它的 GPU 帧完成。延迟宿主向 `DeferredLightingResources::initialize`
提供可选阴影 Shader 包。无阴影的已有宿主不需要这些可选资源。

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

# Renderer pipeline、feature 与 pass

Deferred/Forward Pipeline 负责定义 RenderExtract、RenderBuild、BeforePresent 等阶段和 FeaturePoint。渲染能力由 RenderFeature 贡献；Feature 内的 Shadow、GBuffer、Lighting、PostProcess 等步骤才是 RenderPass。内置 VariantFeature 保留专用图构建，因此不会再次走作者 Pass 适配器；游戏模块的 RenderFeature 可以显式启用作者 Pass 展开。

Renderer 的运行时拓扑和 Editor TypeCatalog 使用相同的稳定 Pipeline、FeaturePoint、Feature、Pass identity。作者 Pass 的参数、启停和稳定资源绑定属于 Feature 实例配置，当前帧的 Graph 资源仍由 RenderGraph 解析。
