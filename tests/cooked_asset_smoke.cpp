/**
 * @file cooked_asset_smoke.cpp
 * @brief Verifies FlatBuffers cooked-asset encoding and input verification.
 */
#include <iostream>

#include "cooked_asset.hpp"

namespace {
bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}  // namespace

int main() {
    using aeg::tooling::AssetId;
    using aeg::tooling::AssetKind;
    using aeg::tooling::CookedAsset;
    using aeg::tooling::CookedAssetCodec;

    const auto asset_id = AssetId::parse("167b4198-8d73-423e-9e15-9f5b6589df51");
    const auto dependency_id = AssetId::parse("7801518b-df32-43c7-b982-02fdc56b03e8");
    if (!require(static_cast<bool>(asset_id) && static_cast<bool>(dependency_id),
                 "cooked asset smoke cannot parse fixture UUIDs")) {
        return 1;
    }

    CookedAsset source{};
    source.id = asset_id.value;
    source.kind = AssetKind::kMaterial;
    source.schema_version = 3;
    source.runtime_dependencies = {dependency_id.value};
    source.payload = {std::byte{0x42}, std::byte{0x19}};

    CookedAssetCodec codec;
    const auto encoded = codec.encode(source);
    if (!require(static_cast<bool>(encoded), "cooked asset smoke cannot encode FlatBuffer")) {
        return 2;
    }
    const auto decoded = codec.decode(encoded.value);
    if (!require(static_cast<bool>(decoded), "cooked asset smoke cannot verify FlatBuffer") ||
        !require(decoded.value.id == source.id && decoded.value.kind == source.kind &&
                     decoded.value.schema_version == source.schema_version && decoded.value.payload == source.payload &&
                     decoded.value.runtime_dependencies == source.runtime_dependencies,
                 "cooked asset smoke decoded data differs")) {
        return 2;
    }

    auto damaged = encoded.value;
    damaged.resize(4);
    if (!require(!codec.decode(damaged), "cooked asset smoke accepted a truncated FlatBuffer")) {
        return 3;
    }
    return 0;
}
