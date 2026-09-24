/**
 * @file render_graph.hpp
 * @brief Declarative resource scheduling, lifetime analysis, barriers, and pass execution.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "frame_context.hpp"
#include "result.hpp"

namespace aeg::render_graph {
/** @brief Sentinel used for invalid render-graph handle indices. */
inline constexpr uint32_t kInvalidHandleIndex = std::numeric_limits<uint32_t>::max();

/**
 * @brief Opaque handle for a logical render-graph texture.
 *
 * Handles are valid only for the graph instance and resource generation
 * that created them. They are invalidated by reset().
 */
struct RenderGraphTexture {
    uint32_t index{kInvalidHandleIndex};
    uint32_t generation{0};
    uint64_t graph_id{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != kInvalidHandleIndex && generation != 0 && graph_id != 0;
    }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(RenderGraphTexture, RenderGraphTexture) = default;
};

/** @brief Opaque handle for a logical render-graph buffer. */
struct RenderGraphBuffer {
    uint32_t index{kInvalidHandleIndex};
    uint32_t generation{0};
    uint64_t graph_id{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != kInvalidHandleIndex && generation != 0 && graph_id != 0;
    }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(RenderGraphBuffer, RenderGraphBuffer) = default;
};

/** @brief Opaque handle for a render-graph pass declaration. */
struct RenderGraphPass {
    uint32_t index{kInvalidHandleIndex};
    uint32_t generation{0};
    uint64_t graph_id{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != kInvalidHandleIndex && generation != 0 && graph_id != 0;
    }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(RenderGraphPass, RenderGraphPass) = default;
};

/** @brief Byte range of a logical buffer used by one pass. */
struct RenderGraphBufferRange {
    uint64_t offset{0};
    uint64_t size{std::numeric_limits<uint64_t>::max()};
};

/** @brief Determines how the graph allocates and owns a logical resource. */
enum class ResourceLifetime : uint8_t { kTransient, kPersistent, kImported };
/** @brief Declares whether a pass reads, writes, or both reads and writes a resource. */
enum class ResourceAccess : uint8_t { kRead, kWrite, kReadWrite };

/** @brief Position of a compiled barrier in a pass command stream. */
enum class RenderGraphBarrierPhase : uint8_t { kBeforePass, kAfterPass };

/** @brief Semantic reason for a compiled render-graph barrier. */
enum class RenderGraphBarrierKind : uint8_t {
    kTransition,
    kMemoryDependency,
    kQueueTransfer,
};

/** @brief Resource kind referenced by a compiled render-graph barrier. */
enum class RenderGraphResourceKind : uint8_t { kBuffer, kTexture };

/**
 * @brief Logical barrier requirement emitted by RenderGraph::compile().
 *
 * Handles are intentionally not stored here. Physical handles are resolved
 * for the active frame slot during execute(). The before state is the
 * compiler's expected state; execution resolves the actual runtime state
 * before recording the explicit barrier.
 */
struct RenderGraphBarrierPlan {
    RenderGraphResourceKind resource_kind{RenderGraphResourceKind::kBuffer};
    uint32_t resource_index{kInvalidHandleIndex};
    uint32_t resource_generation{0};
    RenderGraphBarrierPhase phase{RenderGraphBarrierPhase::kBeforePass};
    RenderGraphBarrierKind kind{RenderGraphBarrierKind::kTransition};
    ResourceAccess access{ResourceAccess::kRead};
    rhi::ResourceState before{};
    rhi::ResourceState after{};
    RenderGraphBufferRange buffer_range{};
    rhi::SubresourceRange subresources{};
    bool discard_contents{false};
    bool requires_memory_dependency{false};
    bool requires_queue_transfer{false};
};

/** @brief Compiled pass topology and barrier requirements. */
struct RenderGraphPassPlan {
    RenderGraphPass pass{};
    uint32_t batch_index{kInvalidHandleIndex};
    uint32_t topological_index{kInvalidHandleIndex};
    bool live{false};
    std::vector<RenderGraphBarrierPlan> barriers{};
};

/** @brief Audited dependency between two queue submission batches. */
struct RenderGraphQueueDependency {
    uint32_t source_batch{kInvalidHandleIndex};
    uint32_t destination_batch{kInvalidHandleIndex};
    rhi::QueueClass source_queue{rhi::QueueClass::kAuto};
    rhi::QueueClass destination_queue{rhi::QueueClass::kAuto};
    rhi::PipelineStageMask destination_stages{rhi::PipelineStage::kNone};
    uint32_t resource_hazard_count{0};
};

