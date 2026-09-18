/**
 * @file cooked_asset.hpp
 * @brief FlatBuffers envelope shared by all cooked asset payloads.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../Engine/Asset/public/cooked_asset.hpp"
#include "asset_metadata.hpp"

namespace aeg::tooling {
/** @brief Compatibility name for the Engine-owned cooked asset ABI. */
using CookedAsset = asset::CookedAsset;

/** @brief Tooling facade for the Engine-owned cooked envelope codec. */
class CookedAssetCodec final {
public:
    [[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode(const CookedAsset& asset) const;
    [[nodiscard]] foundation::ResultValue<CookedAsset> decode(std::span<const std::byte> bytes) const;
};
}  // namespace aeg::tooling
