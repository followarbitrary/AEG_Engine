/**
 * @file render_graph_types.hpp
 * @brief Logical RenderGraph resource handles and buffer ranges.
 */
#pragma once

#include <cstdint>
#include <limits>

namespace aeg::render_graph {
/** @brief Sentinel used for invalid render-graph handle indices. */
inline constexpr uint32_t kInvalidHandleIndex = std::numeric_limits<uint32_t>::max();

/**
 * @brief Opaque handle for a logical render-graph texture.
 *
 * Handles are valid only for the graph instance and resource generation
 * that created them. They are invalidated by reset().
 */
struct RenderGraphTexture {
    uint32_t index{kInvalidHandleIndex};
    uint32_t generation{0};
    uint64_t graph_id{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != kInvalidHandleIndex && generation != 0 && graph_id != 0;
    }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(RenderGraphTexture, RenderGraphTexture) = default;
};

/** @brief Opaque handle for a logical render-graph buffer. */
struct RenderGraphBuffer {
    uint32_t index{kInvalidHandleIndex};
    uint32_t generation{0};
    uint64_t graph_id{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != kInvalidHandleIndex && generation != 0 && graph_id != 0;
    }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(RenderGraphBuffer, RenderGraphBuffer) = default;
};

/** @brief Opaque handle for a render-graph pass declaration. */
struct RenderGraphPass {
    uint32_t index{kInvalidHandleIndex};
    uint32_t generation{0};
    uint64_t graph_id{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != kInvalidHandleIndex && generation != 0 && graph_id != 0;
    }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(RenderGraphPass, RenderGraphPass) = default;
};

/** @brief Byte range of a logical buffer used by one pass. */
struct RenderGraphBufferRange {
    uint64_t offset{0};
    uint64_t size{std::numeric_limits<uint64_t>::max()};
};
}  // namespace aeg::render_graph
