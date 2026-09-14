/**
 * @file editor_font_pipeline.cpp
 * @brief Implements the vendored TrueType bitmap atlas builder.
 */
#include "editor_font_pipeline.hpp"

#include <algorithm>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace aeg::application::asset_pipeline {

foundation::Result build_font_atlas(std::span<const std::byte> font_bytes, FontAtlas& output, float pixel_height) {
    if (font_bytes.empty() || pixel_height <= 0.0f) {
        return foundation::invalid_argument("Font atlas input is invalid");
    }
    constexpr uint32_t kWidth = 1024;
    constexpr uint32_t kHeight = 256;
    std::vector<unsigned char> coverage(static_cast<std::size_t>(kWidth) * kHeight, 0);
    std::array<stbtt_bakedchar, kFontGlyphCount> glyphs{};
    const int baked = stbtt_BakeFontBitmap(reinterpret_cast<const unsigned char*>(font_bytes.data()), 0, pixel_height,
                                           coverage.data(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                           kFontFirstGlyph, kFontGlyphCount, glyphs.data());
    if (baked <= 0) {
        return foundation::invalid_argument("TrueType font atlas baking failed");
    }
    output = {};
    output.width = kWidth;
    output.height = kHeight;
    output.pixel_height = pixel_height;
    output.glyphs = glyphs;
    output.rgba.resize(static_cast<std::size_t>(kWidth) * kHeight * 4u);
    for (std::size_t index = 0; index < coverage.size(); ++index) {
        output.rgba[index * 4u + 0u] = std::byte{0xff};
        output.rgba[index * 4u + 1u] = std::byte{0xff};
        output.rgba[index * 4u + 2u] = std::byte{0xff};
        output.rgba[index * 4u + 3u] = static_cast<std::byte>(coverage[index]);
    }
    return foundation::Result::success();
}

}  // namespace aeg::application::asset_pipeline
