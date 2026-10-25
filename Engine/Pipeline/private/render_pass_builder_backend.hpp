#pragma once

#include "pipeline.hpp"

namespace aeg::pipeline {

/** @brief Engine-private backend adapter for render-pass resource declarations. */
class RenderPassBuilderBackend {
public:
    virtual ~RenderPassBuilderBackend() = default;

    [[nodiscard]] virtual foundation::ResultValue<RenderGraphBuffer> find_buffer(RenderResourceId id) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphTexture> find_texture(RenderResourceId id) const = 0;

    [[nodiscard]] virtual foundation::Result read_buffer(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                         rhi::PipelineStageMask pipeline_stages,
                                                         rhi::ShaderStageMask shader_stages,
                                                         RenderGraphBufferRange range) = 0;
    [[nodiscard]] virtual foundation::Result write_buffer(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                          rhi::PipelineStageMask pipeline_stages,
                                                          rhi::ShaderStageMask shader_stages,
                                                          RenderGraphBufferRange range, bool discard_contents) = 0;
    [[nodiscard]] virtual foundation::Result read_write_buffer(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                               rhi::PipelineStageMask pipeline_stages,
                                                               rhi::ShaderStageMask shader_stages,
                                                               RenderGraphBufferRange range) = 0;
    [[nodiscard]] virtual foundation::Result read_texture(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                          rhi::PipelineStageMask pipeline_stages,
                                                          rhi::ShaderStageMask shader_stages,
                                                          rhi::SubresourceRange subresources) = 0;
    [[nodiscard]] virtual foundation::Result write_texture(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                           rhi::PipelineStageMask pipeline_stages,
                                                           rhi::ShaderStageMask shader_stages,
                                                           rhi::SubresourceRange subresources,
                                                           bool discard_contents) = 0;
    [[nodiscard]] virtual foundation::Result read_write_texture(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                                rhi::PipelineStageMask pipeline_stages,
                                                                rhi::ShaderStageMask shader_stages,
                                                                rhi::SubresourceRange subresources) = 0;
    [[nodiscard]] virtual foundation::Result use_color_attachment(RenderGraphTexture texture, rhi::LoadOp load_op,
                                                                  rhi::StoreOp store_op,
                                                                  rhi::ClearColorValue clear_color,
                                                                  rhi::ResourceUsage final_usage,
                                                                  rhi::SubresourceRange subresources) = 0;
    [[nodiscard]] virtual foundation::Result use_depth_stencil_attachment(
        RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op, float clear_depth,
        uint32_t clear_stencil, rhi::ResourceUsage final_usage, rhi::SubresourceRange subresources) = 0;
    virtual void set_side_effects(bool enabled) noexcept = 0;
    [[nodiscard]] virtual foundation::Result declare_render_program(const RenderProgramRequest&,
                                                                    const RenderPassConfiguration&) {
        return foundation::unsupported("render program preparation is unavailable");
    }
};

}  // namespace aeg::pipeline
