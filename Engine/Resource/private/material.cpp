/**
 * @file material.cpp
 * @brief Implements texture-material binding creation, updates, and cleanup.
 */
#include "../public/material.hpp"

#include <array>
#include <cmath>
#include <span>

#include "frame_context.hpp"

namespace aeg::resource {
namespace {
[[nodiscard]] foundation::Result destroy_if_valid(rhi::Device& device, rhi::BindingSetHandle& handle) {
    if (!handle.is_valid()) {
        return foundation::Result::success();
    }
    const foundation::Result result = device.destroy_binding_set(handle);
    if (result || result.code == foundation::ResultCode::kNotFound) {
        handle = {};
        return foundation::Result::success();
    }
    return result;
}

[[nodiscard]] foundation::Result destroy_if_valid(rhi::Device& device, rhi::SamplerHandle& handle) {
    if (!handle.is_valid()) {
        return foundation::Result::success();
    }
    const foundation::Result result = device.destroy_sampler(handle);
    if (result || result.code == foundation::ResultCode::kNotFound) {
        handle = {};
        return foundation::Result::success();
    }
    return result;
}

[[nodiscard]] foundation::Result destroy_if_valid(rhi::Device& device, rhi::BindingLayoutHandle& handle) {
    if (!handle.is_valid()) {
        return foundation::Result::success();
    }
    const foundation::Result result = device.destroy_binding_layout(handle);
    if (result || result.code == foundation::ResultCode::kNotFound) {
        handle = {};
        return foundation::Result::success();
    }
    return result;
}

[[nodiscard]] foundation::Result validate_parameter_buffer(rhi::BufferRange parameter_buffer) noexcept {
    if (!parameter_buffer.buffer.is_valid() || parameter_buffer.size < sizeof(TextureMaterialParameters)) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Texture material parameters require a complete uniform buffer range");
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::ResultValue<rhi::BindingSetHandle> create_binding_version(
    rhi::Device& device, const TextureMaterial& material, rhi::TextureViewHandle texture_view,
    rhi::BufferRange parameter_buffer) {
    const auto set_result = device.create_binding_set(
        rhi::BindingSetDesc{material.binding_layout, rhi::BindingSetLifetime::kPersistent,
                            rhi::DebugName{"Resource.TextureMaterial.Version"}});
    if (!set_result) {
        return {set_result.result, {}};
    }

    std::array<rhi::BindingWrite, 3> writes{
        rhi::BindingWrite{rhi::BindingLocation{material.binding_group, 0}, 0,
                          rhi::BindingResource{rhi::BindingType::kSampledTexture, texture_view, {}, {}, {}, {}}},
        rhi::BindingWrite{rhi::BindingLocation{material.binding_group, 1}, 0,
                          rhi::BindingResource{rhi::BindingType::kSampler, {}, {}, {}, material.sampler, {}}},
        rhi::BindingWrite{rhi::BindingLocation{material.binding_group, 2}, 0,
                          rhi::BindingResource{rhi::BindingType::kUniformBuffer, {}, {}, parameter_buffer, {}, {}}},
    };
    const std::span<const rhi::BindingWrite> writes_to_apply =
        parameter_buffer.buffer.is_valid() ? std::span<const rhi::BindingWrite>{writes}
                                            : std::span<const rhi::BindingWrite>{writes}.first(2);
    const foundation::Result update_result = device.update_binding_set(set_result.value, writes_to_apply);
    if (!update_result) {
        rhi::BindingSetHandle failed_set = set_result.value;
        (void)destroy_if_valid(device, failed_set);
        return {update_result, {}};
    }
    return {foundation::Result::success(), set_result.value};
}

[[nodiscard]] foundation::Result update_material_version(
    rhi::Device& device, render_graph::FrameContext& frame_context, TextureMaterial& material,
    rhi::TextureViewHandle texture_view, rhi::BufferRange parameter_buffer, bool update_texture,
    bool update_parameters) {
    if (&frame_context.device() != &device) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Texture material frame update uses a different RHI device");
    }
    if (!frame_context.is_frame_active()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                            "Texture material frame update requires an active frame");
    }
    if (!material.is_valid()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                            "Texture material is not initialized");
    }
    if (update_texture && !texture_view.is_valid()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Texture material requires a valid texture view");
    }
    if (update_parameters) {
        if (const foundation::Result result = validate_parameter_buffer(parameter_buffer); !result) {
            return result;
        }
    }

