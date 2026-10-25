/**
 * @file renderer_profile_contract.hpp
 * @brief Versioned, backend-neutral Shader/Layout/PSO contracts for Renderer profiles.
 *
 * Profile owners publish these value contracts together with frame resources.
 * Shader reflection, layout allocation, and PSO creation remain private to
 * Renderer and its host; consumers compare only this stable metadata.
 */
#pragma once

#include <cstdint>

#include "renderer_attachment_contract.hpp"

namespace aeg::renderer {

inline constexpr uint32_t kRendererProfileContractVersion = 1u;

/** @brief Renderer profile whose shader/layout contract is being published. */
enum class RendererProfile : uint8_t {
    kUnknown,
    kShadow,
    kGBuffer,
    kDeferredLighting,
    kForwardClustered,
    kPostProcess,
    kLightCulling,
};

/** @brief Versioned shader/layout identity for one Renderer profile. */
struct RendererProfileShaderContract final {
    uint32_t version{kRendererProfileContractVersion};
    RendererProfile profile{RendererProfile::kUnknown};
    uint64_t layout_hash{0};
    /** Monotonic ShaderManager cache epoch that owns the reflected layout. */
    uint64_t cache_epoch{1};

    [[nodiscard]] bool is_valid() const noexcept {
        switch (profile) {
            case RendererProfile::kShadow:
            case RendererProfile::kGBuffer:
            case RendererProfile::kDeferredLighting:
            case RendererProfile::kForwardClustered:
            case RendererProfile::kPostProcess:
            case RendererProfile::kLightCulling:
                return version == kRendererProfileContractVersion && layout_hash != 0 && cache_epoch != 0;
            default:
                return false;
        }
    }

    [[nodiscard]] bool is_valid(RendererProfile expected) const noexcept {
        return is_valid() && profile == expected;
    }

    friend bool operator==(const RendererProfileShaderContract&, const RendererProfileShaderContract&) = default;
};

/** @brief Versioned graphics PSO identity for one Renderer profile. */
struct RendererProfilePsoContract final {
    RendererProfileShaderContract shader{};
    RenderMaterialAttachmentContract attachment{};
    /** Monotonic host epoch for the concrete Shader/PSO variant cache. */
    uint64_t cache_epoch{1};

    [[nodiscard]] bool is_valid(RendererProfile expected) const noexcept {
        if (!shader.is_valid(expected) || !attachment.is_valid() || cache_epoch == 0 ||
            shader.cache_epoch != cache_epoch)
            return false;
        switch (expected) {
            case RendererProfile::kShadow:
                return attachment.is_depth_only();
            case RendererProfile::kGBuffer:
                return attachment.is_gbuffer_attachment();
            default:
                return attachment.is_color_attachment();
        }
    }

    friend bool operator==(const RendererProfilePsoContract&, const RendererProfilePsoContract&) = default;
};

}  // namespace aeg::renderer
