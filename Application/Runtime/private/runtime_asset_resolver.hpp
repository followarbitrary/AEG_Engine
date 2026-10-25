/**
 * @file runtime_asset_resolver.hpp
 * @brief Runtime-only UUID asset lookup and renderer resource boundary.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "asset_handle_table.hpp"
#include "asset_package_reader.hpp"
#include "material.hpp"
#include "mesh_payload.hpp"
#include "renderer.hpp"
#include "runtime_asset_cache.hpp"
#include "texture.hpp"

namespace aeg::application::runtime {
struct RuntimeAssetUploadHost;
}

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
    using UploadContext = RuntimeAssetUploadHost;
    RuntimeAssetResolver();
    explicit RuntimeAssetResolver(asset::AssetPackageReader package) noexcept;
    ~RuntimeAssetResolver();
    RuntimeAssetResolver(const RuntimeAssetResolver&) = delete;
    RuntimeAssetResolver& operator=(const RuntimeAssetResolver&) = delete;
    RuntimeAssetResolver(RuntimeAssetResolver&& other) noexcept;
    RuntimeAssetResolver& operator=(RuntimeAssetResolver&&) = delete;

    [[nodiscard]] static foundation::ResultValue<RuntimeAssetResolver> open(const std::filesystem::path& package_path);

    [[nodiscard]] foundation::Result mount(asset::AssetPackageReader package) noexcept;
    /**
     * @brief Waits for submitted frames and destroys resolver-owned GPU assets.
     *
     * This must be called with no active frame before replacing a package or
     * destroying the host device. It is deliberately explicit so mount cannot
     * silently invalidate resources still referenced by a submitted frame.
     */
    [[nodiscard]] foundation::Result retire_gpu_resources() noexcept;
    [[nodiscard]] const asset::AssetPackageReader* package() const noexcept { return package_.get(); }
    [[nodiscard]] RuntimeAssetCache* cache() noexcept { return cache_.get(); }
    [[nodiscard]] const RuntimeAssetCache* cache() const noexcept { return cache_.get(); }
    [[nodiscard]] asset::AssetHandleTable& handles() noexcept { return handles_; }
    [[nodiscard]] const asset::AssetHandleTable& handles() const noexcept { return handles_; }
    /** @brief Borrows a host-owned upload contract. */
    foundation::Result set_upload_context(RuntimeAssetUploadHost* context) noexcept;
    foundation::Result set_upload_context(UploadContext& context) noexcept {
        return set_upload_context(&context);
    }

    /** @brief Creates a renderer identity for a packaged Mesh UUID. */
    [[nodiscard]] foundation::ResultValue<renderer::MeshAssetId> bind_mesh(foundation::AssetId id);
    /** @brief Creates a renderer identity for a packaged Material UUID. */
    [[nodiscard]] foundation::ResultValue<renderer::MaterialInstanceId> bind_material(foundation::AssetId id);
    /** @brief Validates and decodes a packaged mesh without creating a GPU object. */
    [[nodiscard]] foundation::ResultValue<asset::MeshPayload> decode_mesh(renderer::MeshAssetId id) const;

    [[nodiscard]] foundation::ResultValue<renderer::RenderMeshResource> resolve_mesh(
        renderer::MeshAssetId mesh) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderMaterialResource> resolve_material(
        renderer::MaterialInstanceId material) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderObjectResource> resolve_object(
        const renderer::RenderInstance& instance, const renderer::RenderView& view) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderLightingResource> resolve_lighting(
        const renderer::RenderSceneSnapshot& snapshot, const renderer::RenderView& view) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderDeferredLightingResource> resolve_deferred_lighting(
        const renderer::RenderView& view) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderShadowResource> resolve_shadow(
        const renderer::RenderShadowView& view) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderLightCullingResource> resolve_light_culling(
        const renderer::RenderSceneSnapshot& snapshot, const renderer::RenderShadowView& shadow) const override;
    [[nodiscard]] foundation::ResultValue<renderer::RenderPostProcessResource> resolve_post_process(
        const renderer::RenderView& view) const override;

