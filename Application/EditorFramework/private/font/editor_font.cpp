/**
 * @file editor_font.cpp
 * @brief Implements the backend-neutral editor font catalog.
 */
#include "editor_font.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace aeg::editor {
namespace {
using FontBytes = std::span<const std::byte>;

[[nodiscard]] bool contains(FontBytes data, std::size_t offset, std::size_t size) noexcept {
    return offset <= data.size() && size <= data.size() - offset;
}

// Callers establish the byte range before reading big-endian SFNT values.
[[nodiscard]] uint16_t read_u16(FontBytes data, std::size_t offset) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) << 8u) |
           static_cast<uint16_t>(data[offset + 1]);
}

[[nodiscard]] uint32_t read_u32(FontBytes data, std::size_t offset) noexcept {
    return static_cast<uint32_t>(read_u16(data, offset)) << 16u | read_u16(data, offset + 2);
}

[[nodiscard]] bool supported_cmap(FontBytes cmap, std::size_t glyph_count) noexcept {
    if (!contains(cmap, 0, 4) || read_u16(cmap, 0) != 0) {
        return false;
    }
    const std::size_t count = read_u16(cmap, 2);
    if (!contains(cmap, 4, count * 8)) {
        return false;
    }
    std::size_t selected = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t record = 4 + index * 8;
        const uint16_t platform = read_u16(cmap, record);
        const uint16_t encoding = read_u16(cmap, record + 2);
        // stb uses the last Unicode record; Microsoft Symbol (3, 0) is unsupported.
        if (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))) {
            selected = read_u32(cmap, record + 4);
        }
    }
    if (selected < 4 + count * 8 || !contains(cmap, selected, 4)) {
        return false;
    }
    FontBytes mapping = cmap.subspan(selected);
    const uint16_t format = read_u16(mapping, 0);
    const bool long_format = format == 12 || format == 13;
    if (long_format && !contains(mapping, 0, 16)) {
        return false;
    }
    const std::size_t length = long_format ? read_u32(mapping, 4) : read_u16(mapping, 2);
    if (!contains(mapping, 0, length)) {
        return false;
    }
    mapping = mapping.first(length);
    if (format == 0) {
        return mapping.size() >= 262 && std::all_of(mapping.begin() + 6, mapping.begin() + 262,
                                                    [glyph_count](std::byte glyph) {
                                                        return static_cast<uint8_t>(glyph) < glyph_count;
                                                    });
    }
    if (format == 6) {
        if (!contains(mapping, 0, 10) || !contains(mapping, 10, static_cast<std::size_t>(read_u16(mapping, 8)) * 2)) {
            return false;
        }
        for (std::size_t index = 0; index < read_u16(mapping, 8); ++index) {
            if (read_u16(mapping, 10 + index * 2) >= glyph_count) {
                return false;
            }
        }
        return true;
    }
    if (long_format) {
        if (!contains(mapping, 0, 16)) {
            return false;
        }
        const std::size_t groups = read_u32(mapping, 12);
        if (groups > (mapping.size() - 16) / 12) {
            return false;
        }
        uint32_t previous_end = 0;
        for (std::size_t index = 0; index < groups; ++index) {
            const uint32_t start = read_u32(mapping, 16 + index * 12);
            const uint32_t end = read_u32(mapping, 20 + index * 12);
            const uint32_t glyph = read_u32(mapping, 24 + index * 12);
            if (start > end || end > 0x10FFFFu || (index != 0 && start <= previous_end) || glyph >= glyph_count ||
                (format == 12 && end - start >= glyph_count - glyph)) {
                return false;
            }
            previous_end = end;
        }
        return true;
    }
    if (format != 4 || !contains(mapping, 0, 16)) {
        return false;
    }
    const uint16_t segment_bytes = read_u16(mapping, 6);
    const std::size_t segments = segment_bytes / 2;
    if (segments == 0 || (segment_bytes & 1u) != 0 || !contains(mapping, 16, segments * 8)) {
        return false;
    }
    uint16_t search_range = 2;
    uint16_t search_selector = 0;
    while (static_cast<std::size_t>(search_range) * 2 <= segment_bytes) {
        search_range = static_cast<uint16_t>(search_range * 2);
        ++search_selector;
    }
    if (read_u16(mapping, 8) != search_range || read_u16(mapping, 10) != search_selector ||
        read_u16(mapping, 12) != segment_bytes - search_range) {
        return false;
    }
    uint16_t previous_end = 0;
    for (std::size_t index = 0; index < segments; ++index) {
        const uint16_t end = read_u16(mapping, 14 + index * 2);
        const uint16_t start = read_u16(mapping, 16 + segments * 2 + index * 2);
        if (start > end || (index != 0 && start <= previous_end)) {
            return false;
        }
        const std::size_t range_record = 16 + segments * 6 + index * 2;
        const uint16_t range_offset = read_u16(mapping, range_record);
        if (range_offset != 0 && ((range_offset & 1u) != 0 || range_record + range_offset < 16 + segments * 8 ||
                                 !contains(mapping, range_record + range_offset,
                                           (static_cast<std::size_t>(end) - start + 1) * 2))) {
            return false;
        }
        const uint16_t delta = read_u16(mapping, 16 + segments * 4 + index * 2);
        for (uint32_t codepoint = start; codepoint <= end; ++codepoint) {
            // Match stb's mapping behavior, including its direct range-offset glyph reads.
            const uint16_t glyph = range_offset == 0 ? static_cast<uint16_t>(codepoint + delta)
                                                    : read_u16(mapping, range_record + range_offset + (codepoint - start) * 2);
            if (glyph >= glyph_count) {
                return false;
            }
        }
        previous_end = end;
    }
    return previous_end == 0xFFFFu;
}

