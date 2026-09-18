# Application Project

`Application/Project` 负责版本化 `Project.aegproj` 配置契约与项目根目录相对路径映射。它只依赖
Foundation 和 toml11，不依赖 Engine Runtime、World、RHI、Vulkan 或平台原生 API。

公共入口：`public/project_context.hpp`。

`ProjectPaths` 会拒绝绝对路径以及逃逸出配置目录的路径。更上层的项目会话负责资产数据库、
World、后台任务和 GPU 延迟销毁。

`ProjectManager` 接受项目文件或项目根目录，持有唯一已加载的 `ProjectContext`。它会在通知已注册的
`ProjectLifecycleParticipant` 前创建配置目录；若后续 participant 加载失败，则回滚已启动的
participant；卸载时按逆序释放。participant 采用显式共享所有权，避免项目会话关闭后保留借用的
Application 对象。
# Project files

新建工程使用 `Project.aegproj`，内容沿用 `ProjectConfig` 的 TOML schema；旧版
`project.aegproject` 继续由现有 codec 读取。工程根目录下的 `Assets/`、`Source/`、
`Cache/`、`Saved/` 和 `Binaries/` 由项目路径配置解析，游戏源码通过 `EngineTools.cmake`
接入 Pipeline/Feature/RenderPass 扩展。

## 游戏工程模板

新工程的最小目录如下：

```text
MyGameProject/
├── Project.aegproj
├── Assets/Pipelines/Default.aegpipe
└── Source/
    ├── GameApp.cpp
    ├── GameModule.cpp
    └── Render/{Pipelines,Features,Passes}/
```

`CMakeLists.txt` 通过 `EngineTools.cmake` 调用 `aeg_add_game_module()` 和
`aeg_add_game_executable()`。模块目标生成 Editor 用的 DLL、metadata/manifest bundle；
可执行目标把同一份游戏模块源码静态链接进发布程序。Pipeline、Feature 和 RenderPass
必须继承对应专有基类，并在 `ModuleDefinition` 中登记稳定的 `TypeCatalog` 条目。Editor
只消费 metadata，不扫描或编译游戏源码；`.aegpipe` 中的类型、实例、参数和端口身份由
该目录校验并传给 Cooker 与 Runtime。

`ProjectCooker` 会把启动场景和 Pipeline 的 `.meta` 产品自动加入干净缓存，然后生成独立的
`Binaries/Shipping/<config>/game.aegbin`。发布启动只需要静态 EXE、运行库、两个 manifest
文件和 `.aegbin`，不需要 Editor DLL、源码资产或引擎源码路径。