/** @brief Compiled queue batch and its pass membership. */
struct RenderGraphBatchPlan {
    uint32_t index{kInvalidHandleIndex};
    rhi::QueueClass queue{rhi::QueueClass::kGraphics};
    std::vector<uint32_t> pass_indices{};
    std::vector<RenderGraphQueueDependency> dependencies{};
    bool submits_frame{false};
    bool waits_for_acquire{false};
};

/**
 * @brief Immutable logical execution plan produced by RenderGraph::compile().
 *
 * The plan is invalidated by reset(), clear_passes(), imported-resource
 * replacement, or any declaration change that requires recompilation. It
 * contains no backend-native data.
 */
struct RenderGraphPlan {
    uint64_t revision{0};
    std::vector<uint32_t> topological_order{};
    std::vector<RenderGraphPassPlan> pass_plans{};
    std::vector<RenderGraphBatchPlan> batches{};
    std::vector<RenderGraphQueueDependency> queue_dependencies{};
};

/**
 * @brief Statistics produced by compile().
 *
 * Counts describe the last successful compilation and are reset when the
 * graph is reset or compilation fails.
 */
struct RenderGraphCompileInfo {
    uint32_t resource_count{0};
    uint32_t pass_count{0};
    uint32_t culled_pass_count{0};
    uint32_t batch_count{0};
    uint32_t transient_buffer_slot_count{0};
    uint32_t transient_texture_slot_count{0};
    uint32_t barrier_count{0};
    uint32_t barrier_point_count{0};
    uint32_t queue_dependency_count{0};
};

/**
 * @brief Shared ownership retained until the frame consuming a graph completes.
 *
 * The lease is borrowed while RenderGraph::execute() runs. The shared
 * ownership is copied into the active FrameContext slot before submission.
 */
class RenderGraphExecutionLease final {
public:
    RenderGraphExecutionLease() noexcept = default;
    explicit RenderGraphExecutionLease(std::shared_ptr<const void> owner) noexcept : owner_(std::move(owner)) {}

    [[nodiscard]] bool is_valid() const noexcept { return owner_.use_count() != 0; }

private:
    friend class RenderGraph;

    [[nodiscard]] const std::shared_ptr<const void>& owner() const noexcept { return owner_; }

    std::shared_ptr<const void> owner_{};
};

/**
 * @brief Options controlling execution of a compiled graph.
 *
 * When an acquired swapchain image is part of the graph, RenderGraph
 * automatically waits on acquire for the first access batch and signals
 * present from the final access batch. present_acquired_frame only selects
 * whether present() is called before execute() returns.
 *
 * execution_leases must remain valid until execute() returns. They are
 * not retained by the descriptor after the call.
 */
struct RenderGraphExecuteDesc {
    bool present_acquired_frame{false};
    std::span<const RenderGraphExecutionLease> execution_leases{};
};

/** @brief Submission and presentation results from execute(). */
struct RenderGraphExecutionResult {
    rhi::GpuCompletionSet queue_completions{};
    rhi::GpuSyncPoint completion{};
    /** @brief Number of successful graphics draw commands recorded by this graph execution. */
    uint32_t draw_call_count{0};
    uint32_t executed_pass_count{0};
    uint32_t submitted_batch_count{0};
    bool presented{false};
    bool suboptimal{false};
};

class RenderGraph;
class PassBuilder;
class RenderPassCommandContext;
using PassExecutionContext = RenderPassCommandContext;
class RenderGraphFeatureRuntime;

/** @brief Callback that declares resource usage for a pass. */
using PassSetupCallback = std::function<foundation::Result(PassBuilder&)>;
/** @brief Callback that records commands for one compiled pass. */
using PassExecuteCallback = std::function<foundation::Result(RenderPassCommandContext&)>;

/**
 * @brief Declares the resource contract of one render-graph pass.
 *
 * A builder is valid only during its setup callback. Declarations are used
 * for dependency analysis, transient allocation, and automatic barriers.
 */
class PassBuilder final {
public:
    /** @brief Declares a read of a buffer range. */
    [[nodiscard]] foundation::Result read(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                          rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                          rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                          RenderGraphBufferRange range = {});

