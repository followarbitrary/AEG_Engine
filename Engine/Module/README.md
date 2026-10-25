# Module

## 目录职责

`Module` 负责静态 C++ Engine/Gameplay 模块的生命周期、依赖验证和注册产物所有权。
模块是 `FeatureDefinition`、`FeatureCatalog`、反射类型以及相关模块元数据的边界；五域
（Logic、Physics、Render、Audio、Stream）是 Feature 契约的分类，不是模块层级。模块不替代
`FeatureRegistry`、`TypeRegistry` 或 `EngineFrameOrchestrator` 的内部职责；Frame/Render
执行所需的 Pipeline、Stage 和 RenderGraph Pass 只能作为内部投影存在。

## 当前实现

当前提供 `module.hpp` 中的 `Module`、`ModuleDescriptor`、`ModuleDependency`、
`ModuleRegistrationContext` 和 `ModuleRegistry`。模块注册过程具备事务语义：模块回调
失败时，已经注册的 FeatureDefinition 和反射类型由同一清理实现回滚。
ModuleRegistrationContext 只提供 Feature 和反射注册；旧 Phase、Pipeline、Binding 和
RenderPass 注册及其 registry getter 已删除。ModuleRegistry 必须接收 FeatureRegistry，
可以接收反射 registry，没有旧构造兼容入口。

模块注销要求所有直接依赖模块先注销，再调用模块的 unregister 生命周期并清理其定义。
Feature 清理按逆序记录尝试，遇到 Composite 依赖时重复移除可删除项；随后逆序清理
Reflection Type。启动回滚和正常卸载共享同一清理实现。`ModuleRegistry::list_modules()` 提供按稳定 ModuleId
排序的只读快照，Editor 可在模块加载/卸载后刷新模块状态。跨 DLL 的固定布局 C ABI 由
`abi/module_api.h` 定义，动态 Loader 在加载时必须完成版本、表大小和能力位协商。

## 使用边界

- 模块描述符的 `ModuleId` 和版本必须稳定，依赖必须声明最低兼容版本。
- `FeatureDefinition` 及其 `FeatureCatalog` 条目的 `owner_module` 必须等于当前注册模块的 ID。
  Pipeline、Stage、RenderPass 等名称只描述 Frame/Render 的内部执行投影，不进入新的
  Feature 作者目录。
- `AEG_REGISTER_GAME_FEATURE` 会在进入 Catalog 前绑定当前模块 ID；手工指定其他模块或在
  注册阶段篡改归属都会失败，metadata 解码也会拒绝跨模块 Feature 条目。
- Editor 只消费通过 owner 校验的模块元数据和 `FeatureCatalog` 条目。
- 模块不得保存指向 Registry 内部容器的借用指针。
- 动态插件不能直接跨边界传递 STL、异常或 C++ 虚表对象；应使用架构文档定义的版本化
  C ABI 门面。
# Game module extensibility

游戏模块通过 `feature_module.hpp` 提供 Feature-only 的 `FeatureModuleDefinition`。
静态游戏目标调用 `register_static_module()`。游戏代码通过
`add_feature_definition()`（或 `AEG_REGISTER_GAME_FEATURE`）发布五域 Feature 合约和
实现；ModuleRegistrationContext 同样只接收 FeatureDefinition 和反射注册。
动态模块的 `module_api.h` 使用固定宽度 C ABI；标准导出宏从 DLL 内实际 Feature 工厂
提供 metadata 和版本化执行表，Loader 校验 bundle、合同和上下文后装配宿主 Feature 回调。
DLL 加载、协议协商与实际 Feature invocation 是各自独立的边界。
`AEG::GameSDK` 只传播 Foundation、Feature 和 Module 作者头文件；内部库作为 link-only
实现依赖保留。`AEG::GameRuntime` 使用轻量 `game_runtime_bootstrap.hpp`，不传播 Pipeline 或
Concurrency 的编译使用要求。游戏作者不需要包含 Concurrency，也不受引擎内部的线程实现规则约束。
Editor 只读取 `ModuleMetadata`，不依赖 DLL 内的 C++ 对象或虚表。
ModuleMetadata 只携带 ModuleDescriptor 与 FeatureCatalog；旧 catalog、descriptor 别名和
aeg_module 对 aeg_pipeline 的公开依赖已经删除。元数据 schema、ABI 和 SDK major 必须为 2，
拒绝旧 catalog、缺少正式头部和其他 schema。Feature 契约 JSON 由 Feature 编解码，
Module 只负责描述符、容器、模块所有权和 SHA-256。

