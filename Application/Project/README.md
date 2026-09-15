# Application Project

`Application/Project` 负责版本化 `.aegproject` 配置契约与项目根目录相对路径映射。它只依赖
Foundation 和 toml11，不依赖 Engine Runtime、World、RHI、Vulkan 或平台原生 API。

公共入口：`public/project_context.hpp`。

`ProjectPaths` 会拒绝绝对路径以及逃逸出配置目录的路径。更上层的项目会话负责资产数据库、
World、后台任务和 GPU 延迟销毁。

`ProjectManager` 持有唯一已加载的 `ProjectContext`。它会在通知已注册的
`ProjectLifecycleParticipant` 前创建配置目录；若后续 participant 加载失败，则回滚已启动的
participant；卸载时按逆序释放。participant 采用显式共享所有权，避免项目会话关闭后保留借用的
Application 对象。