    /** @brief Declares a write of a buffer range. */
    [[nodiscard]] foundation::Result write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                           rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                           rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                           RenderGraphBufferRange range = {}, bool discard_contents = true);
    /** @brief Declares a write with an explicit post-write resource state. */
    [[nodiscard]] foundation::Result write_to(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                              rhi::PipelineStageMask pipeline_stages,
                                              rhi::ShaderStageMask shader_stages, rhi::ResourceUsage final_usage,
                                              RenderGraphBufferRange range = {}, bool discard_contents = true);

    /** @brief Declares read and write access to a buffer range in one pass. */
    [[nodiscard]] foundation::Result read_write(
        RenderGraphBuffer buffer, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
        rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll, RenderGraphBufferRange range = {});

    /** @brief Declares a read of texture subresources. */
    [[nodiscard]] foundation::Result read(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                          rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                          rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                          rhi::SubresourceRange subresources = {});
    /** @brief Declares a texture read with an explicit post-read resource state. */
    [[nodiscard]] foundation::Result read_to(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                             rhi::PipelineStageMask pipeline_stages, rhi::ShaderStageMask shader_stages,
                                             rhi::ResourceUsage final_usage, rhi::SubresourceRange subresources = {});

    /** @brief Declares a write of texture subresources. */
    [[nodiscard]] foundation::Result write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                           rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                           rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                           rhi::SubresourceRange subresources = {}, bool discard_contents = true);

    /** @brief Declares read and write access to texture subresources. */
    [[nodiscard]] foundation::Result read_write(
        RenderGraphTexture texture, rhi::ResourceUsage usage,
        rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
        rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll, rhi::SubresourceRange subresources = {});

    /** @brief Declares a color attachment and its load/store policy. */
    [[nodiscard]] foundation::Result use_color_attachment(
        RenderGraphTexture texture, rhi::LoadOp load_op, rhi::StoreOp store_op, rhi::ClearColorValue clear_color = {},
        rhi::ResourceUsage final_usage = rhi::ResourceUsage::kColorAttachmentWrite,
        rhi::SubresourceRange subresources = {});

    /** @brief Declares a depth/stencil attachment and its load/store policy. */
    [[nodiscard]] foundation::Result use_depth_stencil_attachment(
        RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op, float clear_depth = 1.0f,
        uint32_t clear_stencil = 0, rhi::ResourceUsage final_usage = rhi::ResourceUsage::kDepthStencilWrite,
        rhi::SubresourceRange subresources = {});

    /** @brief Marks the pass as externally observable and therefore non-cullable. */
    void set_side_effects(bool enabled = true) noexcept;

private:
    friend class RenderGraph;

    /** @brief Creates a builder bound to one internal pass record. */
    PassBuilder(RenderGraph& graph, uint32_t pass_index) noexcept : graph_(graph), pass_index_(pass_index) {}

    RenderGraph& graph_;
    uint32_t pass_index_{kInvalidHandleIndex};
};

/**
 * @brief Provides restricted resource resolution and command recording to a pass.
 *
 * The context is valid only during its execute callback. Resource and view
 * lookup returns physical handles selected or prepared by the graph. The
 * RHI device and command-list handle remain private to the graph.
 */
class RenderPassCommandContext final {
public:
    [[nodiscard]] RenderGraphPass pass() const noexcept { return pass_; }

    /** @brief Resolves a logical buffer to its physical RHI handle. */
    [[nodiscard]] foundation::ResultValue<rhi::BufferHandle> resolve_buffer(RenderGraphBuffer buffer) const;
    /** @brief Resolves a logical texture to its physical RHI handle. */
    [[nodiscard]] foundation::ResultValue<rhi::TextureHandle> resolve_texture(RenderGraphTexture texture) const;
    /** @brief Resolves a previously prepared view for a logical buffer. */
    [[nodiscard]] foundation::ResultValue<rhi::BufferViewHandle> resolve_buffer_view(
        RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset = 0,
        uint64_t size = std::numeric_limits<uint64_t>::max(), rhi::Format format = rhi::Format::kUnknown,
        uint32_t structure_stride = 0) const;
    /** @brief Resolves a previously prepared view for a logical texture. */
    [[nodiscard]] foundation::ResultValue<rhi::TextureViewHandle> resolve_texture_view(
        RenderGraphTexture texture, rhi::TextureViewKind kind = rhi::TextureViewKind::kSampled,
        rhi::SubresourceRange subresources = {}, rhi::Format format = rhi::Format::kUnknown,
        rhi::TextureViewDimension dimension = rhi::TextureViewDimension::kAuto) const;

