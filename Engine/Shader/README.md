# Shader

## 目录职责

`Shader` 负责运行时 Shader Asset、HLSL 编译产物、Shader Reflection、Shader Contract、
Pipeline Layout 缓存以及由 Contract 驱动的 Pipeline 创建辅助。

## 当前实现

- `public/shader_asset.hpp`：Shader 二进制资产和阶段信息。
- `public/shader_contract.hpp`：Reflection 结果、绑定契约和布局描述。
- `private/shader_manager.cpp`：Shader 管理和运行时实现。

## 依赖边界

Shader 可以依赖后端无关的 RHI 类型，但不应把 Vulkan/SPIR-V 或 D3D12 专有对象暴露
到公共 Contract。离线编译和代码生成由根目录构建脚本及 `Tooling` 负责。
