# Shader 命名与目录规范

本文档定义引擎 Shader 源码、组件目录和构建产物的统一约定。所有新增
Shader 都必须遵循本规范。命名的目标是让开发者只通过文件名和路径就能
判断 Shader 的功能、所属组件以及构建类型。

## 1. 文件名

Shader 源文件使用以下格式：

```text
<functional_name>.<stage>.hlsl
```

`functional_name` 必须满足以下规则：

- 使用全小写 `snake_case`。
- 以小写字母开头，只使用小写字母、数字和下划线。
- 描述 Shader 的实际渲染或计算职责，不使用无意义的 `shader`、`source`
  或 `program` 后缀。
- 不包含 `vulkan`、`d3d12` 等后端名称。后端差异由 RHI 和 Shader 编译
  配置处理，不能进入公共 Shader 名称。
- 同一个 Shader Program 的所有阶段必须使用完全相同的
  `functional_name`。

推荐使用的阶段后缀如下：

| 后缀 | 阶段 |
| --- | --- |
| `.vert.hlsl` | Vertex Shader |
| `.frag.hlsl` | Fragment/Pixel Shader |
| `.comp.hlsl` | Compute Shader |
| `.geom.hlsl` | Geometry Shader |
| `.tesc.hlsl` | Tessellation Control/Hull Shader |
| `.tese.hlsl` | Tessellation Evaluation/Domain Shader |
| `.mesh.hlsl` | Mesh Shader |

例如，使用全屏三角形将纹理复制到目标的两个阶段应命名为：

```text
fullscreen_texture_blit.vert.hlsl
fullscreen_texture_blit.frag.hlsl
```

不要使用 `offscreen_composite` 作为这组 Shader 的名称，因为该实现执行
的是全屏纹理拷贝，并没有表达多输入合成或混合操作。

## 2. 目录结构

目录名同样使用全小写 `snake_case`。同一框架组件或功能域的 Shader 必须
放在独立目录中：

```text
shaders/
    common/                      # 跨组件复用的 HLSL include
    core/                        # 引擎内置 Shader
        <component>/
            <functional_name>.<stage>.hlsl
    gameplay/                    # 游戏或运行时 Shader
        <feature>/
            <functional_name>.<stage>.hlsl
```

当前测试程序中的 Gameplay Shader 使用等价的测试目录：

```text
tests/shaders/
    <feature>/
        <functional_name>.<stage>.hlsl
```

当前示例目录如下：

```text
shaders/core/presentation/
    fullscreen_texture_blit.vert.hlsl
    fullscreen_texture_blit.frag.hlsl

tests/shaders/triangle/
    textured_triangle.vert.hlsl
    textured_triangle.frag.hlsl
```

组件目录应表达框架职责，例如 `presentation`、`post_process`、`lighting`
和 `ui`。功能名应表达具体 Shader Program，例如
`fullscreen_texture_blit`、`tonemap` 或 `textured_triangle`。

## 3. Common Shader Include

`common` 只用于跨组件复用的 HLSL include 文件，例如类型、数学
辅助函数和颜色空间转换函数。Common 文件必须满足以下规则：

- 不包含 Vertex、Fragment 或 Compute 的入口点。
- 文件名使用全小写 `snake_case`，并且不使用受支持的阶段后缀。
- 不依赖某个具体组件的资源绑定布局。
- 不使用 Vulkan、D3D12 或其他后端专属语法和名称。
- 组件 Shader 通过 include 使用 Common 内容，不复制粘贴实现。

如果一段代码只被一个组件使用，应放在该组件目录内，而不是放入
`common`。

## 4. Core 与 Gameplay Shader

Core Shader 是引擎底座的一部分，在构建时编译为 SPIR-V，并生成轻量的
`.hpp + .cpp` 接口和数据文件：

```text
shaders/public/generated/<program>_shaders.hpp
shaders/private/generated/<program>_shaders.cpp
```

Gameplay Shader 通过 Shader Contract 构建，编译后的 SPIR-V 经 Reflection
生成 `.aegshader` 文件。Gameplay 代码应通过 `ShaderManager` 加载 Contract，
由 Reflection 生成 Binding Layout 和 Pipeline Layout，不直接依赖某个后端
的 Shader 字段。

Core Shader 的生成接口使用统一的 `aeg::shaders` 命名空间。组件目录会参与
程序名和符号名的生成，因此移动组件目录会改变生成接口，调用方必须同步
更新 include 和符号引用。

## 5. 审查清单

新增或移动 Shader 时，确认以下事项：

1. 文件名是否准确描述功能，并符合 `<name>.<stage>.hlsl` 格式。
2. 同一 Program 的各阶段是否使用相同的功能前缀。
3. Shader 是否位于正确的 Core、Gameplay 或组件目录。
4. 可跨组件复用的代码是否位于 `common`。
5. 名称中是否意外包含后端、平台或编译器专属信息。
6. 移动 Core Shader 后，是否同步更新生成接口的 include 和符号引用。
