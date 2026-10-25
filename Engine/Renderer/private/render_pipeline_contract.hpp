/** @file render_pipeline_contract.hpp @brief Shared renderer execution profile identities. */
#pragma once
#include <array>
#include <span>
#include <string>

#include "pipeline.hpp"
#include "renderer_variants_internal.hpp"

namespace aeg::renderer::detail {
enum class Variant { kDeferred, kForward };
inline constexpr std::array<const char*, 7> kPassNames{
    "Renderer.ShadowPass",           "Renderer.GBufferPass",
    "Renderer.DeferredLightingPass", "Renderer.SkyboxPostProcessPass",
    "Renderer.ZPrepassPass",         "Renderer.LightCullingPass",
    "Renderer.ForwardOpaquePass"};
inline constexpr std::array<const char*, 7> kPassNodes{
    "Shadow", "GBuffer", "DeferredLighting", "SkyboxPostProcess", "ZPrepass", "LightCulling", "ForwardOpaque"};
inline constexpr std::array<size_t, 4> kDeferredPasses{0, 1, 2, 3};
inline constexpr std::array<size_t, 5> kForwardPasses{0, 4, 5, 6, 3};
inline std::span<const size_t> pass_indices(Variant variant) {
    return variant == Variant::kDeferred ? std::span<const size_t>(kDeferredPasses)
                                         : std::span<const size_t>(kForwardPasses);
}
inline constexpr pipeline::ModuleId kModule{"Renderer"};
inline constexpr pipeline::Version kVersion{1, 0, 0};
inline constexpr auto kScope = pipeline::to_mask(pipeline::PipelineScope::kFrame);
inline pipeline::PipelineId pipeline_id(Variant variant) {
    return variant == Variant::kDeferred ? kDeferredRenderPipelineId : kForwardRenderPipelineId;
}
inline const char* pipeline_name(Variant variant) {
    return variant == Variant::kDeferred ? "Renderer.DeferredRenderPipeline" : "Renderer.ForwardRenderPipeline";
}
inline std::string stage_name(Variant variant, const char* stage) {
    return std::string(pipeline_name(variant)) + "." + stage;
}
}  // namespace aeg::renderer::detail
