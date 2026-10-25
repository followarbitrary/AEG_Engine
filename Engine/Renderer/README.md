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
`RenderSceneSnapshot` 和 `Renderer`。`Renderer` 仅消费不可变的
`WorldSnapshot`，通过注册的提取回调构造不可变场景快照，并可将快照发布为 Frame
Scope 的 `RenderSceneSnapshot` Artifact。`EngineFrameOrchestrator` 在
`RenderExtract` Phase 自动执行该发布流程；没有配置 Renderer 时不会改变原有帧流程。

`RenderPipeline` 提供 Visibility、Shadow、Depth、Opaque、Lighting、Transparent、
PostProcess 和 BeforePresent 的私有技术 Stage 拓扑。具体绘制和计算来自
FeatureRuntime 的 committed `RenderFeatureOutput`，由 projector 和
`RenderFeatureDrawLowerer` 展开为实际 RenderGraph 工作。旧 `OpaqueRenderFeature`、
`RenderFeature`/`PipelineFeature` executable 类和 instance Runtime 入口已删除；旧结构
声明、FeaturePoint、Feature 注册/查询/绑定、贡献展开及 Pipeline instance 配置链也已物理删除。
固定 Frame `PipelineDefinition` 的 Stage、Artifact 与依赖继续保留。

Scene 原生录制的 `build_*_render_graph` 辅助函数位于
`Engine/Renderer/private/render_feature_graph_builders.hpp`。它们复用资源解析和录制器，
为操作记录内部 Pass；真实 Feature WorkItem 由 projector 产生。公共 Renderer 头不把这些
Builder 接口提供给游戏作者。公共层保留 RenderView、资源生命周期和资源值/resolver
合同。

`RenderFeatureContext` 的完整定义以及 `render_feature_context(builder)` lookup helper
属于 Renderer 的内部装配边界，位于 `Engine/Renderer/private/render_feature_context.hpp`。
它不属于公共 Renderer 头；原生 lowerer 与固定技术装配通过目标的 private include
requirement 取得实现头。公共 Renderer value/resolver 合同不暴露此执行上下文或
RenderPassRegistry 宿主。

## Deferred 与 Forward 管线

Renderer 私有原生执行设施保留两组技术拓扑；它们不是作者目录或项目选择入口。
业务作者使用 `.aegfeature`，运行时从 RenderIntent 投影实际工作：

- `Renderer.DeferredRenderPipeline`：`RenderExtract`、`Shadow`、`GBuffer`、
  `DeferredLighting`、`SkyboxPostProcess`、`BeforePresent`；
- `Renderer.ForwardRenderPipeline`：`RenderExtract`、`Shadow`、`ZPrepass`、
  `LightCulling`、`ForwardOpaque`、`SkyboxPostProcess`、`BeforePresent`。

原生 Pipeline/Pass 的 ID 和装配函数位于 Renderer-private
`renderer_variants_internal.hpp`，供技术执行设施与定向测试使用；旧公开
`renderer_variants.hpp`、`register_renderer_authoring_types()` 及 11 项作者目录已删除。
作者使用 FeatureGraph。`renderer_feature_profiles` 从同一工厂生成真实可执行
Scene Feature、三个定义的目录和内部 preset 图；Frame Runtime capture 的 owned
RenderIntent 交给 Renderer lowerer，每个业务 WorkItem 保留实际 Feature 来源。

当前 Pass 先形成可执行的 RenderGraph 拓扑：Deferred 创建 ShadowMap 和三张 GBuffer，
其中 GBuffer 在提供 `RenderMaterialResource::gbuffer_pipeline` 时会解析网格、材质、
对象资源并发出 indexed draw；Deferred Lighting 采样 Albedo、编码 Normal 和 Depth，
执行共享方向光与环境光契约的全屏绘制。Forward 的 `ForwardOpaque` 复用同一套资源解析和绘制路径，
并声明对 Z-prepass 深度、ShadowMap 与 LightClusters 的依赖。外部 SceneColor/SceneDepth
由 FrameContext 绑定，临时资源由 RenderGraph 管理。

