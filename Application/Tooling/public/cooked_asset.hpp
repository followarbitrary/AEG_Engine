/**
 * @file cooked_asset.hpp
 * @brief FlatBuffers envelope shared by all cooked asset payloads.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "asset_metadata.hpp"

namespace aeg::tooling {
/** @brief One verified cooked payload and its runtime dependency identities. */
struct CookedAsset final {
    AssetId id{};
    AssetKind kind{AssetKind::kMesh};
    uint32_t schema_version{};
    std::vector<AssetId> runtime_dependencies{};
    std::vector<std::byte> payload{};
};

/** @brief Encodes and verifies the versioned FlatBuffers cooked envelope. */
class CookedAssetCodec final {
public:
    [[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode(const CookedAsset& asset) const;
    [[nodiscard]] foundation::ResultValue<CookedAsset> decode(std::span<const std::byte> bytes) const;
};
}  // namespace aeg::tooling
