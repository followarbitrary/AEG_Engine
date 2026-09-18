/**
 * @file cooked_asset_encoder.hpp
 * @brief Tooling entry point for the runtime-owned cooked envelope.
 */
#pragma once

#include <cstddef>
#include <vector>

#include "../../../Engine/Asset/public/cooked_asset.hpp"

namespace aeg::tooling {
[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_cooked_asset(const aeg::asset::CookedAsset& asset);
}  // namespace aeg::tooling
