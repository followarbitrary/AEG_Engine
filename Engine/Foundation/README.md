# Foundation

## 目录职责

`Foundation` 是引擎最低层的通用基础模块，提供结果类型、错误码、通用值语义和
其他不带领域含义的基础定义。

## 当前实现

- `public/result.hpp`：`Result` 与 `ResultValue`。
- `private/result.cpp`：结果类型的非内联实现。
- `ResultCode::kCancelled`：用于任务、批次和帧级协作式取消的稳定错误分类。

## 依赖边界

该模块不得依赖 RHI、RenderGraph、Pipeline、平台窗口系统或游戏领域模块。其他
模块可以依赖 Foundation，但 Foundation 不应知道调用者属于哪个执行域。
