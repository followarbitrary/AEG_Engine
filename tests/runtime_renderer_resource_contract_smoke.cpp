/** @file runtime_renderer_resource_contract_smoke.cpp
 *  @brief Validates runtime shadow, clustered-light and post-process host callbacks.
 */
#include <iostream>
#include <memory>

#include "renderer.hpp"
#include "runtime_asset_resolver.hpp"
#include "shadow_rendering.hpp"

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

    // The resolver owns a copy of each std::function. Its captured state must
    // remain alive while the host can still submit work using the returned
    // resource; resolver teardown only releases the callback copy and never
    // destroys host GPU handles.
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
        return 2;

    // Dropping the caller's copy does not release the resolver-owned callback.
    // Clearing the upload context does, which is the host's explicit lifetime
    // boundary after submitted GPU work has completed.
    captured.reset();
    if (!require(!captured_weak.expired(), "resolver did not retain callback capture") ||
        !require(resolver.resolve_shadow(shadow).value.buffer == rhi::BufferHandle{0x1234u},
                 "retained shadow callback state was not usable"))
        return 3;
    context = {};
    resolver.set_upload_context({});
    if (!require(captured_weak.expired(), "clearing host context did not release callback capture") ||
        !require(not_ready(resolver.resolve_shadow(shadow)), "cleared shadow callback did not fail closed"))
        return 4;

    std::cout << "runtime renderer resource contract passed\n";
    return 0;
}
