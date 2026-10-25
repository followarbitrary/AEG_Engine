# Application Tooling

## 目录职责

该目录用于离线工具和构建辅助，包括 Shader 编译/反射工具、资产检查、代码生成、
格式验证和开发期诊断工具。

## 当前状态

- `public/asset_metadata.hpp`：Source UUID、稳定子资产键和版本化 `.meta` TOML 编解码。
  Asset UUID/AssetKind 直接复用 Engine 的运行时类型，避免 Tooling 重新定义一套身份 ABI。
  Meta 是导入器身份和产品身份的唯一持久来源；路径和数组索引不得代替 UUID。
  保存操作先写临时文件，再原子替换旧 Meta，导入中断不会覆盖既有 UUID。
- `public/asset_package.hpp`：版本化 `.aegbin` 单文件包 Writer。Writer 按完整 Asset UUID 排序并写出
  小端包头、16 字节对齐 payload 和定长索引；运行时 Reader 位于 `Engine/Asset`，Tooling 只通过
  兼容别名使用它，避免维护两份包读取规则。Writer 始终先写临时文件，完成后才原子替换目标包。
- `public/cooked_asset.hpp`：Tooling 对 Engine Cooked envelope codec 的兼容门面。FlatBuffers schema 位于
  `Engine/Asset/schema/cooked_asset.fbs`，由 Engine 生成并拥有；它保存 Asset UUID、类型、schema
  version、运行时依赖和类型特定 payload，读取时必须先执行 FlatBuffers Verifier。
- `public/asset_cooker.hpp`：Importer 注册表，负责读取 `.meta`、选择解码器、校验 Meta 产品身份、
  将 `CookedAsset` 编码为 FlatBuffers，并发布到不可变缓存 bundle。`public/stb_texture_importer.hpp`
  是首个实际解码器，使用 `stb_image` 解码栅格源为 RGBA8；其 payload 前 12 字节依次为 little-endian
  宽、高和通道数（恒为 4），随后为 RGBA 像素。`public/cgltf_mesh_importer.hpp` 使用 `cgltf` 解析
  `.gltf`/`.glb`，每个三角形 primitive 对应一个 Meta mesh 产品；payload 为 16 字节头（顶点数、索引数、
  32 字节 stride、保留字段），再接 position/normal/uv 交错顶点和 `uint32` 索引。
- `public/cooked_artifact_store.hpp`：每个 Source UUID 的版本化烘焙输出。产物和 manifest 写入新的
  不可变目录后，才以原子文件替换更新唯一可变的 `current.toml`，读取端不会看到半发布 bundle。
- `public/asset_dependency_packer.hpp`：打包时由项目资产索引提供 `AssetId -> CookedAsset bytes` 解析器；
  它从场景或其他根资产递归读取 FlatBuffers 运行时依赖、验证每个返回值的 ID、去重并交给
  `AssetPackageWriter` 输出一个 `.aegbin`。循环引用因访问集而只收录一次。

现有 Shader 构建工具仍通过根目录 CMake 配置组织。后续 Importer、Cooker 与 Packer 必须基于
Meta 的 SourceId/AssetId 建立可追踪的输入、输出和增量构建依赖。

## 游戏工程交付工具

`aeg_project_scaffold_tool` 创建可直接配置的游戏工程模板。模板把游戏源码放在
`Source/`，把 FeatureGraph 文档放在 `Assets/Features/`，并同时生成 CMake、模块 metadata
和 `.meta` 产品身份文件。

`aeg_project_cook_tool` 读取 `Project.aegproj` 与已验证的模块 manifest，检查启动 Scene/FeatureGraph
的源码与 `.meta` 指纹并刷新必要的缓存，最后写出 `game.aegbin`、文本 manifest、
二进制 manifest 和 `module/` 下的完整已验证模块 bundle。完整结果先写入新的
`versions/<publication>/`，验证后再原子替换输出根的 `current.toml` selector，失败 Cook
保留原 selector。二进制 manifest 固定模块 manifest 的相对路径和 SHA-256；Cook 会对复制
后的 bundle 再次调用 Module 正式验证器。Cook 阶段只接受 `.meta` 声明的 Asset UUID，运行时
不重新读取 TOML 或源码资产。

### 保存后 Cook 与缓存有效性

启动资产的 AssetId 不变，不代表源码没有变化。`ProjectCooker` 在私有缓存目录
`.project-cook-sources/` 中按 SourceId 和产品 AssetId 保存源码及 `.meta` 的 SHA-256 指纹。
源码或产品元数据改变后，下次 Cook 会重新导入；没有指纹的旧缓存也会重建，不要求用户
手工删除 Cache。源码未变且指纹匹配时可复用缓存；实际缓存 payload 和最终包仍要经过正式校验。

