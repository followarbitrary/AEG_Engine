#pragma once

#include "result.hpp"
#include "rhi.hpp"

namespace aeg::resource {
    struct TextureMaterialDesc {
        rhi::TextureViewHandle texture_view{};
        rhi::SamplerDesc sampler{};
        rhi::DebugName name{};
    };

    struct TextureMaterial {
        rhi::TextureViewHandle texture_view{};
        rhi::SamplerHandle sampler{};
        rhi::BindingLayoutHandle binding_layout{};
        rhi::BindingSetHandle binding_set{};

        [[nodiscard]] bool is_valid() const noexcept {
            return texture_view.is_valid() && sampler.is_valid() && binding_layout.is_valid() &&
                   binding_set.is_valid();
        }

        void reset() noexcept { *this = {}; }
    };

    [[nodiscard]] foundation::ResultValue<TextureMaterial> create_texture_material(
            rhi::Device& device, const TextureMaterialDesc& desc);

    [[nodiscard]] foundation::Result update_texture_material(
            rhi::Device& device, TextureMaterial& material, rhi::TextureViewHandle texture_view);

    [[nodiscard]] foundation::Result destroy_texture_material(
            rhi::Device& device, TextureMaterial& material);
} // namespace aeg::resource
