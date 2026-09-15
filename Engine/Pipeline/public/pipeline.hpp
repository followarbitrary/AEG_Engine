/**
 * @file pipeline.hpp
 * @brief Public interfaces for defining, compiling, and executing pipelines.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "concurrency.hpp"
#include "pipeline_types.hpp"
#include "rhi_struct.hpp"

namespace aeg::rhi {
class Device;
}

namespace aeg::pipeline {
class FeatureBuildContext;
class ArtifactRegistry;
class ArtifactScopeBridge;
class PipelineFeature;
class RenderGraphBuilder;
class RenderGraphBuilderBackend;
class RenderPassBuilder;
class RenderPassBuilderBackend;
class RenderPassExecutionContext;
class RenderPassExecutionContextBackend;

using RenderPassSetupFunction = std::function<foundation::Result(RenderPassBuilder&)>;
using RenderPassExecuteFunction = std::function<foundation::Result(RenderPassExecutionContext&)>;

template <FeatureDomain Domain>
class DomainGraphBuilder;

using LogicGraphBuilder = DomainGraphBuilder<FeatureDomain::kLogic>;
using PhysicsGraphBuilder = DomainGraphBuilder<FeatureDomain::kPhysics>;
using AudioGraphBuilder = DomainGraphBuilder<FeatureDomain::kAudio>;
using StreamGraphBuilder = DomainGraphBuilder<FeatureDomain::kStream>;

/** @brief Declares how an explicit RenderFeature compositor combines state textures. */
enum class RenderStateBlendMode : uint8_t {
    /** The compositor replaces the destination with the selected state. */
    kReplace,
    /** The compositor alpha-composites each state over the destination. */
    kOver,
    /** The compositor additively combines state colors. */
    kAdd,
};

/** @brief Runtime metadata associated with one render-feature contribution. */
struct RenderGraphFeatureContext {
    PipelineScope scope{PipelineScope::kFrame};
    PipelineId pipeline{};
    FeatureId feature{};
    FeatureInstanceId feature_instance{};
    FeaturePartId part{};
    FeaturePointId feature_point{};
    FeaturePointPosition point_position{FeaturePointPosition::kBefore};
    FeaturePointFlags point_flags{FeaturePointFlags::kNone};
    /** State texture produced by this contribution when RenderGraph state composition is enabled. */
    RenderGraphTexture state_output{};
    /** State texture forwarded from an upstream contribution; invalid until an input link is resolved. */
    RenderGraphTexture state_input{};
    /** All state textures entering this contribution, in compiled Pipeline edge order. */
    std::span<const RenderGraphTexture> state_inputs{};
    /** Number of independent state layers represented by this contribution. */
    uint32_t state_layer_count{0};
    void* user_data{nullptr};
    const void* feature_data{nullptr};

    [[nodiscard]] bool allows(FeaturePointFlags flag) const noexcept { return has_flag(point_flags, flag); }
};

/** @brief Backend adapter for render-pass resource declarations. */
class RenderPassBuilderBackend {
public:
    virtual ~RenderPassBuilderBackend() = default;

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
};

/** @brief Backend adapter for render-pass command recording. */
class RenderPassExecutionContextBackend {
public:
    virtual ~RenderPassExecutionContextBackend() = default;

    [[nodiscard]] virtual RenderGraphPass pass() const noexcept = 0;
    [[nodiscard]] virtual foundation::ResultValue<rhi::BufferHandle> buffer(RenderGraphBuffer buffer) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<rhi::TextureHandle> texture(RenderGraphTexture texture) const = 0;
    /** @brief Resolves a view prepared before the pass execute callback. */
    [[nodiscard]] virtual foundation::ResultValue<rhi::BufferViewHandle> buffer_view(RenderGraphBuffer buffer,
                                                                                     rhi::BufferViewKind kind,
                                                                                     uint64_t offset, uint64_t size,
                                                                                     rhi::Format format,
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
    [[nodiscard]] virtual foundation::Result copy_texture(RenderGraphTexture source,
                                                          rhi::TextureSubresourceLayers source_subresources,
                                                          rhi::Offset3D source_offset, RenderGraphTexture destination,
                                                          rhi::TextureSubresourceLayers destination_subresources,
                                                          rhi::Offset3D destination_offset, rhi::Extent3D extent) = 0;
    [[nodiscard]] virtual foundation::Result clear_texture(RenderGraphTexture texture,
                                                           rhi::SubresourceRange subresources,
                                                           rhi::ClearColorValue color, float depth,
                                                           uint32_t stencil) = 0;
};

/** @brief Backend adapter for logical resources and render-pass creation. */
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
    virtual void set_feature_owner(std::shared_ptr<const PipelineFeature> owner) = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderGraphPass> add_pass(std::string_view name,
                                                                            RenderGraphPassType type,
                                                                            rhi::QueueClass queue,
                                                                            RenderPassSetupFunction setup,
                                                                            RenderPassExecuteFunction execute) = 0;
};

/** @brief Public facade used by RenderFeature declaration and graph build callbacks. */
class RenderGraphBuilder final {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kRender;

    [[nodiscard]] PipelineStageHandle add_stage(const StageDescriptor& descriptor);
    [[nodiscard]] PipelineTaskHandle add_task(const TaskDescriptor& descriptor);
    [[nodiscard]] PipelineArtifactHandle declare_artifact(const ArtifactDescriptor& descriptor);
    [[nodiscard]] foundation::Result bind_task(PipelineTaskHandle task, PipelineStageHandle stage);
    [[nodiscard]] foundation::Result reads(PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result writes(PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result publish_snapshot(PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result runs_before(PipelineStageHandle stage, StageId other);
    [[nodiscard]] foundation::Result runs_after(PipelineStageHandle stage, StageId other);
    [[nodiscard]] foundation::Result expose_artifact(ArtifactId artifact, PipelineScope target_scope);

    [[nodiscard]] PipelineScope scope() const noexcept;
    [[nodiscard]] PipelineId pipeline_id() const noexcept;
    [[nodiscard]] FeatureId feature_id() const noexcept;
    [[nodiscard]] FeatureInstanceId feature_instance_id() const noexcept;
    [[nodiscard]] FeaturePartId part_id() const noexcept;
    [[nodiscard]] FeaturePointId feature_point() const noexcept;
    [[nodiscard]] FeaturePointPosition feature_point_position() const noexcept;
    [[nodiscard]] FeaturePointFlags feature_point_flags() const noexcept;
    [[nodiscard]] void* user_data() const noexcept;
    [[nodiscard]] const void* feature_data() const noexcept;
    [[nodiscard]] RenderGraphTexture state_input() const noexcept;
    [[nodiscard]] std::span<const RenderGraphTexture> state_inputs() const noexcept;
    [[nodiscard]] RenderGraphTexture state_output() const noexcept;
    [[nodiscard]] uint32_t state_layer_count() const noexcept;
    [[nodiscard]] bool allows(FeaturePointFlags flag) const noexcept;
    [[nodiscard]] bool is_runtime() const noexcept;

    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> create_buffer(
        const rhi::BufferDesc& desc, RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::kTransient);
    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> create_buffer(
        RenderResourceId id, const rhi::BufferDesc& desc,
        RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::kTransient);
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> create_texture(
        const rhi::TextureDesc& desc, RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::kTransient);
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> create_texture(
        RenderResourceId id, const rhi::TextureDesc& desc,
        RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::kTransient);
    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> import_buffer(
        RenderResourceId id, rhi::BufferHandle buffer, const rhi::BufferDesc& desc,
        const rhi::ResourceState& initial_state = {});
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> import_texture(
        RenderResourceId id, rhi::TextureHandle texture, rhi::TextureViewHandle default_view,
        const rhi::TextureDesc& desc, const rhi::ResourceState& initial_state = {});
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> import_swapchain_image(
        RenderResourceId id, const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
        const rhi::ResourceState& initial_state = {});
    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> find_buffer(RenderResourceId id) const;
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> find_texture(RenderResourceId id) const;
    [[nodiscard]] foundation::Result bind_buffer(RenderResourceId id, RenderGraphBuffer buffer);
    [[nodiscard]] foundation::Result bind_texture(RenderResourceId id, RenderGraphTexture texture);
    [[nodiscard]] foundation::Result export_buffer(RenderGraphBuffer buffer);
    [[nodiscard]] foundation::Result export_texture(RenderGraphTexture texture);

    [[nodiscard]] foundation::ResultValue<RenderGraphPass> add_pass(std::string_view name, RenderGraphPassType type,
                                                                    rhi::QueueClass queue,
                                                                    RenderPassSetupFunction setup,
                                                                    RenderPassExecuteFunction execute);
    [[nodiscard]] foundation::ResultValue<RenderGraphPass> add_graphics_pass(
        std::string_view name, RenderPassSetupFunction setup, RenderPassExecuteFunction execute,
        rhi::QueueClass queue = rhi::QueueClass::kGraphics);
    [[nodiscard]] foundation::ResultValue<RenderGraphPass> add_compute_pass(
        std::string_view name, RenderPassSetupFunction setup, RenderPassExecuteFunction execute,
        rhi::QueueClass queue = rhi::QueueClass::kCompute);
    [[nodiscard]] foundation::ResultValue<RenderGraphPass> add_copy_pass(
        std::string_view name, RenderPassSetupFunction setup, RenderPassExecuteFunction execute,
        rhi::QueueClass queue = rhi::QueueClass::kCopy);

    /** @brief Creates a runtime facade backed by a RenderGraph adapter. */
    RenderGraphBuilder(RenderGraphBuilderBackend& backend, const RenderGraphFeatureContext& context) noexcept;

private:
    explicit RenderGraphBuilder(FeatureBuildContext& context) noexcept;

    FeatureBuildContext* declaration_context_{nullptr};
    RenderGraphBuilderBackend* runtime_backend_{nullptr};
    RenderGraphFeatureContext runtime_context_{};

    friend class FeatureBuildContext;
};

/** @brief Facade used by one render-pass setup callback. */
class RenderPassBuilder final {
public:
    explicit RenderPassBuilder(RenderPassBuilderBackend& backend,
                               FeaturePointFlags point_flags = FeaturePointFlags::kNone) noexcept
        : backend_(&backend), point_flags_(point_flags) {}

    [[nodiscard]] foundation::Result read(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                          rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                          rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                          RenderGraphBufferRange range = {});
    [[nodiscard]] foundation::Result write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                           rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                           rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                           RenderGraphBufferRange range = {}, bool discard_contents = true);
    [[nodiscard]] foundation::Result read_write(
        RenderGraphBuffer buffer, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
        rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll, RenderGraphBufferRange range = {});
    [[nodiscard]] foundation::Result read(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                          rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                          rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                          rhi::SubresourceRange subresources = {});
    [[nodiscard]] foundation::Result write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                           rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                           rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                           rhi::SubresourceRange subresources = {}, bool discard_contents = true);
    [[nodiscard]] foundation::Result read_write(
        RenderGraphTexture texture, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
        rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll, rhi::SubresourceRange subresources = {});
    [[nodiscard]] foundation::Result use_color_attachment(
        RenderGraphTexture texture, rhi::LoadOp load_op, rhi::StoreOp store_op, rhi::ClearColorValue clear_color = {},
        rhi::ResourceUsage final_usage = rhi::ResourceUsage::kColorAttachmentWrite,
        rhi::SubresourceRange subresources = {});
    [[nodiscard]] foundation::Result use_depth_stencil_attachment(
        RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op, float clear_depth = 1.0f,
        uint32_t clear_stencil = 0, rhi::ResourceUsage final_usage = rhi::ResourceUsage::kDepthStencilWrite,
        rhi::SubresourceRange subresources = {});
    void set_side_effects(bool enabled = true) noexcept;

private:
    RenderPassBuilderBackend* backend_{nullptr};
    FeaturePointFlags point_flags_{FeaturePointFlags::kNone};
};

/** @brief Facade used by one render-pass command callback. */
class RenderPassExecutionContext final {
public:
    explicit RenderPassExecutionContext(RenderPassExecutionContextBackend& backend) noexcept : backend_(&backend) {}

