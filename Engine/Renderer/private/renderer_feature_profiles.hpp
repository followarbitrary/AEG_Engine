/** @brief Engine-owned, pure Scene Feature definitions and their FeatureGraph presets. */
#pragma once

#include "feature_graph.hpp"

namespace aeg::renderer {

enum class SceneRenderProfile { kOpaque, kForward, kDeferred };
inline constexpr feature::ClockId kSceneRenderProfileClock{"Frame"};

[[nodiscard]] foundation::ResultValue<feature::FeatureDefinition> scene_render_profile_definition(
    SceneRenderProfile profile);
/** Reserved identities are checked before any definition is registered. */
[[nodiscard]] foundation::Result register_scene_render_profiles(feature::FeatureRegistry& registry);
/** Metadata is exported from the same executable definitions used by registration. */
[[nodiscard]] foundation::ResultValue<feature::FeatureCatalog> scene_render_profile_catalog();
/** The preset digest describes the complete three-definition engine Scene catalog. */
[[nodiscard]] foundation::ResultValue<feature::FeatureGraphDocument> scene_render_profile_graph(
    SceneRenderProfile profile);

} // namespace aeg::renderer