Z-prepass 通过 `RenderMaterialResource::depth_pipeline` 执行几何体深度绘制，ForwardOpaque
加载其深度结果。`DeferredLightingResources`、`LightCullingResources`、
`PostProcessResources` 和 `ShadowResources` 是 Renderer 私有的逐帧资源装配 helper，声明
位于 `Engine/Renderer/private`；宿主通过公开的 `RenderResourceResolver` 回调返回
`Render*Resource` 值合同。宿主通过 `DeferredLightingResources` 创建全屏光照 pipeline，并在
`resolve_deferred_lighting()` 中返回当前帧的绑定服务；GBuffer 的临时采样视图由执行上下文
解析，descriptor 每次执行单独分配并交给 FrameContext 延迟销毁。有几何体却缺失这些
资源时构图返回错误；空场景允许只清屏。光照参数复用 `RenderLightingResource` 的 uniform
范围，和 GBuffer 视图一起绑定到全屏 shader 的 group 0；Opaque 的 group 2 契约不变。
每个 profile helper 只接受与其创建 Device 相同的 FrameContext，避免把 owner Device
创建的 buffer、descriptor 或 PSO 交给 foreign host 退休。
DeferredLighting 资源值还必须携带匹配的 `RendererProfilePsoContract`；若启用 shadow PSO，
其独立 contract 也必须有效，Runtime resolver 不允许 frame-aware callback 绕过该校验。
PostProcess 资源值同样必须发布有效的 constants/Uniform 描述、prepare callback 和
`RendererProfile::kPostProcess` contract；Runtime callback 返回值会在进入 Graph 前 fail-closed。

这些 profile 的稳定逐帧输入/资源值合同集中在公开的
`Engine/Renderer/public/renderer_profile_resource_contract.hpp`，`renderer.hpp` 只聚合该窄边界
以维持现有 Resolver ABI。Shader reflection、descriptor/layout 选择、PSO factory 和资源装配
实现仍属于 `Engine/Renderer/private`。

Profile 的 Shader/PSO identity 进一步集中在公开的
`Engine/Renderer/public/renderer_profile_contract.hpp`。`RendererProfileShaderContract`
以 profile、contract version 和 layout hash 标识当前 shader/layout，
`RendererProfilePsoContract` 再绑定附件合同；DeferredLighting、LightCulling 和
PostProcess 的逐帧资源所有者在创建真实 layout/PSO 后发布这些值，RenderGraph 在消费前
拒绝 profile、version、layout 或附件不匹配。reflection、descriptor set、PSO factory
以及 cache 仍由 Renderer/private 持有，公共头只承载后端无关的值合同。

clustered Forward material variant 通过同一 PSO 合同边界发布：私有 clustered factory
为 shadowed 与 unshadowed shader layout 分别计算 layout hash，并把各自的
`RendererProfilePsoContract` 放入 material variant；Runtime 与 Editor upload host/resolver
只转发这些值，Opaque selector 在选择 clustered pipeline 前校验 profile、version、layout
和 attachment，避免把两种 shader layout 当作同一个 PSO identity。

共享 Shadow/Z-prepass 的 depth-only PSO 使用 `RendererProfile::kShadow`。
`RenderMaterialResource::depth_profile_contract` 保留真实反射 layout hash、Shader/cache
epoch 与无颜色附件的 depth 合同；Runtime、Editor 在创建实际 PSO 时发布这些值。
中立附件值支持合法 depth-only 形状，Shadow profile 拒绝 primary/additional color，
其他 graphics profile 继续要求 primary color。Feature depth 降低层校验 profile/version、
resource/PSO epoch、新旧附件元数据及实际目标合同；空 caster 只清屏，不要求无用 PSO。
仍活跃的内部 Shadow/Z-prepass 入口同时核对固定 D32/X1 目标；不匹配合同在调用
mesh/object resolver 前拒绝，不因无效绘制创建对象上传或描述符。
Runtime 的私有 upload host 保活 depth layout，暖缓存返回前核对其实际 hash/epoch，
过期或矛盾的合同不能通过覆盖 epoch 修复；撤销 depth PSO 时清空返回的 depth 元数据。