    [[nodiscard]] RenderGraphPass pass() const noexcept;
    [[nodiscard]] foundation::ResultValue<rhi::BufferHandle> buffer(RenderGraphBuffer buffer) const;
    [[nodiscard]] foundation::ResultValue<rhi::TextureHandle> texture(RenderGraphTexture texture) const;
    [[nodiscard]] foundation::ResultValue<rhi::BufferViewHandle> buffer_view(
        RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset = 0,
        uint64_t size = std::numeric_limits<uint64_t>::max(), rhi::Format format = rhi::Format::kUnknown,
        uint32_t structure_stride = 0);
    [[nodiscard]] foundation::ResultValue<rhi::TextureViewHandle> texture_view(
        RenderGraphTexture texture, rhi::TextureViewKind kind = rhi::TextureViewKind::kSampled,
        rhi::SubresourceRange subresources = {}, rhi::Format format = rhi::Format::kUnknown,
        rhi::TextureViewDimension dimension = rhi::TextureViewDimension::kAuto);
    [[nodiscard]] foundation::Result begin_rendering();
    [[nodiscard]] foundation::Result end_rendering();
    [[nodiscard]] bool rendering_active() const noexcept;
    [[nodiscard]] bool rendering_started() const noexcept;
    [[nodiscard]] foundation::Result set_viewport(const rhi::Viewport& viewport);
    [[nodiscard]] foundation::Result set_scissor(const rhi::ScissorRect& scissor);
    [[nodiscard]] foundation::Result bind_pipeline(rhi::PipelineHandle pipeline);
    [[nodiscard]] foundation::Result bind_binding_set(uint32_t group, rhi::BindingSetHandle set,
                                                      std::span<const uint32_t> dynamic_offsets = {});
    [[nodiscard]] foundation::Result set_inline_constants(uint32_t offset, rhi::ShaderStageMask shader_stages,
                                                          std::span<const std::byte> data);
    [[nodiscard]] foundation::Result bind_vertex_buffer(uint32_t binding, rhi::BufferHandle buffer,
                                                        uint64_t offset = 0);
    [[nodiscard]] foundation::Result bind_index_buffer(rhi::BufferHandle buffer, uint64_t offset,
                                                       rhi::IndexFormat format);
    [[nodiscard]] foundation::Result draw(const rhi::DrawDesc& desc);
    [[nodiscard]] foundation::Result draw_indexed(const rhi::DrawIndexedDesc& desc);
    [[nodiscard]] foundation::Result dispatch(const rhi::DispatchDesc& desc);
    [[nodiscard]] foundation::Result dispatch_indirect(const rhi::DispatchIndirectDesc& desc);
    [[nodiscard]] foundation::Result copy_buffer(RenderGraphBuffer source, RenderGraphBuffer destination,
                                                 uint64_t source_offset, uint64_t destination_offset, uint64_t size);
    [[nodiscard]] foundation::Result copy_buffer_to_texture(RenderGraphBuffer source, uint64_t source_offset,
                                                            uint32_t source_row_length, uint32_t source_image_height,
                                                            RenderGraphTexture destination,
                                                            rhi::TextureSubresourceLayers destination_subresources,
                                                            rhi::Offset3D destination_offset, rhi::Extent3D extent);
    [[nodiscard]] foundation::Result copy_texture_to_buffer(RenderGraphTexture source,
                                                            rhi::TextureSubresourceLayers source_subresources,
                                                            rhi::Offset3D source_offset, rhi::Extent3D extent,
                                                            RenderGraphBuffer destination, uint64_t destination_offset,
                                                            uint32_t destination_row_length,
                                                            uint32_t destination_image_height);
    [[nodiscard]] foundation::Result copy_texture(RenderGraphTexture source,
                                                  rhi::TextureSubresourceLayers source_subresources,
                                                  rhi::Offset3D source_offset, RenderGraphTexture destination,
                                                  rhi::TextureSubresourceLayers destination_subresources,
                                                  rhi::Offset3D destination_offset, rhi::Extent3D extent);
    [[nodiscard]] foundation::Result clear_texture(RenderGraphTexture texture, rhi::SubresourceRange subresources = {},
                                                   rhi::ClearColorValue color = {}, float depth = 1.0f,
                                                   uint32_t stencil = 0);

private:
    RenderPassExecutionContextBackend* backend_{nullptr};
};

/** @brief Type-erased payload installed into an ArtifactRegistry. */
struct ArtifactPayload {
    ArtifactTypeId type{};
    std::shared_ptr<const void> owner{};
    const void* data{nullptr};
    std::size_t size{0};
    std::size_t alignment{alignof(std::max_align_t)};
    bool writable{false};

    /** @brief Creates an owned payload backed by a shared object. */
    template <typename Type>
    [[nodiscard]] static ArtifactPayload owned(ArtifactTypeId type, std::shared_ptr<Type> value,
                                               bool allow_writes = true) {
        ArtifactPayload payload;
        payload.type = type;
        payload.owner = std::move(value);
        payload.data = payload.owner.get();
        payload.size = sizeof(Type);
        payload.alignment = alignof(Type);
        payload.writable = allow_writes && !std::is_const_v<Type>;
        return payload;
    }

    /** @brief Creates a non-owning immutable view over external memory. */
    [[nodiscard]] static ArtifactPayload borrowed(ArtifactTypeId type, const void* data, std::size_t size,
                                                  std::size_t alignment = alignof(std::max_align_t)) {
        ArtifactPayload payload;
        payload.type = type;
        payload.owner = std::shared_ptr<const void>(data, [](const void*) {});
        payload.data = data;
        payload.size = size;
        payload.alignment = alignment;
        payload.writable = false;
        return payload;
    }
};

/** @brief Read-only runtime lease for one published artifact value. */
class ArtifactView final {
public:
    ArtifactView() noexcept = default;

    [[nodiscard]] bool is_valid() const noexcept { return valid_; }
    [[nodiscard]] ArtifactId id() const noexcept { return id_; }
    [[nodiscard]] ArtifactTypeId type() const noexcept { return type_; }
    [[nodiscard]] PipelineScope owner_scope() const noexcept { return owner_scope_; }
    [[nodiscard]] ArtifactStorage storage() const noexcept { return storage_; }
    [[nodiscard]] uint32_t version() const noexcept { return version_; }
    [[nodiscard]] ArtifactRevision revision() const noexcept { return revision_; }
    [[nodiscard]] const void* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }

    template <typename Type>
    [[nodiscard]] const Type* as(ArtifactTypeId expected_type) const noexcept {
        if (!valid_ || type_ != expected_type || data_ == nullptr || size_ < sizeof(Type)) {
            return nullptr;
        }
        return static_cast<const Type*>(data_);
    }

private:
    ArtifactView(ArtifactId id, ArtifactTypeId type, PipelineScope owner_scope, ArtifactStorage storage,
                 uint32_t version, ArtifactRevision revision, const void* data, std::size_t size, std::size_t alignment,
                 std::shared_ptr<const void> owner) noexcept
        : id_(id),
          type_(type),
          owner_scope_(owner_scope),
          storage_(storage),
          version_(version),
          revision_(revision),
          data_(data),
          size_(size),
          alignment_(alignment),
          owner_(std::move(owner)),
          valid_(true) {}

    ArtifactId id_{};
    ArtifactTypeId type_{};
    PipelineScope owner_scope_{PipelineScope::kEngine};
    ArtifactStorage storage_{ArtifactStorage::kValue};
    uint32_t version_{0};
    ArtifactRevision revision_{0};
    const void* data_{nullptr};
    std::size_t size_{0};
    std::size_t alignment_{0};
    std::shared_ptr<const void> owner_{};
    bool valid_{false};

    friend class ArtifactRegistry;
    friend class ArtifactScopeBridge;
};

/** @brief Exclusive runtime lease for writing one artifact value. */
class ArtifactWriter final {
public:
    ArtifactWriter() noexcept = default;
    ~ArtifactWriter();

    ArtifactWriter(const ArtifactWriter&) = delete;
    ArtifactWriter& operator=(const ArtifactWriter&) = delete;
    ArtifactWriter(ArtifactWriter&&) noexcept;
    ArtifactWriter& operator=(ArtifactWriter&&) noexcept;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] ArtifactId id() const noexcept;
    [[nodiscard]] ArtifactTypeId type() const noexcept;
    [[nodiscard]] PipelineScope owner_scope() const noexcept;
    [[nodiscard]] ArtifactStorage storage() const noexcept;
    [[nodiscard]] uint32_t version() const noexcept;
    [[nodiscard]] ArtifactRevision revision() const noexcept;
    [[nodiscard]] void* data() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t alignment() const noexcept;

    template <typename Type>
    [[nodiscard]] Type* as(ArtifactTypeId expected_type) const noexcept {
        if (!is_valid() || type() != expected_type || data() == nullptr || size() < sizeof(Type)) {
            return nullptr;
        }
        return static_cast<Type*>(data());
    }

    /** @brief Publishes the mutation and releases the exclusive lease. */
    [[nodiscard]] foundation::Result commit();

private:
    struct Impl;

    explicit ArtifactWriter(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_{};

    friend class ArtifactRegistry;
};

