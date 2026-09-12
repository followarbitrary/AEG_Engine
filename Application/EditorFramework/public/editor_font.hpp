/**
 * @file editor_font.hpp
 * @brief Backend-neutral editor font metadata and catalog.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "result.hpp"

namespace aeg::editor {
/** @brief Usage category exposed by the editor font preference model. */
enum class EditorFontKind : uint8_t {
    kInterface,
    kMonospace,
};

/** @brief Stable metadata used to present one bundled font in editor preferences. */
struct EditorFontDescriptor final {
    /** @brief Stable machine-readable key persisted by editor preferences. */
    std::string key{};
    /** @brief User-visible font family name. */
    std::string display_name{};
    /** @brief Intended editor usage category. */
    EditorFontKind kind{EditorFontKind::kInterface};
    /** @brief Whether the font contains common simplified Chinese glyphs. */
    bool supports_chinese{false};
    /** @brief Whether the font includes programming ligatures. */
    bool supports_ligatures{false};
    /** @brief Whether the font is one of the editor's shipped defaults. */
    bool is_default{false};
    /** @brief Whether preference actions must keep this font installed. */
    bool is_protected{false};

    /** @brief Returns whether the descriptor contains stable preference metadata. */
    [[nodiscard]] bool is_valid() const noexcept { return !key.empty() && !display_name.empty(); }
};

/** @brief Owned font bytes and metadata for one editor font family. */
struct EditorFontAsset final {
    /** @brief Stable metadata for the font family. */
    EditorFontDescriptor descriptor{};
    /** @brief Font bytes retained for the lifetime of the catalog. */
    std::vector<std::byte> font_data{};
    /** @brief Face index used when the system font file is a collection. */
    uint32_t face_index{0};

    /** @brief Returns whether both metadata and font bytes are present. */
    [[nodiscard]] bool is_valid() const noexcept { return descriptor.is_valid() && !font_data.empty(); }
};

/** @brief Application-owned catalog of bundled fonts available to the editor host. */
class EditorFontCatalog final {
public:
    /** @brief Registers a bundled font by copying its file bytes into the catalog. */
    [[nodiscard]] foundation::Result register_font(const EditorFontDescriptor& descriptor,
                                                   std::span<const std::byte> font_data,
                                                   uint32_t face_index = 0);
    /** @brief Removes a registered font from the catalog. */
    [[nodiscard]] foundation::Result unregister_font(std::string_view font_key);

    /** @brief Finds a registered font without transferring ownership. */
    [[nodiscard]] const EditorFontAsset* find(std::string_view font_key) const noexcept;
    /** @brief Returns all registered font assets in deterministic registration order. */
    [[nodiscard]] std::span<const EditorFontAsset> assets() const noexcept;
    /** @brief Returns the number of registered font assets. */
    [[nodiscard]] std::size_t font_count() const noexcept { return fonts_.size(); }

private:
    /** @brief Owned font assets presented to the editor host. */
    std::vector<EditorFontAsset> fonts_{};
};
}  // namespace aeg::editor
