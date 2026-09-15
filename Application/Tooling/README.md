# Application Tooling

## 目录职责

该目录用于离线工具和构建辅助，包括 Shader 编译/反射工具、资产检查、代码生成、
格式验证和开发期诊断工具。

## 当前状态

- `public/asset_metadata.hpp`：128 位 Source/Asset UUID、稳定子资产键和版本化 `.meta`
  TOML 编解码。Meta 是导入器身份和产品身份的唯一持久来源；路径和数组索引不得代替 UUID。
  保存操作先写临时文件，再原子替换旧 Meta，导入中断不会覆盖既有 UUID。
- `public/asset_package.hpp`：版本化 `.aegbin` 单文件包。Writer 按完整 Asset UUID 排序并写出
  小端包头、16 字节对齐 payload 和定长索引；Reader 在公开任何 payload 前验证文件范围、索引排序、
  UUID 唯一性和条目边界。Writer 始终先写临时文件，完成后才原子替换目标包。
- `schema/cooked_asset.fbs` 与 `public/cooked_asset.hpp`：每个 `.aeg*` 产物的 FlatBuffers
  envelope。它保存 Asset UUID、类型、schema version、运行时依赖和类型特定 payload；读取时必须
  先执行 FlatBuffers Verifier，再允许下游类型解码。
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

## 边界

Tooling 不属于运行时交付物，不应被 Runtime 或游戏逻辑作为必要运行时依赖。工具
可以读取公共模块接口和资产格式，但不应链接后端私有实现来绕过抽象边界。
