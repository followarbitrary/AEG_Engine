/**
 * @file render_feature_runtime.cpp
 * @brief Adapts Pipeline render features to the concrete RenderGraph API.
 */
#include "../public/render_feature_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../../Pipeline/public/pipeline_configuration.hpp"
#include "../../Pipeline/public/render_pass.hpp"
#include "../public/render_state_flow.hpp"

namespace aeg::render_graph {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;

inline constexpr uint32_t kKnownFeaturePointFlags =
    static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayCreateArtifacts) |
    static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayCreateGraphicsPass) |
    static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayCreateComputePass) |
    static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayCreateCopyPass) |
    static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayUseDedicatedQueue) |
    static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayWriteFinalOutput) |
    static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayPublishSnapshot);

[[nodiscard]] foundation::Result append_feature_message(std::string prefix, const foundation::Result& result) {
    if (!result.message.empty()) {
        prefix += ": ";
        prefix += result.message;
    }
    return foundation::Result::failure(result.code, std::move(prefix));
}

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
    void set_feature_owner(std::shared_ptr<const pipeline::PipelineFeature> owner) override {
        current_feature_owner_ = std::move(owner);
    }
    void set_feature_point_flags(pipeline::FeaturePointFlags point_flags) noexcept {
        current_feature_point_flags_ = point_flags;
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

    [[nodiscard]] foundation::Result register_initial_resources(
        std::span<const RenderGraphFeatureBufferBinding> buffers,
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
    std::shared_ptr<const pipeline::PipelineFeature> current_feature_owner_{};
    pipeline::FeaturePointFlags current_feature_point_flags_{pipeline::FeaturePointFlags::kNone};

    friend class RenderPassBuilderBackendImpl;
    friend class RenderPassExecutionContextBackendImpl;
};

[[nodiscard]] foundation::Result validate_render_feature_plan(const pipeline::PipelinePlan& plan) {
    if (plan.revision == 0 || !plan.pipeline.is_valid()) {
        return invalid_state("render feature runtime received an invalid pipeline plan");
    }

    if (static_cast<uint8_t>(plan.scope) > static_cast<uint8_t>(pipeline::PipelineScope::kEditor)) {
        return invalid_state("render feature runtime received a plan with an invalid scope");
    }

    for (const pipeline::PipelineFeatureContributionRecord& contribution : plan.feature_contributions) {
        if (!contribution.feature_instance.is_valid() || !contribution.feature.is_valid() ||
            !contribution.point.is_valid() || contribution.domain == pipeline::FeatureDomain::kComposite ||
            (static_cast<uint32_t>(contribution.point_flags) & ~kKnownFeaturePointFlags) != 0) {
            return invalid_state("render feature runtime received an invalid feature contribution");
        }
    }

    for (size_t implementation_index = 0; implementation_index < plan.feature_implementations.size();
         ++implementation_index) {
        const pipeline::PipelineFeatureImplementationRecord& implementation =
            plan.feature_implementations[implementation_index];
        if (!implementation.feature_instance.is_valid() || !implementation.feature.is_valid() ||
            implementation.implementation == nullptr) {
            return invalid_state("render feature runtime received an invalid feature implementation");
        }
        for (size_t duplicate_index = implementation_index + 1; duplicate_index < plan.feature_implementations.size();
             ++duplicate_index) {
            const pipeline::PipelineFeatureImplementationRecord& duplicate =
                plan.feature_implementations[duplicate_index];
            if (duplicate.feature_instance == implementation.feature_instance &&
                duplicate.feature == implementation.feature && duplicate.part == implementation.part) {
                return invalid_state("render feature runtime received duplicate feature implementations");
            }
        }
    }

    for (const pipeline::PipelineFeatureContributionRecord& contribution : plan.feature_contributions) {
        if (contribution.domain != pipeline::FeatureDomain::kRender) {
            continue;
        }
        const auto implementation =
            std::find_if(plan.feature_implementations.begin(), plan.feature_implementations.end(),
                         [&](const pipeline::PipelineFeatureImplementationRecord& candidate) {
                             return candidate.feature_instance == contribution.feature_instance &&
                                    candidate.feature == contribution.feature && candidate.part == contribution.part;
                         });
        if (implementation == plan.feature_implementations.end()) {
            continue;
        }
        if (implementation->implementation->descriptor().id != contribution.feature ||
            (implementation->implementation->feature_domain_mask() &
             pipeline::to_mask(pipeline::FeatureDomain::kRender)) == 0) {
            return invalid_state("render feature implementation does not satisfy its contribution contract");
        }
    }
    return foundation::Result::success();
}

[[nodiscard]] std::shared_ptr<const pipeline::PipelineFeature> find_plan_feature_implementation(
    const pipeline::PipelinePlan& plan, const pipeline::PipelineFeatureContributionRecord& contribution) {
    const auto iterator = std::find_if(plan.feature_implementations.begin(), plan.feature_implementations.end(),
                                       [&](const pipeline::PipelineFeatureImplementationRecord& implementation) {
                                           return implementation.feature_instance == contribution.feature_instance &&
                                                  implementation.feature == contribution.feature &&
                                                  implementation.part == contribution.part;
                                       });
    return iterator == plan.feature_implementations.end() ? nullptr : iterator->implementation;
}
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
    const std::shared_ptr<const pipeline::PipelineFeature> feature_owner = current_feature_owner_;
    const pipeline::FeaturePointFlags point_flags = current_feature_point_flags_;

    PassSetupCallback setup_callback = [self, feature_owner, point_flags,
                                        setup = std::move(setup)](PassBuilder& builder) mutable {
        static_cast<void>(feature_owner);
        if (!setup) {
            return foundation::Result::success();
        }
        RenderPassBuilderBackendImpl pass_backend(self, builder);
        pipeline::RenderPassBuilder facade(pass_backend, point_flags);
        return setup(facade);
    };
    PassExecuteCallback execute_callback = [self, feature_owner,
                                            execute = std::move(execute)](PassExecutionContext& context) mutable {
        static_cast<void>(feature_owner);
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
    if (!graph_.contains_buffer(graph_buffer)) {
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
    if (!graph_.contains_texture(graph_texture)) {
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

foundation::Result RenderGraphBuilderBackendImpl::register_initial_resources(
    std::span<const RenderGraphFeatureBufferBinding> buffers,
    std::span<const RenderGraphFeatureTextureBinding> textures) {
    try {
        buffers_.reserve(buffers_.size() + buffers.size());
        textures_.reserve(textures_.size() + textures.size());
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                           "render graph feature resource index allocation failed");
    }

    for (const RenderGraphFeatureBufferBinding& binding : buffers) {
        if (!binding.id.is_valid() || !binding.buffer.is_valid()) {
            return invalid_argument("initial render graph buffer binding is invalid");
        }
        if (!graph_.contains_buffer(binding.buffer)) {
            return not_found("initial render graph buffer handle does not belong to this graph");
        }
        if (resource_id_taken(binding.id)) {
            return invalid_state("initial render graph resource id is already registered");
        }
        if (const foundation::Result result = remember_buffer(binding.id, to_pipeline_buffer(binding.buffer));
            !result) {
            return result;
        }
    }
    for (const RenderGraphFeatureTextureBinding& binding : textures) {
        if (!binding.id.is_valid() || !binding.texture.is_valid()) {
            return invalid_argument("initial render graph texture binding is invalid");
        }
        if (!graph_.contains_texture(binding.texture)) {
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

[[nodiscard]] foundation::Result expand_authored_render_passes(pipeline::RenderGraphBuilder& builder,
                                                               const pipeline::RenderGraphFeatureContext& context) {
    if (context.configuration == nullptr) {
        return invalid_state("authored render passes require a Feature configuration");
    }
    if (context.render_passes == nullptr) {
        return invalid_state("authored render passes require a RenderPassRegistry");
    }

    std::unordered_set<uint64_t> instances;
    std::unordered_map<pipeline::RenderPassInstanceId, pipeline::RenderGraphPass,
                       pipeline::StableIdHash<pipeline::RenderPassInstanceIdTag>>
        handles;
    for (const pipeline::RenderPassInstanceConfiguration& authored : context.configuration->passes) {
        if (!authored.enabled) {
            continue;
        }
        if (context.part.is_valid()) {
            if (!authored.resolved_part.is_valid()) {
                return invalid_argument("authored render pass has no resolved CompositeFeature part");
            }
            if (authored.resolved_part != context.part) {
                continue;
            }
        }
        if (!authored.instance.is_valid() || !instances.insert(authored.instance.value()).second) {
            return invalid_argument("authored render pass instance is missing or duplicated");
        }
        if (static_cast<uint8_t>(authored.queue) > static_cast<uint8_t>(rhi::QueueClass::kCopy)) {
            return invalid_argument("authored render pass queue is invalid");
        }

        const auto registered = context.render_passes->find_pass(authored.type);
        if (!registered) {
            return registered.result;
        }
        const auto& descriptor = registered.value->descriptor();
        const auto queue_bit = uint64_t{1} << static_cast<uint8_t>(authored.queue);
        if ((descriptor.allowed_queue_mask & queue_bit) == 0) {
            return invalid_argument("authored render pass queue is not allowed");
        }
        if ((descriptor.graph_type == pipeline::RenderGraphPassType::kGraphics &&
             authored.queue != rhi::QueueClass::kGraphics) ||
            (descriptor.graph_type == pipeline::RenderGraphPassType::kCompute &&
             authored.queue != rhi::QueueClass::kCompute) ||
            (descriptor.graph_type == pipeline::RenderGraphPassType::kCopy &&
             authored.queue != rhi::QueueClass::kCopy)) {
            return invalid_argument("authored render pass graph type and queue do not match");
        }

        auto configuration = std::make_shared<pipeline::RenderPassConfiguration>();
        configuration->type = authored.type;
        configuration->instance = authored.instance;
        configuration->queue = authored.queue;
        configuration->enabled = authored.enabled;
        configuration->parameters = authored.parameters;
        configuration->bindings = authored.resource_bindings;
        configuration->source = authored.source;
        const auto added =
            pipeline::add_registered_render_pass(builder, *context.render_passes, std::move(configuration));
        if (!added) {
            return added.result;
        }
        handles.emplace(authored.instance, added.value);
    }
    for (const pipeline::RenderPassInstanceConfiguration& authored : context.configuration->passes) {
        const auto destination = handles.find(authored.instance);
        if (destination == handles.end()) {
            continue;
        }
        for (const pipeline::RenderPassInstanceId source_instance : authored.ordered_after) {
            const auto source = handles.find(source_instance);
            if (source == handles.end()) {
                return invalid_argument("authored render pass ordering references an inactive pass");
            }
            if (const foundation::Result dependency = builder.add_dependency(source->second, destination->second);
                !dependency) {
                return dependency;
            }
        }
    }
    return foundation::Result::success();
}

[[nodiscard]] pipeline::RenderResourceId render_state_output_id(uint32_t contribution_index) noexcept {
    constexpr uint64_t kRenderStateOutputSalt = 0x9e3779b97f4a7c15ull;
    return pipeline::RenderResourceId(kRenderStateOutputSalt ^ (static_cast<uint64_t>(contribution_index) + 1ull));
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

struct RenderGraphFeatureRuntime::Impl final {
    Impl(RenderGraph& graph_reference, const pipeline::PipelinePlan& plan_reference,
         const pipeline::PipelineRegistry& registry_reference, const RenderGraphFeatureRuntimeDesc& desc_reference)
        : graph(graph_reference), plan(&plan_reference), registry(registry_reference), desc(desc_reference) {}

    Impl(RenderGraph& graph_reference, pipeline::PipelinePlanHandle plan_reference,
         const pipeline::PipelineRegistry& registry_reference, const RenderGraphFeatureRuntimeDesc& desc_reference)
        : graph(graph_reference),
          plan(plan_reference.get()),
          plan_owner(std::move(plan_reference)),
          registry(registry_reference),
          desc(desc_reference) {}

    RenderGraph& graph;
    const pipeline::PipelinePlan* plan{nullptr};
    pipeline::PipelinePlanHandle plan_owner{};
    const pipeline::PipelineRegistry& registry;
    RenderGraphFeatureRuntimeDesc desc{};
    bool built{false};
};

RenderGraphFeatureRuntime::RenderGraphFeatureRuntime(RenderGraph& graph, const pipeline::PipelinePlan& plan,
                                                     const pipeline::PipelineRegistry& registry,
                                                     const RenderGraphFeatureRuntimeDesc& desc)
    : impl_(std::make_unique<Impl>(graph, plan, registry, desc)) {}

RenderGraphFeatureRuntime::~RenderGraphFeatureRuntime() = default;

RenderGraphFeatureRuntime::RenderGraphFeatureRuntime(RenderGraph& graph, pipeline::PipelinePlanHandle plan,
                                                     const pipeline::PipelineRegistry& registry,
                                                     const RenderGraphFeatureRuntimeDesc& desc)
    : impl_(std::make_unique<Impl>(graph, std::move(plan), registry, desc)) {}

foundation::Result RenderGraphFeatureRuntime::build() {
    if (impl_ == nullptr) {
        return invalid_state("render feature runtime is not initialized");
    }
    if (impl_->built) {
        return invalid_state("render feature runtime has already built its contributions");
    }
    if (impl_->plan == nullptr) {
        return invalid_argument("render feature runtime received a null pipeline plan");
    }
    if (const foundation::Result plan_result = validate_render_feature_plan(*impl_->plan); !plan_result) {
        return plan_result;
    }

    if (const foundation::Result transaction_result = impl_->graph.begin_feature_build_transaction();
        !transaction_result) {
        return transaction_result;
    }
    const auto rollback = [this](foundation::Result result) {
        impl_->graph.rollback_feature_build_transaction();
        return result;
    };

    if (impl_->plan_owner != nullptr) {
        const std::shared_ptr<const void> plan_owner = impl_->plan_owner;
        if (const foundation::Result result = impl_->graph.retain_execution_owner(plan_owner); !result) {
            return rollback(result);
        }
    }

    std::shared_ptr<RenderGraphBuilderBackendImpl> backend;
    try {
        backend = std::make_shared<RenderGraphBuilderBackendImpl>(impl_->graph);
    } catch (const std::bad_alloc&) {
        return rollback(foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                    "render feature backend allocation failed"));
    }
    if (const foundation::Result result =
            backend->register_initial_resources(impl_->desc.buffers, impl_->desc.textures);
        !result) {
        return rollback(result);
    }

    const RenderStateFlow state_flow = build_render_state_flow(*impl_->plan, impl_->desc.state.link_overrides);
    if (state_flow.has_cycle) {
        return rollback(invalid_argument("render-state links form a cycle"));
    }
    std::vector<pipeline::RenderGraphTexture> state_outputs(impl_->plan->feature_contributions.size());
    std::vector<std::vector<pipeline::RenderGraphTexture>> state_inputs(impl_->plan->feature_contributions.size());
    // A RenderFeature may consume state without producing a replacement state
    // texture (for example readback, inspection, or culling).  Keep the writer
    // identities separately so downstream writers can receive the original
    // state through such pass-through Features.
    std::vector<std::vector<uint32_t>> state_input_writers(impl_->plan->feature_contributions.size());
    std::vector<uint8_t> state_writer_contributions(impl_->plan->feature_contributions.size(), 0);
    std::vector<pipeline::RenderStateBlendMode> state_writer_blend_modes(impl_->plan->feature_contributions.size(),
                                                                         pipeline::RenderStateBlendMode::kReplace);
    std::vector<uint8_t> state_writer_capabilities(impl_->plan->feature_contributions.size(), 0);
    if (impl_->desc.state.enabled) {
        for (uint32_t contribution_index = 0; contribution_index < impl_->plan->feature_contributions.size();
             ++contribution_index) {
            const auto& contribution = impl_->plan->feature_contributions[contribution_index];
            if (contribution.domain != pipeline::FeatureDomain::kRender ||
                !state_flow.contributions[contribution_index].reaches_present) {
                continue;
            }
            std::shared_ptr<const pipeline::PipelineFeature> feature_implementation =
                find_plan_feature_implementation(*impl_->plan, contribution);
            if (feature_implementation == nullptr) {
                const auto feature_result = impl_->registry.find_feature(contribution.feature);
                if (!feature_result) {
                    return rollback(append_feature_message("render feature lookup failed", feature_result.result));
                }
                feature_implementation = feature_result.value;
            }
            state_writer_capabilities[contribution_index] =
                feature_implementation->uses_render_state_output() ? 1u : 0u;
            state_writer_blend_modes[contribution_index] = feature_implementation->render_state_blend_mode();
            for (const RenderStateBlendOverride& override : impl_->desc.state.blend_overrides) {
                if (override.node >= impl_->plan->nodes.size()) {
                    continue;
                }
                const uint64_t stage_index = impl_->plan->nodes[override.node].definition_stage_index;
                if (stage_index >= contribution.first_stage_index &&
                    stage_index < contribution.first_stage_index + contribution.stage_count) {
                    state_writer_blend_modes[contribution_index] = override.blend_mode;
                }
            }
        }
    }
    if (impl_->desc.state.enabled) {
        const rhi::TextureDesc& state_desc = impl_->desc.state.texture_desc;
        if (state_desc.format == rhi::Format::kUnknown || state_desc.extent.width == 0 ||
            state_desc.extent.height == 0 || state_desc.extent.depth == 0 || state_desc.mip_levels == 0 ||
            state_desc.array_layers == 0 || state_desc.capabilities == rhi::TextureCapability::kNone) {
            return rollback(invalid_argument("render-state texture template is incomplete (format=" +
                                             std::to_string(static_cast<uint32_t>(state_desc.format)) +
                                             ", extent=" + std::to_string(state_desc.extent.width) + "x" +
                                             std::to_string(state_desc.extent.height) + "x" +
                                             std::to_string(state_desc.extent.depth) + ")"));
        }
        for (uint32_t contribution_index = 0; contribution_index < impl_->plan->feature_contributions.size();
             ++contribution_index) {
            const auto& contribution = impl_->plan->feature_contributions[contribution_index];
            if (contribution.domain != pipeline::FeatureDomain::kRender ||
                !state_flow.contributions[contribution_index].reaches_present ||
                state_writer_capabilities[contribution_index] == 0) {
                continue;
            }
            const auto texture =
                backend->create_texture(render_state_output_id(contribution_index), impl_->desc.state.texture_desc,
                                        pipeline::RenderGraphResourceLifetime::kTransient);
            if (!texture) {
                return rollback(append_feature_message("render state output allocation failed", texture.result));
            }
            state_outputs[contribution_index] = texture.value;
        }
        for (uint32_t contribution_index = 0; contribution_index < impl_->plan->feature_contributions.size();
             ++contribution_index) {
            std::vector<uint8_t> visiting(impl_->plan->feature_contributions.size(), 0);
            const auto collect_input_writers = [&](auto&& self, uint32_t destination) -> void {
                if (destination >= state_flow.contributions.size() || visiting[destination] != 0)
                    return;
                visiting[destination] = 1;
                for (const uint32_t predecessor : state_flow.contributions[destination].input_contributions) {
                    if (predecessor >= state_outputs.size())
                        continue;
                    if (state_writer_capabilities[predecessor] != 0 && state_outputs[predecessor].is_valid()) {
                        if (std::find(state_input_writers[contribution_index].begin(),
                                      state_input_writers[contribution_index].end(),
                                      predecessor) == state_input_writers[contribution_index].end()) {
                            state_input_writers[contribution_index].push_back(predecessor);
                            state_inputs[contribution_index].push_back(state_outputs[predecessor]);
                        }
                    } else {
                        self(self, predecessor);
                    }
                }
            };
            collect_input_writers(collect_input_writers, contribution_index);
        }
    }
    for (uint32_t contribution_index = 0; contribution_index < impl_->plan->feature_contributions.size();
         ++contribution_index) {
        const pipeline::PipelineFeatureContributionRecord& contribution =
            impl_->plan->feature_contributions[contribution_index];
        if (contribution.domain != pipeline::FeatureDomain::kRender) {
            continue;
        }

        std::shared_ptr<const pipeline::PipelineFeature> feature_implementation =
            find_plan_feature_implementation(*impl_->plan, contribution);
        if (feature_implementation == nullptr) {
            const auto feature_result = impl_->registry.find_feature(contribution.feature);
            if (!feature_result) {
                return rollback(append_feature_message("render feature lookup failed", feature_result.result));
            }
            feature_implementation = feature_result.value;
        }
        if ((feature_implementation->feature_domain_mask() & pipeline::to_mask(pipeline::FeatureDomain::kRender)) ==
            0) {
            return rollback(invalid_state("render feature contribution has no render domain implementation"));
        }
        // A disconnected state writer must not render into the final target.
        // Non-writer features still build: editor output/readback features use
        // this path to clear the viewport and report a detached Present state.
        if (!state_flow.contributions[contribution_index].reaches_present &&
            (!impl_->desc.state.enabled || feature_implementation->uses_render_state_output())) {
            continue;
        }
        const bool writes_render_state =
            impl_->desc.state.enabled && feature_implementation->uses_render_state_output();
        if (writes_render_state) {
            if (!state_outputs[contribution_index].is_valid()) {
                return rollback(invalid_state("render-state feature has no allocated state output"));
            }
            if (state_inputs[contribution_index].size() > 1) {
                if (!impl_->desc.state.compose_terminal_states) {
                    return rollback(invalid_state("multiple render-state inputs require a state compositor callback"));
                }
                std::vector<RenderStateCompositeInput> merge_inputs{};
                merge_inputs.reserve(state_input_writers[contribution_index].size());
                for (const uint32_t writer : state_input_writers[contribution_index]) {
                    merge_inputs.push_back({state_outputs[writer], state_writer_blend_modes[writer]});
                }
                if (merge_inputs.size() > 1) {
                    const pipeline::RenderGraphFeatureContext composite_context{
                        .scope = impl_->plan->scope,
                        .pipeline = impl_->plan->pipeline,
                        .point_flags = pipeline::FeaturePointFlags::kMayCreateArtifacts |
                                       pipeline::FeaturePointFlags::kMayCreateGraphicsPass |
                                       pipeline::FeaturePointFlags::kMayCreateComputePass |
                                       pipeline::FeaturePointFlags::kMayCreateCopyPass |
                                       pipeline::FeaturePointFlags::kMayUseDedicatedQueue |
                                       pipeline::FeaturePointFlags::kMayWriteFinalOutput,
                        .user_data = impl_->desc.user_data,
                        .feature_data = impl_->desc.feature_data,
                    };
                    backend->set_feature_point_flags(composite_context.point_flags);
                    pipeline::RenderGraphBuilder composite_builder(*backend, composite_context);
                    foundation::Result composite_result;
                    try {
                        composite_result = impl_->desc.state.compose_terminal_states(composite_builder, merge_inputs,
                                                                                     state_outputs[contribution_index]);
                    } catch (const std::bad_alloc&) {
                        composite_result = foundation::Result::failure(
                            foundation::ResultCode::kOutOfMemory, "render-state merge compositor allocation failed");
                    } catch (const std::exception& exception) {
                        composite_result =
                            foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
                    } catch (...) {
                        composite_result =
                            foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                        "render-state merge compositor threw an unknown exception");
                    }
                    backend->set_feature_point_flags(pipeline::FeaturePointFlags::kNone);
                    if (!composite_result) {
                        return rollback(
                            append_feature_message("render-state input composition failed", composite_result));
                    }
                }
            }
        }

        backend->set_feature_owner(feature_implementation);
        backend->set_feature_point_flags(contribution.point_flags);
        const pipeline::FeatureInstanceConfiguration* feature_configuration = nullptr;
        if (impl_->desc.configuration != nullptr) {
            for (const pipeline::FeatureInstanceConfiguration& candidate : impl_->desc.configuration->features) {
                if (candidate.instance == contribution.feature_instance) {
                    feature_configuration = &candidate;
                    break;
                }
            }
        }
        const pipeline::RenderGraphFeatureContext feature_context{
            impl_->plan->scope,
            impl_->plan->pipeline,
            contribution.feature,
            contribution.feature_instance,
            contribution.part,
            contribution.point,
            contribution.point_position,
            contribution.point_flags,
            state_outputs[contribution_index],
            state_inputs[contribution_index].empty() ? pipeline::RenderGraphTexture{}
                                                     : state_inputs[contribution_index].front(),
            state_inputs[contribution_index],
            state_flow.contributions[contribution_index].layer_count,
            impl_->desc.user_data,
            impl_->desc.feature_data,
            feature_configuration,
            impl_->desc.render_passes,
        };
        pipeline::RenderGraphBuilder builder(*backend, feature_context);

        foundation::Result build_result;
        try {
            build_result = feature_implementation->build_render_graph(builder);
        } catch (const std::bad_alloc&) {
            build_result = foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                       "render feature graph build allocation failed");
        } catch (const std::exception& exception) {
            build_result = foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
        } catch (...) {
            build_result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                       "render feature graph build threw an unknown exception");
        }
        if (!build_result) {
            backend->set_feature_owner({});
            backend->set_feature_point_flags(pipeline::FeaturePointFlags::kNone);
            return rollback(append_feature_message("render feature graph build failed", build_result));
        }
        const bool expands_authored_passes =
            feature_configuration != nullptr && feature_configuration->expand_authored_passes;
        const bool legacy_expands_authored_passes = feature_implementation->uses_authored_render_passes_for_part(
            contribution.feature_instance, contribution.part);
        if (expands_authored_passes || legacy_expands_authored_passes) {
            foundation::Result authored_pass_result;
            try {
                authored_pass_result = expand_authored_render_passes(builder, feature_context);
            } catch (const std::bad_alloc&) {
                authored_pass_result = foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                                   "authored render pass expansion allocation failed");
            } catch (const std::exception& exception) {
                authored_pass_result = foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
            } catch (...) {
                authored_pass_result = foundation::Result::failure(
                    foundation::ResultCode::kUnknown, "authored render pass expansion threw an unknown exception");
            }
            if (!authored_pass_result) {
                backend->set_feature_owner({});
                backend->set_feature_point_flags(pipeline::FeaturePointFlags::kNone);
                return rollback(append_feature_message("authored render pass expansion failed", authored_pass_result));
            }
        }
        backend->set_feature_owner({});
        backend->set_feature_point_flags(pipeline::FeaturePointFlags::kNone);
        if (writes_render_state) {
            state_writer_contributions[contribution_index] = 1;
        }
    }

    std::vector<uint32_t> terminal_state_writers{};
    for (uint32_t source = 0; source < state_writer_contributions.size(); ++source) {
        if (state_writer_contributions[source] == 0) {
            continue;
        }
        if (state_flow.contributions[source].connects_to_present) {
            terminal_state_writers.push_back(source);
            continue;
        }
        std::vector<uint8_t> visited(state_writer_contributions.size(), 0);
        std::vector<uint32_t> pending = state_flow.contributions[source].output_contributions;
        bool reaches_later_writer = false;
        for (std::size_t cursor = 0; cursor < pending.size() && !reaches_later_writer; ++cursor) {
            const uint32_t node = pending[cursor];
            if (node >= state_writer_contributions.size() || visited[node] != 0) {
                continue;
            }
            visited[node] = 1;
            if (state_writer_contributions[node] != 0) {
                reaches_later_writer = true;
                break;
            }
            pending.insert(pending.end(), state_flow.contributions[node].output_contributions.begin(),
                           state_flow.contributions[node].output_contributions.end());
        }
        if (!reaches_later_writer) {
            terminal_state_writers.push_back(source);
        }
    }
    if (impl_->desc.state.enabled && impl_->desc.state.present_target_id.is_valid() &&
        !terminal_state_writers.empty()) {
        if (impl_->desc.state.present_extent.width == 0 || impl_->desc.state.present_extent.height == 0 ||
            impl_->desc.state.present_extent.depth == 0) {
            return rollback(invalid_argument("render-state Present extent is invalid"));
        }
        const auto present_target = backend->find_texture(impl_->desc.state.present_target_id);
        if (!present_target) {
            return rollback(append_feature_message("render-state Present target lookup failed", present_target.result));
        }
        const pipeline::RenderGraphTexture destination = present_target.value;
        std::vector<RenderStateCompositeInput> terminal_states{};
        terminal_states.reserve(terminal_state_writers.size());
        for (const uint32_t writer : terminal_state_writers) {
            terminal_states.push_back({state_outputs[writer], state_writer_blend_modes[writer]});
        }
        if (terminal_states.size() > 1) {
            if (!impl_->desc.state.compose_terminal_states) {
                return rollback(
                    invalid_state("multiple terminal render-state outputs require a terminal compositor callback"));
            }
            const pipeline::RenderGraphFeatureContext composite_context{
                .scope = impl_->plan->scope,
                .pipeline = impl_->plan->pipeline,
                .point_flags = pipeline::FeaturePointFlags::kMayCreateArtifacts |
                               pipeline::FeaturePointFlags::kMayCreateGraphicsPass |
                               pipeline::FeaturePointFlags::kMayCreateComputePass |
                               pipeline::FeaturePointFlags::kMayCreateCopyPass |
                               pipeline::FeaturePointFlags::kMayUseDedicatedQueue |
                               pipeline::FeaturePointFlags::kMayWriteFinalOutput,
                .user_data = impl_->desc.user_data,
                .feature_data = impl_->desc.feature_data,
            };
            backend->set_feature_point_flags(composite_context.point_flags);
            pipeline::RenderGraphBuilder composite_builder(*backend, composite_context);
            foundation::Result composite_result;
            try {
                composite_result =
                    impl_->desc.state.compose_terminal_states(composite_builder, terminal_states, destination);
            } catch (const std::bad_alloc&) {
                composite_result = foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                               "render-state terminal compositor allocation failed");
            } catch (const std::exception& exception) {
                composite_result = foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
            } catch (...) {
                composite_result = foundation::Result::failure(
                    foundation::ResultCode::kUnknown, "render-state terminal compositor threw an unknown exception");
            }
            backend->set_feature_point_flags(pipeline::FeaturePointFlags::kNone);
            if (!composite_result) {
                return rollback(append_feature_message("render-state terminal composition failed", composite_result));
            }
        } else {
            const pipeline::RenderGraphTexture source = terminal_states.front().texture;
            const rhi::Extent3D extent = impl_->desc.state.present_extent;
            const auto pass = backend->add_pass(
                "RenderState.Present", pipeline::RenderGraphPassType::kCopy, rhi::QueueClass::kCopy,
                [source, destination](pipeline::RenderPassBuilder& pass_builder) {
                    if (const foundation::Result result = pass_builder.read(
                            source, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll);
                        !result) {
                        return result;
                    }
                    if (const foundation::Result result =
                            pass_builder.write(destination, rhi::ResourceUsage::kCopyDestination,
                                               rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll, {}, true);
                        !result) {
                        return result;
                    }
                    pass_builder.set_side_effects();
                    return foundation::Result::success();
                },
                [source, destination, extent](pipeline::RenderPassExecutionContext& execution) {
                    return execution.copy_texture(source, rhi::TextureSubresourceLayers{rhi::Aspect::kColor, 0, 0, 1},
                                                  rhi::Offset3D{0, 0, 0}, destination,
                                                  rhi::TextureSubresourceLayers{rhi::Aspect::kColor, 0, 0, 1},
                                                  rhi::Offset3D{0, 0, 0}, extent);
                });
            if (!pass) {
                return rollback(append_feature_message("render-state Present copy creation failed", pass.result));
            }
        }
    }

    impl_->graph.commit_feature_build_transaction();
    impl_->plan_owner.reset();
    impl_->plan = nullptr;
    impl_->built = true;
    return foundation::Result::success();
}

foundation::Result build_render_graph_features(RenderGraph& graph, const pipeline::PipelinePlan& plan,
                                               const pipeline::PipelineRegistry& registry,
                                               const RenderGraphFeatureRuntimeDesc& desc) {
    RenderGraphFeatureRuntime runtime(graph, plan, registry, desc);
    return runtime.build();
}

foundation::Result build_render_graph_features(RenderGraph& graph, pipeline::PipelinePlanHandle plan,
                                               const pipeline::PipelineRegistry& registry,
                                               const RenderGraphFeatureRuntimeDesc& desc) {
    if (plan == nullptr) {
        return invalid_argument("render feature runtime received a null pipeline plan");
    }
    RenderGraphFeatureRuntime runtime(graph, std::move(plan), registry, desc);
    return runtime.build();
}
}  // namespace aeg::render_graph
