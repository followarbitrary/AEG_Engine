# Runtime

## 目录职责

`Runtime` 负责把引擎生命周期、Pipeline Runtime、World Snapshot 与帧内
RenderGraph 连接成统一的执行入口。它位于 Engine Scope 与 Frame Scope 之间，
不持有应用层、编辑器或具体 RHI 后端的业务状态。

## 公共与私有边界

跨模块使用的数据类型、稳定身份和函数声明位于 `public`。组件的 Reflection
构建、ECS 注册校验、渲染提取以及 `RuntimeSession` 的具体状态迁移均位于
`private` 的 `.cpp` 文件。除稳定数据契约和必要的模板接口外，`.hpp` 不放置
具体实现。

## 生命周期与帧编排

`EngineFrameOrchestrator` 负责 Phase Registry 同步、Pipeline Plan 固定、Frame
Begin/End、RenderBuild、RenderExecute、Present、Abort 与 Shutdown。它还负责已注册
World 的命令消费、`WorldCommit` Snapshot 发布，以及 World-to-Frame Bridge 的安装和
清理。

`RuntimeSession` 在 Orchestrator 外提供停止、播放、暂停和关闭状态机。它不拥有
Orchestrator；调用方必须保证传入对象的生命周期覆盖 Session，并在停止时重新从零开始
固定 tick 计数。

## 运行时组件

`public/Components` 提供以下稳定组件数据契约：

- `Transform` 复用 `math::Transform`，稳定身份为 `Game.Transform`。
- `Camera` 保存透视投影参数，稳定身份为 `Game.Camera`。
- `MeshRenderer` 保存网格、材质、局部包围球和可见性数据，稳定身份为
  `Game.MeshRenderer`。
- `TextRenderer` 保存世界空间文字和 Font Asset ID，稳定身份为 `Game.TextRenderer`。
- `DirectionalLight` 保存方向、颜色和强度，稳定身份为 `Game.DirectionalLight`。

`register_render_component_reflection()` 显式向调用方提供的 `TypeRegistry` 注册五种
schema（包括 `TextRenderer` 和 `DirectionalLight`）；`register_reflected_render_components()`
为一个 `World` 注册对应的持久化 ECS storage；`register_render_component_bundle()` 顺序执行两者。注册操作会验证已有身份的
元数据，兼容的重复注册保持幂等，不兼容的注册返回失败。仅需要临时、不参与 Scene
持久化的 World 可以使用 `register_render_components()` 注册非反射 storage。

`Transform` 的 `Vec3` 字段使用 `Math.Vec3` 的固定 16 字节四浮点 wire layout：前三个
浮点为 XYZ 语义，第四个槽位在写入和读取后规范化为零，绝不按紧凑三个浮点编码。`Quat`
使用 `Math.Quat` 的四个 XYZW 浮点编码。

`TextRenderer` 保存稳定 Font Asset ID、UTF-8 文本、世界空间字号和 RGBA 颜色；Runtime
提取器将其转换为世界空间 RenderInstance。编辑器为其构建 vendored 字体图集并通过标准
ResourceUploader 上传，非 ASCII 字符在当前 ASCII 图集 fast-path 中替换为 `?`。

## Scene 与 Renderer 适配

Scene 通过 Runtime 组件的稳定 ECS/Reflection 身份保存组件 payload，并可在已注册对应
storage 的 World 中恢复。`make_render_camera()` 使用 Math 的可失败逆矩阵和透视投影接口
创建后端无关的 `RenderCamera`；`make_render_view()` 只从不可变 `WorldSnapshot` 中查找显式
指定的相机实体，并验证 Snapshot、实体、`Transform`、`Camera` 和输出尺寸后构造
`RenderView`。`extract_mesh_renderers()` 只读取已提交的 `WorldSnapshot`，将可见且资源身份
有效的 `MeshRenderer` 转换为 `RenderInstance`。提取器的注册身份由调用方持有并负责注销。

当 `EngineFrameDesc::render_views` 为空时，调用方可以提供
`EngineFrameDesc::render_view_resolver`。编排器会在 RenderExtract 阶段针对每个已注册的
World/View 使用其已提交 Snapshot 调用一次解析器，并验证返回值仍属于该 World/View；解析器
不得保存 Snapshot 引用。显式 `render_views` 与解析器互斥，避免同一帧混用两套 View 来源。
Runtime 组件只通过该公共 Snapshot 回调适配相机，Engine 编排器不依赖 Runtime 组件头文件。

