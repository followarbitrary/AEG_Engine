#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

#include "frame_context.hpp"

namespace aeg::render_graph {
    inline constexpr uint32_t kInvalidHandleIndex = std::numeric_limits<uint32_t>::max();

    struct RenderGraphTexture {
        uint32_t index{kInvalidHandleIndex};
        uint32_t generation{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return index != kInvalidHandleIndex && generation != 0;
        }

        constexpr explicit operator bool() const noexcept { return is_valid(); }
        friend constexpr bool operator==(RenderGraphTexture, RenderGraphTexture) = default;
    };

    struct RenderGraphBuffer {
        uint32_t index{kInvalidHandleIndex};
        uint32_t generation{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return index != kInvalidHandleIndex && generation != 0;
        }

        constexpr explicit operator bool() const noexcept { return is_valid(); }
        friend constexpr bool operator==(RenderGraphBuffer, RenderGraphBuffer) = default;
    };

    struct RenderGraphPass {
        uint32_t index{kInvalidHandleIndex};
        uint32_t generation{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return index != kInvalidHandleIndex && generation != 0;
        }

        constexpr explicit operator bool() const noexcept { return is_valid(); }
        friend constexpr bool operator==(RenderGraphPass, RenderGraphPass) = default;
    };

    struct RenderGraphBufferRange {
        uint64_t offset{0};
        uint64_t size{std::numeric_limits<uint64_t>::max()};
    };

    enum class ResourceLifetime : uint8_t { kTransient, kPersistent, kImported };
    enum class ResourceAccess : uint8_t { kRead, kWrite, kReadWrite };

    struct RenderGraphCompileInfo {
        uint32_t resource_count{0};
        uint32_t pass_count{0};
        uint32_t culled_pass_count{0};
        uint32_t batch_count{0};
        uint32_t transient_buffer_slot_count{0};
        uint32_t transient_texture_slot_count{0};
        uint32_t barrier_count{0};
    };

    struct RenderGraphExecuteDesc {
        bool present_acquired_frame{false};
    };

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

    using PassSetupCallback = std::function<rhi::Result(PassBuilder&)>;
    using PassExecuteCallback = std::function<rhi::Result(PassExecutionContext&)>;

    class PassBuilder final {
    public:
        [[nodiscard]] rhi::Result read(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                       rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                       rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                       RenderGraphBufferRange range = {});

        [[nodiscard]] rhi::Result write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                        rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                        rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                        RenderGraphBufferRange range = {}, bool discard_contents = true);

