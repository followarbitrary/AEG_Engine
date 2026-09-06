# RHI Vulkan Backend

## 目录职责

该目录是 Vulkan 后端的隔离实现区，包括 Vulkan Device、工厂、VMA 集成、调试工具和
Vulkan 专有配置。

## 约束

这里的代码可以直接使用 Vulkan SDK、VMA 和 Vulkan 原生句柄，但不得被上层模块直接
包含。任何需要进入通用 RHI 的能力，都必须先映射为后端无关的公共语义。

## 扩展方式

未来新增 D3D12 或其他后端时，应在 `RHI/private` 下建立独立实现目录，并复用公共
RHI 工厂契约，而不是把多个后端代码混入本目录。
