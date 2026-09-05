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
#include <utility>

namespace aeg::render_graph {
    namespace {
        [[nodiscard]] foundation::Result invalid_argument(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
        }

        [[nodiscard]] foundation::Result invalid_state(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState, std::move(message));
        }

        [[nodiscard]] foundation::Result not_found(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound, std::move(message));
        }

        inline constexpr uint32_t kKnownFeaturePointFlags =
            static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayCreateArtifacts) |
            static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayCreateGraphicsPass) |
            static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayCreateComputePass) |
            static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayCreateCopyPass) |
            static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayUseDedicatedQueue) |
            static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayWriteFinalOutput) |
            static_cast<uint32_t>(pipeline::FeaturePointFlags::kMayPublishSnapshot);

        [[nodiscard]] foundation::Result append_feature_message(
            std::string prefix, const foundation::Result& result) {
            if (!result.message.empty()) {
                prefix += ": ";
                prefix += result.message;
            }
            return foundation::Result::failure(result.code, std::move(prefix));
        }

        [[nodiscard]] ResourceLifetime convert_lifetime(
            pipeline::RenderGraphResourceLifetime lifetime) noexcept {
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

        [[nodiscard]] RenderGraphBuffer to_render_graph_buffer(
            pipeline::RenderGraphBuffer handle) noexcept {
            return RenderGraphBuffer{handle.index, handle.generation, handle.graph_id};
        }

        [[nodiscard]] RenderGraphTexture to_render_graph_texture(
            pipeline::RenderGraphTexture handle) noexcept {
            return RenderGraphTexture{handle.index, handle.generation, handle.graph_id};
        }

        [[nodiscard]] pipeline::RenderGraphBuffer to_pipeline_buffer(
            RenderGraphBuffer handle) noexcept {
            return pipeline::RenderGraphBuffer{handle.index, handle.generation, handle.graph_id};
        }

        [[nodiscard]] pipeline::RenderGraphTexture to_pipeline_texture(
            RenderGraphTexture handle) noexcept {
            return pipeline::RenderGraphTexture{handle.index, handle.generation, handle.graph_id};
        }

        [[nodiscard]] pipeline::RenderGraphPass to_pipeline_pass(
            RenderGraphPass handle) noexcept {
            return pipeline::RenderGraphPass{handle.index, handle.generation, handle.graph_id};
        }

        class RenderGraphBuilderBackendImpl;

        class RenderPassBuilderBackendImpl final : public pipeline::RenderPassBuilderBackend {
        public:
            RenderPassBuilderBackendImpl(std::shared_ptr<RenderGraphBuilderBackendImpl> owner,
                                         PassBuilder& builder) noexcept
                : owner_(std::move(owner)), builder_(builder) {
            }

            [[nodiscard]] foundation::Result read_buffer(
                pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
                pipeline::RenderGraphBufferRange range) override;
            [[nodiscard]] foundation::Result write_buffer(
                pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
                pipeline::RenderGraphBufferRange range, bool discard_contents) override;
            [[nodiscard]] foundation::Result read_write_buffer(
                pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
                pipeline::RenderGraphBufferRange range) override;
            [[nodiscard]] foundation::Result read_texture(
                pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
                rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
                rhi::SubresourceRange subresources) override;
            [[nodiscard]] foundation::Result write_texture(
                pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
                rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
                rhi::SubresourceRange subresources, bool discard_contents) override;
            [[nodiscard]] foundation::Result read_write_texture(
                pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
                rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
                rhi::SubresourceRange subresources) override;
            [[nodiscard]] foundation::Result use_color_attachment(
                pipeline::RenderGraphTexture texture, rhi::LoadOp load_op, rhi::StoreOp store_op,
                rhi::ClearColorValue clear_color, rhi::ResourceUsage final_usage,
                rhi::SubresourceRange subresources) override;
            [[nodiscard]] foundation::Result use_depth_stencil_attachment(
                pipeline::RenderGraphTexture texture, rhi::LoadOp depth_load_op,
                rhi::StoreOp depth_store_op, float clear_depth, uint32_t clear_stencil,
                rhi::ResourceUsage final_usage, rhi::SubresourceRange subresources) override;
            void set_side_effects(bool enabled) noexcept override;

        private:
            std::shared_ptr<RenderGraphBuilderBackendImpl> owner_{};
            PassBuilder& builder_;
        };

        class RenderPassExecutionContextBackendImpl final
            : public pipeline::RenderPassExecutionContextBackend {
        public:
            RenderPassExecutionContextBackendImpl(std::shared_ptr<RenderGraphBuilderBackendImpl> owner,
                                                  PassExecutionContext& context) noexcept
                : owner_(std::move(owner)), context_(context) {
            }

            [[nodiscard]] rhi::Device& device() const noexcept override;
            [[nodiscard]] rhi::CommandListHandle command_list() const noexcept override;
            [[nodiscard]] pipeline::RenderGraphPass pass() const noexcept override;
            [[nodiscard]] foundation::ResultValue<rhi::BufferHandle> buffer(
                pipeline::RenderGraphBuffer buffer) const override;
            [[nodiscard]] foundation::ResultValue<rhi::TextureHandle> texture(
                pipeline::RenderGraphTexture texture) const override;
            [[nodiscard]] foundation::ResultValue<rhi::BufferViewHandle> buffer_view(
                pipeline::RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset,
                uint64_t size, rhi::Format format, uint32_t structure_stride) override;
            [[nodiscard]] foundation::ResultValue<rhi::TextureViewHandle> texture_view(
                pipeline::RenderGraphTexture texture, rhi::TextureViewKind kind,
                rhi::SubresourceRange subresources, rhi::Format format,
                rhi::TextureViewDimension dimension) override;
            [[nodiscard]] foundation::Result begin_rendering() override;
            [[nodiscard]] foundation::Result end_rendering() override;
            [[nodiscard]] bool rendering_active() const noexcept override;
            [[nodiscard]] bool rendering_started() const noexcept override;
            [[nodiscard]] foundation::Result copy_buffer(
                pipeline::RenderGraphBuffer source, pipeline::RenderGraphBuffer destination,
                uint64_t source_offset, uint64_t destination_offset, uint64_t size) override;
            [[nodiscard]] foundation::Result copy_buffer_to_texture(
                pipeline::RenderGraphBuffer source, uint64_t source_offset,
                uint32_t source_row_length, uint32_t source_image_height,
                pipeline::RenderGraphTexture destination,
                rhi::TextureSubresourceLayers destination_subresources,
                rhi::Offset3D destination_offset, rhi::Extent3D extent) override;
            [[nodiscard]] foundation::Result copy_texture_to_buffer(
                pipeline::RenderGraphTexture source,
                rhi::TextureSubresourceLayers source_subresources,
                rhi::Offset3D source_offset, rhi::Extent3D extent,
                pipeline::RenderGraphBuffer destination, uint64_t destination_offset,
                uint32_t destination_row_length, uint32_t destination_image_height) override;
            [[nodiscard]] foundation::Result copy_texture(
                pipeline::RenderGraphTexture source,
                rhi::TextureSubresourceLayers source_subresources,
                rhi::Offset3D source_offset, pipeline::RenderGraphTexture destination,
                rhi::TextureSubresourceLayers destination_subresources,
                rhi::Offset3D destination_offset, rhi::Extent3D extent) override;
            [[nodiscard]] foundation::Result clear_texture(
                pipeline::RenderGraphTexture texture, rhi::SubresourceRange subresources,
                rhi::ClearColorValue color, float depth, uint32_t stencil) override;

        private:
            std::shared_ptr<RenderGraphBuilderBackendImpl> owner_{};
            PassExecutionContext& context_;
        };

        class RenderGraphBuilderBackendImpl final
            : public pipeline::RenderGraphBuilderBackend,
              public std::enable_shared_from_this<RenderGraphBuilderBackendImpl> {
        public:
            explicit RenderGraphBuilderBackendImpl(RenderGraph& graph) noexcept : graph_(graph) {
            }

            [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> create_buffer(
                pipeline::RenderResourceId id, const rhi::BufferDesc& desc,
                pipeline::RenderGraphResourceLifetime lifetime) override;
            [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> create_texture(
                pipeline::RenderResourceId id, const rhi::TextureDesc& desc,
                pipeline::RenderGraphResourceLifetime lifetime) override;
            [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> import_buffer(
                pipeline::RenderResourceId id, rhi::BufferHandle buffer,
                const rhi::BufferDesc& desc, const rhi::ResourceState& initial_state) override;
            [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> import_texture(
                pipeline::RenderResourceId id, rhi::TextureHandle texture,
                rhi::TextureViewHandle default_view, const rhi::TextureDesc& desc,
                const rhi::ResourceState& initial_state) override;
            [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> import_swapchain_image(
                pipeline::RenderResourceId id, const rhi::FrameAcquireResult& acquired,
                const rhi::TextureDesc& desc, const rhi::ResourceState& initial_state) override;
            [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> find_buffer(
                pipeline::RenderResourceId id) const override;
            [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphTexture> find_texture(
                pipeline::RenderResourceId id) const override;
            [[nodiscard]] foundation::Result export_buffer(
                pipeline::RenderGraphBuffer buffer) override;
            [[nodiscard]] foundation::Result export_texture(
                pipeline::RenderGraphTexture texture) override;
            void set_feature_owner(
                std::shared_ptr<const pipeline::PipelineFeature> owner) override {
                current_feature_owner_ = std::move(owner);
            }
            void set_feature_point_flags(pipeline::FeaturePointFlags point_flags) noexcept {
                current_feature_point_flags_ = point_flags;
            }
            [[nodiscard]] foundation::ResultValue<pipeline::RenderGraphPass> add_pass(
                std::string_view name, pipeline::RenderGraphPassType type,
                rhi::QueueClass queue, pipeline::RenderPassSetupFunction setup,
                pipeline::RenderPassExecuteFunction execute) override;

            [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> resolve_buffer(
                pipeline::RenderGraphBuffer buffer) const;
            [[nodiscard]] foundation::ResultValue<RenderGraphTexture> resolve_texture(
                pipeline::RenderGraphTexture texture) const;

            [[nodiscard]] foundation::Result register_initial_resources(
                std::span<const RenderGraphFeatureBufferBinding> buffers,
                std::span<const RenderGraphFeatureTextureBinding> textures);

        private:
            [[nodiscard]] bool resource_id_taken(pipeline::RenderResourceId id) const noexcept;
            [[nodiscard]] foundation::Result remember_buffer(
                pipeline::RenderResourceId id, pipeline::RenderGraphBuffer handle);
            [[nodiscard]] foundation::Result remember_texture(
                pipeline::RenderResourceId id, pipeline::RenderGraphTexture handle);

            RenderGraph& graph_;
            std::unordered_map<pipeline::RenderResourceId, pipeline::RenderGraphBuffer,
                               pipeline::StableIdHash<pipeline::RenderGraphResourceIdTag>>
                buffers_{};
            std::unordered_map<pipeline::RenderResourceId, pipeline::RenderGraphTexture,
                               pipeline::StableIdHash<pipeline::RenderGraphResourceIdTag>>
                textures_{};
            std::shared_ptr<const pipeline::PipelineFeature> current_feature_owner_{};
            pipeline::FeaturePointFlags current_feature_point_flags_{
                pipeline::FeaturePointFlags::kNone};

            friend class RenderPassBuilderBackendImpl;
            friend class RenderPassExecutionContextBackendImpl;
        };

        [[nodiscard]] foundation::Result validate_render_feature_plan(
            const pipeline::PipelinePlan& plan) {
            if (plan.revision == 0 || !plan.pipeline.is_valid()) {
                return invalid_state("render feature runtime received an invalid pipeline plan");
            }

            if (static_cast<uint8_t>(plan.scope) >
                static_cast<uint8_t>(pipeline::PipelineScope::kEditor)) {
                return invalid_state("render feature runtime received a plan with an invalid scope");
            }

            for (const pipeline::PipelineFeatureContributionRecord& contribution :
                 plan.feature_contributions) {
                if (!contribution.feature_instance.is_valid() || !contribution.feature.is_valid() ||
                    !contribution.point.is_valid() ||
                    contribution.domain == pipeline::FeatureDomain::kComposite ||
                    (static_cast<uint32_t>(contribution.point_flags) & ~kKnownFeaturePointFlags) != 0) {
                    return invalid_state("render feature runtime received an invalid feature contribution");
                }
            }

            for (size_t implementation_index = 0;
                 implementation_index < plan.feature_implementations.size(); ++implementation_index) {
                const pipeline::PipelineFeatureImplementationRecord& implementation =
                    plan.feature_implementations[implementation_index];
                if (!implementation.feature_instance.is_valid() || !implementation.feature.is_valid() ||
                    implementation.implementation == nullptr) {
                    return invalid_state("render feature runtime received an invalid feature implementation");
                }
                for (size_t duplicate_index = implementation_index + 1;
                     duplicate_index < plan.feature_implementations.size(); ++duplicate_index) {
                    const pipeline::PipelineFeatureImplementationRecord& duplicate =
                        plan.feature_implementations[duplicate_index];
                    if (duplicate.feature_instance == implementation.feature_instance &&
                        duplicate.feature == implementation.feature && duplicate.part == implementation.part) {
                        return invalid_state(
                            "render feature runtime received duplicate feature implementations");
                    }
                }
            }

            for (const pipeline::PipelineFeatureContributionRecord& contribution :
                 plan.feature_contributions) {
                if (contribution.domain != pipeline::FeatureDomain::kRender) {
                    continue;
                }
                const auto implementation = std::find_if(
                    plan.feature_implementations.begin(), plan.feature_implementations.end(),
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
                    return invalid_state(
                        "render feature implementation does not satisfy its contribution contract");
                }
            }
            return foundation::Result::success();
        }

        [[nodiscard]] std::shared_ptr<const pipeline::PipelineFeature>
        find_plan_feature_implementation(
            const pipeline::PipelinePlan& plan,
            const pipeline::PipelineFeatureContributionRecord& contribution) {
            const auto iterator = std::find_if(
                plan.feature_implementations.begin(), plan.feature_implementations.end(),
                [&](const pipeline::PipelineFeatureImplementationRecord& implementation) {
                    return implementation.feature_instance == contribution.feature_instance &&
                           implementation.feature == contribution.feature &&
                           implementation.part == contribution.part;
                });
            return iterator == plan.feature_implementations.end() ? nullptr : iterator->implementation;
        }
    }  // namespace

    foundation::ResultValue<pipeline::RenderGraphBuffer> RenderGraphBuilderBackendImpl::create_buffer(
        pipeline::RenderResourceId id, const rhi::BufferDesc& desc,
        pipeline::RenderGraphResourceLifetime lifetime) {
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
        if (const foundation::Result remember_result = remember_buffer(id, handle);
            !remember_result) {
            return {remember_result, {}};
        }
        return {foundation::Result::success(), handle};
    }

    foundation::ResultValue<pipeline::RenderGraphTexture> RenderGraphBuilderBackendImpl::create_texture(
        pipeline::RenderResourceId id, const rhi::TextureDesc& desc,
        pipeline::RenderGraphResourceLifetime lifetime) {
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
        if (const foundation::Result remember_result = remember_texture(id, handle);
            !remember_result) {
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
        if (const foundation::Result remember_result = remember_buffer(id, handle);
            !remember_result) {
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
        if (const foundation::Result remember_result = remember_texture(id, handle);
            !remember_result) {
            return {remember_result, {}};
        }
        return {foundation::Result::success(), handle};
    }

    foundation::ResultValue<pipeline::RenderGraphTexture>
    RenderGraphBuilderBackendImpl::import_swapchain_image(
        pipeline::RenderResourceId id, const rhi::FrameAcquireResult& acquired,
        const rhi::TextureDesc& desc, const rhi::ResourceState& initial_state) {
        if (resource_id_taken(id)) {
            return {invalid_state("render graph resource id is already registered"), {}};
        }
        auto result = graph_.import_swapchain_image(acquired, desc, initial_state);
        if (!result) {
            return {result.result, {}};
        }
        const pipeline::RenderGraphTexture handle = to_pipeline_texture(result.value);
        if (const foundation::Result remember_result = remember_texture(id, handle);
            !remember_result) {
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
            return {not_found("render graph texture resource id is not registered"), {}};
        }
        return {foundation::Result::success(), iterator->second};
    }

    foundation::Result RenderGraphBuilderBackendImpl::export_buffer(
        pipeline::RenderGraphBuffer buffer) {
        return graph_.export_buffer(to_render_graph_buffer(buffer));
    }

    foundation::Result RenderGraphBuilderBackendImpl::export_texture(
        pipeline::RenderGraphTexture texture) {
        return graph_.export_texture(to_render_graph_texture(texture));
    }

    foundation::ResultValue<pipeline::RenderGraphPass> RenderGraphBuilderBackendImpl::add_pass(
        std::string_view name, pipeline::RenderGraphPassType type, rhi::QueueClass queue,
        pipeline::RenderPassSetupFunction setup, pipeline::RenderPassExecuteFunction execute) {
        static_cast<void>(type);
        const std::shared_ptr<RenderGraphBuilderBackendImpl> self = shared_from_this();
        const std::shared_ptr<const pipeline::PipelineFeature> feature_owner = current_feature_owner_;
        const pipeline::FeaturePointFlags point_flags = current_feature_point_flags_;

        PassSetupCallback setup_callback = [self, feature_owner,
                                             point_flags,
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
                                                execute = std::move(execute)](
                                                   PassExecutionContext& context) mutable {
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

    foundation::Result RenderGraphBuilderBackendImpl::remember_buffer(
        pipeline::RenderResourceId id, pipeline::RenderGraphBuffer handle) {
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

    foundation::Result RenderGraphBuilderBackendImpl::remember_texture(
        pipeline::RenderResourceId id, pipeline::RenderGraphTexture handle) {
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
            return foundation::Result::failure(
                foundation::ResultCode::kOutOfMemory,
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
            if (const foundation::Result result = remember_buffer(
                    binding.id, to_pipeline_buffer(binding.buffer));
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
            if (const foundation::Result result = remember_texture(
                    binding.id, to_pipeline_texture(binding.texture));
                !result) {
                return result;
            }
        }
        return foundation::Result::success();
    }

    foundation::Result RenderPassBuilderBackendImpl::read_buffer(
        pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
        pipeline::RenderGraphBufferRange range) {
        const auto resolved = owner_->resolve_buffer(buffer);
        if (!resolved) {
            return resolved.result;
        }
        return builder_.read(resolved.value, usage, pipeline_stages, shader_stages,
                             RenderGraphBufferRange{range.offset, range.size});
    }

    foundation::Result RenderPassBuilderBackendImpl::write_buffer(
        pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
        pipeline::RenderGraphBufferRange range, bool discard_contents) {
        const auto resolved = owner_->resolve_buffer(buffer);
        if (!resolved) {
            return resolved.result;
        }
        return builder_.write(resolved.value, usage, pipeline_stages, shader_stages,
                              RenderGraphBufferRange{range.offset, range.size}, discard_contents);
    }

    foundation::Result RenderPassBuilderBackendImpl::read_write_buffer(
        pipeline::RenderGraphBuffer buffer, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
        pipeline::RenderGraphBufferRange range) {
        const auto resolved = owner_->resolve_buffer(buffer);
        if (!resolved) {
            return resolved.result;
        }
        return builder_.read_write(resolved.value, usage, pipeline_stages, shader_stages,
                                   RenderGraphBufferRange{range.offset, range.size});
    }

    foundation::Result RenderPassBuilderBackendImpl::read_texture(
        pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
        rhi::SubresourceRange subresources) {
        const auto resolved = owner_->resolve_texture(texture);
        if (!resolved) {
            return resolved.result;
        }
        return builder_.read(resolved.value, usage, pipeline_stages, shader_stages, subresources);
    }

    foundation::Result RenderPassBuilderBackendImpl::write_texture(
        pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
        rhi::SubresourceRange subresources, bool discard_contents) {
        const auto resolved = owner_->resolve_texture(texture);
        if (!resolved) {
            return resolved.result;
        }
        return builder_.write(resolved.value, usage, pipeline_stages, shader_stages,
                              subresources, discard_contents);
    }

    foundation::Result RenderPassBuilderBackendImpl::read_write_texture(
        pipeline::RenderGraphTexture texture, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
        rhi::SubresourceRange subresources) {
        const auto resolved = owner_->resolve_texture(texture);
        if (!resolved) {
            return resolved.result;
        }
        return builder_.read_write(resolved.value, usage, pipeline_stages, shader_stages, subresources);
    }

    foundation::Result RenderPassBuilderBackendImpl::use_color_attachment(
        pipeline::RenderGraphTexture texture, rhi::LoadOp load_op, rhi::StoreOp store_op,
        rhi::ClearColorValue clear_color, rhi::ResourceUsage final_usage,
        rhi::SubresourceRange subresources) {
        const auto resolved = owner_->resolve_texture(texture);
        if (!resolved) {
            return resolved.result;
        }
        return builder_.use_color_attachment(resolved.value, load_op, store_op, clear_color,
                                             final_usage, subresources);
    }

    foundation::Result RenderPassBuilderBackendImpl::use_depth_stencil_attachment(
        pipeline::RenderGraphTexture texture, rhi::LoadOp depth_load_op,
        rhi::StoreOp depth_store_op, float clear_depth, uint32_t clear_stencil,
        rhi::ResourceUsage final_usage, rhi::SubresourceRange subresources) {
        const auto resolved = owner_->resolve_texture(texture);
        if (!resolved) {
            return resolved.result;
        }
        return builder_.use_depth_stencil_attachment(resolved.value, depth_load_op, depth_store_op,
                                                     clear_depth, clear_stencil, final_usage,
                                                     subresources);
    }

    void RenderPassBuilderBackendImpl::set_side_effects(bool enabled) noexcept {
        builder_.set_side_effects(enabled);
    }

    rhi::Device& RenderPassExecutionContextBackendImpl::device() const noexcept {
        return context_.device();
    }

    rhi::CommandListHandle RenderPassExecutionContextBackendImpl::command_list() const noexcept {
        return context_.command_list();
    }

    pipeline::RenderGraphPass RenderPassExecutionContextBackendImpl::pass() const noexcept {
        return to_pipeline_pass(context_.pass());
    }

    foundation::ResultValue<rhi::BufferHandle> RenderPassExecutionContextBackendImpl::buffer(
        pipeline::RenderGraphBuffer buffer) const {
        const auto resolved = owner_->resolve_buffer(buffer);
        if (!resolved) {
            return {resolved.result, {}};
        }
        return context_.buffer(resolved.value);
    }

    foundation::ResultValue<rhi::TextureHandle> RenderPassExecutionContextBackendImpl::texture(
        pipeline::RenderGraphTexture texture) const {
        const auto resolved = owner_->resolve_texture(texture);
        if (!resolved) {
            return {resolved.result, {}};
        }
        return context_.texture(resolved.value);
    }

    foundation::ResultValue<rhi::BufferViewHandle> RenderPassExecutionContextBackendImpl::buffer_view(
        pipeline::RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset,
        uint64_t size, rhi::Format format, uint32_t structure_stride) {
        const auto resolved = owner_->resolve_buffer(buffer);
        if (!resolved) {
            return {resolved.result, {}};
        }
        return context_.buffer_view(resolved.value, kind, offset, size, format, structure_stride);
    }

    foundation::ResultValue<rhi::TextureViewHandle> RenderPassExecutionContextBackendImpl::texture_view(
        pipeline::RenderGraphTexture texture, rhi::TextureViewKind kind,
        rhi::SubresourceRange subresources, rhi::Format format,
        rhi::TextureViewDimension dimension) {
        const auto resolved = owner_->resolve_texture(texture);
        if (!resolved) {
            return {resolved.result, {}};
        }
        return context_.texture_view(resolved.value, kind, subresources, format, dimension);
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

    foundation::Result RenderPassExecutionContextBackendImpl::copy_buffer(
        pipeline::RenderGraphBuffer source, pipeline::RenderGraphBuffer destination,
        uint64_t source_offset, uint64_t destination_offset, uint64_t size) {
        const auto resolved_source = owner_->resolve_buffer(source);
        if (!resolved_source) {
            return resolved_source.result;
        }
        const auto resolved_destination = owner_->resolve_buffer(destination);
        if (!resolved_destination) {
            return resolved_destination.result;
        }
        return context_.copy_buffer(resolved_source.value, resolved_destination.value,
                                    source_offset, destination_offset, size);
    }

    foundation::Result RenderPassExecutionContextBackendImpl::copy_buffer_to_texture(
        pipeline::RenderGraphBuffer source, uint64_t source_offset, uint32_t source_row_length,
        uint32_t source_image_height, pipeline::RenderGraphTexture destination,
        rhi::TextureSubresourceLayers destination_subresources,
        rhi::Offset3D destination_offset, rhi::Extent3D extent) {
        const auto resolved_source = owner_->resolve_buffer(source);
        if (!resolved_source) {
            return resolved_source.result;
        }
        const auto resolved_destination = owner_->resolve_texture(destination);
        if (!resolved_destination) {
            return resolved_destination.result;
        }
        return context_.copy_buffer_to_texture(resolved_source.value, source_offset, source_row_length,
                                               source_image_height, resolved_destination.value,
                                               destination_subresources, destination_offset, extent);
    }

    foundation::Result RenderPassExecutionContextBackendImpl::copy_texture_to_buffer(
        pipeline::RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources,
        rhi::Offset3D source_offset, rhi::Extent3D extent,
        pipeline::RenderGraphBuffer destination, uint64_t destination_offset,
        uint32_t destination_row_length, uint32_t destination_image_height) {
        const auto resolved_source = owner_->resolve_texture(source);
        if (!resolved_source) {
            return resolved_source.result;
        }
        const auto resolved_destination = owner_->resolve_buffer(destination);
        if (!resolved_destination) {
            return resolved_destination.result;
        }
        return context_.copy_texture_to_buffer(resolved_source.value, source_subresources,
                                               source_offset, extent, resolved_destination.value,
                                               destination_offset, destination_row_length,
                                               destination_image_height);
    }

    foundation::Result RenderPassExecutionContextBackendImpl::copy_texture(
        pipeline::RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources,
        rhi::Offset3D source_offset, pipeline::RenderGraphTexture destination,
        rhi::TextureSubresourceLayers destination_subresources,
        rhi::Offset3D destination_offset, rhi::Extent3D extent) {
        const auto resolved_source = owner_->resolve_texture(source);
        if (!resolved_source) {
            return resolved_source.result;
        }
        const auto resolved_destination = owner_->resolve_texture(destination);
        if (!resolved_destination) {
            return resolved_destination.result;
        }
        return context_.copy_texture(resolved_source.value, source_subresources, source_offset,
                                     resolved_destination.value, destination_subresources,
                                     destination_offset, extent);
    }

    foundation::Result RenderPassExecutionContextBackendImpl::clear_texture(
        pipeline::RenderGraphTexture texture, rhi::SubresourceRange subresources,
        rhi::ClearColorValue color, float depth, uint32_t stencil) {
        const auto resolved = owner_->resolve_texture(texture);
        if (!resolved) {
            return resolved.result;
        }
        return context_.clear_texture(resolved.value, subresources, color, depth, stencil);
    }

    struct RenderGraphFeatureRuntime::Impl final {
        Impl(RenderGraph& graph_reference, const pipeline::PipelinePlan& plan_reference,
             const pipeline::PipelineRegistry& registry_reference,
             const RenderGraphFeatureRuntimeDesc& desc_reference)
            : graph(graph_reference), plan(&plan_reference), registry(registry_reference), desc(desc_reference) {
        }

        Impl(RenderGraph& graph_reference, pipeline::PipelinePlanHandle plan_reference,
             const pipeline::PipelineRegistry& registry_reference,
             const RenderGraphFeatureRuntimeDesc& desc_reference)
            : graph(graph_reference), plan(plan_reference.get()), plan_owner(std::move(plan_reference)),
              registry(registry_reference), desc(desc_reference) {
        }

        RenderGraph& graph;
        const pipeline::PipelinePlan* plan{nullptr};
        pipeline::PipelinePlanHandle plan_owner{};
        const pipeline::PipelineRegistry& registry;
        RenderGraphFeatureRuntimeDesc desc{};
        bool built{false};
    };

    RenderGraphFeatureRuntime::RenderGraphFeatureRuntime(
        RenderGraph& graph, const pipeline::PipelinePlan& plan,
        const pipeline::PipelineRegistry& registry, const RenderGraphFeatureRuntimeDesc& desc)
        : impl_(std::make_unique<Impl>(graph, plan, registry, desc)) {
    }

    RenderGraphFeatureRuntime::~RenderGraphFeatureRuntime() = default;

    RenderGraphFeatureRuntime::RenderGraphFeatureRuntime(
        RenderGraph& graph, pipeline::PipelinePlanHandle plan,
        const pipeline::PipelineRegistry& registry, const RenderGraphFeatureRuntimeDesc& desc)
        : impl_(std::make_unique<Impl>(graph, std::move(plan), registry, desc)) {
    }

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
        if (const foundation::Result plan_result = validate_render_feature_plan(*impl_->plan);
            !plan_result) {
            return plan_result;
        }

        if (const foundation::Result transaction_result =
                    impl_->graph.begin_feature_build_transaction();
            !transaction_result) {
            return transaction_result;
        }
        const auto rollback = [this](foundation::Result result) {
            impl_->graph.rollback_feature_build_transaction();
            return result;
        };

        if (impl_->plan_owner != nullptr) {
            const std::shared_ptr<const void> plan_owner = impl_->plan_owner;
            if (const foundation::Result result = impl_->graph.retain_execution_owner(plan_owner);
                !result) {
                return rollback(result);
            }
        }

        std::shared_ptr<RenderGraphBuilderBackendImpl> backend;
        try {
            backend = std::make_shared<RenderGraphBuilderBackendImpl>(impl_->graph);
        } catch (const std::bad_alloc&) {
            return rollback(foundation::Result::failure(
                    foundation::ResultCode::kOutOfMemory,
                    "render feature backend allocation failed"));
        }
        if (const foundation::Result result = backend->register_initial_resources(
                impl_->desc.buffers, impl_->desc.textures);
            !result) {
            return rollback(result);
        }

        for (const pipeline::PipelineFeatureContributionRecord& contribution :
             impl_->plan->feature_contributions) {
            if (contribution.domain != pipeline::FeatureDomain::kRender) {
                continue;
            }

            std::shared_ptr<const pipeline::PipelineFeature> feature_implementation =
                find_plan_feature_implementation(*impl_->plan, contribution);
            if (feature_implementation == nullptr) {
                const auto feature_result = impl_->registry.find_feature(contribution.feature);
                if (!feature_result) {
                    return rollback(append_feature_message(
                            "render feature lookup failed", feature_result.result));
                }
                feature_implementation = feature_result.value;
            }
            if ((feature_implementation->feature_domain_mask() &
                 pipeline::to_mask(pipeline::FeatureDomain::kRender)) == 0) {
                return rollback(invalid_state(
                        "render feature contribution has no render domain implementation"));
            }

            backend->set_feature_owner(feature_implementation);
            backend->set_feature_point_flags(contribution.point_flags);
            const pipeline::RenderGraphFeatureContext feature_context{
                impl_->plan->scope,
                impl_->plan->pipeline,
                contribution.feature,
                contribution.feature_instance,
                contribution.part,
                contribution.point,
                contribution.point_position,
                contribution.point_flags,
                impl_->desc.user_data,
            };
            pipeline::RenderGraphBuilder builder(*backend, feature_context);

            foundation::Result build_result;
            try {
                build_result = feature_implementation->build_render_graph(builder);
            } catch (const std::bad_alloc&) {
                build_result = foundation::Result::failure(
                    foundation::ResultCode::kOutOfMemory,
                    "render feature graph build allocation failed");
            } catch (const std::exception& exception) {
                build_result = foundation::Result::failure(
                    foundation::ResultCode::kUnknown, exception.what());
            } catch (...) {
                build_result = foundation::Result::failure(
                    foundation::ResultCode::kUnknown,
                    "render feature graph build threw an unknown exception");
            }
            backend->set_feature_owner({});
            backend->set_feature_point_flags(pipeline::FeaturePointFlags::kNone);
            if (!build_result) {
                return rollback(append_feature_message(
                        "render feature graph build failed", build_result));
            }
        }

        impl_->graph.commit_feature_build_transaction();
        impl_->plan_owner.reset();
        impl_->plan = nullptr;
        impl_->built = true;
        return foundation::Result::success();
    }

    foundation::Result build_render_graph_features(
        RenderGraph& graph, const pipeline::PipelinePlan& plan,
        const pipeline::PipelineRegistry& registry, const RenderGraphFeatureRuntimeDesc& desc) {
        RenderGraphFeatureRuntime runtime(graph, plan, registry, desc);
        return runtime.build();
    }

    foundation::Result build_render_graph_features(
        RenderGraph& graph, pipeline::PipelinePlanHandle plan,
        const pipeline::PipelineRegistry& registry, const RenderGraphFeatureRuntimeDesc& desc) {
        if (plan == nullptr) {
            return invalid_argument("render feature runtime received a null pipeline plan");
        }
        RenderGraphFeatureRuntime runtime(graph, std::move(plan), registry, desc);
        return runtime.build();
    }
}  // namespace aeg::render_graph
