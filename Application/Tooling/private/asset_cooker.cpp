/**
 * @file asset_cooker.cpp
 * @brief Raw-asset cooker implementation.
 */
#include "asset_cooker.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "cgltf_mesh_importer.hpp"
#include "font_importer.hpp"
#include "material_asset_importer.hpp"
#include "stb_texture_importer.hpp"

namespace aeg::tooling {
AssetCooker::AssetCooker(std::filesystem::path cooked_cache_root) : store_(std::move(cooked_cache_root)) {}

foundation::Result AssetCooker::register_default_importers() {
    if (const foundation::Result result = register_importer(std::make_unique<CgltfMeshImporter>()); !result) {
        return result;
    }
    if (const foundation::Result result = register_importer(std::make_unique<StbTextureImporter>()); !result) {
        return result;
    }
    if (const foundation::Result result = register_importer(std::make_unique<MaterialAssetImporter>()); !result) {
        return result;
    }
    return register_importer(std::make_unique<FontImporter>());
}

foundation::Result AssetCooker::register_importer(std::unique_ptr<AssetImporter> importer) {
    if (importer == nullptr) {
        return foundation::invalid_argument("cannot register a null asset importer");
    }
    importers_.push_back(std::move(importer));
    return foundation::Result::success();
}

foundation::ResultValue<CookedArtifactBundle> AssetCooker::cook(const std::filesystem::path& source) const {
    if (source.empty()) {
        return {foundation::invalid_argument("cannot cook an empty source path"), {}};
    }
    const auto meta = AssetMetaCodec{}.load(source.string() + ".meta");
    if (!meta) {
        return {meta.result, {}};
    }
    const auto importer = std::find_if(importers_.begin(), importers_.end(),
                                       [&source](const auto& candidate) { return candidate->supports(source); });
    if (importer == importers_.end()) {
        return {foundation::unsupported("no registered importer supports this source file"), {}};
    }
    const auto decoded = (*importer)->cook(source, meta.value);
    if (!decoded) {
        return {decoded.result, {}};
    }
    if (decoded.value.empty()) {
        return {foundation::invalid_argument("asset importer returned no cooked products"), {}};
    }

    std::vector<CookedArtifact> artifacts;
    artifacts.reserve(decoded.value.size());
    for (const CookedAsset& asset : decoded.value) {
        const auto declared = std::find_if(
            meta.value.products.begin(), meta.value.products.end(),
            [&asset](const CookedProduct& product) { return product.id == asset.id && product.kind == asset.kind; });
        if (declared == meta.value.products.end()) {
            return {foundation::invalid_argument("asset importer emitted a product absent from source metadata"), {}};
        }
        const auto encoded = CookedAssetCodec{}.encode(asset);
        if (!encoded) {
            return {encoded.result, {}};
        }
        artifacts.push_back({asset.id, asset.kind, asset.schema_version, encoded.value});
    }
    if (artifacts.size() != meta.value.products.size()) {
        return {foundation::invalid_argument("asset importer did not emit every product declared by source metadata"),
                {}};
    }
    return store_.publish(meta.value.source, artifacts);
}
}  // namespace aeg::tooling
