/**
 * @file editor_font_pipeline.hpp
 * @brief Small vendored TrueType atlas builder used by runtime world-space text.
 */
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "imstb_truetype.h"
#include "result.hpp"

namespace aeg::application::asset_pipeline {

inline constexpr int kFontFirstGlyph = 32;
inline constexpr int kFontGlyphCount = 96;

struct FontAtlas final {
    uint32_t width{0};
    uint32_t height{0};
    float pixel_height{0.0f};
    std::vector<std::byte> rgba{};
    std::array<stbtt_bakedchar, kFontGlyphCount> glyphs{};
};

[[nodiscard]] foundation::Result build_font_atlas(std::span<const std::byte> font_bytes, FontAtlas& output,
                                                  float pixel_height = 64.0f);

}  // namespace aeg::application::asset_pipeline
