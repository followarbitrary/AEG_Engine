/**
 * @file asset_package_reader.hpp
 * @brief Runtime-only verified reader for .aegbin packages.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "asset_identity.hpp"
#include "cooked_asset.hpp"

namespace aeg::asset {
/** @brief One package entry returned with an identity and owned bytes. */
struct PackagedAsset final {
    foundation::AssetId id{};
    foundation::AssetKind kind{foundation::AssetKind::kMesh};
    uint32_t schema_version{};
    std::vector<std::byte> bytes{};
};

/** @brief Opens, validates and serves entries from a cooked package. */
class AssetPackageReader final {
public:
    [[nodiscard]] static foundation::ResultValue<AssetPackageReader> open(const std::filesystem::path& file);
    [[nodiscard]] std::size_t entry_count() const noexcept;
    [[nodiscard]] foundation::ResultValue<PackagedAsset> read(foundation::AssetId id) const;
    [[nodiscard]] foundation::ResultValue<CookedAsset> read_cooked(foundation::AssetId id) const;

private:
    struct IndexEntry final {
        foundation::AssetId id{};
        foundation::AssetKind kind{foundation::AssetKind::kMesh};
        uint32_t schema_version{};
        uint64_t offset{};
        uint64_t byte_count{};
    };

    std::vector<std::byte> bytes_{};
    std::vector<IndexEntry> index_{};
};
}  // namespace aeg::asset
