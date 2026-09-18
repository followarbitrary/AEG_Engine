/** @file cooked_asset_encoder.cpp */
#include "cooked_asset_encoder.hpp"

namespace aeg::tooling {
foundation::ResultValue<std::vector<std::byte>> encode_cooked_asset(const aeg::asset::CookedAsset& asset) {
    return asset::encode_cooked_asset(asset);
}
}  // namespace aeg::tooling
