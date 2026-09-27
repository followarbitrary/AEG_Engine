/**
 * @file material_payload.hpp
 * @brief Neutral material payload and checked decoder.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "asset_identity.hpp"
#include "pipeline_authoring_types.hpp"

namespace aeg::asset {
struct MaterialTextureBinding final {
    uint64_t binding_id{};
    AssetId texture{};
};

struct MaterialPayload final {
    uint32_t schema_version{1};
    AssetId shader{};
    std::vector<MaterialTextureBinding> textures{};
    pipeline::ParameterBlock parameters{};
};

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_material_payload(const MaterialPayload& payload);
[[nodiscard]] foundation::ResultValue<MaterialPayload> decode_material_payload(std::span<const std::byte> bytes);
}  // namespace aeg::asset
