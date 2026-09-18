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
`Source/`，把 Pipeline 文档放在 `Assets/Pipelines/`，并同时生成 CMake、模块 metadata
和 `.meta` 产品身份文件。

`aeg_project_cook_tool` 读取 `Project.aegproj` 与已验证的模块 manifest，先刷新派生数据索引，
再自动补齐启动场景和启动 Pipeline 的干净缓存，最后写出 `game.aegbin`、文本 manifest
和二进制 manifest。Cook 阶段只接受 `.meta` 声明的 Asset UUID，运行时不重新读取 TOML
或源码资产。

模块的 `module.metadata.json` 使用仓库内的 nlohmann/json 编解码；`toml11` 只负责项目、
Pipeline 和资产 metadata 的 TOML。这样 Editor、Cooker 与 Runtime 共用稳定的类型目录、
实例身份和契约摘要，不需要让通用 Editor 编译游戏 C++。

Editor 的离线目录可调用 `EditorCompositionCatalog::replace_module_metadata()` 读取这份
JSON；它会校验契约摘要并复制 `TypeCatalog`，因此 DLL 重命名或暂时不可加载时仍能显示
Pipeline/Feature/Pass 节点，但不会绕过预览阶段的 bundle/ABI 校验。

## 边界

Tooling 不属于运行时交付物，不应被 Runtime 或游戏逻辑作为必要运行时依赖。工具
可以读取公共模块接口和资产格式，但不应链接后端私有实现来绕过抽象边界。
