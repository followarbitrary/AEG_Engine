# AEG FlowGraph：验收与交付

[返回架构总览](<../AEG FlowGraph Architecture.md>) · [编辑器操作指南](editor-experience.md)

本页记录当前 Feature 五域编排的验收范围和交付链。规范模型见
[Feature 五域架构设计](<../AEG Feature Five-Domain Architecture.md>)；架构文档中的目标能力
只有具备实现和验收证据后，才属于本页的已交付范围。

当前作者入口是 `.aegfeature`：Logic、Physics、Render、Audio、Stream 五域 Feature 使用
同一套稳定 Definition/Node/Port 身份、契约目录和文档事务。Logic/Process 状态图已有编辑与运行
路径；ExecutionPlan 和 RenderGraph 用于检查真实发布结果，内部 Pipeline/Pass 不进入作者目录。

## 1. 各操作验证什么

| 操作 | 输入与结果 | 是否执行游戏 Feature callback |
|---|---|---|
| Save | 将当前作者草稿原子保存到源文件，更新保存检查点 | 否 |
| Validate draft | 检查当前草稿与当前 FeatureCatalog，显示节点、端口、边和 owner path 诊断 | 否 |
| Compile preview | 生成未发布的五域作者预览；只有显式点击并成功后才建立 last-good | 否 |
| Build | 通过项目 CMake 构建模块；factory 模式先导出契约，再链接和发布 DLL bundle | 导出契约不执行 callback |
| Load Module | 验证并选择模块 bundle，请求 Scene 会话加载；成功后的实际发布在 Scene 帧边界完成 | 不是作者预览；后续运行使用真实模块 |
| Apply | 使用已加载的可执行模块，将编辑草稿提交给 Runtime 发布流程 | 发布后由实际运行调度执行 |
| Play | 进入游戏运行模式，推进相应游戏时钟、Process 和 callback | 是 |
| Cook | 读取已保存的项目、启动资产与显式模块 manifest，验证并生成交付版本 | 不执行游戏 callback，不启动 CMake |

`BuildAndCook` 按顺序完成构建和交付验证。仅点击 Save 或 Compile preview 不会更新运行态，
也不会产生可独立运行的交付包。没有可执行模块时仍能 Validate/Compile；主编辑器的 Apply
保持禁用。编辑器自身的 Scene Preview 与游戏根的执行状态分别检查。

`Load Saved Project` 使用项目文件中保存的完整启动图列表；它与提交当前编辑草稿的 Apply
有不同输入。不要把模块选择、作者预览或保存成功当成 Runtime 已经采用新图的证据。

## 2. 作者预览的验收状态

打开图会刷新当前草稿投影和诊断，不会自动生成成功编译结果。Validate 不会建立或替换
last-good；重复检查也不应改动图、catalog、dirty 检查点或 Undo/Redo 历史。

| 场景 | 必须观察到的结果 |
|---|---|
| 尚未成功 Compile | 显示 `Preview unavailable until Compile preview succeeds.` |
| 合法草稿点击 Compile preview | 显示 `Authoring preview compiled` 和 `Preview: last-good` |
| 语义、draft revision 或 catalog 改变 | 旧成功结果标记为 `last-good (stale)` |
| 非法草稿再次 Compile | 显示 `Compile preview blocked`，保留旧成功结果和定位诊断 |
| Undo/Redo | 恢复对应作者快照；预览操作本身不新增历史事务 |
| 切换路径或重开文档 | 清除其他文档生命期的预览；同地址重建对象也不能沿用旧结果 |

当前草稿与上次编译结果分别展示。非法草稿仍显示自己的泳道和 transport 边；下方的
Compiled authoring preview 保存上次成功的 Clock groups、provenance 和边。
这样可以同时看到“当前哪里错了”和“上次成功编译了什么”，不会用旧成功图掩盖新错误。
纯布局修改不改变图的语义摘要或运行计划。

跨域/跨 Clock 的 Data 边必须由生产校验器拒绝；Snapshot、Queue、Event、Command、Receipt
按各自契约检查 Clock、容量、deadline 和端口条件。鼠标连边入口会先验证事务，不能写入的边
应立即诊断。若源文件或导入快照已有非法边，Compile 必须阻断，且不能清空 last-good。
同一节点重复声明 Process 子图也必须与正式编译器一样拒绝。

## 3. 当前核心验收场景