/** @brief Runtime storage and contract table for one pipeline Scope. */
class ArtifactRegistry final {
public:
    explicit ArtifactRegistry(PipelineScope scope = PipelineScope::kEngine);
    ~ArtifactRegistry();

    ArtifactRegistry(const ArtifactRegistry&) = delete;
    ArtifactRegistry& operator=(const ArtifactRegistry&) = delete;
    ArtifactRegistry(ArtifactRegistry&&) = delete;
    ArtifactRegistry& operator=(ArtifactRegistry&&) = delete;

    [[nodiscard]] PipelineScope scope() const noexcept;
    [[nodiscard]] bool contains(ArtifactId id) const noexcept;

    /** @brief Installs one immutable artifact contract without publishing a value. */
    [[nodiscard]] foundation::Result install_contract(const ArtifactDescriptor& descriptor);
    /** @brief Installs all contracts carried by a compiled plan. */
    [[nodiscard]] foundation::Result install_plan(const PipelinePlan& plan);
    /** @brief Atomically replaces the contracts owned by one installed plan. */
    [[nodiscard]] foundation::Result replace_plan(const PipelinePlan& previous, const PipelinePlan& next);
    /** @brief Releases the contract references owned by one installed plan. */
    [[nodiscard]] foundation::Result retire_plan(const PipelinePlan& plan);
    /** @brief Removes a contract and any value owned by this registry. */
    [[nodiscard]] foundation::Result unregister_artifact(ArtifactId id);
    /** @brief Returns the installed immutable contract for an artifact. */
    [[nodiscard]] foundation::ResultValue<ArtifactDescriptor> find_descriptor(ArtifactId id) const;
    /** @brief Reads the most recently published artifact value. */
    [[nodiscard]] foundation::ResultValue<ArtifactView> read(ArtifactId id, ArtifactTypeId expected_type = {},
                                                             uint32_t required_version = 0) const;
    /** @brief Acquires an exclusive lease for an artifact owned by this Scope. */
    [[nodiscard]] foundation::ResultValue<ArtifactWriter> write(ArtifactId id, ArtifactTypeId expected_type = {},
                                                                uint32_t required_version = 0);
    /** @brief Publishes or replaces an artifact value owned by this Scope. */
    [[nodiscard]] foundation::Result publish(ArtifactId id, ArtifactPayload payload);
    /** @brief Releases payloads whose lifetime has ended. */
    [[nodiscard]] foundation::Result release_lifetime(ArtifactLifetime lifetime);

    template <typename Type, typename... Arguments>
    [[nodiscard]] foundation::Result emplace_artifact(ArtifactId id, ArtifactTypeId type, Arguments&&... arguments) {
        try {
            std::shared_ptr<Type> value = std::make_shared<Type>(std::forward<Arguments>(arguments)...);
            const foundation::ResultValue<ArtifactDescriptor> descriptor = find_descriptor(id);
            if (!descriptor) {
                return descriptor.result;
            }
            if (!type.is_valid() || descriptor.value.type != type) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                   "artifact type does not match its contract");
            }
            const bool allow_writes = descriptor.value.storage != ArtifactStorage::kImmutableSnapshot &&
                                      descriptor.value.ownership != OwnershipPolicy::kSharedImmutable;
            return publish(id, ArtifactPayload::owned(descriptor.value.type, std::move(value), allow_writes));
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory, "artifact allocation failed");
        } catch (const std::exception& exception) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
        } catch (...) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "artifact construction threw an unknown exception");
        }
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};

    friend class ArtifactScopeBridge;
    friend class PipelineExecutionContext;
};

/** @brief Read-only snapshot bridge from a lower Scope to an upper Scope. */
class ArtifactScopeBridge final {
public:
    ArtifactScopeBridge(PipelineScope source_scope, PipelineScope target_scope,
                        const ArtifactRegistry& source_registry);
    ~ArtifactScopeBridge();

    ArtifactScopeBridge(const ArtifactScopeBridge&) = delete;
    ArtifactScopeBridge& operator=(const ArtifactScopeBridge&) = delete;
    ArtifactScopeBridge(ArtifactScopeBridge&&) = delete;
    ArtifactScopeBridge& operator=(ArtifactScopeBridge&&) = delete;

    [[nodiscard]] PipelineScope source_scope() const noexcept;
    [[nodiscard]] PipelineScope target_scope() const noexcept;
    /** @brief Verifies that the bridge reads from the supplied source registry. */
    [[nodiscard]] bool is_bound_to(const ArtifactRegistry& source_registry) const noexcept;

    /** @brief Allows one immutable snapshot contract to cross the Scope boundary. */
    [[nodiscard]] foundation::Result expose(ArtifactId id);
    /** @brief Captures all exposed source values and replaces the previous snapshot set. */
    [[nodiscard]] foundation::Result capture();
    /** @brief Reads a value from the most recently captured snapshot set. */
    [[nodiscard]] foundation::ResultValue<ArtifactView> read(ArtifactId id, ArtifactTypeId expected_type = {},
                                                             uint32_t required_version = 0) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Runtime context shared by tasks during one pipeline execution. */
class PipelineExecutionContext final {
public:
    explicit PipelineExecutionContext(void* user_data = nullptr, PipelineScope scope = PipelineScope::kEngine,
                                      ArtifactRegistry* artifact_registry = nullptr,
                                      const ArtifactScopeBridge* scope_bridge = nullptr,
                                      void* scope_data = nullptr) noexcept
        : user_data_(user_data),
          scope_(scope),
          artifact_registry_(artifact_registry),
          scope_bridge_(scope_bridge),
          scope_data_(scope_data) {}

    [[nodiscard]] void* user_data() const noexcept { return user_data_; }
    /** @brief Returns scope-owned runtime context supplied by the frame orchestrator. */
    [[nodiscard]] void* scope_data() const noexcept { return scope_data_; }
    [[nodiscard]] PipelineScope scope() const noexcept { return scope_; }
    [[nodiscard]] ArtifactRegistry* artifact_registry() const noexcept { return artifact_registry_; }
    [[nodiscard]] const ArtifactScopeBridge* scope_bridge() const noexcept { return scope_bridge_; }
    [[nodiscard]] concurrency::CancellationToken cancellation() const noexcept { return cancellation_; }
    void set_scope_data(void* scope_data) noexcept { scope_data_ = scope_data; }
    void set_artifact_runtime(ArtifactRegistry* artifact_registry,
                              const ArtifactScopeBridge* scope_bridge = nullptr) noexcept {
        artifact_registry_ = artifact_registry;
        scope_bridge_ = scope_bridge;
    }

private:
    void* user_data_{nullptr};
    PipelineScope scope_{PipelineScope::kEngine};
    ArtifactRegistry* artifact_registry_{nullptr};
    const ArtifactScopeBridge* scope_bridge_{nullptr};
    void* scope_data_{nullptr};
    const PipelinePlan* active_plan_{nullptr};
    concurrency::CancellationToken cancellation_{};

    [[nodiscard]] foundation::Result begin_plan(const PipelinePlan& plan,
                                                concurrency::CancellationToken cancellation) noexcept;
    void end_plan() noexcept;
    [[nodiscard]] foundation::ResultValue<ArtifactView> read_artifact(const PipelineNode& node, ArtifactId id,
                                                                      ArtifactTypeId expected_type,
                                                                      uint32_t required_version) const;
    [[nodiscard]] foundation::ResultValue<ArtifactWriter> write_artifact(const PipelineNode& node, ArtifactId id,
                                                                         ArtifactTypeId expected_type,
                                                                         uint32_t required_version);
    [[nodiscard]] foundation::Result publish_artifact(const PipelineNode& node, ArtifactId id, ArtifactPayload payload);

    friend class PipelineScheduler;
    friend class PipelineScopeHost;
    friend class PipelineTaskContext;
};

/** @brief Per-task view passed to a TaskExecutionFunction. */
class PipelineTaskContext final {
public:
    PipelineTaskContext(PipelineExecutionContext& execution_context, const PipelineNode& node) noexcept
        : execution_context_(execution_context), node_(node) {}

    [[nodiscard]] PipelineExecutionContext& execution_context() const noexcept { return execution_context_; }
    [[nodiscard]] const PipelineNode& node() const noexcept { return node_; }
    [[nodiscard]] void* user_data() const noexcept { return node_.user_data; }
    /** @brief Returns scope-owned runtime context supplied by the pipeline host. */
    [[nodiscard]] void* scope_data() const noexcept { return execution_context_.scope_data(); }
    [[nodiscard]] concurrency::CancellationToken cancellation() const noexcept {
        return execution_context_.cancellation();
    }
    [[nodiscard]] foundation::ResultValue<ArtifactView> read_artifact(ArtifactId id, ArtifactTypeId expected_type = {},
                                                                      uint32_t required_version = 0) const;
    [[nodiscard]] foundation::ResultValue<ArtifactWriter> write_artifact(ArtifactId id,
                                                                         ArtifactTypeId expected_type = {},
                                                                         uint32_t required_version = 0) const;
    [[nodiscard]] foundation::Result publish_artifact(ArtifactId id, ArtifactPayload payload) const;

private:
    PipelineExecutionContext& execution_context_;
    const PipelineNode& node_;
};

/** @brief Declarative builder used by Pipeline::build(). */
class PipelineBuilder final {
public:
    explicit PipelineBuilder(const PipelineDescriptor& descriptor);
    ~PipelineBuilder() = default;

    PipelineBuilder(const PipelineBuilder&) = delete;
    PipelineBuilder& operator=(const PipelineBuilder&) = delete;
    PipelineBuilder(PipelineBuilder&&) = delete;
    PipelineBuilder& operator=(PipelineBuilder&&) = delete;

    /** @brief Adds a stage declaration to the current definition. */
    [[nodiscard]] PipelineStageHandle add_stage(const StageDescriptor& descriptor);
    /** @brief Adds an executable task declaration to the current definition. */
    [[nodiscard]] PipelineTaskHandle add_task(const TaskDescriptor& descriptor);
    /** @brief Adds an artifact contract to the current definition. */
    [[nodiscard]] PipelineArtifactHandle declare_artifact(const ArtifactDescriptor& descriptor);
    /** @brief Publishes a feature extension point on an anchor stage. */
    [[nodiscard]] PipelineFeaturePointHandle add_feature_point(const FeaturePointDescriptor& descriptor);
    /** @brief Declares a feature binding owned by this pipeline definition. */
    [[nodiscard]] foundation::Result bind_feature(const FeatureBindingDescriptor& binding);

