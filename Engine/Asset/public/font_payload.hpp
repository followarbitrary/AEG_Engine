#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "result.hpp"

namespace aeg::asset {
/** Runtime font face bytes and collection face index. */
struct FontPayload final {
    uint32_t face_index{};
    std::vector<std::byte> font_bytes{};
};

/** Validates an sfnt face or collection and the selected collection face index. */
[[nodiscard]] foundation::Result validate_font_bytes(std::span<const std::byte> bytes, uint32_t face_index = 0);

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_font_payload(const FontPayload& payload);
[[nodiscard]] foundation::ResultValue<FontPayload> decode_font_payload(std::span<const std::byte> bytes);
}  // namespace aeg::asset
