/** @brief GPU tile light-list construction and host-owned resource preparation. */
#pragma once

#include <filesystem>

#include "shadow_rendering.hpp"

namespace aeg::renderer {
inline constexpr uint32_t kLightCullingTileSize = 16;
/** @brief Finite local light budgets are rejected explicitly instead of silently truncating lists. */
inline constexpr uint32_t kMaxLocalLights = 1024;

class LightCullingResources final {
public:
    LightCullingResources();
    ~LightCullingResources();
    LightCullingResources(const LightCullingResources&) = delete;
    LightCullingResources& operator=(const LightCullingResources&) = delete;
    /** @brief Optional forward group-three layout is borrowed and must outlive submitted frames. */
    [[nodiscard]] foundation::Result initialize(rhi::Device& device, const std::filesystem::path& compute_package,
                                                rhi::BindingLayoutHandle forward_layout = {});
    [[nodiscard]] foundation::ResultValue<RenderLightCullingResource> for_frame(render_graph::FrameContext& frame,
                                                                                const RenderSceneSnapshot& scene,
                                                                                const RenderShadowView& shadow) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_{};
};

/** @brief Imports immutable light data and dispatches to the dedicated compute queue. */
[[nodiscard]] foundation::Result build_light_culling_render_graph(pipeline::RenderGraphBuilder& builder,
                                                                  const RenderFeatureContext& context,
                                                                  pipeline::RenderGraphTexture depth,
                                                                  pipeline::RenderGraphBuffer clusters,
                                                                  RenderLightCullingResource& resource,
                                                                  pipeline::RenderPassInstanceId authored_instance = {});
}  // namespace aeg::renderer