GBuffer 使用 `RenderMaterialResource::gbuffer_profile_contract` 发布实际 Shader layout
hash/cache epoch 与完整 ordered MRT/depth 合同。中立 `kGBuffer` profile 要求三个完整
颜色槽和合法 depth；正式 Feature 目标与内部 Deferred 目标仍核对真实固定顺序
RGBA16Float/RGBA16Float/RGBA8Unorm + D32Float/X1。无效版本、profile、epoch 或有序
附件在 mesh/object resolver 之前拒绝，已有图工作按原事务回滚。Runtime/Editor 从同一
PSO 创建描述发布 metadata，Runtime 保活并核对实际 reflected layout；撤销 GBuffer
句柄时两份 metadata 清空，换代使用真实新 layout/PSO，暖材质保留已有 descriptor/参数资源。

Shadow 已执行方向光视图的深度绘制，前向与延迟 shader 均可采样 ShadowMap；
`RenderView::shadow_extent` 的独立尺寸会贯穿内部 ShadowMap、LightCulling 与
Feature attachment 校验，外部 Runtime/Editor 多 View ShadowDepth preparation
仍由宿主按 View 组织。
Forward 的 LightCulling 在计算队列按 16×16 像素分块，使用 Z-prepass 深度和视空间
光源范围生成点光源/聚光灯列表；ForwardOpaque 通过 `clustered_pipeline` 消费列表，
叠加距离衰减与光锥衰减，并保留方向光阴影。宿主通过 `LightCullingResources` 准备当前帧
资源，借用同一 ShaderManager 的 group 3 布局；帧结束后旧绑定不可重用。公开的 shadow
view/value 合同仍由 `renderer.hpp` 提供，供 `resolve_shadow()` 和 `resolve_light_culling()` 使用。
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
中立 RenderPostProcessScene 直接展开内部全屏工作，复用本文件所述的 shader/PSO、
PostProcessResources 和唯一录制器。设置与校验已移到 Feature 值合同，无第二份类型或
校验函数。Renderer 只复制临时 View 覆盖该操作的设置，不改变场景快照；真实 PSO
附件合同从创建描述发布。每次调用的常量缓冲沿用帧退休，通过现有资源身份/import
函数和 Graph 原生去重绑定，避免固定 ID 让同帧不同参数冲突。

正式 Scene profile 的 lowerer 通过私有 `RenderPassExecutionHost` 执行原生
Pass 的 `setup/execute`。Runtime/Editor 在 post-build 提交实际 registry 及当前 epoch，
lowerer 校验 captured operation 的 WorkItem、owner、完整 invocation、frame 和 View，
再将原生命令交给固定 PassRegistry dispatch。图保留解析出的 Pass、帧配置和执行租约；
注册表后续变动不改写已准备命令。显式 null/stale host、缺失 Pass 或无效来源会失败。
固定技术夹具可用 `NativeGraphBuildSession` 直接调用同一 helper，保留独立的 null-host
direct Compute 入口；这些技术工作不伪造业务 Feature 身份。

Editor 默认 Scene Preview 通过独立 Engine FeatureRuntime 执行真实
`Renderer.Scene.Opaque/Forward/Deferred` Atomic Render Feature；它与 Game registry、
Clock 和 history 隔离。Scene 每帧 tick/capture，准备 View Color/Depth/HDR/GBuffer/Shadow，
然后依次投影 Preview 和 Game 输出。空图不恢复隐藏原生工作；恢复默认预览先准备
候选 FeatureHost/history，再在 Edit 帧边界交换。Frame/Render 面板只读显示真实 EP/RG，
旧 `.aegframe` 选择、启动字段和 `.aegscene.pipeline` 作者 sidecar 均不再消费。

