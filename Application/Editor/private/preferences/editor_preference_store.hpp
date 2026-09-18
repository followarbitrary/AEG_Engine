#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "font/editor_font_repository.hpp"

namespace aeg::application {

/** @brief Versioned editor preference values persisted independently of game projects. */
struct EditorPreferenceFileData final {
    std::string active_font_key{};
    float font_size_pixels{18.0f};
    std::vector<EditorFontFileRecord> user_fonts{};
};

/** @brief Reads and atomically writes the bounded editor preference file format. */
class EditorPreferenceStore final {
public:
    explicit EditorPreferenceStore(std::filesystem::path file_path) : file_path_(std::move(file_path)) {}

    [[nodiscard]] foundation::ResultValue<EditorPreferenceFileData> load() const;
    [[nodiscard]] foundation::Result write_atomic(const EditorPreferenceFileData& data) const;

private:
    std::filesystem::path file_path_{};
};

}  // namespace aeg::application
