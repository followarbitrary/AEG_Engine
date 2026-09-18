/** @file texture_payload.cpp */
#include "texture_payload.hpp"

#include <cstring>
#include <limits>

namespace aeg::asset {
namespace {
template <typename T>
[[nodiscard]] bool read_le(std::span<const std::byte> bytes, std::size_t offset, T& output) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        return false;
    std::memcpy(&output, bytes.data() + offset, sizeof(T));
    return true;
}
}  // namespace

foundation::ResultValue<TexturePayload> decode_texture_payload(std::span<const std::byte> bytes) {
    uint32_t width = 0, height = 0, channels = 0;
    if (!read_le(bytes, 0, width) || !read_le(bytes, 4, height) || !read_le(bytes, 8, channels) || width == 0 ||
        height == 0 || channels == 0 || channels > 4) {
        return {foundation::invalid_argument("Texture payload header is invalid"), {}};
    }
    constexpr uint64_t kMaxPixels = 16ull * 1024ull * 1024ull;
    const uint64_t pixel_count = static_cast<uint64_t>(width) * height;
    const uint64_t pixel_bytes = pixel_count * channels;
    if (pixel_count > kMaxPixels || pixel_bytes > std::numeric_limits<std::size_t>::max() - 12 ||
        bytes.size() != 12 + static_cast<std::size_t>(pixel_bytes)) {
        return {foundation::invalid_argument("Texture payload size is invalid"), {}};
    }
    TexturePayload payload{};
    payload.width = width;
    payload.height = height;
    payload.channels = channels;
    payload.pixels.assign(bytes.begin() + 12, bytes.end());
    return {foundation::Result::success(), std::move(payload)};
}
}  // namespace aeg::asset