导入前原子写入无效标记，防止导入器已切换 bundle 后旧指纹继续描述新产物。导入失败时，
只有确认选中的缓存 manifest 未改变，才恢复先前的指纹或原先的缺省状态；成功导入后还会
核对源码前后指纹，变化时失败并要求以稳定保存的源重试。这是增量导入的来源记录，
不提供多人编辑的全局源快照或语义合并协议。

Cook 依据已验证模块 metadata 重建语义注册表，不需要运行游戏 callback。非空 FeatureGraph
必须有正确的 `contract_digest`；仅有可解析的 TOML 或合法 UUID 不足以交付。正式编译、
资产参数依赖闭包、实际缓存中的 UUID/AssetKind/schema、World 路由和复制后的模块 bundle
都需验证。失败不会更新交付根的 `current.toml`，也不会覆盖已发布版本目录；一次失败的
Cook 可能已生成新缓存 bundle，不能把交付 last-good 与缓存当前版本混为一谈。

`aeg_feature_end_to_end_smoke` 使用真实 Editor 保存的非空 FeatureGraph 和 GameSDK DLL，
通过上述 Cook selector 进入动态 Runtime，验证 Snapshot 输入、Render 输出来源以及最终
RenderGraph 红蓝像素和 GPU 输出退休；不另建 Cooker、注册器或渲染夹具。

模块描述符和容器使用 Module 的 JSON codec，Feature 契约使用 Feature 的统一 codec；`toml11` 只负责项目、
FeatureGraph 和资产 metadata 的 TOML。这样 Editor、Cooker 与 Runtime 共用稳定的 Feature 目录、
实例身份和契约摘要，不需要让通用 Editor 编译游戏 C++。

Editor 的离线目录可调用 `EditorCompositionCatalog::replace_module_metadata()` 读取这份
JSON；它会校验契约摘要并复制 `FeatureCatalog`。因此没有可执行 DLL 时仍能显示节点，
并执行 `Validate draft`、`Compile preview`。这两项作者操作不创建 Runtime generation，
不加载 DLL，也不执行 Feature callback。只有显式 Compile 成功才建立作者预览 last-good；
错误草稿继续显示自己的节点、边和诊断，上次成功结果保留并标记 stale。

实际 `Load Module`/`Apply` 仍经过 bundle 内容摘要、ABI、DLL 契约及 Runtime 发布校验。
metadata-only 展示或作者预览成功不能代替可执行模块加载。作者图只有 Feature 身份；
Frame/Render 的内部 Pipeline/Pass 是执行实现与只读检查内容。

### 共享 factory 与模块发布

游戏模板通过 `aeg_add_game_module()` 的 `METADATA_FACTORY_HEADER` 和 `METADATA_FACTORY`
构建 host Contract Exporter。Exporter 与 DLL 使用相同的 `GAME_SOURCES` 及共享 factory；
配置相关输出位于 `<build>/generated/module-contracts/<target>/<config>/module.source.metadata.json`。
只有导出和契约校验成功后，才继续链接并将 DLL、metadata、manifest 交给既有发布器。
仅声明 metadata 改变也必须触发重新链接/发布，不能继续选择旧声明的 bundle。

factory 在 host 上只构造声明和回调，不创建 GPU 资源或运行游戏逻辑。Exporter 不加载 DLL，
也不执行 Feature 执行或状态校验 callback。声明拒绝或导出失败会阻断新发布，保留上次完整
成功 bundle。显式 `METADATA <path>` 与 factory 参数互斥；交叉编译应传入预先在 host 导出的
metadata。模板的 `Source/GameModule.metadata.json` 是未构建项目的离线目录快照，不是 factory
模式的构建输入；修改实际契约后应重新导出并按需要更新该离线快照。

完整操作边界见 [验收与交付](../../aeg-flowgraph/validation-and-delivery.md)：Save 写作者源文件，
Build 构建 C++，Compile preview 记录未发布作者预览，Apply 提交 Runtime 根，Play 推进游戏
执行；Cook 使用已保存的启动资产与显式模块 manifest 制作交付包。Cook 不代替 Build。

`ProjectOperationService` 的 Build/Cook 流程由 Logic Process FeatureGraph 驱动。Tooling 内部后台执行通过
`aeg::concurrency::WorkerExecutor` 的 `kIoWorker` 域提交，并使用 `CancellationToken` 与
`TaskContext` 传播取消；调用方只持有 operation ID 并轮询状态，不接触线程句柄。该并发实现属于
引擎 Tooling 内部，游戏 Feature/Module 不需要也不能依赖它。

