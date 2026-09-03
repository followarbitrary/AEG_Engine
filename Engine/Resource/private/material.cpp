#include "../public/material.hpp"

#include <array>
#include <span>

namespace aeg::resource {
    namespace {
        [[nodiscard]] foundation::Result destroy_if_valid(
                rhi::Device& device, rhi::BindingSetHandle& handle) {
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

        [[nodiscard]] foundation::Result destroy_if_valid(
                rhi::Device& device, rhi::SamplerHandle& handle) {
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

        [[nodiscard]] foundation::Result destroy_if_valid(
                rhi::Device& device, rhi::BindingLayoutHandle& handle) {
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
    } // namespace

    foundation::ResultValue<TextureMaterial> create_texture_material(
            rhi::Device& device, const TextureMaterialDesc& desc) {
        if (!desc.texture_view.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                 "Texture material requires a valid texture view"), {}};
        }

        auto sampler_result = device.create_sampler(desc.sampler);
        if (!sampler_result) {
            return {sampler_result.result, {}};
        }

        TextureMaterial material{};
        material.texture_view = desc.texture_view;
        material.sampler = sampler_result.value;

        const std::array<rhi::BindingLayoutEntry, 2> layout_entries{
                rhi::BindingLayoutEntry{
                        rhi::BindingLocation{0, 0},
                        rhi::BindingType::kSampledTexture,
                        rhi::BindingArrayMode::kSingle,
                        1,
                        rhi::ShaderStage::kFragment,
                        rhi::BindingFlags::kNone
                },
                rhi::BindingLayoutEntry{
                        rhi::BindingLocation{0, 1},
                        rhi::BindingType::kSampler,
                        rhi::BindingArrayMode::kSingle,
                        1,
                        rhi::ShaderStage::kFragment,
                        rhi::BindingFlags::kNone
                }
        };
        const rhi::BindingLayoutDesc layout_desc{layout_entries, desc.name};
        auto layout_result = device.create_binding_layout(layout_desc);
        if (!layout_result) {
            (void) destroy_if_valid(device, material.sampler);
            return {layout_result.result, {}};
        }
        material.binding_layout = layout_result.value;

        auto set_result = device.create_binding_set(material.binding_layout);
        if (!set_result) {
            (void) destroy_if_valid(device, material.binding_layout);
            (void) destroy_if_valid(device, material.sampler);
            return {set_result.result, {}};
        }
        material.binding_set = set_result.value;

        const std::array<rhi::BindingWrite, 2> writes{
                rhi::BindingWrite{
                        rhi::BindingLocation{0, 0},
                        0,
                        rhi::BindingResource{
                                rhi::BindingType::kSampledTexture,
                                desc.texture_view,
                                {},
                                {},
                                {},
                                {}
                        }
                },
                rhi::BindingWrite{
                        rhi::BindingLocation{0, 1},
                        0,
                        rhi::BindingResource{
                                rhi::BindingType::kSampler,
                                {},
                                {},
                                {},
                                material.sampler,
                                {}
                        }
                }
        };
        const foundation::Result update_result = device.update_binding_set(material.binding_set, writes);
        if (!update_result) {
            (void) destroy_if_valid(device, material.binding_set);
            (void) destroy_if_valid(device, material.binding_layout);
            (void) destroy_if_valid(device, material.sampler);
            material.texture_view = {};
            return {update_result, {}};
        }

        return {foundation::Result::success(), material};
    }

    foundation::Result update_texture_material(
            rhi::Device& device, TextureMaterial& material, rhi::TextureViewHandle texture_view) {
        if (!material.is_valid()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "Texture material is not initialized");
        }
        if (!texture_view.is_valid()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Texture material requires a valid texture view");
        }

        const rhi::BindingWrite write{
                rhi::BindingLocation{0, 0},
                0,
                rhi::BindingResource{
                        rhi::BindingType::kSampledTexture,
                        texture_view,
                        {},
                        {},
                        {},
                        {}
                }
        };
        const foundation::Result result = device.update_binding_set(material.binding_set, std::span{&write, 1});
        if (result) {
            material.texture_view = texture_view;
        }
        return result;
    }

    foundation::Result destroy_texture_material(rhi::Device& device, TextureMaterial& material) {
        const foundation::Result binding_set_result = destroy_if_valid(device, material.binding_set);
        if (!binding_set_result) {
            return binding_set_result;
        }

        foundation::Result first_error = destroy_if_valid(device, material.sampler);
        const foundation::Result layout_result = destroy_if_valid(device, material.binding_layout);
        if (first_error && !layout_result) {
            first_error = layout_result;
        }
        if (!material.sampler.is_valid() && !material.binding_layout.is_valid() &&
            !material.binding_set.is_valid()) {
            material.texture_view = {};
        }
        return first_error;
    }
} // namespace aeg::resource
