# Module

## 目录职责

`Module` 负责静态 C++ Engine/Gameplay 模块的生命周期、依赖验证和注册产物所有权。
模块是 Pipeline、Feature、Phase、Reflection Type 等扩展定义的边界；它不替代
`PipelineRegistry`、`TypeRegistry` 或 `EngineFrameOrchestrator` 的内部职责。

## 当前实现

当前提供 `module.hpp` 中的 `Module`、`ModuleDescriptor`、`ModuleDependency`、
`ModuleRegistrationContext` 和 `ModuleRegistry`。模块注册过程具备事务语义：模块回调
失败时，已经注册的 Phase、Pipeline、Feature、Feature Binding 和 Reflection Type 会按
逆序回滚。

模块注销要求所有直接依赖模块先注销，并按 Feature Binding、Pipeline、Feature、Phase、
Reflection Type 的顺序释放模块拥有的注册项。`ModuleRegistry::list_modules()` 提供按稳定
ModuleId 排序的只读快照，Editor 可在模块加载/卸载后刷新模块状态。跨 DLL 的固定布局 C ABI
由 `abi/module_api.h` 定义，动态 Loader 在加载时必须完成版本、表大小和能力位协商。

## 使用边界

- 模块描述符的 `ModuleId` 和版本必须稳定，依赖必须声明最低兼容版本。
- Pipeline 和 Feature 的 `owner_module` 必须等于当前注册模块的 ID。
- 模块导出的 `TypeCatalog` 条目的 `owner_module` 也必须等于模块自身 ID；Editor 只消费通过
  该校验的元数据。
- 模块不得保存指向 Registry 内部容器的借用指针。
- 动态插件不能直接跨边界传递 STL、异常或 C++ 虚表对象；应使用架构文档定义的版本化
  C ABI 门面。
# Game module extensibility

游戏模块通过 `game_module.hpp` 提供统一的 `ModuleDefinition`。静态游戏目标调用
`register_static_module()`，动态模块通过 `module_api.h` 的固定宽度 C ABI 提供同一份
类型目录和构建身份。Editor 只读取 `ModuleMetadata`，不依赖 DLL 内的 C++ 对象或虚表。

Pipeline、Feature、RenderPass 的语义由 Pipeline 公共基类和 `TypeCatalog` 描述；编辑器
使用元数据绘制节点和参数，运行时再由经过校验的 Registry 实例化实际对象。CompositeFeature
目前是文档和注册审查中的组合约定，不在代码层增加额外 trait 或 compile-time 限制。

外部工程应通过 `EngineTools.cmake` 调用：

```cmake
include("${ENGINE_DIR}/cmake/EngineTools.cmake")
file(GLOB_RECURSE GAME_SOURCES Source/*.cpp Source/*.hpp)
aeg_add_game_executable(MyGameExecutable SOURCES ${GAME_SOURCES})
```

## Editor metadata boundary

通用 Editor 不扫描或编译游戏工程源码来推断节点类型。游戏模块在加载时发布经过版本协商的 `ModuleMetadata` 与 `TypeCatalog`；Editor 以稳定 `TypeId`、类别、版本、owner 和参数/端口 schema 建立可视化节点目录。`.aegpipe` 只引用这些稳定身份，同名显示标题不会产生二义性，运行时再通过模块 Registry 实例化对象。

## Immutable module bundles

模块 staging 目录包含 DLL、`module.metadata.json` 和 `module.manifest.json`。manifest 只保存相对路径、Build ID、target/configuration、contract digest 以及 metadata/DLL 的 SHA-256；`validate_module_bundle()` 会先拒绝绝对路径和目录穿越，再核对所有身份与摘要。

发布前可使用 `aeg_module_metadata_tool` 将元数据规范化并生成 manifest；读取旧的 metadata-only 开发 fixture 仍受支持，但不提供文件完整性声明。

动态 Loader 的宿主借用通过 `DynamicModuleLoader::acquire()` 返回的
`DynamicModuleLease` 表示。卸载会先进入 retiring 状态：若仍有 lease，立即卸载或等待超时
都会返回 `kTimeout` 并保持 DLL 加载；只有所有回调、Plan 和实例释放后才关闭 native module。

## Checked contexts and values

`private/module_context_table.*` 是 ABI 适配器共用的句柄表。`AegAbiContext` 只包含 owner
cookie、slot、generation 和 kind；宿主在每次调用前检查四项，释放后的句柄不会因为 slot
复用而重新获得访问权。`ModuleContextLease` 用 RAII 保持一个上下文在异步回调期间有效。

`AegAbiValue` 使用显式的固定宽度 kind，字符串和数组通过借用视图传递，接收方必须在当前
ABI 调用中复制内容。`validate_abi_value()` 会拒绝未知 kind、非零保留位、空指针加非零长度
以及超过协议上限的字符串和数组；这套约束供后续 Pipeline、Render 和 Reflection 表复用。

`AegAbiModuleApi` 的表指针位于稳定前缀之后，当前可选表分别覆盖 metadata、registration、
Pipeline/Feature declaration、task、RenderGraph、Pass setup/command 和 value reflection。
每张表都以 `struct_size`、`version` 开始，旧宿主可以只读取自己认识的前缀，模块也不能把
未协商的可选表当成已实现能力。

`DynamicModuleHandle::api` 保存本次加载成功协商的 API 快照，供 Editor/诊断层读取能力位；
`validate_module_api()` 会同时检查能力位对应的表指针，避免 DLL 只声明能力却没有可调用
实现。标准导出宏目前只提供握手，能力位保持为零，实际模块应显式提供并填充对应版本化表。
