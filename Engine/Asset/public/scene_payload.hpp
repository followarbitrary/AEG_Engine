/**
 * @file scene_payload.hpp
 * @brief Neutral scene bytes plus typed AssetId relocation records.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "asset_identity.hpp"

namespace aeg::asset {
struct SceneAssetReference final {
    std::array<uint8_t, 16> scene_entity_uuid{};
    std::string component_type{};
    uint32_t component_version{};
    uint64_t field_id{};
    AssetId asset{};
    AssetKind kind{AssetKind::kMesh};
};

struct ScenePayload final {
    uint32_t schema_version{1};
    std::vector<std::byte> serialized_scene{};
    std::vector<SceneAssetReference> references{};
};

[[nodiscard]] foundation::ResultValue<ScenePayload> decode_scene_payload(std::span<const std::byte> bytes);
[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_scene_payload(const ScenePayload& payload);
}  // namespace aeg::asset
