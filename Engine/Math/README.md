# Math

## 目录职责

`Math` 提供图形学和引擎运行时使用的数学基础，包括向量、矩阵、四元数、变换、
投影约定、视锥体几何、标量运算以及可选的 SIMD 加速路径。

## 当前实现

- `public/math.hpp`：面向模块使用者的数学接口和语义定义，包括 `Plane`、`Frustum`
  和 `try_extract_frustum()`。
- `private/math.cpp`：数学实现与编译配置适配。
- `private/simd`：平台相关 SIMD 内部实现，不属于公共 ABI。

## 依赖边界

Math 不参与 Pipeline 拓扑，不持有 RHI 资源，也不依赖 Renderer、RenderGraph 或
Pipeline。Frustum 是通用几何类型；Renderer 负责提供相机矩阵和 Clip Space Convention，
Math 负责平面提取和包围体测试。查询前必须保证 Frustum 有效；查询接口针对重复
包围体测试进行了热路径设计。SIMD 优化必须保持与标量实现一致的数值语义、布局约束
和边界行为。

所有通用数学类型、算法和 SIMD 实现必须位于 `Engine/Math`，并通过 `aeg_math`
目标提供。Renderer、Runtime、EditorFramework 等上层模块只消费该公共接口；编辑器
中的射线拾取、Gizmo 和布局计算属于各自的功能逻辑，不能在这些目录重新定义数学类型。
在启用 `AEG_MATH_ENABLE_SIMD` 且目标平台支持 SSE2 时，向量算术、点积、叉积、长度平方、
矩阵乘法和四元数基础运算使用 `private/simd` 路径；不满足条件时使用等价的标量实现。
