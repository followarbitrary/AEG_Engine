# Application Project

`Application/Project` 负责版本化 `Project.aegproj` 配置契约、项目根目录相对路径映射和项目脚手架。
它只依赖 Foundation 与 toml11，不拥有 FeatureGraph 作者文档编解码。
这些跨工具与编辑器共享的 codec 位于 `Application/Authoring`，避免把 Project 配置层误当成
Feature 文档层；Project 仍不依赖 Engine Runtime、World、RHI、Vulkan 或平台原生 API。

公共入口：`public/project_context.hpp`。

`ProjectPaths` 会拒绝绝对路径以及逃逸出配置目录的路径。更上层的项目会话负责资产数据库、
World、后台任务和 GPU 延迟销毁。

`ProjectManager` 接受项目文件或项目根目录，持有唯一已加载的 `ProjectContext`。它会在通知已注册的
`ProjectLifecycleParticipant` 前创建配置目录；若后续 participant 加载失败，则回滚已启动的
participant；卸载时按逆序释放。participant 采用显式共享所有权，避免项目会话关闭后保留借用的
Application 对象。

## Project files

新建工程使用 `Project.aegproj`，内容遵循明确的版本 2 `ProjectConfig` TOML schema。
缺省版本、版本 1、`AEG_PROJECT` header 和旧 `.aegproject` 扩展明确拒绝；没有自动
转换、旧文件 fallback 或 retained-source 重盖版本。显式现代项目文件保留实际路径，
现代配置也不能覆写既有旧格式或损坏目标而隐式转换；源诊断先于目标写入检查。
配置路径使用 UTF-8 编解码，TOML 经原生文件流读取并保留 UTF-8 来源名称；多个启动
roots 的顺序和 vendor 字段继续保留。工程根目录下的
`Assets/`、`Source/`、`Cache/`、`Saved/` 和 `Binaries/` 由项目路径配置解析，游戏源码通过
`EngineTools.cmake` 接入 Feature Contract/Definition 和 FeatureGraph。

## 游戏工程模板

新工程的最小目录如下：

```text
MyGameProject/
├── CMakeLists.txt
├── Project.aegproj
├── Assets/Features/Default.aegfeature
└── Source/
    ├── GameApp.cpp
    └── GameModule.cpp
```

`CMakeLists.txt` 通过 `EngineTools.cmake` 调用 `aeg_add_game_module()` 和
`aeg_add_game_executable()`。模块目标生成 Editor 用的 DLL、metadata/manifest bundle；
可执行目标只链接 Runtime 启动入口，并在 Cooked manifest 中加载同一份游戏模块 DLL。游戏代码只登记 Feature Contract/Definition，
并在 `FeatureModuleDefinition` 中登记稳定的 FeatureCatalog 摘要。Editor 只消费 metadata，不扫描或
编译游戏源码；`.aegfeature` 中的类型、实例、参数和端口身份由该目录校验并传给 Cooker 与
Runtime。Pipeline、Stage 和 RenderGraph Pass 只属于派生执行视图。

默认 FeatureGraph 含一个 Logic/Process 示例（Ready → Running → Done），并绑定模板
metadata/factory 的完整契约摘要；新建后可直接在 Flow Graph 中校验和预览。
模板的 Build、Binaries、Cache、Saved 和 .aeg 产物不会复制进新工程。

`ProjectCooker` 会把启动场景和 FeatureGraph 的 `.meta` 产品自动加入干净缓存，然后在
`Binaries/Shipping/<config>/versions/<publication>/` 写入完整 package、manifest 和 `module/`
bundle，最后原子替换根目录的 `current.toml` selector。发布启动只需要 EXE、运行库、selector
和所选版本目录，不需要 Editor、源码资产或引擎源码路径；失败 Cook 不会改动 last-good selector。
Cook 为启动 Scene/FeatureGraph 记录私有的源码与 `.meta` 内容指纹。保存源码或更改产品元数据后，
下次 Cook 会重新导入；无指纹的旧缓存也会重建。源码未改变时保留缓存，仍校验实际打包产物。

`aeg_feature_end_to_end_smoke` 从该脚手架创建非空 FeatureGraph 项目，沿真实 Editor
保存、Cook、动态 Runtime 启动和 RenderGraph GPU 像素路径验证交付连接。

`build.startup_feature_graphs` 保存有序的运行时 FeatureGraph roots。
`build.startup_pipelines` 和 `build.startup_frame_projections` 已删除；即使值为空，
读取或保存 retained TOML 也明确返回 Unsupported，不会透传、丢弃或转换旧字段。
启动 roots 的旧 `.aegpipe`/`.aegframe` 扩展同样拒绝，Windows ASCII 大小写一致。
无关 vendor 字段及多个 FeatureGraph roots 的顺序继续保留。

