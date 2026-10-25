/** @brief Engine-only Artifact snapshot bridge access for World/Frame infrastructure. */
#pragma once

#include "../public/world_scope.hpp"
#include "pipeline.hpp"

namespace aeg::world::detail {
inline constexpr pipeline::ArtifactId kWorldSnapshotArtifactId{"WorldSnapshot"};
inline constexpr pipeline::ArtifactTypeId kWorldSnapshotArtifactTypeId{"WorldSnapshot"};
inline constexpr uint32_t kWorldSnapshotArtifactVersion = 1;

struct WorldSnapshotExecution final {
    [[nodiscard]] static const pipeline::ArtifactScopeBridge* bridge(const WorldFrameScope& frame) noexcept;
    [[nodiscard]] static foundation::ResultValue<std::unique_ptr<pipeline::ArtifactScopeBridge>> create_frame_bridge(
        const WorldScope& scope);
    [[nodiscard]] static foundation::Result capture_frame_bridge(const WorldScope& scope,
                                                                 pipeline::ArtifactScopeBridge& bridge);
};
} // namespace aeg::world::detail
