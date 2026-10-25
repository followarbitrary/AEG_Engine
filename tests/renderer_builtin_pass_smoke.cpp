/** @file renderer_builtin_pass_smoke.cpp */
#include "feature_clock_test_fixture.hpp"
#include <initializer_list>
#include <iostream>
#include <renderer_variants_internal.hpp>
#include "../Engine/Renderer/private/renderer_feature_profiles.hpp"
#include "../Engine/Pipeline/private/render_pass_registry_execution.hpp"

namespace {
[[nodiscard]] constexpr uint64_t queue_bit(aeg::rhi::QueueClass queue) {
    return uint64_t{1} << static_cast<uint8_t>(queue);
}

[[nodiscard]] bool is_expected_pass(const aeg::pipeline::RenderPassDescriptor& descriptor,
                                    aeg::pipeline::RenderPassTypeId id, aeg::pipeline::RenderGraphPassType type,
                                    aeg::rhi::QueueClass queue) {
    return descriptor.type == id && descriptor.owner_module == aeg::pipeline::ModuleId("Renderer") &&
           descriptor.version == aeg::pipeline::Version{1, 0, 0} && descriptor.graph_type == type &&
           descriptor.allowed_queue_mask == queue_bit(queue);
}
}  // namespace

int main() {
    aeg::pipeline::RenderPassRegistry passes;
    if (!aeg::renderer::register_builtin_render_passes(passes) ||
        aeg::renderer::register_builtin_render_passes(passes)) {
        return 1;
    }
    const auto shadow = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
        passes, aeg::renderer::kShadowPassTypeId);
    const auto gbuffer = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
        passes, aeg::renderer::kGBufferPassTypeId);
    const auto deferred_lighting = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
        passes, aeg::renderer::kDeferredLightingPassTypeId);
    const auto skybox = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
        passes, aeg::renderer::kSkyboxPostProcessPassTypeId);
    const auto z_prepass = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
        passes, aeg::renderer::kZPrepassPassTypeId);
    const auto light_culling = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
        passes, aeg::renderer::kLightCullingPassTypeId);
    const auto forward_opaque = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
        passes, aeg::renderer::kForwardOpaquePassTypeId);
    if (!shadow || !gbuffer || !deferred_lighting || !skybox || !z_prepass || !light_culling || !forward_opaque) {
        return 2;
    }
    const auto graphics = aeg::pipeline::RenderGraphPassType::kGraphics;
    if (!is_expected_pass(shadow.value->descriptor(), aeg::renderer::kShadowPassTypeId, graphics,
                          aeg::rhi::QueueClass::kGraphics) ||
        !is_expected_pass(gbuffer.value->descriptor(), aeg::renderer::kGBufferPassTypeId, graphics,
                          aeg::rhi::QueueClass::kGraphics) ||
        !is_expected_pass(deferred_lighting.value->descriptor(), aeg::renderer::kDeferredLightingPassTypeId, graphics,
                          aeg::rhi::QueueClass::kGraphics) ||
        !is_expected_pass(skybox.value->descriptor(), aeg::renderer::kSkyboxPostProcessPassTypeId, graphics,
                          aeg::rhi::QueueClass::kGraphics) ||
        !is_expected_pass(z_prepass.value->descriptor(), aeg::renderer::kZPrepassPassTypeId, graphics,
                          aeg::rhi::QueueClass::kGraphics) ||
        !is_expected_pass(forward_opaque.value->descriptor(), aeg::renderer::kForwardOpaquePassTypeId, graphics,
                          aeg::rhi::QueueClass::kGraphics) ||
        !is_expected_pass(light_culling.value->descriptor(), aeg::renderer::kLightCullingPassTypeId,
                          aeg::pipeline::RenderGraphPassType::kCompute, aeg::rhi::QueueClass::kCompute)) {
        return 3;
    }
    aeg::pipeline::PipelineRegistry frame_plans;
    aeg::pipeline::RenderPassRegistry native_passes;
    if (!aeg::renderer::register_builtin_rendering(frame_plans, native_passes))
        return 41;
    aeg::feature::FeatureRegistry scene_features{aeg::feature::test::clock_test_host()};
    if (!aeg::renderer::register_scene_render_profiles(scene_features))
        return 42;
    for (const auto* name : {"Renderer.Scene.Opaque", "Renderer.Scene.Forward", "Renderer.Scene.Deferred"})
        if (scene_features.find_definition(aeg::feature::FeatureDefinitionId(name)) == nullptr)
            return 42;
    if (scene_features.find_definition(aeg::feature::FeatureDefinitionId("Renderer.OpaqueRenderFeature")) != nullptr)
        return 42;
    bool retired_variants_missing = true;
    for (const auto* retired_name : {"Renderer.ForwardRenderFeature", "Renderer.DeferredRenderFeature"}) {
        if (scene_features.find_definition(aeg::feature::FeatureDefinitionId(retired_name)) != nullptr) {
            std::cerr << "Scene registration retained retired identity " << retired_name << '\n';
            retired_variants_missing = false;
        }
    }
    if (!retired_variants_missing)
        return 43;
    return 0;
}