## 边界

Renderer 可以消费 World 的渲染快照、Shader Contract、Resource 和 RenderGraph，
但不应直接访问 RHI 后端私有对象，也不应把通用 Pipeline 调度逻辑复制到本目录。
`RenderSceneSnapshot` 只保存后端无关的资产 ID、变换、视图和光照数据；GPU 资源句柄
解析与同步仍由 Resource、RenderGraph 和 RHI 负责。

## 核心数据流

`WorldSnapshot` 是唯一的场景输入。`Renderer` 根据 `RenderView` 和已注册的
`RenderExtractFunction` 生成一个不可变的 `RenderSceneSnapshot`，随后由
`EngineFrameOrchestrator` 将其发布到对应的 Frame Scope Artifact。宿主在 RenderBuild
的 post-build hook 中，把当前 View 的 owning 快照、目标绑定与 FeatureRuntime 的
committed capture 一起交给 `build_render_feature_outputs()`；projector 生成真实 work，
lowerer 用私有 `RenderFeatureContext` 复用原生资源解析与录制器。Orchestrator 不再
遍历 Frame plan 调用旧 instance Runtime 或 executable Feature callback。

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
        +--> owning Scene/View bindings + explicit attachments
                    |                           FeatureRuntime tick/capture
                    |                                      |
                    +------------------+-------------------+
                                       v
                         build_render_feature_outputs()
                                       |
                                       v
                       Projector + RenderFeatureDrawLowerer
                                       |
                                       v
                       RenderGraph work + native Pass dispatch
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

`RenderView::shadow_extent` 是可选的独立方向光 ShadowMap 尺寸；零值回退到
`output_extent`，非零值会同时驱动光空间 View、shadow texel 常量和内置 Forward/Deferred
ShadowMap 纹理尺寸。Shadow depth/采样附件必须匹配该独立尺寸。Runtime/Editor 的多 View
外部 ShadowDepth identity 可通过 `make_view_scoped_render_resource_id()` 派生，首个 View
仍可使用兼容的 `Renderer.ShadowDepth` identity。

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
  Object Binding 会在具体 Scene 操作降低时按 View 解析。

解析器指针只在提取和当前帧 Scene 构图期间借用，Orchestrator 不取得其所有权。
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

### 私有 `RenderFeatureContext`

`RenderFeatureDrawLowerer` 为当前操作构造此上下文，并显式传给固定原生 helper。
`scene_snapshot` 指向当前 Frame/View 的不可变快照，`resource_resolver` 仅在构图时
借用；`pass_execution` 携带 native registry/epoch，`operation_source` 指向实际
captured Scene 操作来源。固定技术 Session 通过仅含 `flags` 与 `context_data` 的
`pipeline::RenderGraphBuildContext` 借用该上下文，`render_feature_context(builder)`
从 `builder.context_data()` 读取；旧 Builder 的 scope、Pipeline/Feature/Part/Point 身份、
作者配置和 userdata getter 已删除。Frame/View 与操作来源继续由实际 Scene 上下文校验。

native build 使用独立的六位 `pipeline::RenderGraphBuildFlags`，分别允许资源创建/导入、
Graphics/Compute/Copy Pass、专用队列和最终输出写入；资源位为 `kMayCreateResources`。
这些权限控制现有资源与 Pass backend，不再由结构 FeaturePoint 提供，也不增加作者身份。

上下文本身只在构图调用期间有效。选中的 owning 场景快照、已准备原生命令及 GPU
资源租约由图和 FrameContext 保活到退休；旧 Orchestrator-owned legacy context
容器已删除，作者 callback 不取得这些指针。

### Opaque Scene 投影

