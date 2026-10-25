# MyGameProject template

## 第一次使用

1. 在编辑器打开项目，进入 **Flow Graph**，确认 `Select FeatureGraph` 为
   [Default.aegfeature](Assets/Features/Default.aegfeature)。默认图已有一个 Logic / Process 节点
   **Gameplay starter**（`Game.Logic.Gameplay`），状态图为 `Ready → Running → Done`。
2. 点击画布中的节点，或在 Logic 泳道选择它，查看 **Contract Inspector**。
   选中节点只改变选择；点击画布空白处可清除选择。
3. 保持该节点选中，点击 **Process editor**，即可跳到它的状态图；也可在
   `Process subgraphs` 区点击 **Use selected as Process owner**。选中状态卡片后，
   **Make selected entry** 设置入口，**Explicit terminal** 设置终态。状态卡片可拖动；状态、转移与布局
   编辑共用 Undo/Redo。
4. 点击 **Check draft** 跳到五域检查区，再点击 **Validate draft**。
   默认图应显示 `Draft validation passed`。点击 **Compile preview** 后应显示
   `Authoring preview compiled` 和 `Preview: last-good`。
   这两步使用离线契约目录，无需先构建 DLL，也不会执行 Gameplay callback。
5. 点击 Flow Graph 内的 **Save** 保存作者文档。Build/Cook 使用磁盘上的项目、Scene 和
   FeatureGraph 文件，不会自动保存未保存的编辑。

默认 Scene 没有物体；这个示例演示状态编排。Logic 泳道有节点，其余 Physics、Render、Audio、
Stream 泳道在添加对应 Feature 前保持为空。节点显示名用于阅读和搜索；连接、保存和诊断仍使用
完整 Definition ID、NodeId 和 PortId。

## 从预览到执行

1. 打开 **Build > Build / Cook...**。确认项目文件、`Engine directory`、`CMake executable`、
   `Configuration` 与 `Target`；模板模块 Target 为 `GameModule`，配置示例为 `Debug`。
   点击 **Build** 并等待成功。
2. 返回 Flow Graph，在 `Module manifest` 填入构建结果的实际清单路径。默认模板为
   `Binaries/Editor/windows-x64-vulkan/dev/Debug/module.manifest.json`。点击 **Load Module**。
   第一次加载会读取已保存的启动图，在下一 Scene 帧发布完整批次；已有活动批次时会保留其图。
3. 加载成功后，若要发布当前打开的草稿，点击 **Apply** 并等待 `Publication pending` 结束。
   没有 executable module 时 Apply 禁用。更新模块契约时，按界面的 **Review Contract Changes**
   检查影响，再使用相应的 **Accept & Load Module** 操作。
4. 点击 **Play** 推进场景模拟。当前 callback 在两次满足 Clock 条件的 Logic invocation 中依次提交
   Running 和 Done；这不等于两个渲染帧。
   若修改状态名称或转移，请同时修改 [GameModule.cpp](Source/GameModule.cpp) 中的 callback。

| 操作 | 使用的输入与结果 |
| --- | --- |
| Validate draft / Compile preview | 当前作者草稿与契约目录；生成未发布的检查结果和预览 |
| Save | 将当前作者文档写入 `.aegfeature`，不发布执行 |
| Build | 从已保存源码构建模块及 metadata/manifest bundle，不自动替换当前活动模块 |
| Load Module | 校验并加载真实模块；保留活动图，首次加载使用保存的启动图 |
| Load Saved Project | 强制读取保存的 `startup_feature_graphs` 完整批次；保留打开的作者草稿 |
| Apply | 用当前草稿替换活动批次中的对应图，在下一 Scene 帧提交 |
| Cook / Build and Cook | 打包已保存的启动资源；后者先构建，再 Cook，不执行当前未保存草稿 |

## 添加节点与连接

点击 **Node catalog**，按名称或完整 Definition ID 搜索，在 **Feature Catalog** 中点击 **Add node**。
从画布的输出端口拖到输入端口可连边；先选择 `Connection kind`，再填写适用的 capacity/deadline。
Data 只适用于相同 Domain 和 Clock；跨域或跨 Clock 需按契约选择 Snapshot、Queue 等 transport。
端口方向、Schema、版本和 transport 限制由同一生产校验器检查，具体错误显示在当前边的诊断中。
Gameplay 示例没有数据端口，它的状态转移应在 Process 状态图内编辑。

创建自己的类型可从 **Feature definitions > New Feature** 开始。**Save Draft** 保存恢复草稿；
**Save and Generate** 才把有效契约加入作者目录并生成代码。生成后还要把源码和注册入口接入
[CMakeLists.txt](CMakeLists.txt) 的 `GAME_SOURCES` 及共享 module factory，构建后再加载执行。

`Game.Render.Outline` 是目录中可检查的 Render 契约示例，当前 callback 没有绘制实现，
因此默认图不会放入它来暗示已有 Outline 效果。添加自己的游戏行为从 `Source/GameModule.cpp`
的 Feature factory 和 callback 开始。保存图仅保存作者数据；Compile preview 不执行游戏代码。

## 失败时保留什么

