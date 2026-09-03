/**
 * @file render_target.hpp
 * @brief Creation and destruction helpers for offscreen color targets.
 */
#pragma once

#include <cstdint>

#include "result.hpp"
#include "rhi.hpp"

namespace aeg::resource {
    /** @brief Description of an offscreen color render target. */
    struct OffscreenRenderTargetDesc {
        rhi::Extent2D extent{};
        rhi::Format format{rhi::Format::kUnknown};
        rhi::SampleCount samples{rhi::SampleCount::kX1};
        bool sampled{true};
        rhi::DebugName name{};
    };

    /**
     * @brief RHI objects comprising an offscreen color target.
     *
     * The texture and views are owned by this value after successful creation.
     * A multisampled target may not expose sampled_view when the backend cannot
     * sample it directly.
     */
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

        /** @brief Clears handles and metadata without destroying RHI objects. */
        void reset() noexcept { *this = {}; }
    };

    /** @brief Creates an offscreen color texture and its required views. */
    [[nodiscard]] foundation::ResultValue<OffscreenRenderTarget> create_offscreen_render_target(
            rhi::Device& device, const OffscreenRenderTargetDesc& desc);

    /** @brief Destroys all objects owned by an offscreen target. */
    [[nodiscard]] foundation::Result destroy_offscreen_render_target(
            rhi::Device& device, OffscreenRenderTarget& target);
} // namespace aeg::resource
