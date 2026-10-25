/**
 * @file renderer_attachment_contract.hpp
 * @brief Stable backend-neutral graphics attachment contract.
 *
 * The contract is shared by material variants and Renderer profile PSOs.
 * Backend pipeline objects and reflection remain outside the public seam.
 */
#pragma once

#include <array>

#include "rhi_struct.hpp"

namespace aeg::renderer {

/** @brief Attachment ABI published by a host-created graphics PSO. */
struct RenderMaterialAttachmentContract final {
    rhi::Format color_format{rhi::Format::kUnknown};
    rhi::SampleCount samples{rhi::SampleCount::kX1};
    rhi::Format depth_stencil_format{rhi::Format::kUnknown};
    /** Ordered color attachments 1 and 2; Unknown means unused in the fixed 0/1/3 ABI. */
    std::array<rhi::Format, 2> additional_color_formats{};

    /** @brief A depth-only PSO has no primary or additional color attachment. */
    [[nodiscard]] bool is_depth_only() const noexcept {
        if (color_format != rhi::Format::kUnknown ||
            additional_color_formats[0] != rhi::Format::kUnknown ||
            additional_color_formats[1] != rhi::Format::kUnknown)
            return false;
        return is_depth_format(depth_stencil_format);
    }

    /** @brief Existing material/color PSOs publish a primary color attachment. */
    [[nodiscard]] bool is_color_attachment() const noexcept {
        return color_format != rhi::Format::kUnknown;
    }

    /** @brief One legal color attachment, with optional depth and no additional MRT slots. */
    [[nodiscard]] bool is_single_color_attachment() const noexcept {
        return is_valid() && is_render_color_format(color_format) &&
               additional_color_formats[0] == rhi::Format::kUnknown &&
               additional_color_formats[1] == rhi::Format::kUnknown &&
               (depth_stencil_format == rhi::Format::kUnknown || is_depth_format(depth_stencil_format));
    }

    /** @brief Complete ordered three-color/depth ABI, without canonical profile format restrictions. */
    [[nodiscard]] bool is_gbuffer_attachment() const noexcept {
        return is_render_color_format(color_format) &&
               is_render_color_format(additional_color_formats[0]) &&
               is_render_color_format(additional_color_formats[1]) && is_depth_format(depth_stencil_format);
    }

    [[nodiscard]] bool is_valid() const noexcept {
        if (!is_color_attachment() && !is_depth_only())
            return false;
        switch (samples) {
            case rhi::SampleCount::kX1:
            case rhi::SampleCount::kX2:
            case rhi::SampleCount::kX4:
            case rhi::SampleCount::kX8:
            case rhi::SampleCount::kX16:
            case rhi::SampleCount::kX32:
            case rhi::SampleCount::kX64:
                return true;
            default:
                return false;
        }
    }

    friend bool operator==(const RenderMaterialAttachmentContract&, const RenderMaterialAttachmentContract&) = default;

private:
    [[nodiscard]] static bool is_depth_format(rhi::Format format) noexcept {
        switch (format) {
            case rhi::Format::kD24UnormS8Uint:
            case rhi::Format::kD32Float:
            case rhi::Format::kD32FloatS8Uint:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] static bool is_render_color_format(rhi::Format format) noexcept {
        switch (format) {
            case rhi::Format::kR8Unorm:
            case rhi::Format::kRg8Unorm:
            case rhi::Format::kRgba8Unorm:
            case rhi::Format::kBgra8Unorm:
            case rhi::Format::kRgba8Srgb:
            case rhi::Format::kRgba16Float:
            case rhi::Format::kRgba32Float:
            case rhi::Format::kR32Uint:
            case rhi::Format::kR32Float:
            case rhi::Format::kRg32Float:
            case rhi::Format::kRgb32Float:
                return true;
            default:
                return false;
        }
    }
};

}  // namespace aeg::renderer
