#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "asset_package_reader.hpp"
#include "font_payload.hpp"
#include "material_payload.hpp"
#include "mesh_payload.hpp"
#include "runtime_mesh_vertex.hpp"
#include "texture_payload.hpp"

namespace aeg::application::runtime {

enum class RuntimeAssetState : uint8_t { kUnloaded, kLoading, kResident, kRetiring, kFailed };

struct RuntimeAssetHandle final {
    uint64_t value{0};
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
    auto operator<=>(const RuntimeAssetHandle&) const = default;
};

struct RuntimeMeshAsset final {
    RuntimeAssetHandle handle{};
    RuntimeAssetState state{RuntimeAssetState::kUnloaded};
    asset::MeshPayload payload{};
    std::vector<RuntimeMeshVertex> vertices{};
};

struct RuntimeMaterialAsset final {
    RuntimeAssetHandle handle{};
    RuntimeAssetState state{RuntimeAssetState::kUnloaded};
    asset::MaterialPayload payload{};
};

struct RuntimeTextureAsset final {
    RuntimeAssetHandle handle{};
    RuntimeAssetState state{RuntimeAssetState::kUnloaded};
    asset::TexturePayload payload{};
};

/** @brief CPU-resident runtime font payload; atlas rasterization remains host-owned. */
struct RuntimeFontAsset final {
    RuntimeAssetHandle handle{};
    RuntimeAssetState state{RuntimeAssetState::kUnloaded};
    asset::FontPayload payload{};

    /** Returns an atlas-neutral view consumed by a host rasterizer. */
    [[nodiscard]] std::span<const std::byte> face_bytes() const noexcept {
        return {payload.font_bytes.data(), payload.font_bytes.size()};
    }
    [[nodiscard]] uint32_t face_index() const noexcept { return payload.face_index; }
};

/** CPU side runtime cache. GPU ownership is attached by a host with a live frame/device. */
class RuntimeAssetCache final {
public:
    RuntimeAssetCache() = default;
    explicit RuntimeAssetCache(std::shared_ptr<asset::AssetPackageReader> package) noexcept
        : package_(std::move(package)) {}

    [[nodiscard]] foundation::Result mount(asset::AssetPackageReader package) noexcept;
    [[nodiscard]] foundation::ResultValue<RuntimeMeshAsset> load_mesh(foundation::AssetId id);
    [[nodiscard]] foundation::ResultValue<RuntimeMaterialAsset> load_material(foundation::AssetId id);
    [[nodiscard]] foundation::ResultValue<RuntimeTextureAsset> load_texture(foundation::AssetId id);
    [[nodiscard]] foundation::ResultValue<RuntimeFontAsset> load_font(foundation::AssetId id);
    [[nodiscard]] foundation::Result retry(foundation::AssetId id, foundation::AssetKind kind) noexcept;
    [[nodiscard]] std::size_t entry_count() const noexcept { return entries_.size(); }

private:
    struct Key final {
        foundation::AssetId id{};
        foundation::AssetKind kind{foundation::AssetKind::kMesh};
        auto operator==(const Key&) const -> bool = default;
    };
    struct KeyHash final {
        std::size_t operator()(const Key& key) const noexcept;
    };
    struct Entry final {
        RuntimeAssetState state{RuntimeAssetState::kUnloaded};
        RuntimeAssetHandle handle{};
        foundation::Result error{};
        asset::MeshPayload mesh{};
        std::vector<RuntimeMeshVertex> vertices{};
        asset::MaterialPayload material{};
        asset::TexturePayload texture{};
        asset::FontPayload font{};
    };

    [[nodiscard]] foundation::ResultValue<Entry*> begin(foundation::AssetId id, foundation::AssetKind kind);
    [[nodiscard]] uint64_t handle_value(foundation::AssetId id, foundation::AssetKind kind) noexcept;

    std::shared_ptr<asset::AssetPackageReader> package_{};
    std::unordered_map<Key, Entry, KeyHash> entries_{};
    uint64_t next_handle_{1};
};
}  // namespace aeg::application::runtime
