#pragma once

#include "asset_cooker.hpp"

namespace aeg::tooling {

class FeatureGraphAssetImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports(const std::filesystem::path& source) const noexcept override;
    [[nodiscard]] foundation::ResultValue<std::vector<CookedAsset>> cook(
        const std::filesystem::path& source, const AssetMeta& meta) const override;
};

}  // namespace aeg::tooling
