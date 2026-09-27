#include "runtime_asset_cache.hpp"

#include <limits>
#include <string>

namespace aeg::application::runtime {
namespace {
foundation::Result kind_error(asset::AssetKind expected, asset::AssetKind actual) {
    return foundation::invalid_argument("Runtime asset kind mismatch: expected " +
                                        std::to_string(static_cast<unsigned>(expected)) + ", got " +
                                        std::to_string(static_cast<unsigned>(actual)));
}
}  // namespace

std::size_t RuntimeAssetCache::KeyHash::operator()(const Key& key) const noexcept {
    uint64_t value = 14695981039346656037ull ^ static_cast<uint8_t>(key.kind);
    for (const auto byte : key.id.value.bytes) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    return static_cast<std::size_t>(value ^ (value >> 32u));
}

uint64_t RuntimeAssetCache::handle_value(asset::AssetId id, asset::AssetKind kind) noexcept {
    (void)id;
    (void)kind;
    if (next_handle_ == 0)
        next_handle_ = 1;
    return next_handle_++;
}

foundation::Result RuntimeAssetCache::mount(asset::AssetPackageReader package) noexcept {
    package_ = std::make_shared<asset::AssetPackageReader>(std::move(package));
    entries_.clear();
    next_handle_ = 1;
    return foundation::Result::success();
}

foundation::ResultValue<RuntimeAssetCache::Entry*> RuntimeAssetCache::begin(asset::AssetId id, asset::AssetKind kind) {
    if (!package_)
        return {foundation::invalid_state("Runtime asset package is not mounted"), {}};
    auto [iterator, inserted] = entries_.try_emplace(Key{id, kind});
    Entry& entry = iterator->second;
    if (!inserted && entry.state == RuntimeAssetState::kFailed)
        return {entry.error, {}};
    if (!inserted && entry.state == RuntimeAssetState::kResident)
        return {foundation::Result::success(), &entry};
    entry.state = RuntimeAssetState::kLoading;
    entry.handle = RuntimeAssetHandle{handle_value(id, kind)};
    return {foundation::Result::success(), &entry};
}

foundation::ResultValue<RuntimeMeshAsset> RuntimeAssetCache::load_mesh(asset::AssetId id) {
    auto started = begin(id, asset::AssetKind::kMesh);
    if (!started)
        return {started.result, {}};
    Entry& entry = *started.value;
    if (entry.state == RuntimeAssetState::kResident)
        return {foundation::Result::success(), RuntimeMeshAsset{entry.handle, entry.state, entry.mesh, entry.vertices}};
    const auto cooked = package_->read_cooked(id);
    if (!cooked) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = cooked.result;
        return {entry.error, {}};
    }
    if (cooked.value.kind != asset::AssetKind::kMesh) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = kind_error(asset::AssetKind::kMesh, cooked.value.kind);
        return {entry.error, {}};
    }
    auto payload = asset::decode_mesh_payload(cooked.value.payload);
    if (!payload) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = payload.result;
        return {entry.error, {}};
    }
    auto vertices = make_renderer_vertices(payload.value);
    if (!vertices) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = vertices.result;
        return {entry.error, {}};
    }
    entry.mesh = std::move(payload.value);
    entry.vertices = std::move(vertices.value);
    entry.state = RuntimeAssetState::kResident;
    return {foundation::Result::success(), RuntimeMeshAsset{entry.handle, entry.state, entry.mesh, entry.vertices}};
}

foundation::ResultValue<RuntimeTextureAsset> RuntimeAssetCache::load_texture(asset::AssetId id) {
    auto started = begin(id, asset::AssetKind::kTexture);
    if (!started)
        return {started.result, {}};
    Entry& entry = *started.value;
    if (entry.state == RuntimeAssetState::kResident)
        return {foundation::Result::success(), RuntimeTextureAsset{entry.handle, entry.state, entry.texture}};
    const auto cooked = package_->read_cooked(id);
    if (!cooked) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = cooked.result;
        return {entry.error, {}};
    }
    if (cooked.value.kind != asset::AssetKind::kTexture) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = kind_error(asset::AssetKind::kTexture, cooked.value.kind);
        return {entry.error, {}};
    }
    auto payload = asset::decode_texture_payload(cooked.value.payload);
    if (!payload) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = payload.result;
        return {entry.error, {}};
    }
    entry.texture = std::move(payload.value);
    entry.state = RuntimeAssetState::kResident;
    return {foundation::Result::success(), RuntimeTextureAsset{entry.handle, entry.state, entry.texture}};
}

foundation::ResultValue<RuntimeFontAsset> RuntimeAssetCache::load_font(asset::AssetId id) {
    auto started = begin(id, asset::AssetKind::kFont);
    if (!started)
        return {started.result, {}};
    Entry& entry = *started.value;
    if (entry.state == RuntimeAssetState::kResident)
        return {foundation::Result::success(), RuntimeFontAsset{entry.handle, entry.state, entry.font}};
    const auto cooked = package_->read_cooked(id);
    if (!cooked) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = cooked.result;
        return {entry.error, {}};
    }
    if (cooked.value.kind != asset::AssetKind::kFont) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = kind_error(asset::AssetKind::kFont, cooked.value.kind);
        return {entry.error, {}};
    }
    auto payload = asset::decode_font_payload(cooked.value.payload);
    if (!payload) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = payload.result;
        return {entry.error, {}};
    }
    entry.font = std::move(payload.value);
    entry.state = RuntimeAssetState::kResident;
    return {foundation::Result::success(), RuntimeFontAsset{entry.handle, entry.state, entry.font}};
}

foundation::ResultValue<RuntimeMaterialAsset> RuntimeAssetCache::load_material(asset::AssetId id) {
    auto started = begin(id, asset::AssetKind::kMaterial);
    if (!started)
        return {started.result, {}};
    Entry& entry = *started.value;
    if (entry.state == RuntimeAssetState::kResident)
        return {foundation::Result::success(), RuntimeMaterialAsset{entry.handle, entry.state, entry.material}};
    const auto cooked = package_->read_cooked(id);
    if (!cooked) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = cooked.result;
        return {entry.error, {}};
    }
    if (cooked.value.kind != asset::AssetKind::kMaterial) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = kind_error(asset::AssetKind::kMaterial, cooked.value.kind);
        return {entry.error, {}};
    }
    auto payload = asset::decode_material_payload(cooked.value.payload);
    if (!payload) {
        entry.state = RuntimeAssetState::kFailed;
        entry.error = payload.result;
        return {entry.error, {}};
    }
    for (const auto& texture : payload.value.textures) {
        if (!load_texture(texture.texture)) {
            entry.state = RuntimeAssetState::kFailed;
            entry.error = foundation::not_found("Material references missing texture " + texture.texture.to_string());
            return {entry.error, {}};
        }
    }
    entry.material = std::move(payload.value);
    entry.state = RuntimeAssetState::kResident;
    return {foundation::Result::success(), RuntimeMaterialAsset{entry.handle, entry.state, entry.material}};
}

foundation::Result RuntimeAssetCache::retry(asset::AssetId id, asset::AssetKind kind) noexcept {
    const auto found = entries_.find(Key{id, kind});
    if (found != entries_.end() && found->second.state == RuntimeAssetState::kFailed)
        entries_.erase(found);
    return foundation::Result::success();
}
}  // namespace aeg::application::runtime
