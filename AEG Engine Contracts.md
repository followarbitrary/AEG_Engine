# AEG Engine 开发契约

> 文档性质：项目级目录、模块边界、命名和扩展约定
>
> 维护日期：2026-09-10
>
> 详细 API 语义以各模块 `public` 头文件为准；AEG Domain-Oriented Pipeline Architecture
> 文档负责说明完整的 Pipeline 架构。本文件只保留所有开发者都必须遵守的项目级规则。

## 1. 规范性措辞

- **必须**：不满足即违反项目契约。
- **不得**：明确禁止的实现方式。
- **应**：默认推荐方式；确有必要偏离时，应补充说明。
- **口头约定**：当前主要依靠开发者自觉遵守，未通过类型系统或运行时完整强制。

本文档描述的口头约定不是可忽略的建议。新增模块、目录、Pipeline、Feature 或后端时，
代码评审应以本文件为检查依据。

## 2. 项目目录职责

### 2.1 `Engine` 一级目录

| 目录            | 职责                                                                            |
|-----------------|---------------------------------------------------------------------------------|
| `Foundation`    | 最低层通用基础类型，统一 `Result`、错误码和跨模块错误构造函数                   |
| `Logging`       | 统一日志门面、日志级别、日志分类、输出配置和 Quill 适配                         |
| `Math`          | 向量、矩阵、四元数、变换、投影、视锥体和 SIMD 数学实现                          |
| `Concurrency`   | `jthread`、协程、Executor、Completion、取消、TaskGroup 和 TaskGraph             |
| `Reflection`    | 类型、字段元数据和受控成员访问，为序列化与编辑器提供反射能力                    |
| `Serialization` | 基于 Reflection 的通用二进制对象编码与解码                                      |
| `ECS`           | Entity、Component、World 可变状态、查询、命令缓冲和不可变快照                   |
| `Pipeline`      | Pipeline、Stage、Task、Feature、Artifact、Compiler 和 Scheduler                 |
| `Module`        | C++ 模块生命周期、依赖检查和注册产物所有权                                      |
| `World`         | ECS 与 World Scope Pipeline 的绑定、快照发布和 World Archive                    |
| `Scene`         | 稳定场景文档、Scene GUID、实体层级、组件载荷和 World 实例化                     |
| `RHI`           | 后端无关的 GPU 资源、View、Binding、Command、Queue 和同步抽象                   |
| `RenderGraph`   | 帧内资源图、Pass、资源生命周期、显式 Barrier 和队列同步                         |
| `Resource`      | Mesh、Texture、Material、Render Target、资源上传和延迟销毁                      |
| `Shader`        | Shader Asset、Reflection、Shader Contract、Pipeline Layout 和运行时加载         |
| `Renderer`      | RenderView、World-to-Render 提取、可见性、RenderSceneSnapshot 和 RenderPipeline |
| `Runtime`       | Engine 生命周期、Scope 注册、Pipeline 执行和 RenderGraph 帧编排                 |
| `Platform`      | 窗口、Surface、文件系统、动态库和操作系统适配；公共窗口契约为 Experimental      |
| `Diagnostics`   | 断言、验证、性能分析、崩溃上下文和运行时诊断；当前为预留目录                    |
| `Tooling`       | Shader、资产、代码生成和格式验证工具；当前为预留目录                            |

### 2.2 `Application` 一级目录

| 目录              | 职责                                                                               |
|-------------------|------------------------------------------------------------------------------------|
| `Editor`          | 编辑器应用入口、真实编辑器面板、编辑器数据桥接和应用侧帧编排                       |
| `EditorFramework` | 编辑器状态、工具窗口、选择集、属性面板、场景预览和编辑器扩展生命周期；Experimental |
| `Platform`        | 操作系统窗口、Surface 和平台事件的具体实现；公共语义来自 `Engine/Platform/public`  |

