/**
 * @file cooked_asset.hpp
 * @brief Runtime decoder for the verified cooked asset envelope.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "asset_identity.hpp"

namespace aeg::asset {
/** @brief Runtime-owned cooked payload and its UUID dependencies. */
struct CookedAsset final {
    AssetId id{};
    AssetKind kind{AssetKind::kMesh};
    uint32_t schema_version{};
    std::vector<AssetId> runtime_dependencies{};
    std::vector<std::byte> payload{};
};

/** @brief Encode a versioned cooked envelope owned by the runtime ABI. */
[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_cooked_asset(const CookedAsset& asset);

/** @brief Decode and verify a FlatBuffers cooked envelope without Tooling. */
[[nodiscard]] foundation::ResultValue<CookedAsset> decode_cooked_asset(std::span<const std::byte> bytes);
}  // namespace aeg::asset