private:
    struct StableIdHash final {
        [[nodiscard]] std::size_t operator()(uint64_t value) const noexcept {
            value ^= value >> 30u;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27u;
            return static_cast<std::size_t>(value ^ (value >> 31u));
        }
    };

    /** Immutable constants for one frame/view; changed scene lighting gets its own allocation. */
    struct LightingCacheKey final {
        uint64_t frame_number{};
        renderer::RenderViewId view{};
        renderer::RenderLightingConstants constants{};

        [[nodiscard]] bool operator==(const LightingCacheKey& other) const noexcept {
            return frame_number == other.frame_number && view == other.view &&
                constants.directional_direction_intensity == other.constants.directional_direction_intensity &&
                constants.directional_color == other.constants.directional_color &&
                constants.ambient_color == other.constants.ambient_color;
        }
    };

    struct LightingCacheKeyHash final {
        [[nodiscard]] std::size_t operator()(const LightingCacheKey& key) const noexcept {
            std::size_t hash = StableIdHash{}(key.frame_number);
            const auto combine = [&hash](std::size_t value) {
                hash ^= value + static_cast<std::size_t>(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
            };
            combine(StableIdHash{}(key.view.value()));
            for (const auto& vector : {key.constants.directional_direction_intensity,
                                      key.constants.directional_color, key.constants.ambient_color}) {
                // std::hash<float> respects numeric equality, including +0/-0.
                for (float value : {vector.x, vector.y, vector.z, vector.w}) combine(std::hash<float>{}(value));
            }
            return hash;
        }
    };

    [[nodiscard]] static uint64_t renderer_key(foundation::AssetId id) noexcept;
    [[nodiscard]] foundation::ResultValue<foundation::AssetId> mesh_asset(renderer::MeshAssetId id) const;
    [[nodiscard]] foundation::ResultValue<foundation::AssetId> material_asset(renderer::MaterialInstanceId id) const;

    std::shared_ptr<asset::AssetPackageReader> package_{};
    std::unique_ptr<RuntimeAssetCache> cache_{};
    asset::AssetHandleTable handles_{};
    std::unordered_map<uint64_t, foundation::AssetId, StableIdHash> meshes_{};
    std::unordered_map<uint64_t, foundation::AssetId, StableIdHash> materials_{};
    uint64_t next_renderer_id_{1};
    RuntimeAssetUploadHost* upload_context_{nullptr};
    uint64_t upload_resource_cache_epoch_{0};
    const void* upload_device_identity_{nullptr};
    const void* upload_frame_identity_{nullptr};
    const void* upload_graph_identity_{nullptr};
    mutable std::unordered_map<uint64_t, renderer::RenderMeshResource, StableIdHash> gpu_meshes_{};
    mutable std::unordered_map<uint64_t, std::shared_ptr<const void>, StableIdHash> mesh_uploads_{};
    mutable std::unordered_map<uint64_t, resource::GpuTexture, StableIdHash> gpu_textures_{};
    mutable std::unordered_map<uint64_t, render_graph::RenderGraphTexture, StableIdHash> gpu_texture_resources_{};
    mutable std::unordered_map<uint64_t, std::shared_ptr<const void>, StableIdHash> texture_uploads_{};
    mutable std::unordered_map<uint64_t, resource::TextureMaterial, StableIdHash> gpu_materials_{};
    // Contract validation survives GPU resource retirement so a later
    // readiness check can distinguish a previously validated payload from an
    // unknown authored parameter block.
    mutable std::unordered_set<uint64_t, StableIdHash> validated_material_contracts_{};
    mutable std::unordered_map<uint64_t, renderer::RenderMaterialResource, StableIdHash> gpu_material_resources_{};
    /** Candidate resources retained when a GPU material transaction fails. */
    mutable std::unordered_multimap<uint64_t, resource::TextureMaterial, StableIdHash> failed_gpu_materials_{};
    mutable std::unordered_multimap<uint64_t, rhi::BufferHandle, StableIdHash> failed_material_parameter_buffers_{};
    mutable std::unordered_map<uint64_t, renderer::RenderObjectResource, StableIdHash> gpu_objects_{};
    mutable std::unordered_map<uint64_t, uint64_t, StableIdHash> gpu_object_frame_numbers_{};
    mutable std::unordered_map<LightingCacheKey, renderer::RenderLightingResource, LightingCacheKeyHash> gpu_lighting_{};
    mutable std::unordered_map<LightingCacheKey, std::shared_ptr<const void>, LightingCacheKeyHash> lighting_uploads_{};
};

}  // namespace aeg::application::runtime