`Application/EditorFramework` 可以依赖 Engine 的公共接口，但 Engine 的任何模块不得依赖
Application 或编辑器业务数据。编辑器框架不得直接包含 Application/Platform 的 private
头文件，也不得直接调用 Win32、Vulkan、D3D12 或其他 native API。

`Engine/Platform/public` 只表达后端无关的窗口身份、状态、Surface 绑定和平台服务。窗口
位置、尺寸、显示、聚焦和关闭请求可以由编辑器多视口使用，但不得把 HWND、X11 Window 或
其他 native 类型加入 Engine 公共头文件。具体窗口实现必须位于
`Application/Platform/private/<platform>`。

### 2.3 `RHI` 后端目录

`Engine/RHI/private/vulkan` 只存放 Vulkan 实现、VMA 集成和 Vulkan 专用配置。未来新增
D3D12、Metal 或其他后端时，必须在 `private` 下建立独立后端目录，并通过通用 RHI 工厂
接入。后端目录不得修改通用公共接口来暴露 native API 类型。

### 2.4 其他特殊目录

| 目录              | 职责                                        |
|-------------------|---------------------------------------------|
| `ThirdParty`      | 第三方依赖的完整源码、许可证和必要构建文件  |
| `cmake`           | 通用构建函数、Shader 编译和代码生成规则     |
| `shaders/common`  | 跨组件复用且不包含入口点的 HLSL 公共代码    |
| `shaders/core`    | 引擎内置 Shader 源码                        |
| `shaders/public`  | 生成的 Shader `.hpp` 接口和公共 Shader 类型 |
| `shaders/private` | 生成的 Shader `.cpp` 数据定义和私有生成物   |
| `tests`           | smoke test、契约验证和最小可运行示例        |

## 3. Math 数学约定

本节定义跨模块共享的数学语义、数值行为和数据布局。详细 API 以
`Engine/Math/public/math.hpp` 为准；实现不得改变本节的可观察语义。

### 3.1 坐标系、角度和基础类型

- 世界空间和视图空间统一使用右手坐标系：`+X` 向右、`+Y` 向上，相机默认沿 `-Z`
  方向观察。
- 角度统一使用弧度；正旋转遵循右手定则。
- `Vec2`、`Vec3`、`Vec4` 的语义分量顺序分别为 `x, y`、`x, y, z` 和 `x, y, z, w`。
- `Quat` 的存储顺序为 `x, y, z, w`，默认值是单位旋转 `(0, 0, 0, 1)`；旋转 API
  使用弧度，要求使用单位四元数表达旋转。
- `Vec3` 为 16 字节对齐且占用 16 字节；第四个浮点数仅用于对齐，不是语义分量，
  不得按紧凑的三个浮点数序列化。

### 3.2 矩阵和变换

- `Mat4` 是由四个列向量组成的列主序 4x4 矩阵；`operator[]` 按列访问，`at(row, column)`
  是规范的元素访问方式。
- 向量是列向量，放在矩阵右侧相乘。变换链统一写作
  `clip = projection * view * model * local`，矩阵乘法从右向左组合。
- `trs(translation, rotation, scale)` 先缩放、再旋转、最后平移，等价于
  `translation(translation) * from_quat(rotation) * scale(scale)`。
- `Mat4{}` 是零矩阵；需要单位矩阵时必须调用 `identity()`。
- 点变换使用齐次 `w = 1` 并执行透视除法；向量变换使用 `w = 0`，不得受到平移影响。
- `Mat4` 直接上传 Shader 的前提是 Shader 使用相同的列主序约定；引擎不会隐式转置。

### 3.3 相机和 Clip Space

- `perspective()` 和 `orthographic()` 默认使用 `0..1` 的 NDC 深度范围。
- `near_plane` 和 `far_plane` 是正距离；可见的相机空间 Z 位于
  `[-far_plane, -near_plane]`。
- `ClipSpaceConvention::y_down` 使正的 NDC Y 轴向下。
- `ClipSpaceConvention::reversed_z` 将 Near 映射到 `1`、Far 映射到 `0`；默认约定将
  Near 映射到 `0`、Far 映射到 `1`。
