# 项目内置的第三方源码

编辑器资源管线使用或预留的全部第三方代码都放在此目录中。构建系统不会从
系统安装位置解析这些依赖。

本目录中的第三方代码和字体保留各自的许可证与版权声明，不因仓库根目录的
Apache-2.0 协议而改变。分发包含这些组件的源码或二进制时，应一并保留适用的
许可证、版权与致谢；根目录 [NOTICE](../NOTICE) 记录了本项目的 FreeType 致谢。

| 目录 | 版本 / 快照 | 许可证 | 用途 |
|---|---|---|---|
| `cgltf` | 1.14 | [LICENSE](cgltf/LICENSE)（MIT） | glTF/GLB 导入基础库 |
| `stb` | 上游快照 | [stb_image.h 文件末尾](stb/stb_image.h)（MIT / 公共领域，可任选其一） | PNG、JPEG、BMP、TGA、GIF、PSD、PIC、PNM 和 HDR 图片解码 |
| `toml11` | 4.4.0 | [LICENSE](toml11/LICENSE)（MIT） | `.aegmat` 材质编写元数据解析 |
| `nlohmann/json` | 3.11.3 | [LICENSE.MIT](nlohmann/json/LICENSE.MIT)（MIT） | 模块元数据 JSON 编解码 |
| `flatbuffers` | 24.3.25 | [LICENSE](flatbuffers/LICENSE)（Apache-2.0） | Cooked Asset / Pipeline 二进制载荷编解码 |
| `freetype` | 2.13.3 | [LICENSE.TXT](freetype/LICENSE.TXT)（FTL / GPL-2.0-or-later；本项目选择 [FTL](freetype/docs/FTL.TXT)） | 编辑器字体解析与 CPU atlas 后端 |
| `msdfgen` | 1.12.1 | [LICENSE.txt](msdfgen/LICENSE.txt)（MIT） | 内置距离场字体后端 |
| `ImGui` | 1.93.0 WIP | [LICENSE.txt](ImGui/LICENSE.txt)（MIT） | 编辑器即时模式 UI |
| `Quill` | 上游快照 | [LICENSE](Quill/LICENSE)（MIT） | 异步日志 |
| `SPIRV-Reflect` | 上游快照 | [LICENSE](SPIRV-Reflect/LICENSE)（Apache-2.0；全文见根目录 [LICENSE](../LICENSE)） | SPIR-V shader 反射 |
| `VulkanMemoryAllocator` | 上游快照 | [LICENSE.txt](VulkanMemoryAllocator/LICENSE.txt)（MIT） | Vulkan 内存分配 |
| `Fonts` | 上游快照 | 各字体目录的 `OFL.txt`（SIL OFL 1.1） | 编辑器字体与回归样本 |

`Fonts/` 中的 Inter、FiraCode、JetBrainsMono、IBM Plex Sans、Noto Sans SC 和
Source Code Pro 各自附带 OFL 全文，分发字体时应保留对应目录中的声明。

部分组件内嵌其他第三方代码或字体，例如 Quill 的 fmt、ImGui 的 Proggy 字体，以及
SPIRV-Reflect 的 Khronos SPIR-V 头文件；其独立版权与许可声明保留在对应源码中。

编辑器通过仓库内的 Dear ImGui FreeType adapter 使用 FreeType 在 CPU 上
解析 TrueType 与 CFF OpenType 轮廓并构建图集；GPU 纹理上传仍由
Application/Engine Resource/RHI 路径负责。`msdfgen` 已随源码归档放入仓库，
供后续 MSDF 图集烘焙使用。

`Fonts/SourceCodePro/SourceCodePro-Regular.otf` 是随仓库保存的 OFL CFF
OpenType 回归样本，与同目录 `OFL.txt` 共享许可证。

## 离线归档

`_downloads/` 保存对应上游快照，构建不需要联网下载。恢复或同步第三方目录后，
可用下面的 SHA-256 值核对归档完整性：

| 归档 | SHA-256 |
|---|---|
| `flatbuffers.zip` | `E706F5EB6CA8F78E237BF3F7ECCFFA1C5EC9A96D3C1C938F08DC09AAB1884528` |
| `freetype.zip` | `A7165CB405DFCE349AE64D6F59EFB2567565CA8EDBACBEFF2A8D0E4CFA779CF5` |
| `msdfgen.zip` | `CB3FEB6697481ADDC84D3BDD2CA8329BEECDC4DDB9267580567FCF02705BD387` |
| `nlohmann-json-3.11.3.tar.xz` | `D6C65ACA6B1ED68E7A182F4757257B107AE403032760ED6EF121C9D55E81757D` |
| `toml11.zip` | `075DD130A9F517C3EAED502ECE6A2C1088B1D0DD4AA79882FD0CA7609325FFDC` |
