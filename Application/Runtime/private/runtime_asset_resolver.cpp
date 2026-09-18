#include "runtime_asset_resolver.hpp"

#include <cstring>

namespace aeg::application::runtime {
namespace {
template <typename T>
uint64_t stable_value(T value) noexcept {
    return value.value();
}
}  // namespace

RuntimeAssetResolver::RuntimeAssetResolver(asset::AssetPackageReader package) noexcept
    : package_(std::make_shared<asset::AssetPackageReader>(std::move(package))) {}

foundation::ResultValue<RuntimeAssetResolver> RuntimeAssetResolver::open(const std::filesystem::path& package_path) {
    auto package = asset::AssetPackageReader::open(package_path);
    if (!package)
        return {package.result, {}};
    return {foundation::Result::success(), RuntimeAssetResolver{std::move(package.value)}};
}

foundation::Result RuntimeAssetResolver::mount(asset::AssetPackageReader package) noexcept {
    package_ = std::make_shared<asset::AssetPackageReader>(std::move(package));
    meshes_.clear();
    materials_.clear();
    handles_ = asset::AssetHandleTable{};
    return foundation::Result::success();
}

uint64_t RuntimeAssetResolver::renderer_key(asset::AssetId id) noexcept {
    uint64_t value = 14695981039346656037ull;
    for (const uint8_t byte : id.value.bytes) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    return value == 0 ? 1 : value;
}

foundation::ResultValue<renderer::MeshAssetId> RuntimeAssetResolver::bind_mesh(asset::AssetId id) {
    if (!package_)
        return {foundation::invalid_state("Runtime asset package is not mounted"), {}};
    const auto cooked = package_->read_cooked(id);
    if (!cooked || cooked.value.kind != asset::AssetKind::kMesh) {
        return {cooked ? foundation::invalid_argument("Asset is not a mesh") : cooked.result, {}};
    }
    const uint64_t key = renderer_key(id);
    meshes_[key] = id;
    return {foundation::Result::success(), renderer::MeshAssetId{key}};
}

foundation::ResultValue<renderer::MaterialInstanceId> RuntimeAssetResolver::bind_material(asset::AssetId id) {
    if (!package_)
        return {foundation::invalid_state("Runtime asset package is not mounted"), {}};
    const auto cooked = package_->read_cooked(id);
    if (!cooked || cooked.value.kind != asset::AssetKind::kMaterial) {
        return {cooked ? foundation::invalid_argument("Asset is not a material") : cooked.result, {}};
    }
    const uint64_t key = renderer_key(id);
    materials_[key] = id;
    return {foundation::Result::success(), renderer::MaterialInstanceId{key}};
}

foundation::ResultValue<asset::AssetId> RuntimeAssetResolver::mesh_asset(renderer::MeshAssetId id) const {
    const auto found = meshes_.find(stable_value(id));
    if (found == meshes_.end())
        return {foundation::not_found("Mesh renderer identity is not bound"), {}};
    return {foundation::Result::success(), found->second};
}

foundation::ResultValue<asset::AssetId> RuntimeAssetResolver::material_asset(renderer::MaterialInstanceId id) const {
    const auto found = materials_.find(stable_value(id));
    if (found == materials_.end())
        return {foundation::not_found("Material renderer identity is not bound"), {}};
    return {foundation::Result::success(), found->second};
}

foundation::ResultValue<asset::MeshPayload> RuntimeAssetResolver::decode_mesh(renderer::MeshAssetId id) const {
    if (!package_)
        return {foundation::invalid_state("Runtime asset package is not mounted"), {}};
    const auto asset_id = mesh_asset(id);
    if (!asset_id)
        return {asset_id.result, {}};
    const auto cooked = package_->read_cooked(asset_id.value);
    if (!cooked)
        return {cooked.result, {}};
    return asset::decode_mesh_payload(cooked.value.payload);
}

foundation::ResultValue<renderer::RenderMeshResource> RuntimeAssetResolver::resolve_mesh(
    renderer::MeshAssetId mesh) const {
    const auto decoded = decode_mesh(mesh);
    if (!decoded)
        return {decoded.result, {}};
    return {foundation::unsupported("GPU mesh upload is owned by the runtime host"), {}};
}

foundation::ResultValue<renderer::RenderMaterialResource> RuntimeAssetResolver::resolve_material(
    renderer::MaterialInstanceId material) const {
    const auto asset_id = material_asset(material);
    if (!asset_id)
        return {asset_id.result, {}};
    return {foundation::unsupported("GPU material upload is owned by the runtime host"), {}};
}

foundation::ResultValue<renderer::RenderObjectResource> RuntimeAssetResolver::resolve_object(
    const renderer::RenderInstance&, const renderer::RenderView&) const {
    return {foundation::unsupported("Runtime object binding is owned by the frame host"), {}};
}
}  // namespace aeg::application::runtime