旧 `OpaqueRenderFeature` 类、register/bind 接口及私有注册头已删除。作者返回真正
Render Feature 的 Scene 意图；宿主把实际 committed capture 和当前 View 的
`RenderSceneSnapshot` 交给私有 lowerer。固定原生绘制器逐个解析 Mesh、Material 和
View-local Object Binding，并通过原生 PassRegistry 记录带真实 Feature work 的绘制。
Pass 会显式读取 Vertex/Index/Uniform Buffer，并写入该操作选定的 Color Attachment；
可选 Depth/Stencil Attachment 使用显式 `OpaqueDrawOptions` 的 Load/Clear 语义，
固定技术默认路径使用 View 的目标和原清屏策略。因此
Resource Resolver 提供的 Buffer 描述必须包含相应的 RHI capability，调用方提供的 Depth
Texture 必须声明 `kDepthStencil` capability。

调用方必须先导入或创建目标 Texture，并将当前图的句柄交给
`build_render_feature_outputs()` 的目标绑定。使用 `EngineFrameOrchestrator` 时，应在
`EngineFramePreparationContext` 中导入目标并以同一稳定 `RenderResourceId` 绑定。当前
最小 Shader Contract 位于 `shaders/core/opaque/unlit_mesh.vert.hlsl` 与
`shaders/core/opaque/unlit_mesh.frag.hlsl`，固定使用 group 0 Object、group 1 Material、
group 2 Lighting。顶点 Position 为 `float4`、UV 为 `float2`、Normal 为 `float3`；顶点
Shader 输出插值 UV 和 View-space Normal。材质参数块继续提供稳定的 `float4` Tint，片元
Shader 按插值 UV 采样纹理，并计算
`albedo * tint * (ambient + color * intensity * max(dot(normal, -direction), 0))`。
基础 Opaque 绘制只支持一个 Directional Light，不包含镜面反射、PBR 或多光源循环；没有
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

Renderer 内部的 `register_render_pipeline()` helper 注册标准 Frame 级 `RenderPipeline`。该 helper 只属于
Renderer 的装配实现，不是公共作者 API。该管线只定义
Visibility、Shadow、Depth、Opaque、Lighting、Transparent、PostProcess 和
BeforePresent 的 Stage 拓扑；实际业务 Pass、资源和 Shader 由 committed
Feature 意图经 projector/lowerer 生成。

标准拓扑由 `make_render_pipeline_definition()` 收集 owning `PipelineDefinition` 值；
不存在 `RenderPipeline` 作者类或虚拟 `build()`，也没有结构 FeaturePoint 或 register/bind
扩展入口。Registry 仍完整验证 Definition 的 Stage、Artifact、访问、ordering 与 exposure；
编译保留 Phase、scope、版本和拓扑检查。计划的 `definition_owner` 保活实际不可变
Definition，Frame 的 Artifact 生命周期、active/pending/retired 切换、plan pin 和执行 lease
仍保留。RenderBuild 在 preparation 后运行宿主 post-build hook，不通过技术 Stage 分发业务 callback。

RenderPipeline 的实现可以依赖 `RenderSceneSnapshot`，但不能反向修改 World，也不能将
后端对象写入 Renderer 公共接口。资源状态转换、队列同步和物理执行仍由 RenderGraph
与 RHI 负责。

## Renderer 内部技术执行路径

原生 Deferred/Forward 执行适配器保留 RenderExtract、RenderBuild、BeforePresent 等技术阶段。
五域 Render Feature 输出 typed RenderIntent；Renderer 将 Shadow、GBuffer、Lighting、
PostProcess 操作展开为内部 RenderGraph Pass，保留实际 Feature 来源。

Editor FeatureCatalog 只包含 Feature Definition、Module、Clock、Domain 和版本合同。
固定 Pipeline 与 native Pass ID 留在私有技术执行设施；旧 FeaturePoint 身份已删除，
当前帧资源由 RenderGraph 解析。现代 Feature 的 Domain 合同继续由 Engine/Feature 持有，
技术 PipelineDescriptor 不再携带旧绑定使用的 domains。

