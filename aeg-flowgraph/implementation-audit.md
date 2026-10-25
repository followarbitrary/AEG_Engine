# FlowGraph 实现核查与补齐记录

核查日期：2026-10-18。原计划为根目录 `AEG FlowGraph Implementation Plan.md`。
当前实现位于 `codex/feature-five-domain`，保留本地修改，不创建 worktree、不提交。

## 当前结论

**原计划尚未全部完成。** Five Domain 原主线的 238/238 历史验收不等于这份 FlowGraph
产品计划的全部场景已经通过。旧实施进度中的 Pipeline 作者实现已部分被移除，必须重新映射。

本轮已通过源码、现有测试断言及电脑工具查看真实编辑器。主 FeatureGraph 当前为节点/边表格，
使用数字端口 ID 的 Add edge 表单；只有 Process 子视图存在真实节点画布。
这证明主 Feature 画布交互仍缺失，不将旧截图或旧进度文字算作当前证据。

## 架构裁定

- 保留最新五域架构：Feature 是唯一作者身份，Pipeline/Pass 是引擎内部投影。
  旧计划要求修改的已删 Pipeline 作者接口不恢复；旧格式的拒绝行为保持。
- 保留产品要求：显式输入来源、类型授权的 Variadic、真实数据依赖、保存恢复、节点画布、
  可复用图接口、源码导出和只读运行观察，不能因为换了架构就算已完成。
- 参数的 typed default / AssetKind 不能代替端口来源；Composite Definition 不能冒充
  可编辑 GraphAsset 子图；GPU 录制完成不能冒充 CPU Readback Ready。
- 历史测试结果仅作为线索。本轮实现分别取得新的失败/通过证据，再做适用范围回归。

## 逐任务核查

| 原任务 | 已有能力及依据 | 仍需完成 |
| --- | --- | --- |
| 1 输入合同 | 端口方向/schema/version、typed 参数、资产 resolver、宿主快照输入 | 端口六来源/必需性/唯一活动来源、Variadic Group/SlotId；当前 input_bindings/variadic_slots 只是字符串往返 |
| 2 持久化/Cook/摘要 | FeatureGraph TOML/AEGF、临时文件与原子替换、合同摘要、Process 布局 Cook 隔离 | 图语义摘要、备份/完整性/外部冲突与恢复、结构化来源和槽位往返、未知嵌套字段无损处理 |
| 3 编译/接口 | 实际端口 DAG、跨域/Clock transport、Composite 端口映射和递归拒绝 | 来源/槽位编译、GraphAsset 版本化公开接口和调用者升级；旧 Ordering/Present 作者格式由新规范明确取代 |
| 4 执行/预览 | CPU owner 提交依赖、取消/重试/迟到隔离、固定 GPU 同/跨队列同步、History/View、last-good | 每输出完成域/状态、条件开放的异步 Readback 作者闭环、S9 同帧双 View 一体证据、逐资源观察 |
| 5 编辑事务 | 增删/连线、参数、Process 编辑、统一 Undo/Redo | Feature 主图复制/替换、布局/快捷键、来源和槽位事务、定义/子图导航 |
| 6 编辑器界面 | 真实目录筛选、数字连线、Process 画布、发布/检查 | 主 Feature 节点与端口画布、拖线候选、聚焦/缩放/复制、完整错误定位 |
| 7 代码节点/IDE | Feature-only 四文件生成、真实 DLL 回调、bundle 摘要核对、接口差异审阅、模块 Reset | 通用每模块 host-safe Exporter、实际 IDE 导航/构建回流、源码修订信息 |
| 8 全面验收 | 原五域基线及若干真实 GPU/Editor/Shipping 回归 | 按当前架构逐条 S1–S12/S2A、产品硬要求和实际新增路径建立新证据，不能沿用旧测试名结案 |

## 实施顺序与状态

1. **作者数据安全和主图可操作性**：图规范摘要、备份/完整性/并发冲突/恢复；
   Feature 画布、稳定布局与复制/拖线；通用代码合同 Exporter。进行中。
2. **端口来源与可变输入**：单一合同建模，贯穿 JSON/TOML/AEGF、Compiler、Runtime、编辑器。
3. **图资产复用和基础算子**：版本化公开接口、调用路径、影响清单与 Merge/Compose/Select/Join/Adapter。
4. **运行观察及条件能力**：真实资源/裁剪/别名/屏障来源；严格完成协议与受控 Readback。
5. **交付验收**：真实编辑器操作、作者保存/重开、Cook/独立 Runtime、GPU 结果和完整适用测试。

后续条目只有在对应实现与本轮行为门禁完成后才改为完成。本记录不会把尚缺能力移入“历史完成”。

## 本轮证据

- `build/flowgraph-io-red-ctest.log`：第二次保存没有上一完整版本备份，真实失败。
- `build/flowgraph-digest-red-ctest.log`：新增语义摘要未实现时，字段变化/集合语义回归真实失败。
- `build/flowgraph-first-gates-build.log`：四个实际目标 Debug 构建 exit0。
- `build/flowgraph-first-gates-ctest.log`：摘要、I/O 通过；主画布、Feature 布局 Cook 剥离、
  通用 Exporter 三项按缺失行为失败，没有降低断言。
- `build/flowgraph-unknown-red-ctest.log`：未知节点字段被静默接受，真实失败；随后改为拒绝，保留原文件。
- `build/flowgraph-source-safety-build.log` 与 `build/flowgraph-source-safety-ctest.log`：
  作者图语义、I/O、真实进程中断恢复及公开边界 **4/4，0.89秒，exit0**。
  其中并发、完整性和恢复是新增行为检查，不冒称每个子断言都单独完成过 RED。

保存使用精确原文字节作为编辑会话版本，在文件写锁内检查冲突；新文件先持久 flush、校验再替换，
上一完整源保存在 `.bak`。恢复是显式操作，不会因坏文件静默切换；恢复也检查用户观察的原文件版本。
新 source 可保存完整性与语义摘要；既有未加完整性头的合法 TOML 仍可读取。
未知结构字段拒绝解析并保留磁盘原文，已声明的扩展数据继续往返。
