# RenderGraph

中立 `RenderDrawScene` 由 Renderer 实现引擎内部 `RenderSceneDrawLowerer` 接口，
RenderGraph 只提供复用 backend、实际目标描述、构图事务、操作顺序及来源记录。
返回的 owning 场景租约在 execute 时转交 FrameContext，直到 GPU 帧退休。RenderGraph
不反向依赖 Renderer；作者意图只有中立值，不包含该接口或原生句柄。

## 目录职责

`RenderGraph` 管理帧内 GPU 资源和 Pass 的声明式有向无环图，负责依赖分析、资源
生命周期、显式 Barrier、跨队列同步、Transient 资源分配以及执行提交。

## 当前实现

- `public/render_graph.hpp`：资源、Pass、Plan、Barrier 和执行接口。
- `private/render_graph_execution.hpp`：RenderGraph-private 的 Device、编译元数据与执行计划
  访问桥；`device`/`compile_info`/`plan` 不属于 public authoring/value contract。
- `private/render_graph_resource_access.hpp`：RenderGraph-private 的 handle generation、
  graph identity 与 ownership membership 查询；`contains_buffer`/`contains_texture` 不属于
  public authoring/value contract。
- `public/frame_context.hpp`：帧槽、命令提交、完成跟踪和延迟销毁。
- `public/render_feature_runtime.hpp`：将已提交的 Feature RenderIntent 展开为实际图步骤；
  `build_render_graph_features()` 接收 owning 输出快照与中立纹理绑定，不传播 Pipeline 类型。
  `RenderGraphFeatureRuntimeDesc` 只携带 textures、借用的 opaque Renderer lowerer、
  current-frame owner 与 history key。旧 instance Runtime、Pipeline Plan 执行入口、buffer
  binding DTO、配置/PassHost/state descriptor transport 已删除。
- `public/render_feature_projection.hpp`：在 `aeg::render_graph` 中承载五域编译结果投影所需的
  帧内类型化 WorkItem 值。实际 Pass 保留 Feature/Module/Node/Instance/Invocation/Frame 来源；
  一个 Feature 可以拥有多个内部 Pass。
- `private/render_feature_projector.hpp`：RenderFeature 到执行计划和 WorkItem 的 typed
  lowering adapter；它供 Engine 宿主和 free build 入口核对实际捕获的输出，不属于公共
  authoring/value contract。
- `RenderPassCommandContext`：执行回调只能解析已准备资源并录制命令，不能取得 `Device`
  或创建/销毁 RHI 资源；attachment view 和声明的 sampled-read view 在回调前由 Graph 准备。
- 图创建的纹理与视图通过 FrameContext 保留到 GPU 完成。重建图时可立即释放没有 descriptor
  引用的句柄；仍被当前帧 descriptor 引用的资源在 descriptor 退休后释放，导入纹理本体仍由宿主拥有。
- 宿主替换导入纹理或退出时，可在无活动帧的边界调用 `FrameContext::drain()`：等待已提交帧，
  先清理所有命令引用，再退休 descriptor 和共享所有权。帧编号与暂存分配内容保留到正常帧槽复用。
- `private`：图编译、运行时状态、帧上下文和 Feature Runtime 的实现。跨图的
  `RenderFeatureHistory` 与 `RenderFeatureHistoryTexture` 也属于 Engine-private temporal
  host；free build API 以借用的 opaque history owner 参数进入私有事务，不传播历史
  纹理句柄、租约或事务声明。

## 依赖边界

RenderGraph 只管理 Render Domain 的图，不承担通用业务 Pipeline 的阶段调度。它
通过 RHI 的后端无关接口提交工作，不能在公共接口中暴露 Vulkan 或 D3D12 类型。

## 内部 RenderPass 投影契约

Frame/Render 投影中的 RenderPass 继承 `aeg::pipeline::RenderPass`，由 `RenderPassRegistry` 按稳定
`RenderPassTypeId` 创建。Pass 的 setup/execute 回调只能使用 `RenderPassBuilder`、
`RenderPassExecutionContext` 和当前 `RenderPassConfiguration`；资源状态、依赖和同步由
RenderGraph 统一管理。Pass 不得保存 Device、原生命令缓冲或跨帧 Graph Handle。
游戏模块的公开注册入口只发布 FeatureDefinition；这里的 Pass 类型属于引擎内部投影。

