/**
 * @file light_culling_origin.hpp
 * @brief Renderer-private provenance for frame-owned light-culling resources.
 */
#pragma once

#include "frame_context.hpp"
#include "renderer_profile_resource_contract.hpp"

namespace aeg::renderer {

struct RenderSceneSnapshot;

/** @brief Native frame/device/scene provenance retained by LightCullingResources. */
struct LightCullingOrigin final {
    const render_graph::FrameContext* frame{nullptr};
    const rhi::Device* device{nullptr};
    const RenderSceneSnapshot* scene{nullptr};
    uint64_t frame_number{0};
    rhi::PipelineHandle pipeline{};
    rhi::BufferHandle constants{}, lights{};
    rhi::BufferDesc constants_desc{}, lights_desc{};
    uint32_t tile_count_x{0}, tile_count_y{0};
    uint64_t cluster_bytes{0};
    bool shadow_enabled{false};
};

/** @brief Validates a public resource value against its private provenance token. */
[[nodiscard]] bool light_culling_resource_has_origin(const RenderLightCullingResource& resource,
                                                     const render_graph::FrameContext& frame,
                                                     const rhi::Device& device,
                                                     const RenderSceneSnapshot& scene) noexcept;

}  // namespace aeg::renderer
