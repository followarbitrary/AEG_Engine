# World

## 目录职责

`World` 持有 ECS 领域聚合状态、宿主命令和不可变快照。WorldScope 只接受已有的
`ecs::World` 所有权，不接受业务 Pipeline、Registry 或执行计划注入。每个 Frame/View
读取强类型 WorldSnapshot；底层 Artifact 存储与桥接属于 Engine 私有技术实现。

## 公共入口

- `public/world_scope.hpp`：`WorldScope`、`WorldFrameScope` 和 `WorldFrameKey`，不传播
  Pipeline/Artifact 头或类型；Frame 读取拥有共享不可变数据的 WorldSnapshot 值。
- `public/world_serializer.hpp`：`WorldSerializer` 与 `WorldSerializationOptions`，用于
  对已提交的 `ecs::WorldSnapshot` 执行运行时 World Archive 编解码。
- `WorldScope::command_buffer()`：外部输入、网络和 Editor 命令的线程安全入口。
- `WorldScope::commit()`：将 ECS 状态发布为不可变快照并写入 World Artifact。
- `WorldScope::create_frame_scope()`：创建绑定到指定 frame number/view id 的 Frame 读取视图。

## World Archive

`WorldSerializer::serialize()` 只接受已提交的 `WorldSnapshot`，并将 frame number、fixed
tick、EntityId、ComponentTypeId、Reflection TypeId、Schema Version 以及每个组件值的
反射对象记录写入 `serialization::BinaryWriter`。`deserialize()` 在读取并完整校验归档
后，清空目标 World，重新创建运行时实体并恢复组件值；源归档中的 EntityId 只用于建立
临时映射，不能作为跨会话稳定身份。

归档要求所有包含值的组件都已通过 `register_reflected_component<Type>()` 注册，并且目标
World 的组件注册必须匹配 ComponentTypeId、Reflection TypeId 和不低于归档的 Schema
Version。当前版本允许读取不高于目标版本的旧数据，但尚未提供 Schema migration callback；
新增字段只能依赖默认初始化语义。未反射且为空的组件池不会写入归档，包含值的未反射组件
会使序列化失败。

读取阶段失败不会修改目标 World；恢复阶段失败会清空目标 World，以避免暴露部分恢复状态。
`WorldSerializationOptions` 用于限制实体数、组件类型数、组件值数和单值载荷大小，调用方
应根据存档来源设置适当上限。该格式是运行时 World Archive，不是最终的 Scene、Prefab、
资产容器或网络协议；稳定场景 GUID、资源引用和版本迁移应由后续 Scene Asset 层定义。

## 所有权与生命周期

推荐使用 `WorldManager::find()` 返回的 `std::shared_ptr<ecs::World>` 构造
`WorldScope`，确保 Scope 与 Manager 查询到的是同一份可变状态。当前公共构造函数只接受
`std::shared_ptr<ecs::World>`；不得仅凭 `WorldHandle` 重新构造 World，因为句柄本身不携带
可变状态所有权。`WorldScope` 持有命令缓冲和快照引用，没有 Pipeline Host。

Frame 只读取已提交的不可变快照，不取得 `ecs::World&`。World 销毁和 Frame
退休必须在对应的生命周期边界完成。每次
`WorldScope::initialize()` 都会建立新的 Scope 活跃代际；`shutdown()` 会使该代际失效
并清空尚未消费的命令。已经完成 `capture()` 的 `WorldFrameScope` 仍可读取其持有的
不可变 Snapshot，但在关闭后不得再次 `capture()`；重新初始化也不会复活旧的 Frame Scope。

Engine 私有的 Frame 观察器仍可使用当前 View 的技术 Artifact 桥；私有
`WorldSnapshotExecution` 提供 canonical 身份和受控桥访问。Orchestrator 明确拒绝
原始 World Pipeline 注册，不再向任务提供可变 WorldScope 指针。

## 依赖边界

公共模块依赖 ECS、Serialization、Reflection 和 Foundation；Pipeline 仅为私有链接依赖。
World 不依赖 RenderGraph、
RHI 后端或窗口系统。World Archive 仅复用通用 Reflection Serialization 的对象编码，
不改变 World Scope 的跨作用域数据流规则。
统一帧编排由 `EngineFrameOrchestrator` 完成：`PreUpdate` 应用命令，`WorldCommit`
发布快照，随后 Frame Scope 执行 `RenderExtract`。Frame Pipeline 的
`PipelineExecutionContext::scope_data()` 指向当前 `WorldFrameScope`，其 `key()` 提供
`frame_number`、`WorldHandle` 和 `view_id`；Frame 仍只能通过该 Scope 的桥接快照读取
World 数据。

World-frame 回归使用真实 FeatureGraph、Logic Call invocation 和已提交 outbox，经 Engine
Runtime 的生产 typed consumer 应用真实 Game.Transform。外部 command→5、两个已接受
pending→6→7、WorldCommit→Frame snapshot 保持顺序，失败预检不改 World 或确认交付。
真实 copy-throw snapshot 失败后，Applied receipt 保持，移除失败组件后可重试发布而不重放。

多 View 的帧观察使用 Runtime 私有 owning Stage trace，直接保留当前 WorldFrameScope
的 immutable snapshot 和 host 实际 bridge read，验证 Scope 地址、帧号、revision、View、
userdata 及 sentinel bridge 恢复。Frame Pipeline 的 raw CPU callback 已删除；业务夹具
使用真正 Feature StateDelta/outbox，由固定生产 consumer 应用；诊断 trace 只保留观察值。

`private/world_generation_access.hpp` 仅为 Engine 内部适配器捕获/验证原 active_token_ 和
实际 ECS owner 的 weak lease。shutdown/reinitialize 后旧 route 失效，即使 WorldHandle
数值相同也不能复活。这里不添加代次 counter，也不引入 Runtime、Feature 或组件协议
依赖；固定 Game.Transform 解码和授权消费属于 Engine Runtime。其余 World 业务仍继续
按五域主线迁移，不能据此宣称全部组件已有通用应用事务。