    /** @brief Binds a task to a stage. */
    [[nodiscard]] foundation::Result bind_task(PipelineTaskHandle task, PipelineStageHandle stage);
    /** @brief Declares a read access for a stage. */
    [[nodiscard]] foundation::Result reads(PipelineStageHandle stage, const ArtifactBinding& binding);
    /** @brief Declares a write access for a stage. */
    [[nodiscard]] foundation::Result writes(PipelineStageHandle stage, const ArtifactBinding& binding);
    /** @brief Declares a read/write access for a stage. */
    [[nodiscard]] foundation::Result reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding);
    /** @brief Declares immutable snapshot publication for a stage. */
    [[nodiscard]] foundation::Result publish_snapshot(PipelineStageHandle stage, const ArtifactBinding& binding);
    /** @brief Requires one stage to execute before another stage. */
    [[nodiscard]] foundation::Result runs_before(PipelineStageHandle stage, StageId other);
    /** @brief Requires one stage to execute after another stage. */
    [[nodiscard]] foundation::Result runs_after(PipelineStageHandle stage, StageId other);
    /** @brief Exposes an artifact to another pipeline scope. */
    [[nodiscard]] foundation::Result expose_artifact(ArtifactId artifact, PipelineScope target_scope);

    /** @brief Returns the first definition error recorded by the builder. */
    [[nodiscard]] const foundation::Result& status() const noexcept { return status_; }
    /** @brief Finalizes the definition and transfers its declaration records. */
    [[nodiscard]] foundation::ResultValue<PipelineDefinition> finish();

private:
    [[nodiscard]] bool validate_stage(PipelineStageHandle stage) const noexcept;
    [[nodiscard]] bool validate_task(PipelineTaskHandle task) const noexcept;
    [[nodiscard]] bool validate_artifact(PipelineArtifactHandle artifact) const noexcept;
    [[nodiscard]] bool validate_feature_point(PipelineFeaturePointHandle point) const noexcept;
    [[nodiscard]] uint32_t find_stage(StageId id) const noexcept;
    [[nodiscard]] uint32_t find_artifact(ArtifactId id) const noexcept;
    [[nodiscard]] uint32_t find_feature_point(FeaturePointId id) const noexcept;
    [[nodiscard]] foundation::Result add_binding(PipelineStageHandle stage, const ArtifactBinding& binding,
                                                 ArtifactAccess expected_access);
    void set_failure(foundation::ResultCode code, std::string message);

    explicit PipelineBuilder(const PipelineDefinition& definition);

    static constexpr uint32_t kBuilderGeneration = 1;

    PipelineDescriptor descriptor_{};
    foundation::Result status_{};
    std::vector<PipelineStageRecord> stages_{};
    std::vector<PipelineTaskRecord> tasks_{};
    std::vector<PipelineArtifactRecord> artifacts_{};
    std::vector<PipelineFeaturePointRecord> feature_points_{};
    std::vector<PipelineFeatureBindingRecord> feature_bindings_{};
    std::vector<PipelineBindingRecord> bindings_{};
    std::vector<PipelineOrderingRecord> orderings_{};
    std::vector<PipelineExposureRecord> exposures_{};
    bool finished_{false};

    friend class FeatureBuildContext;
    friend class PipelineCompiler;
};

/** @brief Static pipeline definition supplied to a Registry. */
class Pipeline {
public:
    virtual ~Pipeline() = default;

    [[nodiscard]] virtual const PipelineDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result build(PipelineBuilder& builder) const = 0;
};

/** @brief Type-erased interface for a registered pipeline feature. */
class PipelineFeature {
public:
    virtual ~PipelineFeature() = default;

    [[nodiscard]] virtual const FeatureDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual FeatureDomain feature_domain() const noexcept;
    [[nodiscard]] virtual FeatureDomainMask feature_domain_mask() const noexcept;
    [[nodiscard]] virtual foundation::Result collect_contributions(FeatureBuildContext& context) const = 0;
    [[nodiscard]] virtual foundation::Result build_render_graph(RenderGraphBuilder&) const {
        return foundation::Result::success();
    }
    /** @brief True when this feature writes a composable color state output. */
    [[nodiscard]] virtual bool uses_render_state_output() const noexcept { return false; }
    /** @brief Declares the blending semantic used when this feature consumes state inputs. */
    [[nodiscard]] virtual RenderStateBlendMode render_state_blend_mode() const noexcept {
        return RenderStateBlendMode::kReplace;
    }
};

/** @brief Construction-time context that restricts a feature to one pipeline extension point. */
class FeatureBuildContext final {
public:
    [[nodiscard]] PipelineScope scope() const noexcept { return scope_; }
    [[nodiscard]] PipelineId pipeline_id() const noexcept { return pipeline_id_; }
    [[nodiscard]] FeatureId feature_id() const noexcept { return feature_id_; }
    [[nodiscard]] FeatureInstanceId feature_instance_id() const noexcept { return feature_instance_id_; }
    [[nodiscard]] FeaturePartId part_id() const noexcept { return part_id_; }
    [[nodiscard]] FeaturePointId feature_point() const noexcept { return feature_point_.id; }
    [[nodiscard]] FeatureDomain domain() const noexcept { return active_domain_; }
    [[nodiscard]] const FeaturePointDescriptor& feature_point_descriptor() const noexcept { return feature_point_; }
    [[nodiscard]] FeaturePointFlags feature_point_flags() const noexcept { return feature_point_.flags; }
    [[nodiscard]] bool domain_is_active(FeatureDomain domain) const noexcept {
        return active_domain_ == domain && domain != FeatureDomain::kComposite &&
               (feature_point_.allowed_domains & to_mask(domain)) != 0;
    }
    [[nodiscard]] bool allows(FeaturePointFlags flag) const noexcept { return has_flag(feature_point_.flags, flag); }
    [[nodiscard]] uint32_t stage_count() const noexcept;

    [[nodiscard]] FeatureBuildContext for_domain(FeatureDomain domain) const noexcept {
        FeatureBuildContext result(*this);
        result.active_domain_ = domain;
        return result;
    }

    [[nodiscard]] FeatureBuildContext for_part(FeaturePartId part) const noexcept {
        FeatureBuildContext result(*this);
        result.part_id_ = part;
        return result;
    }

    [[nodiscard]] LogicGraphBuilder logic_builder() noexcept;
    [[nodiscard]] PhysicsGraphBuilder physics_builder() noexcept;
    [[nodiscard]] RenderGraphBuilder render_builder() noexcept;
    [[nodiscard]] AudioGraphBuilder audio_builder() noexcept;
    [[nodiscard]] StreamGraphBuilder stream_builder() noexcept;

    FeatureBuildContext(const FeatureBuildContext&) noexcept = default;
    FeatureBuildContext& operator=(const FeatureBuildContext&) noexcept = default;

private:
    FeatureBuildContext(PipelineBuilder& builder, PipelineScope scope, PipelineId pipeline_id, FeatureId feature_id,
                        FeatureInstanceId feature_instance_id, FeaturePartId part_id,
                        const FeaturePointDescriptor& feature_point, FeatureDomain active_domain,
                        std::vector<PipelineFeatureContributionRecord>* contribution_records) noexcept
        : builder_(&builder),
          scope_(scope),
          pipeline_id_(pipeline_id),
          feature_id_(feature_id),
          feature_instance_id_(feature_instance_id),
          part_id_(part_id),
          feature_point_(feature_point),
          active_domain_(active_domain),
          contribution_records_(contribution_records) {}

    [[nodiscard]] foundation::Result reject(foundation::ResultCode code, std::string message);
    void on_stage_added(PipelineStageHandle stage);
    void record_contribution(FeatureDomain domain, uint32_t first_stage_index);

    PipelineBuilder* builder_{nullptr};
    PipelineScope scope_{PipelineScope::kEngine};
    PipelineId pipeline_id_{};
    FeatureId feature_id_{};
    FeatureInstanceId feature_instance_id_{};
    FeaturePartId part_id_{};
    FeaturePointDescriptor feature_point_{};
    FeatureDomain active_domain_{FeatureDomain::kLogic};
    std::vector<PipelineFeatureContributionRecord>* contribution_records_{nullptr};

    friend class PipelineCompiler;
    friend class LogicFeature;
    friend class PhysicsFeature;
    friend class RenderFeature;
    friend class AudioFeature;
    friend class StreamFeature;
    friend class RenderGraphBuilder;
    template <FeatureDomain Domain>
    friend class DomainGraphBuilder;
};

/** @brief Domain-restricted view over the pipeline declaration builder. */
template <FeatureDomain Domain>
class DomainGraphBuilder final {
public:
    static constexpr FeatureDomain kDomain = Domain;

    [[nodiscard]] PipelineStageHandle add_stage(const StageDescriptor& descriptor);
    [[nodiscard]] PipelineTaskHandle add_task(const TaskDescriptor& descriptor);
    [[nodiscard]] PipelineArtifactHandle declare_artifact(const ArtifactDescriptor& descriptor);

