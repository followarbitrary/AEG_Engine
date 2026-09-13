/**
 * @file component_render_extraction.hpp
 * @brief Adapts Runtime render components to the backend-neutral Renderer API.
 */
#pragma once

#include "render_components.hpp"
#include "renderer.hpp"

namespace aeg::engine::components {
/** @brief Converts a Runtime camera entity into a validated renderer camera. */
[[nodiscard]] foundation::ResultValue<renderer::RenderCamera> make_render_camera(
    const Transform& transform, const Camera& camera, float aspect_ratio,
    math::ClipSpaceConvention clip_space_convention = {});

/** @brief Snapshot-only settings used to construct one Runtime component RenderView. */
struct RuntimeRenderViewDesc final {
    /** @brief Runtime camera entity resolved from the supplied immutable WorldSnapshot. */
    ecs::EntityId camera_entity{};
    /** @brief Registered World view slot represented by the resulting RenderView. */
    uint32_t view_id{0};
    /** @brief Pixel extent used to derive the camera aspect ratio. */
    renderer::RenderExtent output_extent{};
    /** @brief Per-view mask used by Renderer visibility filtering. */
    uint32_t visibility_mask{0xffffffffu};
    /** @brief Optional logical color output identity for RenderGraph integration. */
    pipeline::RenderResourceId color_target{};
    /** @brief Optional logical depth output identity for RenderGraph integration. */
    pipeline::RenderResourceId depth_target{};
    /** @brief Enables conservative bounds-based frustum filtering during extraction. */
    bool frustum_culling{false};
    /** @brief Projection convention used when constructing the camera matrix. */
    math::ClipSpaceConvention clip_space_convention{};
};

/** @brief Resolves Transform and Camera components from an immutable snapshot into one RenderView. */
[[nodiscard]] foundation::ResultValue<renderer::RenderView> make_render_view(const ecs::WorldSnapshot& world_snapshot,
                                                                             const RuntimeRenderViewDesc& desc);

/** @brief Extracts visible Runtime mesh components from one immutable WorldSnapshot. */
[[nodiscard]] foundation::Result extract_mesh_renderers(const renderer::RenderExtractContext& context);

/** @brief Registers the Runtime mesh extraction callback and returns its removable identity. */
[[nodiscard]] foundation::ResultValue<renderer::RenderExtractorId> register_mesh_renderer_extractor(
    renderer::Renderer& renderer);
}  // namespace aeg::engine::components
