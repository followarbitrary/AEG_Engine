/** @file shadow_rendering.hpp @brief Directional shadow view and graph contracts. */
#pragma once

#include "pipeline_configuration.hpp"
#include "renderer.hpp"

namespace aeg::render_graph {
class FrameContext;
}
namespace aeg::rhi {
class Device;
}

namespace aeg::renderer {
/** @brief Reconstructs light clip coordinates from camera NDC (depth in [0,1]). */
struct RenderShadowConstants final {
    math::Mat4 clip_to_shadow{math::identity()};
    /** @brief Inverse viewport extent, depth comparison bias, reserved. */
    math::Vec4 parameters{};
};
static_assert(sizeof(RenderShadowConstants) == 80);

struct RenderShadowView final {
    RenderView view{};
    RenderShadowConstants constants{};
    bool enabled{false};
};

/** @brief Host-side resource ownership; descriptors and constants retire with their GPU frame. */
class ShadowResources final {
public:
    ShadowResources();
    ~ShadowResources();
    ShadowResources(const ShadowResources&) = delete;
    ShadowResources& operator=(const ShadowResources&) = delete;
    /** @brief The supplied forward group-three layout must outlive all submitted frames. */
    [[nodiscard]] foundation::Result initialize(rhi::Device& device, rhi::BindingLayoutHandle forward_layout);
    [[nodiscard]] foundation::ResultValue<RenderShadowResource> for_frame(render_graph::FrameContext& frame,
                                                                          const RenderShadowConstants& constants) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_{};
};

/** @brief Fits the supported directional light to all shadow caster bounds. */
[[nodiscard]] foundation::ResultValue<RenderShadowView> make_directional_shadow_view(
    const RenderSceneSnapshot& snapshot);

/** @brief Draws casters from the light view, or clears when there is no shadow light. */
[[nodiscard]] foundation::Result build_shadow_render_graph(pipeline::RenderGraphBuilder& builder,
                                                           const RenderFeatureContext& context,
                                                           const RenderShadowView& shadow,
                                                           pipeline::RenderGraphTexture depth_target,
                                                           pipeline::RenderPassInstanceId authored_instance = {});
}  // namespace aeg::renderer