- 公共 Math API 不承诺 OpenGL 风格的 `-1..1` NDC 深度范围。调用方不得仅凭后端习惯
  推断深度映射。
- `RenderCamera::view_projection()` 必须返回 `projection * view`；Renderer 传递相机
  矩阵时必须同时传递对应的 `ClipSpaceConvention`。

### 3.4 平面和视锥体

- 平面方程统一表示为 `n.x * x + n.y * y + n.z * z + offset`。
- Frustum 平面顺序固定为 `Left, Right, Bottom, Top, Near, Far`，对应
  `FrustumPlane::kLeft` 至 `FrustumPlane::kFar`。
- `try_extract_frustum()` 生成归一化平面；提取失败时必须保持输出参数不变。
- 平面测试的“内部”语义为方程值大于或等于零：
  `a * x + b * y + c * z + d >= 0`。
- `Frustum::contains_point()` 和 `intersects_sphere()` 的调用方必须先确保 Frustum
  有效且平面已归一化。其他模块不得复制平面提取公式。

### 3.5 失败、比较和数值规则

- `try_*` 函数失败时返回 `false`，并保持输出参数不变。
- 值返回包装函数失败时返回零向量或零矩阵；`normalize(Quat)`、`inverse(Quat)` 和
  轴角构造失败时返回单位四元数 `(0, 0, 0, 1)`。
- `operator==` 执行逐分量精确比较；有限浮点值的近似比较必须使用 `nearly_equal()`
  并显式选择合适的绝对容差。
- `kEpsilon` 是默认的数值阈值，不表示单位换算，也不得被当作世界尺度或时间步长。
- 非有限输入或数值奇异矩阵属于无效输入；不得静默映射为另一个有效数学对象。

### 3.6 SIMD 和 ABI

- SIMD 仅是 Math 的实现细节；公共 API 不暴露 SIMD 类型。
- SSE2 和标量回退路径必须保持相同的语义、布局约束和边界行为；允许存在轻微浮点
  舍入差异，但不得改变可观察的失败规则或坐标/矩阵约定。
- `Mat4` 的公共布局和列主序语义属于跨模块 ABI 契约；序列化、上传或跨模块传递时
  不得依据具体 SIMD 实现推断布局。

## 4. 新建目录契约

### 4.1 先确定目录所属层级

新目录必须先回答两个问题：

1. 它属于基础设施、领域服务、运行时编排、后端实现还是工具链？
2. 它依赖哪些已有模块，是否会形成反向依赖或循环依赖？

目录职责必须单一。若一个目录同时承担资源管理、调度、渲染策略和平台适配，应拆分为
不同模块，而不是继续增加内部开关。

### 4.2 `public` / `private` 分离

- 需要跨模块使用的类型和接口放入 `public`。
- 实现细节、缓存、后端对象、线程实现和临时工具放入 `private`。
- `public` 不能包含具体后端头文件、平台头文件或私有实现类型。
- 公共结构体只能暴露稳定的语义数据；不能为了省略封装而暴露内部容器、分配器、锁或
  native 指针。
- 新目录若形成稳定模块，应提供 README，说明目录职责、公共入口和依赖边界。
- 仅用于实现细节的深层目录不要求单独 README，但 `RHI` 后端等具有独立边界的目录必须
  有说明文档。

### 4.3 依赖方向

- 基础模块不得依赖上层领域模块。
- 上层模块只能通过公共接口使用下层模块，不得 include private 文件。
- `Engine` Scope 不得依赖 World、Frame 或 Editor 业务数据。
- World Scope 可以使用 Engine 能力；Frame Scope 只能读取 Engine 数据和已提交的 World
  Snapshot；Editor 是 Frame 上的插件式 Overlay。
- Renderer 不得直接修改 World；RenderGraph 不得承担通用业务 Pipeline 调度；RHI 不得
  依赖 Renderer 或游戏逻辑。

### 4.4 生命周期与所有权

