/**
 * @file render_feature_runtime.cpp
 * @brief Lowers committed Feature outputs and fixed native work into a concrete RenderGraph.
 */
#include "../public/render_feature_runtime.hpp"
#include "render_feature_history.hpp"
#include "render_feature_history_access.hpp"
#include "render_feature_projector.hpp"
#include "native_graph_build_session.hpp"
#include "render_graph_feature_access.hpp"
#include "render_graph_execution.hpp"
#include "render_graph_resource_access.hpp"
#include "render_scene_lowerer.hpp"
#include "frame_context.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../../Pipeline/private/render_pass_builder_backend.hpp"
#include "../../Pipeline/private/render_pass_execution_context_backend.hpp"
#include "../../Pipeline/private/render_graph_builder_backend.hpp"

namespace aeg::render_graph {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;

[[nodiscard]] ResourceLifetime convert_lifetime(pipeline::RenderGraphResourceLifetime lifetime) noexcept {
    switch (lifetime) {
        case pipeline::RenderGraphResourceLifetime::kTransient:
            return ResourceLifetime::kTransient;
        case pipeline::RenderGraphResourceLifetime::kPersistent:
            return ResourceLifetime::kPersistent;
        case pipeline::RenderGraphResourceLifetime::kImported:
            return ResourceLifetime::kImported;
    }
    return ResourceLifetime::kTransient;
}

[[nodiscard]] RenderGraphBuffer to_render_graph_buffer(pipeline::RenderGraphBuffer handle) noexcept {
    return RenderGraphBuffer{handle.index, handle.generation, handle.graph_id};
}

[[nodiscard]] RenderGraphTexture to_render_graph_texture(pipeline::RenderGraphTexture handle) noexcept {
    return RenderGraphTexture{handle.index, handle.generation, handle.graph_id};
}

[[nodiscard]] pipeline::RenderGraphBuffer to_pipeline_buffer(RenderGraphBuffer handle) noexcept {
    return pipeline::RenderGraphBuffer{handle.index, handle.generation, handle.graph_id};
}

[[nodiscard]] pipeline::RenderGraphTexture to_pipeline_texture(RenderGraphTexture handle) noexcept {
    return pipeline::RenderGraphTexture{handle.index, handle.generation, handle.graph_id};
}

[[nodiscard]] pipeline::RenderGraphPass to_pipeline_pass(RenderGraphPass handle) noexcept {
    return pipeline::RenderGraphPass{handle.index, handle.generation, handle.graph_id};
}

[[nodiscard]] RenderGraphPass to_render_graph_pass(pipeline::RenderGraphPass handle) noexcept {
    return RenderGraphPass{handle.index, handle.generation, handle.graph_id};
}

class RenderGraphBuilderBackendImpl;

class RenderPassBuilderBackendImpl final : public pipeline::RenderPassBuilderBackend {
public:
    RenderPassBuilderBackendImpl(std::shared_ptr<RenderGraphBuilderBackendImpl> owner, PassBuilder& builder) noexcept
        : owner_(std::move(owner)), builder_(builder) {}

    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> find_buffer(
        pipeline::RenderResourceId id) const override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> find_texture(
        pipeline::RenderResourceId id) const override;

    [[nodiscard]] foundation::Result read_buffer(pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                 rhi::PipelineStageMask pipeline_stages,
                                                 rhi::ShaderStageMask shader_stages,
                                                 pipeline::RenderGraphBufferRange range) override;
    [[nodiscard]] foundation::Result write_buffer(pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                  rhi::PipelineStageMask pipeline_stages,
                                                  rhi::ShaderStageMask shader_stages,
                                                  pipeline::RenderGraphBufferRange range,
                                                  bool discard_contents) override;
    [[nodiscard]] foundation::Result read_write_buffer(pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                       rhi::PipelineStageMask pipeline_stages,
                                                       rhi::ShaderStageMask shader_stages,
                                                       pipeline::RenderGraphBufferRange range) override;
    [[nodiscard]] foundation::Result read_texture(pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                  rhi::PipelineStageMask pipeline_stages,
                                                  rhi::ShaderStageMask shader_stages,
                                                  rhi::SubresourceRange subresources) override;
    [[nodiscard]] foundation::Result write_texture(pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                   rhi::PipelineStageMask pipeline_stages,
                                                   rhi::ShaderStageMask shader_stages,
                                                   rhi::SubresourceRange subresources, bool discard_contents) override;
    [[nodiscard]] foundation::Result read_write_texture(pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                        rhi::PipelineStageMask pipeline_stages,
                                                        rhi::ShaderStageMask shader_stages,
                                                        rhi::SubresourceRange subresources) override;
    [[nodiscard]] foundation::Result use_color_attachment(pipeline::RenderGraphTexture texture, rhi::LoadOp load_op,
                                                          rhi::StoreOp store_op, rhi::ClearColorValue clear_color,
                                                          rhi::ResourceUsage final_usage,
                                                          rhi::SubresourceRange subresources) override;
    [[nodiscard]] foundation::Result use_depth_stencil_attachment(
        pipeline::RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op, float clear_depth,
        uint32_t clear_stencil, rhi::ResourceUsage final_usage, rhi::SubresourceRange subresources) override;
    void set_side_effects(bool enabled) noexcept override;

private:
    std::shared_ptr<RenderGraphBuilderBackendImpl> owner_{};
    PassBuilder& builder_;
};

class RenderPassExecutionContextBackendImpl final : public pipeline::RenderPassExecutionContextBackend {
public:
    RenderPassExecutionContextBackendImpl(std::shared_ptr<RenderGraphBuilderBackendImpl> owner,
                                          PassExecutionContext& context) noexcept
        : owner_(std::move(owner)), context_(context) {}

