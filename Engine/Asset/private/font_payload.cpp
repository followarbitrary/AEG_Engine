#include "font_payload.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace aeg::asset {
namespace {
constexpr std::array<char, 4> kMagic{'A', 'E', 'G', 'F'};
constexpr uint32_t kVersion = 1;
void put_u32(std::vector<std::byte>& out, std::size_t at, uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        out[at + i] = static_cast<std::byte>((value >> (i * 8u)) & 0xffu);
}
uint32_t get_u32(std::span<const std::byte> bytes, std::size_t at) {
    uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[at + i])) << (i * 8u);
    return value;
}
uint16_t get_u16(std::span<const std::byte> bytes, std::size_t at) {
    return static_cast<uint16_t>(std::to_integer<uint8_t>(bytes[at]) << 8u | std::to_integer<uint8_t>(bytes[at + 1]));
}
uint32_t get_be32(std::span<const std::byte> bytes, std::size_t at) {
    return (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[at])) << 24u) |
           (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[at + 1])) << 16u) |
           (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[at + 2])) << 8u) |
           static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[at + 3]));
}
foundation::Result validate_sfnt(std::span<const std::byte> bytes, uint32_t face_index) {
    if (bytes.size() < 12)
        return foundation::invalid_argument("Font face header is truncated");
    const uint32_t signature = get_be32(bytes, 0);
    std::size_t offset = 0;
    uint32_t face_count = 1;
    if (signature == 0x74746366u) {  // ttcf
        if (bytes.size() < 12)
            return foundation::invalid_argument("Font collection header is truncated");
        const uint32_t version = get_be32(bytes, 4);
        if (version != 0x00010000u && version != 0x00020000u)
            return foundation::unsupported("Unsupported TrueType collection version");
        face_count = get_be32(bytes, 8);
        if (face_count == 0 || face_count > (bytes.size() - 12) / 4)
            return foundation::invalid_argument("TrueType collection face count is invalid");
        if (face_index >= face_count)
            return foundation::invalid_argument("Font face index is out of range");
        offset = get_be32(bytes, 12 + static_cast<std::size_t>(face_index) * 4);
    } else if (signature != 0x00010000u && signature != 0x4f54544fu && signature != 0x74727565u &&
               signature != 0x74797031u) {
        return foundation::invalid_argument("Font source is not a supported sfnt font");
    } else if (face_index != 0) {
        return foundation::invalid_argument("Single-face font has no requested face index");
    }
    if (offset > bytes.size() || bytes.size() - offset < 12)
        return foundation::invalid_argument("Font face offset is out of bounds");
    const auto face = bytes.subspan(offset);
    const uint16_t table_count = get_u16(face, 4);
    const std::size_t records_size = static_cast<std::size_t>(table_count) * 16;
    if (records_size > face.size() - 12)
        return foundation::invalid_argument("Font table directory is truncated");
    for (std::size_t i = 0; i < table_count; ++i) {
        const std::size_t record = 12 + i * 16;
        const uint32_t table_offset = get_be32(face, record + 8);
        const uint32_t table_length = get_be32(face, record + 12);
        if (table_offset > face.size() || table_length > face.size() - table_offset)
            return foundation::invalid_argument("Font table range is out of bounds");
    }
    return foundation::Result::success();
}
}  // namespace
foundation::Result validate_font_bytes(std::span<const std::byte> bytes, uint32_t face_index) {
    return validate_sfnt(bytes, face_index);
}
foundation::ResultValue<std::vector<std::byte>> encode_font_payload(const FontPayload& payload) {
    if (payload.font_bytes.empty())
        return {foundation::invalid_argument("Font payload bytes are empty"), {}};
    if (payload.font_bytes.size() > UINT32_MAX)
        return {foundation::invalid_argument("Font payload is too large"), {}};
    auto valid = validate_font_bytes(payload.font_bytes, payload.face_index);
    if (!valid)
        return {valid, {}};
    std::vector<std::byte> bytes(16 + payload.font_bytes.size());
    std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
    put_u32(bytes, 4, kVersion);
    put_u32(bytes, 8, payload.face_index);
    put_u32(bytes, 12, static_cast<uint32_t>(payload.font_bytes.size()));
    std::copy(payload.font_bytes.begin(), payload.font_bytes.end(), bytes.begin() + 16);
    return {foundation::Result::success(), std::move(bytes)};
}
foundation::ResultValue<FontPayload> decode_font_payload(std::span<const std::byte> bytes) {
    if (bytes.size() < 16 || std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0)
        return {foundation::invalid_argument("Font payload header is invalid"), {}};
    if (get_u32(bytes, 4) != kVersion)
        return {foundation::unsupported("Unsupported font payload version"), {}};
    const uint32_t size = get_u32(bytes, 12);
    if (size == 0 || bytes.size() != 16ull + size)
        return {foundation::invalid_argument("Font payload size is invalid"), {}};
    FontPayload result{};
    result.face_index = get_u32(bytes, 8);
    result.font_bytes.assign(bytes.begin() + 16, bytes.end());
    auto valid = validate_font_bytes(result.font_bytes, result.face_index);
    if (!valid)
        return {valid, {}};
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::asset
