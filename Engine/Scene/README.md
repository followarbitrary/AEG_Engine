# Scene

## 目录职责

`Scene` 负责编辑器与运行时共享的场景文档模型。它保存稳定的场景资产身份、实体层级、
编辑器标志和反射组件载荷，并在实例化时将文档实体映射为当前 `ecs::World` 中的运行时
`EntityId`。

## 公共入口

- `public/scene_asset.hpp`：`SceneAsset`、`SceneEntityId`、`SceneAssetId`、组件记录、
  `SceneInstance` 和 `SceneSerializer`。
- `SceneAsset::add_entity()`：加入一个稳定场景实体及其父子关系。
- `SceneAsset::set_entity_name()` / `set_entity_tag()`：更新编辑器可见的实体元数据。
- `SceneAsset::add_reflected_component()`：通过 Reflection Registry 将组件对象编码为
  场景组件载荷。
- `SceneAsset::remove_component()`：删除一个实体上的反射组件载荷。
- `SceneAsset::validate()`：检查身份唯一性、父节点存在性、层级环、组件归属和组件键重复。
- `SceneAsset::instantiate()`：创建运行时实体、恢复反射组件，并返回稳定身份到运行时
  `EntityId` 的映射。
- `SceneSerializer`：对场景文档执行确定性的版本化二进制编解码。

## 身份与生命周期

`SceneEntityId` 是跨保存、编辑和加载过程保持稳定的文档身份；`ecs::EntityId` 是某个
World 实例内由索引和 generation 构成的运行时身份。两者不得互换，也不得将运行时
`EntityId` 直接写入 Scene Asset 作为持久化主键。每次实例化都建立新的
`SceneInstance` 映射；World 被清空或销毁后，旧映射即失效。

场景资产本身不拥有 World，不保存 RHI 资源对象，也不管理编辑器选择状态。组件数据以
Reflection TypeId、Schema Version 和序列化载荷保存；资源引用应由反射字段中的稳定资产
身份表达，而不是写入后端句柄或内存地址。

实体记录同时保存稳定的 Name 和可选 Tag。当前 Scene archive writer 使用格式版本 2，
reader 兼容版本 1 并将旧记录的 Tag 置空。

## 层级与组件约束

每个实体的 `parent` 可以为空表示根节点，也可以指向同一资产中的另一个实体。父节点必须
存在，实体不得以自身为父，整个层级必须是有向无环图；当前实现按单父节点语义构成树或
森林。一个实体至多拥有一个相同 `ComponentTypeId` 的组件记录。

`SceneEntityFlags::kHidden` 表示编辑器或场景可见性策略要求隐藏，`kLocked` 表示编辑器
不应允许普通交互修改；这些标志只属于场景文档，不会自动改变 ECS 或 Renderer 行为。

## 序列化与实例化

`SceneSerializer` 使用独立于 `WorldSerializer` 的 Scene Asset 文件头，按稳定实体身份和
组件键排序后写出，因而同一文档的插入顺序不会影响归档字节序列。写入失败不会污染调用
方已有的 `BinaryWriter` 数据。读取过程受 `SceneSerializationOptions` 的名称、实体、
组件和载荷上限约束，并在返回前执行完整文档校验。

`SceneAsset::instantiate()` 默认清空目标 World 后创建实体；传入
`SceneInstantiationOptions{false}` 时采用增量实例化，仅回滚本次创建的实体。任一组件
schema 不匹配、反射类型缺失或载荷解码失败都会使实例化失败，并清理本次产生的部分状态。
当前不包含 Prefab 继承、资源异步加载、Schema Migration Callback 和稳定子资产引用解析，
这些能力应建立在 Scene Asset 模型之上继续扩展。

## 依赖边界

Scene 可以依赖 ECS、Reflection、Serialization 和 Foundation，不依赖 RenderGraph、RHI
后端、窗口系统或 Editor UI。编辑器应将 Scene Asset 作为文档模型，通过命令或事务修改它；
运行时应通过实例化结果和 World Snapshot 参与 Pipeline 数据流。