- 新增对象必须明确由谁创建、谁持有、谁销毁以及何时可以销毁。
- `std::span`、`std::string_view`、引用、裸指针和 `void*` 默认均为借用，不转移所有权。
- 借用数据的有效期必须覆盖整个调用或回调执行期；若回调会延迟执行，必须使用
  `shared_ptr`、Frame Retention 或明确的 Owner 机制延长生命周期。
- GPU 资源在对应提交完成前不得销毁；优先使用 `FrameContext` 的延迟销毁接口。
- World Pipeline 必须绑定具体 `WorldHandle`，不得使用 Singleton 表示 World 状态。
- 任何跨 Scope 数据必须通过不可变 Snapshot、Artifact 或显式 Bridge 传递。

### 4.5 错误处理

- 可能失败的公共函数必须返回 `foundation::Result` 或 `ResultValue<T>`。
- 通用错误构造函数统一放在 `Foundation`，其他模块不得复制实现同语义的 helper。
- 调用方必须检查 `Result` 后才能读取 `ResultValue<T>::value`。
- `Result::message` 只用于诊断，不得被当作稳定机器接口；逻辑判断使用 `ResultCode`。
- 模块边界不得传播异常；实现应捕获异常并转换为统一错误码。
- 非法参数、非法状态、不支持能力、未找到对象和内存不足必须使用对应错误类别，不能
  全部返回 `kUnknown`。

## 5. 命名与格式约定

### 5.1 命名

本项目采用以下约定。函数命名是项目特例，使用 snake_case；其他命名遵循下表：

| 类别        | 约定                     | 示例                              |
|-------------|--------------------------|-----------------------------------|
| 文件        | 全小写，单词以下划线分隔 | `render_graph.hpp`                |
| 类/结构体   | UpperCamelCase           | `PipelineScheduler`               |
| 变量/参数   | snake_case               | `frame_number`                    |
| 私有成员    | snake_case，以下划线结尾 | `pipeline_id_`                    |
| 函数/方法   | snake_case               | `create_texture()`                |
| 常量/枚举值 | `k` + UpperCamelCase     | `kMaxBufferSize`                  |
| 宏          | 全大写，下划线分隔       | `AEG_RHI_ENABLE_DEBUG`            |
| 命名空间    | 全小写，下划线分隔       | `aeg::render_graph`               |
| 模板参数    | UpperCamelCase           | `template <typename BuilderType>` |

名称必须表达语义，不得使用无意义的 `Manager2`、`DataObject`、`Temp`、`Misc` 等名称。
Shader 名称另遵守 `shaders/SHADER_NAMING.md`。

### 5.2 头文件与 Include

- 优先使用 `#pragma once`。
- 头文件必须自洽：直接使用的类型应显式 include 对应声明，不依赖传递包含。
- 遵循项目现有 Include 顺序和 `.clang-format` 配置；不得为了个人偏好重排已有顺序。
- 第三方依赖必须来自 `ThirdParty`，不得依赖开发机上的本地安装副本。
- 公共头文件尽量减少传递依赖；可以使用前置声明时，不应无条件 include 完整实现头。

### 5.3 排版与注释

- 使用 4 个空格缩进，禁止 Tab。
- 代码格式以仓库 `.clang-format` 为准；提交前必须执行项目配置的格式化流程。
- 行宽遵循 `.clang-format`，不得手工引入明显超长行。
- 使用 K&R/Attach 大括号风格。
- 指针和引用符号紧贴类型：`const std::string& name`、`int* value`。
- 单参数构造函数必须使用 `explicit`，除非明确需要隐式转换。
- 不修改成员状态的方法必须标记 `const`；不抛异常的方法应标记 `noexcept`。
- 只添加必要注释：类、模块和复杂契约需要说明；普通显而易见的逐行代码不写注释。
- 函数注释只说明作用，不重复书写返回值和参数说明。

## 6. Pipeline 与 Feature 口头约定

