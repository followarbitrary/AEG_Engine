# Serialization

## 目录职责

该目录用于配置、存档、资产元数据和运行时对象的序列化与反序列化，包括版本迁移、
格式注册、增量保存和错误恢复策略。

## 当前状态

当前提供 `serialization.hpp` 中的 `BinaryWriter`、`BinaryReader`、
`serialize_object()` 和 `deserialize_object()`。该实现使用小端、自描述字段记录：对象
携带 `TypeId` 和 Schema 版本，每个字段携带稳定 `FieldId` 与字节长度，因此读取旧版本
数据时可以跳过未知字段，读取高于当前 Schema 的数据时会明确返回不支持。

当前序列化器依赖 `Reflection::TypeRegistry`，支持标量、字符串和已注册嵌套对象，并对
输入长度、嵌套深度和字段载荷设置上限。它是运行时基础编码，不等同于最终的场景、Prefab、
资产容器或网络协议；这些格式应在其上定义独立的文件头、资源引用和迁移策略。

`Engine/World` 的 `WorldSerializer` 在此基础编码之上增加 World Archive 文件头和 ECS
实体/组件记录。它负责归档层的完整性校验、组件 schema 绑定、EntityId 临时重映射和恢复
失败清理；Serialization 模块本身不依赖 ECS，也不感知 World 的生命周期。当前 World
Archive 支持读取不高于目标 Schema Version 的数据，但尚未提供通用 Schema migration
callback，因此场景资产层仍需定义显式的迁移策略。

## 边界

Serialization 可以使用 Foundation 和 Reflection，不应直接管理 RHI 资源或执行
渲染命令。资源引用应通过稳定资产 ID 或明确的资源句柄协议表达。
