/**
 * @file editor_font.cpp
 * @brief Implements the backend-neutral editor font catalog.
 */
#include "editor_font.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace aeg::editor {
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
