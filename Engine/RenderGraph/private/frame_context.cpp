/**
 * @file frame_context.cpp
 * @brief Implements frame-slot reuse, staging allocation, and deferred cleanup.
 */
#include "../public/frame_context.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace aeg::render_graph {
    namespace {
        [[nodiscard]] bool checked_add(uint64_t lhs, uint64_t rhs, uint64_t& result) noexcept {
            if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
                return false;
            }
            result = lhs + rhs;
            return true;
        }

        [[nodiscard]] uint64_t align_up(uint64_t value, uint64_t alignment, bool& succeeded) noexcept {
            if (alignment == 0) {
                succeeded = false;
                return 0;
            }

            if (std::has_single_bit(alignment)) {
                const uint64_t mask = alignment - 1;
                if (value > std::numeric_limits<uint64_t>::max() - mask) {
                    succeeded = false;
                    return 0;
                }
                succeeded = true;
                return (value + mask) & ~mask;
            }

            const uint64_t remainder = value % alignment;
            if (remainder == 0) {
                succeeded = true;
                return value;
            }
            const uint64_t padding = alignment - remainder;
            if (value > std::numeric_limits<uint64_t>::max() - padding) {
                succeeded = false;
                return 0;
            }
            succeeded = true;
            return value + padding;
        }

        [[nodiscard]] bool same_frame_token(const rhi::FrameToken& lhs, const rhi::FrameToken& rhs) noexcept {
            return lhs.swapchain == rhs.swapchain && lhs.frame_slot == rhs.frame_slot &&
                   lhs.image_index == rhs.image_index && lhs.generation == rhs.generation;
        }
    } // namespace

    FrameContext::FrameContext(rhi::Device& device, const FrameContextDesc& desc)
        : device_(device), desc_(desc), context_cookie_(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this))) {
    }

    FrameContext::~FrameContext() {
        if (frame_active_) {
            (void) abort_frame();
        }
        (void) device_.wait_idle();

        for (FrameSlot& slot: slots_) {
            for (std::vector<CommandRecord>& commands: slot.command_lists) {
                for (CommandRecord& command: commands) {
                    if (!command.handle.is_valid()) {
                        continue;
                    }
                    (void) device_.reset_command_list(command.handle);
                    (void) device_.destroy_command_list(command.handle);
                    command = {};
                }
            }

            (void) release_deferred(slot.deferred);
            for (const UploadChunk& chunk: slot.upload_chunks) {
                if (chunk.buffer.is_valid()) {
                    (void) device_.destroy_buffer(chunk.buffer);
                }
            }
            for (const UploadChunk& chunk: slot.readback_chunks) {
                if (chunk.buffer.is_valid()) {
                    (void) device_.destroy_buffer(chunk.buffer);
                }
            }
            slot.retained_owners.clear();
        }
        (void) device_.collect_garbage();
    }

    foundation::Result FrameContext::retain_until_complete(std::shared_ptr<const void> owner) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "Cannot retain ownership without an active frame");
        }
        if (owner.use_count() == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Frame retention requires shared ownership");
        }
        try {
            slots_[current_slot_].retained_owners.push_back(std::move(owner));
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                "Frame ownership retention allocation failed");
        }
        return foundation::Result::success();
    }

    foundation::Result FrameContext::validate_desc() const noexcept {
        if (desc_.frames_in_flight == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "FrameContext frames_in_flight must be greater than zero");
        }
        if (desc_.upload_chunk_size == 0 || desc_.readback_chunk_size == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "FrameContext upload and readback chunk sizes must be non-zero");
        }
        if (desc_.upload_alignment == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "FrameContext upload alignment must be non-zero");
        }
        return foundation::Result::success();
    }

    foundation::Result FrameContext::begin_frame() {
        if (frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "A FrameContext frame is already active");
        }

        const foundation::Result desc_result = validate_desc();
        if (!desc_result) {
            return desc_result;
        }

        try {
            if (slots_.empty()) {
                slots_.resize(desc_.frames_in_flight);
            }
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "FrameContext frame-slot allocation failed");
        }

        current_slot_ = static_cast<uint32_t>(frame_number_ % desc_.frames_in_flight);
        FrameSlot& slot = slots_[current_slot_];
        const foundation::Result prepare_result = prepare_slot(slot);
        if (!prepare_result) {
            return prepare_result;
        }

        slot.generation = frame_number_;
        slot.completions.clear();
        frame_active_ = true;
        has_acquired_frame_ = false;
        acquired_frame_submitted_ = false;
        acquired_frame_ = {};
        ++frame_number_;
        return foundation::Result::success();
    }

    foundation::Result FrameContext::end_frame() {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "No FrameContext frame is active");
        }
        if (has_acquired_frame_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        acquired_frame_submitted_
                                                ? "Acquired frame must be presented before ending the frame"
                                                : "Acquired frame must be submitted, presented, or cancelled");
        }

        for (const std::vector<CommandRecord>& commands: slots_[current_slot_].command_lists) {
            for (const CommandRecord& command: commands) {
                if (command.recording) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "A command list is still recording at frame end");
                }
                if (command.ended && !command.submitted) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "An ended command list was not submitted before frame end");
                }
            }
        }

        frame_active_ = false;
        return foundation::Result::success();
    }

    foundation::Result FrameContext::abort_frame() {
        if (!frame_active_) {
            return foundation::Result::success();
        }

        FrameSlot& slot = slots_[current_slot_];

        for (const rhi::GpuSyncPoint& completion: slot.completions.points()) {
            if (!completion.is_valid()) {
                continue;
            }
            const foundation::Result wait_result = device_.wait(completion, desc_.wait_timeout_nanoseconds);
            if (!wait_result) {
                return wait_result;
            }
        }

        if (has_acquired_frame_ && acquired_frame_submitted_) {
            const foundation::Result present_result = device_.present(rhi::PresentDesc{acquired_frame_.frame}).result;
            if (!present_result && present_result.code != foundation::ResultCode::kOutOfDate &&
                present_result.code != foundation::ResultCode::kSurfaceLost) {
                return present_result;
            }
            has_acquired_frame_ = false;
            acquired_frame_submitted_ = false;
            acquired_frame_ = {};
        } else if (has_acquired_frame_) {
            const foundation::Result cancel_result = device_.cancel_frame(acquired_frame_.frame);
            if (!cancel_result) {
                return cancel_result;
            }
            has_acquired_frame_ = false;
            acquired_frame_submitted_ = false;
            acquired_frame_ = {};
        }

        for (std::vector<CommandRecord>& commands: slot.command_lists) {
            for (CommandRecord& command: commands) {
                if (!command.handle.is_valid()) {
                    continue;
                }
                if (command.recording) {
                    (void) device_.end_rendering(command.handle);
                    const foundation::Result end_result = device_.end_command_list(command.handle);
                    if (!end_result) {
                        return end_result;
                    }
                    command.recording = false;
                    command.ended = true;
                }
                if (command.ended && !command.submitted) {
                    const foundation::Result reset_result = device_.reset_command_list(command.handle);
                    if (!reset_result) {
                        return reset_result;
                    }
                    command.recording = false;
                    command.ended = false;
                }
            }
        }

        if (const foundation::Result result = reset_command_lists(slot); !result) {
            return result;
        }
        if (const foundation::Result result = release_deferred(slot.deferred); !result) {
            return result;
        }
        slot.retained_owners.clear();
        slots_[current_slot_].completions.clear();
        for (UploadChunk& chunk: slots_[current_slot_].upload_chunks) {
            chunk.offset = 0;
        }
        for (UploadChunk& chunk: slots_[current_slot_].readback_chunks) {
            chunk.offset = 0;
        }
        frame_active_ = false;
        return foundation::Result::success();
    }

    foundation::Result FrameContext::prepare_slot(FrameSlot& slot) {
        for (const rhi::GpuSyncPoint& completion: slot.completions.points()) {
            if (!completion.is_valid()) {
                continue;
            }
            const foundation::Result wait_result = device_.wait(completion, desc_.wait_timeout_nanoseconds);
            if (!wait_result) {
                return wait_result;
            }
        }

        const foundation::Result reset_result = reset_command_lists(slot);
        if (!reset_result) {
            return reset_result;
        }
        const foundation::Result release_result = release_deferred(slot.deferred);
        if (!release_result) {
            return release_result;
        }
        slot.retained_owners.clear();

        for (UploadChunk& chunk: slot.upload_chunks) {
            chunk.offset = 0;
        }
        for (UploadChunk& chunk: slot.readback_chunks) {
            chunk.offset = 0;
        }
        return foundation::Result::success();
    }

    foundation::Result FrameContext::reset_command_lists(FrameSlot& slot) {
        for (std::vector<CommandRecord>& commands: slot.command_lists) {
            for (CommandRecord& command: commands) {
                if (!command.handle.is_valid()) {
                    command = {};
                    continue;
                }
                if (command.recording) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "Cannot reset a recording FrameContext command list");
                }
                const foundation::Result reset_result = device_.reset_command_list(command.handle);
                if (!reset_result) {
                    return reset_result;
                }
                command.recording = false;
                command.ended = false;
                command.submitted = false;
            }
        }
        return foundation::Result::success();
    }

    foundation::ResultValue<rhi::FrameAcquireResult> FrameContext::acquire_next_image(rhi::SwapchainHandle swapchain) {
        if (!frame_active_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Cannot acquire a swapchain image without an active frame"), {}};
        }
        if (has_acquired_frame_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "The current FrameContext already owns an acquired frame"), {}};
        }

        auto result = device_.acquire_next_image(swapchain);
        if (!result) {
            return result;
        }
        acquired_frame_ = result.value;
        has_acquired_frame_ = true;
        acquired_frame_submitted_ = false;
        return result;
    }

    const rhi::FrameAcquireResult* FrameContext::acquired_frame() const noexcept {
        return has_acquired_frame_ ? &acquired_frame_ : nullptr;
    }

    foundation::Result FrameContext::cancel_frame() {
        if (!frame_active_ || !has_acquired_frame_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "No acquired swapchain frame is available to cancel");
        }
        if (acquired_frame_submitted_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "A submitted swapchain frame cannot be cancelled");
        }

        const foundation::Result result = device_.cancel_frame(acquired_frame_.frame);
        if (result) {
            has_acquired_frame_ = false;
            acquired_frame_submitted_ = false;
            acquired_frame_ = {};
        }
        return result;
    }

    foundation::ResultValue<rhi::PresentResult> FrameContext::present() {
        if (!frame_active_ || !has_acquired_frame_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "No acquired swapchain frame is available to present"), {}};
        }
        if (!acquired_frame_submitted_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "The acquired swapchain frame must be submitted before presentation"), {}};
        }

        auto result = device_.present(rhi::PresentDesc{acquired_frame_.frame});
        if (result || result.result.code == foundation::ResultCode::kOutOfDate ||
            result.result.code == foundation::ResultCode::kSurfaceLost) {
            has_acquired_frame_ = false;
            acquired_frame_submitted_ = false;
            acquired_frame_ = {};
        }
        return result;
    }

    foundation::ResultValue<rhi::CommandListHandle> FrameContext::begin_command_list(
            rhi::QueueClass queue, uint32_t batch_index, rhi::BarrierMode barrier_mode) {
        if (!frame_active_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Cannot begin a command list without an active frame"), {}};
        }

        queue = normalize_queue(queue);
        if (queue == rhi::QueueClass::kPresent) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Present is not a command recording queue"), {}};
        }
        const uint32_t index = queue_index(queue);
        std::vector<CommandRecord>& commands = slots_[current_slot_].command_lists[index];
        try {
            if (batch_index >= commands.size()) {
                commands.resize(static_cast<size_t>(batch_index) + 1);
            }
        } catch (const std::bad_alloc&) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "FrameContext command-list cache allocation failed"), {}};
        }

        CommandRecord& command = commands[batch_index];
        if (!command.handle.is_valid()) {
            const rhi::CommandListDesc desc{queue, true, {}, barrier_mode};
            auto create_result = device_.create_command_list(desc);
            if (!create_result) {
                return create_result;
            }
            command.handle = create_result.value;
            command.queue = queue;
            command.barrier_mode = barrier_mode;
        } else if (command.queue != queue) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "FrameContext command-list cache queue mismatch"), {}};
        } else if (command.barrier_mode != barrier_mode) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "FrameContext command-list cache barrier mode mismatch"), {}};
        }

        if (command.recording || command.ended || command.submitted) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "FrameContext command list is already active for this frame"), {}};
        }
        const foundation::Result begin_result = device_.begin_command_list(command.handle);
        if (!begin_result) {
            return {begin_result, {}};
        }
        command.recording = true;
        return {foundation::Result::success(), command.handle};
    }

    foundation::Result FrameContext::end_command_list(rhi::CommandListHandle command_list) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Cannot end a command list without an active frame");
        }
        CommandRecord* command = find_command(command_list);
        if (command == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Command list does not belong to the active FrameContext");
        }
        if (!command->recording) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "FrameContext command list is not recording");
        }
        const foundation::Result result = device_.end_command_list(command_list);
        if (result) {
            command->recording = false;
            command->ended = true;
        }
        return result;
    }

    foundation::ResultValue<FrameContext::UploadChunk*> FrameContext::acquire_chunk(std::vector<UploadChunk>& chunks,
                                                                              uint64_t size, uint64_t alignment,
                                                                              bool readback) {
        const uint64_t effective_alignment = alignment == 0 ? desc_.upload_alignment : alignment;
        if (size == 0 || effective_alignment == 0) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Frame allocation size and alignment must be non-zero"), nullptr};
        }

        if (!chunks.empty()) {
            UploadChunk& current = chunks.back();
            bool aligned = false;
            const uint64_t aligned_offset = align_up(current.offset, effective_alignment, aligned);
            uint64_t end = 0;
            if (aligned && checked_add(aligned_offset, size, end) && end <= current.capacity) {
                return {foundation::Result::success(), &current};
            }
        }

        const uint64_t base_capacity = readback ? desc_.readback_chunk_size : desc_.upload_chunk_size;
        uint64_t required_capacity = 0;
        if (!checked_add(size, effective_alignment - 1, required_capacity)) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Frame allocation size overflows"), nullptr};
        }

        uint64_t capacity = std::max(base_capacity, required_capacity);
        if (!chunks.empty()) {
            uint64_t doubled = 0;
            if (checked_add(chunks.back().capacity, chunks.back().capacity, doubled)) {
                capacity = std::max(capacity, doubled);
            }
        }

        rhi::BufferDesc buffer_desc{};
        buffer_desc.size = capacity;
        buffer_desc.capabilities = rhi::BufferCapability::kTransferSource |
                                   rhi::BufferCapability::kTransferDestination;
        buffer_desc.memory = readback ? rhi::MemoryClass::kReadback : rhi::MemoryClass::kUpload;
        buffer_desc.initial_usage = readback ? rhi::ResourceUsage::kHostRead : rhi::ResourceUsage::kHostWrite;
        buffer_desc.initial_stages = rhi::PipelineStage::kHost;
        buffer_desc.initial_queue = rhi::QueueClass::kCopy;
        buffer_desc.name = rhi::DebugName{readback ? "FrameContext.Readback" : "FrameContext.Upload"};

        auto buffer_result = device_.create_buffer(buffer_desc);
        if (!buffer_result) {
            return {buffer_result.result, nullptr};
        }

        try {
            chunks.push_back(UploadChunk{buffer_result.value, capacity, 0});
        } catch (const std::bad_alloc&) {
            (void) device_.destroy_buffer(buffer_result.value);
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Frame allocation chunk bookkeeping failed"), nullptr};
        }
        return {foundation::Result::success(), &chunks.back()};
    }

    foundation::ResultValue<FrameAllocation> FrameContext::allocate_upload(uint64_t size, uint64_t alignment) {
        if (!frame_active_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Cannot allocate upload memory without an active frame"), {}};
        }
        auto chunk_result = acquire_chunk(slots_[current_slot_].upload_chunks, size, alignment, false);
        if (!chunk_result) {
            return {chunk_result.result, {}};
        }

        UploadChunk& chunk = *chunk_result.value;
        const uint64_t effective_alignment = alignment == 0 ? desc_.upload_alignment : alignment;
        bool aligned = false;
        const uint64_t offset = align_up(chunk.offset, effective_alignment, aligned);
        uint64_t end = 0;
        if (!aligned || !checked_add(offset, size, end) || end > chunk.capacity) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Upload allocation does not fit its chunk"), {}};
        }
        chunk.offset = end;
        return {foundation::Result::success(),
                FrameAllocation{chunk.buffer, offset, size, current_slot_, frame_number_ - 1, context_cookie_,
                                chunk.capacity}};
    }

    foundation::Result FrameContext::validate_allocation(const FrameAllocation& allocation) const noexcept {
        if (!allocation.is_valid() || allocation.context_cookie != context_cookie_ ||
            allocation.frame_slot >= slots_.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Frame allocation does not belong to this FrameContext");
        }
        if (slots_[allocation.frame_slot].generation != allocation.frame_number) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Frame allocation belongs to a recycled frame slot");
        }
        return foundation::Result::success();
    }

    foundation::Result FrameContext::write_upload(const FrameAllocation& allocation, std::span<const std::byte> data) {
        return write_upload(allocation, 0, data);
    }

    foundation::Result FrameContext::write_upload(const FrameAllocation& allocation, uint64_t relative_offset,
                                                  std::span<const std::byte> data) {
        const foundation::Result allocation_result = validate_allocation(allocation);
        if (!allocation_result) {
            return allocation_result;
        }
        if (!frame_active_ || allocation.frame_slot != current_slot_ || allocation.frame_number != frame_number_ - 1) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Upload allocation is not from the active frame");
        }
        if (relative_offset > allocation.size || data.size() > allocation.size - relative_offset) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Upload data exceeds the allocation size");
        }
        if (relative_offset > std::numeric_limits<uint64_t>::max() - allocation.offset) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Upload allocation offset overflows");
        }
        return device_.write_buffer(allocation.buffer, allocation.offset + relative_offset, data);
    }

    foundation::ResultValue<FrameAllocation> FrameContext::allocate_readback(uint64_t size, uint64_t alignment) {
        if (!frame_active_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Cannot allocate readback memory without an active frame"), {}};
        }
        auto chunk_result = acquire_chunk(slots_[current_slot_].readback_chunks, size, alignment, true);
        if (!chunk_result) {
            return {chunk_result.result, {}};
        }

        UploadChunk& chunk = *chunk_result.value;
        const uint64_t effective_alignment = alignment == 0 ? desc_.upload_alignment : alignment;
        bool aligned = false;
        const uint64_t offset = align_up(chunk.offset, effective_alignment, aligned);
        uint64_t end = 0;
        if (!aligned || !checked_add(offset, size, end) || end > chunk.capacity) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Readback allocation does not fit its chunk"), {}};
        }
        chunk.offset = end;
        return {foundation::Result::success(),
                FrameAllocation{chunk.buffer, offset, size, current_slot_, frame_number_ - 1, context_cookie_,
                                chunk.capacity}};
    }

    foundation::Result FrameContext::read_readback(const FrameAllocation& allocation, std::span<std::byte> data) {
        const foundation::Result allocation_result = validate_allocation(allocation);
        if (!allocation_result) {
            return allocation_result;
        }
        if (data.size() > allocation.size) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Readback data exceeds the allocation size");
        }
        return device_.read_buffer(allocation.buffer, allocation.offset, data);
    }

    foundation::ResultValue<rhi::SubmitResult> FrameContext::submit(const rhi::SubmitDesc& desc) {
        if (!frame_active_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Cannot submit without an active frame"), {}};
        }
        rhi::SubmitDesc normalized_desc = desc;
        normalized_desc.queue = normalize_queue(desc.queue);
        auto result = device_.submit(normalized_desc);
        if (!result) {
            return result;
        }

        FrameSlot& slot = slots_[current_slot_];
        (void) slot.completions.add(result.value.completion);
        for (const rhi::CommandListHandle command_list: normalized_desc.command_lists) {
            if (CommandRecord* command = find_command(command_list); command != nullptr) {
                command->submitted = true;
                command->ended = true;
                command->recording = false;
            }
        }
        return result;
    }

    foundation::ResultValue<rhi::SubmitResult> FrameContext::submit_frame(const rhi::FrameSubmitDesc& desc) {
        if (!frame_active_ || !has_acquired_frame_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Cannot submit a swapchain frame without an acquired image"), {}};
        }
        if (!same_frame_token(desc.frame, acquired_frame_.frame)) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Frame submit token does not match the acquired image"), {}};
        }
        if (acquired_frame_submitted_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "The acquired swapchain frame has already been submitted"), {}};
        }

        rhi::FrameSubmitDesc normalized_desc = desc;
        normalized_desc.queue = normalize_queue(desc.queue);
        auto result = device_.submit_frame(normalized_desc);
        if (!result) {
            return result;
        }

        (void) slots_[current_slot_].completions.add(result.value.completion);
        acquired_frame_submitted_ = true;
        for (const rhi::CommandListHandle command_list: normalized_desc.command_lists) {
            if (CommandRecord* command = find_command(command_list); command != nullptr) {
                command->submitted = true;
                command->ended = true;
                command->recording = false;
            }
        }
        return result;
    }

    FrameContext::CommandRecord* FrameContext::find_command(rhi::CommandListHandle command_list) noexcept {
        for (std::vector<CommandRecord>& commands: slots_[current_slot_].command_lists) {
            for (CommandRecord& command: commands) {
                if (command.handle == command_list) {
                    return &command;
                }
            }
        }
        return nullptr;
    }

    const FrameContext::CommandRecord* FrameContext::find_command(
            rhi::CommandListHandle command_list) const noexcept {
        for (const std::vector<CommandRecord>& commands: slots_[current_slot_].command_lists) {
            for (const CommandRecord& command: commands) {
                if (command.handle == command_list) {
                    return &command;
                }
            }
        }
        return nullptr;
    }

    template<typename HandleType>
    foundation::Result FrameContext::add_deferred_handle(std::vector<HandleType>& handles, HandleType handle) {
        if (!handle.is_valid()) {
            return foundation::Result::success();
        }
        if (std::find(handles.begin(), handles.end(), handle) != handles.end()) {
            return foundation::Result::success();
        }
        try {
            handles.push_back(handle);
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "FrameContext deferred-resource bookkeeping failed");
        }
        return foundation::Result::success();
    }

    foundation::Result FrameContext::add_deferred_handle(std::vector<rhi::BufferHandle>& handles,
                                                  rhi::BufferHandle handle) {
        return add_deferred_handle<rhi::BufferHandle>(handles, handle);
    }

    foundation::Result FrameContext::add_deferred_handle(std::vector<rhi::TextureHandle>& handles,
                                                  rhi::TextureHandle handle) {
        return add_deferred_handle<rhi::TextureHandle>(handles, handle);
    }

    foundation::Result FrameContext::add_deferred_handle(std::vector<rhi::BufferViewHandle>& handles,
                                                  rhi::BufferViewHandle handle) {
        return add_deferred_handle<rhi::BufferViewHandle>(handles, handle);
    }

    foundation::Result FrameContext::add_deferred_handle(std::vector<rhi::TextureViewHandle>& handles,
                                                  rhi::TextureViewHandle handle) {
        return add_deferred_handle<rhi::TextureViewHandle>(handles, handle);
    }

    foundation::Result FrameContext::add_deferred_handle(std::vector<rhi::SamplerHandle>& handles,
                                                  rhi::SamplerHandle handle) {
        return add_deferred_handle<rhi::SamplerHandle>(handles, handle);
    }

    foundation::Result FrameContext::add_deferred_handle(std::vector<rhi::BindingLayoutHandle>& handles,
                                                  rhi::BindingLayoutHandle handle) {
        return add_deferred_handle<rhi::BindingLayoutHandle>(handles, handle);
    }

    foundation::Result FrameContext::add_deferred_handle(std::vector<rhi::BindingSetHandle>& handles,
                                                  rhi::BindingSetHandle handle) {
        return add_deferred_handle<rhi::BindingSetHandle>(handles, handle);
    }

    foundation::Result FrameContext::add_deferred_handle(std::vector<rhi::PipelineHandle>& handles,
                                                  rhi::PipelineHandle handle) {
        return add_deferred_handle<rhi::PipelineHandle>(handles, handle);
    }

    foundation::Result FrameContext::defer_destroy_buffer(rhi::BufferHandle buffer) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Deferred resources require an active frame");
        }
        return add_deferred_handle(slots_[current_slot_].deferred.buffers, buffer);
    }

    foundation::Result FrameContext::defer_destroy_texture(rhi::TextureHandle texture) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Deferred resources require an active frame");
        }
        return add_deferred_handle(slots_[current_slot_].deferred.textures, texture);
    }

    foundation::Result FrameContext::defer_destroy_buffer_view(rhi::BufferViewHandle view) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Deferred resources require an active frame");
        }
        return add_deferred_handle(slots_[current_slot_].deferred.buffer_views, view);
    }

    foundation::Result FrameContext::defer_destroy_texture_view(rhi::TextureViewHandle view) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Deferred resources require an active frame");
        }
        return add_deferred_handle(slots_[current_slot_].deferred.texture_views, view);
    }

    foundation::Result FrameContext::defer_destroy_sampler(rhi::SamplerHandle sampler) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Deferred resources require an active frame");
        }
        return add_deferred_handle(slots_[current_slot_].deferred.samplers, sampler);
    }

    foundation::Result FrameContext::defer_destroy_binding_layout(rhi::BindingLayoutHandle layout) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Deferred resources require an active frame");
        }
        return add_deferred_handle(slots_[current_slot_].deferred.binding_layouts, layout);
    }

    foundation::Result FrameContext::defer_destroy_binding_set(rhi::BindingSetHandle set) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Deferred resources require an active frame");
        }
        return add_deferred_handle(slots_[current_slot_].deferred.binding_sets, set);
    }

    foundation::Result FrameContext::defer_destroy_pipeline(rhi::PipelineHandle pipeline) {
        if (!frame_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Deferred resources require an active frame");
        }
        return add_deferred_handle(slots_[current_slot_].deferred.pipelines, pipeline);
    }

    foundation::Result FrameContext::release_deferred(DeferredResources& resources) {
        foundation::Result first_error = foundation::Result::success();
        const auto destroy_all = [&first_error](auto& handles, auto&& destroy_fn) {
            for (const auto handle: handles) {
                const foundation::Result result = destroy_fn(handle);
                if (!result && result.code != foundation::ResultCode::kNotFound && first_error) {
                    first_error = result;
                }
            }
            handles.clear();
        };

        destroy_all(resources.binding_sets, [this](rhi::BindingSetHandle handle) {
            return device_.destroy_binding_set(handle);
        });
        destroy_all(resources.buffer_views, [this](rhi::BufferViewHandle handle) {
            return device_.destroy_buffer_view(handle);
        });
        destroy_all(resources.texture_views, [this](rhi::TextureViewHandle handle) {
            return device_.destroy_texture_view(handle);
        });
        destroy_all(resources.samplers, [this](rhi::SamplerHandle handle) {
            return device_.destroy_sampler(handle);
        });
        destroy_all(resources.pipelines, [this](rhi::PipelineHandle handle) {
            return device_.destroy_pipeline(handle);
        });
        destroy_all(resources.binding_layouts, [this](rhi::BindingLayoutHandle handle) {
            return device_.destroy_binding_layout(handle);
        });
        destroy_all(resources.buffers, [this](rhi::BufferHandle handle) {
            return device_.destroy_buffer(handle);
        });
        destroy_all(resources.textures, [this](rhi::TextureHandle handle) {
            return device_.destroy_texture(handle);
        });
        return first_error;
    }
} // namespace aeg::render_graph