- 所有具有明确执行时机、输入输出和依赖关系的功能，优先建模为 Pipeline。
- 基础服务，如 Math、Logging、Allocator、RHI Device 和资源缓存，不应伪装成业务 Pipeline。
- 一个具体 Feature 默认只属于一个主领域，应从 `LogicFeature`、`PhysicsFeature`、
  `RenderFeature`、`AudioFeature` 或 `StreamFeature` 之一继承。
- `PipelineFeature` 是公共类型擦除根；直接继承在 C++ 上合法，但属于不推荐用法，当前不
  通过类型系统或 Registry 强制禁止。
- 跨领域 Feature 使用 `CompositeFeature<Parts...>` 的成员组合，不使用多重继承绕过领域
  Builder 边界。Part 不作为独立顶层 Feature 注册，也不得嵌套 Composite。
- Feature 只能通过所属领域 Builder 声明 Stage、Task、Artifact 和依赖，不得保存 Builder
  引用到构建回调之外。
- Pipeline 的 Definition、Instance 和 Plan 必须分离；一帧内使用的 Plan 必须保持固定。
- Pipeline 之间通过 Artifact 和显式顺序关系通信，不通过隐式全局调用链通信。
- 新增 Pipeline 可以注册自定义 Stage 和 FeaturePoint；新增全局 Phase 必须通过
  `PhaseRegistry` 相对已有 Phase 注册。

## 7. RHI、RenderGraph 与 Shader 约定

### 7.1 RHI

- `rhi.hpp`、`rhi_struct.hpp`、`rhi_config.hpp` 必须保持后端无关。
- Vulkan、D3D12、VMA 和平台窗口类型只能出现在 RHI private 后端或适配实现中。
- RHI Handle 只能交给创建它的 Device 使用和销毁；0 表示无效 Handle。
- Buffer/Texture 创建时必须声明整个生命周期需要的 Capability，不能依赖后续隐式补能力。
- Physical Device 选择必须综合评估可用性、能力、队列、Swapchain、内存和设备特征，不得
  只按“独立显卡”分类选择。
- GPU 同步必须通过 `GpuSyncPoint`、Queue Submission 和明确的等待关系表达。

### 7.2 RenderGraph

- RenderGraph 必须先声明资源和 Pass，再 Compile，最后 Execute。
- Pass Setup 负责声明资源访问；Pass Execute 负责录制命令，二者不能混用生命周期。
- RenderGraph 使用显式 Barrier；不得让后端通过隐式状态猜测替代图声明。
- 跨 Queue 的资源依赖必须同时表达资源状态、内存可见性和 Queue Transfer。
- Imported 资源由外部 Owner 管理，Graph 不负责销毁；Transient 资源由 Graph 管理其图内
  生命周期。
- Graph Handle 不能跨 Graph、跨 generation 或跨 reset 使用。

### 7.3 Shader

- Shader 使用原生、平台无关的 HLSL，不写入 Vulkan、D3D12 或平台专属语法。
- Shader 文件名使用 `<functional_name>.<stage>.hlsl`，功能名和目录使用小写 snake_case。
- 同一 Shader Program 的各阶段必须使用相同功能名前缀。
- 跨组件复用代码放在 `shaders/common`；组件专用代码放在对应组件目录。
- Core Shader 构建产物采用 `.hpp` 接口 + `.cpp` 数据，接口保持轻量。
- Gameplay Shader 通过统一 CMake 规则扫描、编译和生成 Contract，不为每个 Shader 复制一套
  编译命令。
- Shader Contract 必须先经过 Reflection/验证，再生成 Binding Layout 和 RHI Pipeline。

## 8. API 状态标记

公共模块、类型或接口在 README、头文件注释或架构文档中应标记其成熟度。未标记的公共
接口默认视为 `Experimental`，不得被游戏业务作为长期稳定依赖。