    /** @brief Begins dynamic rendering using this pass's declared attachments. */
    [[nodiscard]] foundation::Result begin_rendering();
    /** @brief Ends dynamic rendering previously begun in this context. */
    [[nodiscard]] foundation::Result end_rendering();
    [[nodiscard]] bool rendering_active() const noexcept { return rendering_active_; }
    [[nodiscard]] bool rendering_started() const noexcept { return rendering_started_; }

    /** @brief Records dynamic graphics state and resource bindings. */
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

    /** @brief Records a buffer-to-buffer copy between logical resources. */
    [[nodiscard]] foundation::Result copy_buffer(RenderGraphBuffer source, RenderGraphBuffer destination,
                                                 uint64_t source_offset, uint64_t destination_offset, uint64_t size);
    /** @brief Records a buffer-to-texture copy between logical resources. */
    [[nodiscard]] foundation::Result copy_buffer_to_texture(RenderGraphBuffer source, uint64_t source_offset,
                                                            uint32_t source_row_length, uint32_t source_image_height,
                                                            RenderGraphTexture destination,
                                                            rhi::TextureSubresourceLayers destination_subresources,
                                                            rhi::Offset3D destination_offset, rhi::Extent3D extent);
    /** @brief Records a texture-to-buffer copy between logical resources. */
    [[nodiscard]] foundation::Result copy_texture_to_buffer(RenderGraphTexture source,
                                                            rhi::TextureSubresourceLayers source_subresources,
                                                            rhi::Offset3D source_offset, rhi::Extent3D extent,
                                                            RenderGraphBuffer destination, uint64_t destination_offset,
                                                            uint32_t destination_row_length,
                                                            uint32_t destination_image_height);
    /** @brief Records a texture-to-texture copy between logical resources. */
    [[nodiscard]] foundation::Result copy_texture(RenderGraphTexture source,
                                                  rhi::TextureSubresourceLayers source_subresources,
                                                  rhi::Offset3D source_offset, RenderGraphTexture destination,
                                                  rhi::TextureSubresourceLayers destination_subresources,
                                                  rhi::Offset3D destination_offset, rhi::Extent3D extent);
    /** @brief Records a clear operation for a logical texture. */
    [[nodiscard]] foundation::Result clear_texture(RenderGraphTexture texture, rhi::SubresourceRange subresources = {},
                                                   rhi::ClearColorValue color = {}, float depth = 1.0f,
                                                   uint32_t stencil = 0);

private:
    friend class RenderGraph;

    RenderPassCommandContext(RenderGraph& graph, rhi::Device& device, RenderGraphPass pass,
                             rhi::CommandListHandle command_list, uint32_t pass_index, uint32_t frame_slot,
                             uint32_t& draw_call_count) noexcept;

    RenderGraph& graph_;
    rhi::Device& device_;
    RenderGraphPass pass_{};
    rhi::CommandListHandle command_list_{};
    uint32_t pass_index_{kInvalidHandleIndex};
    uint32_t frame_slot_{kInvalidHandleIndex};
    uint32_t* draw_call_count_{nullptr};
    bool rendering_active_{false};
    bool rendering_started_{false};
};

/**
 * @brief Builds and executes a dependency-aware render graph.
 *
 * The graph owns logical resource declarations and transient physical
 * allocations. Imported resources remain owned by the caller. A graph must
 * be compiled successfully after declarations change and before execute().
 */
class RenderGraph final {
public:
    explicit RenderGraph(rhi::Device& device);
    ~RenderGraph();

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;

    [[nodiscard]] rhi::Device& device() const noexcept { return device_; }

    /**
     * @brief Clears passes and all graph-owned logical resources.
     *
     * Imported RHI resources are detached but never destroyed by the graph.
     */
    void reset() noexcept;
    /**
     * @brief Clears pass declarations while preserving logical resources.
     *
     * Persistent allocations and imported-resource records remain available
     * for the next set of pass declarations.
     */
    void clear_passes() noexcept;

