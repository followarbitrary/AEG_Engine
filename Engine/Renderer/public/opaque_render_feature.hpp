/**
 * @file opaque_render_feature.hpp
 * @brief Public registration contract for the built-in opaque graphics feature.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "pipeline.hpp"
#include "result.hpp"

namespace aeg::renderer {
struct RenderFeatureContext;
struct RenderView;
struct RenderInstance;
struct RenderShadowResource;
struct RenderLightCullingResource;

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

/**
 * @brief Builds the same indexed opaque draw pass for a variant pipeline.
 *
 * The variant path can preserve a depth prepass while reusing the renderer's
 * existing mesh/material/object resource contract. The graph builder still
 * supplies the immutable RenderFeatureContext; no backend object crosses this
 * helper boundary.
 */
[[nodiscard]] foundation::Result build_opaque_render_graph(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, bool preserve_depth = false,
    pipeline::RenderGraphTexture shadow_map = {}, pipeline::RenderGraphBuffer light_clusters = {},
    const RenderShadowResource* shadow_resource = nullptr, const RenderLightCullingResource* light_culling = nullptr,
    pipeline::RenderGraphTexture color_override = {});

/** @brief Builds an indexed geometry pass that writes the three deferred targets. */
[[nodiscard]] foundation::Result build_gbuffer_render_graph(pipeline::RenderGraphBuilder& builder,
                                                            const RenderFeatureContext& context,
                                                            pipeline::RenderGraphTexture albedo_target,
                                                            pipeline::RenderGraphTexture normal_target,
                                                            pipeline::RenderGraphTexture material_target,
                                                            pipeline::RenderGraphTexture depth_target);

/** @brief Builds a depth-only indexed geometry pass for shadow/Z-prepass stages. */
[[nodiscard]] foundation::Result build_depth_render_graph(pipeline::RenderGraphBuilder& builder,
                                                          const RenderFeatureContext& context, const RenderView& view,
                                                          pipeline::RenderGraphTexture depth_target,
                                                          std::string_view pass_name = "Renderer.DepthPass",
                                                          std::span<const RenderInstance> instances = {});
}  // namespace aeg::renderer