- 打开图只显示当前草稿，不会自动建立成功的编译预览；Validate 也不会建立 last-good。
  Compile 成功后才缓存 last-good。语义或目录改变时旧结果标为 `last-good (stale)`；
  非法边使 Compile 阻断，当前非法节点/边仍可检查，旧编译结果单独保留。修复后重新 Compile。
  切换或重新打开文档会清除该预览缓存。
- Build、模块加载或 Apply 失败时，查看诊断并修复来源；已发布的活动批次不会被失败候选替换。
  作者预览的 last-good 与实际 Runtime generation 是两种不同结果。
- Cook 失败不会推进输出目录中的 `current.toml`，上一版交付仍可使用。
  Cook 会比较启动 Scene/FeatureGraph 的源码及 `.meta` 内容指纹；保存后再次 Cook 会重新导入变化的
  来源，无指纹旧缓存也会重建。源码未变时仍检查实际缓存和最终包，已有缓存不能绕过契约校验。

## 修改项目源码目录

模板的源码位置由 [Project.aegproj](Project.aegproj) 的 `[paths] source = "Source"` 指定。
例如改为 `source = "Native"` 时，需要同时移动源码、离线 metadata 与 `FlowGraph` 合同目录，并修改
CMake 中的 `GAME_SOURCES`、`METADATA_FACTORY_HEADER` 和 `APP_SOURCE` 路径；修改配置不会移动文件。
作者目录加载、Definition 保存/生成和实现导航读取所选项目配置，不会混入旧 Source 目录的合同。
同一根目录有多份 `.aegproj` 时，应显式打开需要的项目文件。

## 模块与构建约定

`Source/GameModule.hpp/.cpp` 中的 `aeg_define_GameModule()` 是游戏项目 Feature Contract/Definition
的共享 factory。Editor 的
FeatureGraph 只保存稳定 Definition/Node/Port 身份、参数、连接和布局；RenderGraph 的内部
WorkItem 不进入作者目录。

模块源码使用 `FeatureModuleDefinition` 和 `AEG_REGISTER_GAME_FEATURE`；游戏作者不需要
注册 Frame/Render Pipeline、Phase 或 RenderPass。
`GameApp.cpp` 使用轻量 `game_runtime_bootstrap.hpp`。游戏目标不获得引擎内部 Concurrency
头文件或执行器 API；引擎的线程封装规则不约束游戏开发者自己的线程与协程。

构建 `GameModule` 时，`aeg_add_game_module` 的 `METADATA_FACTORY_HEADER` 和 `METADATA_FACTORY`
让 CMake 用同一份 `GAME_SOURCES` 构建独立的 `GameModule_contract_exporter` host 工具。
工具调用共享 factory，通过引擎既有校验器和 codec 输出
`<build>/generated/module-contracts/GameModule/<config>/module.source.metadata.json`；
这条构建链无需手写导出 JSON。factory 只构造声明和回调，不能创建 GPU 资源或执行游戏逻辑；
exporter 不加载 DLL，也不会调用 Feature 执行或状态校验回调。

factory 模式用私有宏 `AEG_FEATURE_CONTRACT_EXPORTER=1` 编译共享源码，DLL 的
`AEG_EXPORT_GAME_MODULE_API` 入口按该宏排除；工具的 `main` 由引擎提供。
模块目标追加的 include paths、compile definitions/options 和链接依赖也用于 exporter。
该模式用于 native host 构建；交叉编译时使用预先在 host 导出的 `METADATA` 显式文件。

成功导出后才链接并发布 DLL。既有发布工具把 DLL 复制到 `Binaries/Editor/...`，生成带
SHA-256 的 `module.metadata.json` 和 `module.manifest.json`；运行时只接受清单校验和
DLL 合同一致性比较均通过的模块。factory 拒绝声明会使构建失败并保留上次成功的 bundle。
`Source/GameModule.metadata.json` 保留为脚手架的离线目录快照，不再是模块构建的输入。
已有项目仍可使用 `METADATA <path>`；它与 factory 参数互斥。
Build 不会自动把生成的 metadata 回写到 Source 离线快照。手工修改 factory 契约后，需要让离线目录
与成功导出的合同一致，再刷新或重新打开项目检查图的版本与摘要诊断；Build 成功本身不代表当前
打开草稿的离线目录已更新。

`Source/FlowGraph/*.flowgraph.contract.json` 使用现有编辑器 envelope 保存可读名称和端口标签。
只有 Definition ID、版本、owner 与完整契约摘要都匹配离线目录时，名称才用于图中显示；
这些标签不会覆盖模块契约或改变 Runtime 身份。不同 Definition 可以同名，连接仍按 ID 处理。

默认图的非零 `contract_digest` 与离线目录及共享 factory 一致。改变契约后，应处理版本、图迁移和
受影响连接，再重新构建模块并验证图；非空图缺少或使用过期摘要会使 Cook 失败，不能把摘要改为 0
来绕过检查。

`Assets/Scenes/Main.aegscene.meta` 和 `Assets/Features/Default.aegfeature.meta` 固定了
源文件与 cooked product 的 UUID。脚手架会同时生成 Main scene，Asset Cooker 可据此建立
场景与启动 FeatureGraph 的依赖闭包。

Process/StateMachine 是 FeatureGraph 中 Logic/Process Feature 的子图；RenderFeature 的多个
内部 WorkItem 只在派生 RenderGraph 视图中可见。
