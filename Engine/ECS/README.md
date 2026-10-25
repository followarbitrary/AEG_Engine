# ECS

## 目录职责

该目录用于承载 Entity-Component-System 以及 World 数据管理，包括实体标识、组件
存储、查询、结构变更命令、快照和并行访问协议。

## 当前状态

当前提供 `ecs.hpp`，包含 `WorldManager`、`World`、`EntityId`、`WorldHandle`、
`WorldCommandBuffer` 和 `WorldSnapshot`。组件采用按类型注册的 dense/sparse pool；
实体与 World 均使用 generation 校验，避免索引复用后访问旧句柄。

组件可以通过 `register_component<Type>()` 注册为普通运行时组件，也可以通过
`register_reflected_component<Type>()` 绑定一个 `reflection::TypeDescriptor`。后者会将
ComponentTypeId 与 Reflection TypeId、Schema Version 一并写入 `ComponentDescriptor`，
并在 `WorldSnapshot` 中保留这些元数据，使 World Archive、编辑器和工具链能够按稳定身份
访问 type-erased 组件值。普通组件仍可参与 ECS 快照和渲染提取，但不能被当前
`WorldSerializer` 持久化。

`World` 的可变 API 默认只允许 owner thread 调用。跨线程调用方应将意图编码到
`WorldCommandBuffer`，由 World 在 `Input`/`PreUpdate` 的受控窗口应用。`commit()` 会
生成独立的不可变快照，快照可以被多个 Frame/View 并行读取。

owner 身份统一使用 `aeg::concurrency::ThreadId` 与
`aeg::concurrency::current_thread_id()` 比较；ECS 不创建或管理自己的线程。

组件查询期间禁止实体创建、实体销毁、组件类型注册、组件增删和命令批量应用等
结构性变更；查询回调可以修改当前组件值，但不得改变正在遍历的 pool 结构。
查询结束后，World 才重新接受这些结构性操作。

## 快照数据模型

`WorldSnapshot` 是不可变、可共享的提交结果。除按 C++ 类型查询外，它还提供
`component_type_at()`、`component_value_count()`、`component_entity_at()`、
`component_value_at()`、`component_schema_type()` 和 `component_schema_version()`，供
Reflection Serialization 等 type-erased 消费者遍历组件数据。`component_type_at()` 按
ComponentTypeId 的稳定升序返回注册组件类型；组件值仍按其 dense pool 顺序返回。

`World::clear()` 销毁当前所有实体和组件值，并使仍在使用的实体代际失效。它只清空运行时
状态，不注销组件类型，因此适合归档恢复前的目标 World 重置。

## 边界

ECS 负责组织和发布 World 数据，不负责渲染命令录制、不直接管理 RHI 资源，也不
替代 `Pipeline` 的阶段依赖图。跨 Scope 数据应通过快照、Artifact 或明确的桥接
协议传递。组件要参与快照，必须满足可复制构造、无异常移动构造/赋值和无异常析构
约束；反射组件还必须默认可构造，以便 `WorldSerializer` 在恢复时创建目标对象。这使
dense pool 的删除、快照复制和归档恢复具有可预测的异常语义。
