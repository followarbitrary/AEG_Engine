# MyGameProject template

`Source/Render/` 是游戏项目自定义渲染扩展的唯一入口：Feature 继承引擎提供的领域基类，
Pass 继承 `aeg::pipeline::RenderPass`，并通过稳定类型 ID 注册到模块目录。Editor 读取模块
导出的 `TypeCatalog` 元数据绘制 Pipeline/Feature/Pass 节点；`.aegpipe` 只保存稳定 ID、
参数、端口连接和布局。

`Source/GameModule.metadata.json` 是 Editor 的离线类型目录输入。构建 `GameModule` 时，
CMake 会先把 DLL 复制到 `Binaries/Editor/...` staging 目录，再生成带 SHA-256 的
`module.metadata.json` 和 `module.manifest.json`；运行时只接受通过该清单校验的模块。

`Assets/Scenes/Main.aegscene.meta` 和 `Assets/Pipelines/Default.aegpipe.meta` 固定了
源文件与 cooked product 的 UUID。脚手架会同时生成 Main scene，Asset Cooker 可据此建立
场景与启动 Pipeline 的依赖闭包。

CompositeFeature 当前只作为设计与注册审查约定，不在代码层增加额外约束。