    [[nodiscard]] foundation::Result bind_task(PipelineTaskHandle task, PipelineStageHandle stage);
    [[nodiscard]] foundation::Result reads(PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result writes(PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result publish_snapshot(PipelineStageHandle stage, const ArtifactBinding& binding);
    [[nodiscard]] foundation::Result runs_before(PipelineStageHandle stage, StageId other);
    [[nodiscard]] foundation::Result runs_after(PipelineStageHandle stage, StageId other);
    [[nodiscard]] foundation::Result expose_artifact(ArtifactId artifact, PipelineScope target_scope);

private:
    explicit DomainGraphBuilder(FeatureBuildContext& context) noexcept : context_(&context) {}

    [[nodiscard]] bool is_usable() const noexcept { return context_ != nullptr && context_->domain_is_active(Domain); }

    FeatureBuildContext* context_{nullptr};

    friend class FeatureBuildContext;
};

/** @brief Feature restricted to CPU and non-visual computation contributions. */
class LogicFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kLogic;
    using BuilderType = LogicGraphBuilder;

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
    [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
    [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
    [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
};

/** @brief Feature restricted to simulation and physics graph contributions. */
class PhysicsFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kPhysics;
    using BuilderType = PhysicsGraphBuilder;

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
    [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
    [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
    [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
};

/** @brief Feature restricted to render graph and presentation contributions. */
class RenderFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kRender;
    using BuilderType = RenderGraphBuilder;

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
    [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
    [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
    [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
};

/** @brief Feature restricted to audio graph and spatial sound contributions. */
class AudioFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kAudio;
    using BuilderType = AudioGraphBuilder;

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
    [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
    [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
    [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
};

/** @brief Feature restricted to IO, decoding, and resource streaming contributions. */
class StreamFeature : public PipelineFeature {
public:
    static constexpr FeatureDomain kDomain = FeatureDomain::kStream;
    using BuilderType = StreamGraphBuilder;

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
    [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
    [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
    [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

protected:
    [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
};

/** @brief Member-composition adapter for a feature spanning multiple domains. */
template <typename... Parts>
class CompositeFeature : public PipelineFeature {
public:
    static_assert(sizeof...(Parts) > 0, "CompositeFeature requires at least one part");
    static_assert((std::is_base_of_v<PipelineFeature, Parts> && ...),
                  "CompositeFeature parts must derive from PipelineFeature");
    static_assert((requires { Parts::kDomain; } && ...), "CompositeFeature parts must expose a domain tag");
    static_assert((requires { typename Parts::BuilderType; } && ...),
                  "CompositeFeature parts must expose a domain builder type");
    static_assert(((Parts::kDomain != FeatureDomain::kComposite) && ...),
                  "CompositeFeature cannot contain another CompositeFeature");

    static constexpr FeatureDomain kDomain = FeatureDomain::kComposite;
    static constexpr FeatureDomainMask kDomainMask = (to_mask(Parts::kDomain) | ...);

    CompositeFeature()
        requires((std::is_default_constructible_v<Parts> && ...))
    = default;

    template <typename... Arguments>
        requires(sizeof...(Arguments) == sizeof...(Parts))
    explicit CompositeFeature(Arguments&&... arguments) : parts(std::forward<Arguments>(arguments)...) {}

    [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
    [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return kDomainMask; }

    [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final {
        foundation::Result result = foundation::Result::success();
        uint32_t part_index = 0;
        const auto visit_part = [&](const auto& part) {
            if (!result) {
                return;
            }
            using PartType = std::remove_cvref_t<decltype(part)>;
            FeatureBuildContext part_context =
                context.for_part(make_feature_part_id(context.feature_instance_id(), part_index++));
            part_context = part_context.for_domain(PartType::kDomain);
            result = part.collect_contributions(part_context);
        };
        std::apply([&](const auto&... part) { (visit_part(part), ...); }, parts);
        return result;
    }

    [[nodiscard]] foundation::Result build_render_graph(RenderGraphBuilder& builder) const final {
        foundation::Result result = foundation::Result::success();
        uint32_t part_index = 0;
        const auto visit_part = [&](const auto& part) {
            using PartType = std::remove_cvref_t<decltype(part)>;
            const FeaturePartId part_id = make_feature_part_id(builder.feature_instance_id(), part_index++);
            if (!result || builder.part_id() != part_id) {
                return;
            }
            if constexpr (PartType::kDomain == FeatureDomain::kRender) {
                result = static_cast<const PipelineFeature&>(part).build_render_graph(builder);
            }
        };
        std::apply([&](const auto&... part) { (visit_part(part), ...); }, parts);
        return result;
    }

    template <typename Part>
    [[nodiscard]] Part& part() noexcept {
        return std::get<Part>(parts);
    }

    template <typename Part>
    [[nodiscard]] const Part& part() const noexcept {
        return std::get<Part>(parts);
    }

protected:
    std::tuple<Parts...> parts{};
};

/** @brief Runtime owner-independent interface for a pipeline instance. */
struct PipelineActivationContext {
    void* user_data{nullptr};
};

/** @brief Runtime teardown context for a pipeline instance. */
struct PipelineDeactivationContext {
    void* user_data{nullptr};
};

/** @brief Pipeline definition bound to one concrete Scope owner. */
class PipelineInstance {
public:
    virtual ~PipelineInstance() = default;

    [[nodiscard]] virtual PipelineInstanceId id() const noexcept = 0;
    [[nodiscard]] virtual PipelineId pipeline_id() const noexcept = 0;
    [[nodiscard]] virtual PipelineScope scope() const noexcept = 0;
    [[nodiscard]] virtual void* owner() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result activate(const PipelineActivationContext& context) = 0;
    [[nodiscard]] virtual foundation::Result deactivate(const PipelineDeactivationContext& context) = 0;
};

using PipelineFactory = std::function<std::unique_ptr<Pipeline>()>;
using FeatureFactory = std::function<std::unique_ptr<PipelineFeature>()>;

/** @brief Thread-safe registry of built-in and extension-defined global Phases. */
class PhaseRegistry final {
public:
    PhaseRegistry();
    ~PhaseRegistry();

    PhaseRegistry(const PhaseRegistry&) = delete;
    PhaseRegistry& operator=(const PhaseRegistry&) = delete;
    PhaseRegistry(PhaseRegistry&&) = delete;
    PhaseRegistry& operator=(PhaseRegistry&&) = delete;

    /** @brief Registers a custom Phase relative to an existing Phase. */
    [[nodiscard]] foundation::Result register_phase(const PhaseDescriptor& descriptor);
    /** @brief Removes a custom Phase that is not used as another Phase's anchor. */
    [[nodiscard]] foundation::Result unregister_phase(PhaseId id);
    /** @brief Tests whether a Phase is present in the registry. */
    [[nodiscard]] bool contains(PhaseId id) const noexcept;
    /** @brief Returns one Phase declaration; its name is borrowed from this registry. */
    [[nodiscard]] foundation::ResultValue<PhaseDescriptor> find_phase(PhaseId id) const;
    /** @brief Captures the ordered Phase list and revision under one registry read lock. */
    [[nodiscard]] foundation::ResultValue<PhaseRegistrySnapshot> snapshot() const;
    /** @brief Returns the deterministic global execution order of all Phases. */
    [[nodiscard]] foundation::ResultValue<std::vector<PhaseId>> ordered_phases() const;
    /** @brief Returns the revision changed by every successful registration or removal. */
    [[nodiscard]] uint64_t revision() const noexcept;

private:
    [[nodiscard]] foundation::ResultValue<std::vector<PhaseId>> ordered_phases_locked() const;

    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Thread-safe registry of immutable pipeline definitions. */
class PipelineRegistry final {
public:
    PipelineRegistry();
    ~PipelineRegistry();

    PipelineRegistry(const PipelineRegistry&) = delete;
    PipelineRegistry& operator=(const PipelineRegistry&) = delete;
    PipelineRegistry(PipelineRegistry&&) = delete;
    PipelineRegistry& operator=(PipelineRegistry&&) = delete;

    /** @brief Creates, builds, validates, and registers one pipeline type. */
    [[nodiscard]] foundation::Result register_pipeline(PipelineFactory factory);
    /** @brief Removes a pipeline type from future compiler lookups. */
    [[nodiscard]] foundation::Result unregister_pipeline(PipelineId id);
    /** @brief Returns a shared immutable view of one registered definition. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const PipelineDefinition>> find_definition(
        PipelineId id) const;
    /** @brief Creates, validates, and registers one feature type. */
    [[nodiscard]] foundation::Result register_feature(FeatureFactory factory);
    /** @brief Removes a feature type that has no active bindings. */
    [[nodiscard]] foundation::Result unregister_feature(FeatureId id);
    /** @brief Returns immutable metadata for one registered feature. */
    [[nodiscard]] foundation::ResultValue<FeatureDescriptor> find_feature_descriptor(FeatureId id) const;
    /** @brief Returns the live immutable implementation of one registered feature. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const PipelineFeature>> find_feature(FeatureId id) const;
    /** @brief Binds a registered feature to a pipeline feature point. */
    [[nodiscard]] foundation::Result bind_feature(const FeatureBindingDescriptor& binding);
    /** @brief Removes one feature binding by its stable instance identity. */
    [[nodiscard]] foundation::Result unbind_feature(FeatureInstanceId instance);

    /** @brief Returns the global Phase registry shared by pipeline compilation. */
    [[nodiscard]] PhaseRegistry& phase_registry() noexcept { return phase_registry_; }
    /** @brief Returns the read-only global Phase registry. */
    [[nodiscard]] const PhaseRegistry& phase_registry() const noexcept { return phase_registry_; }

private:
    struct Registration {
        std::shared_ptr<Pipeline> implementation{};
        std::shared_ptr<const PipelineDefinition> definition{};
    };

    struct FeatureRegistration {
        std::shared_ptr<PipelineFeature> implementation{};
        FeatureDescriptor descriptor{};
    };

    [[nodiscard]] foundation::ResultValue<Registration> acquire_registration(PipelineId id) const;
    [[nodiscard]] foundation::ResultValue<FeatureRegistration> acquire_feature_registration(FeatureId id) const;
    [[nodiscard]] foundation::ResultValue<std::vector<FeatureBindingDescriptor>> acquire_feature_bindings(
        PipelineId pipeline, PipelineScope scope) const;

    PhaseRegistry phase_registry_{};
    mutable std::shared_mutex mutex_;
    std::unordered_map<PipelineId, Registration, StableIdHash<PipelineIdTag>> registrations_{};
    std::unordered_map<FeatureId, FeatureRegistration, StableIdHash<FeatureIdTag>> features_{};
    std::unordered_map<FeatureInstanceId, FeatureBindingDescriptor, StableIdHash<FeatureInstanceIdTag>>
        feature_bindings_{};

    friend class PipelineCompiler;
};

/** @brief Input required to compile one registered pipeline definition. */
struct PipelineCompileInput {
    PipelineId pipeline{};
    PipelineScope scope{PipelineScope::kEngine};
    CapabilityMask available_capabilities{0};
    const PipelineRegistry* registry{nullptr};
    PipelineInstanceId instance{};
    const PhaseRegistry* phase_registry{nullptr};
};

/** @brief Converts one registered definition into an immutable execution plan. */
class PipelineCompiler final {
public:
    [[nodiscard]] foundation::ResultValue<PipelinePlan> compile(const PipelineCompileInput& input) const;
};

/** @brief Configures worker lanes used by PipelineScheduler. */
struct PipelineSchedulerDesc {
    uint32_t cpu_worker_count{0};
    uint32_t io_worker_count{0};
    uint32_t render_thread_worker_count{1};
    uint32_t gpu_worker_count{1};
};

/** @brief Executes compiled pipeline batches on their declared execution domains. */
class PipelineScheduler final {
public:
    explicit PipelineScheduler(uint32_t worker_count = 0);
    explicit PipelineScheduler(const PipelineSchedulerDesc& desc);
    ~PipelineScheduler();

    PipelineScheduler(const PipelineScheduler&) = delete;
    PipelineScheduler& operator=(const PipelineScheduler&) = delete;
    PipelineScheduler(PipelineScheduler&&) noexcept;
    PipelineScheduler& operator=(PipelineScheduler&&) noexcept;

    /** @brief Executes batches in dependency order and parallelizes safe batches. */
    [[nodiscard]] foundation::Result execute(const PipelinePlan& plan, PipelineExecutionContext& context,
                                             concurrency::CancellationToken cancellation = {}) const;
    /** @brief Executes only nodes assigned to one global Phase. */
    [[nodiscard]] foundation::Result execute_phase(const PipelinePlan& plan, PipelineExecutionContext& context,
                                                   PhaseId phase,
                                                   concurrency::CancellationToken cancellation = {}) const;
    /** @brief Returns the number of worker threads owned by this scheduler. */
    [[nodiscard]] uint32_t worker_count() const noexcept;
    /** @brief Returns the worker count for a concrete execution domain. */
    [[nodiscard]] uint32_t worker_count(ExecutionDomain domain) const noexcept;
    /** @brief Replaces the executor mapped to one non-main execution domain. */
    [[nodiscard]] foundation::Result set_executor(ExecutionDomain domain,
                                                  std::shared_ptr<concurrency::Executor> executor);
    /** @brief Returns the executor currently mapped to one execution domain. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<concurrency::Executor>> find_executor(
        ExecutionDomain domain) const;
    /** @brief Waits until all mapped executors become idle. */
    [[nodiscard]] foundation::Result wait_idle(uint64_t timeout_nanoseconds = concurrency::kWaitIndefinitely) const;

private:
    [[nodiscard]] foundation::Result execute_internal(const PipelinePlan& plan, PipelineExecutionContext& context,
                                                      const PhaseId* phase,
                                                      concurrency::CancellationToken cancellation) const;

    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Generic Scope owner for one independently compiled pipeline instance. */
class PipelineScopeHost : public PipelineInstance {
public:
    explicit PipelineScopeHost(PipelineScope scope, PipelineInstanceId instance = {}, void* owner = nullptr)
        : scope_(scope), instance_(instance), owner_(owner), artifacts_(scope) {}

    PipelineScopeHost(const PipelineScopeHost&) = delete;
    PipelineScopeHost& operator=(const PipelineScopeHost&) = delete;
    PipelineScopeHost(PipelineScopeHost&&) = delete;
    PipelineScopeHost& operator=(PipelineScopeHost&&) = delete;

    [[nodiscard]] PipelineInstanceId id() const noexcept override { return instance_; }
    [[nodiscard]] PipelineScope scope() const noexcept override { return scope_; }
    [[nodiscard]] PipelineInstanceId instance_id() const noexcept { return instance_; }
    [[nodiscard]] PipelineId pipeline_id() const noexcept override { return pipeline_; }
    [[nodiscard]] void* owner() const noexcept override { return owner_; }
    [[nodiscard]] bool is_active() const noexcept;
    [[nodiscard]] ArtifactRegistry& artifacts() noexcept { return artifacts_; }
    [[nodiscard]] const ArtifactRegistry& artifacts() const noexcept { return artifacts_; }
    [[nodiscard]] const PipelinePlan* active_plan() const noexcept;
    [[nodiscard]] PipelinePlanHandle active_plan_snapshot() const noexcept;
    [[nodiscard]] bool has_pending_plan() const noexcept;
    [[nodiscard]] std::size_t retired_plan_count() const noexcept;
    [[nodiscard]] const ArtifactScopeBridge* scope_bridge() const noexcept { return scope_bridge_; }

    [[nodiscard]] foundation::Result bind_pipeline(PipelineId pipeline);
    [[nodiscard]] foundation::Result rebuild(const PipelineRegistry& registry,
                                             CapabilityMask available_capabilities = 0);
    /** @brief Stages an Editor-authored dependency override that becomes active at the next safe execution boundary. */
    [[nodiscard]] foundation::Result add_runtime_ordering(uint32_t source_node, uint32_t destination_node);
    /** @brief Removes one dependency override at the next safe execution boundary. */
    [[nodiscard]] foundation::Result remove_runtime_ordering(uint32_t source_node, uint32_t destination_node);
    /** @brief Applies a plan staged by rebuild at a safe execution boundary. */
    [[nodiscard]] foundation::Result apply_pending_plan();
    /** @brief Releases retired plans that have no remaining external references. */
    [[nodiscard]] foundation::Result collect_retired_plans();
    void set_scope_bridge(const ArtifactScopeBridge* scope_bridge) noexcept { scope_bridge_ = scope_bridge; }
    [[nodiscard]] foundation::Result activate(const PipelineActivationContext& context) override;
    [[nodiscard]] foundation::Result deactivate(const PipelineDeactivationContext& context) override;
    [[nodiscard]] foundation::Result execute(PipelineScheduler& scheduler, PipelineExecutionContext& context,
                                             concurrency::CancellationToken cancellation = {}) const;
    /** @brief Executes one Phase using an optional frame-pinned Plan. */
    [[nodiscard]] foundation::Result execute_phase(PipelineScheduler& scheduler, PipelineExecutionContext& context,
                                                   PhaseId phase, PipelinePlanHandle pinned_plan = {},
                                                   concurrency::CancellationToken cancellation = {}) const;

private:
    PipelineScope scope_{PipelineScope::kEngine};
    PipelineInstanceId instance_{};
    void* owner_{nullptr};
    PipelineId pipeline_{};
    mutable ArtifactRegistry artifacts_;
    const ArtifactScopeBridge* scope_bridge_{nullptr};
    mutable std::shared_mutex plan_mutex_;
    mutable PipelinePlanHandle active_plan_{};
    mutable PipelinePlanHandle pending_plan_{};
    mutable std::vector<PipelinePlanHandle> retired_plans_{};
    mutable bool active_{false};

    [[nodiscard]] foundation::Result apply_pending_plan_internal() const;
};

inline foundation::Result render_graph_builder_invalid_state() {
    return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                       "render graph builder is not attached to a runtime backend");
}

inline foundation::Result render_graph_builder_permission_denied(FeaturePointFlags flag) {
    static_cast<void>(flag);
    return foundation::Result::failure(foundation::ResultCode::kUnsupported,
                                       "feature point does not grant the requested render graph permission");
}

inline RenderGraphBuilder::RenderGraphBuilder(FeatureBuildContext& context) noexcept : declaration_context_(&context) {}

inline RenderGraphBuilder::RenderGraphBuilder(RenderGraphBuilderBackend& backend,
                                              const RenderGraphFeatureContext& context) noexcept
    : runtime_backend_(&backend), runtime_context_(context) {}

inline PipelineStageHandle RenderGraphBuilder::add_stage(const StageDescriptor& descriptor) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return {};
    }
    const PipelineStageHandle stage = declaration_context_->builder_->add_stage(descriptor);
    if (stage) {
        declaration_context_->on_stage_added(stage);
    }
    return stage;
}

inline PipelineTaskHandle RenderGraphBuilder::add_task(const TaskDescriptor& descriptor) {
    return declaration_context_ != nullptr && declaration_context_->domain_is_active(kDomain)
               ? declaration_context_->builder_->add_task(descriptor)
               : PipelineTaskHandle{};
}

inline PipelineArtifactHandle RenderGraphBuilder::declare_artifact(const ArtifactDescriptor& descriptor) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return {};
    }
    if (!declaration_context_->allows(FeaturePointFlags::kMayCreateArtifacts)) {
        static_cast<void>(declaration_context_->reject(foundation::ResultCode::kUnsupported,
                                                       "feature point does not allow artifact creation"));
        return {};
    }
    return declaration_context_->builder_->declare_artifact(descriptor);
}

inline foundation::Result RenderGraphBuilder::bind_task(PipelineTaskHandle task, PipelineStageHandle stage) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return render_graph_builder_invalid_state();
    }
    return declaration_context_->builder_->bind_task(task, stage);
}

inline foundation::Result RenderGraphBuilder::reads(PipelineStageHandle stage, const ArtifactBinding& binding) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return render_graph_builder_invalid_state();
    }
    return declaration_context_->builder_->reads(stage, binding);
}

inline foundation::Result RenderGraphBuilder::writes(PipelineStageHandle stage, const ArtifactBinding& binding) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return render_graph_builder_invalid_state();
    }
    return declaration_context_->builder_->writes(stage, binding);
}

inline foundation::Result RenderGraphBuilder::reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return render_graph_builder_invalid_state();
    }
    return declaration_context_->builder_->reads_writes(stage, binding);
}

inline foundation::Result RenderGraphBuilder::publish_snapshot(PipelineStageHandle stage,
                                                               const ArtifactBinding& binding) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return render_graph_builder_invalid_state();
    }
    if (!declaration_context_->allows(FeaturePointFlags::kMayPublishSnapshot)) {
        return declaration_context_->reject(foundation::ResultCode::kUnsupported,
                                            "feature point does not allow snapshot publication");
    }
    return declaration_context_->builder_->publish_snapshot(stage, binding);
}

inline foundation::Result RenderGraphBuilder::runs_before(PipelineStageHandle stage, StageId other) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return render_graph_builder_invalid_state();
    }
    return declaration_context_->builder_->runs_before(stage, other);
}

inline foundation::Result RenderGraphBuilder::runs_after(PipelineStageHandle stage, StageId other) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return render_graph_builder_invalid_state();
    }
    return declaration_context_->builder_->runs_after(stage, other);
}

