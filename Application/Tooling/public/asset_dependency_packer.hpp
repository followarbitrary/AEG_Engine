/**
 * @file asset_dependency_packer.hpp
 * @brief Builds a package from root assets and their FlatBuffers dependencies.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <vector>

#include "asset_package.hpp"
#include "cooked_asset.hpp"

namespace aeg::tooling {
/** @brief Resolves the verified FlatBuffers envelope bytes of one cooked asset identity. */
using CookedAssetResolver = std::function<foundation::ResultValue<std::vector<std::byte>>(AssetId)>;

/** @brief Traverses runtime references from scene roots and writes their deterministic `.aegbin` closure. */
class AssetDependencyPacker final {
public:
    [[nodiscard]] foundation::Result write(const std::filesystem::path& output, std::span<const AssetId> roots,
                                           const CookedAssetResolver& resolve) const;
};
}  // namespace aeg::tooling
