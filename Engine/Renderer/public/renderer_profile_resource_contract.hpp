/**
 * @file renderer_profile_resource_contract.hpp
 * @brief Stable host-owned resource contracts returned by Renderer profiles.
 *
 * These value contracts are the narrow public seam between Renderer profile
 * resource owners and RenderResourceResolver implementations. Shader
 * reflection, pipeline factories, and profile allocation helpers remain
 * Renderer-private.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "render_graph_types.hpp"
#include "renderer_material_contract.hpp"
#include "renderer_profile_contract.hpp"
#include "renderer_shader_contract.hpp"
#include "result.hpp"
#include "rhi_struct.hpp"

namespace aeg::renderer {
/** @brief GBuffer views and lighting range resolved after graph resource allocation. */
struct RenderDeferredLightingInputs final {
    rhi::TextureViewHandle albedo{};
    rhi::TextureViewHandle normal{};
    rhi::TextureViewHandle depth{};
    rhi::BufferRange lighting{};
    rhi::TextureViewHandle shadow_map{};
    rhi::BufferRange shadow_constants{};
};

/**
 * @brief Host-owned fullscreen lighting resources for the current frame.
 *
 * The callback prepares a fresh group-zero descriptor set and retains it until
 * GPU frame completion. It must not modify a set referenced by an earlier pass.
 */
struct RenderDeferredLightingResource final {
    rhi::PipelineHandle pipeline{};
    std::function<foundation::ResultValue<rhi::BindingSetHandle>(const RenderDeferredLightingInputs&)>
        prepare_inputs{};
    rhi::PipelineHandle shadow_pipeline{};
    RenderMaterialAttachmentContract attachment_contract{};
    RenderMaterialAttachmentContract shadow_pipeline_contract{};
    RendererProfilePsoContract profile_contract{};
    RendererProfilePsoContract shadow_profile_contract{};

    [[nodiscard]] bool is_valid() const noexcept {
        if (!pipeline.is_valid() || !prepare_inputs || !profile_contract.is_valid(RendererProfile::kDeferredLighting))
            return false;
        return !shadow_pipeline.is_valid() || shadow_profile_contract.is_valid(RendererProfile::kDeferredLighting);
    }
};

/** @brief Frame-owned shadow constants and a lazy group-three input descriptor. */
struct RenderShadowResource final {
    rhi::BufferHandle buffer{};
    rhi::BufferDesc buffer_desc{};
    render_graph::RenderGraphBuffer graph_buffer{};
    std::function<foundation::ResultValue<rhi::BindingSetHandle>(rhi::TextureViewHandle)> prepare_inputs{};

    [[nodiscard]] bool is_valid() const noexcept {
        return buffer.is_valid() && buffer_desc.size >= sizeof(RenderShadowConstants) &&
               rhi::has_flag(buffer_desc.capabilities, rhi::BufferCapability::kUniform) && bool(prepare_inputs);
    }

};

/** @brief Host-owned light culling resources imported into the current graph. */
struct RenderLightCullingResource final {
    rhi::PipelineHandle pipeline{};
    rhi::BufferHandle constants{}, lights{};
    rhi::BufferDesc constants_desc{}, lights_desc{};
    render_graph::RenderGraphBuffer constants_graph{}, lights_graph{};
    uint32_t tile_count_x{0}, tile_count_y{0};
    uint64_t cluster_bytes{0};
    /** Compute requires camera depth; forward requires shadow depth only in shadow mode. */
    std::function<foundation::ResultValue<rhi::BindingSetHandle>(rhi::TextureViewHandle, rhi::BufferHandle)>
        prepare_compute{}, prepare_forward{};
    /** Immutable descriptor mode captured together with the uploaded frame constants. */
    bool shadow_enabled{false};
    RendererProfileShaderContract shader_contract{};
    /** Renderer-private frame/device/scene provenance token. */
    std::shared_ptr<const void> origin_token{};

    [[nodiscard]] bool is_valid() const noexcept {
        return pipeline.is_valid() && constants.is_valid() && constants_desc.size != 0 &&
               rhi::has_flag(constants_desc.capabilities, rhi::BufferCapability::kUniform) && lights.is_valid() &&
               lights_desc.size != 0 && rhi::has_flag(lights_desc.capabilities, rhi::BufferCapability::kStorage) &&
               tile_count_x != 0 && tile_count_y != 0 && cluster_bytes != 0 && bool(prepare_compute) &&
               bool(prepare_forward) && shader_contract.is_valid(RendererProfile::kLightCulling);
    }

};

/** @brief Current-frame sky/postprocess shader, constants and descriptor preparation. */
struct RenderPostProcessResource final {
    rhi::PipelineHandle pipeline{};
    rhi::BufferHandle constants{};
    rhi::BufferDesc constants_desc{};
    std::function<foundation::ResultValue<rhi::BindingSetHandle>(rhi::TextureViewHandle, rhi::TextureViewHandle)>
        prepare_inputs{};
    RenderMaterialAttachmentContract attachment_contract{};
    RendererProfilePsoContract profile_contract{};

    [[nodiscard]] bool is_valid() const noexcept {
        return pipeline.is_valid() && constants.is_valid() && constants_desc.size != 0 &&
               rhi::has_flag(constants_desc.capabilities, rhi::BufferCapability::kUniform) &&
               bool(prepare_inputs) && profile_contract.is_valid(RendererProfile::kPostProcess);
    }
};

}  // namespace aeg::renderer