| 场景 | 通过标准 | 主要自动证据 |
|---|---|---|
| 新工程离线编排 | 默认 Gameplay starter 含 Ready → Running → Done；未构建 DLL 也能预览；模板契约摘要与真实 factory 目录一致 | `aeg_project_scaffold_smoke`、`aeg_editor_project_feature_authoring_smoke` |
| 真实 Validate/Compile 点击 | 鼠标实际命中按钮；合法预览成功；非法 Data 边阻断；旧 Snapshot 编译结果保留并标 stale | `aeg_editor_feature_graph_orchestration_ui_smoke` |
| 跨域选择与可读身份 | Composite 多泳道投影有独立控件身份，仍选择同一个作者 NodeId；同名 Definition 不串线；名称和 ID 都能搜索 | 同上及 `aeg_editor_feature_graph_canvas_ui_smoke` |
| 画布可操作 | 端口拖线、复制、拖动、Delete、外部选择同步、取消选择、精确 Undo；原生滚轮可到达末尾控件 | canvas UI、native scroll smoke |
| 契约编辑与项目目录 | 只使用配置的源码目录；如 Source 改为 Native，旧 Source 合同不会混入；Save/Generate 和实现导航使用所选项目 | node contract storage、Definition UI/service、implementation resolver smoke |
| 文档保存和重开 | 原子保存、完整备份、稳定身份与未知扩展保留；失败不推进保存检查点 | graph document I/O、source crash、transactions smoke |
| 保存后重新 Cook | 同 AssetId 的源码或 `.meta` 改变后重新导入；旧无指纹缓存重建；非法新源阻断交付并保留上次 selector | `aeg_project_cooker_smoke` |
| 最终包可信 | 实际缓存和最终包的 UUID、类型、schema、参数依赖、World 路由及契约经过正式校验 | ProjectCooker、runtime package closure 等 smoke |
| 模块和实际主窗口 | factory 导出与运行目录一致；失败导出/发布保留完整旧 bundle；实际 Main 可构建、编辑、保存和发布 | contract exporter、module publication、application workflow 测试 |
| 实际渲染来源 | Opaque/Forward/Deferred、Frame/Render 来源、generation、操作归属及像素通过真实 GPU 路径验证 | renderer、Scene Runtime 和实际 Main recovery 测试 |

新项目的 Scene 默认没有物体。Gameplay starter 演示 Process 状态转换；
Outline 是可检查的契约示例，当前 callback 没有绘制实现。五条泳道可用并不意味着模板已经为
每个域提供了完整游戏实现，也不意味着已有任意 Shader、Pass 或性能图形化编辑器。

## 4. 四层 last-good 的边界

| 层次 | 成功时替换什么 | 失败时保留什么 |
|---|---|---|
| 作者 Compile preview | 当前文档生命期的成功预览 | 旧成功预览；当前非法草稿与诊断继续可见，旧预览可标 stale |
| Runtime 发布 | 在 Scene 帧边界提交完整候选根与 generation | 已发布根及其来源；pending 或失败候选不伪装成已生效 |
| 模块 bundle | 通过校验的 DLL、metadata 和 manifest | 上次完整成功 bundle；metadata-only 读取不激活代码 |
| Cook 交付 | 完整版本目录及输出根的 `current.toml` | 旧交付 selector 和已发布版本目录 |

这些状态分别管理。作者预览不是可执行 Runtime generation；模块 metadata 可读也不表示 DLL
可运行；Cook 的缓存 bundle 也不是已交付版本。不能用其中一层成功覆盖另一层失败。

### 4.1 模块导出与 metadata-only

模板的 `aeg_define_GameModule()` 是 host-safe 的共享声明 factory。
`aeg_add_game_module` 的 `METADATA_FACTORY_HEADER` 与 `METADATA_FACTORY` 让同一份
`GAME_SOURCES` 生成独立的 `GameModule_contract_exporter`，先输出配置对应的
`generated/module-contracts/GameModule/<config>/module.source.metadata.json`。

Exporter 不加载 DLL，也不执行 Feature 执行或状态校验 callback；factory 自身只能构造声明和
回调，不能在声明阶段创建 GPU 资源或运行游戏逻辑。导出校验成功后才继续链接和 bundle 发布。
显式 `METADATA <path>` 模式与 factory 参数互斥；仅修改声明 metadata 也必须触发重新发布。
交叉编译使用已在 host 导出的显式 metadata。

