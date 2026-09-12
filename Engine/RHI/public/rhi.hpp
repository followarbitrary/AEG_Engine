/**
 * @file rhi.hpp
 * @brief Backend-neutral device interface for resources, commands, queues, and presentation.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "result.hpp"
#include "rhi_struct.hpp"

namespace aeg::rhi {
    /**
     * @brief Common rendering hardware interface implemented by each backend.
     *
     * Device owns all handles returned by its methods. Handles are opaque,
     * backend-independent identifiers and must be destroyed through the same
     * Device that created them. Command-list methods are stateful: callers must
     * follow create/begin/record/end/submit/reset/destroy sequencing documented
     * by the individual operations.
     */
    class Device {
    public:
        /** @brief Virtual destructor for backend implementations. */
        virtual ~Device() = default;

        /** @brief Returns capabilities of the selected adapter. */
        [[nodiscard]] virtual const DeviceCapabilities &capabilities() const noexcept = 0;

        /** @brief Creates a presentation surface from opaque native handles. */
        [[nodiscard]] virtual foundation::ResultValue<SurfaceHandle> create_surface(const SurfaceDesc &desc) = 0;

        /** @brief Destroys a surface that is no longer used by a swapchain. */
        [[nodiscard]] virtual foundation::Result destroy_surface(SurfaceHandle surface) = 0;

        /** @brief Creates a swapchain for a surface. */
        [[nodiscard]] virtual foundation::ResultValue<SwapchainHandle> create_swapchain(const SwapchainDesc &desc) = 0;

        /** @brief Destroys a swapchain after all acquired frames are resolved. */
        [[nodiscard]] virtual foundation::Result destroy_swapchain(SwapchainHandle swapchain) = 0;

        /** @brief Recreates a swapchain in place after resize or surface changes. */
        [[nodiscard]] virtual foundation::Result recreate_swapchain(
            SwapchainHandle swapchain, const SwapchainDesc &desc = {}) = 0;

        /** @brief Reads the effective swapchain configuration and image list. */
        [[nodiscard]] virtual foundation::ResultValue<SwapchainInfo> get_swapchain_info(
            SwapchainHandle swapchain) const = 0;

        /** @brief Acquires an available swapchain image. */
        [[nodiscard]] virtual foundation::ResultValue<FrameAcquireResult> acquire_next_image(
            SwapchainHandle swapchain) = 0;

        /** @brief Submits command lists associated with an acquired frame. */
        [[nodiscard]] virtual foundation::ResultValue<SubmitResult> submit_frame(const FrameSubmitDesc &desc) = 0;

        /** @brief Presents a previously acquired and submitted frame. */
        [[nodiscard]] virtual foundation::ResultValue<PresentResult> present(const PresentDesc &desc) = 0;

        /** @brief Cancels an acquired frame without presenting it. */
        [[nodiscard]] virtual foundation::Result cancel_frame(const FrameToken &frame) = 0;

        /** @brief Creates a buffer with the requested capabilities and memory class. */
        [[nodiscard]] virtual foundation::ResultValue<BufferHandle> create_buffer(const BufferDesc &desc) = 0;

        /** @brief Destroys a buffer after all GPU uses have completed. */
        [[nodiscard]] virtual foundation::Result destroy_buffer(BufferHandle buffer) = 0;

        /** @brief Reads the current backend-neutral state and GPU use of a buffer. */
        [[nodiscard]] virtual foundation::ResultValue<BufferStateSnapshot> query_buffer_state(
                BufferHandle buffer) const = 0;

        /** @brief Writes CPU data into a host-visible buffer range. */
        [[nodiscard]] virtual foundation::Result write_buffer(BufferHandle buffer, uint64_t offset,
                                                  std::span<const std::byte> data) = 0;

        /** @brief Reads a host-visible buffer range into CPU memory. */
        [[nodiscard]] virtual foundation::Result read_buffer(BufferHandle buffer, uint64_t offset,
                                                 std::span<std::byte> data) = 0;

        /** @brief Creates a graphics or compute pipeline. */
        [[nodiscard]] virtual foundation::ResultValue<PipelineHandle> create_pipeline(const PipelineDesc &desc) = 0;

        /** @brief Destroys a pipeline after all command lists using it are complete. */
        [[nodiscard]] virtual foundation::Result destroy_pipeline(PipelineHandle pipeline) = 0;

        /** @brief Creates a texture with the requested format, shape, and capabilities. */
        [[nodiscard]] virtual foundation::ResultValue<TextureHandle> create_texture(const TextureDesc &desc) = 0;

        /** @brief Destroys a texture after all views and GPU uses are released. */
        [[nodiscard]] virtual foundation::Result destroy_texture(TextureHandle texture) = 0;

        /** @brief Reads the current backend-neutral subresource state and GPU use of a texture. */
        [[nodiscard]] virtual foundation::ResultValue<TextureStateSnapshot> query_texture_state(
                TextureHandle texture) const = 0;

        /** @brief Creates a typed or structured view into a buffer. */
        [[nodiscard]] virtual foundation::ResultValue<BufferViewHandle> create_buffer_view(
            const BufferViewDesc &desc) = 0;

        /** @brief Destroys a buffer view. */
        [[nodiscard]] virtual foundation::Result destroy_buffer_view(BufferViewHandle view) = 0;

        /** @brief Creates a sampled, storage, or attachment view into a texture. */
        [[nodiscard]] virtual foundation::ResultValue<TextureViewHandle> create_texture_view(
            const TextureViewDesc &desc) = 0;

        /** @brief Destroys a texture view. */
        [[nodiscard]] virtual foundation::Result destroy_texture_view(TextureViewHandle view) = 0;

        /** @brief Creates an immutable sampler object from a sampling description. */
        [[nodiscard]] virtual foundation::ResultValue<SamplerHandle> create_sampler(const SamplerDesc &desc) = 0;

        /** @brief Destroys a sampler. */
        [[nodiscard]] virtual foundation::Result destroy_sampler(SamplerHandle sampler) = 0;

        /** @brief Creates a binding layout from shader-visible binding declarations. */
        [[nodiscard]] virtual foundation::ResultValue<BindingLayoutHandle> create_binding_layout(
            const BindingLayoutDesc &desc) = 0;

        /** @brief Destroys a binding layout after dependent sets and pipelines are released. */
        [[nodiscard]] virtual foundation::Result destroy_binding_layout(BindingLayoutHandle layout) = 0;

        /** @brief Allocates a binding set compatible with a layout. */
        [[nodiscard]] virtual foundation::ResultValue<BindingSetHandle> create_binding_set(
            const BindingSetDesc &desc) = 0;

        /** @brief Writes resource handles into a binding set. */
        [[nodiscard]] virtual foundation::Result update_binding_set(
            BindingSetHandle set, std::span<const BindingWrite> writes) = 0;

        /** @brief Destroys a binding set after command lists no longer reference it. */
        [[nodiscard]] virtual foundation::Result destroy_binding_set(BindingSetHandle set) = 0;

        /** @brief Allocates a command list for one logical queue. */
        [[nodiscard]] virtual foundation::ResultValue<CommandListHandle> create_command_list(
            const CommandListDesc &desc) = 0;

        /** @brief Begins recording into a reset command list. */
        [[nodiscard]] virtual foundation::Result begin_command_list(CommandListHandle command_list) = 0;

        /** @brief Begins dynamic rendering with the supplied attachments. */
        [[nodiscard]] virtual foundation::Result begin_rendering(
            CommandListHandle command_list, const RenderingDesc &desc) = 0;

        /** @brief Ends dynamic rendering on a recording command list. */
        [[nodiscard]] virtual foundation::Result end_rendering(CommandListHandle command_list) = 0;

        /** @brief Records explicit resource state transitions. */
        [[nodiscard]] virtual foundation::Result barrier(CommandListHandle command_list, const BarrierDesc &desc) = 0;

        /** @brief Sets the dynamic viewport for subsequent draws. */
        [[nodiscard]] virtual foundation::Result set_viewport(
            CommandListHandle command_list, const Viewport &viewport) = 0;

        /** @brief Sets the dynamic scissor rectangle for subsequent draws. */
        [[nodiscard]] virtual foundation::Result set_scissor(
            CommandListHandle command_list, const ScissorRect &scissor) = 0;

        /** @brief Binds a graphics or compute pipeline to a command list. */
        [[nodiscard]] virtual foundation::Result bind_pipeline(
            CommandListHandle command_list, PipelineHandle pipeline) = 0;

        /** @brief Binds one descriptor group and optional dynamic offsets. */
        [[nodiscard]] virtual foundation::Result bind_binding_set(CommandListHandle command_list, uint32_t group,
                                                      BindingSetHandle set,
                                                      std::span<const uint32_t> dynamic_offsets = {}) = 0;

        /** @brief Updates inline constants for selected shader stages. */
        [[nodiscard]] virtual foundation::Result set_inline_constants(CommandListHandle command_list, uint32_t offset,
                                                          ShaderStageMask shader_stages,
                                                          std::span<const std::byte> data) = 0;

        /** @brief Binds one vertex buffer at a pipeline binding slot. */
        [[nodiscard]] virtual foundation::Result bind_vertex_buffer(CommandListHandle command_list, uint32_t binding,
                                                        BufferHandle buffer, uint64_t offset = 0) = 0;

        /** @brief Binds an index buffer for subsequent indexed draws. */
        [[nodiscard]] virtual foundation::Result bind_index_buffer(CommandListHandle command_list, BufferHandle buffer,
                                                       uint64_t offset, IndexFormat format) = 0;

        /** @brief Records a direct non-indexed draw. */
        [[nodiscard]] virtual foundation::Result draw(CommandListHandle command_list, const DrawDesc &desc) = 0;

        /** @brief Records a direct indexed draw. */
        [[nodiscard]] virtual foundation::Result draw_indexed(CommandListHandle command_list,
                                                  const DrawIndexedDesc &desc) = 0;

        /** @brief Records one or more non-indexed draws from GPU arguments. */
        [[nodiscard]] virtual foundation::Result draw_indirect(CommandListHandle command_list,
                                                   const DrawIndirectDesc &desc) = 0;

        /** @brief Records a direct compute dispatch. */
        [[nodiscard]] virtual foundation::Result dispatch(CommandListHandle command_list, const DispatchDesc &desc) = 0;

        /** @brief Records a compute dispatch from GPU arguments. */
        [[nodiscard]] virtual foundation::Result dispatch_indirect(CommandListHandle command_list,
                                                       const DispatchIndirectDesc &desc) = 0;

        /** @brief Records a buffer-to-buffer copy. */
        [[nodiscard]] virtual foundation::Result copy_buffer(
            CommandListHandle command_list, const BufferCopyDesc &desc) = 0;

        /** @brief Records a buffer-to-texture copy. */
        [[nodiscard]] virtual foundation::Result copy_buffer_to_texture(CommandListHandle command_list,
                                                            const BufferTextureCopyDesc &desc) = 0;

        /** @brief Records a texture-to-buffer copy. */
        [[nodiscard]] virtual foundation::Result copy_texture_to_buffer(CommandListHandle command_list,
                                                            const TextureBufferCopyDesc &desc) = 0;

        /** @brief Records a texture-to-texture copy. */
        [[nodiscard]] virtual foundation::Result copy_texture(
            CommandListHandle command_list, const TextureCopyDesc &desc) = 0;

        /** @brief Records a color, depth, or stencil texture clear. */
        [[nodiscard]] virtual foundation::Result clear_texture(CommandListHandle command_list,
                                                   const ClearTextureDesc &desc) = 0;

        /** @brief Ends command recording and makes the list eligible for submission. */
        [[nodiscard]] virtual foundation::Result end_command_list(CommandListHandle command_list) = 0;

        /** @brief Resets a completed or unsubmitted command list for new recording. */
        [[nodiscard]] virtual foundation::Result reset_command_list(CommandListHandle command_list) = 0;

        /** @brief Destroys a command list after its submitted work completes. */
        [[nodiscard]] virtual foundation::Result destroy_command_list(CommandListHandle command_list) = 0;

        /** @brief Submits ended command lists to a logical queue. */
        [[nodiscard]] virtual foundation::ResultValue<SubmitResult> submit(const SubmitDesc &desc) = 0;

        /** @brief Blocks the calling thread until a GPU sync point completes. */
        [[nodiscard]] virtual foundation::Result wait(const GpuSyncPoint &point, uint64_t timeout_nanoseconds) = 0;

        /** @brief Queries completion without blocking. */
        [[nodiscard]] virtual bool is_complete(const GpuSyncPoint &point) const = 0;

        /** @brief Releases resources whose deferred GPU completion points have passed. */
        [[nodiscard]] virtual foundation::Result collect_garbage() = 0;

        /** @brief Blocks until all device queues are idle and collects retired resources. */
        [[nodiscard]] virtual foundation::Result wait_idle() = 0;
    };

    /**
     * @brief Creates the backend selected by DeviceCreateDesc.
     *
     * The runtime backend module provides the implementation behind this common
     * factory. Test-only backends expose private factories and are not dispatched
     * here.
     */
    [[nodiscard]] foundation::ResultValue<std::unique_ptr<Device> > create_device(const DeviceCreateDesc &desc = {});

} // namespace aeg::rhi
