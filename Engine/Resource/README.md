# Resource

## 目录职责

`Resource` 提供引擎资源的运行时表示和 GPU 生命周期辅助，包括 Mesh、Texture、
Material、Offscreen Render Target、Buffer/Texture 上传以及延迟销毁。

## 当前实现

- `public`：CPU 数据、GPU 句柄组合、资源描述和上传接口。
- `private`：资源校验、RHI 对象创建、RenderGraph 上传 Pass 和销毁实现。

`TextureMaterial` 将外部拥有的纹理 View 与 Resource 拥有的 Sampler、BindingSet 组合为可更新的材质资源。调用方负责保证纹理 View 在材质使用期间有效。
材质参数通过外部拥有的 Uniform Buffer 绑定到指定材质组；`update_texture_material_parameters()`
只更新 BindingSet，不接管 Buffer 生命周期，并且只适用于首次提交前的初始化。

### 跨帧材质版本

材质参与 GPU 提交后，不得原地修改仍可能被 GPU 读取的 BindingSet。纹理或参数更新应在
`FrameContext::begin_frame()` 与 `FrameContext::end_frame()` 之间调用
`update_texture_material_for_frame()` 或 `update_texture_material_parameters_for_frame()`。
这些接口创建新的持久 BindingSet 版本，成功后再切换 `TextureMaterial`，并通过
`FrameContext` 延迟释放旧版本；旧版本只有在对应帧槽的完成点通过后才会回收，不要求每帧
调用 `wait_idle()`。更新失败时不会改写当前材质版本。

`TextureMaterialParameters::tint` 必须是有限的 `[0, 1]` 值，上传的 Uniform Buffer
范围也必须完整覆盖一个参数块。RenderGraph 中使用的纹理和参数 Buffer 句柄必须在当前
图中重新声明，不能跨图复用旧 Handle。

## 依赖边界

Resource 可以依赖 RHI 和 RenderGraph，但不应依赖具体后端实现。资源对象的所有权、
上传完成条件和销毁时机必须显式表达，不能通过全局缓存或隐式设备同步掩盖生命周期。
