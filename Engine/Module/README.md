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
Reflection Type 的顺序释放模块拥有的注册项。`ModuleRegistry` 当前面向同一 C++ 编译
配置下的静态模块；跨 DLL 的固定布局 C ABI 和动态 Loader 仍属于后续 ABI 层工作。

## 使用边界

- 模块描述符的 `ModuleId` 和版本必须稳定，依赖必须声明最低兼容版本。
- Pipeline 和 Feature 的 `owner_module` 必须等于当前注册模块的 ID。
- 模块不得保存指向 Registry 内部容器的借用指针。
- 动态插件不能直接跨边界传递 STL、异常或 C++ 虚表对象；应使用架构文档定义的版本化
  C ABI 门面。