## 中立 Feature 场景绘制

五域作者通过 `StateDelta.render_intent` 提交 `RenderDrawScene`：场景槽 ID/版本、外部
颜色目标、Load/Clear 和清屏颜色。游戏 SDK 不暴露 Renderer、Builder、Pass 或 RHI。
引擎的 `RenderFeatureDrawLowerer` 接受当前帧 owning 场景绑定，校验版本、实例所有权、
唯一性、附件和 View 尺寸，再复用 Opaque 的网格/材质/对象/灯光及 indexed draw 路径。
正式 Runtime/Editor 使用 registered lowerer，把实际 Feature 来源交给固定原生
PassRegistry dispatch；内部 direct lowerer 也复用相同录制器。每个业务 Pass 保留
真实 Feature work 和操作来源。

当前支持单 mip/layer、X1、2D RGBA8/BGRA8 颜色目标及固定材质 Shader ABI；宿主创建
PSO 时通过公开 `renderer_material_contract.hpp` 发布 `RenderMaterialAttachmentContract`，
`renderer.hpp` 仅聚合该稳定 host/value 边界以维持解析器 ABI。固定材质合同同时发布
`RenderMaterialShaderContract` 的独立 version/layout hash；Runtime host 只发布该值并在私有
`ShaderPipelineLayout` 上校验 hash，不把 `ShaderProgram`、reflection 或 `ShaderManager` 带入
Renderer public。固定 shader-facing 的顶点布局、Object/Lighting/Shadow constants 位于
`renderer_shader_contract.hpp`，`renderer.hpp` 只聚合该头。该头不承载 shader reflection、
layout object 或 pipeline factory；这些 profile 资源所有者仍属于 Renderer/private。中立路径要求
颜色格式/采样/深度合同唯一匹配。可选外部 D32Float 深度附件共用 Load/Clear，须匹配 View 尺寸；主 PSO 与可选
`depth_attachment_variant` 复用同一 Shader/layout、描述符和绘制路径，空场景也执行附件语义。
场景快照由 Graph 保留到 FrameContext 退休，资源解析器只在构图时调用；GPU
资源由既有宿主缓存/退休屏障管理。Opaque/Forward/Deferred Scene profile 已使用此链；
任意 Shader/PSO 作者选择与更广渲染能力仍不在已完成范围内。

Runtime/Editor post-build 按 `RenderSceneSnapshot` 的 View 分组提交 Feature output。首个
View 保留 canonical resource identity，后续 View 由 Renderer-private
`scope_render_feature_output()` 派生 Scene、颜色/深度、ShadowDepth 及操作引用的稳定
scope，并以 View key 隔离 temporal history；宿主为 scoped feature textures 建立对应别名。
原生 Pass 执行通过 RenderGraph-private `RenderPassExecutionHost` 完成，不进入
Renderer public header；旧 authored-pass 通用展开与 instance Runtime 桥已删除。

Renderer profile 的 Shader contract 与 PSO contract 同时携带 ShaderManager
`cache_epoch`；profile owner 从实际 layout 发布该值，PSO 校验 shader/PSO epoch
一致且 material variant 自身 shader contract 有效后才可被 Graph lowerer 使用。Runtime material shader contract 也验证 borrowed
layout 的 epoch，cache 清理或 host 替换不会继续命中旧的 layout/descriptor/variant。

RenderDrawDepthScene 复用既有 Z-prepass 的 depth-only packet、网格/对象导入及 indexed
draw，直接展开内部 Pass；depth_pipeline_contract 验证宿主的无颜色/X1/D32 PSO。
空场景也处理 Load/Clear，颜色与独立深度入口共用场景/附件验证和 GPU 退休保留。
引擎仍通过现有材质解析器取得 depth_pipeline；Runtime 的材质缓存依赖保持不变。

