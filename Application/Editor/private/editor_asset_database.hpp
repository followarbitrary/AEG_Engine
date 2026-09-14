/**
 * @file editor_asset_database.hpp
 * @brief Project-local Asset Browser index and import metadata.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "result.hpp"

namespace aeg::application {

enum class EditorAssetKind : uint8_t {
    kUnknown,
    kTexture,
    kMesh,
    kMaterial,
    kScene,
    kFont,
};

enum class EditorAssetStatus : uint8_t {
    kImported,
    kMissing,
    kUnsupported,
    kFailed,
};

struct EditorAssetRecord final {
    uint64_t id{0};
    EditorAssetKind kind{EditorAssetKind::kUnknown};
    EditorAssetStatus status{EditorAssetStatus::kUnsupported};
    std::string relative_path{};
    std::string display_name{};
    std::string extension{};
    uint64_t file_size{0};
    int64_t modified_ticks{0};
    uint64_t content_hash{0};
    std::string error{};
    std::vector<uint64_t> dependencies{};
};

/** @brief Scans project Assets and persists stable IDs in a project-local manifest. */
class EditorAssetDatabase final {
public:
    EditorAssetDatabase() = default;

    [[nodiscard]] foundation::Result initialize(const std::filesystem::path& project_root);
    [[nodiscard]] foundation::Result refresh();
    [[nodiscard]] foundation::Result reimport(uint64_t id);
    [[nodiscard]] foundation::Result rename(uint64_t id, std::string_view new_name);
    [[nodiscard]] foundation::Result remove(uint64_t id);

    [[nodiscard]] const std::filesystem::path& project_root() const noexcept { return project_root_; }
    [[nodiscard]] const std::filesystem::path& assets_root() const noexcept { return assets_root_; }
    [[nodiscard]] std::span<const EditorAssetRecord> assets() const noexcept { return assets_; }
    [[nodiscard]] const EditorAssetRecord* find(uint64_t id) const noexcept;
    [[nodiscard]] const EditorAssetRecord* find_by_relative_path(std::string_view path) const noexcept;
    [[nodiscard]] bool is_valid(uint64_t id, EditorAssetKind kind) const noexcept;

private:
    [[nodiscard]] foundation::Result load_manifest();
    [[nodiscard]] foundation::Result save_manifest() const;

    std::filesystem::path project_root_{};
    std::filesystem::path assets_root_{};
    std::filesystem::path manifest_path_{};
    std::vector<EditorAssetRecord> assets_{};
};

[[nodiscard]] std::string_view editor_asset_kind_name(EditorAssetKind kind) noexcept;
[[nodiscard]] std::string_view editor_asset_status_name(EditorAssetStatus status) noexcept;

}  // namespace aeg::application
