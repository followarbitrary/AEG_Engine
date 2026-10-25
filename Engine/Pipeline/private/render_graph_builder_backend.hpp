#pragma once

#include "pipeline.hpp"

namespace aeg::pipeline {

/** @brief Engine-private backend adapter for logical resources and pass creation. */
class RenderGraphBuilderBackend {
public:
    virtual ~RenderGraphBuilderBackend() = default;

    [[nodiscard]] virtual foundation::ResultValue<RenderGraphBuffer> create_buffer(
        RenderResourceId id, const rhi::BufferDesc& desc, RenderGraphResourceLifetime lifetime) = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphTexture> create_texture(
        RenderResourceId id, const rhi::TextureDesc& desc, RenderGraphResourceLifetime lifetime) = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphBuffer> import_buffer(
        RenderResourceId id, rhi::BufferHandle buffer, const rhi::BufferDesc& desc,
        const rhi::ResourceState& initial_state) = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphTexture> import_texture(
        RenderResourceId id, rhi::TextureHandle texture, rhi::TextureViewHandle default_view,
        const rhi::TextureDesc& desc, const rhi::ResourceState& initial_state) = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphTexture> import_swapchain_image(
        RenderResourceId id, const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
        const rhi::ResourceState& initial_state) = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphBuffer> find_buffer(RenderResourceId id) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphTexture> find_texture(RenderResourceId id) const = 0;
    [[nodiscard]] virtual foundation::Result bind_buffer(RenderResourceId id, RenderGraphBuffer buffer) = 0;
    [[nodiscard]] virtual foundation::Result bind_texture(RenderResourceId id, RenderGraphTexture texture) = 0;
    [[nodiscard]] virtual foundation::Result export_buffer(RenderGraphBuffer buffer) = 0;
    [[nodiscard]] virtual foundation::Result export_texture(RenderGraphTexture texture) = 0;
    [[nodiscard]] virtual foundation::Result retain_execution_owner(std::shared_ptr<const void> owner) = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphPass> add_pass(std::string_view name,
                                                                            RenderGraphPassType type,
                                                                            rhi::QueueClass queue,
                                                                            RenderPassSetupFunction setup,
                                                                            RenderPassExecuteFunction execute) = 0;
    [[nodiscard]] virtual foundation::Result add_dependency(RenderGraphPass source, RenderGraphPass destination) = 0;
};

}  // namespace aeg::pipeline