inline foundation::Result RenderGraphBuilder::expose_artifact(ArtifactId artifact, PipelineScope target_scope) {
    if (declaration_context_ == nullptr || !declaration_context_->domain_is_active(kDomain)) {
        return render_graph_builder_invalid_state();
    }
    return declaration_context_->builder_->expose_artifact(artifact, target_scope);
}

inline PipelineScope RenderGraphBuilder::scope() const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->scope() : runtime_context_.scope;
}

inline PipelineId RenderGraphBuilder::pipeline_id() const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->pipeline_id() : runtime_context_.pipeline;
}

inline FeatureId RenderGraphBuilder::feature_id() const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->feature_id() : runtime_context_.feature;
}

inline FeatureInstanceId RenderGraphBuilder::feature_instance_id() const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->feature_instance_id()
                                           : runtime_context_.feature_instance;
}

inline FeaturePartId RenderGraphBuilder::part_id() const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->part_id() : runtime_context_.part;
}

inline FeaturePointId RenderGraphBuilder::feature_point() const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->feature_point() : runtime_context_.feature_point;
}

inline FeaturePointPosition RenderGraphBuilder::feature_point_position() const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->feature_point_descriptor().position
                                           : runtime_context_.point_position;
}

inline FeaturePointFlags RenderGraphBuilder::feature_point_flags() const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->feature_point_flags() : runtime_context_.point_flags;
}