`GameModule.metadata.json` 仍是构建身份和发布包的显式输入，不能由静态 C++ 定义安全地
反射出完整端口/副作用 schema。Feature catalog 的 canonical 来源是模块中的
`FeatureModuleDefinition`；metadata 工具在当前版本只校验并封装外部 sidecar，因此 CI
应把 sidecar 与 Feature 定义一起校验，避免重复条目漂移；静态 Runtime 会把模块定义重新
计算的 contract digest 与 package manifest 比对，sidecar 漂移会在启动前失败。后续 ABI metadata callback
可以把 catalog 导出接到同一条链路，但在 callback 表完成实现前不宣称已自动生成。

Feature 的语义由 `FeatureDefinition` 的五域契约和 `FeatureCatalog` 描述；编辑器使用元数据
绘制 Feature 节点、参数和端口，运行时再由经过校验的 Registry 实例化 Feature。Atomic/
Composite、Call/Process/Continuous 和 StatePolicy 是彼此正交的契约维度；Composite 的域集合
必须由子 Feature 的叶子域递归得到。Render Feature 可以投影为 RenderGraph Pass，但该 Pass
不成为通用作者类型；Frame/Render Pipeline 也不拥有 Feature 的业务状态。

外部工程应通过 `EngineTools.cmake` 调用：

```cmake
include("${ENGINE_DIR}/cmake/EngineTools.cmake")
file(GLOB_RECURSE GAME_SOURCES Source/*.cpp Source/*.hpp)
aeg_add_game_executable(MyGameExecutable SOURCES ${GAME_SOURCES})
```

## Editor metadata boundary

通用 Editor 不扫描或编译游戏工程源码来推断节点类型。游戏模块在加载时发布经过版本协商的 `ModuleMetadata` 与 `FeatureCatalog`；Editor 以稳定 `FeatureDefinitionId`、五域、版本、owner 和类型化参数/端口 schema 建立可视化节点目录。`.aegfeature` 只引用这些稳定身份，同名显示标题不会产生二义性，运行时再通过模块 Registry 实例化对象。

## Immutable module bundles

模块 staging 目录包含 DLL、`module.metadata.json` 和 `module.manifest.json`。manifest 只保存相对路径、Build ID、target/configuration、contract digest 以及 metadata/DLL 的 SHA-256；`validate_module_bundle()` 会先拒绝绝对路径和目录穿越，再核对所有身份与摘要。

发布前可使用 `aeg_module_metadata_tool` 将元数据规范化并生成 manifest。源 sidecar 通过
`read_module_metadata()` 读取；它不是可加载 bundle。`validate_module_bundle()` 和
DynamicModuleLoader 都要求正式 manifest，Loader 在加载前重新校验，不接受旧 metadata-only
或无 DLL 的开发兼容路径。标准 `AEG_EXPORT_GAME_MODULE_API(factory)` 从 DLL 内实际
Feature 工厂自动提供版本化执行表；C++ 工厂、STL 对象和异常不跨越 DLL 边界。

动态 Loader 的宿主借用通过 `DynamicModuleLoader::acquire()` 返回的
`DynamicModuleLease` 表示。卸载会先进入 retiring 状态：若仍有 lease，立即卸载或等待超时
都会返回 `kTimeout` 并保持 DLL 加载；只有所有回调、Plan 和实例释放后才关闭 native module。

## Checked contexts and values

`private/module_context_table.*` 是 ABI 适配器共用的句柄表。`AegAbiContext` 只包含 owner
cookie、slot、generation 和 kind；宿主在每次调用前检查四项，释放后的句柄不会因为 slot
复用而重新获得访问权。`ModuleContextLease` 用 RAII 保持一个上下文在异步回调期间有效。

`AegAbiValue` 使用显式的固定宽度 kind，字符串和数组通过借用视图传递，接收方必须在当前
ABI 调用中复制内容。`validate_abi_value()` 会拒绝未知 kind、非零保留位、空指针加非零长度
以及超过协议上限的字符串和数组；这套约束供 Feature 声明、RenderGraph 投影和 Reflection
表复用。

`AegAbiModuleApi` 的表指针位于固定前缀之后，当前可选表包含 metadata、Feature execution
和 value reflection。原未实现的 registration/FeatureGraph/Process 占位表已删除。作者
RenderGraph、Pass setup/command 表以及旧
RenderProgram provider 头文件已删除；Render Feature 通过中立 RenderIntent 表达工作，
引擎拥有 RHI 资源和 RenderGraph 投影。每张表以 `struct_size`、`version` 开始，接收方只能
读取已协商的前缀；Feature execution v2 要求完整固定表及全部通用回调槽，v1 被拒绝。
`AegFeatureInvocation.version`、执行表 version 和私有 callback wire version 均使用 2。

