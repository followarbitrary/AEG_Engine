/**
 * @file material.hpp
 * @brief Helpers for creating and managing simple texture materials.
 */
#pragma once

#include "result.hpp"
#include "rhi.hpp"

namespace aeg::resource {
    /** @brief Inputs required to create a texture-backed material binding set. */
    struct TextureMaterialDesc {
        rhi::TextureViewHandle texture_view{};
        rhi::SamplerDesc sampler{};
        rhi::BindingLayoutHandle binding_layout{};
        rhi::DebugName name{};
    };

    /**
     * @brief Runtime handles making up a texture material.
     *
     * The material owns the sampler and binding set. It owns binding_layout
     * only when owns_binding_layout is true; texture_view remains caller-owned.
     */
    struct TextureMaterial {
        rhi::TextureViewHandle texture_view{};
        rhi::SamplerHandle sampler{};
        rhi::BindingLayoutHandle binding_layout{};
        rhi::BindingSetHandle binding_set{};
        bool owns_binding_layout{false};

        [[nodiscard]] bool is_valid() const noexcept {
            return texture_view.is_valid() && sampler.is_valid() && binding_layout.is_valid() &&
                   binding_set.is_valid();
        }

        void reset() noexcept { *this = {}; }
    };

    /** @brief Creates a texture material and populates its binding set. */
    [[nodiscard]] foundation::ResultValue<TextureMaterial> create_texture_material(
            rhi::Device& device, const TextureMaterialDesc& desc);

    /** @brief Replaces the texture view in an existing material binding set. */
    [[nodiscard]] foundation::Result update_texture_material(
            rhi::Device& device, TextureMaterial& material, rhi::TextureViewHandle texture_view);

    /** @brief Destroys material-owned RHI objects and resets the material. */
    [[nodiscard]] foundation::Result destroy_texture_material(
            rhi::Device& device, TextureMaterial& material);
} // namespace aeg::resource
