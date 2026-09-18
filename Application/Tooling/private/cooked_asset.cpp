/**
 * @file cooked_asset.cpp
 * @brief Tooling facade over the Engine-owned cooked asset codec.
 */
#include "cooked_asset.hpp"

namespace aeg::tooling {

foundation::ResultValue<std::vector<std::byte>> CookedAssetCodec::encode(const CookedAsset& asset) const {
    return asset::encode_cooked_asset(asset);
}

foundation::ResultValue<CookedAsset> CookedAssetCodec::decode(std::span<const std::byte> bytes) const {
    return asset::decode_cooked_asset(bytes);
}

}  // namespace aeg::tooling
