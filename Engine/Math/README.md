# Math

## 目录职责

`Math` 提供图形学和引擎运行时使用的数学基础，包括向量、矩阵、四元数、变换、
标量运算以及可选的 SIMD 加速路径。

## 当前实现

- `public/math.hpp`：面向模块使用者的数学接口和语义定义。
- `private/math.cpp`：数学实现与编译配置适配。
- `private/simd`：平台相关 SIMD 内部实现，不属于公共 ABI。

## 依赖边界

Math 不参与 Pipeline 拓扑，不持有 RHI 资源，也不依赖 RenderGraph。SIMD 优化必须
保持与标量实现一致的数值语义、布局约束和边界行为。