        [[nodiscard]] rhi::Result read_write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                             rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                             rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                             RenderGraphBufferRange range = {});

        [[nodiscard]] rhi::Result read(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                       rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                       rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                       rhi::SubresourceRange subresources = {});

        [[nodiscard]] rhi::Result write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                        rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                        rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                        rhi::SubresourceRange subresources = {}, bool discard_contents = true);

        [[nodiscard]] rhi::Result read_write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                             rhi::PipelineStageMask pipeline_stages = rhi::PipelineStage::kAllCommands,
                                             rhi::ShaderStageMask shader_stages = rhi::ShaderStage::kAll,
                                             rhi::SubresourceRange subresources = {});

        [[nodiscard]] rhi::Result use_color_attachment(
                RenderGraphTexture texture, rhi::LoadOp load_op, rhi::StoreOp store_op,
                rhi::ClearColorValue clear_color = {},
                rhi::ResourceUsage final_usage = rhi::ResourceUsage::kColorAttachmentWrite,
                rhi::SubresourceRange subresources = {});

        [[nodiscard]] rhi::Result use_depth_stencil_attachment(
                RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op,
                float clear_depth = 1.0f, uint32_t clear_stencil = 0,
                rhi::ResourceUsage final_usage = rhi::ResourceUsage::kDepthStencilWrite,
                rhi::SubresourceRange subresources = {});

        void set_side_effects(bool enabled = true) noexcept;

    private:
        friend class RenderGraph;

        PassBuilder(RenderGraph& graph, uint32_t pass_index) noexcept : graph_(graph), pass_index_(pass_index) {
        }

        RenderGraph& graph_;
        uint32_t pass_index_{kInvalidHandleIndex};
    };

    class PassExecutionContext final {
    public:
        [[nodiscard]] rhi::Device& device() const noexcept { return device_; }
        [[nodiscard]] rhi::CommandListHandle command_list() const noexcept { return command_list_; }
        [[nodiscard]] RenderGraphPass pass() const noexcept { return pass_; }

        [[nodiscard]] rhi::ResultValue<rhi::BufferHandle> buffer(RenderGraphBuffer buffer) const;
        [[nodiscard]] rhi::ResultValue<rhi::TextureHandle> texture(RenderGraphTexture texture) const;
        [[nodiscard]] rhi::ResultValue<rhi::BufferViewHandle> buffer_view(RenderGraphBuffer buffer,
                                                                            rhi::BufferViewKind kind,
                                                                            uint64_t offset = 0,
                                                                            uint64_t size = std::numeric_limits<uint64_t>::max(),
                                                                            rhi::Format format = rhi::Format::kUnknown,
                                                                            uint32_t structure_stride = 0);
        [[nodiscard]] rhi::ResultValue<rhi::TextureViewHandle> texture_view(
                RenderGraphTexture texture, rhi::TextureViewKind kind = rhi::TextureViewKind::kSampled,
                rhi::SubresourceRange subresources = {}, rhi::Format format = rhi::Format::kUnknown,
                rhi::TextureViewDimension dimension = rhi::TextureViewDimension::kAuto);

        [[nodiscard]] rhi::Result begin_rendering();
        [[nodiscard]] rhi::Result end_rendering();
        [[nodiscard]] bool rendering_active() const noexcept { return rendering_active_; }
        [[nodiscard]] bool rendering_started() const noexcept { return rendering_started_; }

        [[nodiscard]] rhi::Result copy_buffer(RenderGraphBuffer source, RenderGraphBuffer destination,
                                              uint64_t source_offset, uint64_t destination_offset, uint64_t size);
        [[nodiscard]] rhi::Result copy_buffer_to_texture(
                RenderGraphBuffer source, uint64_t source_offset, uint32_t source_row_length,
                uint32_t source_image_height, RenderGraphTexture destination,
                rhi::TextureSubresourceLayers destination_subresources, rhi::Offset3D destination_offset,
                rhi::Extent3D extent);
        [[nodiscard]] rhi::Result copy_texture_to_buffer(
                RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources,
                rhi::Offset3D source_offset, rhi::Extent3D extent, RenderGraphBuffer destination,
                uint64_t destination_offset, uint32_t destination_row_length, uint32_t destination_image_height);
        [[nodiscard]] rhi::Result copy_texture(RenderGraphTexture source,
                                               rhi::TextureSubresourceLayers source_subresources,
                                               rhi::Offset3D source_offset, RenderGraphTexture destination,
                                               rhi::TextureSubresourceLayers destination_subresources,
                                               rhi::Offset3D destination_offset, rhi::Extent3D extent);
        [[nodiscard]] rhi::Result clear_texture(RenderGraphTexture texture,
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

    class RenderGraph final {
    public:
        explicit RenderGraph(rhi::Device& device);
        ~RenderGraph();

        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;
        RenderGraph(RenderGraph&&) = delete;
        RenderGraph& operator=(RenderGraph&&) = delete;

        [[nodiscard]] rhi::Device& device() const noexcept { return device_; }

        // Clears passes and all graph-owned resources. Imported resources are not destroyed by the graph.
        void reset() noexcept;
        // Clears only pass declarations, preserving logical resource handles and persistent allocations.
        void clear_passes() noexcept;

        [[nodiscard]] rhi::ResultValue<RenderGraphBuffer> create_buffer(const rhi::BufferDesc& desc,
                                                                         ResourceLifetime lifetime =
                                                                                 ResourceLifetime::kTransient);
        [[nodiscard]] rhi::ResultValue<RenderGraphTexture> create_texture(
                const rhi::TextureDesc& desc,
                ResourceLifetime lifetime = ResourceLifetime::kTransient);

        [[nodiscard]] rhi::ResultValue<RenderGraphBuffer> import_buffer(
                rhi::BufferHandle buffer, const rhi::BufferDesc& desc,
                const rhi::ResourceState& initial_state = {});
        [[nodiscard]] rhi::ResultValue<RenderGraphTexture> import_texture(
                rhi::TextureHandle texture, rhi::TextureViewHandle default_view, const rhi::TextureDesc& desc,
                const rhi::ResourceState& initial_state = {});
        [[nodiscard]] rhi::Result update_imported_buffer(RenderGraphBuffer resource, rhi::BufferHandle buffer,
                                                         const rhi::ResourceState& initial_state = {});
        [[nodiscard]] rhi::Result update_imported_texture(RenderGraphTexture resource, rhi::TextureHandle texture,
                                                          rhi::TextureViewHandle default_view,
                                                          const rhi::ResourceState& initial_state = {});
        [[nodiscard]] rhi::ResultValue<RenderGraphTexture> import_swapchain_image(
                const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
                const rhi::ResourceState& initial_state = {});

        [[nodiscard]] rhi::Result export_buffer(RenderGraphBuffer resource);
        [[nodiscard]] rhi::Result export_texture(RenderGraphTexture resource);

        [[nodiscard]] rhi::ResultValue<RenderGraphPass> add_pass(std::string_view name, rhi::QueueClass queue,
                                                                  PassSetupCallback setup,
                                                                  PassExecuteCallback execute);

        [[nodiscard]] rhi::Result compile();
        [[nodiscard]] const RenderGraphCompileInfo& compile_info() const noexcept { return compile_info_; }
        [[nodiscard]] rhi::ResultValue<RenderGraphExecutionResult> execute(
                FrameContext& frame_context, const RenderGraphExecuteDesc& desc = {});

    private:
        friend class PassBuilder;
        friend class PassExecutionContext;

        struct Impl;
        std::unique_ptr<Impl> impl_{};
        rhi::Device& device_;
        RenderGraphCompileInfo compile_info_{};
    };
} // namespace aeg::render_graph
