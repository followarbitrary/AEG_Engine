#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "feature_module.hpp"
#include "feature_world_transform.hpp"
#include "concurrency.hpp"

namespace editor_world_motion_test {

using namespace aeg;
using namespace aeg::feature;

inline constexpr module::ModuleId kModule{"com.aeg.editor.world.motion"};
inline constexpr FeatureDefinitionId kMotion{"Editor.World.Motion"};
inline constexpr FeatureNodeId kNode{"Motion"};
inline constexpr FeatureNodeId kAuthorRoot{"MotionRoot"};
inline constexpr PortId kObservationPort{"WorldObservation"};
inline constexpr ClockId kClock{"Editor.World.Motion.Clock"};

inline std::atomic_bool callback_released{true};
inline std::atomic_uint32_t callback_count{0};

inline std::vector<uint8_t> trace_payload(uint8_t count, uint64_t revision, float before_x,
                                          double elapsed, double frame_delta) {
    std::vector<uint8_t> bytes;
    bytes.reserve(29);
    bytes.push_back(count);
    const auto append_u64 = [&](uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    };
    append_u64(revision);
    const auto before_bits = std::bit_cast<uint32_t>(before_x);
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<uint8_t>(before_bits >> shift));
    append_u64(std::bit_cast<uint64_t>(elapsed));
    append_u64(std::bit_cast<uint64_t>(frame_delta));
    return bytes;
}

inline module::FeatureModuleDefinition definition() {
    module::FeatureModuleDefinition result{};
    result.descriptor = {kModule, "Editor World Motion Fixture", {1, 0, 0}, {}};

    FeatureDefinition motion{};
    motion.id = kMotion;
    motion.version = {1, 0, 0};
    auto& contract = motion.contract;
    contract.definition_id = motion.id;
    contract.version = motion.version;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kProcess;
    contract.state = StatePolicy::kProcessState;
    contract.has_continuation = true;
    contract.scope = {ScopeId("World")};
    contract.clock = aeg::feature::make_periodic_clock_contract(kClock, 60, 4, LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.ports = {{kObservationPort, PortDirection::kInput,
                       kWorldTransformInputSchema, kWorldTransformInputVersion}};
    contract.side_effects = {{kWorldTransformCommandEffect, kWorldTransformCommandVersion,
                              SideEffectKind::kCommand, CommitPolicy::kOutbox}};
    motion.execute = [](const InvocationToken& token, const StateSnapshot& state,
                        std::span<const uint8_t> input, std::span<const uint8_t>)
        -> foundation::ResultValue<StateDelta> {
        callback_count.fetch_add(1, std::memory_order_relaxed);
        while (!callback_released.load(std::memory_order_acquire)) {
            if (const auto waited = concurrency::sleep_for(1'000'000); !waited)
                return {waited, {}};
        }

        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.next_state = state.current_state;
        delta.process_completed = false;
        const uint8_t wave = state.payload.empty() ? 0 : state.payload.front();
        if (input.empty()) {
            delta.state_payload = std::vector<uint8_t>{static_cast<uint8_t>(wave + 1u)};
            return {foundation::Result::success(), std::move(delta)};
        }

        const auto observed = decode_world_transform_input(input);
        if (!observed) return {observed.result, {}};
        WorldTransformCommand command{};
        command.expected_world_revision = observed.value.observation.world_revision;
        command.expected = observed.value.observation.before;
        command.replacement = command.expected;
        command.replacement.translation[0] = wave == 0 ? 7.0f : wave == 1 ? 41.0f : 43.0f;
        const auto encoded = encode_world_transform_command(command);
        if (!encoded) return {encoded.result, {}};
        delta.outbox.push_back(encoded.value);
        delta.state_payload = trace_payload(static_cast<uint8_t>(wave + 1u),
            observed.value.observation.world_revision, observed.value.observation.before.translation[0],
            observed.value.observation.elapsed_seconds, observed.value.observation.frame_delta_seconds);
        return {foundation::Result::success(), std::move(delta)};
    };

    if (!AEG_REGISTER_GAME_FEATURE(result, std::move(motion)))
        throw std::runtime_error("Editor World Motion fixture registration failed");
    return result;
}

}  // namespace editor_world_motion_test
