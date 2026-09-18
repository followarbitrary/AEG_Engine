/**
 * @file texture_payload.hpp
 * @brief Neutral RGBA texture payload decoder.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "result.hpp"

namespace aeg::asset {
struct TexturePayload final {
    uint32_t width{};
    uint32_t height{};
    uint32_t channels{};
    std::vector<std::byte> pixels{};
};

[[nodiscard]] foundation::ResultValue<TexturePayload> decode_texture_payload(std::span<const std::byte> bytes);
}  // namespace aeg::asset