    const rhi::TextureViewHandle next_texture_view = update_texture ? texture_view : material.texture_view;
    const rhi::BufferRange next_parameter_buffer = update_parameters ? parameter_buffer : material.parameter_buffer;
    if (!next_texture_view.is_valid()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Texture material version requires a valid texture view");
    }

    const auto next_set_result = create_binding_version(device, material, next_texture_view, next_parameter_buffer);
    if (!next_set_result) {
        return next_set_result.result;
    }

    const rhi::BindingSetHandle previous_set = material.binding_set;
    if (const foundation::Result defer_result = frame_context.defer_destroy_binding_set(previous_set); !defer_result) {
        rhi::BindingSetHandle failed_set = next_set_result.value;
        (void)destroy_if_valid(device, failed_set);
        return defer_result;
    }

    material.binding_set = next_set_result.value;
    material.texture_view = next_texture_view;
    material.parameter_buffer = next_parameter_buffer;
    return foundation::Result::success();
}
}  // namespace

bool TextureMaterial::is_valid() const noexcept {
    return texture_view.is_valid() && sampler.is_valid() && binding_layout.is_valid() && binding_set.is_valid();
}

void TextureMaterial::reset() noexcept {
    *this = {};
}

foundation::Result validate_texture_material_parameters(const TextureMaterialParameters& parameters) noexcept {
    for (const float value : parameters.tint) {
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Texture material Tint must be finite and within [0, 1]");
        }
    }
    return foundation::Result::success();
}

foundation::ResultValue<TextureMaterial> create_texture_material(rhi::Device& device, const TextureMaterialDesc& desc) {
    if (!desc.texture_view.is_valid()) {
        return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Texture material requires a valid texture view"),
                {}};
    }

    auto sampler_result = device.create_sampler(desc.sampler);
    if (!sampler_result) {
        return {sampler_result.result, {}};
    }

    TextureMaterial material{};
    material.texture_view = desc.texture_view;
    material.sampler = sampler_result.value;
    material.binding_group = desc.binding_group;

    if (desc.binding_layout.is_valid()) {
        material.binding_layout = desc.binding_layout;
        material.owns_binding_layout = false;
    } else {
        const std::array<rhi::BindingLayoutEntry, 2> layout_entries{
            rhi::BindingLayoutEntry{rhi::BindingLocation{desc.binding_group, 0}, rhi::BindingType::kSampledTexture,
                                    rhi::BindingArrayMode::kSingle, 1, rhi::ShaderStage::kFragment,
                                    rhi::BindingFlags::kNone},
            rhi::BindingLayoutEntry{rhi::BindingLocation{desc.binding_group, 1}, rhi::BindingType::kSampler,
                                    rhi::BindingArrayMode::kSingle, 1, rhi::ShaderStage::kFragment,
                                    rhi::BindingFlags::kNone}};
        const rhi::BindingLayoutDesc layout_desc{layout_entries, desc.name};
        auto layout_result = device.create_binding_layout(layout_desc);
        if (!layout_result) {
            (void)destroy_if_valid(device, material.sampler);
            return {layout_result.result, {}};
        }
        material.binding_layout = layout_result.value;
        material.owns_binding_layout = true;
    }

    auto set_result = device.create_binding_set(
        rhi::BindingSetDesc{material.binding_layout, rhi::BindingSetLifetime::kPersistent, desc.name});
    if (!set_result) {
        if (material.owns_binding_layout) {
            (void)destroy_if_valid(device, material.binding_layout);
        }
        (void)destroy_if_valid(device, material.sampler);
        return {set_result.result, {}};
    }
    material.binding_set = set_result.value;

    const std::array<rhi::BindingWrite, 2> writes{
        rhi::BindingWrite{rhi::BindingLocation{material.binding_group, 0}, 0,
                          rhi::BindingResource{rhi::BindingType::kSampledTexture, desc.texture_view, {}, {}, {}, {}}},
        rhi::BindingWrite{rhi::BindingLocation{material.binding_group, 1}, 0,
                          rhi::BindingResource{rhi::BindingType::kSampler, {}, {}, {}, material.sampler, {}}}};
    const foundation::Result update_result = device.update_binding_set(material.binding_set, writes);
    if (!update_result) {
        (void)destroy_if_valid(device, material.binding_set);
        if (material.owns_binding_layout) {
            (void)destroy_if_valid(device, material.binding_layout);
        }
        (void)destroy_if_valid(device, material.sampler);
        material.texture_view = {};
        material.owns_binding_layout = false;
        return {update_result, {}};
    }

    return {foundation::Result::success(), material};
}

