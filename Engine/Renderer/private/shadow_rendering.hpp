/** @file shadow_rendering.hpp @brief Renderer-private directional shadow resources and fitting. */
#pragma once

#include "renderer.hpp"

namespace aeg::render_graph {
class FrameContext;
}
namespace aeg::rhi {
class Device;
}

namespace aeg::renderer {
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

}  // namespace aeg::renderer