| 标记           | 含义                                                               |
|----------------|--------------------------------------------------------------------|
| `Stable`       | 已形成长期维护承诺；破坏性变更必须遵循兼容性规则并完成迁移说明     |
| `Experimental` | 可用但仍在演进；允许调整签名、数据模型和行为，调用方应隔离依赖范围 |
| `Reserved`     | 仅保留目录、名称或架构位置；不构成可调用 API，也不得产生运行时依赖 |
| `Deprecated`   | 仍保留以支持迁移；禁止新增依赖，必须在文档中给出替代接口和移除条件 |

状态标记用于表达演进承诺，不替代版本号。一个模块可以整体为 `Stable`，但其中新增的
独立 API 仍可标为 `Experimental`。

## 9. 线程安全标注规范

公共类型只要存在并发使用可能，就应在类注释、构造函数注释或 README 中明确其线程模型。
不允许要求调用方通过阅读 private 实现推断锁、线程亲和性或回调恢复线程。

| 标记                | 含义                                                           |
|---------------------|----------------------------------------------------------------|
| `Thread-safe`       | 多线程可并发调用公共方法；返回的借用对象仍须遵守自身有效期规则 |
| `Thread-compatible` | 不要求内部同步；不同实例可并发使用，同一实例须由调用方同步     |
| `Thread-confined`   | 对象固定服务于一个逻辑线程或执行域，不能跨线程调用或转移       |
| `Owner-thread only` | 只能由声明或绑定的 owner thread 调用，例如可变 `ecs::World`    |
| `Not-thread-safe`   | 不提供并发保证；所有并发访问必须由外部同步                     |

新增异步接口还必须说明：回调在哪个执行域运行、是否可能内联调用、取消语义、析构是否等待
任务收敛，以及回调捕获对象必须维持到何时。`Completion` 恢复协程时的线程亲和性不得被
隐式假设。

## 10. 资源与对象命名规则

名称用于诊断、缓存、序列化、图构建和资产定位时，必须具备稳定且可读的语义；不得用内存
地址、递增临时编号或后端对象名替代公开身份。

| 对象                                            | 命名规则                                                                                     |
|-------------------------------------------------|----------------------------------------------------------------------------------------------|
| 模块、Pipeline、Feature、Artifact、FeaturePoint | 使用命名空间式稳定身份，例如 `Engine.RenderPipeline`、`Game.WeatherState`                    |
| Stage、Task                                     | 使用功能性 UpperCamelCase 标识，例如 `RenderExtract`、`UploadTexture`                        |
| RenderGraph Resource                            | 使用表达内容语义的 UpperCamelCase，例如 `SceneColor`、`SceneDepth`、`ReflectionBuffer`       |
| RenderGraph Pass                                | 使用动作和用途组合，例如 `BuildHiZ`、`DeferredLighting`、`PresentComposite`                  |
| RHI DebugName                                   | 使用可定位对象类型和用途的字符串，例如 `Renderer.SceneColor`；不得包含 Vulkan/D3D12 专属名词 |
| ECS Component / Reflection Type                 | 使用稳定逻辑名称，例如 `Game.Transform`、`Game.PlayerController`                             |
| Scene Asset / Scene Entity                      | 使用稳定 GUID 作为身份；人类可读名称只用于显示，不承担唯一性                                 |
| Shader Program                                  | 遵循 `shaders/SHADER_NAMING.md`，文件名描述功能而非后端或编译器                              |
| 日志 Category                                   | 使用既有 `logging::Category`；新增类别前应确认无法归入现有分类                               |

名称变更若影响 `StableId`、Reflection TypeId、ArtifactId、Shader Contract、序列化字段或资产
定位，即属于兼容性变更，不能被当作普通重命名处理。

## 11. 测试契约

新增稳定模块或改变既有公共行为时，应新增与风险匹配的最小验证。测试应优先使用
`tests/<module>_smoke.cpp` 命名，并通过 CMake/CTest 注册。