`AEG_CAP_ALL` 是唯一的正式能力位集合；`validate_module_api()` 同时受该集合和宿主支持
集合约束。旧 Pipeline、作者 RenderGraph/Pass 位与其他未知位均拒绝，即使调用方的支持
掩码包含它们。引擎内部 RenderGraph/RHI 的图与命令接口不进入这个 Module 作者 ABI。

`DynamicModuleHandle::api` 保存本次加载成功协商的 API 快照，供 Editor/诊断层读取能力位；
`validate_module_api()` 会同时检查能力位对应的表指针，避免 DLL 只声明能力却没有可调用
实现。标准导出宏发布 `AEG_CAP_FEATURES`；逻辑域复用同一 Feature 执行路径，
旧独立 logic 位已退休。测试用引擎 fallback 入口仍只提供零能力握手。


## Dynamic Feature callbacks

`DynamicModuleLoader::make_feature_module()` 复制 DLL 实际工厂导出的完整 metadata，
按 Feature ID 与正式 sidecar 对齐，用现有 `module_contract_sha256()` 比较完整 descriptor、
契约、所有条目及 schema/ABI/SDK 版本。条目显示顺序不改变身份。静态注册、动态导出和
宿主导入共用 `validate_feature_module()`，注册仍使用既有 ModuleRegistry。

C invocation 固定宽度 token 和只读 state/input/parameter 视图仅在调用期间有效。响应
经宿主 ByteSink 深拷贝。`private/feature_callback_codec.cpp` 保留 AFR1 magic，wire version
使用 2，并与 `AEG_FEATURE_EXECUTION_API_VERSION` 编译期核对；旧 version-1 响应被拒绝。
同一字段遍历和 Serialization primitives 编解码全部 StateDelta、九种 RenderOperation、
authored message 以及有序 `outbox` 数组。每条 OutboxIntent 保留 effect ID、version 和独立
payload；effect 必须有效，version 必须非零。

codec 复用 Feature 公共 `kMaxOutboxIntentsPerCommit=1024` 和 `kMaxOutboxIntentBytes=1MiB`，
双向检查数量和单条字节上限，解码在预留 author-supplied 数量前检查剩余字节。完整 callback
响应仍有 16 MiB 上限，包含协议头、其他 Delta 字段和 outbox 编码；它与 Runtime 的 16 MiB
pending payload 预算独立。标记、版本、枚举、截断及尾随字节仍检查。codec 只检查 wire
结构，owner 的状态/端口/权限和 `kCommand + kOutbox` 副作用授权仍由 FeatureRuntime 验证。
canonical binding、delivery handle、receipt、source 和 Render evidence 不进入 authored wire。
错误 ABI 协商在初始化工厂前由共享前缀检查拒绝；所有实际 DLL endpoint 在进入
作者回调前使用原 ModuleContextTable 检查上下文。

宿主 execute 和 state-validator wrapper 自动共享 `DynamicModuleLease`。Registry、
调用副本和 Async work 保留 callable 即保留代码；最后一个副本释放后正常卸载。
Render 与 outbox 输出完成深拷贝后只含宿主值；canonical 命令来源和 receipt 由 Runtime
签发，不依赖原 DLL callback。GPU 生命周期继续由原 FrameContext 管理，不需要人为保留 DLL。
真实夹具覆盖两个 DLL 的 Async 回调换代、旧工作唯一租约阻止卸载、
晚到结果丢弃、正常 stop/join/unregister；并在 DLL 关闭后执行 untouched intent 的
红蓝像素和 frame drain。该换代证明不是整个 ModuleRegistry bundle 热替换。

FeatureRuntime 的 pending journal 与 State/Process/ResourceBus 同事务接受 outbox，
通过私有 canonical binding/handle 执行消费、拒绝与终态 receipt。pending 阻止 graph/定义
换代，shutdown 明确取消剩余命令；模块卸载不把已复制命令变成可重放 callback。
Module v2 负责把作者的命令值完整交给 Runtime；固定 `World.Transform` v1 生产 consumer
位于 Engine Runtime private，通过显式启动绑定消费，Module 不执行 World mutation。

Application 启动加载/Main 可执行 Module、Scene Apply/Inspect、完整非空
Module→Editor→Cook→Runtime→Render 门槛以及原 Task5/6/7 其余范围继续实施。

DynamicModuleLoader.load 对传入的已选 owning ModuleBundle 重新验证 manifest 后，
在打开 DLL 前比较实际二进制/metadata 路径、module ID、build/platform/config、两个
内容摘要与 contract digest。即使磁盘 manifest 已合法变成另一构建，也返回 InvalidState
并保留未加载 handle；显式重新选择新的正式 bundle 后才允许加载。此检查不依赖
Project 或 Tooling 类型，Editor/Runtime 复用同一 Module 加载边界。
