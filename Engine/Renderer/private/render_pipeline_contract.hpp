/** @file render_pipeline_contract.hpp @brief Shared renderer runtime/authoring identities. */
#pragma once
#include <array>
#include <span>

#include "renderer_variants.hpp"

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
inline constexpr auto kDomain = pipeline::to_mask(pipeline::FeatureDomain::kRender);
inline pipeline::PipelineId pipeline_id(Variant variant) {
    return variant == Variant::kDeferred ? kDeferredRenderPipelineId : kForwardRenderPipelineId;
}
inline pipeline::FeatureId feature_id(Variant variant) {
    return variant == Variant::kDeferred ? kDeferredRenderFeatureId : kForwardRenderFeatureId;
}
inline const char* pipeline_name(Variant variant) {
    return variant == Variant::kDeferred ? "Renderer.DeferredRenderPipeline" : "Renderer.ForwardRenderPipeline";
}
inline const char* feature_name(Variant variant) {
    return variant == Variant::kDeferred ? "Renderer.DeferredRenderFeature" : "Renderer.ForwardRenderFeature";
}
inline std::string stage_name(Variant variant, const char* stage) {
    return std::string(pipeline_name(variant)) + "." + stage;
}
inline pipeline::FeaturePointFlags point_flags(Variant variant, const char* stage) {
    using F = pipeline::FeaturePointFlags;
    F flags = F::kMayCreateArtifacts;
    if (std::string_view(stage) == "LightCulling")
        return flags | F::kMayCreateComputePass | F::kMayUseDedicatedQueue;
    if (std::string_view(stage) == "RenderExtract")
        return flags;
    flags = flags | F::kMayCreateGraphicsPass;
    if (std::string_view(stage) == "BeforePresent") {
        flags = flags | F::kMayWriteFinalOutput;
        // The built-in forward feature expands its complete graph at this anchor.
        if (variant == Variant::kForward)
            flags = flags | F::kMayCreateComputePass | F::kMayUseDedicatedQueue;
    }
    return flags;
}
[[nodiscard]] pipeline::TypeCatalogEntry make_pass_entry(size_t index);
[[nodiscard]] foundation::Result validate_pass_configuration(const pipeline::RenderPassConfiguration&, size_t index);
}  // namespace aeg::renderer::detail
