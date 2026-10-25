# Engine Asset 运行时边界

`aeg_asset` 是 Cooked 资产载荷的运行时部分，负责 Cooked 封装格式及其编解码、
包索引校验、会话内 UUID 句柄以及中立载荷；UUID、`AssetId` 和稳定的 `AssetKind`
值由更低层 Foundation 提供，避免 Feature/Shader 反向依赖 Asset。

该目标不依赖 Tooling、TOML、Editor、cgltf 或 stb。Tooling 侧通过轻量外观调用
Engine 所有的 `encode_cooked_asset()`；运行时从包中读取同一份经过校验的字节数据。
在 `read_cooked()` 返回资产之前，包索引标识、封装标识、资产类型和 schema 版本必须保持一致。

这个边界是有意设计的：源文件元数据、导入器、Cooker 缓存和包写入属于
`Application/Tooling`；运行时标识、序列化资产 ABI、包读取和载荷解码属于本模块。
资产由 Application 工具生成，并不意味着运行时契约需要依赖 Application。

FeatureGraph 的二进制 AEGF 载荷由本模块提供单一编解码入口
`aeg::asset` 中的 `feature_graph_asset.hpp`。它只保存 FeatureGraph 文档和版本化资产边界；运行时 staging
由 `Application/Runtime` 的 FeatureGraph host 负责，资产层不拥有 Feature 状态。

RenderGraph Pass 仍是 Render Feature 的内部投影；运行时资产层不把 Pass 暴露为作者身份。

发布包由 `game.manifest.bin` 绑定项目、模块契约、启动 Scene、启动 FeatureGraph 和 `game.aegbin` 摘要。运行时在首帧前校验这些身份以及 Scene 的引用资产；发布目录不需要源资产、Editor 或 Tooling 文件。

`world_command_routes.hpp` 提供共享中立路由 DTO、AGWBIND1/schema2 编解码和纯 graph/contract 验证；Graph 身份保留作者原字符串，Scene/entity 使用 Foundation UUID。邻接 Scene 的作者 sidecar 与 schema3 `GameBuildManifest` 使用同一路由块。两端严格版本、完整消费、来源去重及共同预算：路由16MiB、4096条、owner深256、graph4096 bytes；manifest总64MiB。Asset不拥有live World或绑定执行；Cook验证最终包并重定位Scene product，Bootstrap在首回调前私有转换到Runtime DTO。

Font 资产使用 `AEGF` v1 载荷保存字体原始字节和 collection face index。运行时资产缓存可以校验并加载该 CPU 载荷；glyph rasterization、atlas 上传和 `TextRenderer` 绘制仍属于后续 renderer 功能。
