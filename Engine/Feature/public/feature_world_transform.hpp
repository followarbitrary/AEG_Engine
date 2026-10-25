/** @file feature_world_transform.hpp
 *  @brief Owning World Transform values and their versioned Feature wire protocol.
 */
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "feature_contract.hpp"

namespace aeg::feature {

inline constexpr std::string_view kWorldTransformComponentName{"Game.Transform"};
inline constexpr SideEffectId kWorldTransformCommandEffect{"World.Transform"};
inline constexpr uint32_t kWorldTransformCommandVersion = 1;
inline constexpr ValueSchemaId kWorldTransformInputSchema{"World.Transform.Observation"};
inline constexpr uint32_t kWorldTransformInputVersion = 1;
inline constexpr uint32_t kMaxWorldTransformInputBytes = 1024 * 1024;

/** Semantic components, independent of native Transform storage and padding. */
struct WorldTransformValue final {
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{0.f, 0.f, 0.f, 1.f};
    std::array<float, 3> scale{1.f, 1.f, 1.f};

    friend bool operator==(const WorldTransformValue&, const WorldTransformValue&) = default;
};

/** A conditional replacement; the target World and entity belong to the consumer binding. */
struct WorldTransformCommand final {
    uint64_t expected_world_revision{0};
    WorldTransformValue expected{};
    WorldTransformValue replacement{};
};

/** An observation made for one invocation, with independent logical and host times. */
struct WorldTransformObservation final {
    uint64_t world_revision{0};
    WorldTransformValue before{};
    double elapsed_seconds{0};
    double frame_delta_seconds{0};
};

/** Owns both the observation and the original routed Channel input. */
struct WorldTransformInput final {
    WorldTransformObservation observation{};
    std::vector<uint8_t> channel_input{};
};

/** Validates a reflected Scene component with the Runtime's Transform schema and value decoder. */
[[nodiscard]] foundation::Result validate_world_transform_component(
    std::span<const uint8_t> bytes, uint32_t schema_version);

/** Encodes the existing World.Transform v1 command using its canonical reflected schema. */
[[nodiscard]] foundation::ResultValue<OutboxIntent> encode_world_transform_command(
    const WorldTransformCommand& command);
[[nodiscard]] foundation::ResultValue<WorldTransformCommand> decode_world_transform_command(
    const OutboxIntent& intent);

/** Encodes a strict versioned observation envelope without changing its Channel bytes. */
[[nodiscard]] foundation::ResultValue<std::vector<uint8_t>> encode_world_transform_input(
    const WorldTransformInput& input);
[[nodiscard]] foundation::ResultValue<WorldTransformInput> decode_world_transform_input(
    std::span<const uint8_t> bytes);

}  // namespace aeg::feature
