/**
 * @file runtime_asset_resolver.hpp
 * @brief Runtime-only UUID asset lookup and renderer resource boundary.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>

#include "asset_handle_table.hpp"
#include "asset_package_reader.hpp"
#include "mesh_payload.hpp"
#include "renderer.hpp"

namespace aeg::application::runtime {

/**
 * @brief Resolves packaged UUIDs without exposing Tooling or source metadata.
 *
 * GPU upload is intentionally supplied by the host. The resolver still owns
 * the session-local handle table and validates every packaged payload before a
 * renderer-facing lookup can succeed.
 */
class RuntimeAssetResolver final : public renderer::RenderResourceResolver {
public:
    RuntimeAssetResolver() = default;
    explicit RuntimeAssetResolver(asset::AssetPackageReader package) noexcept;

    [[nodiscard]] static foundation::ResultValue<RuntimeAssetResolver> open(const std::filesystem::path& package_path);

    [[nodiscard]] foundation::Result mount(asset::AssetPackageReader package) noexcept;
    [[nodiscard]] const asset::AssetPackageReader* package() const noexcept { return package_.get(); }
    [[nodiscard]] asset::AssetHandleTable& handles() noexcept { return handles_; }
    [[nodiscard]] const asset::AssetHandleTable& handles() const noexcept { return handles_; }

    /** @brief Creates a renderer identity for a packaged Mesh UUID. */
    [[nodiscard]] foundation::ResultValue<renderer::MeshAssetId> bind_mesh(asset::AssetId id);
    /** @brief Creates a renderer identity for a packaged Material UUID. */
    [[nodiscard]] foundation::ResultValue<renderer::MaterialInstanceId> bind_material(asset::AssetId id);
    /** @brief Validates and decodes a packaged mesh without creating a GPU object. */
    [[nodiscard]] foundation::ResultValue<asset::MeshPayload> decode_mesh(renderer::MeshAssetId id) const;

    [[nodiscard]] foundation::ResultValue<renderer::RenderMeshResource> resolve_mesh(
        renderer::MeshAssetId mesh) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderMaterialResource> resolve_material(
        renderer::MaterialInstanceId material) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderObjectResource> resolve_object(
        const renderer::RenderInstance& instance, const renderer::RenderView& view) const override;

private:
    struct StableIdHash final {
        [[nodiscard]] std::size_t operator()(uint64_t value) const noexcept {
            value ^= value >> 30u;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27u;
            return static_cast<std::size_t>(value ^ (value >> 31u));
        }
    };

    [[nodiscard]] static uint64_t renderer_key(asset::AssetId id) noexcept;
    [[nodiscard]] foundation::ResultValue<asset::AssetId> mesh_asset(renderer::MeshAssetId id) const;
    [[nodiscard]] foundation::ResultValue<asset::AssetId> material_asset(renderer::MaterialInstanceId id) const;

    std::shared_ptr<asset::AssetPackageReader> package_{};
    asset::AssetHandleTable handles_{};
    std::unordered_map<uint64_t, asset::AssetId, StableIdHash> meshes_{};
    std::unordered_map<uint64_t, asset::AssetId, StableIdHash> materials_{};
};

}  // namespace aeg::application::runtime
