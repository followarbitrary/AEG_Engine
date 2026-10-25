#pragma once
#include "feature_module.hpp"
#include "feature_render_test_fixture.hpp"

namespace dynamic_feature_test {
using namespace aeg;
using namespace aeg::feature;
inline constexpr auto kLogic = FeatureDefinitionId("Dynamic.Logic");
inline constexpr auto kProcessLogic = FeatureDefinitionId("Dynamic.ProcessLogic");
inline constexpr auto kColor = PortId("Color");
inline constexpr auto kSchema = ValueSchemaId("Dynamic.Color.Byte");

inline module::FeatureModuleDefinition definition(uint16_t revision = 1) {
    module::FeatureModuleDefinition result{};
    result.descriptor = {module::ModuleId("com.aeg.dynamic.fixture"), "Dynamic Fixture", {revision, 0, 0}, {}};
    auto render = render_feature_test::definition();
    render.owner_module = {};
    render.version = {revision, 0, 0};
    render.contract.version = render.version;
    render.contract.ports = {{kColor, PortDirection::kInput, kSchema, 1}};
    render.validate_state_payload = [](std::span<const uint8_t> value) {
        return value.size() <= 1 && (value.empty() || value.front() != 255)
            ? foundation::Result::success() : foundation::invalid_argument("DLL rejected state");
    };
    render.execute = [callback = std::move(render.execute), schema = render.contract.parameters](
        const InvocationToken& token, const StateSnapshot& state, std::span<const uint8_t> input,
        std::span<const uint8_t> parameters) {
        const auto channel = decode_feature_parameters(parameters, schema);
        if (!channel) return foundation::ResultValue<StateDelta>{channel.result, {}};
        // Snapshot edges do not wait for this tick's producer. No first value means no presentation yet.
        // Bootstrap advances Logic and Render through the async owner boundary.
        // These dedicated E2E channels intentionally tolerate the first
        // snapshot's absence and publish on the next tick when the real
        // routed message is available. Existing lifetime/Scene channels keep
        // their strict first-value behavior.
        if (input.empty() && (channel.value.front().payload.front() == 3 ||
                              channel.value.front().payload.front() == 4 ||
                              channel.value.front().payload.front() == 5))
            return foundation::ResultValue<StateDelta>{foundation::Result::success(), StateDelta{token.state_version}};
        if (input.size() != 1 || input.front() != channel.value.front().payload.front())
            return foundation::ResultValue<StateDelta>{foundation::invalid_argument("DLL Render requires routed Color"), {}};
        return callback(token, state, input, parameters);
    };
    FeatureDefinition logic{};
    logic.id = kLogic;
    logic.version = {revision, 0, 0};
    logic.contract = render.contract;
    logic.contract.definition_id = logic.id;
    logic.contract.domains = to_mask(FeatureDomain::kLogic);
    logic.contract.permissions = {0, 1};
    logic.contract.side_effects.clear();
    logic.contract.ports = {{kColor, PortDirection::kOutput, kSchema, 1}};
    logic.validate_state_payload = render.validate_state_payload;
    logic.execute = [schema = logic.contract.parameters, revision](const InvocationToken& token, const StateSnapshot& state,
        std::span<const uint8_t> input, std::span<const uint8_t> parameters) {
        if (!input.empty() && input.front() == 254) throw std::runtime_error("DLL author exception");
        if (!input.empty() && input.front() == 253)
            return foundation::ResultValue<StateDelta>{foundation::not_ready(std::string("wait\0DLL", 8)), {}};
        const auto channel = decode_feature_parameters(parameters, schema);
        if (!channel) return foundation::ResultValue<StateDelta>{channel.result, {}};
        StateDelta delta{token.state_version};
        delta.payload_revision = token.input_version;
        delta.next_state = state.current_state;
        delta.output_payload = channel.value.front().payload;
        PortMessage message{};
        message.source_port = kColor;
        message.schema = kSchema;
        message.schema_version = state.schema_version.major;
        message.sequence = token.invocation_id.value();
        message.causality_token = token.attempt_id.value();
        message.timestamp_ticks = token.generation.value();
        message.transaction_id = token.process_revision.value() + state.schema_version.minor;
        message.deadline_ticks = static_cast<uint32_t>(state.version.value() + state.schema_version.patch);
        message.payload = delta.output_payload;
        delta.output_messages = {std::move(message)};
        if (input.empty() || input.front() != 252) {
            delta.state_payload = std::vector<uint8_t>{};
            if (input.empty() || input.front() != 251)
                *delta.state_payload = {!input.empty() && input.front() == 255 ? uint8_t{255}
                    : state.payload.empty() ? uint8_t{1} : uint8_t(state.payload.front() + (revision == 1 ? 1 : 10))};
        }
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    FeatureDefinition process_logic = logic;
    process_logic.id = kProcessLogic;
    process_logic.contract.definition_id = process_logic.id;
    process_logic.contract.execution = ExecutionKind::kProcess;
    process_logic.contract.state = StatePolicy::kProcessState;
    process_logic.contract.has_continuation = true;
    const auto process_execute = std::move(process_logic.execute);
    process_logic.execute = [callback = std::move(process_execute)](const InvocationToken& token,
        const StateSnapshot& state, std::span<const uint8_t> input, std::span<const uint8_t> parameters) {
        auto result = callback(token, state, input, parameters);
        if (result) result.value.next_state.clear();
        return result;
    };
    if (!AEG_REGISTER_GAME_FEATURE(result, std::move(logic)) ||
        !AEG_REGISTER_GAME_FEATURE(result, std::move(process_logic)) ||
        !AEG_REGISTER_GAME_FEATURE(result, std::move(render)))
        throw std::runtime_error("dynamic fixture registration failed");
    return result;
}
}  // namespace dynamic_feature_test
