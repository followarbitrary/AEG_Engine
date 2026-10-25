/**
 * @file pipeline.hpp
 * @brief Engine-private interfaces for technical projection compilation and execution.
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

#include "pipeline_types.hpp"
#include "render_program.hpp"
#include "rhi_struct.hpp"

namespace aeg::rhi {
class Device;
}

namespace aeg::pipeline {
class ArtifactRegistry;
class ArtifactScopeBridge;
class RenderGraphBuilder;
class RenderGraphBuilderBackend;
class RenderPassBuilder;
class RenderPassBuilderBackend;
class RenderPassExecutionContext;
class RenderPassExecutionContextBackend;
class RenderPassRegistry;
namespace detail {
struct PipelineCompilerExecution;
struct PhaseRegistryExecution;
struct PipelineScopeHostExecution;
}
struct RenderPassConfiguration;

using RenderPassSetupFunction = std::function<foundation::Result(RenderPassBuilder&)>;
using RenderPassExecuteFunction = std::function<foundation::Result(RenderPassExecutionContext&)>;

/** @brief Borrowed data and permissions for one engine-owned native graph build. */
struct RenderGraphBuildContext {
    RenderGraphBuildFlags flags{RenderGraphBuildFlags::kNone};
    const void* context_data{nullptr};
};

/** @brief Private native graph adapter; structural declaration callbacks are retired. */
class RenderGraphBuilder final {
public:
    [[nodiscard]] RenderGraphBuildFlags build_flags() const noexcept;
    [[nodiscard]] const void* context_data() const noexcept;
    [[nodiscard]] bool allows(RenderGraphBuildFlags flag) const noexcept;
    [[nodiscard]] bool is_runtime() const noexcept;
    /** @brief Records an actual resolved native pass handle for fixed sequence dependencies. */
    [[nodiscard]] foundation::Result remember_native_pass(RenderPassInstanceId instance, RenderGraphPass pass);
    [[nodiscard]] foundation::ResultValue<RenderGraphPass> native_pass(RenderPassInstanceId instance) const;
    /** @brief Retains a fixed native execution lease until the graph's GPU frame retires. */
    [[nodiscard]] foundation::Result retain_execution_owner(std::shared_ptr<const void> owner);

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
    [[nodiscard]] foundation::Result add_dependency(RenderGraphPass source, RenderGraphPass destination);

    /** @brief Creates a runtime facade backed by a RenderGraph adapter. */
    RenderGraphBuilder(RenderGraphBuilderBackend& backend, const RenderGraphBuildContext& context) noexcept;

private:
    RenderGraphBuilderBackend* runtime_backend_{nullptr};
    RenderGraphBuildContext build_context_{};
    std::unordered_map<RenderPassInstanceId, RenderGraphPass, StableIdHash<RenderPassInstanceIdTag>> native_passes_{};
};

/** @brief Facade used by one render-pass setup callback. */
class RenderPassBuilder final {
public:
    explicit RenderPassBuilder(RenderPassBuilderBackend& backend,
                               RenderGraphBuildFlags build_flags = RenderGraphBuildFlags::kNone) noexcept;

    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> find_buffer(RenderResourceId id) const;
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> find_texture(RenderResourceId id) const;

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
    [[nodiscard]] foundation::Result declare_render_program(const RenderProgramRequest& request,
                                                            const RenderPassConfiguration& configuration);

private:
    RenderPassBuilderBackend* backend_{nullptr};
    RenderGraphBuildFlags build_flags_{RenderGraphBuildFlags::kNone};
};

/** @brief Facade used by one render-pass command callback. */
class RenderPassExecutionContext final {
public:
    explicit RenderPassExecutionContext(RenderPassExecutionContextBackend& backend) noexcept;

