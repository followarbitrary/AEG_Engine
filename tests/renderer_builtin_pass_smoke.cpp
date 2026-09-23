/** @file renderer_builtin_pass_smoke.cpp */
#include <renderer_variants.hpp>

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
    const auto shadow = passes.find_pass(aeg::renderer::kShadowPassTypeId);
    const auto gbuffer = passes.find_pass(aeg::renderer::kGBufferPassTypeId);
    const auto deferred_lighting = passes.find_pass(aeg::renderer::kDeferredLightingPassTypeId);
    const auto skybox = passes.find_pass(aeg::renderer::kSkyboxPostProcessPassTypeId);
    const auto z_prepass = passes.find_pass(aeg::renderer::kZPrepassPassTypeId);
    const auto light_culling = passes.find_pass(aeg::renderer::kLightCullingPassTypeId);
    const auto forward_opaque = passes.find_pass(aeg::renderer::kForwardOpaquePassTypeId);
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
    return 0;
}
