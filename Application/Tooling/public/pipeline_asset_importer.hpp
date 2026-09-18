#pragma once

#include <memory>

#include "asset_cooker.hpp"
#include "pipeline_authoring.hpp"

namespace aeg::tooling {

class PipelineAssetImporter final : public AssetImporter {
public:
    explicit PipelineAssetImporter(std::shared_ptr<const pipeline::TypeCatalog> catalog);
    [[nodiscard]] bool supports(const std::filesystem::path& source) const noexcept override;
    [[nodiscard]] foundation::ResultValue<std::vector<CookedAsset>> cook(const std::filesystem::path& source,
                                                                         const AssetMeta& meta) const override;

private:
    std::shared_ptr<const pipeline::TypeCatalog> catalog_{};
};

}  // namespace aeg::tooling
