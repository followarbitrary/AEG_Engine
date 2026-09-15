/**
 * @file asset_cooker.hpp
 * @brief Extensible raw-asset import and transactional cooked-artifact publication.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "cooked_artifact_store.hpp"
#include "cooked_asset.hpp"

namespace aeg::tooling {
/** @brief Decodes one raw source format into typed, dependency-aware cooked assets. */
class AssetImporter {
public:
    virtual ~AssetImporter() = default;

    [[nodiscard]] virtual bool supports(const std::filesystem::path& source) const noexcept = 0;
    [[nodiscard]] virtual foundation::ResultValue<std::vector<CookedAsset>> cook(const std::filesystem::path& source,
                                                                                 const AssetMeta& meta) const = 0;
};

/** @brief Coordinates Meta loading, importer selection, FlatBuffers encoding, and bundle publication. */
class AssetCooker final {
public:
    explicit AssetCooker(std::filesystem::path cooked_cache_root);

    [[nodiscard]] foundation::Result register_default_importers();
    [[nodiscard]] foundation::Result register_importer(std::unique_ptr<AssetImporter> importer);
    [[nodiscard]] foundation::ResultValue<CookedArtifactBundle> cook(const std::filesystem::path& source) const;

private:
    CookedArtifactStore store_;
    std::vector<std::unique_ptr<AssetImporter>> importers_{};
};
}  // namespace aeg::tooling
