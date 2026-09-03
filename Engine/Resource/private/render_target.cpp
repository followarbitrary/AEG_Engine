/**
 * @file render_target.cpp
 * @brief Implements offscreen color target creation and destruction.
 */
#include "../public/render_target.hpp"

namespace aeg::resource {
    namespace {
        [[nodiscard]] foundation::Result destroy_if_valid(
                rhi::Device& device, rhi::TextureViewHandle& handle) {
            if (!handle.is_valid()) {
                return foundation::Result::success();
            }
            const foundation::Result result = device.destroy_texture_view(handle);
            if (result || result.code == foundation::ResultCode::kNotFound) {
                handle = {};
                return foundation::Result::success();
            }
            return result;
        }

        [[nodiscard]] foundation::Result destroy_if_valid(
                rhi::Device& device, rhi::TextureHandle& handle) {
            if (!handle.is_valid()) {
                return foundation::Result::success();
            }
            const foundation::Result result = device.destroy_texture(handle);
            if (result || result.code == foundation::ResultCode::kNotFound) {
                handle = {};
                return foundation::Result::success();
            }
            return result;
        }
    } // namespace

    foundation::ResultValue<OffscreenRenderTarget> create_offscreen_render_target(
            rhi::Device& device, const OffscreenRenderTargetDesc& desc) {
        if (desc.extent.width == 0 || desc.extent.height == 0) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                 "Offscreen render target extent must be non-zero"), {}};
        }
        if (desc.format == rhi::Format::kUnknown) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                 "Offscreen render target format must be specified"), {}};
        }

        rhi::TextureDesc texture_desc{};
        texture_desc.extent = {desc.extent.width, desc.extent.height, 1};
        texture_desc.format = desc.format;
        texture_desc.samples = desc.samples;
        texture_desc.capabilities = rhi::TextureCapability::kColorAttachment;
        if (desc.sampled) {
            texture_desc.capabilities |= rhi::TextureCapability::kSampled;
        }
        texture_desc.memory = rhi::MemoryClass::kDeviceLocal;
        texture_desc.initial_usage = rhi::ResourceUsage::kUndefined;
        texture_desc.initial_stages = rhi::PipelineStage::kAllCommands;
        texture_desc.initial_queue = rhi::QueueClass::kAuto;
        texture_desc.name = desc.name;

        auto texture_result = device.create_texture(texture_desc);
        if (!texture_result) {
            return {texture_result.result, {}};
        }

        OffscreenRenderTarget target{};
        target.texture = texture_result.value;

        const rhi::SubresourceRange subresources{rhi::Aspect::kColor, 0, 1, 0, 1};
        const rhi::TextureViewDesc color_view_desc{
                target.texture,
                rhi::TextureViewKind::kColorAttachment,
                rhi::TextureViewDimension::kTex2D,
                desc.format,
                subresources,
                {},
                desc.name
        };
        auto color_view_result = device.create_texture_view(color_view_desc);
        if (!color_view_result) {
            (void) destroy_if_valid(device, target.texture);
            return {color_view_result.result, {}};
        }
        target.color_view = color_view_result.value;

        if (desc.sampled) {
            const rhi::TextureViewDesc sampled_view_desc{
                    target.texture,
                    rhi::TextureViewKind::kSampled,
                    rhi::TextureViewDimension::kTex2D,
                    desc.format,
                    subresources,
                    {},
                    desc.name
            };
            auto sampled_view_result = device.create_texture_view(sampled_view_desc);
            if (!sampled_view_result) {
                (void) destroy_if_valid(device, target.color_view);
                (void) destroy_if_valid(device, target.texture);
                return {sampled_view_result.result, {}};
            }
            target.sampled_view = sampled_view_result.value;
        }

        target.texture_desc = texture_desc;
        target.texture_desc.name = {};
        return {foundation::Result::success(), target};
    }

    foundation::Result destroy_offscreen_render_target(
            rhi::Device& device, OffscreenRenderTarget& target) {
        foundation::Result first_error = destroy_if_valid(device, target.sampled_view);
        const foundation::Result color_view_result = destroy_if_valid(device, target.color_view);
        if (first_error && !color_view_result) {
            first_error = color_view_result;
        }
        const foundation::Result texture_result = destroy_if_valid(device, target.texture);
        if (first_error && !texture_result) {
            first_error = texture_result;
        }
        if (!target.texture.is_valid() && !target.color_view.is_valid() && !target.sampled_view.is_valid()) {
            target.reset();
        }
        return first_error;
    }
} // namespace aeg::resource
