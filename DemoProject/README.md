# Rendering Showcase

打开 `Project.aegproj`。启动场景是 `Scenes/RenderingShowcase.aegscene`，示例资源使用当前 Forward Renderer profile。
原来的空 `Scenes/Main.aegscene` 保留，可用于从空场景开始编辑。

演示包含棋盘地面、橙色方块、青色球体、蓝色立柱及两个小方块，另有相机、方向光、点光源、聚光灯和天空环境，共 12 个实体。
模型、材质和中性纹理位于 `Assets/RenderingShowcase`，通过项目资产导入流程加载。

## 观察与切换

1. 在 **Hierarchy** 选择物体，可在 Inspector 修改 Transform 和 MeshRenderer 的 Tint。
2. 在 **Sun - Directional Shadows** 中切换 `casts_shadow`，观察地面阴影。
3. 调整 **Blue Point Light - Forward** 和 **Warm Spot Light - Forward** 的强度、范围及位置。
4. 在 **Sky and Exposure** 中调整天空、曝光、Gamma 和色调映射。

项目的启动 roots 指向 `Assets/Features/Motion.aegfeature`，由 `Source/GameModule.cpp` 导出的 `Demo.Logic.Motion` Process 提供持续旋转命令。它只通过显式 `World.Transform.Observation/1` 输入和 `World.Transform/1` Outbox 命令工作；Editor 不会猜测目标实体或自动绑定路由。

模块身份保存在 `Project.aegproj`，由项目 CMake 生成动态 DLL bundle。原 Pipeline FlowGraph 源文件已迁出作者路径。

视口 GPU 回读预览位于 `Saved/Previews/Forward.png` 和 `Saved/Previews/Deferred.png`。

## 从命令行打开

在引擎仓库根目录执行：

```powershell
& ".\build\Debug\AetherEngine.exe" --project ".\DemoProject\Project.aegproj"
```

如果 Editor 已经打开旧场景，请重新打开项目，或通过 File 菜单打开 `Scenes/RenderingShowcase.aegscene`。
