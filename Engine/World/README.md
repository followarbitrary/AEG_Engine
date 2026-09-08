# World

## 目录职责

`World` 是 ECS 可变状态与 Domain-Oriented Pipeline `kWorld` Scope 之间的适配层。
它负责把一个具体 `ecs::World` 绑定到 World Pipeline Host，维护标准
`WorldSnapshot` Artifact，并为每个 Frame/View 建立只读的 World-to-Frame Bridge。

## 公共入口

- `public/world_scope.hpp`：`WorldScope`、`WorldFrameScope`、`WorldFrameKey` 和标准
  World Snapshot Artifact 身份。
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
可变状态所有权。`WorldScope` 不拥有 Pipeline Registry，但拥有自己的 Pipeline Host、命令
缓冲和快照引用。

World Pipeline Host 的 Scope 固定为 `PipelineScope::kWorld`；Frame 只能通过
`ArtifactScopeBridge` 读取已提交快照，不得取得 `ecs::World&`。World 销毁、Frame
退休和 Pipeline Plan 切换必须在对应的生命周期边界完成。每次
`WorldScope::initialize()` 都会建立新的 Scope 活跃代际；`shutdown()` 会使该代际失效
并清空尚未消费的命令。已经完成 `capture()` 的 `WorldFrameScope` 仍可读取其持有的
不可变 Snapshot，但在关闭后不得再次 `capture()`；重新初始化也不会复活旧的 Frame Scope。

`PipelineExecutionContext::user_data()` 保留为编排调用方提供的宿主上下文；
World Pipeline 通过 `PipelineExecutionContext::scope_data()` 获取 `WorldScope*`，
Frame Pipeline 通过同一接口获取当前 View 的 `WorldFrameScope*`。这些指针均为非拥有
借用，不得跨越 Scope 或 Frame 生命周期保存。

## 依赖边界

该模块可以依赖 ECS、Pipeline、Serialization、Reflection 和 Foundation，不依赖 RenderGraph、
RHI 后端或窗口系统。World Archive 仅复用通用 Reflection Serialization 的对象编码，
不改变 World Scope 的跨作用域数据流规则。
统一帧编排由 `EngineFrameOrchestrator` 完成：`PreUpdate` 应用命令，`WorldCommit`
发布快照，随后 Frame Scope 执行 `RenderExtract`。Frame Pipeline 的
`PipelineExecutionContext::scope_data()` 指向当前 `WorldFrameScope`，其 `key()` 提供
`frame_number`、`WorldHandle` 和 `view_id`；Frame 仍只能通过该 Scope 的桥接快照读取
World 数据。
