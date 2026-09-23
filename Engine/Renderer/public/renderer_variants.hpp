/**
 * @file renderer_variants.hpp
 * @brief Stable identities and registration seams for built-in renderer variants.
 */
#pragma once

#include <cstdint>

#include "pipeline.hpp"
#include "pipeline_authoring.hpp"
#include "pipeline_authoring_types.hpp"
#include "render_pass.hpp"
#include "renderer.hpp"

namespace aeg::renderer {

inline constexpr pipeline::PipelineId kDeferredRenderPipelineId{"Renderer.DeferredRenderPipeline"};
inline constexpr pipeline::PipelineId kForwardRenderPipelineId{"Renderer.ForwardRenderPipeline"};
inline constexpr pipeline::FeatureId kDeferredRenderFeatureId{"Renderer.DeferredRenderFeature"};
inline constexpr pipeline::FeatureId kForwardRenderFeatureId{"Renderer.ForwardRenderFeature"};

inline constexpr pipeline::RenderPassTypeId kShadowPassTypeId{"Renderer.ShadowPass"};
inline constexpr pipeline::RenderPassTypeId kGBufferPassTypeId{"Renderer.GBufferPass"};
inline constexpr pipeline::RenderPassTypeId kDeferredLightingPassTypeId{"Renderer.DeferredLightingPass"};
inline constexpr pipeline::RenderPassTypeId kSkyboxPostProcessPassTypeId{"Renderer.SkyboxPostProcessPass"};
inline constexpr pipeline::RenderPassTypeId kZPrepassPassTypeId{"Renderer.ZPrepassPass"};
inline constexpr pipeline::RenderPassTypeId kLightCullingPassTypeId{"Renderer.LightCullingPass"};
inline constexpr pipeline::RenderPassTypeId kForwardOpaquePassTypeId{"Renderer.ForwardOpaquePass"};

[[nodiscard]] foundation::Result register_deferred_render_pipeline(pipeline::PipelineRegistry& registry);
[[nodiscard]] foundation::Result register_forward_render_pipeline(pipeline::PipelineRegistry& registry);
[[nodiscard]] foundation::Result register_deferred_render_feature(pipeline::PipelineRegistry& registry);
[[nodiscard]] foundation::Result register_forward_render_feature(pipeline::PipelineRegistry& registry);
[[nodiscard]] foundation::Result bind_deferred_render_feature(pipeline::PipelineRegistry& registry,
                                                              int32_t priority = 0);
[[nodiscard]] foundation::Result bind_forward_render_feature(pipeline::PipelineRegistry& registry,
                                                             int32_t priority = 0);
[[nodiscard]] foundation::Result register_builtin_render_passes(pipeline::RenderPassRegistry& registry);
[[nodiscard]] foundation::Result register_builtin_rendering(pipeline::PipelineRegistry& pipeline_registry,
                                                            pipeline::RenderPassRegistry& pass_registry);
[[nodiscard]] foundation::Result register_renderer_authoring_types(pipeline::TypeCatalog& catalog);

}  // namespace aeg::renderer
