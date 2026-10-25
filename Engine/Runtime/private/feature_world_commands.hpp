/** @brief Engine-private typed command projection from accepted Feature outbox entries to World owners. */
#pragma once

#include <cstdint>
#include <memory>

#include "Components/render_components.hpp"
#include "feature_runtime.hpp"
#include "reflection.hpp"
#include "world_scope.hpp"

namespace aeg::engine::detail {

inline constexpr feature::SideEffectId kWorldTransformCommandEffect{"World.Transform"};
inline constexpr uint32_t kWorldTransformCommandVersion = 1;

struct WorldTransformCommand final {
    uint64_t expected_world_revision{0};
    components::Transform expected{};
    components::Transform replacement{};
};

[[nodiscard]] foundation::ResultValue<feature::OutboxIntent> encode_world_transform_command(
    const reflection::TypeRegistry& registry, const WorldTransformCommand& command);
[[nodiscard]] foundation::ResultValue<WorldTransformCommand> decode_world_transform_command(
    const reflection::TypeRegistry& registry, const feature::OutboxIntent& intent);

/** Explicit routes authorize one actual source/protocol to one World generation and entity. */
class FeatureWorldCommandConsumer final {
public:
    FeatureWorldCommandConsumer(feature::FeatureRuntime& runtime, const reflection::TypeRegistry& registry);
    ~FeatureWorldCommandConsumer();
    FeatureWorldCommandConsumer(const FeatureWorldCommandConsumer&) = delete;
    FeatureWorldCommandConsumer& operator=(const FeatureWorldCommandConsumer&) = delete;

    [[nodiscard]] foundation::Result bind(feature::FeatureInstanceId instance, world::WorldScope& scope,
                                          ecs::EntityId entity, feature::SideEffectId effect, uint32_t version,
                                          feature::PortId observation_port = {});
    /** Supplies the host duration separately from the invocation's logical clock quantum. */
    [[nodiscard]] foundation::Result set_frame_delta(double seconds);
    [[nodiscard]] foundation::Result consume(world::WorldScope& scope);
    /** True only when the latest consume found a real committable invocation or retry. */
    [[nodiscard]] bool observation_waiting() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

}  // namespace aeg::engine::detail