    [[nodiscard]] pipeline::RenderGraphPass pass() const noexcept override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> find_buffer(
        pipeline::RenderResourceId id) const override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> find_texture(
        pipeline::RenderResourceId id) const override;
    [[nodiscard]] foundation::ResultValue<rhi::BufferHandle> buffer(pipeline::RenderGraphBuffer buffer) const override;
    [[nodiscard]] foundation::ResultValue<rhi::TextureHandle> texture(
        pipeline::RenderGraphTexture texture) const override;
    [[nodiscard]] foundation::ResultValue<rhi::BufferViewHandle> buffer_view(pipeline::RenderGraphBuffer buffer,
                                                                             rhi::BufferViewKind kind, uint64_t offset,
                                                                             uint64_t size, rhi::Format format,
                                                                             uint32_t structure_stride) override;
    [[nodiscard]] foundation::ResultValue<rhi::TextureViewHandle> texture_view(
        pipeline::RenderGraphTexture texture, rhi::TextureViewKind kind, rhi::SubresourceRange subresources,
        rhi::Format format, rhi::TextureViewDimension dimension) override;
    [[nodiscard]] foundation::Result begin_rendering() override;
    [[nodiscard]] foundation::Result end_rendering() override;
    [[nodiscard]] bool rendering_active() const noexcept override;
    [[nodiscard]] bool rendering_started() const noexcept override;
    [[nodiscard]] foundation::Result set_viewport(const rhi::Viewport& viewport) override;
    [[nodiscard]] foundation::Result set_scissor(const rhi::ScissorRect& scissor) override;
    [[nodiscard]] foundation::Result bind_pipeline(rhi::PipelineHandle pipeline) override;
    [[nodiscard]] foundation::Result bind_binding_set(uint32_t group, rhi::BindingSetHandle set,
                                                      std::span<const uint32_t> dynamic_offsets) override;
    [[nodiscard]] foundation::Result set_inline_constants(uint32_t offset, rhi::ShaderStageMask shader_stages,
                                                          std::span<const std::byte> data) override;
    [[nodiscard]] foundation::Result bind_vertex_buffer(uint32_t binding, rhi::BufferHandle buffer,
                                                        uint64_t offset) override;
    [[nodiscard]] foundation::Result bind_index_buffer(rhi::BufferHandle buffer, uint64_t offset,
                                                       rhi::IndexFormat format) override;
    [[nodiscard]] foundation::Result draw(const rhi::DrawDesc& desc) override;
    [[nodiscard]] foundation::Result draw_indexed(const rhi::DrawIndexedDesc& desc) override;
    [[nodiscard]] foundation::Result dispatch(const rhi::DispatchDesc& desc) override;
    [[nodiscard]] foundation::Result dispatch_indirect(const rhi::DispatchIndirectDesc& desc) override;
    [[nodiscard]] foundation::Result copy_buffer(pipeline::RenderGraphBuffer source,
                                                 pipeline::RenderGraphBuffer destination, uint64_t source_offset,
                                                 uint64_t destination_offset, uint64_t size) override;
    [[nodiscard]] foundation::Result copy_buffer_to_texture(pipeline::RenderGraphBuffer source, uint64_t source_offset,
                                                            uint32_t source_row_length, uint32_t source_image_height,
                                                            pipeline::RenderGraphTexture destination,
                                                            rhi::TextureSubresourceLayers destination_subresources,
                                                            rhi::Offset3D destination_offset,
                                                            rhi::Extent3D extent) override;
    [[nodiscard]] foundation::Result copy_texture_to_buffer(
        pipeline::RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources,
        rhi::Offset3D source_offset, rhi::Extent3D extent, pipeline::RenderGraphBuffer destination,
        uint64_t destination_offset, uint32_t destination_row_length, uint32_t destination_image_height) override;
    [[nodiscard]] foundation::Result copy_texture(pipeline::RenderGraphTexture source,
                                                  rhi::TextureSubresourceLayers source_subresources,
                                                  rhi::Offset3D source_offset, pipeline::RenderGraphTexture destination,
                                                  rhi::TextureSubresourceLayers destination_subresources,
                                                  rhi::Offset3D destination_offset, rhi::Extent3D extent) override;
    [[nodiscard]] foundation::Result clear_texture(pipeline::RenderGraphTexture texture,
                                                   rhi::SubresourceRange subresources, rhi::ClearColorValue color,
                                                   float depth, uint32_t stencil) override;

private:
    std::shared_ptr<RenderGraphBuilderBackendImpl> owner_{};
    PassExecutionContext& context_;
};

class RenderGraphBuilderBackendImpl final : public pipeline::RenderGraphBuilderBackend,
                                            public std::enable_shared_from_this<RenderGraphBuilderBackendImpl> {
public:
    explicit RenderGraphBuilderBackendImpl(RenderGraph& graph) noexcept : graph_(graph) {}

    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> create_buffer(
        pipeline::RenderResourceId id, const rhi::BufferDesc& desc,
        pipeline::RenderGraphResourceLifetime lifetime) override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> create_texture(
        pipeline::RenderResourceId id, const rhi::TextureDesc& desc,
        pipeline::RenderGraphResourceLifetime lifetime) override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> import_buffer(
        pipeline::RenderResourceId id, rhi::BufferHandle buffer, const rhi::BufferDesc& desc,
        const rhi::ResourceState& initial_state) override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> import_texture(
        pipeline::RenderResourceId id, rhi::TextureHandle texture, rhi::TextureViewHandle default_view,
        const rhi::TextureDesc& desc, const rhi::ResourceState& initial_state) override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> import_swapchain_image(
        pipeline::RenderResourceId id, const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
        const rhi::ResourceState& initial_state) override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> find_buffer(
        pipeline::RenderResourceId id) const override;
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> find_texture(
        pipeline::RenderResourceId id) const override;
    [[nodiscard]] foundation::Result bind_buffer(pipeline::RenderResourceId id,
                                                 pipeline::RenderGraphBuffer buffer) override;
    [[nodiscard]] foundation::Result bind_texture(pipeline::RenderResourceId id,
                                                  pipeline::RenderGraphTexture texture) override;
    [[nodiscard]] foundation::Result export_buffer(pipeline::RenderGraphBuffer buffer) override;
    [[nodiscard]] foundation::Result export_texture(pipeline::RenderGraphTexture texture) override;
    [[nodiscard]] foundation::Result retain_execution_owner(std::shared_ptr<const void> owner) override {
        return detail::RenderGraphFeatureGraphAccess::retain_execution_owner(graph_, std::move(owner));
    }
    void set_build_flags(pipeline::RenderGraphBuildFlags flags) noexcept {
        current_build_flags_ = flags;
    }
    [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphPass> add_pass(
        std::string_view name, pipeline::RenderGraphPassType type, rhi::QueueClass queue,
        pipeline::RenderPassSetupFunction setup, pipeline::RenderPassExecuteFunction execute) override;
    [[nodiscard]] foundation::Result add_dependency(pipeline::RenderGraphPass source,
                                                    pipeline::RenderGraphPass destination) override {
        return graph_.add_dependency(to_render_graph_pass(source), to_render_graph_pass(destination));
    }

    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> resolve_buffer(pipeline::RenderGraphBuffer buffer) const;
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> resolve_texture(
        pipeline::RenderGraphTexture texture) const;

    [[nodiscard]] foundation::Result register_initial_textures(
        std::span<const RenderGraphFeatureTextureBinding> textures);

private:
    [[nodiscard]] bool resource_id_taken(pipeline::RenderResourceId id) const noexcept;
    [[nodiscard]] foundation::Result remember_buffer(pipeline::RenderResourceId id, pipeline::RenderGraphBuffer handle);
    [[nodiscard]] foundation::Result remember_texture(pipeline::RenderResourceId id,
                                                      pipeline::RenderGraphTexture handle);

    RenderGraph& graph_;
    std::unordered_map<pipeline::RenderResourceId, pipeline::RenderGraphBuffer,
                       pipeline::StableIdHash<pipeline::RenderGraphResourceIdTag>>
        buffers_{};
    std::unordered_map<pipeline::RenderResourceId, pipeline::RenderGraphTexture,
                       pipeline::StableIdHash<pipeline::RenderGraphResourceIdTag>>
        textures_{};
    pipeline::RenderGraphBuildFlags current_build_flags_{pipeline::RenderGraphBuildFlags::kNone};

    friend class RenderPassBuilderBackendImpl;
    friend class RenderPassExecutionContextBackendImpl;
};

}  // namespace

foundation::ResultValue<pipeline::RenderGraphBuffer> RenderGraphBuilderBackendImpl::create_buffer(
    pipeline::RenderResourceId id, const rhi::BufferDesc& desc, pipeline::RenderGraphResourceLifetime lifetime) {
    if (lifetime == pipeline::RenderGraphResourceLifetime::kImported) {
        return {invalid_argument("imported buffers must use import_buffer"), {}};
    }
    if (resource_id_taken(id)) {
        return {invalid_state("render graph resource id is already registered"), {}};
    }
    auto result = graph_.create_buffer(desc, convert_lifetime(lifetime));
    if (!result) {
        return {result.result, {}};
    }
    const pipeline::RenderGraphBuffer handle = to_pipeline_buffer(result.value);
    if (const foundation::Result remember_result = remember_buffer(id, handle); !remember_result) {
        return {remember_result, {}};
    }
    return {foundation::Result::success(), handle};
}

foundation::ResultValue<pipeline::RenderGraphTexture> RenderGraphBuilderBackendImpl::create_texture(
    pipeline::RenderResourceId id, const rhi::TextureDesc& desc, pipeline::RenderGraphResourceLifetime lifetime) {
    if (lifetime == pipeline::RenderGraphResourceLifetime::kImported) {
        return {invalid_argument("imported textures must use import_texture"), {}};
    }
    if (resource_id_taken(id)) {
        return {invalid_state("render graph resource id is already registered"), {}};
    }
    auto result = graph_.create_texture(desc, convert_lifetime(lifetime));
    if (!result) {
        return {result.result, {}};
    }
    const pipeline::RenderGraphTexture handle = to_pipeline_texture(result.value);
    if (const foundation::Result remember_result = remember_texture(id, handle); !remember_result) {
        return {remember_result, {}};
    }
    return {foundation::Result::success(), handle};
}

foundation::ResultValue<pipeline::RenderGraphBuffer> RenderGraphBuilderBackendImpl::import_buffer(
    pipeline::RenderResourceId id, rhi::BufferHandle buffer, const rhi::BufferDesc& desc,
    const rhi::ResourceState& initial_state) {
    if (resource_id_taken(id)) {
        return {invalid_state("render graph resource id is already registered"), {}};
    }
    auto result = graph_.import_buffer(buffer, desc, initial_state);
    if (!result) {
        return {result.result, {}};
    }
    const pipeline::RenderGraphBuffer handle = to_pipeline_buffer(result.value);
    if (const foundation::Result remember_result = remember_buffer(id, handle); !remember_result) {
        return {remember_result, {}};
    }
    return {foundation::Result::success(), handle};
}

foundation::ResultValue<pipeline::RenderGraphTexture> RenderGraphBuilderBackendImpl::import_texture(
    pipeline::RenderResourceId id, rhi::TextureHandle texture, rhi::TextureViewHandle default_view,
    const rhi::TextureDesc& desc, const rhi::ResourceState& initial_state) {
    if (resource_id_taken(id)) {
        return {invalid_state("render graph resource id is already registered"), {}};
    }
    auto result = graph_.import_texture(texture, default_view, desc, initial_state);
    if (!result) {
        return {result.result, {}};
    }
    const pipeline::RenderGraphTexture handle = to_pipeline_texture(result.value);
    if (const foundation::Result remember_result = remember_texture(id, handle); !remember_result) {
        return {remember_result, {}};
    }
    return {foundation::Result::success(), handle};
}

foundation::ResultValue<pipeline::RenderGraphTexture> RenderGraphBuilderBackendImpl::import_swapchain_image(
    pipeline::RenderResourceId id, const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
    const rhi::ResourceState& initial_state) {
    if (resource_id_taken(id)) {
        return {invalid_state("render graph resource id is already registered"), {}};
    }
    auto result = graph_.import_swapchain_image(acquired, desc, initial_state);
    if (!result) {
        return {result.result, {}};
    }
    const pipeline::RenderGraphTexture handle = to_pipeline_texture(result.value);
    if (const foundation::Result remember_result = remember_texture(id, handle); !remember_result) {
        return {remember_result, {}};
    }
    return {foundation::Result::success(), handle};
}

foundation::ResultValue<pipeline::RenderGraphBuffer> RenderGraphBuilderBackendImpl::find_buffer(
    pipeline::RenderResourceId id) const {
    if (!id.is_valid()) {
        return {invalid_argument("render graph buffer resource id is invalid"), {}};
    }
    const auto iterator = buffers_.find(id);
    if (iterator == buffers_.end()) {
        return {not_found("render graph buffer resource id is not registered"), {}};
    }
    return {foundation::Result::success(), iterator->second};
}

foundation::ResultValue<pipeline::RenderGraphTexture> RenderGraphBuilderBackendImpl::find_texture(
    pipeline::RenderResourceId id) const {
    if (!id.is_valid()) {
        return {invalid_argument("render graph texture resource id is invalid"), {}};
    }
    const auto iterator = textures_.find(id);
    if (iterator == textures_.end()) {
        return {not_found("render graph texture resource id is not registered: " + std::to_string(id.value())), {}};
    }
    return {foundation::Result::success(), iterator->second};
}

foundation::Result RenderGraphBuilderBackendImpl::bind_buffer(pipeline::RenderResourceId id,
                                                              pipeline::RenderGraphBuffer buffer) {
    if (!id.is_valid() || !buffer.is_valid()) {
        return invalid_argument("render graph buffer binding is invalid");
    }
    if (resource_id_taken(id)) {
        return invalid_state("render graph resource id is already registered");
    }
    const foundation::ResultValue<RenderGraphBuffer> resolved = resolve_buffer(buffer);
    if (!resolved) {
        return resolved.result;
    }
    return remember_buffer(id, buffer);
}

foundation::Result RenderGraphBuilderBackendImpl::bind_texture(pipeline::RenderResourceId id,
                                                               pipeline::RenderGraphTexture texture) {
    if (!id.is_valid() || !texture.is_valid()) {
        return invalid_argument("render graph texture binding is invalid");
    }
    if (resource_id_taken(id)) {
        return invalid_state("render graph resource id is already registered");
    }
    const foundation::ResultValue<RenderGraphTexture> resolved = resolve_texture(texture);
    if (!resolved) {
        return resolved.result;
    }
    return remember_texture(id, texture);
}

foundation::Result RenderGraphBuilderBackendImpl::export_buffer(pipeline::RenderGraphBuffer buffer) {
    return graph_.export_buffer(to_render_graph_buffer(buffer));
}

foundation::Result RenderGraphBuilderBackendImpl::export_texture(pipeline::RenderGraphTexture texture) {
    return graph_.export_texture(to_render_graph_texture(texture));
}

foundation::ResultValue<pipeline::RenderGraphPass> RenderGraphBuilderBackendImpl::add_pass(
    std::string_view name, pipeline::RenderGraphPassType type, rhi::QueueClass queue,
    pipeline::RenderPassSetupFunction setup, pipeline::RenderPassExecuteFunction execute) {
    static_cast<void>(type);
    const std::shared_ptr<RenderGraphBuilderBackendImpl> self = shared_from_this();
    const pipeline::RenderGraphBuildFlags flags = current_build_flags_;

    PassSetupCallback setup_callback = [self, flags,
                                        setup = std::move(setup)](PassBuilder& builder) mutable {
        if (!setup) {
            return foundation::Result::success();
        }
        RenderPassBuilderBackendImpl pass_backend(self, builder);
        pipeline::RenderPassBuilder facade(pass_backend, flags);
        return setup(facade);
    };
    PassExecuteCallback execute_callback = [self,
                                            execute = std::move(execute)](PassExecutionContext& context) mutable {
        if (!execute) {
            return foundation::Result::success();
        }
        RenderPassExecutionContextBackendImpl pass_backend(self, context);
        pipeline::RenderPassExecutionContext facade(pass_backend);
        return execute(facade);
    };

    auto result = graph_.add_pass(name, queue, std::move(setup_callback), std::move(execute_callback));
    if (!result) {
        return {result.result, {}};
    }
    return {foundation::Result::success(), to_pipeline_pass(result.value)};
}

foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilderBackendImpl::resolve_buffer(
    pipeline::RenderGraphBuffer buffer) const {
    if (!buffer.is_valid()) {
        return {invalid_argument("render graph buffer handle is invalid"), {}};
    }
    const RenderGraphBuffer graph_buffer = to_render_graph_buffer(buffer);
    if (!detail::RenderGraphResourceAccess::contains_buffer(graph_, graph_buffer)) {
        return {not_found("render graph buffer handle does not belong to this graph"), {}};
    }
    return {foundation::Result::success(), graph_buffer};
}

foundation::ResultValue<RenderGraphTexture> RenderGraphBuilderBackendImpl::resolve_texture(
    pipeline::RenderGraphTexture texture) const {
    if (!texture.is_valid()) {
        return {invalid_argument("render graph texture handle is invalid"), {}};
    }
    const RenderGraphTexture graph_texture = to_render_graph_texture(texture);
    if (!detail::RenderGraphResourceAccess::contains_texture(graph_, graph_texture)) {
        return {not_found("render graph texture handle does not belong to this graph"), {}};
    }
    return {foundation::Result::success(), graph_texture};
}

bool RenderGraphBuilderBackendImpl::resource_id_taken(pipeline::RenderResourceId id) const noexcept {
    return id.is_valid() && (buffers_.find(id) != buffers_.end() || textures_.find(id) != textures_.end());
}

foundation::Result RenderGraphBuilderBackendImpl::remember_buffer(pipeline::RenderResourceId id,
                                                                  pipeline::RenderGraphBuffer handle) {
    if (!id.is_valid()) {
        return foundation::Result::success();
    }
    try {
        buffers_.emplace(id, handle);
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                           "render graph buffer resource index allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result RenderGraphBuilderBackendImpl::remember_texture(pipeline::RenderResourceId id,
                                                                   pipeline::RenderGraphTexture handle) {
    if (!id.is_valid()) {
        return foundation::Result::success();
    }
    try {
        textures_.emplace(id, handle);
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                           "render graph texture resource index allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result RenderGraphBuilderBackendImpl::register_initial_textures(
    std::span<const RenderGraphFeatureTextureBinding> textures) {
    try {
        textures_.reserve(textures_.size() + textures.size());
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                           "render graph feature resource index allocation failed");
    }

    for (const RenderGraphFeatureTextureBinding& binding : textures) {
        if (!binding.id.is_valid() || !binding.texture.is_valid()) {
            return invalid_argument("initial render graph texture binding is invalid");
        }
        if (!detail::RenderGraphResourceAccess::contains_texture(graph_, binding.texture)) {
            return not_found("initial render graph texture handle does not belong to this graph");
        }
        if (resource_id_taken(binding.id)) {
            return invalid_state("initial render graph resource id is already registered");
        }
        if (const foundation::Result result = remember_texture(binding.id, to_pipeline_texture(binding.texture));
            !result) {
            return result;
        }
    }
    return foundation::Result::success();
}

foundation::ResultValue<pipeline::RenderGraphBuffer> RenderPassBuilderBackendImpl::find_buffer(
    pipeline::RenderResourceId id) const {
    return owner_->find_buffer(id);
}

foundation::ResultValue<pipeline::RenderGraphTexture> RenderPassBuilderBackendImpl::find_texture(
    pipeline::RenderResourceId id) const {
    return owner_->find_texture(id);
}

foundation::Result RenderPassBuilderBackendImpl::read_buffer(pipeline::RenderGraphBuffer buffer,
                                                             rhi::ResourceUsage usage,
                                                             rhi::PipelineStageMask pipeline_stages,
                                                             rhi::ShaderStageMask shader_stages,
                                                             pipeline::RenderGraphBufferRange range) {
    const auto resolved = owner_->resolve_buffer(buffer);
    if (!resolved) {
        return resolved.result;
    }
    return builder_.read(resolved.value, usage, pipeline_stages, shader_stages,
                         RenderGraphBufferRange{range.offset, range.size});
}

foundation::Result RenderPassBuilderBackendImpl::write_buffer(
    pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage, rhi::PipelineStageMask pipeline_stages,
    rhi::ShaderStageMask shader_stages, pipeline::RenderGraphBufferRange range, bool discard_contents) {
    const auto resolved = owner_->resolve_buffer(buffer);
    if (!resolved) {
        return resolved.result;
    }
    return builder_.write(resolved.value, usage, pipeline_stages, shader_stages,
                          RenderGraphBufferRange{range.offset, range.size}, discard_contents);
}

foundation::Result RenderPassBuilderBackendImpl::read_write_buffer(pipeline::RenderGraphBuffer buffer,
                                                                   rhi::ResourceUsage usage,
                                                                   rhi::PipelineStageMask pipeline_stages,
                                                                   rhi::ShaderStageMask shader_stages,
                                                                   pipeline::RenderGraphBufferRange range) {
    const auto resolved = owner_->resolve_buffer(buffer);
    if (!resolved) {
        return resolved.result;
    }
    return builder_.read_write(resolved.value, usage, pipeline_stages, shader_stages,
                               RenderGraphBufferRange{range.offset, range.size});
}

foundation::Result RenderPassBuilderBackendImpl::read_texture(pipeline::RenderGraphTexture texture,
                                                              rhi::ResourceUsage usage,
                                                              rhi::PipelineStageMask pipeline_stages,
                                                              rhi::ShaderStageMask shader_stages,
                                                              rhi::SubresourceRange subresources) {
    const auto resolved = owner_->resolve_texture(texture);
    if (!resolved) {
        return resolved.result;
    }
    return builder_.read(resolved.value, usage, pipeline_stages, shader_stages, subresources);
}

foundation::Result RenderPassBuilderBackendImpl::write_texture(
    pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage, rhi::PipelineStageMask pipeline_stages,
    rhi::ShaderStageMask shader_stages, rhi::SubresourceRange subresources, bool discard_contents) {
    const auto resolved = owner_->resolve_texture(texture);
    if (!resolved) {
        return resolved.result;
    }
    return builder_.write(resolved.value, usage, pipeline_stages, shader_stages, subresources, discard_contents);
}

foundation::Result RenderPassBuilderBackendImpl::read_write_texture(pipeline::RenderGraphTexture texture,
                                                                    rhi::ResourceUsage usage,
                                                                    rhi::PipelineStageMask pipeline_stages,
                                                                    rhi::ShaderStageMask shader_stages,
                                                                    rhi::SubresourceRange subresources) {
    const auto resolved = owner_->resolve_texture(texture);
    if (!resolved) {
        return resolved.result;
    }
    return builder_.read_write(resolved.value, usage, pipeline_stages, shader_stages, subresources);
}

foundation::Result RenderPassBuilderBackendImpl::use_color_attachment(pipeline::RenderGraphTexture texture,
                                                                      rhi::LoadOp load_op, rhi::StoreOp store_op,
                                                                      rhi::ClearColorValue clear_color,
                                                                      rhi::ResourceUsage final_usage,
                                                                      rhi::SubresourceRange subresources) {
    const auto resolved = owner_->resolve_texture(texture);
    if (!resolved) {
        return resolved.result;
    }
    return builder_.use_color_attachment(resolved.value, load_op, store_op, clear_color, final_usage, subresources);
}

foundation::Result RenderPassBuilderBackendImpl::use_depth_stencil_attachment(
    pipeline::RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op, float clear_depth,
    uint32_t clear_stencil, rhi::ResourceUsage final_usage, rhi::SubresourceRange subresources) {
    const auto resolved = owner_->resolve_texture(texture);
    if (!resolved) {
        return resolved.result;
    }
    return builder_.use_depth_stencil_attachment(resolved.value, depth_load_op, depth_store_op, clear_depth,
                                                 clear_stencil, final_usage, subresources);
}

void RenderPassBuilderBackendImpl::set_side_effects(bool enabled) noexcept {
    builder_.set_side_effects(enabled);
}

pipeline::RenderGraphPass RenderPassExecutionContextBackendImpl::pass() const noexcept {
    return to_pipeline_pass(context_.pass());
}

foundation::ResultValue<pipeline::RenderGraphBuffer> RenderPassExecutionContextBackendImpl::find_buffer(
    pipeline::RenderResourceId id) const {
    return owner_->find_buffer(id);
}

foundation::ResultValue<pipeline::RenderGraphTexture> RenderPassExecutionContextBackendImpl::find_texture(
    pipeline::RenderResourceId id) const {
    return owner_->find_texture(id);
}

foundation::ResultValue<rhi::BufferHandle> RenderPassExecutionContextBackendImpl::buffer(
    pipeline::RenderGraphBuffer buffer) const {
    const auto resolved = owner_->resolve_buffer(buffer);
    if (!resolved) {
        return {resolved.result, {}};
    }
    return context_.resolve_buffer(resolved.value);
}

foundation::ResultValue<rhi::TextureHandle> RenderPassExecutionContextBackendImpl::texture(
    pipeline::RenderGraphTexture texture) const {
    const auto resolved = owner_->resolve_texture(texture);
    if (!resolved) {
        return {resolved.result, {}};
    }
    return context_.resolve_texture(resolved.value);
}

foundation::ResultValue<rhi::BufferViewHandle> RenderPassExecutionContextBackendImpl::buffer_view(
    pipeline::RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset, uint64_t size, rhi::Format format,
    uint32_t structure_stride) {
    const auto resolved = owner_->resolve_buffer(buffer);
    if (!resolved) {
        return {resolved.result, {}};
    }
    return context_.resolve_buffer_view(resolved.value, kind, offset, size, format, structure_stride);
}

foundation::ResultValue<rhi::TextureViewHandle> RenderPassExecutionContextBackendImpl::texture_view(
    pipeline::RenderGraphTexture texture, rhi::TextureViewKind kind, rhi::SubresourceRange subresources,
    rhi::Format format, rhi::TextureViewDimension dimension) {
    const auto resolved = owner_->resolve_texture(texture);
    if (!resolved) {
        return {resolved.result, {}};
    }
    return context_.resolve_texture_view(resolved.value, kind, subresources, format, dimension);
}

foundation::Result RenderPassExecutionContextBackendImpl::begin_rendering() {
    return context_.begin_rendering();
}

foundation::Result RenderPassExecutionContextBackendImpl::end_rendering() {
    return context_.end_rendering();
}

bool RenderPassExecutionContextBackendImpl::rendering_active() const noexcept {
    return context_.rendering_active();
}

bool RenderPassExecutionContextBackendImpl::rendering_started() const noexcept {
    return context_.rendering_started();
}

foundation::Result RenderPassExecutionContextBackendImpl::set_viewport(const rhi::Viewport& viewport) {
    return context_.set_viewport(viewport);
}

foundation::Result RenderPassExecutionContextBackendImpl::set_scissor(const rhi::ScissorRect& scissor) {
    return context_.set_scissor(scissor);
}

foundation::Result RenderPassExecutionContextBackendImpl::bind_pipeline(rhi::PipelineHandle pipeline) {
    return context_.bind_pipeline(pipeline);
}

foundation::Result RenderPassExecutionContextBackendImpl::bind_binding_set(uint32_t group, rhi::BindingSetHandle set,
                                                                           std::span<const uint32_t> dynamic_offsets) {
    return context_.bind_binding_set(group, set, dynamic_offsets);
}

foundation::Result RenderPassExecutionContextBackendImpl::set_inline_constants(uint32_t offset,
                                                                               rhi::ShaderStageMask shader_stages,
                                                                               std::span<const std::byte> data) {
    return context_.set_inline_constants(offset, shader_stages, data);
}

foundation::Result RenderPassExecutionContextBackendImpl::bind_vertex_buffer(uint32_t binding, rhi::BufferHandle buffer,
                                                                             uint64_t offset) {
    return context_.bind_vertex_buffer(binding, buffer, offset);
}

foundation::Result RenderPassExecutionContextBackendImpl::bind_index_buffer(rhi::BufferHandle buffer, uint64_t offset,
                                                                            rhi::IndexFormat format) {
    return context_.bind_index_buffer(buffer, offset, format);
}

foundation::Result RenderPassExecutionContextBackendImpl::draw(const rhi::DrawDesc& desc) {
    return context_.draw(desc);
}

foundation::Result RenderPassExecutionContextBackendImpl::draw_indexed(const rhi::DrawIndexedDesc& desc) {
    return context_.draw_indexed(desc);
}

foundation::Result RenderPassExecutionContextBackendImpl::dispatch(const rhi::DispatchDesc& desc) {
    return context_.dispatch(desc);
}

foundation::Result RenderPassExecutionContextBackendImpl::dispatch_indirect(const rhi::DispatchIndirectDesc& desc) {
    return context_.dispatch_indirect(desc);
}

foundation::Result RenderPassExecutionContextBackendImpl::copy_buffer(pipeline::RenderGraphBuffer source,
                                                                      pipeline::RenderGraphBuffer destination,
                                                                      uint64_t source_offset,
                                                                      uint64_t destination_offset, uint64_t size) {
    const auto resolved_source = owner_->resolve_buffer(source);
    if (!resolved_source) {
        return resolved_source.result;
    }
    const auto resolved_destination = owner_->resolve_buffer(destination);
    if (!resolved_destination) {
        return resolved_destination.result;
    }
    return context_.copy_buffer(resolved_source.value, resolved_destination.value, source_offset, destination_offset,
                                size);
}

foundation::Result RenderPassExecutionContextBackendImpl::copy_buffer_to_texture(
    pipeline::RenderGraphBuffer source, uint64_t source_offset, uint32_t source_row_length,
    uint32_t source_image_height, pipeline::RenderGraphTexture destination,
    rhi::TextureSubresourceLayers destination_subresources, rhi::Offset3D destination_offset, rhi::Extent3D extent) {
    const auto resolved_source = owner_->resolve_buffer(source);
    if (!resolved_source) {
        return resolved_source.result;
    }
    const auto resolved_destination = owner_->resolve_texture(destination);
    if (!resolved_destination) {
        return resolved_destination.result;
    }
    return context_.copy_buffer_to_texture(resolved_source.value, source_offset, source_row_length, source_image_height,
                                           resolved_destination.value, destination_subresources, destination_offset,
                                           extent);
}

foundation::Result RenderPassExecutionContextBackendImpl::copy_texture_to_buffer(
    pipeline::RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources, rhi::Offset3D source_offset,
    rhi::Extent3D extent, pipeline::RenderGraphBuffer destination, uint64_t destination_offset,
    uint32_t destination_row_length, uint32_t destination_image_height) {
    const auto resolved_source = owner_->resolve_texture(source);
    if (!resolved_source) {
        return resolved_source.result;
    }
    const auto resolved_destination = owner_->resolve_buffer(destination);
    if (!resolved_destination) {
        return resolved_destination.result;
    }
    return context_.copy_texture_to_buffer(resolved_source.value, source_subresources, source_offset, extent,
                                           resolved_destination.value, destination_offset, destination_row_length,
                                           destination_image_height);
}

foundation::Result RenderPassExecutionContextBackendImpl::copy_texture(
    pipeline::RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources, rhi::Offset3D source_offset,
    pipeline::RenderGraphTexture destination, rhi::TextureSubresourceLayers destination_subresources,
    rhi::Offset3D destination_offset, rhi::Extent3D extent) {
    const auto resolved_source = owner_->resolve_texture(source);
    if (!resolved_source) {
        return resolved_source.result;
    }
    const auto resolved_destination = owner_->resolve_texture(destination);
    if (!resolved_destination) {
        return resolved_destination.result;
    }
    return context_.copy_texture(resolved_source.value, source_subresources, source_offset, resolved_destination.value,
                                 destination_subresources, destination_offset, extent);
}

foundation::Result RenderPassExecutionContextBackendImpl::clear_texture(pipeline::RenderGraphTexture texture,
                                                                        rhi::SubresourceRange subresources,
                                                                        rhi::ClearColorValue color, float depth,
                                                                        uint32_t stencil) {
    const auto resolved = owner_->resolve_texture(texture);
    if (!resolved) {
        return resolved.result;
    }
    return context_.clear_texture(resolved.value, subresources, color, depth, stencil);
}

struct NativeGraphBuildSession::Impl final {
    explicit Impl(RenderGraph& target) noexcept : graph(target) {}
    ~Impl() {
        if (transaction_active) detail::RenderGraphFeatureGraphAccess::rollback_feature_build_transaction(graph);
    }

    RenderGraph& graph;
    bool transaction_active{false};
    std::shared_ptr<RenderGraphBuilderBackendImpl> backend{};
    std::unique_ptr<pipeline::RenderGraphBuilder> builder{};
};

NativeGraphBuildSession::NativeGraphBuildSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
NativeGraphBuildSession::~NativeGraphBuildSession() = default;

foundation::ResultValue<std::unique_ptr<NativeGraphBuildSession>> NativeGraphBuildSession::begin(
    RenderGraph& graph, std::span<const RenderGraphFeatureTextureBinding> textures,
    pipeline::RenderGraphBuildFlags flags, const void* context_data) {
    try {
        auto impl = std::make_unique<Impl>(graph);
        if (const auto result = detail::RenderGraphFeatureGraphAccess::begin_feature_build_transaction(graph); !result)
            return {result, {}};
        impl->transaction_active = true;
        impl->backend = std::make_shared<RenderGraphBuilderBackendImpl>(graph);
        if (const auto result = impl->backend->register_initial_textures(textures); !result) return {result, {}};
        impl->backend->set_build_flags(flags);
        pipeline::RenderGraphBuildContext context{};
        context.flags = flags;
        context.context_data = context_data;
        impl->builder = std::make_unique<pipeline::RenderGraphBuilder>(*impl->backend, context);
        return {foundation::Result::success(),
            std::unique_ptr<NativeGraphBuildSession>(new NativeGraphBuildSession(std::move(impl)))};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("native graph build session allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
            "native graph build session threw an unknown exception"), {}};
    }
}

pipeline::RenderGraphBuilder& NativeGraphBuildSession::builder() noexcept { return *impl_->builder; }

void NativeGraphBuildSession::commit() noexcept {
    if (impl_->transaction_active) {
        detail::RenderGraphFeatureGraphAccess::commit_feature_build_transaction(impl_->graph);
        impl_->transaction_active = false;
    }
}

foundation::Result build_render_graph_features(RenderGraph& graph,
    std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> outputs,
    const RenderGraphFeatureRuntimeDesc& desc, uint64_t frame_index, const void* history_owner) {
    auto* history_host = const_cast<RenderFeatureHistory*>(static_cast<const RenderFeatureHistory*>(history_owner));
    if (!outputs) return invalid_argument("Feature render output snapshot is null");
    const auto projected = RenderFeatureProjector{}.project(*outputs, frame_index);
    if (!projected) return projected.result;
    const auto execution_plan = RenderFeatureProjector{}.plan(*outputs);
    if (!execution_plan) return execution_plan.result;
    if (const auto result = detail::RenderGraphFeatureGraphAccess::begin_feature_build_transaction(graph); !result) return result;
    const auto* draw_lowerer = static_cast<const RenderSceneDrawLowerer*>(desc.draw_lowerer);
    const auto* current_frame = static_cast<const FrameContext*>(desc.current_frame_owner);
    const auto rollback = [&](foundation::Result result) {
        detail::RenderGraphFeatureGraphAccess::rollback_feature_build_transaction(graph);
        return result;
    };
    try {
        if (!outputs->empty()) {
            if (const auto result = detail::RenderGraphFeatureGraphAccess::retain_execution_owner(graph, outputs); !result) return rollback(result);
        }
        if (history_host && !detail::RenderFeatureHistoryRuntimeAccess::uses_device(
                                  *history_host, detail::RenderGraphExecution::device(graph)))
            return rollback(invalid_argument("Feature history and graph must share a device"));
        const auto history = history_host && !outputs->empty()
            ? detail::RenderFeatureHistoryRuntimeAccess::stage(*history_host,
                                                               RenderFeatureHistoryKey{desc.history_key})
            : nullptr;
        std::vector<size_t> work_offsets(outputs->size());
        const auto build_identity = std::make_shared<const RenderFeatureBuildIdentity>();
        const auto build_scope = [&](pipeline::RenderGraphTexture texture) {
            return RenderFeatureBuildScope{build_identity, &detail::RenderGraphExecution::device(graph), current_frame,
                                           frame_index, texture.graph_id};
        };
        std::vector<std::vector<std::shared_ptr<const RenderLightListLease>>> light_lists(outputs->size());
        std::vector<RenderSceneDepthProduction> depth_productions;
        uint64_t depth_contents_revision = 0;
        const auto depth_production = [&](pipeline::RenderGraphTexture texture) -> const RenderSceneDepthProduction* {
            const auto found = std::find_if(depth_productions.begin(), depth_productions.end(), [&](const auto& production) {
                return detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(texture), to_render_graph_texture(production.texture));
            });
            return found == depth_productions.end() ? nullptr : &*found;
        };
        const auto invalidate_depth = [&](pipeline::RenderGraphTexture texture) {
            std::erase_if(depth_productions, [&](const auto& production) {
                return detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(texture), to_render_graph_texture(production.texture));
            });
        };
        const auto record_depth = [&](const feature::RenderSceneReference& scene, const feature::RenderDepthAttachment& depth,
            pipeline::RenderGraphTexture texture, feature::RenderDepthSceneProjection projection,
            std::shared_ptr<const void> selected_snapshot) {
            const auto scope = build_scope(texture);
            const auto* previous = depth_production(texture);
            const bool preserves = previous && previous->matches(scope, scene, depth.target, texture, projection, selected_snapshot.get());
            auto contents_revision = preserves ? previous->contents_revision : 0;
            if (depth.load == feature::RenderAttachmentLoad::kClear)
                contents_revision = ++depth_contents_revision;
            invalidate_depth(texture);
            if (depth.load == feature::RenderAttachmentLoad::kClear || preserves)
                depth_productions.push_back({scope, scene, depth.target, texture, projection, std::move(selected_snapshot), contents_revision});
        };
        size_t work_count = 0;
        for (size_t index = 0; index < outputs->size(); ++index) {
            work_offsets[index] = work_count;
            light_lists[index].resize(outputs->at(index).intent.operations.size());
            work_count += outputs->at(index).intent.operations.size();
        }
        std::vector<RenderGraphTexture> referenced_imports;
        const auto register_import = [&](RenderGraphTexture handle) {
            for (const auto previous_handle : referenced_imports) {
                if (previous_handle == handle) return foundation::Result::success();
                if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, previous_handle, handle))
                    return invalid_argument("Feature bindings import one native image as different graph resources");
            }
            referenced_imports.push_back(handle);
            return foundation::Result::success();
        };
        for (const auto output_index : execution_plan.value.output_order) {
            const auto& output = outputs->at(output_index);
            size_t work_index = work_offsets[output_index];
            auto backend = std::make_shared<RenderGraphBuilderBackendImpl>(graph);
            std::vector<RenderGraphFeatureTextureBinding> bindings;
            for (const auto& binding : desc.textures)
                if (!binding.owner.is_valid() || binding.owner == output.instance_id) bindings.push_back(binding);
            if (const auto result = backend->register_initial_textures(bindings); !result) return rollback(result);
            const auto capabilities = output.contract.permissions.capability_mask;
            const auto allows = [&](feature::RenderIntentCapability flag) { return (capabilities & feature::to_mask(flag)) != 0; };
            auto flags = pipeline::RenderGraphBuildFlags::kNone;
            if (allows(feature::RenderIntentCapability::kCreateTransient) || allows(feature::RenderIntentCapability::kCreateHistory) ||
                allows(feature::RenderIntentCapability::kDrawScene) || allows(feature::RenderIntentCapability::kCullLights))
                flags = flags | pipeline::RenderGraphBuildFlags::kMayCreateResources;
            if (allows(feature::RenderIntentCapability::kClearColor) || allows(feature::RenderIntentCapability::kCopyTexture))
                flags = flags | pipeline::RenderGraphBuildFlags::kMayCreateCopyPass;
            if (allows(feature::RenderIntentCapability::kClearColor) || allows(feature::RenderIntentCapability::kDrawScene))
                flags = flags | pipeline::RenderGraphBuildFlags::kMayCreateGraphicsPass;
            if (allows(feature::RenderIntentCapability::kCullLights))
                flags = flags | pipeline::RenderGraphBuildFlags::kMayCreateComputePass |
                    pipeline::RenderGraphBuildFlags::kMayUseDedicatedQueue;
            if (allows(feature::RenderIntentCapability::kWriteExternal) || allows(feature::RenderIntentCapability::kExportOutput))
                flags = flags | pipeline::RenderGraphBuildFlags::kMayWriteFinalOutput;
            backend->set_build_flags(flags);
            pipeline::RenderGraphBuildContext context{};
            context.flags = flags;
            pipeline::RenderGraphBuilder builder(*backend, context);
            struct LocalTexture {
                feature::RenderTextureDeclaration declaration;
                pipeline::RenderGraphTexture texture{};
                bool staged_write{false};
            };
            std::unordered_map<feature::RenderResourceId, LocalTexture,
                               feature::StableIdHash<feature::RenderResourceIdTag>> local;
            if (history)
                if (const auto result = history->retain_declarations(output); !result) return rollback(result);
            for (const auto& texture : output.intent.textures) {
                if (texture.lifetime == feature::RenderTextureLifetime::kHistory) {
                    if (!history) return rollback(invalid_state("Feature history requires a runtime history store"));
                    if (const auto result = history->declare(output.resource_owner, texture); !result) return rollback(result);
                    local.emplace(texture.id, LocalTexture{texture});
                    continue;
                }
                rhi::TextureDesc texture_desc{};
                texture_desc.extent = {texture.width, texture.height, 1};
                texture_desc.format = rhi::Format::kRgba8Unorm;
                texture_desc.capabilities = rhi::TextureCapability::kTransferSource | rhi::TextureCapability::kTransferDestination;
                texture_desc.name = rhi::DebugName{"Feature.TransientColor"};
                const auto created = builder.create_texture(texture_desc);
                if (!created) return rollback(created.result);
                local.emplace(texture.id, LocalTexture{texture, created.value});
            }
            pipeline::RenderGraphPass previous{};
            const auto add_copy = [&](const std::string& name, pipeline::RenderGraphTexture source,
                                      pipeline::RenderGraphTexture destination, rhi::Extent3D extent, bool discard) {
                return builder.add_copy_pass(name,
                    [source, destination, discard](pipeline::RenderPassBuilder& pass) {
                        if (const auto result = pass.read(source, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy,
                                                         rhi::ShaderStage::kAll, {rhi::Aspect::kColor, 0, 1, 0, 1}); !result)
                            return result;
                        return pass.write(destination, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                          rhi::ShaderStage::kAll, {rhi::Aspect::kColor, 0, 1, 0, 1}, discard);
                    },
                    [source, destination, extent](pipeline::RenderPassExecutionContext& command) {
                        return command.copy_texture(source, {rhi::Aspect::kColor, 0, 0, 1}, {}, destination,
                                                    {rhi::Aspect::kColor, 0, 0, 1}, {}, extent);
                    }, rhi::QueueClass::kGraphics);
            };
            const auto import_history = [&](const RenderHistoryAllocation& allocation)
                -> foundation::ResultValue<pipeline::RenderGraphTexture> {
                const auto imported = graph.import_texture(allocation.texture, {}, allocation.description, {}, allocation.owner);
                if (!imported) return {imported.result, {}};
                if (const auto result = detail::RenderGraphFeatureGraphAccess::retain_execution_owner(graph, allocation.owner); !result) return {result, {}};
                if (const auto result = register_import(imported.value); !result) return {result, {}};
                return {foundation::Result::success(), to_pipeline_texture(imported.value)};
            };
            enum class AttachmentRole { kLdrColor, kDepth, kFloatColor };
            const auto resolve = [&](const feature::RenderTextureReference& reference, bool write = false,
                                      bool full_overwrite = false, const RenderWorkItem* work = nullptr,
                                      AttachmentRole role = AttachmentRole::kLdrColor)
                -> foundation::ResultValue<pipeline::RenderGraphTexture> {
                if (!reference.external) {
                    const auto found = local.find(reference.id);
                    if (found == local.end()) return {not_found("local Feature render resource is missing"), {}};
                    auto& resource = found->second;
                    if (resource.declaration.lifetime == feature::RenderTextureLifetime::kTransient)
                        return {foundation::Result::success(), resource.texture};
                    if (!write || resource.staged_write) {
                        if (!resource.texture.is_valid()) {
                            const auto allocation = history->read(output.resource_owner, reference.id);
                            if (!allocation) return {allocation.result, {}};
                            const auto imported = import_history(allocation.value);
                            if (!imported) return imported;
                            resource.texture = imported.value;
                        }
                        return {foundation::Result::success(), resource.texture};
                    }
                    pipeline::RenderGraphTexture previous_contents{};
                    if (!full_overwrite) {
                        previous_contents = resource.texture;
                        if (!previous_contents.is_valid()) {
                            const auto allocation = history->read(output.resource_owner, reference.id);
                            if (!allocation) return {allocation.result, {}};
                            const auto imported = import_history(allocation.value);
                            if (!imported) return imported;
                            previous_contents = imported.value;
                        }
                    }
                    const auto allocation = history->write(output.resource_owner, reference.id);
                    if (!allocation) return {allocation.result, {}};
                    const auto imported = import_history(allocation.value);
                    if (!imported) return imported;
                    resource.texture = imported.value;
                    resource.staged_write = true;
                    // History writes are persistent graph roots even without a final_output.
                    if (const auto result = graph.export_texture(to_render_graph_texture(resource.texture)); !result)
                        return {result, {}};
                    if (previous_contents.is_valid()) {
                        auto prepare = *work;
                        prepare.kind = RenderWorkItemKind::kPrepare;
                        const auto name = std::string("Feature.") + std::to_string(output.instance_id.value()) +
                                          ".HistoryPreserve." + std::to_string(work->id.value());
                        prepare.id = RenderWorkItemId(name);
                        const auto pass = add_copy(name, previous_contents, resource.texture,
                            {resource.declaration.width, resource.declaration.height, 1}, true);
                        if (!pass) return {pass.result, {}};
                        if (const auto result = detail::RenderGraphFeatureGraphAccess::set_feature_work(graph, to_render_graph_pass(pass.value), prepare); !result)
                            return {result, {}};
                        if (previous.is_valid())
                            if (const auto result = builder.add_dependency(previous, pass.value); !result) return {result, {}};
                        previous = pass.value;
                    }
                    return {foundation::Result::success(), resource.texture};
                }
                const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto& value) {
                    return value.id == reference.id;
                });
                if (binding == bindings.end() || binding->version != reference.version)
                    return {invalid_state("external Feature render resource is missing, stale or owned by another instance"), {}};
                const auto texture = builder.find_texture(reference.id);
                if (!texture) return texture;
                const auto description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(texture.value));
                if (!description) return {description.result, {}};
                const auto format = description.value.format;
                if (role == AttachmentRole::kDepth ? format != rhi::Format::kD32Float
                    : role == AttachmentRole::kFloatColor ? format != rhi::Format::kRgba16Float
                    : (format != rhi::Format::kRgba8Unorm && format != rhi::Format::kRgba8Srgb &&
                       format != rhi::Format::kBgra8Unorm))
                    return {invalid_argument("Render.Intent binding format does not match its color/depth role"), {}};
                if (const auto result = register_import(to_render_graph_texture(texture.value)); !result) return {result, {}};
                return texture;
            };
            for (const auto& operation : output.intent.operations) {
                foundation::ResultValue<pipeline::RenderGraphPass> added;
                const auto& work = projected.value.at(work_index++);
                RenderSceneOperationSource source{work, output.resource_owner, output.view_key};
                std::visit([&source](const auto& actual) {
                    using Operation = std::decay_t<decltype(actual)>;
                    if constexpr (std::is_same_v<Operation, feature::RenderClearColor>) source.kind = RenderSceneOperationKind::kClearColor;
                    else if constexpr (std::is_same_v<Operation, feature::RenderCopyTexture>) source.kind = RenderSceneOperationKind::kCopyTexture;
                    else if constexpr (std::is_same_v<Operation, feature::RenderDrawScene>) source.kind = RenderSceneOperationKind::kDrawScene;
                    else if constexpr (std::is_same_v<Operation, feature::RenderDrawDepthScene>) {
                        source.kind = RenderSceneOperationKind::kDrawDepthScene;
                        source.depth_projection = actual.projection;
                    } else if constexpr (std::is_same_v<Operation, feature::RenderDrawGBufferScene>) source.kind = RenderSceneOperationKind::kDrawGBufferScene;
                    else if constexpr (std::is_same_v<Operation, feature::RenderDeferredLightingScene>) source.kind = RenderSceneOperationKind::kDeferredLightingScene;
                    else if constexpr (std::is_same_v<Operation, feature::RenderPostProcessScene>) source.kind = RenderSceneOperationKind::kPostProcessScene;
                    else if constexpr (std::is_same_v<Operation, feature::RenderLightCullingScene>) source.kind = RenderSceneOperationKind::kLightCullingScene;
                    else if constexpr (std::is_same_v<Operation, feature::RenderDrawClusteredScene>) source.kind = RenderSceneOperationKind::kDrawClusteredScene;
                }, operation);
                const auto name = std::string("Feature.") + std::to_string(output.instance_id.value()) + "." +
                                  std::to_string(work.id.value());
                const auto finish_lowering = [&](size_t first_pass, std::shared_ptr<const void> lease) -> foundation::Result {
                    if (lease)
                        if (const auto retained = detail::RenderGraphFeatureGraphAccess::retain_execution_owner(graph, std::move(lease)); !retained) return retained;
                    const auto attributed = detail::RenderGraphFeatureGraphAccess::attribute_feature_passes_since(graph, first_pass, work, to_render_graph_pass(previous));
                    if (!attributed) return attributed.result;
                    previous = to_pipeline_pass(attributed.value);
                    return foundation::Result::success();
                };
                const auto lower_scene = [&](const auto& draw, const RenderSceneDrawTargets& targets,
                    std::shared_ptr<const void>* selected_snapshot = nullptr) -> foundation::Result {
                    if (!draw_lowerer) return invalid_state("scene draw requires an engine Renderer lowerer");
                    const auto first_pass = detail::RenderGraphFeatureGraphAccess::feature_pass_checkpoint(graph);
                    auto sourced_targets = targets;
                    sourced_targets.source = &source;
                    const auto result = draw_lowerer->lower(builder, draw, sourced_targets, output.resource_owner, frame_index);
                    if (!result) return result.result;
                    if (!result.value) return invalid_state("Renderer scene draw returned no owning lease");
                    if (selected_snapshot) *selected_snapshot = result.value;
                    return finish_lowering(first_pass, result.value);
                };
                const auto resolve_light_depths = [&](const feature::RenderTextureReference& camera,
                    const std::optional<feature::RenderTextureReference>& shadow, bool write_depth,
                    RenderSceneDrawTargets& targets) -> foundation::Result {
                    if (!draw_lowerer || !current_frame || !current_frame->is_frame_active() ||
                        current_frame->frame_number() != frame_index ||
                        &current_frame->device() != &detail::RenderGraphExecution::device(graph))
                        return invalid_state("light-list lowering requires the graph's current active frame and Renderer");
                    const auto depth = resolve(camera, write_depth, false, nullptr, AttachmentRole::kDepth);
                    if (!depth) return depth.result;
                    const auto description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(depth.value));
                    if (!description) return description.result;
                    targets.depth = depth.value;
                    targets.depth_description = description.value;
                    targets.depth_production = depth_production(depth.value);
                    if (shadow) {
                        const auto texture = resolve(*shadow, false, false, nullptr, AttachmentRole::kDepth);
                        if (!texture) return texture.result;
                        const auto shadow_description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(texture.value));
                        if (!shadow_description) return shadow_description.result;
                        if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(depth.value), to_render_graph_texture(texture.value)))
                            return invalid_argument("light-list camera and shadow depth bindings alias");
                        targets.shadow = texture.value;
                        targets.shadow_description = shadow_description.value;
                        targets.shadow_production = depth_production(texture.value);
                    }
                    return foundation::Result::success();
                };
                if (const auto* clear = std::get_if<feature::RenderClearColor>(&operation)) {
                    const auto texture = resolve(clear->target, true, true, &work);
                    if (!texture) return rollback(texture.result);
                    invalidate_depth(texture.value);
                    const auto color = clear->color;
                    const auto description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(texture.value));
                    if (!description) return rollback(description.result);
                    if (rhi::has_flag(description.value.capabilities, rhi::TextureCapability::kColorAttachment)) {
                        added = builder.add_graphics_pass(name,
                            [target = texture.value, color](pipeline::RenderPassBuilder& pass) {
                                return pass.use_color_attachment(target, rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                                                                 {color[0], color[1], color[2], color[3]},
                                                                 rhi::ResourceUsage::kColorAttachmentWrite,
                                                                 {rhi::Aspect::kColor, 0, 1, 0, 1});
                            },
                            [](pipeline::RenderPassExecutionContext& command) {
                                if (const auto result = command.begin_rendering(); !result) return result;
                                return command.end_rendering();
                            });
                    } else {
                        if (!rhi::has_flag(description.value.capabilities, rhi::TextureCapability::kTransferDestination))
                            return rollback(invalid_argument("Feature clear requires a color attachment or transfer destination"));
                        added = builder.add_copy_pass(name,
                            [target = texture.value](pipeline::RenderPassBuilder& pass) {
                                return pass.write(target, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                                  rhi::ShaderStage::kAll, {rhi::Aspect::kColor, 0, 1, 0, 1}, true);
                            },
                            [target = texture.value, color](pipeline::RenderPassExecutionContext& command) {
                                return command.clear_texture(target, {rhi::Aspect::kColor, 0, 1, 0, 1},
                                                             {color[0], color[1], color[2], color[3]});
                            }, rhi::QueueClass::kGraphics);
                    }
                } else if (const auto* draw = std::get_if<feature::RenderDrawScene>(&operation)) {
                    const auto texture = resolve(draw->target, true);
                    if (!texture) return rollback(texture.result);
                    const auto description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(texture.value));
                    if (!description) return rollback(description.result);
                    RenderSceneDrawTargets targets{texture.value, description.value};
                    if (draw->depth) {
                        const auto depth = resolve(draw->depth->target, true, false, nullptr, AttachmentRole::kDepth);
                        if (!depth) return rollback(depth.result);
                        const auto depth_desc = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(depth.value));
                        if (!depth_desc) return rollback(depth_desc.result);
                        if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(texture.value), to_render_graph_texture(depth.value)))
                            return rollback(invalid_argument("scene Draw color and depth bindings alias"));
                        targets.depth = depth.value;
                        targets.depth_description = depth_desc.value;
                    }
                    if (draw->directional_shadow) {
                        const auto shadow = resolve(*draw->directional_shadow, false, false, nullptr, AttachmentRole::kDepth);
                        if (!shadow) return rollback(shadow.result);
                        const auto shadow_desc = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(shadow.value));
                        if (!shadow_desc) return rollback(shadow_desc.result);
                        if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(shadow.value), to_render_graph_texture(targets.color)) ||
                            (targets.depth.is_valid() && detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(shadow.value),
                                                                            to_render_graph_texture(targets.depth))))
                            return rollback(invalid_argument("scene shadow input aliases a draw attachment"));
                        targets.shadow = shadow.value;
                        targets.shadow_description = shadow_desc.value;
                    }
                    if (const auto result = lower_scene(*draw, targets); !result) return rollback(result);
                    invalidate_depth(targets.color);
                    if (targets.depth.is_valid()) invalidate_depth(targets.depth);
                    continue;
                } else if (const auto* draw = std::get_if<feature::RenderDrawDepthScene>(&operation)) {
                    const auto depth = resolve(draw->depth.target, true, false, nullptr, AttachmentRole::kDepth);
                    if (!depth) return rollback(depth.result);
                    const auto description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(depth.value));
                    if (!description) return rollback(description.result);
                    RenderSceneDrawTargets targets{};
                    targets.depth = depth.value;
                    targets.depth_description = description.value;
                    std::shared_ptr<const void> selected_snapshot;
                    if (const auto result = lower_scene(*draw, targets, &selected_snapshot); !result) return rollback(result);
                    record_depth(draw->scene, draw->depth, targets.depth, draw->projection, std::move(selected_snapshot));
                    continue;
                } else if (const auto* draw = std::get_if<feature::RenderDrawGBufferScene>(&operation)) {
                    const std::array references{draw->albedo.target, draw->normal.target, draw->material.target, draw->depth.target};
                    std::array<pipeline::RenderGraphTexture, 4> textures{};
                    std::array<rhi::TextureDesc, 4> descriptions{};
                    for (size_t index = 0; index < references.size(); ++index) {
                        const auto role = index < 2 ? AttachmentRole::kFloatColor
                            : index == 3 ? AttachmentRole::kDepth : AttachmentRole::kLdrColor;
                        const auto texture = resolve(references[index], true, false, nullptr, role);
                        if (!texture) return rollback(texture.result);
                        textures[index] = texture.value;
                        const auto description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(texture.value));
                        if (!description) return rollback(description.result);
                        descriptions[index] = description.value;
                        for (size_t previous_index = 0; previous_index < index; ++previous_index)
                            if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(texture.value), to_render_graph_texture(textures[previous_index])))
                                return rollback(invalid_argument("GBuffer MRT and depth bindings alias"));
                    }
                    RenderSceneDrawTargets targets{textures[0], descriptions[0], textures[3], descriptions[3]};
                    targets.additional_colors = {textures[1], textures[2]};
                    targets.additional_color_descriptions = {descriptions[1], descriptions[2]};
                    std::shared_ptr<const void> selected_snapshot;
                    if (const auto result = lower_scene(*draw, targets, &selected_snapshot); !result) return rollback(result);
                    for (size_t index = 0; index < 3; ++index) invalidate_depth(textures[index]);
                    record_depth(draw->scene, draw->depth, targets.depth, feature::RenderDepthSceneProjection::kCamera,
                        std::move(selected_snapshot));
                    continue;
                } else if (const auto* draw = std::get_if<feature::RenderDeferredLightingScene>(&operation)) {
                    const std::array references{draw->hdr.target, draw->albedo, draw->normal, draw->depth,
                        draw->directional_shadow.value_or(feature::RenderTextureReference{})};
                    const size_t count = draw->directional_shadow ? references.size() : references.size() - 1;
                    std::array<pipeline::RenderGraphTexture, 5> textures{};
                    std::array<rhi::TextureDesc, 5> descriptions{};
                    for (size_t index = 0; index < count; ++index) {
                        const auto role = index < 3 ? AttachmentRole::kFloatColor : AttachmentRole::kDepth;
                        const auto texture = resolve(references[index], index == 0, false, nullptr, role);
                        if (!texture) return rollback(texture.result);
                        textures[index] = texture.value;
                        const auto description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(texture.value));
                        if (!description) return rollback(description.result);
                        descriptions[index] = description.value;
                        for (size_t previous = 0; previous < index; ++previous)
                            if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(texture.value), to_render_graph_texture(textures[previous])))
                                return rollback(invalid_argument("DeferredLighting input/output bindings alias"));
                    }
                    RenderSceneDrawTargets targets{textures[0], descriptions[0], textures[3], descriptions[3],
                        textures[4], descriptions[4]};
                    targets.additional_colors = {textures[1], textures[2]};
                    targets.additional_color_descriptions = {descriptions[1], descriptions[2]};
                    if (const auto result = lower_scene(*draw, targets); !result) return rollback(result);
                    invalidate_depth(targets.color);
                    continue;
                } else if (const auto* draw = std::get_if<feature::RenderPostProcessScene>(&operation)) {
                    const std::array references{draw->output.target, draw->hdr, draw->depth};
                    std::array<pipeline::RenderGraphTexture, 3> textures{};
                    std::array<rhi::TextureDesc, 3> descriptions{};
                    for (size_t index = 0; index < references.size(); ++index) {
                        const auto role = index == 0 ? AttachmentRole::kLdrColor
                            : index == 1 ? AttachmentRole::kFloatColor : AttachmentRole::kDepth;
                        const auto texture = resolve(references[index], index == 0, false, nullptr, role);
                        if (!texture) return rollback(texture.result);
                        textures[index] = texture.value;
                        const auto description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(texture.value));
                        if (!description) return rollback(description.result);
                        descriptions[index] = description.value;
                        for (size_t previous = 0; previous < index; ++previous)
                            if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(texture.value), to_render_graph_texture(textures[previous])))
                                return rollback(invalid_argument("PostProcess input/output bindings alias"));
                    }
                    RenderSceneDrawTargets targets{textures[0], descriptions[0], textures[2], descriptions[2]};
                    targets.additional_colors[0] = textures[1];
                    targets.additional_color_descriptions[0] = descriptions[1];
                    if (const auto result = lower_scene(*draw, targets); !result) return rollback(result);
                    invalidate_depth(targets.color);
                    continue;
                } else if (const auto* cull = std::get_if<feature::RenderLightCullingScene>(&operation)) {
                    RenderSceneDrawTargets targets{};
                    if (const auto result = resolve_light_depths(cull->camera_depth, cull->directional_shadow, false, targets); !result)
                        return rollback(result);
                    const auto scope = build_scope(targets.depth);
                    const RenderLightListProduction production{output.resource_owner, output.invocation, output_index, work.operation_index};
                    targets.source = &source;
                    const auto first_pass = detail::RenderGraphFeatureGraphAccess::feature_pass_checkpoint(graph);
                    const auto lowered = draw_lowerer->lower(builder, *cull, targets, output.resource_owner, scope, production);
                    if (!lowered) return rollback(lowered.result);
                    if (!lowered.value || lowered.value->scope() != scope || lowered.value->production() != production)
                        return rollback(invalid_state("Renderer light-list lease has another build or producer identity"));
                    if (const auto result = finish_lowering(first_pass, lowered.value); !result) return rollback(result);
                    light_lists[output_index][work.operation_index] = lowered.value;
                    continue;
                } else if (const auto* draw = std::get_if<feature::RenderDrawClusteredScene>(&operation)) {
                    const auto use = std::find_if(execution_plan.value.light_list_uses.begin(), execution_plan.value.light_list_uses.end(),
                        [&](const auto& item) { return item.consumer_output == output_index && item.consumer_operation == work.operation_index; });
                    if (use == execution_plan.value.light_list_uses.end())
                        return rollback(invalid_state("clustered draw has no trusted light-list producer"));
                    const auto& lease = light_lists.at(use->producer_output).at(use->producer_operation);
                    if (!lease) return rollback(invalid_state("clustered draw producer has not been lowered in this build"));
                    RenderSceneDrawTargets targets{};
                    if (const auto result = resolve_light_depths(draw->depth.target, draw->directional_shadow, true, targets); !result)
                        return rollback(result);
                    const auto color = resolve(draw->hdr.target, true, false, nullptr, AttachmentRole::kFloatColor);
                    if (!color) return rollback(color.result);
                    const auto color_description = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(color.value));
                    if (!color_description) return rollback(color_description.result);
                    if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(color.value), to_render_graph_texture(targets.depth)) ||
                        (targets.shadow.is_valid() && detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(color.value), to_render_graph_texture(targets.shadow))))
                        return rollback(invalid_argument("clustered HDR output aliases its camera or shadow depth"));
                    targets.color = color.value;
                    targets.color_description = color_description.value;
                    const auto scope = build_scope(targets.depth);
                    if (lease->scope() != scope) return rollback(invalid_state("clustered draw lease is stale"));
                    targets.source = &source;
                    const auto first_pass = detail::RenderGraphFeatureGraphAccess::feature_pass_checkpoint(graph);
                    if (const auto result = draw_lowerer->lower(builder, *draw, targets, output.resource_owner, scope, *lease); !result)
                        return rollback(result);
                    if (const auto result = finish_lowering(first_pass, {}); !result) return rollback(result);
                    invalidate_depth(targets.color);
                    continue;
                } else {
                    const auto& copy = std::get<feature::RenderCopyTexture>(operation);
                    const auto source = resolve(copy.source);
                    if (!source) return rollback(source.result);
                    const auto declaration = local.find(copy.destination.id);
                    const bool overwrites_history = !copy.destination.external && declaration != local.end() &&
                        copy.width == declaration->second.declaration.width && copy.height == declaration->second.declaration.height;
                    const auto destination = resolve(copy.destination, true, overwrites_history, &work);
                    if (!destination) return rollback(destination.result);
                    invalidate_depth(destination.value);
                    if (detail::RenderGraphFeatureGraphAccess::textures_alias(graph, to_render_graph_texture(source.value), to_render_graph_texture(destination.value)))
                        return rollback(invalid_argument("Feature texture copy bindings alias the same resource"));
                    const auto source_desc = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(source.value));
                    const auto destination_desc = detail::RenderGraphFeatureGraphAccess::texture_description(graph, to_render_graph_texture(destination.value));
                    if (!source_desc || !destination_desc || source_desc.value.format != destination_desc.value.format ||
                        source_desc.value.samples != destination_desc.value.samples ||
                        !rhi::has_flag(source_desc.value.capabilities, rhi::TextureCapability::kTransferSource) ||
                        !rhi::has_flag(destination_desc.value.capabilities, rhi::TextureCapability::kTransferDestination) ||
                        copy.width > source_desc.value.extent.width || copy.height > source_desc.value.extent.height ||
                        copy.width > destination_desc.value.extent.width || copy.height > destination_desc.value.extent.height)
                        return rollback(invalid_argument("Feature texture copy capabilities, samples, extent or format do not match its resources"));
                    const bool overwrites_destination = copy.width == destination_desc.value.extent.width &&
                        copy.height == destination_desc.value.extent.height && destination_desc.value.extent.depth == 1;
                    added = add_copy(name, source.value, destination.value, {copy.width, copy.height, 1}, overwrites_destination);
                }
                if (!added) return rollback(added.result);
                if (const auto result = detail::RenderGraphFeatureGraphAccess::set_feature_work(graph, to_render_graph_pass(added.value), work); !result) return rollback(result);
                if (previous.is_valid())
                    if (const auto result = builder.add_dependency(previous, added.value); !result) return rollback(result);
                previous = added.value;
            }
            if (output.intent.final_output) {
                const auto target = resolve(*output.intent.final_output);
                if (!target) return rollback(target.result);
                if (const auto result = builder.export_texture(target.value); !result) return rollback(result);
            }
            if (output.intent.depth_output) {
                const auto target = resolve(*output.intent.depth_output, true, false, nullptr, AttachmentRole::kDepth);
                if (!target) return rollback(target.result);
                if (const auto result = builder.export_texture(target.value); !result) return rollback(result);
            }
        }
        if (history)
            if (const auto result = detail::RenderGraphFeatureGraphAccess::retain_submission_owner(graph, history); !result) return rollback(result);
        detail::RenderGraphFeatureGraphAccess::commit_feature_build_transaction(graph);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return rollback(foundation::Result::failure(foundation::ResultCode::kOutOfMemory, "Feature render intent build allocation failed"));
    } catch (const std::exception& error) {
        return rollback(foundation::Result::failure(foundation::ResultCode::kUnknown, error.what()));
    } catch (...) {
        return rollback(invalid_state("Feature render intent build threw an exception"));
    }
}

}  // namespace aeg::render_graph
