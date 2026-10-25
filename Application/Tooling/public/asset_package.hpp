/**
 * @file asset_package.hpp
 * @brief Versioned single-file cooked asset package reader and writer.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "asset_package_reader.hpp"
#include "asset_metadata.hpp"

namespace aeg::tooling {
/** @brief One cooked payload written into an asset package. */
struct AssetPackageEntry final {
    AssetId id{};
    AssetKind kind{AssetKind::kMesh};
    uint32_t schema_version{};
    std::vector<std::byte> bytes{};
};

/** @brief Compatibility name for the Engine-owned runtime package reader result. */
using PackagedAsset = asset::PackagedAsset;

/** @brief Writes one deterministic, indexed `.aegbin` package. */
class AssetPackageWriter final {
public:
    [[nodiscard]] foundation::Result write(const std::filesystem::path& file,
                                           std::vector<AssetPackageEntry> entries) const;
};

/** @brief Compatibility name for the Engine-owned runtime package reader. */
using AssetPackageReader = asset::AssetPackageReader;
}  // namespace aeg::tooling
