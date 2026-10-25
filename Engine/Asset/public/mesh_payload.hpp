/**
 * @file mesh_payload.hpp
 * @brief Neutral decoder for the cooked interleaved mesh payload.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "result.hpp"

namespace aeg::asset {
/** @brief CPU-side mesh data independent of Renderer and GPU handles. */
struct MeshPayload final {
    std::vector<std::array<float, 3>> positions{};
    std::vector<std::array<float, 3>> normals{};
    std::vector<std::array<float, 2>> uvs{};
    std::vector<uint32_t> indices{};
};

[[nodiscard]] foundation::ResultValue<MeshPayload> decode_mesh_payload(std::span<const std::byte> bytes);
}  // namespace aeg::asset
