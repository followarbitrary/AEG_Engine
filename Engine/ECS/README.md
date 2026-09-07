# ECS

## 目录职责

该目录用于承载 Entity-Component-System 以及 World 数据管理，包括实体标识、组件
存储、查询、结构变更命令、快照和并行访问协议。

## 当前状态

当前提供 `ecs.hpp`，包含 `WorldManager`、`World`、`EntityId`、`WorldHandle`、
`WorldCommandBuffer` 和 `WorldSnapshot`。组件采用按类型注册的 dense/sparse pool；
实体与 World 均使用 generation 校验，避免索引复用后访问旧句柄。

`World` 的可变 API 默认只允许 owner thread 调用。跨线程调用方应将意图编码到
`WorldCommandBuffer`，由 World 在 `Input`/`PreUpdate` 的受控窗口应用。`commit()` 会
生成独立的不可变快照，快照可以被多个 Frame/View 并行读取。

组件查询期间禁止实体创建、实体销毁、组件类型注册、组件增删和命令批量应用等
结构性变更；查询回调可以修改当前组件值，但不得改变正在遍历的 pool 结构。
查询结束后，World 才重新接受这些结构性操作。

## 边界

ECS 负责组织和发布 World 数据，不负责渲染命令录制、不直接管理 RHI 资源，也不
替代 `Pipeline` 的阶段依赖图。跨 Scope 数据应通过快照、Artifact 或明确的桥接
协议传递。组件要参与快照，必须满足可复制构造、无异常移动构造/赋值和无异常析构
约束；这使 dense pool 的删除和快照复制具有可预测的异常语义。
