# Reflection

## 目录职责

该目录用于类型元数据和运行时反射，包括类型注册、字段描述、属性访问、枚举信息
以及供序列化、编辑器和工具链使用的元数据查询。

## 当前状态

当前提供 `reflection.hpp` 中的 `TypeId`、`FieldId`、`TypeDescriptor`、
`FieldDescriptor`、`FieldAccessor`、`TypeBuilder` 和线程安全的 `TypeRegistry`。
字段访问器支持 C++ 成员指针适配，可为编辑器属性面板和序列化系统提供受控的读写入口。

当前反射注册采用不可变 Descriptor 快照；`TypeRegistry::find_type()` 返回共享只读描述，
调用方不依赖注册表内部容器的地址。注册和注销均按稳定 `TypeId` 校验重复项，模块注销
可以据此回收其拥有的类型定义。

当前内置类型编码覆盖布尔值、固定宽度整数、浮点数、`std::string` 和已注册嵌套对象。
枚举编辑器、容器字段、构造/析构回调和动态 DLL ABI 适配仍属于后续扩展，不应由当前
接口推断出这些能力已经存在。

## 边界

Reflection 不负责对象生命周期管理，也不应直接依赖编辑器界面或具体序列化格式。
Shader Reflection 属于 `Shader` 模块的专有输入，不等同于通用 C++ 类型反射。
