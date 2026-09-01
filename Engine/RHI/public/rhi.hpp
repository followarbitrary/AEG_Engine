#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "rhi_struct.hpp"

namespace aeg::rhi {
class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual const DeviceCapabilities& capabilities() const noexcept = 0;

    [[nodiscard]] virtual ResultValue<SurfaceHandle> create_surface(const SurfaceDesc& desc) = 0;

    [[nodiscard]] virtual Result destroy_surface(SurfaceHandle surface) = 0;

    [[nodiscard]] virtual ResultValue<SwapchainHandle> create_swapchain(const SwapchainDesc& desc) = 0;

    [[nodiscard]] virtual Result destroy_swapchain(SwapchainHandle swapchain) = 0;

    [[nodiscard]] virtual Result recreate_swapchain(SwapchainHandle swapchain, const SwapchainDesc& desc = {}) = 0;

    [[nodiscard]] virtual ResultValue<SwapchainInfo> get_swapchain_info(SwapchainHandle swapchain) const = 0;

    [[nodiscard]] virtual ResultValue<FrameAcquireResult> acquire_next_image(SwapchainHandle swapchain) = 0;

    [[nodiscard]] virtual ResultValue<SubmitResult> submit_frame(const FrameSubmitDesc& desc) = 0;

    [[nodiscard]] virtual ResultValue<PresentResult> present(const PresentDesc& desc) = 0;

    [[nodiscard]] virtual Result cancel_frame(const FrameToken& frame) = 0;

    [[nodiscard]] virtual ResultValue<BufferHandle> create_buffer(const BufferDesc& desc) = 0;

    [[nodiscard]] virtual Result destroy_buffer(BufferHandle buffer) = 0;

    [[nodiscard]] virtual Result write_buffer(BufferHandle buffer, uint64_t offset,
                                              std::span<const std::byte> data) = 0;

    [[nodiscard]] virtual ResultValue<TextureHandle> create_texture(const TextureDesc& desc) = 0;

    [[nodiscard]] virtual Result destroy_texture(TextureHandle texture) = 0;

    [[nodiscard]] virtual ResultValue<BufferViewHandle> create_buffer_view(const BufferViewDesc& desc) = 0;

    [[nodiscard]] virtual Result destroy_buffer_view(BufferViewHandle view) = 0;

    [[nodiscard]] virtual ResultValue<TextureViewHandle> create_texture_view(const TextureViewDesc& desc) = 0;

    [[nodiscard]] virtual Result destroy_texture_view(TextureViewHandle view) = 0;

    [[nodiscard]] virtual ResultValue<SamplerHandle> create_sampler(const SamplerDesc& desc) = 0;

    [[nodiscard]] virtual Result destroy_sampler(SamplerHandle sampler) = 0;

    [[nodiscard]] virtual ResultValue<BindingLayoutHandle> create_binding_layout(const BindingLayoutDesc& desc) = 0;

    [[nodiscard]] virtual Result destroy_binding_layout(BindingLayoutHandle layout) = 0;

    [[nodiscard]] virtual ResultValue<BindingSetHandle> create_binding_set(BindingLayoutHandle layout) = 0;

    [[nodiscard]] virtual Result update_binding_set(BindingSetHandle set, std::span<const BindingWrite> writes) = 0;

    [[nodiscard]] virtual Result destroy_binding_set(BindingSetHandle set) = 0;

    [[nodiscard]] virtual ResultValue<CommandListHandle> create_command_list(const CommandListDesc& desc) = 0;

    [[nodiscard]] virtual Result begin_command_list(CommandListHandle command_list) = 0;

    [[nodiscard]] virtual Result end_command_list(CommandListHandle command_list) = 0;

    [[nodiscard]] virtual Result reset_command_list(CommandListHandle command_list) = 0;

    [[nodiscard]] virtual Result destroy_command_list(CommandListHandle command_list) = 0;

    [[nodiscard]] virtual ResultValue<SubmitResult> submit(const SubmitDesc& desc) = 0;

    [[nodiscard]] virtual Result wait(const GpuSyncPoint& point, uint64_t timeout_nanoseconds) = 0;

    [[nodiscard]] virtual bool is_complete(const GpuSyncPoint& point) const = 0;

    [[nodiscard]] virtual Result wait_idle() = 0;
};

[[nodiscard]] ResultValue<std::unique_ptr<Device>> create_vulkan_device(const DeviceCreateDesc& desc = {});

[[nodiscard]] const char* result_code_name(ResultCode code) noexcept;
}  // namespace aeg::rhi