RenderDepthSceneProjection::kDirectionalShadow 复用 make_directional_shadow_view 的方向光
拟合与 shadow_casters（缺失时使用 instances），把光空间视图传给既有对象资源解析器。
中立深度路径显式传递 instance span；空 span 只执行附件 Clear/Load，避免阴影关闭时
误绘相机几何。旧内部调用的默认 span 回退仍由原入口控制。原生夹具回读验证倾斜光空间、
相机视野外 caster、关闭/空阴影及同帧相机与阴影对象绑定隔离。

RenderDrawScene.directional_shadow 复用同一拟合、ShadowResources 帧常量和 Opaque
group 3 描述符/采样录制；输入要求 D32、Sampled、2D/X1、单 mip/layer、View 尺寸，
且不能与目的附件别名。shadow_pipeline_contract 校验实际阴影 PSO 的颜色/采样/深度
合同；无有效投影光时使用普通场景光照。共享资源准备函数按原生常量缓冲产生内部
导入 ID，支持同一 Feature 意图内多次绘制，不新增 Shader、PSO、缓存或另一份阴影图。

独立 RenderLightCullingScene→RenderDrawClusteredScene 复用上述阴影深度、既有
LightCullingResources::for_frame 和 Opaque recorder。typed bundle 保留真实 source
receipt、canonical snapshot、camera/shadow 最新深度生产和强 build scope；每次 Clear
更新 build 内 contents_revision，同源同投影 Load 保留版本。消费比较保留与当前版本，
拒绝剔除后替换内容及回滚旧 bundle，不额外分配 identity/cache 或创建另一套 validator。

原生独立 Feature 夹具覆盖有效方向阴影的两路 HDR、中心/视野外 caster 数值、选中
PSO、两个 fresh descriptor set、存活来源/实际 Compute 与 Fragment 访问、失败回滚
及正常 FrameContext 退休。游戏作者仍只返回中立 Feature 意图；内部 Concurrency、
RHI、Builder、native bundle 不进入 GameSDK。完整渲染能力与其他迁移门槛继续开放。

独立 native Feature 回归还使用同一 P/C 定义发布 PA→CA、PB→CB，公共 List/scene/
depth 名称相同，通过既有 owner-qualified 两快照/两 D32 绑定和真实 typed receipt
保持来源隔离。两个 Compute/Forward 各自使用原 native for_frame，Point/Spot 的两幅
HDR、存活 work provenance、origin/其他 producer bundle/强 ABA 回滚及两份退休租约
均有实物验证。实际重叠时资源分开，非重叠时允许已有 transient pool 复用，不能
用公共 ID 或禁止物理复用代替完整生产身份。此项没有增加生产 API 或重复执行路径。


The same native Feature fixture also verifies effective central directional
shadow with complete A/B publication and Process suspend/resume/cancel. Every
participating physical frame has a real enabled shadow, its dark/lit HDR oracle,
camera/Compute/shadow/Forward provenance and one shared culling bundle with two
fresh Forward descriptors. Two physical frames rebuild retained CPU A while the
real slow consumer holds B; the next complete cohort changes tokens and HDR
background together. Process retirement produces a successful fresh empty native
graph, and resume requires current producer/consumer receipts. The existing
observing lowerer forwards native preparation and proves typed bundle, scene and
output leases live before normal drain and expire afterwards, even while only a
preceding strong build identity is retained. These modes reuse all allocation,
recording and validation paths.

Shadow 与 LightCulling callback 同样必须发布完整的 buffer/descriptor、准备回调及对应
Shader/PSO contract；Runtime resolver 在进入 RenderGraph 前统一 fail-closed，避免不完整
资源绕过 Renderer profile 边界。

Renderer profile PSO metadata 还必须通过 `RenderMaterialAttachmentContract::is_valid()`，在
颜色格式和 SampleCount 进入 profile/variant 选择前拒绝非法 attachment 值。