    /** @brief Declares a logical buffer resource. */
    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> create_buffer(
        const rhi::BufferDesc& desc, ResourceLifetime lifetime = ResourceLifetime::kTransient);
    /** @brief Declares a logical texture resource. */
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> create_texture(
        const rhi::TextureDesc& desc, ResourceLifetime lifetime = ResourceLifetime::kTransient);

    /** @brief Tests whether a buffer handle belongs to this graph instance. */
    [[nodiscard]] bool contains_buffer(RenderGraphBuffer resource) const noexcept;
    /** @brief Tests whether a texture handle belongs to this graph instance. */
    [[nodiscard]] bool contains_texture(RenderGraphTexture resource) const noexcept;

    /** @brief Imports an externally owned buffer into the graph. */
    [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> import_buffer(
        rhi::BufferHandle buffer, const rhi::BufferDesc& desc, const rhi::ResourceState& initial_state = {});
    /** @brief Imports an externally owned texture and default view. */
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> import_texture(
        rhi::TextureHandle texture, rhi::TextureViewHandle default_view, const rhi::TextureDesc& desc,
        const rhi::ResourceState& initial_state = {});
    /** @brief Replaces the native buffer attached to an imported resource. */
    [[nodiscard]] foundation::Result update_imported_buffer(RenderGraphBuffer resource, rhi::BufferHandle buffer,
                                                            const rhi::ResourceState& initial_state = {});
    /** @brief Replaces the native texture attached to an imported resource. */
    [[nodiscard]] foundation::Result update_imported_texture(RenderGraphTexture resource, rhi::TextureHandle texture,
                                                             rhi::TextureViewHandle default_view,
                                                             const rhi::ResourceState& initial_state = {});
    /** @brief Replaces an imported texture and refreshes its logical description. */
    [[nodiscard]] foundation::Result update_imported_texture(RenderGraphTexture resource, rhi::TextureHandle texture,
                                                             rhi::TextureViewHandle default_view,
                                                             const rhi::TextureDesc& desc,
                                                             const rhi::ResourceState& initial_state = {});
    /** @brief Imports an acquired swapchain image as a graph texture. */
    [[nodiscard]] foundation::ResultValue<RenderGraphTexture> import_swapchain_image(
        const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
        const rhi::ResourceState& initial_state = {});

    /** @brief Keeps a logical buffer alive as an externally consumable output. */
    [[nodiscard]] foundation::Result export_buffer(RenderGraphBuffer resource);
    /** @brief Keeps a logical texture alive as an externally consumable output. */
    [[nodiscard]] foundation::Result export_texture(RenderGraphTexture resource);

    /** @brief Adds a pass declaration to the graph. */
    [[nodiscard]] foundation::ResultValue<RenderGraphPass> add_pass(std::string_view name, rhi::QueueClass queue,
                                                                    PassSetupCallback setup,
                                                                    PassExecuteCallback execute);
    /** @brief Requires the source pass to finish before the destination pass begins. */
    [[nodiscard]] foundation::Result add_dependency(RenderGraphPass source, RenderGraphPass destination);

    /** @brief Analyzes dependencies, allocates transient resources, and emits barriers. */
    [[nodiscard]] foundation::Result compile();
    /** @brief Returns statistics from the last successful compile(). */
    [[nodiscard]] const RenderGraphCompileInfo& compile_info() const noexcept { return compile_info_; }
    /** @brief Returns the backend-neutral plan from the last successful compile(). */
    [[nodiscard]] const RenderGraphPlan& plan() const noexcept;
    /** @brief Executes the last successful compilation in a frame context. */
    [[nodiscard]] foundation::ResultValue<RenderGraphExecutionResult> execute(FrameContext& frame_context,
                                                                              const RenderGraphExecuteDesc& desc = {});

private:
    friend class PassBuilder;
    friend class RenderPassCommandContext;
    friend class RenderGraphFeatureRuntime;

    [[nodiscard]] foundation::Result begin_feature_build_transaction();
    [[nodiscard]] foundation::Result retain_execution_owner(std::shared_ptr<const void> owner);
    void commit_feature_build_transaction() noexcept;
    void rollback_feature_build_transaction() noexcept;

    /** @brief Opaque implementation containing graph compiler and runtime state. */
    struct Impl;
    std::unique_ptr<Impl> impl_{};
    rhi::Device& device_;
    RenderGraphCompileInfo compile_info_{};
};
}  // namespace aeg::render_graph
