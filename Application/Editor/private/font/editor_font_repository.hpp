#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "editor_font.hpp"

namespace aeg::application {

/** @brief Stable mapping persisted for one imported user font file. */
struct EditorFontFileRecord final {
    std::string key{};
    std::string relative_file_name{};
};

/** @brief Owns user font files and an atomic import/removal transaction. */
class EditorFontRepository final {
public:
    EditorFontRepository() = default;
    ~EditorFontRepository();

    EditorFontRepository(const EditorFontRepository&) = delete;
    EditorFontRepository& operator=(const EditorFontRepository&) = delete;

    [[nodiscard]] foundation::Result initialize(std::filesystem::path root_directory);
    [[nodiscard]] foundation::ResultValue<editor::EditorFontCatalog> scan() const;
    /** @brief Returns deterministic persisted filenames for valid user fonts. */
    [[nodiscard]] foundation::ResultValue<std::vector<EditorFontFileRecord>> list_records() const;
    [[nodiscard]] foundation::ResultValue<editor::EditorFontAsset> stage_import(
        const std::filesystem::path& source_path);
    [[nodiscard]] foundation::Result stage_remove(std::string_view font_key);
    [[nodiscard]] foundation::Result commit();
    void rollback() noexcept;

    [[nodiscard]] const std::filesystem::path& root_directory() const noexcept { return root_directory_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    struct StagedAddition final {
        std::string key{};
        std::string relative_file_name{};
        std::filesystem::path staged_path{};
    };

    [[nodiscard]] foundation::ResultValue<editor::EditorFontAsset> inspect_file(
        const std::filesystem::path& path) const;
    [[nodiscard]] foundation::Result ensure_transaction_directory();
    [[nodiscard]] static std::string user_key(std::string_view inspected_key);
    [[nodiscard]] static bool accepted_extension(const std::filesystem::path& path) noexcept;

    std::filesystem::path root_directory_{};
    std::filesystem::path transaction_directory_{};
    std::vector<StagedAddition> additions_{};
    std::vector<std::string> removals_{};
    bool initialized_{false};
};

}  // namespace aeg::application