无 Swapchain 帧生成 View 时，`EngineFrameDesc::render_output_extent` 会作为
`EngineFrameRenderViewContext::output_extent` 传入解析器；默认值为 `1x1`，离屏渲染调用方
应显式设置实际目标尺寸。这样 Runtime 的 View 解析逻辑无需了解外部 Texture 或具体 RHI
后端。

`EngineFrameDesc::render_feature_preparation` 在 RenderBuild 阶段准备帧内资源，也可加入固定
native 技术工作，适合导入外部目标、分配 Readback 或准备 Upload。回调可先通过
`EngineFramePreparationContext::render_graph` 导入 Texture，再调用
`bind_feature_texture(RenderResourceId, RenderGraphTexture)` 把稳定逻辑资源身份提供给后续
现代 Feature 投影。该绑定只在当前活跃帧的 preparation 期间有效；重复身份必须指向同一 Texture，
Context、FrameAllocation 和 RenderGraph Handle 都不得跨帧保存。

Opaque native 绘制的网格资源还必须声明 `RenderMeshVertexLayout`：interleaved Vertex
Buffer 至少包含 Position `float4`、UV `float2` 和 Normal `float3`，并且 stride、offset、
format 必须与当前 Shader Contract 一致。Object Uniform 使用 `RenderObjectConstants`，
包含 Clip-space MVP 和用于法线变换的 inverse-transpose Matrix，Buffer 范围必须覆盖完整
常量块。缺失 UV/Normal、顶点格式不匹配或越过 Vertex Buffer 范围时，RenderBuild 应返回
失败；该失败不得继续提交不完整的绘制。

Opaque 的帧内光照资源属于独立的 group 2，不得与 Object group 0 或 Material group 1
混用。`RenderResourceResolver::resolve_lighting()` 返回的 `RenderLightingResource` 必须
提供有效的 Uniform Buffer、BindingSet、group 2 和当前 RenderGraph Buffer Handle；Opaque
Pass 会显式声明其 Fragment `kUniformRead`。`RenderLightingConstants` 目前只表达一个
Directional Light 的方向、颜色、强度和环境光，使用 Lambert 项
`max(dot(normal, -light_direction), 0)`。没有 Directional Light 时必须回退到环境光；
多个 Directional Light、越界颜色/环境光、零方向或非法 Buffer 范围都属于失败输入。

光照常量和 BindingSet 按帧生成。更新材质或光照时，调用方应通过 `FrameContext` 的上传
与延迟销毁接口保留旧资源直到提交完成，不得依赖每帧 `wait_idle()`，也不得修改 GPU 仍
在使用的持久 Descriptor。native 绘制只借用当前帧资源，实际 owner 按帧提交与退休。

## 最小编辑器闭环

Editor 以 `SceneAsset` 作为稳定文档身份，以 `SceneInstance` 维护场景实体到 ECS
`EntityId` 的映射。Hierarchy 选择属于 Editor-owned 状态，不写入 Runtime 组件；Inspector
编辑通过 `WorldCommandBuffer` 排队，并在下一次 `WorldCommit` 后从新 Snapshot 读取，因此
不会直接修改已发布的 Snapshot。

`Edit` 模式绑定编辑 World。进入 `Play` 前先提交编辑命令，再从当前 Scene Asset 创建独立的
运行 World；`Pause` 只冻结编辑器状态机，`Stop` 丢弃运行 World 并重新绑定编辑 World。运行
期间的实体或组件修改不得回写编辑文档。保存和重新打开只序列化稳定的 Scene Entity、层级和
反射组件 payload；缺失 Mesh/Material Asset ID 由 Renderer Resolver 返回可诊断的 fallback，
不能使编辑器进程崩溃。

Scene Viewport 是离屏 RenderGraph 输出的借用者。编辑器每帧应先导入当前尺寸的 Color/Depth
目标，再运行 Runtime View 提取和现代 Scene Feature 的 native 投影，最后将颜色目标转换到采样态供 ImGui 使用。
任何 RenderGraph Handle、FrameContext 分配或持久 BindingSet 都不得跨越其声明的生命周期。

## 依赖边界