## 新项目上手与执行边界

编辑器操作步骤见[五域编排上手指南](../../aeg-flowgraph/editor-experience.md)；
游戏工程中随附的 [README](templates/game/README.md) 可以独立阅读，包含实际按钮与默认产物路径。

新建后进入 **Flow Graph**，选择默认图中的 **Gameplay starter**，查看 Contract Inspector；
点击 **Process editor** 可编辑 Ready → Running → Done 状态图。默认 Scene 不含物体，
Outline 只有契约和空 callback，添加它不会产生已有描边效果。

| 用户动作 | 作者数据、执行与交付的关系 |
| --- | --- |
| Validate draft / Compile preview | 使用离线 FeatureCatalog 检查当前未发布草稿，无需 DLL，不调用 Feature 执行 callback |
| Save | 保存 `.aegfeature`，不自动发布执行；Build/Cook 只读取保存后的文件 |
| Build > Build / Cook... > Build | 编译模块与 metadata/manifest，不自动替换当前活动模块 |
| Load Module | 加载真实模块；已有活动批次时保留活动图，否则读取保存的启动 roots，在下一 Scene 帧发布 |
| Load Saved Project | 明确使用保存的 `startup_feature_graphs` 完整批次，保留打开的作者草稿 |
| Apply / Play | Apply 发布当前草稿对应图；Play 推进模拟和新时钟工作，预览编译不会代替这些操作 |
| Cook / Build and Cook | Cook 交付已保存的启动资源；Build and Cook 先构建再打包，成功后切换输出 selector |

源码与 schema 的注册边界见 [Application/Authoring](../Authoring/README.md) 和
[Application/Tooling](../Tooling/README.md)。新 Feature 的生成源码仍需接入项目构建及共享 module factory。
跨模块依赖通过各模块 `public/` 头文件访问。

## 配置源码目录与展示信息

`Project.aegproj` 的 `[paths] source` 是所选项目的源码目录，例如：

```toml
[paths]
source = "Native"
```

从 Source 迁到 Native 时，需要移动源码、离线 metadata 和 FlowGraph 合同文件，并同步项目 CMake 的
源文件、factory header 与 `GameApp.cpp` 路径。配置修改不会移动文件。作者目录加载、Definition
保存/生成和 implementation navigation 使用所选项目的配置；旧 Source 目录的合同不会混入 Native。
根目录有多份 `.aegproj` 时必须明确选择项目文件，编辑器不会在它们之间猜测。

`<source>/FlowGraph/*.flowgraph.contract.json` 和 `*.flowgraph.draft.json` 可提供名称与端口标签。
展示信息只有在 Definition ID、版本、owner 和完整合同摘要匹配时才使用；匹配的恢复草稿名称优先于
旧 generated 名称。显示名可以重复，NodeId、Definition ID 和 PortId 的身份仍保持独立。
Root-only 的内部 Definition Service 保存 API 保留默认 `Source` 行为；配置源目录的调用方需传入
`selected_project_file`。编辑器 UI 会传递其所选或唯一项目文件。

## 保存、失败与恢复

打开图或点击 Validate 不会自动建立编译成功结果。只有 Compile preview 成功才建立作者预览的
last-good；语义或 catalog 改变后标为 stale，非法边会阻断新 Compile 并保留旧结果和当前定位诊断。
作者预览缓存随切换/重新打开文档清除，它不代表一个已发布 Runtime generation。

默认非空模板图的 `contract_digest` 与模板 metadata 和 factory 的目录摘要一致。新工程复制时忽略
模板内已有的生成目录，然后创建自己的空 Build/Binaries/Cache/Saved/.aeg；无需复制模板残留 DLL
才能编辑或预览。非空图缺失摘要或与模块目录不匹配会被 Cook 拒绝。

Cook 的私有启动来源指纹覆盖 Scene/FeatureGraph 源文件和 `.meta` 字节。保存源内容或修改产品元数据
后会重新导入；未变化的来源复用缓存，但实际缓存内容、参数引用、契约与最终包仍受生产校验器检查。
无指纹旧缓存会重新导入。读取、导入或编译失败时，输出 `current.toml` 不会推进，上一交付版本保持可用；
不要通过删除校验、把图摘要置零或清空缓存来替代修复来源。

模块 Build/Load/Apply 失败也不会用失败候选替换活动批次。发布或替换在帧边界完成，按界面诊断修复后
重新执行相应动作；成功的作者预览、已加载 Runtime 批次和 Cook 的 last-good 是三个独立结果。
