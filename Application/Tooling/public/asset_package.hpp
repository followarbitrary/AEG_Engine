/**
 * @file asset_package.hpp
 * @brief Versioned single-file cooked asset package reader and writer.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "asset_metadata.hpp"

namespace aeg::tooling {
/** @brief One cooked payload written into an asset package. */
struct AssetPackageEntry final {
    AssetId id{};
    AssetKind kind{AssetKind::kMesh};
    uint32_t schema_version{};
    std::vector<std::byte> bytes{};
};

/** @brief A decoded package entry returned with owned payload bytes. */
struct PackagedAsset final {
    AssetKind kind{AssetKind::kMesh};
    uint32_t schema_version{};
    std::vector<std::byte> bytes{};
};

/** @brief Writes one deterministic, indexed `.aegbin` package. */
class AssetPackageWriter final {
public:
    [[nodiscard]] foundation::Result write(const std::filesystem::path& file,
                                           std::vector<AssetPackageEntry> entries) const;
};

/** @brief Validates and serves entries from one loaded `.aegbin` package. */
class AssetPackageReader final {
public:
    [[nodiscard]] static foundation::ResultValue<AssetPackageReader> open(const std::filesystem::path& file);

    [[nodiscard]] std::size_t entry_count() const noexcept;
    [[nodiscard]] foundation::ResultValue<PackagedAsset> read(AssetId id) const;

private:
    struct IndexEntry final {
        AssetId id{};
        AssetKind kind{AssetKind::kMesh};
        uint32_t schema_version{};
        uint64_t offset{};
        uint64_t byte_count{};
    };

    std::vector<std::byte> bytes_{};
    std::vector<IndexEntry> index_{};
};
}  // namespace aeg::tooling
