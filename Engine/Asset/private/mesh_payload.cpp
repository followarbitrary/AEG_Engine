/** @file mesh_payload.cpp */
#include "mesh_payload.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace aeg::asset {
namespace {
constexpr uint32_t kHeaderBytes = 16;
constexpr uint32_t kStride = 32;
constexpr uint32_t kMaxCount = 1'000'000;
template <typename T>
[[nodiscard]] bool read_le(std::span<const std::byte> bytes, std::size_t offset, T& output) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        return false;
    std::memcpy(&output, bytes.data() + offset, sizeof(T));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    auto* raw = reinterpret_cast<std::byte*>(&output);
    std::reverse(raw, raw + sizeof(T));
#endif
    return true;
}
[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value) != 0;
}
}  // namespace

foundation::ResultValue<MeshPayload> decode_mesh_payload(std::span<const std::byte> bytes) {
    uint32_t vertex_count = 0, index_count = 0, stride = 0, reserved = 0;
    if (!read_le(bytes, 0, vertex_count) || !read_le(bytes, 4, index_count) || !read_le(bytes, 8, stride) ||
        !read_le(bytes, 12, reserved) || reserved != 0 || stride != kStride || vertex_count == 0 || index_count == 0 ||
        index_count % 3 != 0 || vertex_count > kMaxCount || index_count > kMaxCount) {
        return {foundation::invalid_argument("Mesh payload header is invalid"), {}};
    }
    const uint64_t vertex_bytes = static_cast<uint64_t>(vertex_count) * kStride;
    const uint64_t index_bytes = static_cast<uint64_t>(index_count) * sizeof(uint32_t);
    if (vertex_bytes > std::numeric_limits<std::size_t>::max() - kHeaderBytes ||
        index_bytes > std::numeric_limits<std::size_t>::max() - kHeaderBytes - vertex_bytes ||
        bytes.size() != kHeaderBytes + static_cast<std::size_t>(vertex_bytes + index_bytes)) {
        return {foundation::invalid_argument("Mesh payload size does not match its header"), {}};
    }
    MeshPayload result{};
    result.positions.resize(vertex_count);
    result.normals.resize(vertex_count);
    result.uvs.resize(vertex_count);
    result.indices.resize(index_count);
    for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
        const std::size_t base = kHeaderBytes + static_cast<std::size_t>(vertex) * kStride;
        for (std::size_t component = 0; component < 3; ++component) {
            if (!read_le(bytes, base + component * sizeof(float), result.positions[vertex][component]) ||
                !finite(result.positions[vertex][component]) ||
                !read_le(bytes, base + 12 + component * sizeof(float), result.normals[vertex][component]) ||
                !finite(result.normals[vertex][component])) {
                return {foundation::invalid_argument("Mesh payload contains a non-finite position or normal"), {}};
            }
        }
        for (std::size_t component = 0; component < 2; ++component) {
            if (!read_le(bytes, base + 24 + component * sizeof(float), result.uvs[vertex][component]) ||
                !finite(result.uvs[vertex][component])) {
                return {foundation::invalid_argument("Mesh payload contains a non-finite UV"), {}};
            }
        }
    }
    const std::size_t index_base = kHeaderBytes + static_cast<std::size_t>(vertex_bytes);
    for (uint32_t index = 0; index < index_count; ++index) {
        if (!read_le(bytes, index_base + static_cast<std::size_t>(index) * sizeof(uint32_t), result.indices[index]) ||
            result.indices[index] >= vertex_count) {
            return {foundation::invalid_argument("Mesh payload index is outside the vertex range"), {}};
        }
    }
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::asset
