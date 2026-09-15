/**
 * @file asset_metadata_smoke.cpp
 * @brief Verifies stable source and cooked-product metadata identities.
 */
#include <filesystem>
#include <iostream>

#include "asset_metadata.hpp"

namespace {
bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    using aeg::tooling::AssetId;
    using aeg::tooling::AssetKind;
    using aeg::tooling::AssetMeta;
    using aeg::tooling::AssetMetaCodec;
    using aeg::tooling::CookedProduct;
    using aeg::tooling::SourceId;

    const auto source_id = SourceId::parse("169c2f30-3a65-4f6a-8fb8-f062346cca57");
    const auto mesh_id = AssetId::parse("72a9df5e-4ef1-4fc2-a197-0325620b1be5");
    const auto material_id = AssetId::parse("8a103433-4b67-4f4e-9ab8-b8976556ac58");
    if (!require(static_cast<bool>(source_id) && static_cast<bool>(mesh_id) && static_cast<bool>(material_id),
                 "asset metadata smoke cannot parse UUIDs")) {
        return 1;
    }

    AssetMeta meta{};
    meta.source = source_id.value;
    meta.importer = "gltf";
    meta.products = {{mesh_id.value, AssetKind::kMesh, "mesh:body"},
                     {material_id.value, AssetKind::kMaterial, "material:body"}};

    const fs::path file = fs::temp_directory_path() / "aeg_asset_metadata_smoke.meta";
    std::error_code error;
    fs::remove(file, error);

    AssetMetaCodec codec;
    const auto write = codec.save(file, meta);
    const auto loaded = codec.load(file);
    if (!require(static_cast<bool>(write), "asset metadata smoke cannot save meta file") ||
        !require(static_cast<bool>(loaded), "asset metadata smoke cannot load meta file") ||
        !require(loaded.value.source == meta.source, "asset metadata smoke source UUID did not round-trip") ||
        !require(loaded.value.products == meta.products, "asset metadata smoke products did not round-trip")) {
        fs::remove(file, error);
        return 2;
    }

    meta.products.push_back({mesh_id.value, AssetKind::kTexture, "texture:duplicate"});
    const auto duplicate_write = codec.save(file, meta);
    const auto retained = codec.load(file);
    if (!require(!duplicate_write, "asset metadata smoke accepted duplicate product UUIDs") ||
        !require(static_cast<bool>(retained) && retained.value.products.size() == 2U,
                 "asset metadata smoke did not retain the prior valid metadata")) {
        fs::remove(file, error);
        return 3;
    }

    fs::remove(file, error);
    return 0;
}