Editor 可从离线 metadata 建立作者目录；可读名称和端口标签只有在 ID、owner、version 和完整
契约摘要匹配时使用。实际加载仍验证 bundle 内容摘要、ABI 和 DLL 导出的契约，目录展示不能
绕过这条执行边界。详见 [Tooling](../Application/Tooling/README.md) 与
[游戏模板](../Application/Project/templates/game/README.md)。

### 4.2 Save → Cook 的源码更新

Cook 为启动 Scene/FeatureGraph 记录私有的源码与 `.meta` SHA-256 指纹。命中缓存时仍比较
指纹；变化或旧缓存无指纹时重新导入，无需用户手工清空 Cache。未变源可复用原缓存，但最终
包校验仍检查实际缓存 payload，不把指纹当作类型、schema、参数或路由有效性的证明。

导入发布前先使旧指纹失效。只有源码前后指纹一致才写入有效新指纹；若导入失败，只有确认
选中的缓存 bundle 未变，才恢复旧指纹或原先的缺省状态。检测到源在导入期间变化会报告失败，
要求以稳定保存的源重试；这不是多人协同合并或全局源文件快照事务。

Cook 失败不会推进交付 `current.toml`。重导入可能生成新的缓存 bundle，失败保证不能扩张为
“所有缓存文件始终没有变化”；已交付版本和可变缓存应分别检查。

## 5. 旧格式与后续能力

当前不提供旧三层作者资产的自动转换。旧 `.aegpipe`、`.aegframe`、`.aegproject`、
retired Pipeline/Pass/Present 作者字段和旧启动字段会明确诊断，不能通过改扩展名或重盖版本
当成新 FeatureGraph。现代项目使用版本 2 `Project.aegproj` 与正式 `.aegfeature` 文档。

需要迁移的项目应在独立的现代文档中按 Feature 契约重建并验收；不要期待编辑器自动添加
Ordering/Present 线、FeaturePoint、内部 Pass 或兼容节点。这些名字仍可能出现在历史设计说明中，
不构成当前作者入口。

以下属于后续设计/扩展方向，不能以本轮测试结果宣称交付：

- 通用多人协同、实时语义合并及冲突解决 UI。
- 任意 Shader/命令生成图、完整 GPU 性能时间线和自动优化建议。
- 无需新增实现即可使用的完整 Bloom、LayerCompose、后处理节点库。
- 任意领域能力和平台队列覆盖；仍须先有正式 Feature 契约、实现和执行域支持。

## 6. 复验命令与证据

在仓库根、已配置 `build` 且 `cmake`/`ctest` 位于 PATH 的终端运行。工具链配置见
[根构建说明](../Readme.md)；Visual Studio 自带 CMake 未加入 PATH 时，应使用安装目录中的
`cmake.exe` 和同目录 `ctest.exe` 的完整路径。

先重建所有受影响的库及其使用方，避免把旧二进制的通过当成当前源码证据：

```powershell
cmake --build build --config Debug --parallel 3
ctest --test-dir build -C Debug --output-on-failure -R '^(aeg_editor_feature_graph_(orchestration_ui|canvas_ui|native_scroll|preview)_smoke|aeg_editor_(project_feature_authoring|node_contract_storage|node_definition_service)_smoke|aeg_project_cooker_smoke)$'
```

使用 CTest 调用 ProjectCooker 等测试，可自动传入对应 DLL fixture；不要省略 fixture 参数直接运行
单个测试程序。模块导出与发布可单独复验：

```powershell
ctest --test-dir build -C Debug --output-on-failure -R '^aeg_feature_(contract_exporter|module_publication)_smoke$'
```

完整验收按已声明的资源锁与串行要求调度：

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = '2'
ctest --test-dir build -C Debug --output-on-failure -j 3 --output-junit orchestration-full-ctest.xml
```

原生窗口、主窗口集成和其他独占资源测试应遵循 CMake 中的资源锁。测试若操作默认相对场景文件，
必须使用独立 fixture 工作目录，并在清理前恢复原工作目录；不能依靠测试恰好顺序执行来避免
`editor_scene.aegscene.aegtmp` 冲突。

**已完成的历史验证**：2026-10-25，Debug 全量 CTest **248/248 通过，0 失败，525.25 秒**。
来源为 [实现与验收记录](../docs/superpowers/plans/2026-10-24-orchestration-interaction-acceptance.md)
及当时的 `build/orchestration-20261025-verified-ctest.log`。这是对应实现快照的结果，不是每次
文档编辑后自动重跑的承诺。继续修改代码后应重新执行上述命令并记录新结果。