inline void* RenderGraphBuilder::user_data() const noexcept {
    return declaration_context_ != nullptr ? nullptr : runtime_context_.user_data;
}

inline const void* RenderGraphBuilder::feature_data() const noexcept {
    return declaration_context_ != nullptr ? nullptr : runtime_context_.feature_data;
}

inline RenderGraphTexture RenderGraphBuilder::state_input() const noexcept {
    return declaration_context_ != nullptr ? RenderGraphTexture{} : runtime_context_.state_input;
}

inline std::span<const RenderGraphTexture> RenderGraphBuilder::state_inputs() const noexcept {
    return declaration_context_ != nullptr ? std::span<const RenderGraphTexture>{} : runtime_context_.state_inputs;
}

inline RenderGraphTexture RenderGraphBuilder::state_output() const noexcept {
    return declaration_context_ != nullptr ? RenderGraphTexture{} : runtime_context_.state_output;
}

inline uint32_t RenderGraphBuilder::state_layer_count() const noexcept {
    return declaration_context_ != nullptr ? 0 : runtime_context_.state_layer_count;
}

inline bool RenderGraphBuilder::allows(FeaturePointFlags flag) const noexcept {
    return declaration_context_ != nullptr ? declaration_context_->allows(flag) : runtime_context_.allows(flag);
}

inline bool RenderGraphBuilder::is_runtime() const noexcept {
    return runtime_backend_ != nullptr;
}

inline foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilder::create_buffer(
    const rhi::BufferDesc& desc, RenderGraphResourceLifetime lifetime) {
    return create_buffer({}, desc, lifetime);
}

inline foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilder::create_buffer(
    RenderResourceId id, const rhi::BufferDesc& desc, RenderGraphResourceLifetime lifetime) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(FeaturePointFlags::kMayCreateArtifacts)) {
        return {render_graph_builder_permission_denied(FeaturePointFlags::kMayCreateArtifacts), {}};
    }
    return runtime_backend_->create_buffer(id, desc, lifetime);
}

inline foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::create_texture(
    const rhi::TextureDesc& desc, RenderGraphResourceLifetime lifetime) {
    return create_texture({}, desc, lifetime);
}

inline foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::create_texture(
    RenderResourceId id, const rhi::TextureDesc& desc, RenderGraphResourceLifetime lifetime) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(FeaturePointFlags::kMayCreateArtifacts)) {
        return {render_graph_builder_permission_denied(FeaturePointFlags::kMayCreateArtifacts), {}};
    }
    return runtime_backend_->create_texture(id, desc, lifetime);
}

inline foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilder::import_buffer(
    RenderResourceId id, rhi::BufferHandle buffer, const rhi::BufferDesc& desc,
    const rhi::ResourceState& initial_state) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(FeaturePointFlags::kMayCreateArtifacts)) {
        return {render_graph_builder_permission_denied(FeaturePointFlags::kMayCreateArtifacts), {}};
    }
    return runtime_backend_->import_buffer(id, buffer, desc, initial_state);
}

inline foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::import_texture(
    RenderResourceId id, rhi::TextureHandle texture, rhi::TextureViewHandle default_view, const rhi::TextureDesc& desc,
    const rhi::ResourceState& initial_state) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(FeaturePointFlags::kMayCreateArtifacts)) {
        return {render_graph_builder_permission_denied(FeaturePointFlags::kMayCreateArtifacts), {}};
    }
    return runtime_backend_->import_texture(id, texture, default_view, desc, initial_state);
}

inline foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::import_swapchain_image(
    RenderResourceId id, const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
    const rhi::ResourceState& initial_state) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(FeaturePointFlags::kMayCreateArtifacts)) {
        return {render_graph_builder_permission_denied(FeaturePointFlags::kMayCreateArtifacts), {}};
    }
    return runtime_backend_->import_swapchain_image(id, acquired, desc, initial_state);
}

inline foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilder::find_buffer(RenderResourceId id) const {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    return runtime_backend_->find_buffer(id);
}

inline foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::find_texture(RenderResourceId id) const {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    return runtime_backend_->find_texture(id);
}

inline foundation::Result RenderGraphBuilder::bind_buffer(RenderResourceId id, RenderGraphBuffer buffer) {
    if (runtime_backend_ == nullptr) {
        return render_graph_builder_invalid_state();
    }
    if (!allows(FeaturePointFlags::kMayCreateArtifacts)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayCreateArtifacts);
    }
    return runtime_backend_->bind_buffer(id, buffer);
}

inline foundation::Result RenderGraphBuilder::bind_texture(RenderResourceId id, RenderGraphTexture texture) {
    if (runtime_backend_ == nullptr) {
        return render_graph_builder_invalid_state();
    }
    if (!allows(FeaturePointFlags::kMayCreateArtifacts)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayCreateArtifacts);
    }
    return runtime_backend_->bind_texture(id, texture);
}

inline foundation::Result RenderGraphBuilder::export_buffer(RenderGraphBuffer buffer) {
    if (runtime_backend_ == nullptr) {
        return render_graph_builder_invalid_state();
    }
    if (!allows(FeaturePointFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayWriteFinalOutput);
    }
    return runtime_backend_->export_buffer(buffer);
}

inline foundation::Result RenderGraphBuilder::export_texture(RenderGraphTexture texture) {
    if (runtime_backend_ == nullptr) {
        return render_graph_builder_invalid_state();
    }
    if (!allows(FeaturePointFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayWriteFinalOutput);
    }
    return runtime_backend_->export_texture(texture);
}

inline foundation::ResultValue<RenderGraphPass> RenderGraphBuilder::add_pass(std::string_view name,
                                                                             RenderGraphPassType type,
                                                                             rhi::QueueClass queue,
                                                                             RenderPassSetupFunction setup,
                                                                             RenderPassExecuteFunction execute) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    FeaturePointFlags required_flag = FeaturePointFlags::kNone;
    switch (type) {
        case RenderGraphPassType::kGraphics:
            required_flag = FeaturePointFlags::kMayCreateGraphicsPass;
            break;
        case RenderGraphPassType::kCompute:
            required_flag = FeaturePointFlags::kMayCreateComputePass;
            break;
        case RenderGraphPassType::kCopy:
            required_flag = FeaturePointFlags::kMayCreateCopyPass;
            break;
    }
    if (!allows(required_flag)) {
        return {render_graph_builder_permission_denied(required_flag), {}};
    }
    if ((queue == rhi::QueueClass::kCompute || queue == rhi::QueueClass::kCopy) &&
        !allows(FeaturePointFlags::kMayUseDedicatedQueue)) {
        return {render_graph_builder_permission_denied(FeaturePointFlags::kMayUseDedicatedQueue), {}};
    }
    return runtime_backend_->add_pass(name, type, queue, std::move(setup), std::move(execute));
}

inline foundation::ResultValue<RenderGraphPass> RenderGraphBuilder::add_graphics_pass(std::string_view name,
                                                                                      RenderPassSetupFunction setup,
                                                                                      RenderPassExecuteFunction execute,
                                                                                      rhi::QueueClass queue) {
    return add_pass(name, RenderGraphPassType::kGraphics, queue, std::move(setup), std::move(execute));
}

inline foundation::ResultValue<RenderGraphPass> RenderGraphBuilder::add_compute_pass(std::string_view name,
                                                                                     RenderPassSetupFunction setup,
                                                                                     RenderPassExecuteFunction execute,
                                                                                     rhi::QueueClass queue) {
    return add_pass(name, RenderGraphPassType::kCompute, queue, std::move(setup), std::move(execute));
}

inline foundation::ResultValue<RenderGraphPass> RenderGraphBuilder::add_copy_pass(std::string_view name,
                                                                                  RenderPassSetupFunction setup,
                                                                                  RenderPassExecuteFunction execute,
                                                                                  rhi::QueueClass queue) {
    return add_pass(name, RenderGraphPassType::kCopy, queue, std::move(setup), std::move(execute));
}

inline foundation::Result RenderPassBuilder::read(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                  rhi::PipelineStageMask pipeline_stages,
                                                  rhi::ShaderStageMask shader_stages, RenderGraphBufferRange range) {
    return backend_ == nullptr ? render_graph_builder_invalid_state()
                               : backend_->read_buffer(buffer, usage, pipeline_stages, shader_stages, range);
}

inline foundation::Result RenderPassBuilder::write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                   rhi::PipelineStageMask pipeline_stages,
                                                   rhi::ShaderStageMask shader_stages, RenderGraphBufferRange range,
                                                   bool discard_contents) {
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->write_buffer(buffer, usage, pipeline_stages, shader_stages, range, discard_contents);
}

inline foundation::Result RenderPassBuilder::read_write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                        rhi::PipelineStageMask pipeline_stages,
                                                        rhi::ShaderStageMask shader_stages,
                                                        RenderGraphBufferRange range) {
    return backend_ == nullptr ? render_graph_builder_invalid_state()
                               : backend_->read_write_buffer(buffer, usage, pipeline_stages, shader_stages, range);
}

