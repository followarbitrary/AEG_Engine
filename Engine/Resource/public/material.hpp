/**
 * @file material.hpp
 * @brief Helpers for creating and managing simple texture materials.
 */
#pragma once

#include <array>

#include "result.hpp"
#include "rhi.hpp"

namespace aeg::render_graph {
class FrameContext;
}

namespace aeg::resource {
/** @brief Stable float4 payload consumed by texture material shaders. */
struct TextureMaterialParameters final {
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
};

/** @brief Inputs required to create a texture-backed material binding set. */
struct TextureMaterialDesc {
    rhi::TextureViewHandle texture_view{};
    rhi::SamplerDesc sampler{};
    rhi::BindingLayoutHandle binding_layout{};
    uint16_t binding_group{0};
    rhi::DebugName name{};
};

/**
 * @brief Runtime handles making up a texture material.
 *
 * The material owns the sampler and binding set. It owns binding_layout
 * only when owns_binding_layout is true; texture_view and parameter_buffer
 * remain caller-owned.
 */
struct TextureMaterial {
    rhi::TextureViewHandle texture_view{};
    rhi::SamplerHandle sampler{};
    rhi::BindingLayoutHandle binding_layout{};
    rhi::BindingSetHandle binding_set{};
    uint16_t binding_group{0};
    /** @brief Optional caller-owned uniform range for material parameters. */
    rhi::BufferRange parameter_buffer{};
    bool owns_binding_layout{false};

    /** @brief Reports whether the material owns a complete current binding version. */
    [[nodiscard]] bool is_valid() const noexcept;

    /** @brief Clears handles without destroying the referenced RHI objects. */
    void reset() noexcept;
};

/** @brief Validates normalized finite Tint values before uploading them. */
[[nodiscard]] foundation::Result validate_texture_material_parameters(
    const TextureMaterialParameters& parameters) noexcept;

/** @brief Creates a texture material and populates its binding set. */
[[nodiscard]] foundation::ResultValue<TextureMaterial> create_texture_material(rhi::Device& device,
                                                                               const TextureMaterialDesc& desc);

/**
 * @brief Replaces the texture view in an existing material binding set.
 *
 * This legacy in-place update is valid only before the binding set is submitted
 * or otherwise referenced by GPU work. Use update_texture_material_for_frame()
 * once the material participates in a frame.
 */
[[nodiscard]] foundation::Result update_texture_material(rhi::Device& device, TextureMaterial& material,
                                                         rhi::TextureViewHandle texture_view);

/**
 * @brief Rebinds the material parameter uniform buffer without rebuilding the binding set.
 *
 * This legacy in-place update is valid only before the binding set is submitted
 * or otherwise referenced by GPU work. Use update_texture_material_parameters_for_frame()
 * for frame-visible updates.
 */
[[nodiscard]] foundation::Result update_texture_material_parameters(rhi::Device& device, TextureMaterial& material,
                                                                    rhi::BufferRange parameter_buffer);

/**
 * @brief Replaces the texture binding with a new descriptor version for one active frame.
 *
 * The previous BindingSet is deferred through the supplied FrameContext and
 * remains valid until that frame slot's GPU completion point has passed.
 */
[[nodiscard]] foundation::Result update_texture_material_for_frame(
    rhi::Device& device, render_graph::FrameContext& frame_context, TextureMaterial& material,
    rhi::TextureViewHandle texture_view);

/**
 * @brief Replaces the parameter binding with a new descriptor version for one active frame.
 *
 * The previous BindingSet is deferred through the supplied FrameContext and
 * the material remains unchanged if descriptor creation or retirement fails.
 */
[[nodiscard]] foundation::Result update_texture_material_parameters_for_frame(
    rhi::Device& device, render_graph::FrameContext& frame_context, TextureMaterial& material,
    rhi::BufferRange parameter_buffer);

/** @brief Destroys material-owned RHI objects and resets the material. */
[[nodiscard]] foundation::Result destroy_texture_material(rhi::Device& device, TextureMaterial& material);
}  // namespace aeg::resource
