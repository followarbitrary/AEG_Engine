/** @file runtime_renderer_resource_contract_smoke.cpp
 *  @brief Validates runtime shadow, clustered-light and post-process host callbacks.
 */
#include <iostream>
#include <memory>

#include "renderer.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"

namespace {
using aeg::application::runtime::RuntimeAssetResolver;
using aeg::foundation::ResultCode;

bool require(bool condition, const char* message) {
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

template <typename T>
bool not_ready(const aeg::foundation::ResultValue<T>& result) {
    return !result && result.result.code == ResultCode::kNotReady;
}
}  // namespace

int main() {
    using namespace aeg;
    RuntimeAssetResolver resolver;
    const renderer::RenderView view{};
    const renderer::RenderShadowView shadow{};
    const renderer::RenderSceneSnapshot scene{};

    if (!require(not_ready(resolver.resolve_shadow(shadow)), "missing shadow callback did not fail closed") ||
        !require(not_ready(resolver.resolve_light_culling(scene, shadow)),
                 "missing light-culling callback did not fail closed") ||
        !require(not_ready(resolver.resolve_post_process(view)), "missing post-process callback did not fail closed"))
        return 1;

    auto captured = std::make_shared<uint64_t>(0x1234u);
    const std::weak_ptr<uint64_t> captured_weak = captured;
    const renderer::RenderShadowView* seen_shadow = nullptr;
    const renderer::RenderSceneSnapshot* seen_scene = nullptr;
    const renderer::RenderShadowView* seen_culling_shadow = nullptr;
    const renderer::RenderView* seen_view = nullptr;

    RuntimeAssetResolver::UploadContext context{};
    context.shadow_resolver = [captured, &seen_shadow](const renderer::RenderShadowView& argument) {
        seen_shadow = &argument;
        renderer::RenderShadowResource resource{};
        resource.buffer = rhi::BufferHandle{*captured};
        return foundation::ResultValue<renderer::RenderShadowResource>{foundation::Result::success(), resource};
    };
    context.light_culling_resolver = [&seen_scene, &seen_culling_shadow](
                                         const renderer::RenderSceneSnapshot& argument,
                                         const renderer::RenderShadowView& shadow_argument) {
        seen_scene = &argument;
        seen_culling_shadow = &shadow_argument;
        renderer::RenderLightCullingResource resource{};
        resource.pipeline = rhi::PipelineHandle{0x44u};
        return foundation::ResultValue<renderer::RenderLightCullingResource>{foundation::Result::success(), resource};
    };
    context.post_process_resolver = [&seen_view](const renderer::RenderView& argument) {
        seen_view = &argument;
        renderer::RenderPostProcessResource resource{};
        resource.pipeline = rhi::PipelineHandle{0x55u};
        return foundation::ResultValue<renderer::RenderPostProcessResource>{foundation::Result::success(), resource};
    };
    resolver.set_upload_context(context);

    if (!require(not_ready(resolver.resolve_shadow(shadow)),
                 "incomplete shadow resource contract did not fail closed") ||
        !require(not_ready(resolver.resolve_light_culling(scene, shadow)),
                 "incomplete light-culling resource contract did not fail closed"))
        return 2;

    context.shadow_resolver = [captured, &seen_shadow](const renderer::RenderShadowView& argument) {
        seen_shadow = &argument;
        renderer::RenderShadowResource resource{};
        resource.buffer = rhi::BufferHandle{*captured};
        resource.buffer_desc.size = sizeof(renderer::RenderShadowConstants);
        resource.buffer_desc.capabilities = rhi::BufferCapability::kUniform;
        resource.prepare_inputs = [](rhi::TextureViewHandle) {
            return foundation::ResultValue<rhi::BindingSetHandle>{foundation::Result::success(),
                                                                    rhi::BindingSetHandle{0x33u}};
        };
        return foundation::ResultValue<renderer::RenderShadowResource>{foundation::Result::success(), resource};
    };
    context.light_culling_resolver = [&seen_scene, &seen_culling_shadow](
                                         const renderer::RenderSceneSnapshot& argument,
                                         const renderer::RenderShadowView& shadow_argument) {
        seen_scene = &argument;
        seen_culling_shadow = &shadow_argument;
        renderer::RenderLightCullingResource resource{};
        resource.pipeline = rhi::PipelineHandle{0x44u};
        resource.constants = rhi::BufferHandle{0x45u};
        resource.constants_desc.size = 64;
        resource.constants_desc.capabilities = rhi::BufferCapability::kUniform;
        resource.lights = rhi::BufferHandle{0x46u};
        resource.lights_desc.size = 64;
        resource.lights_desc.capabilities = rhi::BufferCapability::kStorage;
        resource.tile_count_x = 1;
        resource.tile_count_y = 1;
        resource.cluster_bytes = 4;
        resource.prepare_compute = [](rhi::TextureViewHandle, rhi::BufferHandle) {
            return foundation::ResultValue<rhi::BindingSetHandle>{foundation::Result::success(),
                                                                    rhi::BindingSetHandle{0x47u}};
        };
        resource.prepare_forward = [](rhi::TextureViewHandle, rhi::BufferHandle) {
            return foundation::ResultValue<rhi::BindingSetHandle>{foundation::Result::success(),
                                                                    rhi::BindingSetHandle{0x48u}};
        };
        resource.shader_contract = {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kLightCulling,
                                    0x8877665544332211ull, 1};
        return foundation::ResultValue<renderer::RenderLightCullingResource>{foundation::Result::success(), resource};
    };
    resolver.set_upload_context(context);

    if (!require(not_ready(resolver.resolve_post_process(view)),
                 "incomplete post-process Shader/PSO contract did not fail closed"))
        return 2;

    context.post_process_resolver = [&seen_view](const renderer::RenderView& argument) {
        seen_view = &argument;
        renderer::RenderPostProcessResource resource{};
        resource.pipeline = rhi::PipelineHandle{0x55u};
        resource.constants = rhi::BufferHandle{0x66u};
        resource.constants_desc.size = 256;
        resource.constants_desc.capabilities = rhi::BufferCapability::kUniform;
        resource.prepare_inputs = [](rhi::TextureViewHandle, rhi::TextureViewHandle) {
            return foundation::ResultValue<rhi::BindingSetHandle>{foundation::Result::success(),
                                                                    rhi::BindingSetHandle{0x77u}};
        };
        resource.profile_contract = {
            {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kPostProcess,
             0x8877665544332211ull, 1},
            {rhi::Format::kRgba8Unorm, rhi::SampleCount::kX1, rhi::Format::kUnknown},
            1};
        return foundation::ResultValue<renderer::RenderPostProcessResource>{foundation::Result::success(), resource};
    };
    resolver.set_upload_context(context);

    // The resolver borrows the host's callback contract. The caller retains
    // ownership and controls callback lifetime by clearing or replacing the
    // host before releasing captured state.
    if (!require(!captured_weak.expired(), "shadow callback capture was not retained") ||
        !require(resolver.resolve_shadow(shadow).value.buffer == rhi::BufferHandle{0x1234u},
                 "shadow callback result was not returned directly") ||
        !require(seen_shadow == &shadow, "shadow callback argument was not forwarded") ||
        !require(resolver.resolve_light_culling(scene, shadow).value.pipeline == rhi::PipelineHandle{0x44u},
                 "light-culling callback result was not returned directly") ||
        !require(seen_scene == &scene && seen_culling_shadow == &shadow,
                 "light-culling callback arguments were not forwarded") ||
        !require(resolver.resolve_post_process(view).value.pipeline == rhi::PipelineHandle{0x55u},
                 "post-process callback result was not returned directly") ||
        !require(seen_view == &view, "post-process callback argument was not forwarded"))
        return 4;

    // A profile callback borrows the host's Shader/PSO variant cache. When
    // that host epoch changes, resources published by the previous epoch are
    // retired even if their handles and local contracts still look valid.
    const uint64_t published_variant_epoch = context.variant_cache_epoch;
    context.variant_cache_epoch = published_variant_epoch + 1;
    if (!require(not_ready(resolver.resolve_light_culling(scene, shadow)),
                 "stale light-culling Shader/PSO epoch was accepted") ||
        !require(not_ready(resolver.resolve_post_process(view)),
                 "stale post-process Shader/PSO epoch was accepted"))
        return 6;
    context.variant_cache_epoch = published_variant_epoch;

    // Dropping the captured owner does not release the callback while the
    // borrowed host still contains it. Clearing the host is the explicit
    // lifetime boundary after submitted GPU work has completed.
    captured.reset();
    if (!require(!captured_weak.expired(), "resolver did not retain callback capture") ||
        !require(resolver.resolve_shadow(shadow).value.buffer == rhi::BufferHandle{0x1234u},
                 "retained shadow callback state was not usable"))
        return 5;
    context = {};
    resolver.set_upload_context({});
    if (!require(captured_weak.expired(), "clearing host context did not release callback capture") ||
        !require(not_ready(resolver.resolve_shadow(shadow)), "cleared shadow callback did not fail closed"))
        return 5;

    std::cout << "runtime renderer resource contract passed\n";
    return 0;
}