inline foundation::Result RenderPassBuilder::read(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                  rhi::PipelineStageMask pipeline_stages,
                                                  rhi::ShaderStageMask shader_stages,
                                                  rhi::SubresourceRange subresources) {
    if (usage == rhi::ResourceUsage::kPresent && !has_flag(point_flags_, FeaturePointFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr ? render_graph_builder_invalid_state()
                               : backend_->read_texture(texture, usage, pipeline_stages, shader_stages, subresources);
}

inline foundation::Result RenderPassBuilder::write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                   rhi::PipelineStageMask pipeline_stages,
                                                   rhi::ShaderStageMask shader_stages,
                                                   rhi::SubresourceRange subresources, bool discard_contents) {
    if (usage == rhi::ResourceUsage::kPresent && !has_flag(point_flags_, FeaturePointFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr ? render_graph_builder_invalid_state()
                               : backend_->write_texture(texture, usage, pipeline_stages, shader_stages, subresources,
                                                         discard_contents);
}

inline foundation::Result RenderPassBuilder::read_write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                        rhi::PipelineStageMask pipeline_stages,
                                                        rhi::ShaderStageMask shader_stages,
                                                        rhi::SubresourceRange subresources) {
    if (usage == rhi::ResourceUsage::kPresent && !has_flag(point_flags_, FeaturePointFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->read_write_texture(texture, usage, pipeline_stages, shader_stages, subresources);
}

inline foundation::Result RenderPassBuilder::use_color_attachment(RenderGraphTexture texture, rhi::LoadOp load_op,
                                                                  rhi::StoreOp store_op,
                                                                  rhi::ClearColorValue clear_color,
                                                                  rhi::ResourceUsage final_usage,
                                                                  rhi::SubresourceRange subresources) {
    if (final_usage == rhi::ResourceUsage::kPresent &&
        !has_flag(point_flags_, FeaturePointFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->use_color_attachment(texture, load_op, store_op, clear_color, final_usage, subresources);
}

inline foundation::Result RenderPassBuilder::use_depth_stencil_attachment(
    RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op, float clear_depth,
    uint32_t clear_stencil, rhi::ResourceUsage final_usage, rhi::SubresourceRange subresources) {
    if (final_usage == rhi::ResourceUsage::kPresent &&
        !has_flag(point_flags_, FeaturePointFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(FeaturePointFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->use_depth_stencil_attachment(texture, depth_load_op, depth_store_op, clear_depth,
                                                        clear_stencil, final_usage, subresources);
}

inline void RenderPassBuilder::set_side_effects(bool enabled) noexcept {
    if (backend_ != nullptr) {
        backend_->set_side_effects(enabled);
    }
}

inline RenderGraphPass RenderPassExecutionContext::pass() const noexcept {
    return backend_->pass();
}

inline foundation::ResultValue<rhi::BufferHandle> RenderPassExecutionContext::buffer(RenderGraphBuffer buffer) const {
    return backend_->buffer(buffer);
}

inline foundation::ResultValue<rhi::TextureHandle> RenderPassExecutionContext::texture(
    RenderGraphTexture texture) const {
    return backend_->texture(texture);
}

inline foundation::ResultValue<rhi::BufferViewHandle> RenderPassExecutionContext::buffer_view(
    RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset, uint64_t size, rhi::Format format,
    uint32_t structure_stride) {
    return backend_->buffer_view(buffer, kind, offset, size, format, structure_stride);
}

inline foundation::ResultValue<rhi::TextureViewHandle> RenderPassExecutionContext::texture_view(
    RenderGraphTexture texture, rhi::TextureViewKind kind, rhi::SubresourceRange subresources, rhi::Format format,
    rhi::TextureViewDimension dimension) {
    return backend_->texture_view(texture, kind, subresources, format, dimension);
}

inline foundation::Result RenderPassExecutionContext::begin_rendering() {
    return backend_->begin_rendering();
}

inline foundation::Result RenderPassExecutionContext::end_rendering() {
    return backend_->end_rendering();
}

inline bool RenderPassExecutionContext::rendering_active() const noexcept {
    return backend_->rendering_active();
}

inline bool RenderPassExecutionContext::rendering_started() const noexcept {
    return backend_->rendering_started();
}

inline foundation::Result RenderPassExecutionContext::set_viewport(const rhi::Viewport& viewport) {
    return backend_->set_viewport(viewport);
}

inline foundation::Result RenderPassExecutionContext::set_scissor(const rhi::ScissorRect& scissor) {
    return backend_->set_scissor(scissor);
}

inline foundation::Result RenderPassExecutionContext::bind_pipeline(rhi::PipelineHandle pipeline) {
    return backend_->bind_pipeline(pipeline);
}

inline foundation::Result RenderPassExecutionContext::bind_binding_set(uint32_t group, rhi::BindingSetHandle set,
                                                                       std::span<const uint32_t> dynamic_offsets) {
    return backend_->bind_binding_set(group, set, dynamic_offsets);
}

inline foundation::Result RenderPassExecutionContext::set_inline_constants(uint32_t offset,
                                                                           rhi::ShaderStageMask shader_stages,
                                                                           std::span<const std::byte> data) {
    return backend_->set_inline_constants(offset, shader_stages, data);
}

inline foundation::Result RenderPassExecutionContext::bind_vertex_buffer(uint32_t binding, rhi::BufferHandle buffer,
                                                                         uint64_t offset) {
    return backend_->bind_vertex_buffer(binding, buffer, offset);
}

inline foundation::Result RenderPassExecutionContext::bind_index_buffer(rhi::BufferHandle buffer, uint64_t offset,
                                                                        rhi::IndexFormat format) {
    return backend_->bind_index_buffer(buffer, offset, format);
}

inline foundation::Result RenderPassExecutionContext::draw(const rhi::DrawDesc& desc) {
    return backend_->draw(desc);
}

inline foundation::Result RenderPassExecutionContext::draw_indexed(const rhi::DrawIndexedDesc& desc) {
    return backend_->draw_indexed(desc);
}

inline foundation::Result RenderPassExecutionContext::dispatch(const rhi::DispatchDesc& desc) {
    return backend_->dispatch(desc);
}

inline foundation::Result RenderPassExecutionContext::dispatch_indirect(const rhi::DispatchIndirectDesc& desc) {
    return backend_->dispatch_indirect(desc);
}

inline foundation::Result RenderPassExecutionContext::copy_buffer(RenderGraphBuffer source,
                                                                  RenderGraphBuffer destination, uint64_t source_offset,
                                                                  uint64_t destination_offset, uint64_t size) {
    return backend_->copy_buffer(source, destination, source_offset, destination_offset, size);
}

inline foundation::Result RenderPassExecutionContext::copy_buffer_to_texture(
    RenderGraphBuffer source, uint64_t source_offset, uint32_t source_row_length, uint32_t source_image_height,
    RenderGraphTexture destination, rhi::TextureSubresourceLayers destination_subresources,
    rhi::Offset3D destination_offset, rhi::Extent3D extent) {
    return backend_->copy_buffer_to_texture(source, source_offset, source_row_length, source_image_height, destination,
                                            destination_subresources, destination_offset, extent);
}

inline foundation::Result RenderPassExecutionContext::copy_texture_to_buffer(
    RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources, rhi::Offset3D source_offset,
    rhi::Extent3D extent, RenderGraphBuffer destination, uint64_t destination_offset, uint32_t destination_row_length,
    uint32_t destination_image_height) {
    return backend_->copy_texture_to_buffer(source, source_subresources, source_offset, extent, destination,
                                            destination_offset, destination_row_length, destination_image_height);
}

inline foundation::Result RenderPassExecutionContext::copy_texture(
    RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources, rhi::Offset3D source_offset,
    RenderGraphTexture destination, rhi::TextureSubresourceLayers destination_subresources,
    rhi::Offset3D destination_offset, rhi::Extent3D extent) {
    return backend_->copy_texture(source, source_subresources, source_offset, destination, destination_subresources,
                                  destination_offset, extent);
}

inline foundation::Result RenderPassExecutionContext::clear_texture(RenderGraphTexture texture,
                                                                    rhi::SubresourceRange subresources,
                                                                    rhi::ClearColorValue color, float depth,
                                                                    uint32_t stencil) {
    return backend_->clear_texture(texture, subresources, color, depth, stencil);
}

template <FeatureDomain Domain>
PipelineStageHandle DomainGraphBuilder<Domain>::add_stage(const StageDescriptor& descriptor) {
    if (!is_usable()) {
        return {};
    }
    const PipelineStageHandle stage = context_->builder_->add_stage(descriptor);
    if (stage) {
        context_->on_stage_added(stage);
    }
    return stage;
}

template <FeatureDomain Domain>
PipelineTaskHandle DomainGraphBuilder<Domain>::add_task(const TaskDescriptor& descriptor) {
    return is_usable() ? context_->builder_->add_task(descriptor) : PipelineTaskHandle{};
}

template <FeatureDomain Domain>
PipelineArtifactHandle DomainGraphBuilder<Domain>::declare_artifact(const ArtifactDescriptor& descriptor) {
    if (!is_usable()) {
        return {};
    }
    if (!context_->allows(FeaturePointFlags::kMayCreateArtifacts)) {
        static_cast<void>(
            context_->reject(foundation::ResultCode::kUnsupported, "feature point does not allow artifact creation"));
        return {};
    }
    return context_->builder_->declare_artifact(descriptor);
}

template <FeatureDomain Domain>
foundation::Result DomainGraphBuilder<Domain>::bind_task(PipelineTaskHandle task, PipelineStageHandle stage) {
    return is_usable()
               ? context_->builder_->bind_task(task, stage)
               : context_->reject(foundation::ResultCode::kInvalidState, "feature builder domain is not active");
}

template <FeatureDomain Domain>
foundation::Result DomainGraphBuilder<Domain>::reads(PipelineStageHandle stage, const ArtifactBinding& binding) {
    return is_usable()
               ? context_->builder_->reads(stage, binding)
               : context_->reject(foundation::ResultCode::kInvalidState, "feature builder domain is not active");
}

template <FeatureDomain Domain>
foundation::Result DomainGraphBuilder<Domain>::writes(PipelineStageHandle stage, const ArtifactBinding& binding) {
    return is_usable()
               ? context_->builder_->writes(stage, binding)
               : context_->reject(foundation::ResultCode::kInvalidState, "feature builder domain is not active");
}

template <FeatureDomain Domain>
foundation::Result DomainGraphBuilder<Domain>::reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding) {
    return is_usable()
               ? context_->builder_->reads_writes(stage, binding)
               : context_->reject(foundation::ResultCode::kInvalidState, "feature builder domain is not active");
}

template <FeatureDomain Domain>
foundation::Result DomainGraphBuilder<Domain>::publish_snapshot(PipelineStageHandle stage,
                                                                const ArtifactBinding& binding) {
    if (!is_usable()) {
        return context_->reject(foundation::ResultCode::kInvalidState, "feature builder domain is not active");
    }
    if (!context_->allows(FeaturePointFlags::kMayPublishSnapshot)) {
        return context_->reject(foundation::ResultCode::kUnsupported,
                                "feature point does not allow snapshot publication");
    }
    return context_->builder_->publish_snapshot(stage, binding);
}

template <FeatureDomain Domain>
foundation::Result DomainGraphBuilder<Domain>::runs_before(PipelineStageHandle stage, StageId other) {
    return is_usable()
               ? context_->builder_->runs_before(stage, other)
               : context_->reject(foundation::ResultCode::kInvalidState, "feature builder domain is not active");
}

template <FeatureDomain Domain>
foundation::Result DomainGraphBuilder<Domain>::runs_after(PipelineStageHandle stage, StageId other) {
    return is_usable()
               ? context_->builder_->runs_after(stage, other)
               : context_->reject(foundation::ResultCode::kInvalidState, "feature builder domain is not active");
}

template <FeatureDomain Domain>
foundation::Result DomainGraphBuilder<Domain>::expose_artifact(ArtifactId artifact, PipelineScope target_scope) {
    return is_usable()
               ? context_->builder_->expose_artifact(artifact, target_scope)
               : context_->reject(foundation::ResultCode::kInvalidState, "feature builder domain is not active");
}
}  // namespace aeg::pipeline