Frame artifact contract 安装与 RenderSceneSnapshot 发布通过 Renderer-private bridge 完成；
`renderer.hpp` 保持稳定场景/提取 ABI，不暴露 `PipelineScopeHost` 编排句柄。

Light-culling resource provenance follows the same profile boundary. The public
`RenderLightCullingResource` carries only an opaque origin token; frame/device/scene and native
payload validation lives in `private/light_culling_origin.hpp` and is applied by the Renderer
draw adapter before Compute or Forward consumption.

Renderer public headers no longer carry the unused RenderGraph `FrameContext` forward declaration.
`RenderFeatureContext` is declared only in the private Renderer contract; Runtime ownership lives in its implementation.

Material resource values follow a dedicated public contract boundary. Include
`renderer_material_resource_contract.hpp` when a consumer needs the host-owned material GPU
value; `renderer.hpp` keeps the compatibility aggregate include. The contract exposes only
native handles, graph value handles, attachment/profile metadata and cache epoch; shader
reflection, PSO creation and cache ownership remain Renderer-private.

Native RenderPass execution uses the Pipeline-private
`render_pass_execution.hpp` bridge for registration lookup and execution. Renderer public
headers expose neither the helper nor the internal RenderPass registry mutation path.

Engine-private Scene profile definitions now produce real Atomic Render Feature intents for
Opaque, Forward and Deferred. Registry, catalog and preset graphs share those definitions;
callbacks carry no native pipeline or Shader factory. Forward keeps its LightList private and
uses the actual invocation identity. The Editor preview executes these graphs through its
own Feature host and the existing draw lowerer, with actual View HDR/MRT/Shadow attachments.
The old eleven-entry author catalog, Frame file selection and Renderer sidecar chain have been removed.
Global Pipeline authoring serialization has been removed; formal World projection and final five-domain acceptance remain open.

Renderer public values use Foundation stable IDs, Feature resource IDs, and the canonical
RenderGraph logical handles from render_graph_types.hpp. The duplicate Pipeline-owned handle
contract and public snapshot Artifact IDs are gone; Frame publication constants remain private.
Native RHI graphics pipeline values and the existing attachment/Shader/PSO contracts are retained.

Standard and Variant frame topologies now collect owning PipelineDefinition values with fixed
private functions. Registry registration and compilation invoke no Pipeline factory, build or
contribution collector. Opaque/Forward/Deferred Stage records remain fixed technical values.
Their old executable Opaque/VariantFeature classes and the generic structural declaration,
extension-point, register/find/bind and instance-configuration chain have been removed.
Scene rendering uses committed Feature captures and actual RenderWorkItem sources. Technical
Definition/Stage/Artifact validation, definition-owner leases and plan lifecycle remain intact.
Engine-private NativePassSequence retains native type/queue/configuration/ordering validation
without a Feature, point or part identity; it is not a FeatureGraph authoring collection.
Native PassRegistry execution, epoch checks, Shader/PSO contracts and scene pixel/depth/material
regression coverage remain.

Modern Scene operations now use a private fixed native PassRegistry dispatch with actual
Feature work/token, resource owner and View provenance. Runtime and Editor pass their live
registry epoch. Prepared configuration identity is immutable and frame-retained; explicit
null/stale hosts fail. The shadow GPU fixture's Forward/Deferred and unshadowed two-consumer
paths now use those actual captures. ImGui now uses a fixed private overlay assembly, without
a business Feature identity. The generic PipelineFeature/RenderFeature executable hierarchy,
implementation lookup, instance RenderGraphFeatureRuntime and Pipeline-plan execution bridge
have been removed. EngineFrameOrchestrator retains technical preparation/post-build, Stage,
Artifact and lease handling; actual Feature captures still use build_render_graph_features()
with projector, history and the native lowerer.

The old state-input/output fields, composer/flow and renderer state-seed branches had no
remaining producer and have been removed. Explicit color/depth/HDR/MRT attachments,
DrawOptions Load/Store/final usage and Feature Runtime StateStore semantics remain.
