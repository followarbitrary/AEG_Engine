/**
 * @file editor_font_validation_smoke.cpp
 * @brief Verifies editor font compatibility without depending on installed fonts.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

#include "editor_font.hpp"

namespace {
using Bytes = std::vector<std::byte>;

void write_u16(Bytes& data, std::size_t offset, uint16_t value) {
    data[offset] = static_cast<std::byte>(value >> 8u);
    data[offset + 1] = static_cast<std::byte>(value);
}

void write_u32(Bytes& data, std::size_t offset, uint32_t value) {
    write_u16(data, offset, static_cast<uint16_t>(value >> 16u));
    write_u16(data, offset + 2, static_cast<uint16_t>(value));
}

uint32_t read_u32(const Bytes& data, std::size_t offset) {
    uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8u) | static_cast<uint8_t>(data[offset + index]);
    }
    return value;
}

constexpr std::size_t kCmapRecord = 12;
constexpr std::size_t kGlyfRecord = 28;
constexpr std::size_t kHeadRecord = 44;
constexpr std::size_t kHheaRecord = 60;
constexpr std::size_t kLocaRecord = 92;

// Two empty glyphs with a Unicode mapping for 'A'; no rasterizer is needed here.
Bytes make_font() {
    constexpr std::array<std::string_view, 7> tags{"cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp"};
    constexpr std::array<uint32_t, 7> sizes{274, 1, 54, 36, 8, 6, 6};
    Bytes data(12 + tags.size() * 16);
    write_u32(data, 0, 0x00010000);
    write_u16(data, 4, static_cast<uint16_t>(tags.size()));
    for (std::size_t index = 0; index < tags.size(); ++index) {
        const std::size_t record = 12 + index * 16;
        for (std::size_t character = 0; character < 4; ++character) {
            data[record + character] = static_cast<std::byte>(tags[index][character]);
        }
        write_u32(data, record + 8, static_cast<uint32_t>(data.size()));
        write_u32(data, record + 12, sizes[index]);
        data.resize(data.size() + sizes[index]);
    }
    const std::size_t cmap = read_u32(data, kCmapRecord + 8);
    write_u16(data, cmap + 2, 1);
    write_u16(data, cmap + 4, 3);
    write_u16(data, cmap + 6, 1);
    write_u32(data, cmap + 8, 12);
    write_u16(data, cmap + 14, 262);
    data[cmap + 18 + 'A'] = std::byte{1};
    write_u16(data, read_u32(data, kHeadRecord + 8) + 18, 1000);
    const std::size_t hhea = read_u32(data, kHheaRecord + 8);
    write_u16(data, hhea + 4, 800);
    write_u16(data, hhea + 6, static_cast<uint16_t>(-200));
    write_u16(data, hhea + 34, 2);
    write_u16(data, read_u32(data, 108 + 8) + 4, 2);
    return data;
}

bool expect(const Bytes& data, uint32_t face, bool accepted, const char* label) {
    const auto result = aeg::editor::validate_editor_font_data(data, face);
    if (static_cast<bool>(result) == accepted) {
        return true;
    }
    std::fprintf(stderr, "Font validation regression: %s\n", label);
    return false;
}

Bytes with_glyph(Bytes glyph) {
    Bytes font = make_font();
    if ((glyph.size() & 1u) != 0) {
        glyph.push_back(std::byte{0});
    }
    write_u32(font, kGlyfRecord + 8, static_cast<uint32_t>(font.size()));
    write_u32(font, kGlyfRecord + 12, static_cast<uint32_t>(glyph.size()));
    write_u16(font, read_u32(font, kLocaRecord + 8) + 4, static_cast<uint16_t>(glyph.size() / 2));
    font.insert(font.end(), glyph.begin(), glyph.end());
    return font;
}
}  // namespace

int main() {
    const Bytes font = make_font();
    const std::size_t cmap = read_u32(font, kCmapRecord + 8);
    Bytes symbol = font;
    write_u16(symbol, cmap + 6, 0); // Microsoft Symbol is accepted by DirectWrite, but not stb.
    if (!expect(symbol, 0, false, "symbol-only cmap must be rejected") ||
        !expect(font, 0, true, "Unicode TrueType must remain available") ||
        !expect({}, 0, false, "empty data") || !expect(Bytes(4), 0, false, "truncated header") ||
        !expect(font, 1, false, "standalone font has only face zero") ||
        !expect(font, std::numeric_limits<uint32_t>::max(), false, "face index must fit the rasterizer")) {
        return 1;
    }
    const auto rejects_change = [&font](std::size_t offset, uint32_t value, const char* label) {
        Bytes changed = font;
        write_u32(changed, offset, value);
        return expect(changed, 0, false, label);
    };
    if (!rejects_change(0, 0x4F54544F, "CFF/OpenType signature") ||
        !rejects_change(kGlyfRecord, 0x43464632, "CFF2 outlines") ||
        !rejects_change(kHeadRecord + 12, 4, "truncated head table") ||
        !rejects_change(kLocaRecord + 12, 2, "truncated glyph locations") ||
        !rejects_change(kCmapRecord + 8, 0xFFFFFFF0, "table offset outside file") ||
        !rejects_change(kCmapRecord + 12, 0xFFFFFFF0, "table length outside file") ||
        !rejects_change(cmap + 8, 0xFFFFFFF0, "cmap offset outside table")) {
        return 2;
    }
    Bytes unsupported = font;
    write_u16(unsupported, cmap + 12, 2);
    if (!expect(unsupported, 0, false, "unsupported cmap format must not reach glyph baking")) {
        return 3;
    }
    Bytes collection(20);
    write_u32(collection, 0, 0x74746366);
    write_u32(collection, 4, 0x00010000);
    write_u32(collection, 8, 2);
    write_u32(collection, 12, 20);
    write_u32(collection, 16, 20);
    collection.insert(collection.end(), font.begin(), font.end());
    for (std::size_t index = 0; index < 7; ++index) {
        const std::size_t offset = 20 + 12 + index * 16 + 8;
        write_u32(collection, offset, read_u32(collection, offset) + 20);
    }
    if (!expect(collection, 1, true, "nonzero collection face must remain available") ||
        !expect(collection, 2, false, "collection face out of range")) {
        return 4;
    }
    write_u32(collection, 16, 0xFFFFFFF0);
    if (!expect(collection, 1, false, "collection face offset outside file")) {
        return 5;
    }
    Bytes bad_mapping = font;
    bad_mapping[cmap + 18 + 'A'] = std::byte{2};
    if (!expect(bad_mapping, 0, false, "cmap glyph index must fit horizontal metrics")) {
        return 6;
    }
    Bytes simple(16);
    write_u16(simple, 0, 1);
    simple[14] = std::byte{0x31}; // One on-curve point, unchanged x/y coordinates.
    if (!expect(with_glyph(simple), 0, true, "bounded simple glyph remains supported")) {
        return 7;
    }
    simple.resize(12);
    if (!expect(with_glyph(simple), 0, false, "truncated simple contour header")) {
        return 8;
    }
    simple.resize(16);
    simple[14] = std::byte{1}; // Two signed coordinates do not fit the remaining byte.
    if (!expect(with_glyph(simple), 0, false, "truncated simple glyph coordinates")) {
        return 9;
    }
    simple[14] = std::byte{0x39};
    simple[15] = std::byte{1};
    if (!expect(with_glyph(simple), 0, false, "repeat flags exceed the contour point count")) {
        return 10;
    }
    Bytes compound(18);
    write_u16(compound, 0, 0xFFFF);
    write_u16(compound, 10, 3); // Word-sized x/y arguments; component is empty glyph zero.
    if (!expect(with_glyph(compound), 0, true, "bounded compound glyph remains supported")) {
        return 11;
    }
    write_u16(compound, 10, 1);
    if (!expect(with_glyph(compound), 0, false, "stb cannot rasterize point-matched compound glyphs")) {
        return 12;
    }
    write_u16(compound, 10, 3);
    write_u16(compound, 12, 1);
    if (!expect(with_glyph(compound), 0, false, "recursive compound cycle")) {
        return 13;
    }
    write_u16(compound, 12, 2);
    if (!expect(with_glyph(compound), 0, false, "compound glyph index outside the face")) {
        return 14;
    }
    write_u16(compound, 12, 0);
    compound.resize(14);
    if (!expect(with_glyph(compound), 0, false, "truncated compound arguments")) {
        return 15;
    }
    return 0;
}