/** @brief Checks the glyph payload reads used by stb, with cached compound traversal. */
struct GlyphValidator final {
    FontBytes glyf;
    FontBytes loca;
    uint16_t location_format;
    // 0 is unseen, 1 is visiting, and 2+ stores the validated compound depth.
    std::vector<uint8_t> depths;

    [[nodiscard]] std::size_t location(std::size_t index) const noexcept {
        return location_format == 0 ? static_cast<std::size_t>(read_u16(loca, index * 2)) * 2
                                    : read_u32(loca, index * 4);
    }

    [[nodiscard]] bool validate(std::size_t index, uint8_t nesting = 0) noexcept {
        // Conservative recursion limit also bounds stb's recursive compound rasterization.
        constexpr uint8_t kMaximumCompoundDepth = 32;
        if (nesting > kMaximumCompoundDepth || depths[index] == 1) {
            return false;
        }
        if (depths[index] >= 2) {
            return true;
        }
        depths[index] = 1;
        const FontBytes glyph = glyf.subspan(location(index), location(index + 1) - location(index));
        if (glyph.empty() || read_u16(glyph, 0) == 0) {
            depths[index] = 2;
            return true;
        }
        const int16_t contours = static_cast<int16_t>(read_u16(glyph, 0));
        std::size_t cursor = 10;
        if (contours > 0) {
            const std::size_t contour_bytes = static_cast<std::size_t>(contours) * 2;
            if (!contains(glyph, cursor, contour_bytes + 2)) {
                return false;
            }
            for (std::size_t contour = 1; contour < static_cast<std::size_t>(contours); ++contour) {
                if (read_u16(glyph, cursor + contour * 2) <= read_u16(glyph, cursor + (contour - 1) * 2)) {
                    return false;
                }
            }
            const std::size_t point_count = static_cast<std::size_t>(read_u16(glyph, cursor + contour_bytes - 2)) + 1;
            const std::size_t instruction_bytes = read_u16(glyph, cursor + contour_bytes);
            cursor += contour_bytes + 2;
            if (!contains(glyph, cursor, instruction_bytes)) {
                return false;
            }
            cursor += instruction_bytes;
            std::size_t coordinate_bytes = 0;
            for (std::size_t point = 0; point < point_count;) {
                if (!contains(glyph, cursor, 1)) {
                    return false;
                }
                const uint8_t flags = static_cast<uint8_t>(glyph[cursor++]);
                std::size_t repeats = 1;
                if ((flags & 8u) != 0) {
                    if (!contains(glyph, cursor, 1)) {
                        return false;
                    }
                    repeats += static_cast<uint8_t>(glyph[cursor++]);
                }
                if (repeats > point_count - point) {
                    return false;
                }
                coordinate_bytes += repeats * (((flags & 2u) != 0 ? 1u : (flags & 16u) != 0 ? 0u : 2u) +
                                                ((flags & 4u) != 0 ? 1u : (flags & 32u) != 0 ? 0u : 2u));
                point += repeats;
            }
            if (!contains(glyph, cursor, coordinate_bytes)) {
                return false;
            }
            depths[index] = 2;
            return true;
        }
        uint8_t depth = 2;
        uint16_t flags = 0;
        do {
            if (contours != -1 || !contains(glyph, cursor, 4)) {
                return false;
            }
            flags = read_u16(glyph, cursor);
            const std::size_t component = read_u16(glyph, cursor + 2);
            cursor += 4;
            // stb asserts on point-matched components; only x/y positioning is supported.
            if ((flags & 2u) == 0 || component >= depths.size()) {
                return false;
            }
            const std::size_t arguments = (flags & 1u) != 0 ? 4 : 2;
            const std::size_t transform = (flags & 8u) != 0 ? 2 : (flags & 64u) != 0 ? 4 : (flags & 128u) != 0 ? 8 : 0;
            if (!contains(glyph, cursor, arguments + transform) || !validate(component, static_cast<uint8_t>(nesting + 1))) {
                return false;
            }
            cursor += arguments + transform;
            depth = std::max(depth, static_cast<uint8_t>(depths[component] + 1));
            if (depth > kMaximumCompoundDepth + 2) {
                return false;
            }
        } while ((flags & 32u) != 0);
        depths[index] = depth;
        return true;
    }
};
}  // namespace

