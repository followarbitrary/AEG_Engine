/**
 * @file runtime_asset_resolver.hpp
 * @brief Runtime-only UUID asset lookup and renderer resource boundary.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>

#include "asset_handle_table.hpp"
#include "asset_package_reader.hpp"
#include "material.hpp"
#include "mesh_payload.hpp"
#include "renderer.hpp"
#include "resource_uploader.hpp"
#include "runtime_asset_cache.hpp"
#include "shader_contract.hpp"
#include "texture_uploader.hpp"

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
    struct UploadContext final {
        rhi::Device* device{};
        render_graph::FrameContext* frame_context{};
        render_graph::RenderGraph* graph{};
        resource::ResourceUploader* uploader{};
        resource::TextureUploader* texture_uploader{};
        /** Binding layout and pipeline supplied by the active renderer. */
        rhi::BindingLayoutHandle material_binding_layout{};
        rhi::PipelineHandle material_pipeline{};
        /** Shader UUID selected by the host for the fixed material ABI. */
        asset::AssetId material_shader{};
        /** Borrowed reflection contract for the host material shader. */
        const shader::ShaderProgram* material_shader_program{nullptr};
        rhi::SamplerDesc material_sampler{};
        uint16_t material_binding_group{1};
        /** Binding layout for RenderObjectConstants at group zero/binding zero. */
        rhi::BindingLayoutHandle object_binding_layout{};
        /** Binding layout for RenderLightingConstants at group two/binding zero. */
        rhi::BindingLayoutHandle lighting_binding_layout{};
        /** Optional depth/shadow pipeline published with the fixed material ABI. */
        rhi::PipelineHandle shadow_pipeline{};
        /** Optional clustered-forward pipeline published with the fixed material ABI. */
        rhi::PipelineHandle clustered_pipeline{};
        /** Optional depth-only pipeline used by Z-prepass and shadows. */
        rhi::PipelineHandle depth_pipeline{};
        /** Optional GBuffer pipeline used by deferred rendering. */
        rhi::PipelineHandle gbuffer_pipeline{};
        /**
         * Borrowed host-owned fullscreen deferred-lighting resources. The
         * callback must create a fresh frame-safe group-zero descriptor set for
         * each invocation and keep any captured state alive until GPU use ends.
         */
        renderer::RenderDeferredLightingResource deferred_lighting{};
        /**
         * Optional per-frame deferred-lighting resolver. When supplied it
         * takes precedence over the legacy borrowed `deferred_lighting`
         * value so the host can allocate a fresh frame-safe descriptor set
         * for each graph build.
         */
        std::function<foundation::ResultValue<renderer::RenderDeferredLightingResource>(const renderer::RenderView&)>
            deferred_lighting_resolver{};
        /**
         * Borrowed shadow-resource resolver. The callback and any captured
         * host state are retained by the resolver until this context is
         * replaced; the resolver never destroys the returned GPU handles.
         */
        std::function<foundation::ResultValue<renderer::RenderShadowResource>(const renderer::RenderShadowView&)>
            shadow_resolver{};
        /**
         * Borrowed clustered-light resolver. Captured host state must remain
         * valid until submitted GPU work finishes; ownership stays with host.
         */
        std::function<foundation::ResultValue<renderer::RenderLightCullingResource>(
            const renderer::RenderSceneSnapshot&, const renderer::RenderShadowView&)>
            light_culling_resolver{};
        /**
         * Borrowed post-process resolver. Captured host state remains alive
         * through GPU use and is never retired by RuntimeAssetResolver.
         */
        std::function<foundation::ResultValue<renderer::RenderPostProcessResource>(const renderer::RenderView&)>
            post_process_resolver{};
    };
    RuntimeAssetResolver() = default;
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
    void set_upload_context(UploadContext context) noexcept { upload_context_ = context; }

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

    [[nodiscard]] static uint64_t renderer_key(asset::AssetId id) noexcept;
    [[nodiscard]] foundation::ResultValue<asset::AssetId> mesh_asset(renderer::MeshAssetId id) const;
    [[nodiscard]] foundation::ResultValue<asset::AssetId> material_asset(renderer::MaterialInstanceId id) const;

    std::shared_ptr<asset::AssetPackageReader> package_{};
    std::unique_ptr<RuntimeAssetCache> cache_{};
    asset::AssetHandleTable handles_{};
    std::unordered_map<uint64_t, asset::AssetId, StableIdHash> meshes_{};
    std::unordered_map<uint64_t, asset::AssetId, StableIdHash> materials_{};
    uint64_t next_renderer_id_{1};
    UploadContext upload_context_{};
    mutable std::unordered_map<uint64_t, renderer::RenderMeshResource, StableIdHash> gpu_meshes_{};
    mutable std::unordered_map<uint64_t, resource::GpuTexture, StableIdHash> gpu_textures_{};
    mutable std::unordered_map<uint64_t, pipeline::RenderGraphTexture, StableIdHash> gpu_texture_resources_{};
    mutable std::unordered_map<uint64_t, resource::TextureMaterial, StableIdHash> gpu_materials_{};
    mutable std::unordered_map<uint64_t, renderer::RenderMaterialResource, StableIdHash> gpu_material_resources_{};
    /** Candidate resources retained when a GPU material transaction fails. */
    mutable std::unordered_multimap<uint64_t, resource::TextureMaterial, StableIdHash> failed_gpu_materials_{};
    mutable std::unordered_multimap<uint64_t, rhi::BufferHandle, StableIdHash> failed_material_parameter_buffers_{};
    mutable std::unordered_map<uint64_t, renderer::RenderObjectResource, StableIdHash> gpu_objects_{};
    mutable std::unordered_map<uint64_t, uint64_t, StableIdHash> gpu_object_frame_numbers_{};
    mutable std::unordered_map<uint64_t, renderer::RenderLightingResource, StableIdHash> gpu_lighting_{};
};

}  // namespace aeg::application::runtime