    [[nodiscard]] RenderGraphPass pass() const noexcept;
    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> find_buffer(RenderResourceId id) const;
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> find_texture(RenderResourceId id) const;
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
    [[nodiscard]] foundation::Result bind_render_program(const RenderProgramRequest& request,
                                                         const RenderPassConfiguration& configuration);

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

/** @brief Per-call observation of an Engine-private Stage plan. */
class PipelineExecutionContext final {
public:
    explicit PipelineExecutionContext(PipelineScope scope = PipelineScope::kEngine,
                                      ArtifactRegistry* artifact_registry = nullptr,
                                      const ArtifactScopeBridge* scope_bridge = nullptr) noexcept
        : scope_(scope), artifact_registry_(artifact_registry), scope_bridge_(scope_bridge) {}

    [[nodiscard]] PipelineScope scope() const noexcept { return scope_; }
    [[nodiscard]] ArtifactRegistry* artifact_registry() const noexcept { return artifact_registry_; }
    [[nodiscard]] const ArtifactScopeBridge* scope_bridge() const noexcept { return scope_bridge_; }
    /** @brief Node indices from the last successful call; empty after any failed call. */
    [[nodiscard]] std::span<const uint32_t> visited_nodes() const noexcept { return visited_nodes_; }
    void set_artifact_runtime(ArtifactRegistry* artifact_registry,
                              const ArtifactScopeBridge* scope_bridge = nullptr) noexcept {
        artifact_registry_ = artifact_registry;
        scope_bridge_ = scope_bridge;
    }

private:
    PipelineScope scope_{PipelineScope::kEngine};
    ArtifactRegistry* artifact_registry_{nullptr};
    const ArtifactScopeBridge* scope_bridge_{nullptr};
    const PipelinePlan* active_plan_{nullptr};
    std::vector<uint32_t> visited_nodes_{};

    [[nodiscard]] foundation::Result begin_plan(const PipelinePlan& plan) noexcept;
    void end_plan() noexcept;

    friend class PipelineScheduler;
    friend class PipelineScopeHost;
};

/** @brief Collects owning technical Stage and Artifact declaration values. */
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
    /** @brief Adds an artifact contract to the current definition. */
    [[nodiscard]] PipelineArtifactHandle declare_artifact(const ArtifactDescriptor& descriptor);
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
    [[nodiscard]] bool validate_artifact(PipelineArtifactHandle artifact) const noexcept;
    [[nodiscard]] uint32_t find_stage(StageId id) const noexcept;
    [[nodiscard]] uint32_t find_artifact(ArtifactId id) const noexcept;
    [[nodiscard]] foundation::Result add_binding(PipelineStageHandle stage, const ArtifactBinding& binding,
                                                 ArtifactAccess expected_access);
    void set_failure(foundation::ResultCode code, std::string message);

    static constexpr uint32_t kBuilderGeneration = 1;

    PipelineDescriptor descriptor_{};
    foundation::Result status_{};
    std::vector<PipelineStageRecord> stages_{};
    std::vector<PipelineArtifactRecord> artifacts_{};
    std::vector<PipelineBindingRecord> bindings_{};
    std::vector<PipelineOrderingRecord> orderings_{};
    std::vector<PipelineExposureRecord> exposures_{};
    bool finished_{false};

    friend struct detail::PipelineCompilerExecution;
    friend class PipelineRegistry;
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
    /** @brief Returns the deterministic global execution order of all Phases. */
    [[nodiscard]] foundation::ResultValue<std::vector<PhaseId>> ordered_phases() const;

private:
    friend struct detail::PhaseRegistryExecution;
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

    /** @brief Validates and owns declaration records without invoking user code. */
    [[nodiscard]] foundation::Result register_pipeline(PipelineDefinition definition);
    /** @brief Removes a pipeline type from future compiler lookups. */
    [[nodiscard]] foundation::Result unregister_pipeline(PipelineId id);
    /** @brief Returns a shared immutable view of one registered definition. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const PipelineDefinition>> find_definition(
        PipelineId id) const;
    /** @brief Returns the global Phase registry shared by pipeline compilation. */
    [[nodiscard]] PhaseRegistry& phase_registry() noexcept { return phase_registry_; }
    /** @brief Returns the read-only global Phase registry. */
    [[nodiscard]] const PhaseRegistry& phase_registry() const noexcept { return phase_registry_; }

private:
    struct Registration {
        std::shared_ptr<const PipelineDefinition> definition{};
    };

