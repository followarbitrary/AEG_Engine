# Resource

## 目录职责

`Resource` 提供引擎资源的运行时表示和 GPU 生命周期辅助，包括 Mesh、Texture、
Material、Offscreen Render Target、Buffer/Texture 上传以及延迟销毁。

## 当前实现

- `public`：CPU 数据、GPU 句柄组合、资源描述和上传接口。
- `private`：资源校验、RHI 对象创建、RenderGraph 上传 Pass 和销毁实现。

## 依赖边界

Resource 可以依赖 RHI 和 RenderGraph，但不应依赖具体后端实现。资源对象的所有权、
上传完成条件和销毁时机必须显式表达，不能通过全局缓存或隐式设备同步掩盖生命周期。
