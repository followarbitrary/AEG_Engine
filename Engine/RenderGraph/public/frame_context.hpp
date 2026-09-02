#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "result.hpp"
#include "rhi.hpp"

namespace aeg::render_graph {
    inline constexpr uint32_t kQueueCount = 3;

    [[nodiscard]] constexpr uint32_t queue_index(rhi::QueueClass queue) noexcept {
        switch (queue) {
            case rhi::QueueClass::kCompute:
                return 1;
            case rhi::QueueClass::kCopy:
                return 2;
            case rhi::QueueClass::kAuto:
            case rhi::QueueClass::kGraphics:
            case rhi::QueueClass::kPresent:
                return 0;
        }
        return 0;
    }

    [[nodiscard]] constexpr rhi::QueueClass normalize_queue(rhi::QueueClass queue) noexcept {
        return queue == rhi::QueueClass::kAuto ? rhi::QueueClass::kGraphics : queue;
    }

    struct FrameContextDesc {
        uint32_t frames_in_flight{2};
        uint64_t upload_chunk_size{4u * 1024u * 1024u};
        uint64_t readback_chunk_size{4u * 1024u * 1024u};
        uint64_t upload_alignment{256};
        uint64_t wait_timeout_nanoseconds{std::numeric_limits<uint64_t>::max()};
    };

    struct FrameAllocation {
        rhi::BufferHandle buffer{};
        uint64_t offset{0};
        uint64_t size{0};
        uint32_t frame_slot{std::numeric_limits<uint32_t>::max()};
        uint64_t frame_number{0};
        uint64_t context_cookie{0};
        uint64_t buffer_size{0};

        [[nodiscard]] bool is_valid() const noexcept {
            return buffer.is_valid() && size != 0 && frame_slot != std::numeric_limits<uint32_t>::max() &&
                   context_cookie != 0;
        }

        [[nodiscard]] rhi::BufferRange as_range() const noexcept {
            return rhi::BufferRange{buffer, offset, size};
        }
    };

    class FrameContext final {
    public:
        explicit FrameContext(rhi::Device& device, const FrameContextDesc& desc = {});
        ~FrameContext();

        FrameContext(const FrameContext&) = delete;
        FrameContext& operator=(const FrameContext&) = delete;
        FrameContext(FrameContext&&) = delete;
        FrameContext& operator=(FrameContext&&) = delete;

        [[nodiscard]] rhi::Device& device() const noexcept { return device_; }
        [[nodiscard]] const FrameContextDesc& desc() const noexcept { return desc_; }
        [[nodiscard]] bool is_frame_active() const noexcept { return frame_active_; }
        [[nodiscard]] uint32_t frame_slot_index() const noexcept { return current_slot_; }
        [[nodiscard]] uint32_t frames_in_flight() const noexcept { return desc_.frames_in_flight; }
        [[nodiscard]] uint64_t frame_number() const noexcept { return frame_number_; }

        [[nodiscard]] foundation::Result begin_frame();
        [[nodiscard]] foundation::Result end_frame();
        [[nodiscard]] foundation::Result abort_frame();

        [[nodiscard]] foundation::ResultValue<rhi::FrameAcquireResult> acquire_next_image(
            rhi::SwapchainHandle swapchain);
        [[nodiscard]] const rhi::FrameAcquireResult* acquired_frame() const noexcept;
        [[nodiscard]] foundation::Result cancel_frame();
        [[nodiscard]] foundation::ResultValue<rhi::PresentResult> present();

        [[nodiscard]] foundation::ResultValue<rhi::CommandListHandle> begin_command_list(
                rhi::QueueClass queue, uint32_t batch_index = 0);
        [[nodiscard]] foundation::Result end_command_list(rhi::CommandListHandle command_list);

        [[nodiscard]] foundation::ResultValue<FrameAllocation> allocate_upload(uint64_t size, uint64_t alignment = 0);
        [[nodiscard]] foundation::Result write_upload(const FrameAllocation& allocation,
                                               std::span<const std::byte> data);

        [[nodiscard]] foundation::ResultValue<FrameAllocation> allocate_readback(uint64_t size, uint64_t alignment = 0);
        [[nodiscard]] foundation::Result read_readback(const FrameAllocation& allocation, std::span<std::byte> data);

        [[nodiscard]] foundation::ResultValue<rhi::SubmitResult> submit(const rhi::SubmitDesc& desc);
        [[nodiscard]] foundation::ResultValue<rhi::SubmitResult> submit_frame(const rhi::FrameSubmitDesc& desc);

