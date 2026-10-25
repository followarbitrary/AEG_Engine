# Foundation

## 目录职责

`Foundation` 是引擎最低层的通用基础模块，提供结果类型、错误码、通用值语义和
跨模块共享的稳定身份契约；这些定义不依赖 Asset 或其他领域模块的实现。

## 当前实现

- `public/result.hpp`：`Result` 与 `ResultValue`。
- `public/asset_identity.hpp`：位于 `aeg::foundation` 的 UUID、AssetId 和 AssetKind 身份值。
- `public/parameter_value.hpp`：Pipeline、Asset、Shader 共用的中立参数值与参数块。
- `private/result.cpp`：结果类型的非内联实现。
- `invalid_argument()`、`invalid_state()`、`unsupported()`、`not_found()`、
  `not_ready()`、`out_of_memory()` 和 `unknown_exception()`：跨模块共享的标准错误结果构造函数。
- `ResultCode::kCancelled`：用于任务、批次和帧级协作式取消的稳定错误分类。

## 依赖边界

该模块不得依赖 RHI、RenderGraph、Pipeline、Asset、平台窗口系统或游戏领域模块。其他
模块可以依赖 Foundation，但 Foundation 不应知道调用者属于哪个执行域。
