#pragma once

#include "pipeline.hpp"

namespace aeg::pipeline {

/** @brief Engine-private backend adapter for render-pass command recording. */
class RenderPassExecutionContextBackend {
public:
    virtual ~RenderPassExecutionContextBackend() = default;

    [[nodiscard]] virtual RenderGraphPass pass() const noexcept = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphBuffer> find_buffer(RenderResourceId id) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphTexture> find_texture(RenderResourceId id) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<rhi::BufferHandle> buffer(RenderGraphBuffer buffer) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<rhi::TextureHandle> texture(RenderGraphTexture texture) const = 0;
    /** @brief Resolves a view prepared before the pass execute callback. */
    [[nodiscard]] virtual foundation::ResultValue<rhi::BufferViewHandle> buffer_view(
        RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset, uint64_t size, rhi::Format format,
        uint32_t structure_stride) = 0;
    /** @brief Resolves a view prepared before the pass execute callback. */
    [[nodiscard]] virtual foundation::ResultValue<rhi::TextureViewHandle> texture_view(
        RenderGraphTexture texture, rhi::TextureViewKind kind, rhi::SubresourceRange subresources, rhi::Format format,
        rhi::TextureViewDimension dimension) = 0;
    [[nodiscard]] virtual foundation::Result begin_rendering() = 0;
    [[nodiscard]] virtual foundation::Result end_rendering() = 0;
    [[nodiscard]] virtual bool rendering_active() const noexcept = 0;
    [[nodiscard]] virtual bool rendering_started() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result set_viewport(const rhi::Viewport& viewport) = 0;
    [[nodiscard]] virtual foundation::Result set_scissor(const rhi::ScissorRect& scissor) = 0;
    [[nodiscard]] virtual foundation::Result bind_pipeline(rhi::PipelineHandle pipeline) = 0;
    [[nodiscard]] virtual foundation::Result bind_binding_set(uint32_t group, rhi::BindingSetHandle set,
                                                              std::span<const uint32_t> dynamic_offsets) = 0;
    [[nodiscard]] virtual foundation::Result set_inline_constants(uint32_t offset, rhi::ShaderStageMask shader_stages,
                                                                  std::span<const std::byte> data) = 0;
    [[nodiscard]] virtual foundation::Result bind_vertex_buffer(uint32_t binding, rhi::BufferHandle buffer,
                                                                uint64_t offset) = 0;
    [[nodiscard]] virtual foundation::Result bind_index_buffer(rhi::BufferHandle buffer, uint64_t offset,
                                                               rhi::IndexFormat format) = 0;
    [[nodiscard]] virtual foundation::Result draw(const rhi::DrawDesc& desc) = 0;
    [[nodiscard]] virtual foundation::Result draw_indexed(const rhi::DrawIndexedDesc& desc) = 0;
    [[nodiscard]] virtual foundation::Result dispatch(const rhi::DispatchDesc& desc) = 0;
    [[nodiscard]] virtual foundation::Result dispatch_indirect(const rhi::DispatchIndirectDesc& desc) = 0;
    [[nodiscard]] virtual foundation::Result copy_buffer(RenderGraphBuffer source, RenderGraphBuffer destination,
                                                         uint64_t source_offset, uint64_t destination_offset,
                                                         uint64_t size) = 0;
    [[nodiscard]] virtual foundation::Result copy_buffer_to_texture(
        RenderGraphBuffer source, uint64_t source_offset, uint32_t source_row_length, uint32_t source_image_height,
        RenderGraphTexture destination, rhi::TextureSubresourceLayers destination_subresources,
        rhi::Offset3D destination_offset, rhi::Extent3D extent) = 0;
    [[nodiscard]] virtual foundation::Result copy_texture_to_buffer(
        RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources, rhi::Offset3D source_offset,
        rhi::Extent3D extent, RenderGraphBuffer destination, uint64_t destination_offset,
        uint32_t destination_row_length, uint32_t destination_image_height) = 0;
    [[nodiscard]] virtual foundation::Result copy_texture(
        RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources, rhi::Offset3D source_offset,
        RenderGraphTexture destination, rhi::TextureSubresourceLayers destination_subresources,
        rhi::Offset3D destination_offset, rhi::Extent3D extent) = 0;
    [[nodiscard]] virtual foundation::Result clear_texture(RenderGraphTexture texture,
                                                           rhi::SubresourceRange subresources,
                                                           rhi::ClearColorValue color, float depth,
                                                           uint32_t stencil) = 0;
    [[nodiscard]] virtual foundation::Result bind_render_program(const RenderProgramRequest&,
                                                                 const RenderPassConfiguration&) {
        return foundation::unsupported("render program binding is unavailable");
    }
};

}  // namespace aeg::pipeline
