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

#include "result.hpp"
#include "frame_context.hpp"

namespace aeg::render_graph {
    /** @brief Sentinel used for invalid render-graph handle indices. */
    inline constexpr uint32_t kInvalidHandleIndex = std::numeric_limits<uint32_t>::max();

    /**
     * @brief Opaque handle for a logical render-graph texture.
     *
     * Handles are valid only for the graph generation that created them and are
     * invalidated by reset().
     */
    struct RenderGraphTexture {
        uint32_t index{kInvalidHandleIndex};
        uint32_t generation{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return index != kInvalidHandleIndex && generation != 0;
        }

        constexpr explicit operator bool() const noexcept { return is_valid(); }
        friend constexpr bool operator==(RenderGraphTexture, RenderGraphTexture) = default;
    };

    /** @brief Opaque handle for a logical render-graph buffer. */
    struct RenderGraphBuffer {
        uint32_t index{kInvalidHandleIndex};
        uint32_t generation{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return index != kInvalidHandleIndex && generation != 0;
        }

        constexpr explicit operator bool() const noexcept { return is_valid(); }
        friend constexpr bool operator==(RenderGraphBuffer, RenderGraphBuffer) = default;
    };

    /** @brief Opaque handle for a render-graph pass declaration. */
    struct RenderGraphPass {
        uint32_t index{kInvalidHandleIndex};
        uint32_t generation{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return index != kInvalidHandleIndex && generation != 0;
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
    };

    /** @brief Options controlling execution of a compiled graph. */
    struct RenderGraphExecuteDesc {
        bool present_acquired_frame{false};
    };

    /** @brief Submission and presentation results from execute(). */
    struct RenderGraphExecutionResult {
        std::array<rhi::GpuSyncPoint, kQueueCount> queue_completions{};
        rhi::GpuSyncPoint completion{};
        uint32_t executed_pass_count{0};
        uint32_t submitted_batch_count{0};
        bool presented{false};
        bool suboptimal{false};
    };

    class RenderGraph;
    class PassBuilder;
    class PassExecutionContext;

    /** @brief Callback that declares resource usage for a pass. */
    using PassSetupCallback = std::function<foundation::Result(PassBuilder&)>;
    /** @brief Callback that records commands for one compiled pass. */
    using PassExecuteCallback = std::function<foundation::Result(PassExecutionContext&)>;

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

        /** @brief Declares read and write access to a buffer range in one pass. */
        [[nodiscard]] foundation::Result read_write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                             rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                             rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                             RenderGraphBufferRange range = {});

        /** @brief Declares a read of texture subresources. */
        [[nodiscard]] foundation::Result read(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                       rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                       rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                       rhi::SubresourceRange subresources = {});