Runtime 的公共边界使用 Foundation、Math、ECS、Reflection、Serialization、Pipeline、World、
RenderGraph 和 Renderer 的稳定契约，不传播 private 实现或 Vulkan、Win32 等 native API。
实现文件通过显式 private adapter 使用引擎内部执行服务。Scene 依赖 Runtime 组件公开的数据契约和 Reflection
schema，不反向要求 Runtime 依赖 Scene。
# Pipeline composition runtime

`PipelineCompositionRuntime` 负责把多个 `PipelineScopeHost` 绑定成一个可切换组合。
其公共头只声明组合所需的轻量执行身份和 Host/PreparedDocument 引用；配置快照、待应用
队列和计数器均位于 `Engine/Runtime/private` 的 PIMPL 中，避免把 Pipeline authoring
实现容器通过 Runtime 公共头传播给上层。Editor 使用该桥接时必须显式 include 自己需要
的 Pipeline authoring 头。
组合先通过 `stage()` 校验 Host 数量、Owner 和唯一名称，再在安全帧边界调用 `apply_pending()`
进行整体交换。任何 staging 失败都不会修改当前 active 组合，也不会逐 Host 发布部分状态。
停止预览时调用 `stop_and_retire()`；它在同一安全边界停用仍处于 active 状态的 Host，
并清空待应用及当前组合，避免编辑器停止后继续执行旧 Plan。

Host 的技术 Pipeline/Feature 声明配置使用不可变快照；RenderPass 通过现有 RenderGraph
构图入口加入执行图。模块卸载必须等待组合、帧和回调引用全部退休后进行。

EngineFramePostBuildContext 提供本帧 preparation 已解析的纹理绑定，只在回调期间借用。
Application Runtime 在此边界把 owner 已提交的 RenderIntent 快照展开到同一个 RenderGraph，
随后调用原有 readback 回调。Frame profile 的基础设施继续复用；五域 Feature 的业务身份
由每个实际 Pass 的 Feature/Node/Instance/Invocation/Frame 来源追踪。

FeatureGraph 的 World view 可通过无 Pipeline host 的 register_world_frame 重用 WorldFrameScope
和 Renderer 提取路径。post-build context 同时借用当前帧 owning 场景快照 span 与资源解析器，
供 Renderer 中立绘制降级使用；快照租约由 RenderGraph 转交 FrameContext 退休。技术 Frame host
继续使用同一个注册/桥接 helper，避免重复构建场景。

Application 的纯 FeatureGraph 入口按已接受的 Draw 意图请求场景提取；没有 Draw 的帧
不要求 Camera，也不解析不需要的场景资源。

GBuffer 场景意图也请求同一场景提取路径。owned-host 按已接受意图需求为
Renderer.GBuffer.Albedo/Normal/Material 绑定 View 尺寸的临时附件，复用深度附件的
分配/绑定函数和现有 Renderer.SceneDepth。宿主从同一个真实 MRT PSO 创建描述发布
完整附件契约；RuntimeAssetResolver 每次解析转发当前句柄和契约，暖材质复用描述符
及参数缓冲。游戏侧只持有 Feature 资源引用，不持有分配器、PSO 或原生纹理。

DeferredLighting 也触发同一场景提取；owned-host 按意图需求通过现有绑定函数提供
Renderer.SceneLighting HDR 槽及被读取的 GBuffer/阴影槽，不另建分配器。现有光照
缓存以帧编号、View ID、完整已验证常量为键；同帧不同场景的 GPU 数据保持不可变，
上传收据使用同一身份，清空未提交工作后在原分配上重录。仍由宿主显式退休缓存，
不以此宣称有界逐帧驱逐。Deferred PSO 附件合同随 live resource 一起转发。
## 中立后处理

RenderPostProcessScene 按需使用现有 SceneLighting 外部 HDR 槽和场景深度，复用 owned
宿主附件分配/绑定函数与 post_process_resolver。作者可在 Feature 中指定纯值设置；
宿主沿用真实 PSO 合同、帧常量和退休服务。非空打包 smoke 验证 Scene UUID 重定位与
GBuffer→Deferred→PostProcess→最终读回，不用独立清屏作为几何成功证据。