foundation::Result validate_editor_font_data(FontBytes data, uint32_t face_index) {
    if (!contains(data, 0, 12) || data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        face_index > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return foundation::invalid_argument("Editor font bytes or face index exceed the rasterizer limits");
    }
    std::size_t face_offset = 0;
    if (read_u32(data, 0) == 0x74746366u) { // ttcf
        const uint32_t version = read_u32(data, 4);
        const std::size_t faces = read_u32(data, 8);
        if ((version != 0x00010000u && version != 0x00020000u) || faces > (data.size() - 12) / 4 ||
            face_index >= faces) {
            return foundation::invalid_argument("Editor font collection header or face index is invalid");
        }
        face_offset = read_u32(data, 12 + static_cast<std::size_t>(face_index) * 4);
        if (face_offset < 12 + faces * 4 || !contains(data, face_offset, 12)) {
            return foundation::invalid_argument("Editor font collection face is outside the font data");
        }
    } else if (face_index != 0) {
        return foundation::invalid_argument("Editor standalone font has no requested collection face");
    }
    const uint32_t signature = read_u32(data, face_offset);
    if (signature != 0x00010000u && signature != 0x74727565u) { // TrueType or Apple 'true'
        return foundation::unsupported("Editor font must contain supported TrueType outlines");
    }
    const std::size_t table_count = read_u16(data, face_offset + 4);
    if (table_count == 0 || !contains(data, face_offset + 12, table_count * 16)) {
        return foundation::invalid_argument("Editor font table directory is incomplete");
    }
    constexpr std::array<uint32_t, 7> required_tags{
        0x636D6170u, 0x676C7966u, 0x68656164u, 0x68686561u, 0x686D7478u, 0x6C6F6361u, 0x6D617870u};
    std::array<FontBytes, required_tags.size()> tables{}; // cmap, glyf, head, hhea, hmtx, loca, maxp
    for (std::size_t index = 0; index < table_count; ++index) {
        const std::size_t record = face_offset + 12 + index * 16;
        const std::size_t offset = read_u32(data, record + 8);
        const std::size_t length = read_u32(data, record + 12);
        if (!contains(data, offset, length)) {
            return foundation::invalid_argument("Editor font table extends outside the font data");
        }
        const auto tag = std::find(required_tags.begin(), required_tags.end(), read_u32(data, record));
        if (tag != required_tags.end()) {
            FontBytes& table = tables[static_cast<std::size_t>(tag - required_tags.begin())];
            if (table.data() != nullptr || offset == 0) {
                return foundation::invalid_argument("Editor font required table is duplicated or invalid");
            }
            table = data.subspan(offset, length);
        }
    }
    const auto& [cmap, glyf, head, hhea, hmtx, loca, maxp] = tables;
    if (glyf.data() == nullptr || head.size() < 54 || hhea.size() < 36 || maxp.size() < 6) {
        return foundation::unsupported("Editor font lacks required TrueType outline or metric tables");
    }
    const std::size_t glyph_count = read_u16(maxp, 4);
    const std::size_t metric_count = read_u16(hhea, 34);
    const uint16_t location_format = read_u16(head, 50);
    if (glyph_count == 0 || metric_count == 0 || metric_count > glyph_count || location_format > 1 ||
        read_u16(head, 18) == 0 || read_u16(hhea, 4) == read_u16(hhea, 6) ||
        !contains(hmtx, 0, metric_count * 4 + (glyph_count - metric_count) * 2) ||
        !contains(loca, 0, (glyph_count + 1) * (location_format == 0 ? 2 : 4))) {
        return foundation::invalid_argument("Editor font glyph locations or metrics are incomplete");
    }
    std::size_t previous_location = 0;
    for (std::size_t index = 0; index <= glyph_count; ++index) {
        const std::size_t location = location_format == 0 ? static_cast<std::size_t>(read_u16(loca, index * 2)) * 2
                                                        : read_u32(loca, index * 4);
        if (location < previous_location || location > glyf.size() ||
            (location != previous_location && location - previous_location < 10)) {
            return foundation::invalid_argument("Editor font glyph extends outside its outline table");
        }
        previous_location = location;
    }
    if (!supported_cmap(cmap, glyph_count)) {
        return foundation::unsupported("Editor font has no supported, complete Unicode character map");
    }
    try {
        GlyphValidator glyphs{glyf, loca, location_format, std::vector<uint8_t>(glyph_count)};
        for (std::size_t index = 0; index < glyph_count; ++index) {
            if (!glyphs.validate(index)) {
                return foundation::unsupported("Editor font glyph payload is incomplete or unsupported by the rasterizer");
            }
        }
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor font validation allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result EditorFontCatalog::register_font(const EditorFontDescriptor& descriptor,
                                                    std::span<const std::byte> font_data,
                                                    uint32_t face_index) {
    if (!descriptor.is_valid()) {
        return foundation::invalid_argument("Editor font descriptor is incomplete");
    }
    if (font_data.empty()) {
        return foundation::invalid_argument("Editor font data is empty");
    }
    if (find(descriptor.key) != nullptr) {
        return foundation::invalid_argument("Editor font key is already registered");
    }

    try {
        EditorFontAsset asset{};
        asset.descriptor = descriptor;
        asset.font_data.assign(font_data.begin(), font_data.end());
        asset.face_index = face_index;
        fonts_.push_back(std::move(asset));
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor font catalog allocation failed");
    }
}

foundation::Result EditorFontCatalog::unregister_font(std::string_view font_key) {
    const auto iterator = std::find_if(fonts_.begin(), fonts_.end(), [font_key](const EditorFontAsset& asset) {
        return asset.descriptor.key == font_key;
    });
    if (iterator == fonts_.end()) {
        return foundation::not_found("Editor font key is not registered");
    }
    fonts_.erase(iterator);
    return foundation::Result::success();
}

const EditorFontAsset* EditorFontCatalog::find(std::string_view font_key) const noexcept {
    const auto iterator = std::find_if(fonts_.begin(), fonts_.end(), [font_key](const EditorFontAsset& asset) {
        return asset.descriptor.key == font_key;
    });
    return iterator == fonts_.end() ? nullptr : &*iterator;
}

std::span<const EditorFontAsset> EditorFontCatalog::assets() const noexcept {
    return fonts_;
}
}  // namespace aeg::editor
