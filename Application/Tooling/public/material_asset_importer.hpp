/**
 * @file material_asset_importer.hpp
 * @brief UUID-authored v1 Material source importer.
 */
#pragma once

#include "asset_cooker.hpp"

namespace aeg::tooling {
/** @brief Cooks the bounded `.aegmat` v1 source contract into an AGM1 payload. */
class MaterialAssetImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports(const std::filesystem::path& source) const noexcept override;
    [[nodiscard]] foundation::ResultValue<std::vector<CookedAsset>> cook(const std::filesystem::path& source,
                                                                         const AssetMeta& meta) const override;
};
}  // namespace aeg::tooling
