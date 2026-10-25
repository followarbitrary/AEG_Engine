/**
 * @file runtime_asset_upload_host.hpp
 * @brief Runtime-private ShaderProgram/RHI upload host contract.
 *
 * This contract belongs to the Runtime host implementation. It is kept out
 * of Runtime public headers so GameRuntime and package consumers cannot
 * acquire Shader, RHI, RenderGraph, or uploader dependencies accidentally.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "renderer.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "shader_contract.hpp"
#include "texture_uploader.hpp"

namespace aeg::application::runtime {

struct RuntimeAssetUploadHost final {
    rhi::Device* device{};
    render_graph::FrameContext* frame_context{};
    render_graph::RenderGraph* graph{};
    resource::ResourceUploader* uploader{};
    resource::TextureUploader* texture_uploader{};
    /** Binding layout and pipeline supplied by the active renderer. */
    rhi::BindingLayoutHandle material_binding_layout{};
    rhi::PipelineHandle material_pipeline{};
    renderer::RenderMaterialAttachmentContract material_attachment_contract{};
    renderer::RenderMaterialShaderContract material_shader_contract{};
    /** Monotonic identity for the descriptor-compatible material host contract. */
    uint64_t material_contract_epoch{1};
    /** Monotonic identity for the concrete Shader/PSO variant cache. */
    uint64_t variant_cache_epoch{1};
    /** Monotonic identity for non-material mesh/texture/object/lighting GPU caches. */
    uint64_t resource_cache_epoch{1};
    /** Borrowed layout used to verify the published shader contract hash. */
    const shader::ShaderPipelineLayout* material_shader_layout{nullptr};
    /** Optional compatible color+depth PSO, owned by the host. */
    std::optional<renderer::RenderMaterialPipelineVariant> depth_attachment_variant{};
    /** Shader UUID selected by the host for the fixed material ABI. */
    foundation::AssetId material_shader{};
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
    renderer::RenderMaterialAttachmentContract depth_pipeline_contract{};
    /** Optional GBuffer pipeline used by deferred rendering. */
    rhi::PipelineHandle gbuffer_pipeline{};
    /** Borrowed host-owned fullscreen deferred-lighting resources. */
    renderer::RenderDeferredLightingResource deferred_lighting{};
    /** Optional per-frame deferred-lighting resolver. */
    std::function<foundation::ResultValue<renderer::RenderDeferredLightingResource>(const renderer::RenderView&)>
        deferred_lighting_resolver{};
    /** Borrowed shadow-resource resolver. */
    std::function<foundation::ResultValue<renderer::RenderShadowResource>(const renderer::RenderShadowView&)>
        shadow_resolver{};
    /** Borrowed clustered-light resolver. */
    std::function<foundation::ResultValue<renderer::RenderLightCullingResource>(
        const renderer::RenderSceneSnapshot&, const renderer::RenderShadowView&)>
        light_culling_resolver{};
    /** Borrowed post-process resolver. */
    std::function<foundation::ResultValue<renderer::RenderPostProcessResource>(const renderer::RenderView&)>
        post_process_resolver{};
    renderer::RenderMaterialAttachmentContract shadow_pipeline_contract{};
    renderer::RenderMaterialAttachmentContract gbuffer_pipeline_contract{};
    renderer::RenderMaterialAttachmentContract clustered_pipeline_contract{};
    renderer::RendererProfilePsoContract clustered_profile_contract{};
    std::optional<renderer::RenderMaterialPipelineVariant> unshadowed_clustered_variant{};

    /** Versioned identity of the host-owned depth-only Shadow/Z-prepass PSO. */
    renderer::RendererProfilePsoContract depth_profile_contract{};
    /** Borrowed reflected depth layout, retained by the host for its PSO lifetime. */
    const shader::ShaderPipelineLayout* depth_shader_layout{nullptr};
    /** Versioned identity of the host-owned GBuffer Shader/layout/ordered MRT PSO. */
    renderer::RendererProfilePsoContract gbuffer_profile_contract{};
    /** Borrowed reflected GBuffer layout, retained alongside the host PSO. */
    const shader::ShaderPipelineLayout* gbuffer_shader_layout{nullptr};

    /** Actual full layout identity of the host-owned forward shadow sampling PSO. */
    renderer::RenderMaterialShaderContract shadow_shader_contract{};
    /** Concrete shadow PSO generation; reflected Shader epoch remains independent. */
    uint64_t shadow_pipeline_cache_epoch{0};
    /** Borrowed reflected shadow layout, retained by the host alongside its PSO. */
    const shader::ShaderPipelineLayout* shadow_shader_layout{nullptr};
};

}  // namespace aeg::application::runtime
