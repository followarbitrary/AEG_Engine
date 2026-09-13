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

`register_render_component_reflection()` 显式向调用方提供的 `TypeRegistry` 注册三种
schema；`register_reflected_render_components()` 为一个 `World` 注册对应的持久化 ECS
storage；`register_render_component_bundle()` 顺序执行两者。注册操作会验证已有身份的
元数据，兼容的重复注册保持幂等，不兼容的注册返回失败。仅需要临时、不参与 Scene
持久化的 World 可以使用 `register_render_components()` 注册非反射 storage。

`Transform` 的 `Vec3` 字段使用 `Math.Vec3` 的固定 16 字节四浮点 wire layout：前三个
浮点为 XYZ 语义，第四个槽位在写入和读取后规范化为零，绝不按紧凑三个浮点编码。`Quat`
使用 `Math.Quat` 的四个 XYZW 浮点编码。

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

`EngineFrameDesc::render_feature_preparation` 运行在 RenderBuild 的 Feature 构图之前，适合
导入帧内外部目标、分配 Readback 或准备 Upload。回调可先通过
`EngineFramePreparationContext::render_graph` 导入 Texture，再调用
`bind_feature_texture(RenderResourceId, RenderGraphTexture)` 把稳定逻辑资源身份提供给后续
RenderFeature。该绑定只在当前活跃帧且 Feature 构图前有效；重复身份必须指向同一 Texture，
Context、FrameAllocation 和 RenderGraph Handle 都不得跨帧保存。

Opaque RenderFeature 的网格资源还必须声明 `RenderMeshVertexLayout`：interleaved Vertex
Buffer 至少包含 Position `float4` 和 UV `float2`，并且 stride、offset、format 必须与当前
Shader Contract 一致。缺失 UV、顶点格式不匹配或越过 Vertex Buffer 范围时，RenderBuild
应返回失败；该失败不得继续提交不完整的绘制。

## 依赖边界

Runtime 可以依赖 Foundation、Math、ECS、Reflection、Serialization、Pipeline、World、
RenderGraph 和 Renderer 的公共接口，但不得包含它们的 private 头文件，也不得直接暴露
Vulkan、Win32 或其他 native API。Scene 依赖 Runtime 组件公开的数据契约和 Reflection
schema，不反向要求 Runtime 依赖 Scene。