固定 native Pass 的 registry 解析使用私有 `RenderPassExecutionHost`，借用当前
`RenderPassRegistry` 与 epoch。Renderer lowerer 在实际 Scene operation 或固定 native
sequence 中提供该能力，构图时验证当前 epoch、配置及 prepared identity。解析得到的
实际 pass 和 configuration 都由 Graph 保留至 GPU 帧完成；注销 registry 或 reset 图
不会提前释放已提交命令依赖的所有者。排序使用真实注册 pass handle。

旧 `PipelineFeatureDeclaration`、FeaturePoint、结构 Feature 注册/查询/绑定、贡献展开和
Pipeline instance 配置链已物理删除；没有保留旧入口的兼容 facade。固定 Frame Stage、
Artifact 和拓扑直接来自 owning `PipelineDefinition`，计划保留实际 `definition_owner`。
技术编译、Stage 观测、Artifact 发布与访问、plan pin/切换/退休及 execution lease 继续保留；
业务 Render 操作由实际 committed Feature 输出提供。
内部 `RenderPassRegistry`、native setup/execute、Core Graph 与命令录制合同仍保留，
它们服务固定技术执行。

私有 native Builder 的权限由独立 `pipeline::RenderGraphBuildFlags` 表达，共六位：
资源创建/导入、Graphics/Compute/Copy Pass、专用队列及最终输出写入。资源位名为
`kMayCreateResources`，不再与 Artifact 声明混用；旧挂点的 Snapshot 权限位已删除。
`RenderGraphBuildContext` 仅有 `flags` 和借用的 `context_data`；Builder 不携带旧 scope、
Pipeline/Feature/Part/Point 身份、作者配置或 userdata。现代 projector 从真正的 Feature
权限合同生成这些 build flags；固定 NativeSession 由 Engine 宿主显式提供所需权限。

## 已提交 Feature 意图

业务构图直接调用 `build_render_graph_features()`。它先从真实捕获的 RenderFeatureOutput
生成执行计划与 WorkItem，再在事务中展开类型化操作，不再实例化 Pipeline contribution
Runtime 或回调旧 Feature 图构建对象。纹理绑定数组只借用至本次构图返回；其句柄必须
属于当前 Graph，并通过版本、实例归属及实际资源能力检查。

Feature 构图事务、execution-owner/history observer retention、WorkItem provenance、
texture description/alias 和 commit/rollback 由 `private/render_graph_feature_access.hpp`
统一访问；public `RenderGraph` 只保留作者可用的资源、Pass、compile 和 execute 合同，不
暴露这些 Feature runtime helper members。

五域 Feature 的 v1 意图使用同一后端、权限检查、Graph 构图事务和 FrameContext 执行保留。
每实例局部资源表独立；外部绑定必须满足 ID、版本、实例归属和实际纹理能力。构图失败
回滚整份意图快照新增的资源/Pass/保留引用。颜色清屏依据纹理能力复用 attachment 或
transfer 路径；交换链的 acquire、最后访问后的 Present 转换与呈现由 Graph 管理。
所有被引用的外部绑定禁止以多个图句柄导入同一原生纹理；复用同一个图句柄合法。
纹理复制拒绝源目标别名，要求匹配格式/采样数及合法区域与传输能力。v1 操作仅声明
mip 0/layer 0；局部复制保留未覆盖像素，其他 mip/layer 不发生写入或 discard。
意图快照随提交保留，直到 FrameContext 退休；清理 Graph 不会提前释放 GPU 工作的所有者。

