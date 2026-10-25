/**
 * @file renderer_variants_internal.hpp
 * @brief Renderer-only registration seams for built-in pipeline variants.
 *
 * These helpers assemble the engine's built-in execution plans. They are
 * intentionally private because fixed Pipeline/Pass registration belongs
 * to Renderer. Business definitions are registered through FeatureRegistry.
 */
#pragma once

#include <cstdint>

#include "pipeline.hpp"
#include "render_pass.hpp"

namespace aeg::renderer {

// Technical native profile identities; authors only use FeatureGraph definitions.
inline constexpr pipeline::PipelineId kDeferredRenderPipelineId{"Renderer.DeferredRenderPipeline"};
inline constexpr pipeline::PipelineId kForwardRenderPipelineId{"Renderer.ForwardRenderPipeline"};

inline constexpr pipeline::RenderPassTypeId kShadowPassTypeId{"Renderer.ShadowPass"};
inline constexpr pipeline::RenderPassTypeId kGBufferPassTypeId{"Renderer.GBufferPass"};
inline constexpr pipeline::RenderPassTypeId kDeferredLightingPassTypeId{"Renderer.DeferredLightingPass"};
inline constexpr pipeline::RenderPassTypeId kSkyboxPostProcessPassTypeId{"Renderer.SkyboxPostProcessPass"};
inline constexpr pipeline::RenderPassTypeId kZPrepassPassTypeId{"Renderer.ZPrepassPass"};
inline constexpr pipeline::RenderPassTypeId kLightCullingPassTypeId{"Renderer.LightCullingPass"};
inline constexpr pipeline::RenderPassTypeId kForwardOpaquePassTypeId{"Renderer.ForwardOpaquePass"};

[[nodiscard]] foundation::Result register_render_pipeline(pipeline::PipelineRegistry& registry);
[[nodiscard]] foundation::Result register_deferred_render_pipeline(pipeline::PipelineRegistry& registry);
[[nodiscard]] foundation::Result register_forward_render_pipeline(pipeline::PipelineRegistry& registry);
[[nodiscard]] foundation::Result register_builtin_render_passes(pipeline::RenderPassRegistry& registry);
[[nodiscard]] foundation::Result register_builtin_rendering(pipeline::PipelineRegistry& pipeline_registry,
                                                            pipeline::RenderPassRegistry& pass_registry);

}  // namespace aeg::renderer
