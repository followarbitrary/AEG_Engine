/**
 * @file opaque_render_feature.hpp
 * @brief Public registration contract for the built-in opaque graphics feature.
 */
#pragma once

#include <cstdint>

#include "pipeline.hpp"
#include "result.hpp"

namespace aeg::renderer {
/** @brief Stable identity of the built-in opaque RenderFeature. */
inline constexpr pipeline::FeatureId kOpaqueRenderFeatureId{"Renderer.OpaqueRenderFeature"};

/**
 * @brief Registers the Experimental opaque RenderFeature implementation.
 *
 * The feature consumes RenderSceneSnapshot, RenderResourceResolver, and the
 * RenderView color-target binding supplied to RenderGraphFeatureRuntime.
 */
[[nodiscard]] foundation::Result register_opaque_render_feature(pipeline::PipelineRegistry& registry);

/** @brief Binds the opaque RenderFeature to the standard RenderPipeline.Opaque point. */
[[nodiscard]] foundation::Result bind_opaque_render_feature(pipeline::PipelineRegistry& registry, int32_t priority = 0);
}  // namespace aeg::renderer
