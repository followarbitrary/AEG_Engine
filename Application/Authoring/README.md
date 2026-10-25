# Application Authoring

`Application/Authoring` 保存 Editor 与离线工具共享的 FeatureGraph TOML 源文档。
`feature_graph_io.*` 负责 `.aegfeature` 读取、严格校验与原子保存；稳定语义模型属于
`Engine/Feature`。目录不拥有项目状态、资产信封、运行时执行或图形后端。

旧 Pipeline/Pass 作者文档与 `pipeline_document_io` 已删除。公开目标只传播 Feature 与
Foundation 合同，不传播 Pipeline 执行头；中性 ParameterBlock/ParameterValue 保留在
Foundation。Runtime 使用 cooked FeatureGraph，不读取这些作者 TOML。
