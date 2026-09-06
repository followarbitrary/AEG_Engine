# ECS

## 目录职责

该目录用于承载 Entity-Component-System 以及 World 数据管理，包括实体标识、组件
存储、查询、结构变更命令、快照和并行访问协议。

## 当前状态

目录目前仅作为架构预留。World Scope 的 Pipeline 应以 World 句柄或实例为所有者，
不得通过全局单例共享不同 World 的可变状态。

## 边界

ECS 负责组织和发布 World 数据，不负责渲染命令录制、不直接管理 RHI 资源，也不
替代 `Pipeline` 的阶段依赖图。跨 Scope 数据应通过快照、Artifact 或明确的桥接
协议传递。
