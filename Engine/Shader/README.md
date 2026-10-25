# Shader

## 目录职责

`Shader` 负责运行时 Shader Asset、HLSL 编译产物、Shader Reflection、Shader Contract、
Pipeline Layout 缓存以及由 Contract 驱动的 Pipeline 创建辅助。

## 当前实现

- `public/shader_asset.hpp`：Shader 二进制资产和阶段信息。
- `public/shader_contract.hpp`：Reflection 结果、绑定契约和布局描述。
- `private/shader_manager.cpp`：Shader 管理和运行时实现。

`ShaderManager` 为 layout/Shader/PSO cache 发布单调 `cache_epoch`。每个
`ShaderPipelineLayout` 记录创建 epoch；清理 layout cache 会推进 epoch，旧 layout
在 `is_valid()`、binding lookup 和 pipeline creation 前 fail-closed。宿主发布该 epoch
到 Renderer profile/material contracts，避免重建或替换 cache 后复用旧 layout/variant。
如果旧 layout 仍被 BindingSet 或 Pipeline 引用，清理失败的 handle 会进入 pending
retirement 并从 active cache 逐出；依赖释放后再次 clear 才完成销毁，新 epoch 不会重新
包装旧 handle。

`ShaderPipelineLayout` 不保存可在 ShaderManager 析构后解引用的 typed owner。其 owner、
epoch、contract hash 和 cached layout storage 通过 `private/shader_pipeline_layout_access.hpp`
收口；ShaderManager 持有的 lifetime token 过期后，仍存活的 layout value 的 `is_valid()`
安全返回 false。

## 依赖边界

Shader 可以依赖后端无关的 RHI 类型，但不应把 Vulkan/SPIR-V 或 D3D12 专有对象暴露
到公共 Contract。Material/Shader 参数值使用 Foundation 的中立 `ParameterBlock`，不反向
依赖 Pipeline authoring。离线编译和代码生成由根目录构建脚本及 `Tooling` 负责。