| 变更类型                         | 最低测试要求                                             |
|----------------------------------|----------------------------------------------------------|
| 新公共模块或公共 API             | 正常路径 smoke test 与至少一个非法输入/错误路径测试      |
| 所有权、句柄或生命周期变化       | 创建、使用、释放/退休，以及失效后拒绝访问的测试          |
| 并发、异步或协程变化             | 取消、完成、析构收敛和并发访问的测试                     |
| Pipeline/Feature 变化            | 注册、绑定、编译、执行和权限/Scope 拒绝路径测试          |
| RHI/RenderGraph 变化             | 资源状态、提交、帧退休、Barrier 或 Queue Dependency 测试 |
| 序列化/Scene/Shader 文件格式变化 | 正常读写、截断/损坏输入、版本不兼容和确定性输出测试      |

测试必须验证可观察契约，而不是 private 实现细节。无法在当前平台自动测试的后端或窗口行为，
应保留最小 smoke 覆盖，并在模块 README 中说明人工验证前提。

## 12. 变更兼容性规则

以下改动属于兼容性变更，实施前必须评估调用方、持久化数据和插件边界，并同步更新文档、
版本和测试：

| 变更对象                                   | 最低处理要求                                                                                 |
|--------------------------------------------|----------------------------------------------------------------------------------------------|
| 公共函数签名、默认值或语义                 | `Stable` API 必须提供迁移路径或升级主版本；`Experimental` API 必须在变更说明中标明           |
| 公共结构体字段、布局、大小或对齐           | 评估 C++ ABI 与磁盘/网络格式；禁止静默改变持久化布局                                         |
| 公共枚举值和 Mask bit                      | 已发布值不得重编号、复用或改变语义；新增值只能追加或使用未占用 bit                           |
| `StableId`、GUID、TypeId、FieldId 生成规则 | 必须提供兼容读取或显式迁移；不得无说明改变散列输入或算法                                     |
| 序列化、Scene、World、Shader Package 格式  | 必须更新 Magic/Version 或定义兼容读取策略，并测试旧数据和损坏输入                            |
| Pipeline、Feature、Artifact 或 Scope 规则  | 必须更新架构文档、Feature/模块 README 和 conformance/smoke 测试                              |
| RHI 资源、Binding、同步或后端能力          | 必须保持后端无关公共语义；不支持的后端应返回 `kUnsupported`，不得静默降级                    |
| 文件或目录移动                             | 更新 CMake、生成规则、README、include 和所有公开路径引用；Core Shader 移动还必须更新生成符号 |

破坏性变更不得通过修改诊断文本、默认行为或隐式后端回退掩盖。`Deprecated` API 在移除前必须
给出替代路径，并完成项目内调用点迁移；`Reserved` 目录或类型改为可用 API 时，必须先明确
其状态、所有权、线程模型和最小测试覆盖。

## 13. 新模块/新目录提交前检查

1. 目录职责是否单一，名称是否能直接说明用途？
2. 是否明确依赖方向，并且没有形成循环依赖？
3. 公共声明是否只放在 `public`，实现和后端细节是否留在 `private`？
4. 是否提供了必要的 README，并说明职责、入口和边界？
5. 所有权、线程亲和性、回调有效期和销毁时机是否明确？
6. 是否复用了 Foundation 的 Result 和通用 helper？
7. 是否泄漏了 Vulkan、D3D12、平台或第三方专属类型？
8. 是否遵循命名、4 空格、Include 顺序、`.clang-format` 和必要注释约定？
9. 若新增公共枚举、结构体、文件格式或稳定 ID，是否同步说明版本兼容影响？
10. 若实现与架构文档不一致，是否先修正文档再继续扩展？

## 14. 文档维护

新增模块、公共 API、目录边界、Pipeline/Feature 规则或第三方依赖时，必须同步更新：

- 本文件中的目录职责或通用契约；
- 对应模块的 README；
- 需要时更新 `AEG Domain-Oriented Pipeline Architecture.md`；
- 需要时增加或调整 smoke test；
- Shader 变化同步更新 `shaders/SHADER_NAMING.md`。

本文件不记录实现排期，不替代 API Reference，也不把尚未实现的目标能力描述为当前能力。
