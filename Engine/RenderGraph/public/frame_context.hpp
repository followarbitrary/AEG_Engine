/**
 * @file frame_context.hpp
 * @brief Per-frame command, staging-allocation, submission, and retirement state.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "result.hpp"
#include "rhi.hpp"

namespace aeg::render_graph {
/** @brief Number of queue classes tracked by the frame scheduler. */
inline constexpr uint32_t kQueueCount = 3;

/** @brief Maps a public queue class to its frame-local queue slot. */
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

/** @brief Resolves QueueClass::kAuto to the graphics queue. */
[[nodiscard]] constexpr rhi::QueueClass normalize_queue(rhi::QueueClass queue) noexcept {
    return queue == rhi::QueueClass::kAuto ? rhi::QueueClass::kGraphics : queue;
}

/** @brief Configuration for frame-slot reuse and transient staging buffers. */
struct FrameContextDesc {
    uint32_t frames_in_flight{2};
    uint64_t upload_chunk_size{4u * 1024u * 1024u};
    uint64_t readback_chunk_size{4u * 1024u * 1024u};
    uint64_t upload_alignment{256};
    uint64_t wait_timeout_nanoseconds{std::numeric_limits<uint64_t>::max()};
};

/**
 * @brief A frame-local suballocation from an upload or readback buffer.
 *
 * The allocation is valid only while its owning frame slot remains in use.
 * It must not be retained across frame-slot reuse.
 */
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

    [[nodiscard]] rhi::BufferRange as_range() const noexcept { return rhi::BufferRange{buffer, offset, size}; }
};

/**
 * @brief Owns reusable frame slots and coordinates per-frame GPU work.
 *
 * A FrameContext borrows its RHI device. Exactly one frame may be active at
 * a time. Staging allocations, command lists, and deferred destructions are
 * associated with the active slot and become reusable after its completions.
 */
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

    /** @brief Waits for the next slot and begins a new frame. */
    [[nodiscard]] foundation::Result begin_frame();
    /** @brief Ends the active frame after all submitted work is recorded. */
    [[nodiscard]] foundation::Result end_frame();
    /** @brief Aborts the active frame and releases its unsubmitted work. */
    [[nodiscard]] foundation::Result abort_frame();
    /**
     * Waits for all submitted slots and retires command references, deferred resources and ownership leases.
     * Requires no active frame. Frame numbers and allocation contents remain valid until normal slot reuse.
     */
    [[nodiscard]] foundation::Result drain();

    /** @brief Retains shared ownership until the active slot is safely recycled. */
    [[nodiscard]] foundation::Result retain_until_complete(std::shared_ptr<const void> owner);

    /** @brief Acquires the next swapchain image for the active frame. */
    [[nodiscard]] foundation::ResultValue<rhi::FrameAcquireResult> acquire_next_image(rhi::SwapchainHandle swapchain);
    /** @brief Returns the image acquired for the active frame, if any. */
    [[nodiscard]] const rhi::FrameAcquireResult* acquired_frame() const noexcept;
    /** @brief Cancels an acquired image without presenting it. */
    [[nodiscard]] foundation::Result cancel_frame();
    /** @brief Presents the image acquired for the active frame. */
    [[nodiscard]] foundation::ResultValue<rhi::PresentResult> present();

    /** @brief Creates and begins a command list in the active frame slot. */
    [[nodiscard]] foundation::ResultValue<rhi::CommandListHandle> begin_command_list(
        rhi::QueueClass queue, uint32_t batch_index = 0, rhi::BarrierMode barrier_mode = rhi::BarrierMode::kImplicit);
    /** @brief Ends a command list created by begin_command_list(). */
    [[nodiscard]] foundation::Result end_command_list(rhi::CommandListHandle command_list);

    /** @brief Allocates frame-local upload memory. */
    [[nodiscard]] foundation::ResultValue<FrameAllocation> allocate_upload(uint64_t size, uint64_t alignment = 0);
    /** @brief Copies bytes into an upload allocation. */
    [[nodiscard]] foundation::Result write_upload(const FrameAllocation& allocation, std::span<const std::byte> data);
    /** @brief Copies bytes into an upload allocation at a relative offset. */
    [[nodiscard]] foundation::Result write_upload(const FrameAllocation& allocation, uint64_t relative_offset,
                                                  std::span<const std::byte> data);

    /** @brief Allocates frame-local readback memory. */
    [[nodiscard]] foundation::ResultValue<FrameAllocation> allocate_readback(uint64_t size, uint64_t alignment = 0);
    /** @brief Reads bytes from a readback allocation after its GPU work completes. */
    [[nodiscard]] foundation::Result read_readback(const FrameAllocation& allocation, std::span<std::byte> data);

    /** @brief Submits ordinary command lists and records their completion point. */
    [[nodiscard]] foundation::ResultValue<rhi::SubmitResult> submit(const rhi::SubmitDesc& desc);
    /** @brief Submits command lists associated with an acquired swapchain frame. */
    [[nodiscard]] foundation::ResultValue<rhi::SubmitResult> submit_frame(const rhi::FrameSubmitDesc& desc);

    /** @brief Defers buffer destruction until the active frame slot is reusable. */
    [[nodiscard]] foundation::Result defer_destroy_buffer(rhi::BufferHandle buffer);
    /** @brief Defers texture destruction until the active frame slot is reusable. */
    [[nodiscard]] foundation::Result defer_destroy_texture(rhi::TextureHandle texture);
    /** @brief Defers buffer-view destruction until the active frame slot is reusable. */
    [[nodiscard]] foundation::Result defer_destroy_buffer_view(rhi::BufferViewHandle view);
    /** @brief Defers texture-view destruction until the active frame slot is reusable. */
    [[nodiscard]] foundation::Result defer_destroy_texture_view(rhi::TextureViewHandle view);
    /** @brief Defers sampler destruction until the active frame slot is reusable. */
    [[nodiscard]] foundation::Result defer_destroy_sampler(rhi::SamplerHandle sampler);
    /** @brief Defers binding-layout destruction until the active frame slot is reusable. */
    [[nodiscard]] foundation::Result defer_destroy_binding_layout(rhi::BindingLayoutHandle layout);
    /** @brief Defers binding-set destruction until the active frame slot is reusable. */
    [[nodiscard]] foundation::Result defer_destroy_binding_set(rhi::BindingSetHandle set);
    /** @brief Defers pipeline destruction until the active frame slot is reusable. */
    [[nodiscard]] foundation::Result defer_destroy_pipeline(rhi::PipelineHandle pipeline);