foundation::Result update_texture_material(rhi::Device& device, TextureMaterial& material,
                                           rhi::TextureViewHandle texture_view) {
    // This API is intentionally kept for pre-submit setup. A submitted binding
    // set must be replaced through the frame-versioned API below.
    if (!material.is_valid()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                           "Texture material is not initialized");
    }
    if (!texture_view.is_valid()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "Texture material requires a valid texture view");
    }

    const rhi::BindingWrite write{
        rhi::BindingLocation{material.binding_group, 0}, 0,
        rhi::BindingResource{rhi::BindingType::kSampledTexture, texture_view, {}, {}, {}, {}}};
    const foundation::Result result = device.update_binding_set(material.binding_set, std::span{&write, 1});
    if (result) {
        material.texture_view = texture_view;
    }
    return result;
}

foundation::Result update_texture_material_parameters(rhi::Device& device, TextureMaterial& material,
                                                      rhi::BufferRange parameter_buffer) {
    // This API is intentionally kept for pre-submit setup. A submitted binding
    // set must be replaced through the frame-versioned API below.
    if (!material.is_valid()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                           "Texture material is not initialized");
    }
    if (const foundation::Result result = validate_parameter_buffer(parameter_buffer); !result) {
        return result;
    }

    const rhi::BindingWrite write{
        rhi::BindingLocation{material.binding_group, 2}, 0,
        rhi::BindingResource{rhi::BindingType::kUniformBuffer, {}, {}, parameter_buffer, {}, {}}};
    const foundation::Result result = device.update_binding_set(material.binding_set, std::span{&write, 1});
    if (result) {
        material.parameter_buffer = parameter_buffer;
    }
    return result;
}

foundation::Result update_texture_material_for_frame(rhi::Device& device, render_graph::FrameContext& frame_context,
                                                     TextureMaterial& material, rhi::TextureViewHandle texture_view) {
    return update_material_version(device, frame_context, material, texture_view, {}, true, false);
}

foundation::Result update_texture_material_parameters_for_frame(
    rhi::Device& device, render_graph::FrameContext& frame_context, TextureMaterial& material,
    rhi::BufferRange parameter_buffer) {
    return update_material_version(device, frame_context, material, {}, parameter_buffer, false, true);
}

foundation::Result destroy_texture_material(rhi::Device& device, TextureMaterial& material) {
    const foundation::Result binding_set_result = destroy_if_valid(device, material.binding_set);
    if (!binding_set_result) {
        return binding_set_result;
    }

    foundation::Result first_error = destroy_if_valid(device, material.sampler);
    foundation::Result layout_result = foundation::Result::success();
    if (material.owns_binding_layout) {
        layout_result = destroy_if_valid(device, material.binding_layout);
    } else {
        material.binding_layout = {};
    }
    if (first_error && !layout_result) {
        first_error = layout_result;
    }
    if (!material.sampler.is_valid() && !material.binding_layout.is_valid() && !material.binding_set.is_valid()) {
        material.texture_view = {};
        material.parameter_buffer = {};
        material.owns_binding_layout = false;
    }
    return first_error;
}
}  // namespace aeg::resource