跨帧 `RenderFeatureHistory` 按宿主提供的 `HistoryKey`、Instance/局部资源 ID 保存原生纹理，
验证 Node、定义摘要和资源 epoch；Renderer 通常使用 `RenderView::id.value()` 作为 key，
因此同一 Feature 在不同 View 中不会共享历史 allocation。复用 Graph 纹理所有者与
FrameContext 保留；导入视图同时保留底层纹理。
写入在构图事务中暂存新分配，局部写入先复制旧内容，成功执行后才发布；构图/执行失败
保留原历史，历史写入本身是裁剪根。相同版本变更尺寸拒绝，新版本成功提交后替换。
读取只接受已初始化的当前版本；不提供隐式 ping-pong。历史 Graph 每次构图只能尝试
执行一次，成功或失败后必须 reset/重建，避免绕过历史提交事务重放。
每个图只接收一次完整 Feature history 快照，追加另一独立历史事务会回滚该次构图。
有效空意图在成功执行后释放该实例省略的历史声明；缺少输出保留其活跃历史。

这条执行链不增加游戏侧 Builder 或 Pass 注册。RenderDrawScene 通过引擎 Renderer
lowerer 复用原生 Opaque 绘制，作者提交场景 ID/版本及外部目标的 Load/Clear。包括惰性
网格/纹理上传在内的全部新增 Pass 统一附加 Feature 来源和操作依赖。
可选外部 D32Float 深度附件复用颜色绑定的版本/实例检查和事务，拒绝颜色/深度别名。
Load 保留已有深度，Clear 使用作者声明的有限 [0,1] 值；颜色操作仍拒绝深度格式。
RenderDrawDepthScene 复用场景 lowerer、整图回滚、来源/操作依赖和帧保留；depth_output
独立导出外部 D32 图根，颜色 final_output 的格式约束不变。未导出且无消费者的深度工作
仍参与普通图裁剪，不强制设置副作用。
原生 Shader/PSO 与各 profile 的附件合同由 Renderer lowerer 校验，作者意图不直接传递
这些执行对象。

RenderDrawScene.directional_shadow 解析为外部 D32 sampled 输入；版本、归属和目的附件
别名检查属于同一个构图事务。实际 SampledRead 将阴影 producer 与消费者连接，
没有导出或消费者的深度工作仍可裁剪。多个 Feature 导入同一原生缓冲时，相容的
创建/初始状态合同共用一个图身份和状态历史；不相容合同及产生重复原生别名的
update_imported_buffer 被拒绝。重复导入不会重置执行后的状态或队列依赖。

内部 Pass 上传回执仅证明批次已提交，不代表 GPU 已完成。录制寿命随 Pass 释放，
回滚或 clear_passes 不会把缓存资源标记为已上传；解析器复用 GPU 分配并重新暂存上传。
回执只由构图/执行所属线程访问，不向游戏 SDK 暴露。

RenderDrawGBufferScene 通过同一事务解析四个宿主附件。仅明确的 albedo/normal 角色
接受 RGBA16Float；普通颜色清屏、复制和 final_output 保持 LDR。四个原生绑定先检查
别名，Renderer 再核对固定顺序、附件形状及真实宿主 PSO 契约。中立入口复用现有
GBuffer 网格、材质、对象解析和原生录制循环，每个附件独立 Load/Clear。
无消费者的 MRT 工作可裁剪；实际 Load/读回消费者建立资源依赖并保留生产工作。
同一来源、操作索引、执行所有权及帧退休机制也覆盖 MRT，不增加作者 Pass 身份。

DeferredLighting 的明确 HDR/albedo/normal 角色接受浮点颜色；普通 Clear/Copy/final_output
继续保持 LDR。采样 depth/shadow 使用深度角色，五个输入/输出先检查原生别名，之后
由同一 Renderer lowerer 校验 Sampled 能力、View 形状与实际 base/shadow PSO 合同。
复用现有全屏 recorder，仅声明 shader 真正读取的资源；无输出消费者时 Graphics 绘制
可裁剪。持久光照上传沿用 ResourceUploader 的 Copy 副作用，并保留相同 Feature 来源。

PostProcess 解析明确的 HDR、深度和 LDR 输出角色，先在同一事务内验证版本、所有者及
别名，再经 Renderer lowerer 校验 View 形状、采样/附件能力和真实 PSO 合同。内部工作
保留 Feature/Node/操作索引/调用/帧来源；不增加作者 Pass。中立后处理不强制副作用，
通过 final_output 或真实消费形成图根，场景/意图 owning 租约延续到 GPU 帧退休。

