#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "feature_module.hpp"
#include "feature_world_transform.hpp"

namespace demo {
inline aeg::module::FeatureModuleDefinition make_module() {
    using namespace aeg;
    using namespace aeg::feature;

    FeatureDefinition motion{};
    motion.id = FeatureDefinitionId{"Demo.Logic.Motion"};
    motion.version = {1, 0, 0};
    auto& contract = motion.contract;
    contract.definition_id = motion.id;
    contract.version = motion.version;
    contract.structure = FeatureStructure::kAtomic;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kProcess;
    contract.state = StatePolicy::kProcessState;
    contract.scope = ScopeContract{ScopeId{"World"}};
    contract.clock = aeg::feature::make_periodic_clock_contract(ClockId{"Demo.Motion.Clock"}, 60, 4, LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.has_continuation = true;
    contract.ports = {{PortId{"Demo.WorldObservation"}, PortDirection::kInput,
                       kWorldTransformInputSchema, kWorldTransformInputVersion}};
    contract.side_effects = {{kWorldTransformCommandEffect, kWorldTransformCommandVersion,
                              SideEffectKind::kCommand, CommitPolicy::kOutbox}};
    motion.execute = [](const InvocationToken& token, const StateSnapshot& state,
                        std::span<const uint8_t> input, std::span<const uint8_t>)
        -> foundation::ResultValue<StateDelta> {
        if (input.empty())
            return {foundation::not_ready("Demo Motion requires an explicit World observation route"), {}};
        const auto decoded = decode_world_transform_input(input);
        if (!decoded) return {decoded.result, {}};

        float phase = 0.0f;
        if (!state.payload.empty()) {
            if (state.payload.size() != sizeof(uint32_t))
                return {foundation::invalid_argument("Demo Motion process state has an unsupported schema"), {}};
            const uint32_t bits = uint32_t(state.payload[0]) | (uint32_t(state.payload[1]) << 8u) |
                (uint32_t(state.payload[2]) << 16u) | (uint32_t(state.payload[3]) << 24u);
            phase = std::bit_cast<float>(bits);
            if (!std::isfinite(phase))
                return {foundation::invalid_argument("Demo Motion process state is not finite"), {}};
        }
        phase += static_cast<float>(decoded.value.observation.elapsed_seconds * 1.4);
        const float half_angle = std::sin(phase * 0.35f) * 0.5f;
        WorldTransformCommand command{};
        command.expected_world_revision = decoded.value.observation.world_revision;
        command.expected = decoded.value.observation.before;
        command.replacement = command.expected;
        command.replacement.rotation = {0.0f, 0.0f, half_angle,
            std::sqrt(std::max(0.0f, 1.0f - half_angle * half_angle))};
        const auto encoded = encode_world_transform_command(command);
        if (!encoded) return {encoded.result, {}};

        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.next_state = state.current_state;
        delta.process_completed = false;
        delta.outbox.push_back(encoded.value);
        const uint32_t bits = std::bit_cast<uint32_t>(phase);
        delta.state_payload = std::vector<uint8_t>{static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8u),
            static_cast<uint8_t>(bits >> 16u), static_cast<uint8_t>(bits >> 24u)};
        return {foundation::Result::success(), std::move(delta)};
    };

    aeg::module::FeatureModuleDefinition module{};
    module.descriptor = {aeg::module::ModuleId{"com.aeg.demo"}, "AEG Demo", {1, 0, 0}, {}};
    if (const auto result = AEG_REGISTER_GAME_FEATURE(module, std::move(motion)); !result)
        module.status = result;
    return module;
}
}
