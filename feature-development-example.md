# Feature 开发示例

本文用一个 `Game.Logic.Counter` 教你创建 Feature、实现回调、声明参数、注册模块，并在
FeatureGraph 中使用。示例基于当前 C++20 GameSDK：Module metadata / SDK contract major3，
Module ABI major2。游戏代码通过 `AEG::GameSDK` 接入。

示例的业务规则：每次执行读取节点参数 `Step`，把实例中的计数加上 `Step`，保存新计数，
通过 `CounterOut` 输出。默认 `Step = 1`；改成 `5` 后，连续成功提交得到 `5、10、15……`。
它声明 60 Hz Clock，但计数依据成功执行和提交次数，不把墙上时间直接当计数。

阅读顺序：

1. [理解 Definition、节点、参数与状态](#1-理解-definition节点参数与状态)
2. [创建文件并实现完整 Feature](#2-创建文件并实现完整-feature)
3. [逐项理解合同与参数](#3-逐项理解合同与参数)
4. [理解 execute 的输入和返回值](#4-理解-execute-的输入和返回值)
5. [注册到游戏模块](#5-注册到游戏模块)
6. [生成模块元数据并构建](#6-生成模块元数据并构建)
7. [在编辑器中使用](#7-在编辑器中使用)
8. [从编辑器生成 Feature 骨架](#8-从编辑器生成-feature-骨架)
9. [扩展到输入、Process 和其他域](#9-扩展到输入process-和其他域)
10. [常见问题与交付检查](#10-常见问题与交付检查)

## 1 理解 Definition、节点、参数与状态

| 概念 | 本例 | 谁负责 |
| --- | --- | --- |
| FeatureDefinition | `Game.Logic.Counter` 的合同和 C++ 回调 | 开发者在游戏模块里实现 |
| FeatureNode | 图中某一个 Counter 节点，有独立 `node_id` | 图作者添加、配置 |
| FeatureInstance | 已发布图中的运行实例 | Runtime 创建和管理 |
| Parameter | `Step`，节点配置，每次执行只读 | 图作者设置，Feature 解码 |
| Input | 上游端口或宿主提供的执行输入字节 | 数据生产者与消费 Feature 约定协议 |
| State | 上一次提交的 `uint64_t` 计数 | Feature 提议新值，Runtime 在 owner 边界提交 |
| Output | `CounterOut` 上的一条计数消息 | Feature 返回，Runtime 按图的边路由 |

同一 Definition 可以在图里添加多个节点。两个独立 Counter 实例各自保存计数；把其中
一个节点的 `Step` 设成 `5`，不会修改另一个节点的参数。不要把实例计数放到全局变量、
函数内可变 `static` 或被多个实例共享的 lambda 捕获里。

```text
节点配置 Step ────────┐
上次 StateSnapshot ───┼── execute ── StateDelta ── owner 校验并提交
InvocationToken ──────┘                              ├─ 新计数状态
                                                   └─ CounterOut → 图中的下游
```

## 2 创建文件并实现完整 Feature

本节展示直接编写 C++ 的方式。想先用编辑器填写合同并生成骨架，可先看第 8 节，再用本节
的回调逻辑替换生成的桩函数。两种方式选择一种，避免注册同一 Definition 两次。

先通过编辑器的 **Project > New Project** 创建游戏项目，再在项目根目录中创建下列代码文件。
模板已经提供一个可编辑的启动 FeatureGraph；本例使用 `Assets/Features/Default.aegfeature`。
已有项目应选择其真实启动图路径，并保留仍需使用的模块注册和启动根。
这里的 `Source/` 属于游戏项目：

```text
MyGame/
├─ Project.aegproj
├─ CMakeLists.txt
├─ Assets/Features/Default.aegfeature       # 模板已有，后面在图编辑器中添加节点
└─ Source/
   ├─ Features/
   │  ├─ Counter.hpp
   │  └─ Counter.cpp
   ├─ GameModuleDefinition.hpp             # DLL 和元数据工具共用的模块工厂
   ├─ GameModule.cpp                       # DLL 导出入口
   ├─ WriteModuleMetadata.cpp              # 仅开发时运行的元数据工具
   └─ GameModule.metadata.json             # 由工具生成
```

### 2.1 Counter.hpp

```cpp
#pragma once

#include "feature_contract.hpp"

namespace game {

inline constexpr aeg::feature::ParameterId kCounterStep{"Game.Counter.Step"};
inline constexpr aeg::feature::PortId kCounterOut{"CounterOut"};
inline constexpr aeg::feature::ValueSchemaId kCounterSchema{"Game.Counter.UInt64LE"};

[[nodiscard]] aeg::feature::FeatureDefinition make_counter_feature();

} // namespace game
```

这里的 ID 由字符串稳定散列得到。同一类 ID 的拼写改变，就代表身份改变。
`display_name` 可以调整展示名称；已经发布的参数或端口 ID 应保持稳定。
Schema 名称只标识协议，字节如何编码仍需要生产者、消费者共同实现。

### 2.2 Counter.cpp

```cpp
#include "Counter.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace game {
namespace {
using namespace aeg;
using namespace aeg::feature;

// 业务状态和端口 payload 的协议：固定 8 字节、小端 uint64_t。
std::vector<uint8_t> encode_count(uint64_t value) {
    std::vector<uint8_t> bytes(8);
    for (unsigned i = 0; i < 8; ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8u * i));
    return bytes;
}

uint64_t decode_count(std::span<const uint8_t> bytes) {
    // 调用前必须先确认 bytes.size() == 8。
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= uint64_t{bytes[i]} << (8u * i);
    return value;
}

ParameterContract make_step_parameter() {
    ParameterMetadata metadata{};
    metadata.type = ParameterType::kUInt32;
    // ParameterValue 用 uint64_t 同时承载 UInt32 和 UInt64。
    metadata.default_value = uint64_t{1};
    metadata.constraint = NumericRange{1.0, 100.0, 1.0, false};
    metadata.display_name = "Step";
    metadata.description = "Amount added on each successful Counter execution";
    metadata.ui_group = "Counter";
    metadata.ui_order = 0;
    return {kCounterStep, 1, 4, true, std::move(metadata)};
}
} // namespace

aeg::feature::FeatureDefinition make_counter_feature() {
    using namespace aeg;
    using namespace aeg::feature;

    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId{"Game.Logic.Counter"};
    definition.version = {1, 0, 0};

    auto& contract = definition.contract;
    contract.definition_id = definition.id;
    contract.version = definition.version;
    contract.structure = FeatureStructure::kAtomic;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kContinuous;
    contract.state = StatePolicy::kInstanceState;
    contract.scope = ScopeContract{ScopeId{"World"}};
    contract.clock = make_periodic_clock_contract(
        ClockId{"Game.Counter.Clock"}, 60, 8, LateDataPolicy::kQueue,
        DeadlinePolicy::kObserve, 250'000, 0);
    contract.permissions.version = 1;
    contract.state_payload.schema_version = {1, 0, 0};
    contract.state_payload.max_size = 8;
    contract.parameters = {make_step_parameter()};
    contract.ports = {
        {kCounterOut, PortDirection::kOutput, kCounterSchema, 1}
    };

    // 初始状态为空，约定表示 0；之后的非空状态必须正好为 8 字节。
    definition.validate_state_payload = [](std::span<const uint8_t> bytes) {
        if (!bytes.empty() && bytes.size() != 8)
            return foundation::invalid_argument("Counter state must be empty or 8 bytes");
        return foundation::Result::success();
    };

    // 捕获 schema 的拥有副本，不能捕获局部 contract 的引用。
    definition.execute = [schema = contract.parameters](
        const InvocationToken& token,
        const StateSnapshot& state,
        std::span<const uint8_t> input,
        std::span<const uint8_t> parameters)
        -> foundation::ResultValue<StateDelta> {
        // 本例只有输出端口，不消费输入。
        if (!input.empty())
            return {foundation::invalid_argument("Counter does not accept input bytes"), {}};
        if (!state.payload.empty() && state.payload.size() != 8)
            return {foundation::invalid_argument("Unsupported Counter state payload"), {}};

        const auto records = decode_feature_parameters(parameters, schema);
        if (!records) return {records.result, {}};
        const auto record = std::find_if(records.value.begin(), records.value.end(),
            [](const auto& value) { return value.id == kCounterStep; });
        if (record == records.value.end())
            return {foundation::invalid_argument("Counter Step is missing"), {}};
        const auto value = deserialize_parameter_value(record->payload, ParameterType::kUInt32);
        if (!value) return {value.result, {}};
        const auto* step = std::get_if<uint64_t>(&value.value);
        if (!step)
            return {foundation::invalid_argument("Counter Step must be unsigned"), {}};

        const uint64_t previous = state.payload.empty() ? 0 : decode_count(state.payload);
        if (previous > (std::numeric_limits<uint64_t>::max)() - *step)
            return {foundation::invalid_argument("Counter overflow"), {}};
        const uint64_t next = previous + *step;

        PortMessage output{};
        output.source_port = kCounterOut;
        output.schema = kCounterSchema;
        output.schema_version = 1;
        output.payload = encode_count(next);

        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.payload_revision = next; // 本例用计数作业务调试标记。
        delta.state_payload = encode_count(next);
        delta.output_messages.push_back(std::move(output));
        return {foundation::Result::success(), std::move(delta)};
    };
    return definition;
}

} // namespace game
```

参数解码会检查声明、版本、大小、类型、范围并补齐默认值。本例的状态和端口字节是
Feature 自己定义的 8 字节协议；参数 envelope 使用 SDK 提供的 codec。不要把整个
`parameters` 直接按 `uint32_t` 读取，也不要用 `memcpy` 序列化带填充、指针或 STL 成员的结构体。

## 3 逐项理解合同与参数

### 3.1 FeatureDefinition 和基础合同

| 字段                           | 本例值               | 含义和填写规则                                                   |
|--------------------------------|----------------------|------------------------------------------------------------------|
| `definition.id`                | `Game.Logic.Counter` | 稳定功能身份；同一 Runtime 注册目录内唯一，跨模块也不能冲突      |
| `definition.version`           | `1.0.0`              | 定义版本；与 `contract.version` 一致                             |
| `definition.owner_module`      | 注册时填入           | 注册 helper 绑定所属模块；已填写时必须与模块 ID 一致             |
| `contract.definition_id`       | 等于 `definition.id` | 合同声明属于哪一个定义                                           |
| `structure`                    | `kAtomic`            | 原子执行叶子，具有业务回调；Composite 用于展开子 Feature         |
| `domains`                      | `to_mask(kLogic)`    | Atomic 恰好属于五域中的一个；这里填写 bit mask                   |
| `execution`                    | `kContinuous`        | 由已发布计划按 Clock 持续调度                                    |
| `state`                        | `kInstanceState`     | 业务状态归当前实例所有                                           |
| `scope.id`                     | `World`              | 运行作用域身份；声明这个 ID 不会向回调注入 `World*`              |
| `permissions.version`          | `1`                  | 权限合同版本必须非零；本例 capability mask 保持 `0`              |
| `state_payload.schema_version` | `1.0.0`              | 计数字节的布局版本，独立于定义版本                               |
| `state_payload.max_size`       | `8`                  | 状态 payload 最大字节数；额外校验器进一步约束实际长度            |
| `execute`                      | 上面的 lambda        | 读取快照并返回候选 Delta；只在模块内存在，不序列化到资产         |
| `validate_state_payload`       | 空或 8 字节          | 可选的纯业务载荷校验；本例把初始空状态解释为 0，因此也接受空载荷 |

### 3.2 Clock 的 7 个位置参数

```cpp
make_periodic_clock_contract(
    ClockId{"Game.Counter.Clock"}, // 1
    60,                            // 2
    8,                             // 3
    LateDataPolicy::kQueue,         // 4
    DeadlinePolicy::kObserve,       // 5
    250'000,                       // 6
    0);                            // 7
```

| 位置 | 参数               | 本例含义                                         |
|------|--------------------|--------------------------------------------------|
| 1    | `id`               | Clock 的稳定身份；自定义名称不会自动获得宿主准入 |
| 2    | `frequency_hz`     | 名义频率 60 Hz；执行机会来自宿主推进的时间       |
| 3    | `max_backlog`      | 允许保留的积压执行计划上限 8，必须大于 0         |
| 4    | `late_data_policy` | 忙碌或积压时使用 Queue 策略；不能无界积压        |
| 5    | `deadline_policy`  | Observe 记录超期；RejectCommit 会拒绝超期提交    |
| 6    | `work_budget_ns`   | 声明回调工作预算 250,000 ns，即 0.25 ms          |
| 7    | `phase_ns`         | 首次周期的相位偏移 0 ns，必须小于名义周期        |

该 helper 同时填写 capability version `1.0.0`，并按频率把相对 deadline 向上取整：
60 Hz 对应 `16,666,667 ns`。预算必须大于 0 且不超过 deadline；Composite 的预算为 0。
宿主仍要检查 Clock 能力、共享 Clock 调度声明和域预算；声明通过结构校验不等于已准入。
共用同一个 Clock ID 的定义应使用一致的调度合同，叶子自己的工作预算可不同。

`execute` 没有隐含 `delta_time` 参数。需要实际时间时，应使用已声明输入协议中的时间值，
例如现有 World observation 协议提供的 elapsed time。不要假定回调每隔精确 1/60 秒完成。

### 3.3 Step 参数的每一个字段

```cpp
return {kCounterStep, 1, 4, true, std::move(metadata)};
//      id            version max_size required metadata
```

| 字段                   | 本例值                        | 对应什么                                                          |
|------------------------|-------------------------------|-------------------------------------------------------------------|
| `ParameterContract.id` | `Game.Counter.Step`           | 稳定参数身份；回调按它查找记录                                    |
| `version`              | `1`                           | 参数 payload 协议版本；record 必须与声明一致                      |
| `max_size`             | `4`                           | 单个参数 payload 的字节上限，不含 envelope 头；UInt32 固定 4 字节 |
| `required`             | `true`                        | 计算有效值后必须存在；有默认值时，缺省记录可以先补齐默认值        |
| `metadata.type`        | `kUInt32`                     | 参数线上的类型；底层 `ParameterValue` 用 `uint64_t` 承载          |
| `default_value`        | `uint64_t{1}`                 | 节点没给这个参数时使用 1；显式值不会被默认值覆盖                  |
| `constraint`           | `NumericRange{1,100,1,false}` | 最小 1、最大 100、步进 1、不启用 normalized 约束                  |
| `display_name`         | `Step`                        | 编辑器显示文字，不改变参数身份                                    |
| `description`          | 英文说明                      | 参数用途说明                                                      |
| `expert_only`          | 默认 `false`                  | 专家参数展示标记；不构成权限控制                                  |
| `ui_group`             | `Counter`                     | 编辑器参数分组                                                    |
| `ui_order`             | `0`                           | 编辑器展示顺序                                                    |

`step` 约束会参加值校验，不能仅把它理解成拖拽速度。`step = 0` 表示连续数值。
`metadata` 可以省略，此时只校验 envelope 的身份、版本、required 和大小，payload 由 Feature
解释；需要标准编辑控件时应填写类型元数据。

常用类型与 `foundation::ParameterValue` 的对应关系：

| `ParameterType`           | `ParameterValue` 成员类型    | 典型线宽或约束                                    |
|---------------------------|------------------------------|---------------------------------------------------|
| `kBool`                   | `bool`                       | 1 字节                                            |
| `kInt32` / `kInt64`       | `int64_t`                    | 分别 4 / 8 字节，Int32 还检查范围                 |
| `kUInt32` / `kUInt64`     | `uint64_t`                   | 分别 4 / 8 字节，UInt32 还检查范围                |
| `kFloat` / `kDouble`      | `double`                     | 分别 4 / 8 字节，必须有限                         |
| `kString` / `kEnum`       | `std::string`                | 带长度前缀；Enum 可声明 `EnumChoices`             |
| `kAssetReference`         | `foundation::AssetReference` | 完整 16 字节 UUID；可约束资产种类和是否允许空引用 |
| `kVector2/3/4` / `kColor` | `std::vector<double>`        | 固定 2/3/4 个 float64 分量；Color 为 4 分量       |

资产参数写稳定 UUID，由项目或包的 resolver 校验。文件路径、GPU 句柄和任意指针不能代替资产身份。

### 3.4 端口字段

| 字段              | 本例值                  | 说明                                    |
|-------------------|-------------------------|-----------------------------------------|
| `PortContract.id` | `CounterOut`            | 输出端口身份，图的边引用它              |
| `direction`       | `kOutput`               | 生产数据；消费者对应 `kInput`           |
| `schema`          | `Game.Counter.UInt64LE` | 8 字节小端无符号计数协议的稳定身份      |
| `version`         | `1`                     | 端口 payload 协议版本；连接两端必须兼容 |

输出消息填写 `source_port`、`schema`、`schema_version`、`payload`。通常让图决定目标端口和
transport；默认输出消息为 Data。Runtime 为已接受的交付附加来源与 delivery 身份，作者无需
构造 `source`、`delivery_id` 或宿主证据。端口不连边时，这个输出不会自动显示到屏幕或修改场景。

## 4 理解 execute 的输入和返回值

### 4.1 四个回调参数

```cpp
ResultValue<StateDelta> execute(
    const InvocationToken& token,
    const StateSnapshot& state,
    std::span<const uint8_t> input,
    std::span<const uint8_t> parameters);
```

| 参数         | 内容                            | 本例如何使用                                               |
|--------------|---------------------------------|------------------------------------------------------------|
| `token`      | 当前调用及提交版本身份          | 把 `token.state_version` 原样写入 `delta.expected_version` |
| `state`      | 已提交状态的只读快照            | 初始空 payload 视为 0，非空按 8 字节读取                   |
| `input`      | 本次选中输入消息的 payload 字节 | 本例没有输入端口，因此要求为空                             |
| `parameters` | 当前节点的完整参数 envelope     | 先 `decode_feature_parameters`，再解码具体 typed payload   |

当前回调的 `input` 是选中消息 payload 的拼接结果，**不是 `PortMessage` 数组**，也不隐含
端口 ID、分包长度或时间。多个输入需要自行约定无歧义编码或使用既有 typed adapter。
不要把只读 span 或引用保留到回调结束以后；需要保留时复制其数据。

`StateSnapshot` 的 `version` 用于提交一致性，`schema_version` 描述字节布局，`payload` 是业务
状态，`current_state` 是 Process 状态机中的状态名。本例没有 Process 状态机。

| `InvocationToken` 字段 | 含义                                                  |
|------------------------|-------------------------------------------------------|
| `invocation_id`        | 一次调用身份                                          |
| `attempt_id`           | 当前尝试身份，用于区分重试                            |
| `generation`           | 当前 Runtime 代次，换图或生命周期变化会使旧结果失效   |
| `input_version`        | 本次输入快照版本                                      |
| `process_revision`     | Process 生命周期修订；非 Process 不应拿它模拟业务状态 |
| `state_version`        | 捕获状态的提交版本，是 `expected_version` 的来源      |

### 4.2 StateDelta 字段

| 字段                | 本例 / 默认值         | 作用                                                                             |
|---------------------|-----------------------|----------------------------------------------------------------------------------|
| `expected_version`  | `token.state_version` | 乐观提交前提；不能用业务计数替代                                                 |
| `payload_revision`  | `next`                | 业务载荷的附加修订标记；Runtime 不会据此自动生成状态字节或替代版本校验           |
| `state_payload`     | 新计数的 8 字节       | `nullopt` 保留原状态；有效空 vector 明确清空状态                                 |
| `output_messages`   | 一条 CounterOut 消息  | 按源端口分别输出，适合多个输出端口                                               |
| `output_payload`    | 空                    | 无端口选择的 opaque 输出便捷路径；多源端口时有歧义，应优先使用 `output_messages` |
| `next_state`        | 空字符串              | Process 的状态跃迁目标；普通 Continuous 实例必须保持空                           |
| `process_completed` | `false`               | Process 完成标记；本例不使用                                                     |
| `render_intent`     | `nullopt`             | Render Feature 的类型化意图；省略保留上次结果，有效空意图退休之前的工作          |
| `outbox`            | 空数组                | 原子追加命令；空数组保留既有 pending，不代表取消命令                             |

返回 `Result::success()` 表示候选计算成功。Runtime 还会检查代次、版本、状态、输出及合同，
通过后才提交。返回失败时不要预先改变外部对象；失败结果不能撤销回调自行产生的外部副作用。
回调可在域 worker 上执行，业务逻辑应依赖传入快照，避免直接操作编辑器、World 或 GPU。

## 5 注册到游戏模块

### 5.1 GameModuleDefinition.hpp

DLL 和后面的源元数据工具共同调用这个工厂，避免分别维护两份合同。
在已有游戏项目接入时，把注册调用添加到现有工厂，并保留已有 Feature。

```cpp
#pragma once

#include <utility>
#include "feature_module.hpp"
#include "Features/Counter.hpp"

namespace game {

inline aeg::module::FeatureModuleDefinition make_game_module() {
    aeg::module::FeatureModuleDefinition module{};
    module.descriptor.id = aeg::module::ModuleId{"GameModule"};
    module.descriptor.name = "Counter Example Game";
    module.descriptor.version = {1, 0, 0};
    const auto result = AEG_REGISTER_GAME_FEATURE(module, make_counter_feature());
    if (!result) module.status = result;
    return module;
}

} // namespace game
```

`module.descriptor.id` 必须与当前 `Project.aegproj` 的 `[module].id` 对应。
注册 helper 会填充 `owner_module`、校验合同并导出 catalog；失败保存在 `module.status`。
新增多个 Feature 时逐一注册，遇到失败立即返回，避免发布部分成功的模块。

### 5.2 GameModule.cpp

```cpp
#include "GameModuleDefinition.hpp"
#include "module_export.hpp"

AEG_EXPORT_GAME_MODULE_API(game::make_game_module);
```

一个 DLL 保留一个标准导出入口。不要为每个 Feature 再添加一个
`AEG_EXPORT_GAME_MODULE_API`；Feature 都通过模块工厂注册。

## 6 生成模块元数据并构建

模块目录需要既有 C++ 实现，也有源 metadata。仅修改 `.flowgraph.contract.json` 或
`GameModule.metadata.json` 不会生成新的可执行行为；仅编译 DLL 也不会自动从 C++ 更新源 metadata。

本节提供一个**放在游戏项目里的开发工具示例**。它调用同一个模块工厂，再用 SDK 的正式
`encode_module_metadata()` 编码；不手算散列、不复制十进制 ID。它不进入游戏运行时。

### 6.1 WriteModuleMetadata.cpp

```cpp
#include <filesystem>
#include <fstream>
#include <iostream>

#include "GameModuleDefinition.hpp"
#include "module_bundle.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: WriteGameModuleMetadata <output.json>\n";
        return 2;
    }
    const auto definition = game::make_game_module();
    if (!definition.status) {
        std::cerr << definition.status.message << '\n';
        return 3;
    }
    aeg::module::ModuleMetadata metadata{};
    metadata.module = definition.descriptor;
    metadata.feature_catalog = definition.feature_catalog;
    metadata.build_id = "dev";
    metadata.target = "windows-x64-vulkan";
    metadata.configuration = "Debug";
    // 源声明留空；发布工具根据完整合同生成 bundle 的 contract_digest。
    const auto encoded = aeg::module::encode_module_metadata(metadata);
    if (!encoded) {
        std::cerr << encoded.result.message << '\n';
        return 4;
    }
    const std::filesystem::path path{argv[1]};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(encoded.value.data(), static_cast<std::streamsize>(encoded.value.size()));
    output.close();
    if (!output) {
        std::cerr << "Cannot write source metadata: " << path << '\n';
        return 5;
    }
    return 0;
}
```

### 6.2 游戏项目 CMakeLists.txt

下面是本例的模块和工具配置。已有模板项目可在原 `GAME_SOURCES` 中加入 Counter.cpp，
添加元数据工具，并保留 `aeg_add_game_executable()` 和原来的 GameApp.cpp 配置。

```cmake
cmake_minimum_required(VERSION 3.25)
project(CounterExample LANGUAGES CXX)

set(AEG_BUILD_TOOLS ON CACHE BOOL "Build metadata publication tools" FORCE)
include("${ENGINE_DIR}/cmake/EngineTools.cmake")

set(GAME_FEATURE_SOURCES Source/Features/Counter.cpp)

add_executable(WriteGameModuleMetadata
    Source/WriteModuleMetadata.cpp
    ${GAME_FEATURE_SOURCES})
target_link_libraries(WriteGameModuleMetadata PRIVATE AEG::GameSDK)
set_target_properties(WriteGameModuleMetadata PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Tools/$<CONFIG>")

aeg_add_game_module(GameModule
    SOURCES Source/GameModule.cpp ${GAME_FEATURE_SOURCES}
    METADATA "${CMAKE_CURRENT_SOURCE_DIR}/Source/GameModule.metadata.json"
    BUILD_ID dev
    TARGET_PLATFORM windows-x64-vulkan
    OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/Editor/windows-x64-vulkan/dev/$<CONFIG>")
```

关键参数对照：

| CMake 参数         | 含义                                                  |
|--------------------|-------------------------------------------------------|
| `ENGINE_DIR`       | 引擎源码根路径，里面应包含 `cmake/EngineTools.cmake`  |
| `SOURCES`          | 实际编译的 `.cpp`；生成文件放到目录里不会自动加入目标 |
| `METADATA`         | 开发者维护的源 metadata 路径                          |
| `BUILD_ID`         | 本次模块 bundle 的构建身份，本例为 `dev`              |
| `TARGET_PLATFORM`  | 目标平台标识，应与项目的 `[build].target` 一致        |
| `OUTPUT_DIRECTORY` | DLL、正式 metadata 和 manifest 一起发布到的目录       |

在配置好引擎所需编译环境的 PowerShell 中，进入游戏项目根目录，执行：

```powershell
$enginePath = 'E:/Codex Projects/AEG-Engine' # 改成你的引擎源码路径
cmake -S . -B Build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DENGINE_DIR=$enginePath"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build Build/Debug --target WriteGameModuleMetadata
if ($LASTEXITCODE -ne 0) { throw 'Metadata writer build failed' }
& './Build/Debug/Tools/Debug/WriteGameModuleMetadata.exe' './Source/GameModule.metadata.json'
if ($LASTEXITCODE -ne 0) { throw 'Source metadata generation failed' }
cmake --build Build/Debug --target GameModule
if ($LASTEXITCODE -ne 0) { throw 'Game module build failed' }
```

成功发布后，本例 bundle 在：

```text
Binaries/Editor/windows-x64-vulkan/dev/Debug/
├─ GameModule.dll
├─ module.metadata.json
└─ module.manifest.json
```

每次更改参数、端口、Clock、状态 schema 或定义版本后，都要从共享工厂重新生成源 metadata，
再重建模块。修改参数值只涉及图节点配置，不必重新编译 Feature。更换 Build ID、平台或配置时，
同步调整工具中的字段和构建参数；本例工具固定生成 Debug/dev 的声明。

bundle 发布挂在模块目标的 `POST_BUILD`，只有源 metadata 改动时不保证触发重链和发布。
这时应重新执行正式发布工具，或确保模块实际重新链接；不要把一次无工作可做的 build 当作
已刷新 bundle。下列是本节 Ninja 构建布局下的正式发布命令：

```powershell
$bundlePath = './Binaries/Editor/windows-x64-vulkan/dev/Debug'
& './Build/Debug/aeg-engine/aeg_module_metadata_tool.exe' `
    --module "$bundlePath/GameModule.dll" `
    --metadata './Source/GameModule.metadata.json' `
    --output "$bundlePath/module.metadata.json" `
    --manifest "$bundlePath/module.manifest.json" `
    --build-id dev --target windows-x64-vulkan --config Debug
if ($LASTEXITCODE -ne 0) { throw 'Module bundle publication failed' }
```

这条命令针对已匹配的 DLL 更新发布文件，不会把旧 DLL 变成新实现。重新开发时，如果 DLL
已被编辑器加载，关闭项目或编辑器后再覆盖旧 bundle，或者使用新的 Build ID 目录生成候选
bundle。**Stop 不会卸载已加载的 DLL。**

`aeg_module_metadata_tool` 是构建期间的**bundle 发布工具**：读取已有源 metadata、规范化并
生成摘要和 manifest，不会反射 C++ 源码补出新 Feature。仓库的
`aeg_feature_authoring_metadata_generator` 仅支持内置 `template` / `demo`，不能直接替代这里
任意游戏模块的元数据工具。不要手改 bundle 输出中的摘要或 manifest。

## 7 在编辑器中使用

1. 构建完成后重新打开游戏项目，最直接的方式是重启编辑器并显式打开 `Project.aegproj`。
   这样当前图文档会使用新的目录快照。确认 `[module].id = "GameModule"`、
   `[module].target = "GameModule"`，以及 `[build].target = "windows-x64-vulkan"`。
2. 执行 **Assets > Refresh Project Assets**（F5），让编辑器重新读取源 metadata。
   这会刷新项目作者目录，不会加载新的 DLL；已经打开的图可能仍持有旧目录快照，后续增加
   定义时也应保存后重新打开项目。在 FlowGraph 的 `Select FeatureGraph` 中选择模板
   已有的 `Assets/Features/Default.aegfeature`；已有项目选择实际要编辑的有效图。
3. 在 **Feature Catalog** 中找到 Counter，点击对应的 **Add node**。它应为 Logic / Continuous，
   Clock 为 `Game.Counter.Clock` 对应的稳定 ID。
4. 选择节点，把 `Counter` 参数组的 `Step` 改为 `5`，点击 **Apply parameters**。
   这一步将参数草稿写入当前图文档，还没有发布 Runtime。
5. 点击 **Save** 保存当前 `.aegfeature`。在 Project Settings 的 `startup_feature_graphs` 中
   确认包含这张图；模板已配置默认图，已有其他兼容启动根时保留它们。当前图面板的 Save
   保存原路径，没有 FeatureGraph 另存为按钮。
6. 使用 **Load Module**，指定上节的 `module.manifest.json` 和 `Debug` 配置。宿主会校验项目
   匹配、metadata、DLL 和真实导出合同；出现 **Review Contract Changes** 时先审阅再接受。
   随后点击 FlowGraph 的 **Apply** 发布当前图所在的完整活动批次；要按磁盘中已保存的项目启动根
   加载，可使用 **Load Saved Project**。发布在下一 Scene 帧处理，失败时查看诊断。
7. 点击 Play 推进游戏 Clock。通过 **Show ExecutionPlan** 和 **Runtime Inspect** 查看
   实际节点、实例、调用和提交状态；需要消费数值时连接下游 Feature 的兼容输入端口。

Counter 只产生计数数据，没有显示 UI、绘制像素或修改实体的行为。没有下游时也可以演示状态
提交；检查面板提供运行归属和状态信息，不应把任意业务 payload 当作自动显示的文本。

| 操作                 | 改变什么                           |
|----------------------|------------------------------------|
| `Apply parameters`   | 将所选节点的参数草稿写入作者图     |
| `Save`               | 保存 `.aegfeature` 源文档          |
| `Apply`              | 暂存发布包含当前作者图的活动图批次 |
| `Load Saved Project` | 按已保存项目的启动图集合加载       |
| `Play`               | 推进已加载游戏 Feature 的执行时钟  |

要在 C++ 工具中构造 `Step = 5` 的参数 blob，使用同一套 codec：

```cpp
const auto feature = game::make_counter_feature();
const auto payload = aeg::feature::serialize_parameter_value(
    aeg::foundation::ParameterValue{uint64_t{5}}, aeg::feature::ParameterType::kUInt32);
if (!payload) return payload.result; // 此片段放在返回 Result 的作者工具函数中。
const std::vector<aeg::feature::FeatureParameterRecord> values{
    {game::kCounterStep, 1, payload.value}
};
const auto blob = aeg::feature::encode_feature_parameters(values, feature.contract.parameters);
if (!blob) return blob.result;
// 将 blob.value 交给对应 FeatureNode 的 parameter_blob。
```

不要手拼 envelope，或把参数显示顺序当作记录顺序。编码器按 ID 规范化，回调按 ID 查找。
本例没有显式记录时，空 blob 也能经 decoder 补出默认 `Step = 1`。

## 8 从编辑器生成 Feature 骨架

Feature Definitions 提供原生合同编辑，适合从 UI 创建定义。操作流程：

1. 打开游戏项目和可编辑 FeatureGraph，在 **Feature Definitions** 点击 **New Feature**。
2. 填写名称、版本、Structure、域、Execution、State、Scope 和 Clock；所属模块来自当前项目。
   可参照第 3 节填写 Counter 的合同。
3. 在 **Parameters** 添加 Step，在端口区添加 CounterOut，填写状态 schema 和 8 字节上限。
   UI 分配的稳定 ID 应从生成定义读取；修改 Name 或 Display name 不会自动改写已有 ID。
   因此不能根据 UI 中的名称自行重新构造 `FeatureDefinitionId` 或参数 ID；回调使用生成合同
   中真实的 ID，与本例手写的字符串 ID 对应关系需显式调整。
4. **Save Draft** 保存可恢复草稿；**Save and Generate** 严格校验并生成正式文件。
5. 生成后打开 `Source/FlowGraph/` 中对应文件，实现 `execute()`。默认桩返回
   `Unsupported("Feature implementation is not ready")`，生成成功不代表业务已实现。

生成文件的职责：

| 文件                                | 用途                                                 | 开发者接下来做什么                           |
|-------------------------------------|------------------------------------------------------|----------------------------------------------|
| `<Type>.flowgraph.hpp`              | 类型声明、`definition()`、`execute()` 和注册函数声明 | 按声明实现回调                               |
| `<Type>.flowgraph.cpp`              | 合同描述区、定义工厂、待实现回调                     | 把业务放在回调及自有 helper 中，保留生成标记 |
| `<Type>.flowgraph.registration.cpp` | `register_<Type>(module)`                            | 在模块工厂中调用它                           |
| `<Type>.flowgraph.contract.json`    | 完整作者合同与编辑信息                               | 通过定义编辑流程维护，保持与 C++ 合同一致    |

将两个生成的 `.cpp` 加入游戏模块和元数据工具共用的 Feature source 列表；在共享模块工厂
include 生成头文件，然后调用 `game::register_<Type>(module)`，检查返回值。
`<Type>` 和 namespace 使用实际生成名称，不必与显示名称相同。第 6 节的元数据工具继续适用，
但工厂应调用生成的注册函数，替换手写 Counter 的注册调用。

生成器不修改 CMake、模块工厂或模块源 metadata，也不构建 DLL。生成器会维护 descriptor、
header 和 registration 的标记区；业务回调写在描述区外，避免手改下次生成会覆盖的合同。
不要在 UI 和手写 C++ 中分别维护相互冲突的两份 Definition。

## 9 扩展到输入、Process 和其他域

### 9.1 添加输入端口

在 `contract.ports` 增加一个 `kInput` 端口，指定 schema 和版本；在图中从兼容输出连接它，
再在回调里按约定协议解码 `input`。只有声明输入端口，不会自动得到数据。

图的边还声明 `EdgeKind`、源/目标 Clock、`version`、容量和 `deadline_ticks`。
Data 表达数据依赖；Snapshot 适合最新值；Queue / Event 表达排队与事件语义；Command / Receipt
需要对应事务协议。跨 Clock 或跨域不能使用 Data；Queue 的 capacity 必须大于 0，Snapshot
的 deadline_ticks 必须大于 0。

期限按发布图时保存的作者源 Clock 周期换算为时间；避免把毫秒数直接写进 `deadline_ticks`。
Receipt 继承原 Command 的期限，不按 Receipt 自己的源 Clock 重新计时。不要用业务消息中的
时间戳代替 Runtime 的交付来源证明。

### 9.2 选择结构、生命周期和状态策略

| 需求                       | 常用选择                            | 额外工作                                                               |
|----------------------------|-------------------------------------|------------------------------------------------------------------------|
| 每次明确请求计算一次       | Atomic + Call                       | 由宿主或上层调度明确触发；Call 不允许 continuation 或 ProcessState     |
| 按 Clock 持续更新          | Atomic + Continuous + InstanceState | 本例；不提交 Process 跃迁                                              |
| 跨多次执行等待、跃迁、完成 | Atomic + Process + ProcessState     | 声明 continuation，编排状态机，使用 `next_state` / `process_completed` |
| 组合多个 Feature           | Composite + Call + Stateless        | 声明 children、跨边界端口映射；子 Process 承担状态机协调               |
| 不保存本地业务状态         | Stateless 或 ExternalSnapshot       | 保留默认 state payload 合同，不得返回 `state_payload`，包括清空        |

这些维度不能任意组合。当前 Composite 必须为 Call + Stateless，不能声明 continuation；
Continuous + ProcessState 必须显式 `allows_child_process` 并承担相应服务语义，不能直接把本例的
InstanceState 换成 ProcessState。实际需要状态机时，应按 Process 模型设计。

其他合同字段在本例中保持默认值：

| 字段                          | 用途                                                            |
|-------------------------------|-----------------------------------------------------------------|
| `children`                    | Composite 展开子定义，或显式声明的 child-process service 子定义 |
| `child_port_mappings`         | 组合边界的 parent port ↔ child definition/port 及方向映射       |
| `has_continuation`            | 声明需要保留跨执行 continuation；Call 必须为 false              |
| `allows_child_process`        | 显式允许拥有子 Process 的服务模型                               |
| `failure` / `retry`           | 失败策略和最大尝试次数、tick 退避；Retry 至少声明两次尝试       |
| `side_effects`                | 声明额外副作用的 ID、版本、kind 和 commit policy                |
| `permissions.capability_mask` | 请求具体能力；由宿主验证，不能靠填写位值自行授予权限            |

### 9.3 修改 World 或提交 Render 工作

需要改实体 Transform 时，参考已有
[`DemoProject/Source/GameModule.hpp`](DemoProject/Source/GameModule.hpp) 与
[`feature_world_transform.hpp`](Engine/Feature/public/feature_world_transform.hpp)：
通过显式 World observation 输入读取快照，声明 World.Transform 命令副作用，使用
`encode_world_transform_command()` 产生 `OutboxIntent`，写入 `delta.outbox`。
宿主绑定真实 World/entity 并消费已提交命令；ScopeId("World") 本身不建立这个绑定。

自定义 outbox 命令要有匹配的 `SideEffectContract{id, version, kCommand, kOutbox}`，
并提供宿主侧 typed consumer。只声明任意 effect ID，命令不会自动被 World 应用。

Render Feature 使用 `StateDelta::render_intent` 和公开类型化值合同，同时声明 Render.Intent
副作用与所需能力。引擎将它投影到 RenderGraph；游戏 Feature 不注册 Pipeline/Stage/Pass，
不持有 RHI 句柄。Physics、Audio、Stream 同样按各域已有输入/输出协议接入，不能只改
`domains` 就获得设备或资源操作能力。详细边界见
[`Engine/Feature/README.md`](Engine/Feature/README.md)。

### 9.4 更新版本与已有数据

Definition 版本、参数 record 版本、状态 schema 版本和端口版本分别描述不同协议。
变更参数类型、删除参数、调整状态布局或端口协议时，检查旧图、保存的参数和活跃状态是否仍能
解释；不兼容变更需要显式转换、重置或重新发布。只增加一个版本号不会自动转换旧 payload。
活跃状态布局迁移使用 `StateMigration`，由宿主在统一 owner 边界安排。

## 10 常见问题与交付检查

| 现象                                    | 优先检查                                                            |
|-----------------------------------------|---------------------------------------------------------------------|
| Feature 不出现在目录                    | 是否注册、源 metadata 是否重新生成、是否加载了新 manifest           |
| 生成成功但 Play 报 Unsupported          | `execute()` 是否仍是生成桩                                          |
| DLL 构建成功但合同不匹配                | 是否共享同一工厂、重生成源 metadata 并重建 DLL；是否误加载旧 bundle |
| Step 显示正确但执行拒绝参数             | record 的 ID、版本、类型、max_size、范围是否和当前合同一致          |
| 重试或异步提交被拒绝                    | 是否使用当前 token 的 `state_version`；是否发生取消、换图或定义迁移 |
| 状态总是从零开始                        | 是否返回了 `state_payload`；是否每次都重建运行实例或重新发布图      |
| 普通 Counter 报 Process transition 错误 | 是否误填了 `next_state` 或 `process_completed`                      |
| Counter 输出没有任何可见效果            | 是否连接了真正消费计数的下游；本例本身没有显示或 World 修改         |
| Clock 准入失败                          | ID 对应声明是否一致、版本/phase/deadline 是否有效、域预算是否超限   |
| Save 后行为没改变                       | 是否完成 Apply parameters、Apply 发布，并加载了对应的新 DLL         |

开发一个新 Feature 时，至少完成：

- 定义稳定身份、版本、域、生命周期、状态策略、Scope、Clock 和权限合同。
- 声明每个参数及端口的 ID、类型或 schema、版本和大小；约定状态和消息字节协议。
- 实现正常路径、空初始状态、非法输入和数值边界，返回带当前 `expected_version` 的 Delta。
- 在共享模块工厂注册，加入游戏模块及元数据工具的 source 列表，生成源 metadata 后构建 bundle。
- 在真实图中配置参数、连接兼容端口、保存并发布，检查实际实例和业务结果。
- 更新已有协议时处理旧图、参数和状态的兼容或迁移。

## 接口参考

- [合同、回调、状态与 Delta](Engine/Feature/public/feature_contract.hpp)
- [ID、版本、Clock、端口与枚举](Engine/Feature/public/feature_types.hpp)
- [参数 envelope API](Engine/Feature/public/feature_parameters.hpp)
- [类型化参数与约束](Engine/Feature/public/feature_contract_metadata.hpp)
- [ParameterValue 的实际成员类型](Engine/Foundation/public/parameter_value.hpp)
- [PortMessage](Engine/Feature/public/feature_port_router.hpp)
- [模块注册 helper](Engine/Module/public/feature_definition_registration.hpp)
- [模块 metadata codec](Engine/Module/public/module_bundle.hpp)
- [游戏 CMake helper](cmake/aeg_game_targets.cmake)
- [编辑器操作说明](Application/Editor/README.md)