## 私有执行边界

`RenderGraphFeatureRuntimeDesc` 的 `draw_lowerer` 只借用到本次 free build 调用返回。
完整 `RenderSceneDrawLowerer`、目标附件、depth/light-list production、租约与 build scope
位于 `private/render_scene_lowerer.hpp`，公共 runtime header 不传播 Renderer 实现类型。
`current_frame_owner` 同样只在私有构图处恢复为 FrameContext，用于实际 frame/device
与 scope 校验。history owner 单独传入；history key 保持各 View 的时序隔离。

旧 `RenderGraphFeatureRuntime` instance、`render_feature_runtime_pipeline.hpp` Plan 执行桥、
`RenderGraphPassHost` adapter、终端 composer、state-flow 与 Pipeline state input/output/layer
字段已删除。现代投影仍在现有 free API 内使用原来的类型化 lowering、事务与资源检查。
固定 NativeSession 的初始绑定 helper 仅接收纹理，使用独立 BuildFlags 与 BuildContext；
内部 Builder 的实际 buffer create/import/read/write、注册 native Pass、已解析 Pass handle
排序和执行 owner 保留继续可用。旧 `PipelineInstanceConfiguration` 及其 Feature/Pass
作者配置 header 已删除，固定 `RenderPassConfiguration` 和 native sequence 仍保留。

Upload submission receipts follow the same ownership rule. The full
`RenderGraphPassSubmission` object lives in `private/render_graph_upload_submission.hpp`;
Graph, Resource uploader, and Runtime resolver public-facing values carry only an opaque
`shared_ptr<const void>`, while readiness inspection remains private to Runtime.

Engine frame Pipeline services follow the same direction. `EngineFrameOrchestrator` receives
the Runtime-private `EngineFramePipelineHost`; Pipeline Registry/Scheduler access is resolved
inside the Engine private implementation and does not cross the Runtime public constructor
boundary.

Feature history submission observers are also private. `RenderGraph` retains the transaction
through an opaque `shared_ptr<const void>` owner, while the observer interface and its typed
callback dispatch stay in `private/render_graph_submission.hpp` and `render_graph.cpp`.

Feature history storage remains private to RenderGraph. The free build entry accepts a separate
borrowed opaque history owner; its private implementation restores `RenderFeatureHistory` through
`detail::RenderFeatureHistoryRuntimeAccess`. History-key transactions, device validation,
submission, rollback, and GPU leases remain internal.

Feature projection assembly accesses RenderGraph-private transaction and attribution helpers
through `detail::RenderGraphFeatureGraphAccess`. The public RenderGraph header keeps these
helpers out of its author-facing surface; the private bridge serves committed Feature projection
and fixed NativeSession transactions.

Resource upload receipt tracking follows the same rule. The public Graph header does not expose
`track_pass_submission`; Resource-private uploaders call `detail::RenderGraphUploadAccess`,
while the Graph implementation keeps the actual receipt bookkeeping and frame-lifetime token
private. This preserves rollback, reset and frame-mismatch validation without making upload
lifecycle an author-facing RenderGraph contract.

The private Scene lowering source copies the projector's actual operation variant, work,
owner and View. Native command owners transfer to FrameContext until GPU completion;
failed Feature builds roll them back. Private pass inspection copies actual record name
and queue after validating the handle's graph and generation.

Frame retirement waits for all selected slots and clears command references, then releases
deferred resources by dependency kind across slots. Descriptors in a later slot therefore
retire before views referenced by them in an earlier slot. Failed handles remain pending for
retry; drain preserves the original error and does not advance the frame counter.

The private NativeGraphBuildSession reuses the same graph builder backend for fixed Engine
topology assembly. It creates no Pipeline/Feature identity or business callback. Uncommitted
destruction restores the actual transaction checkpoint, including native execution owners;
committed native configurations remain owned by the graph and submitted frame. Business Scene
operations continue through committed Feature projection, with their actual source attribution.
EditorFramework's fixed ImGui overlay also uses this session against its actual target. It
validates the prepared attachment and preserves transaction rollback, Load/side effects and
prepared-frame owners without registering a technical UI pass as a business Feature.