        [[nodiscard]] foundation::Result defer_destroy_buffer(rhi::BufferHandle buffer);
        [[nodiscard]] foundation::Result defer_destroy_texture(rhi::TextureHandle texture);
        [[nodiscard]] foundation::Result defer_destroy_buffer_view(rhi::BufferViewHandle view);
        [[nodiscard]] foundation::Result defer_destroy_texture_view(rhi::TextureViewHandle view);
        [[nodiscard]] foundation::Result defer_destroy_sampler(rhi::SamplerHandle sampler);
        [[nodiscard]] foundation::Result defer_destroy_binding_layout(rhi::BindingLayoutHandle layout);
        [[nodiscard]] foundation::Result defer_destroy_binding_set(rhi::BindingSetHandle set);
        [[nodiscard]] foundation::Result defer_destroy_pipeline(rhi::PipelineHandle pipeline);

    private:
        struct CommandRecord {
            rhi::CommandListHandle handle{};
            rhi::QueueClass queue{rhi::QueueClass::kGraphics};
            bool recording{false};
            bool ended{false};
            bool submitted{false};
        };

        struct UploadChunk {
            rhi::BufferHandle buffer{};
            uint64_t capacity{0};
            uint64_t offset{0};
        };

        struct DeferredResources {
            std::vector<rhi::BufferHandle> buffers{};
            std::vector<rhi::TextureHandle> textures{};
            std::vector<rhi::BufferViewHandle> buffer_views{};
            std::vector<rhi::TextureViewHandle> texture_views{};
            std::vector<rhi::SamplerHandle> samplers{};
            std::vector<rhi::BindingLayoutHandle> binding_layouts{};
            std::vector<rhi::BindingSetHandle> binding_sets{};
            std::vector<rhi::PipelineHandle> pipelines{};
        };

        struct FrameSlot {
            uint64_t generation{0};
            std::array<rhi::GpuSyncPoint, kQueueCount> completions{};
            std::array<std::vector<CommandRecord>, kQueueCount> command_lists{};
            std::vector<UploadChunk> upload_chunks{};
            std::vector<UploadChunk> readback_chunks{};
            DeferredResources deferred{};
        };

        [[nodiscard]] foundation::Result validate_desc() const noexcept;
        [[nodiscard]] foundation::Result prepare_slot(FrameSlot& slot);
        [[nodiscard]] foundation::Result release_deferred(DeferredResources& resources);
        [[nodiscard]] foundation::Result reset_command_lists(FrameSlot& slot);
        [[nodiscard]] CommandRecord* find_command(rhi::CommandListHandle command_list) noexcept;
        [[nodiscard]] const CommandRecord* find_command(rhi::CommandListHandle command_list) const noexcept;
        [[nodiscard]] foundation::Result validate_allocation(const FrameAllocation& allocation) const noexcept;
        [[nodiscard]] foundation::ResultValue<UploadChunk*> acquire_chunk(std::vector<UploadChunk>& chunks,
                                                                     uint64_t size, uint64_t alignment,
                                                                     bool readback);
        template<typename HandleType>
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<HandleType>& handles, HandleType handle);
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<rhi::BufferHandle>& handles,
                                                      rhi::BufferHandle handle);
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<rhi::TextureHandle>& handles,
                                                      rhi::TextureHandle handle);
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<rhi::BufferViewHandle>& handles,
                                                      rhi::BufferViewHandle handle);
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<rhi::TextureViewHandle>& handles,
                                                      rhi::TextureViewHandle handle);
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<rhi::SamplerHandle>& handles,
                                                      rhi::SamplerHandle handle);
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<rhi::BindingLayoutHandle>& handles,
                                                      rhi::BindingLayoutHandle handle);
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<rhi::BindingSetHandle>& handles,
                                                      rhi::BindingSetHandle handle);
        [[nodiscard]] foundation::Result add_deferred_handle(std::vector<rhi::PipelineHandle>& handles,
                                                      rhi::PipelineHandle handle);

        rhi::Device& device_;
        FrameContextDesc desc_{};
        std::vector<FrameSlot> slots_{};
        uint32_t current_slot_{0};
        uint64_t frame_number_{0};
        uint64_t context_cookie_{0};
        bool frame_active_{false};
        bool has_acquired_frame_{false};
        bool acquired_frame_submitted_{false};
        rhi::FrameAcquireResult acquired_frame_{};
    };
} // namespace aeg::render_graph
