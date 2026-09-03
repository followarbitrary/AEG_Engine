#pragma once

#include <cstdint>

#include "result.hpp"
#include "rhi.hpp"

namespace aeg::resource {
    struct OffscreenRenderTargetDesc {
        rhi::Extent2D extent{};
        rhi::Format format{rhi::Format::kUnknown};
        rhi::SampleCount samples{rhi::SampleCount::kX1};
        bool sampled{true};
        rhi::DebugName name{};
    };

    struct OffscreenRenderTarget {
        rhi::TextureHandle texture{};
        rhi::TextureViewHandle color_view{};
        rhi::TextureViewHandle sampled_view{};
        rhi::TextureDesc texture_desc{};

        [[nodiscard]] bool is_valid() const noexcept {
            return texture.is_valid() && color_view.is_valid() &&
                   (!rhi::has_flag(texture_desc.capabilities, rhi::TextureCapability::kSampled) ||
                    sampled_view.is_valid());
        }

        void reset() noexcept { *this = {}; }
    };

    [[nodiscard]] foundation::ResultValue<OffscreenRenderTarget> create_offscreen_render_target(
            rhi::Device& device, const OffscreenRenderTargetDesc& desc);

    [[nodiscard]] foundation::Result destroy_offscreen_render_target(
            rhi::Device& device, OffscreenRenderTarget& target);
} // namespace aeg::resource