    [[nodiscard]] foundation::ResultValue<Registration> acquire_registration(PipelineId id) const;
    PhaseRegistry phase_registry_{};
    mutable std::shared_mutex mutex_;
    std::unordered_map<PipelineId, Registration, StableIdHash<PipelineIdTag>> registrations_{};
    friend struct detail::PipelineCompilerExecution;
};

/** @brief Input consumed by the Engine-owned compiler execution bridge. */
struct PipelineCompileInput {
    PipelineId pipeline{};
    PipelineScope scope{PipelineScope::kEngine};
    CapabilityMask available_capabilities{0};
    const PipelineRegistry* registry{nullptr};
    PipelineInstanceId instance{};
    const PhaseRegistry* phase_registry{nullptr};
};

/** @brief Visits Engine-private Stage plans in dependency order without business callbacks. */
class PipelineScheduler final {
public:
    PipelineScheduler() = default;
    ~PipelineScheduler() = default;

    PipelineScheduler(const PipelineScheduler&) = delete;
    PipelineScheduler& operator=(const PipelineScheduler&) = delete;
    PipelineScheduler(PipelineScheduler&&) noexcept = default;
    PipelineScheduler& operator=(PipelineScheduler&&) noexcept = default;

    /** @brief Observes all Stage nodes in compiled batch order. */
    [[nodiscard]] foundation::Result execute(const PipelinePlan& plan, PipelineExecutionContext& context) const;
    /** @brief Observes only Stage nodes assigned to one global Phase. */
    [[nodiscard]] foundation::Result execute_phase(const PipelinePlan& plan, PipelineExecutionContext& context,
                                                   PhaseId phase) const;

private:
    [[nodiscard]] foundation::Result execute_internal(const PipelinePlan& plan, PipelineExecutionContext& context,
                                                      const PhaseId* phase) const;
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
    [[nodiscard]] foundation::Result activate(const PipelineActivationContext& context) override;
    [[nodiscard]] foundation::Result deactivate(const PipelineDeactivationContext& context) override;
    [[nodiscard]] foundation::Result execute(PipelineScheduler& scheduler, PipelineExecutionContext& context) const;
    /** @brief Executes one Phase using an optional frame-pinned Plan. */
    [[nodiscard]] foundation::Result execute_phase(PipelineScheduler& scheduler, PipelineExecutionContext& context,
                                                   PhaseId phase, PipelinePlanHandle pinned_plan = {}) const;

private:
    friend struct detail::PipelineScopeHostExecution;
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

inline foundation::Result render_graph_builder_permission_denied(RenderGraphBuildFlags flag) {
    static_cast<void>(flag);
    return foundation::Result::failure(foundation::ResultCode::kUnsupported,
                                       "native graph build does not grant the requested render graph permission");
}

inline RenderGraphBuilder::RenderGraphBuilder(RenderGraphBuilderBackend& backend,
                                              const RenderGraphBuildContext& context) noexcept
    : runtime_backend_(&backend), build_context_(context) {}

inline RenderGraphBuildFlags RenderGraphBuilder::build_flags() const noexcept { return build_context_.flags; }
inline const void* RenderGraphBuilder::context_data() const noexcept { return build_context_.context_data; }
inline bool RenderGraphBuilder::allows(RenderGraphBuildFlags flag) const noexcept {
    return has_flag(build_context_.flags, flag);
}

inline bool RenderGraphBuilder::is_runtime() const noexcept {
    return runtime_backend_ != nullptr;
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

}  // namespace aeg::pipeline