## 边界

Tooling 不属于运行时交付物，不应被 Runtime 或游戏逻辑作为必要运行时依赖。工具
通过所属模块的 `public/` 接口读取模块契约和资产格式，不能跨模块导入 `private/` 头文件或
链接后端私有实现绕过抽象边界。

`ProjectOperationService::select_module(project_file, module_manifest, configuration)` 读取显式
选择的 manifest，复用项目 codec、ProjectPaths 和 Module 的正式 bundle 验证器，返回
已有 owning ModuleBundle 的实际 build ID、契约和内容摘要。相对 manifest 基于项目根
解析，显式绝对路径允许自定义 CMake 输出位置；不扫描最新目录，也不推断 dev 或配置。
选择与 Cook 共用 Tooling 私有的项目匹配检查：symbolic module ID、平台和请求配置必须
分别一致，`module.target` 只是 CMake target。Cook 在读取启动资产或写出产物前报告
`cook.module_project`。选择成功尚未加载或激活代码；既有 DynamicModuleLoader 在库打开前重新验证并核对
所选 bundle 的固定事实，拒绝有效 manifest 的构建切换。Scene 后续复用这条加载边界
及 ModuleRegistry，不再复制相同的匹配函数。

Tooling 没有 Pipeline 执行消费者；CMake 已删除其 Pipeline include/link 依赖。离线业务
仍通过现代 Project Process Features 执行，外部 Concurrency 调度只负责服务生命期。
这些 Process 经 `ModuleRegistry` 的正式静态 FeatureModule 注册，拥有
`Engine.Tooling.ProjectOperations` 的真实模块归属。状态持有实际 Runtime 复制的
Definition/Node/Instance/Process、generation、InvocationCommit/Failure；显示阶段名仅在
同一次 Invocation 真正提交后追加，运行中也在每个 owner 边界发布 owning 快照。

Build 执行 Validation/Configure/Compile，Cook 执行 Validation/Cook/Publication 验证，
BuildAndCook 执行五步。Cook 不启动 CMake；Cook/BuildAndCook 必须明确给 module manifest，
不再以空 manifest 静默退化为 Build。Build 缺 Engine 目录或 CMakeLists 明确失败。
`cmake_executable` 可指定真实工具路径，空值保系统 CMake 查找；项目 Build 目录由既有
ProjectPaths 解析。CMake 和 Cook 均为 ExternalIo/ExternalOwner 合同，取消和源提交失败
不撤销已发生的外部 IO；Cook 的原子 selector 发布仍由唯一的 `cook_project` 实现负责。

`cancel` 只请求取消；`execution_complete` 表示后台实际收尾，`wait(operation_id)` 等待
服务执行完成后返回真实最终 owning 状态。已完成 Process、commit 与已发布产物不会
因取消被清空；析构仍 cancel/join。Editor 退出、关闭或切换项目时等待收尾，不把
Cancelled 状态当作已经 join。Tooling 仍不属于 GameSDK 或 Runtime 交付依赖。

## 验证与复验

在仓库根、已配置构建目录且 CMake/CTest 位于 PATH 的终端运行。先重建受影响目标，
再通过 CTest 调用测试；它会为 ProjectCooker 等程序传入所需 DLL fixture。

```powershell
cmake --build build --config Debug --parallel 3
ctest --test-dir build -C Debug --output-on-failure -R '^aeg_(project_cooker_smoke|cooked_artifact_store_smoke|feature_contract_exporter_smoke|feature_module_publication_smoke)$'
```

`aeg_project_cooker_smoke` 覆盖旧无指纹缓存迁移、同 AssetId 的源 A→B 更新、`.meta` 更新、
Scene 源保存、无效新源阻断、未变源的恶意缓存验证，以及失败后交付 selector/版本保留。
`aeg_feature_contract_exporter_smoke` 和 `aeg_feature_module_publication_smoke` 验证真实 factory
导出、声明变化、失败恢复及完整模块 bundle 的 last-good；作者按钮的真实鼠标点击由
`aeg_editor_feature_graph_orchestration_ui_smoke` 覆盖。

2026-10-25 已完成的 Debug 全量结果为 **248/248 通过、0 失败、525.25 秒**，记录见
[实现与验收记录](../../docs/superpowers/plans/2026-10-24-orchestration-interaction-acceptance.md)。
这是对应实现快照的历史证据；本次文档整理没有重新执行构建或 CTest。完整命令、资源锁和
后续能力边界见 [验收与交付](../../aeg-flowgraph/validation-and-delivery.md)。
