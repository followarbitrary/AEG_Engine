/**
 * @file renderer_material_resource_contract.hpp
 * @brief Stable host-owned GPU material resource value contract.
 *
 * This narrow public seam is consumed by RenderResourceResolver and the
 * Renderer draw adapters. Shader reflection, PSO creation and cache ownership
 * remain private to Renderer and its Runtime host.
 */
#pragma once

#include <cstdint>
#include <optional>

#include "render_graph_types.hpp"
#include "renderer_material_contract.hpp"
#include "rhi_struct.hpp"

namespace aeg::renderer {

/** @brief GPU resources resolved for one material instance. */
struct RenderMaterialResource final {
    /** @brief Graphics pipeline selected by this material instance. */
    rhi::PipelineHandle pipeline{};
    /** @brief Descriptor set exposing material textures and samplers. */
    rhi::BindingSetHandle binding_set{};
    /** @brief Pipeline layout group used when binding binding_set. */
    uint32_t binding_group{1};
    /** @brief Graph texture sampled by the material binding set. */
    render_graph::RenderGraphTexture material_texture{};
    /** @brief Uniform buffer containing material parameters. */
    rhi::BufferHandle material_parameter_buffer{};
    /** @brief Immutable creation contract for material_parameter_buffer. */
    rhi::BufferDesc material_parameter_buffer_desc{};
    /** @brief Graph buffer used to declare material parameter reads. */
    render_graph::RenderGraphBuffer material_parameter_resource{};
    /** @brief Byte offset of the material parameter range. */
    uint64_t material_parameter_offset{0};
    /** @brief Byte size of the material parameter range. */
    uint64_t material_parameter_size{0};
    /** @brief Optional graphics pipeline writing Albedo/Normal/Material GBuffer MRTs. */
    rhi::PipelineHandle gbuffer_pipeline{};
    /** @brief Optional depth-only graphics pipeline used by shadow/Z-prepass. */
    rhi::PipelineHandle depth_pipeline{};
    /** @brief Optional forward pipeline consuming the group-three shadow binding. */
    rhi::PipelineHandle shadow_pipeline{};
    /** @brief Forward pipeline consuming the tile light list and a directional shadow map. */
    rhi::PipelineHandle clustered_pipeline{};
    RenderMaterialAttachmentContract attachment_contract{};

    /** Optional color+depth variant of the same fixed material ABI. */
    std::optional<RenderMaterialPipelineVariant> depth_attachment_variant{};

    /** Fixed attachment ABI of depth_pipeline, independent of the color PSO variants. */
    RenderMaterialAttachmentContract depth_pipeline_contract{};

    /** Actual attachment ABI of the host-owned group-three shadow sampling PSO. */
    RenderMaterialAttachmentContract shadow_pipeline_contract{};
    /** Actual ordered MRT and depth ABI of gbuffer_pipeline. */
    RenderMaterialAttachmentContract gbuffer_pipeline_contract{};
    /** Actual HDR/depth ABI of the shadowed clustered pipeline. */
    RenderMaterialAttachmentContract clustered_pipeline_contract{};
    /** Versioned profile/Shader/PSO identity of the shadowed clustered pipeline. */
    RendererProfilePsoContract clustered_profile_contract{};
    /** Versioned identity of the fixed material shader/layout ABI. */
    RenderMaterialShaderContract shader_contract{};
    /** Monotonic host epoch for all concrete Shader/PSO variants in this resource. */
    uint64_t variant_cache_epoch{1};
    /** Same material ABI with no reflected shadow texture or graph shadow dependency. */
    std::optional<RenderMaterialPipelineVariant> unshadowed_clustered_variant{};

    /** Actual depth-only Shader/layout/PSO identity shared by shadow and Z-prepass. */
    RendererProfilePsoContract depth_profile_contract{};
    /** Actual reflected layout/cache identity and ordered MRT ABI of the GBuffer PSO. */
    RendererProfilePsoContract gbuffer_profile_contract{};

    /** Reflected full layout identity of the selected group-three shadow sampling PSO. */
    RenderMaterialShaderContract shadow_shader_contract{};
    /** Concrete shadow PSO generation, independent of its reflected Shader cache epoch. */
    uint64_t shadow_pipeline_cache_epoch{0};

    /** @brief Reports whether this material can issue a graphics draw. */
    [[nodiscard]] bool is_valid() const noexcept;
};

}  // namespace aeg::renderer
