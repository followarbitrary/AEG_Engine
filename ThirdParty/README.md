# 项目内置的第三方源码

编辑器资源管线使用或预留的全部第三方代码都放在此目录中。构建系统不会从
系统安装位置解析这些依赖。

| 目录          | 版本 / 快照 | 许可证                                  | 用途                                                    |
|---------------|-------------|-----------------------------------------|---------------------------------------------------------|
| `cgltf`       | 1.14        | `LICENSE`（MIT）                        | glTF/GLB 导入基础库                                     |
| `stb`         | 上游快照    | `LICENSE`（公共领域）                   | PNG、JPEG、BMP、TGA、GIF、PSD、PIC、PNM 和 HDR 图片解码 |
| `toml11`      | 4.4.0       | `LICENSE`（MIT）                        | `.aegmat` 材质编写元数据解析                            |
| `flatbuffers` | 24.3.25     | `LICENSE`（Apache-2.0）                 | 为未来生成模式的 `.aeg*` 数据载荷预留                   |
| `freetype`    | 2.13.3      | `LICENSE.TXT`（FTL / GPL-2.0-or-later） | 资源管线使用的内置字体后端                              |
| `msdfgen`     | 1.12.1      | `LICENSE.txt`（MIT）                    | 内置距离场字体后端                                      |

当前运行时文本快速路径使用已内置的 Dear ImGui `imstb_truetype` 头文件构建
小型 ASCII 字形图集。FreeType 和 msdfgen 保留在仓库内，使项目后续可以将
图集烘焙切换为 MSDF，而不引入机器本地依赖。
