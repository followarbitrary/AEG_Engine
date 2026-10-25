/**
 * @file render_feature_graph_builders.hpp
 * @brief Engine-owned RenderGraph projections for built-in RenderFeatures.
 *
 * These helpers are implementation seams for the renderer's built-in
 * projections. They are intentionally private: game authors publish Feature
 * contracts and do not author RenderPass or RenderGraphBuilder identities.
 */
#pragma once

#include <span>
#include <string_view>

#include "pipeline.hpp"
#include "render_feature_context.hpp"
#include "renderer.hpp"

namespace aeg::renderer {
/** Shared native resource identity/import helpers; no second renderer cache. */
[[nodiscard]] pipeline::RenderResourceId make_resource_id(uint64_t resource_namespace, uint64_t identity) noexcept;
[[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> find_or_import_buffer(
    pipeline::RenderGraphBuilder& builder, pipeline::RenderResourceId id, rhi::BufferHandle buffer,
    const rhi::BufferDesc& desc);
/** Explicit attachments and Load/Clear options for one Scene operation. */
struct OpaqueDrawOptions final {
    bool preserve_color{false};
    rhi::ClearColorValue clear_color{0.f, 0.f, 0.f, 1.f};
    RenderMaterialAttachmentContract attachment_contract{};
    pipeline::RenderGraphTexture depth_target{};
    bool preserve_depth{false};
    float clear_depth{1.f};
};

/** Shared shadow frame resources; neutral calls derive an import identity from the native constants buffer. */
[[nodiscard]] foundation::ResultValue<RenderShadowResource> prepare_shadow_resources(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, const RenderShadowView& view,
    pipeline::RenderResourceId import_id = {});

[[nodiscard]] foundation::Result build_opaque_render_graph(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, bool preserve_depth = false,
    pipeline::RenderGraphTexture shadow_map = {}, pipeline::RenderGraphBuffer light_clusters = {},
    const RenderShadowResource* shadow_resource = nullptr, const RenderLightCullingResource* light_culling = nullptr,
    pipeline::RenderGraphTexture color_override = {}, pipeline::RenderPassInstanceId authored_instance = {},
    const OpaqueDrawOptions* draw_options = nullptr);

/** Fixed MRT order and per-attachment preservation; shares the existing GBuffer recorder. */
struct GBufferDrawOptions final {
    std::array<bool, 3> preserve_colors{};
    std::array<rhi::ClearColorValue, 3> clear_colors{};
    bool preserve_depth{false};
    float clear_depth{1.f};
    RenderMaterialAttachmentContract attachment_contract{};
};
[[nodiscard]] foundation::Result build_gbuffer_render_graph(pipeline::RenderGraphBuilder& builder,
                                                            const RenderFeatureContext& context,
                                                            pipeline::RenderGraphTexture albedo_target,
                                                            pipeline::RenderGraphTexture normal_target,
                                                            pipeline::RenderGraphTexture material_target,
                                                            pipeline::RenderGraphTexture depth_target,
                                                            pipeline::RenderPassInstanceId authored_instance = {},
                                                            const GBufferDrawOptions* draw_options = nullptr);

/** Neutral calls use explicit instance spans (including empty); legacy calls keep their default selection. */
struct DepthDrawOptions final {
    bool preserve_depth{false};
    float clear_depth{1.f};
    RenderMaterialAttachmentContract attachment_contract{};
};
/** A full-screen consumer writes one HDR attachment and samples externally supplied GBuffer textures. */
struct DeferredLightingDrawOptions final {
    bool preserve_color{false};
    rhi::ClearColorValue clear_color{};
    RenderMaterialAttachmentContract attachment_contract{};
};
[[nodiscard]] foundation::Result build_deferred_lighting_render_graph(pipeline::RenderGraphBuilder& builder,
    const RenderFeatureContext& context, pipeline::RenderGraphTexture albedo, pipeline::RenderGraphTexture normal,
    pipeline::RenderGraphTexture depth, pipeline::RenderGraphTexture color, pipeline::RenderGraphTexture shadow = {},
    const RenderShadowResource* shadow_resource = nullptr, pipeline::RenderPassInstanceId authored_instance = {},
    const DeferredLightingDrawOptions* draw_options = nullptr);
[[nodiscard]] foundation::Result build_depth_render_graph(pipeline::RenderGraphBuilder& builder,
                                                          const RenderFeatureContext& context, const RenderView& view,
                                                          pipeline::RenderGraphTexture depth_target,
                                                          std::string_view pass_name = "Renderer.DepthPass",
                                                          std::span<const RenderInstance> instances = {},
                                                          pipeline::RenderPassInstanceId authored_instance = {},
                                                          const DepthDrawOptions* draw_options = nullptr);

/** One shared recorder; an explicit context without a registry creates a direct Compute pass. */
[[nodiscard]] foundation::Result build_light_culling_render_graph(pipeline::RenderGraphBuilder& builder,
                                                                  const RenderFeatureContext& context,
                                                                  pipeline::RenderGraphTexture depth,
                                                                  pipeline::RenderGraphBuffer clusters,
                                                                  RenderLightCullingResource& resource,
                                                                  pipeline::RenderPassInstanceId authored_instance = {});

[[nodiscard]] foundation::Result build_shadow_render_graph(pipeline::RenderGraphBuilder& builder,
                                                           const RenderFeatureContext& context,
                                                           const RenderShadowView& shadow,
                                                           pipeline::RenderGraphTexture depth_target,
                                                           pipeline::RenderPassInstanceId authored_instance = {});

struct PostProcessDrawOptions final {
    bool preserve_color{false};
    rhi::ClearColorValue clear_color{};
    RenderMaterialAttachmentContract attachment_contract{};
    std::optional<feature::RenderPostProcessSettings> settings{};
};
[[nodiscard]] foundation::Result build_post_process_render_graph(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderGraphTexture hdr_color,
    pipeline::RenderGraphTexture depth, pipeline::RenderGraphTexture output, rhi::ResourceUsage final_usage,
    pipeline::RenderPassInstanceId authored_instance = {}, const PostProcessDrawOptions* draw_options = nullptr);

}  // namespace aeg::renderer
