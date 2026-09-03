/**
 * @file texture.hpp
 * @brief CPU texture subresource data and GPU texture ownership types.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "result.hpp"
#include "rhi_struct.hpp"

namespace aeg::resource {
    /**
     * @brief Non-owning bytes and pitches for one texture subresource.
     *
     * Subresources are tightly packed by default. Pitches are measured in bytes
     * and describe one row and one depth slice respectively. Compressed formats
     * use block-row and block-slice pitches.
     */
    struct TextureSubresourceData {
        std::span<const std::byte> data{};
        uint32_t row_pitch{0};
        uint32_t slice_pitch{0};
    };

    /**
     * @brief Non-owning texture description and ordered subresource data.
     *
     * Entries are ordered mip-major, then array-layer-major: all layers of mip
     * 0, followed by all layers of mip 1, and so on. A 3D texture has one entry
     * per mip; each entry contains the complete depth volume.
     */
    struct TextureData {
        rhi::TextureDesc desc{};
        std::span<const TextureSubresourceData> subresources{};
    };

    /**
     * @brief GPU texture handle, optional sampled view, and effective metadata.
     *
     * The RHI handles are owned by the caller. This value does not destroy them
     * when reset or discarded.
     */
    struct GpuTexture {
        rhi::TextureHandle texture{};
        rhi::TextureViewHandle sampled_view{};
        rhi::Format format{rhi::Format::kUnknown};
        rhi::Extent3D extent{};
        uint32_t mip_levels{0};
        uint32_t array_layers{0};

        [[nodiscard]] bool is_valid() const noexcept { return texture.is_valid(); }

        [[nodiscard]] bool has_sampled_view() const noexcept { return sampled_view.is_valid(); }

        /** @brief Clears handles and metadata without destroying RHI objects. */
        void reset() noexcept { *this = {}; }
    };

    /** @brief Validates texture dimensions, subresource count, pitches, and byte sizes. */
    [[nodiscard]] foundation::Result validate_texture_data(const TextureData& texture) noexcept;
} // namespace aeg::resource