private:
    struct CommandRecord {
        rhi::CommandListHandle handle{};
        rhi::QueueClass queue{rhi::QueueClass::kGraphics};
        rhi::BarrierMode barrier_mode{rhi::BarrierMode::kImplicit};
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
        rhi::GpuCompletionSet completions{};
        std::array<std::vector<CommandRecord>, kQueueCount> command_lists{};
        std::vector<UploadChunk> upload_chunks{};
        std::vector<UploadChunk> readback_chunks{};
        DeferredResources deferred{};
        std::vector<std::shared_ptr<const void>> retained_owners{};
    };

    [[nodiscard]] foundation::Result validate_desc() const noexcept;
    [[nodiscard]] foundation::Result prepare_slot(FrameSlot& slot);
    [[nodiscard]] foundation::Result release_deferred(DeferredResources& resources);
    [[nodiscard]] foundation::Result reset_command_lists(FrameSlot& slot);
    [[nodiscard]] CommandRecord* find_command(rhi::CommandListHandle command_list) noexcept;
    [[nodiscard]] const CommandRecord* find_command(rhi::CommandListHandle command_list) const noexcept;
    [[nodiscard]] foundation::Result validate_allocation(const FrameAllocation& allocation) const noexcept;
    [[nodiscard]] foundation::ResultValue<UploadChunk*> acquire_chunk(std::vector<UploadChunk>& chunks, uint64_t size,
                                                                      uint64_t alignment, bool readback);
    template <typename HandleType>
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
    bool acquired_frame_acquire_waited_{false};
    bool acquired_frame_present_signaled_{false};
    rhi::FrameAcquireResult acquired_frame_{};
};
}  // namespace aeg::render_graph
