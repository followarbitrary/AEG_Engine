/**
 * @file renderer_material_contract.hpp
 * @brief Stable host-owned material Shader/PSO attachment contracts.
 *
 * This header is the narrow public boundary for the fixed Renderer material
 * ABI. It carries value metadata used by RenderResourceResolver; shader
 * reflection, pipeline creation and profile resource owners remain private to
 * Renderer and its host implementation.
 */
#pragma once

#include <cstdint>

#include "renderer_attachment_contract.hpp"
#include "renderer_profile_contract.hpp"

namespace aeg::renderer {

inline constexpr uint32_t kRenderMaterialShaderContractVersion = 1u;

/** @brief Versioned identity of the fixed material shader/pipeline-layout ABI. */
struct RenderMaterialShaderContract final {
    uint32_t version{kRenderMaterialShaderContractVersion};
    uint64_t layout_hash{0};
    /** Monotonic ShaderManager cache epoch that owns the reflected layout. */
    uint64_t cache_epoch{1};

    [[nodiscard]] bool is_valid() const noexcept {
        return version == kRenderMaterialShaderContractVersion && layout_hash != 0 && cache_epoch != 0;
    }

    friend bool operator==(const RenderMaterialShaderContract&, const RenderMaterialShaderContract&) = default;
};

/** @brief Host-owned compatible PSO sharing a material shader/layout and descriptors. */
struct RenderMaterialPipelineVariant final {
    rhi::PipelineHandle pipeline{};
    RenderMaterialAttachmentContract attachment_contract{};
    RenderMaterialShaderContract shader_contract{};
    /** Optional Renderer profile identity; required for Forward/clustered variants. */
    RendererProfilePsoContract profile_contract{};
    /** Monotonic host epoch for this concrete variant cache entry. */
    uint64_t cache_epoch{1};

    [[nodiscard]] bool is_valid(RendererProfile expected) const noexcept {
        return pipeline.is_valid() && shader_contract.is_valid() && profile_contract.is_valid(expected) &&
               profile_contract.attachment == attachment_contract && cache_epoch != 0 &&
               profile_contract.cache_epoch == cache_epoch;
    }
};

}  // namespace aeg::renderer
