# RHI

## 目录职责

`RHI` 是 AEG 的 Rendering Hardware Interface，定义后端无关的 Buffer、Texture、View、
Binding、Pipeline、Command、Queue 和 GPU Sync Point 抽象。

## 目录分层

- `public`：公共 RHI 类型、描述符、句柄、配置和 Device 接口；不得包含后端字段。
- `private`：RHI 工厂及后端实现的内部组织。
- `private/vulkan`：Vulkan 后端实现、VMA 集成和 Vulkan 专有配置。

## 依赖边界

上层模块只能依赖 `public`。新增 D3D12 或其他后端时，应在 `private` 下增加独立
实现，并通过工厂接入，不得修改通用接口以暴露后端对象或扩展名。
