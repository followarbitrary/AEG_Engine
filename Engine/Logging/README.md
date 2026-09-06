# Logging

## 目录职责

`Logging` 提供引擎统一日志入口、日志级别、日志分类、输出配置和 Quill 适配层。
所有运行时模块应通过该模块记录日志，不应直接在业务代码中初始化 Quill 后端。

## 当前实现

- `public/logging.hpp`：公共日志配置和调用接口。
- `private/logging.cpp`：日志初始化、生命周期和输出实现。
- 第三方 Quill 源码位于仓库根目录的 `ThirdParty/Quill`。

## 依赖边界

Logging 可以依赖 Foundation 和 Quill，但不应依赖具体渲染后端或游戏模块。日志
级别和诊断开关由构建配置控制，调用方不应通过修改源码切换常用调试策略。