现代 Scene Feature 输出由 Application 的 post-build 路径调用 Renderer lowerer，并传入实际
native PassRegistry 及其当前 execution epoch。Orchestrator 不再遍历 pinned Pipeline instance
构建旧 RenderFeature，不再读取该 registry 的 epoch 或扩展 authored pass，也不缓存旧
`RenderFeatureContext`。`EngineFrameDesc::render_feature_buffers` 和 `render_feature_state`
以及对应 buffer/state forwarding 已删除。

技术 Pipeline Registry 与 Scheduler 由 Runtime-private `EngineFramePipelineHost` 借用。
Runtime Bootstrap 和 Editor Scene Runtime 在整个 orchestrator 生命周期内保持该 host，
因此 Runtime public 构造边界只传递 opaque host 引用，不传播 Pipeline service references。

Frame 技术 host 注册通过 Runtime-private `EngineFramePipelineRegistration` 适配器
完成；该入口明确拒绝原始 World Pipeline。World 通过公开的 aggregate 注册进入
命令应用、WorldCommit 和只读快照捕获。`EngineFrameOrchestrator` 的 registration API 不要求调用方直接传递
`PipelineScopeHost`；typed host 只在 private implementation 解包，注册重复检查和回滚语义
保持原有路径。

公开 Orchestrator 采用 PImpl；技术计划 pin、Pipeline host、Artifact bridge 与 phase registry
位于实现文件中。构造能力值使用 uint64_t，资源身份来自 Feature，
公开的场景与资源句柄来自 Renderer/RenderGraph。原 abort、release、deactivate、clear
清理顺序和 World/View 回滚保持。`EngineFrameDesc::user_data` 仍传入实际 Renderer extraction
并用于 Stage inspection 的身份观测；`render_resource_resolver` 仍参与提取并借给 post-build
中的 Scene 投影。它们不再转发到旧 instance RenderFeature context。
Pipeline 不再承载 CPU callback/Task/worker。编排器只访问技术 Stage 计划，帧资源/RG 构建
按原生命周期运行。内部 engine_frame_inspection.hpp 提供按需启用的 owning Stage trace，
记录真实 phase/host/frame/userdata identity、WorldFrameKey、typed snapshot 与实际 host
Artifact bridge。默认不累计，活跃帧不能切换，capture 副本在帧清理后仍持有旧不可变值；
这里没有任意 observation callback。真实业务由 FeatureRuntime 的调用/提交结果驱动，
固定 World.Transform typed outbox 由下面的生产适配器消费；demo/callback 归属继续迁移。

Frame 技术计划的 Registry 接收拥有的静态声明值，不再执行 factory、virtual build 或
collect/setup。实际 snapshot Stage 和 Artifact 记录由固定 compiler 展开，plan pin 保
真正不可变 declaration owner；编译失败不产生部分计划或执行业务写入。Frame Stage、Artifact
bridge、plan pin 与 RenderGraph execution leases 继续负责技术执行和生命周期，不提供业务
invocation 来源。项目仍沿原 FeatureGraph compile/publish、FeatureRuntime commit/capture 与
post-build 投影路径执行；World typed outbox 与业务 callback 归属要求继续保留，
声明 owner 不能替代现代 invocation 来源。

`private/feature_world_commands.hpp/.cpp` 提供固定 `World.Transform` v1 codec 和 consumer。
对象使用真实 `Game.Transform` schema1 和既有 Reflection Serialization，payload 包含预期
World revision、expected Transform 与 replacement Transform。严格 canonical record 和
finite 值检查拒绝缺字段、重复/未知字段、错误版本及尾部字节，不复制 Vec3 padding。
绑定保存 canonical Feature source 与 World 私有的实际生命周期 weak lease，实体仅来自
显式 route。组件 schema/存储、owner 线程、World 代次、revision 和 typed expected-before
全部检查后，一次 noexcept 赋值进入 Feature consume/Applied 确认。未通过预检的条目保
Pending；不同成功提交的旧 pending 仍按源顺序交付。重复 source/protocol route 拒绝。

Engine-only `EngineFrameWorldCommands` 在帧间借用该 consumer，`commit_worlds` 在每个
World snapshot commit 前消费。真实 snapshot copy 失败可以留下 Applied 命令和旧 snapshot，
重试仅重新发布 snapshot，不重放已应用命令。无 renderer 的 Bootstrap 使用同一 consumer。
World 不反向依赖 Runtime 组件，公开 Orchestrator 和 GameSDK 不暴露此执行接口。
