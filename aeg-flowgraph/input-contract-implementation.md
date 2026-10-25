# Feature 端口来源与 Variadic 补齐方案

状态：当前 FlowGraph 补齐任务的后续实施分解，尚未实现。
依据：`authoring-and-execution-contract.md` 的端口六来源、稳定槽位与执行完成规则。
不恢复 Pipeline/Pass 作者模型。

## 接口与兼容裁定

1. `PortContract` 尾部增加可选 `InputPortContract`，只有输入端口可填；旧四字段写法继续支持。
   新合同单独声明 allowed sources、required/optional、边数、最大值大小、typed value 校验、
   版本化 Default、资产 adapter 与 Context key。参数默认值不负责选择端口来源。
2. `FeatureContract` 显式区分 `LegacyBytes` 与 `PortEnvelopeV1`。旧 callback 继续收到原字节，
   新 callback 第三个 span 收到公开版本化 envelope，包含 PortId/SlotId、来源、schema/version
   和分别带长度的 values；空值、None、空集合可区分。Runtime 的真实来源证据不进入作者可填写字段。
3. 新增稳定 GroupId、SlotId、DefaultId、ContextKey 和 EdgeId。Graph schema2 记录结构化 binding
   和 slot；schema1 的旧字符串不能推断成可执行来源。旧图保持旧模式，迁移为新输入必须显式进行。
4. binding 使用单一 variant：Unspecified、Edge、Asset、Literal、Default、Context、None。
   DefaultId/revision 与 Literal 分开；Asset 保存 UUID、目标、产品/内容修订，不能写 GPU 句柄。
5. Variadic group 声明成员端口、min/max、顺序及空集合规则；一个端口不能属于互相歧义的输入组。
   slot 记录 node/group/稳定 slot/order。排序不改 SlotId，单条数组边不增加 slot 数。
6. 新输入协议需要独立模块能力位，在执行前拒绝旧宿主；采用 ABI2 的能力协商，不改变 callback
   C 函数签名。增加 SDK minor 的同时严格验证支持范围，不能仅添加旧 reader 会忽略的 JSON 字段。
   具体能力位与 minor 值在实现前核对既有占用并冻结；已有 Legacy 模块保持原要求。

## 编译与执行门槛

- Compiler 拒绝缺来源、禁止来源、重复地址、非 Edge 来源残留活动边、旧 Default revision、
  Literal 类型错误、required 选择 None、未知/重复 slot、数量越界、未知 EdgeId 和歧义多输入。
- Edge 使用真实 delivery 与现有完成/期限条件；Literal/Default 使用共享 typed codec。
- AssetKindResolver 只证明 UUID/种类，不能证明 DeviceReady。资产必须由获准宿主 adapter 在
  真实产品和目标准备后返回拥有 payload/lease 的 Ready，Pending/Invalid/Cancelled 均不能消费。
- Context 按 key/scope/schema 与精确 owner 解析，不能覆盖其他输入记录。所有来源只读 capture
  完成后，统一准入 invocation；任何失败都回滚 owner 准入。
- World observation 现有 `Route::project` 会立即占用 cohort。改为多来源时必须两阶段处理，
  防止 Context 已占 cohort、另一个 Asset 仍 Pending 而没有 invocation 释放它的死锁。
- Retry 冻结输入 envelope、lease 与原期限，不在旧尝试上悄悄读取新来源；保留取消/迁移/过期拒绝。

## 实施片段与验证

1. **合同、格式与 Compiler**：单来源 schema、JSON/TOML/AEGF、摘要、schema1/2迁移负例；
   真实 RED 后实现。涉及 Feature 类型/合同 codec、Graph 模型/编译与 Authoring/Asset codec。
2. **Envelope 与非宿主来源**：Edge/Literal/Default/None 的真实多端口 consumer，证明不再拼接串线；
   覆盖重试、取消与整次提交失败。
3. **Asset**：真实 Pending→Ready、错误/取消/恢复，混合 Asset+Edge，证明种类校验不冒充设备就绪。
4. **Context/World**：纯 capture→统一准入，混合端口保持、Pending 不占死 cohort、失败回滚。
5. **Variadic**：稳定 slot 增删排序、min/max、Undo/Redo、Cook 与真实顺序敏感合并结果。

每片登记独立行为测试，由 root 统一编译/执行。未实现的来源必须显式拒绝准备；不会先写字段并
把它标为 Ready。最后以 S2/S2A 和真实游戏图完成端到端验收。