        /** @brief Declares a write of texture subresources. */
        [[nodiscard]] foundation::Result write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                        rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                        rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                        rhi::SubresourceRange subresources = {}, bool discard_contents = true);

        /** @brief Declares read and write access to texture subresources. */
        [[nodiscard]] foundation::Result read_write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                             rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                             rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                             rhi::SubresourceRange subresources = {});

        /** @brief Declares a color attachment and its load/store policy. */
        [[nodiscard]] foundation::Result use_color_attachment(
                RenderGraphTexture texture, rhi::LoadOp load_op, rhi::StoreOp store_op,
                rhi::ClearColorValue clear_color = {},
                rhi::ResourceUsage final_usage = rhi::ResourceUsage::kColorAttachmentWrite,
                rhi::SubresourceRange subresources = {});

        /** @brief Declares a depth/stencil attachment and its load/store policy. */
        [[nodiscard]] foundation::Result use_depth_stencil_attachment(
                RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op,
                float clear_depth = 1.0f, uint32_t clear_stencil = 0,
                rhi::ResourceUsage final_usage = rhi::ResourceUsage::kDepthStencilWrite,
                rhi::SubresourceRange subresources = {});

        /** @brief Marks the pass as externally observable and therefore non-cullable. */
        void set_side_effects(bool enabled = true) noexcept;

    private:
        friend class RenderGraph;

        /** @brief Creates a builder bound to one internal pass record. */
        PassBuilder(RenderGraph& graph, uint32_t pass_index) noexcept : graph_(graph), pass_index_(pass_index) {
        }

        RenderGraph& graph_;
        uint32_t pass_index_{kInvalidHandleIndex};
    };

    /**
     * @brief Provides resolved resources and command recording access to a pass.
     *
     * The context is valid only during its execute callback. Resource lookup
     * returns physical handles selected by the graph compiler.
     */
    class PassExecutionContext final {
    public:
        [[nodiscard]] rhi::Device& device() const noexcept { return device_; }
        [[nodiscard]] rhi::CommandListHandle command_list() const noexcept { return command_list_; }
        [[nodiscard]] RenderGraphPass pass() const noexcept { return pass_; }

        /** @brief Resolves a logical buffer to its physical RHI handle. */
        [[nodiscard]] foundation::ResultValue<rhi::BufferHandle> buffer(RenderGraphBuffer buffer) const;
        /** @brief Resolves a logical texture to its physical RHI handle. */
        [[nodiscard]] foundation::ResultValue<rhi::TextureHandle> texture(RenderGraphTexture texture) const;
        /** @brief Creates or resolves a view for a logical buffer. */
        [[nodiscard]] foundation::ResultValue<rhi::BufferViewHandle> buffer_view(RenderGraphBuffer buffer,
                                                                            rhi::BufferViewKind kind,
                                                                            uint64_t offset = 0,
                                                                            uint64_t size =
                                                                                std::numeric_limits<uint64_t>::max(),
                                                                            rhi::Format format = rhi::Format::kUnknown,
                                                                            uint32_t structure_stride = 0);
        /** @brief Creates or resolves a view for a logical texture. */
        [[nodiscard]] foundation::ResultValue<rhi::TextureViewHandle> texture_view(
                RenderGraphTexture texture, rhi::TextureViewKind kind = rhi::TextureViewKind::kSampled,
                rhi::SubresourceRange subresources = {}, rhi::Format format = rhi::Format::kUnknown,
                rhi::TextureViewDimension dimension = rhi::TextureViewDimension::kAuto);

        /** @brief Begins dynamic rendering using this pass's declared attachments. */
        [[nodiscard]] foundation::Result begin_rendering();
        /** @brief Ends dynamic rendering previously begun in this context. */
        [[nodiscard]] foundation::Result end_rendering();
        [[nodiscard]] bool rendering_active() const noexcept { return rendering_active_; }
        [[nodiscard]] bool rendering_started() const noexcept { return rendering_started_; }

        /** @brief Records a buffer-to-buffer copy between logical resources. */
        [[nodiscard]] foundation::Result copy_buffer(RenderGraphBuffer source, RenderGraphBuffer destination,
                                              uint64_t source_offset, uint64_t destination_offset, uint64_t size);
        /** @brief Records a buffer-to-texture copy between logical resources. */
        [[nodiscard]] foundation::Result copy_buffer_to_texture(
                RenderGraphBuffer source, uint64_t source_offset, uint32_t source_row_length,
                uint32_t source_image_height, RenderGraphTexture destination,
                rhi::TextureSubresourceLayers destination_subresources, rhi::Offset3D destination_offset,
                rhi::Extent3D extent);
        /** @brief Records a texture-to-buffer copy between logical resources. */
        [[nodiscard]] foundation::Result copy_texture_to_buffer(
                RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources,
                rhi::Offset3D source_offset, rhi::Extent3D extent, RenderGraphBuffer destination,
                uint64_t destination_offset, uint32_t destination_row_length, uint32_t destination_image_height);
        /** @brief Records a texture-to-texture copy between logical resources. */
        [[nodiscard]] foundation::Result copy_texture(RenderGraphTexture source,
                                               rhi::TextureSubresourceLayers source_subresources,
                                               rhi::Offset3D source_offset, RenderGraphTexture destination,
                                               rhi::TextureSubresourceLayers destination_subresources,
                                               rhi::Offset3D destination_offset, rhi::Extent3D extent);
        /** @brief Records a clear operation for a logical texture. */
        [[nodiscard]] foundation::Result clear_texture(RenderGraphTexture texture,
                                                 rhi::SubresourceRange subresources = {},
                                                 rhi::ClearColorValue color = {}, float depth = 1.0f,
                                                 uint32_t stencil = 0);

    private:
        friend class RenderGraph;

        PassExecutionContext(RenderGraph& graph, rhi::Device& device, RenderGraphPass pass,
                             rhi::CommandListHandle command_list, uint32_t pass_index,
                             uint32_t frame_slot) noexcept;

        RenderGraph& graph_;
        rhi::Device& device_;
        RenderGraphPass pass_{};
        rhi::CommandListHandle command_list_{};
        uint32_t pass_index_{kInvalidHandleIndex};
        uint32_t frame_slot_{kInvalidHandleIndex};
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
        [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> create_buffer(const rhi::BufferDesc& desc,
                                                                         ResourceLifetime lifetime =
                                                                                 ResourceLifetime::kTransient);
        /** @brief Declares a logical texture resource. */
        [[nodiscard]] foundation::ResultValue<RenderGraphTexture> create_texture(
                const rhi::TextureDesc& desc,
                ResourceLifetime lifetime = ResourceLifetime::kTransient);

        /** @brief Imports an externally owned buffer into the graph. */
        [[nodiscard]] foundation::ResultValue<RenderGraphBuffer> import_buffer(
                rhi::BufferHandle buffer, const rhi::BufferDesc& desc,
                const rhi::ResourceState& initial_state = {});
        /** @brief Imports an externally owned texture and default view. */
        [[nodiscard]] foundation::ResultValue<RenderGraphTexture> import_texture(
                rhi::TextureHandle texture, rhi::TextureViewHandle default_view, const rhi::TextureDesc& desc,
                const rhi::ResourceState& initial_state = {});
        /** @brief Replaces the native buffer attached to an imported resource. */
        [[nodiscard]] foundation::Result update_imported_buffer(RenderGraphBuffer resource, rhi::BufferHandle buffer,
                                                         const rhi::ResourceState& initial_state = {});
        /** @brief Replaces the native texture attached to an imported resource. */
        [[nodiscard]] foundation::Result update_imported_texture(
            RenderGraphTexture resource, rhi::TextureHandle texture, rhi::TextureViewHandle default_view,
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

        /** @brief Analyzes dependencies, allocates transient resources, and emits barriers. */
        [[nodiscard]] foundation::Result compile();
        /** @brief Returns statistics from the last successful compile(). */
        [[nodiscard]] const RenderGraphCompileInfo& compile_info() const noexcept { return compile_info_; }
        /** @brief Executes the last successful compilation in a frame context. */
        [[nodiscard]] foundation::ResultValue<RenderGraphExecutionResult> execute(
                FrameContext& frame_context, const RenderGraphExecuteDesc& desc = {});

    private:
        friend class PassBuilder;
        friend class PassExecutionContext;

        /** @brief Opaque implementation containing graph compiler and runtime state. */
        struct Impl;
        std::unique_ptr<Impl> impl_{};
        rhi::Device& device_;
        RenderGraphCompileInfo compile_info_{};
    };
} // namespace aeg::render_graph
