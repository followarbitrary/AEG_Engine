/**
 * @file editor_system_font_catalog.hpp
 * @brief Application-private system font discovery for the editor.
 */
#pragma once

#include <string>

#include "editor_font.hpp"
#include "result.hpp"

namespace aeg::application {
/** @brief System font catalog snapshot and the best available editor default. */
struct EditorSystemFontCatalogData final {
    /** @brief Font assets read from the operating system font collection. */
    editor::EditorFontCatalog catalog{};
    /** @brief Stable key selected as the initial editor font. */
    std::string default_font_key{};
};

/** @brief Reads installed fonts without exposing operating-system types to EditorFramework. */
class EditorSystemFontCatalog final {
public:
    /** @brief Enumerates installed system fonts and loads usable faces into memory. */
    [[nodiscard]] foundation::ResultValue<EditorSystemFontCatalogData> load() const;
};
}  // namespace aeg::application
