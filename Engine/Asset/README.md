# Engine Asset 运行时边界

`aeg_asset` 是 Cooked 资产标识的运行时部分，负责 UUID 解析、稳定的
`AssetKind` 值、Cooked 封装格式及其编解码、包索引校验、会话内 UUID 句柄，
以及中立载荷转换。

该目标不依赖 Tooling、TOML、Editor、cgltf 或 stb。Tooling 侧通过轻量外观调用
Engine 所有的 `encode_cooked_asset()`；运行时从包中读取同一份经过校验的字节数据。
在 `read_cooked()` 返回资产之前，包索引标识、封装标识、资产类型和 schema 版本必须保持一致。

这个边界是有意设计的：源文件元数据、导入器、Cooker 缓存和包写入属于
`Application/Tooling`；运行时标识、序列化资产 ABI、包读取和载荷解码属于本模块。
资产由 Application 工具生成，并不意味着运行时契约需要依赖 Application。

`CompositeFeature` 的 Part 由 Pipeline 运行时映射为稳定的 `FeaturePartId`。
RenderPass 的编排仍由父级 Feature 和 Render Part 负责；运行时资产层不拥有也不执行这些 Pass。
