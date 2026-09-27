/** @file runtime_object_vulkan_smoke.cpp @brief Validates RuntimeAssetResolver object bindings on Vulkan. */
#include <array>
#include <cmath>
#include <iostream>
#include <span>

#include "frame_context.hpp"
#include "renderer.hpp"
#include "rhi.hpp"
#include "runtime_asset_resolver.hpp"

namespace {
using namespace aeg;

bool check(const foundation::Result& result, const char* label) {
    if (!result)
        std::cerr << label << ": " << result.message << '\n';
    return static_cast<bool>(result);
}

bool near(float actual, float expected) {
    return std::abs(actual - expected) < 1.0e-4f;
}

bool check_constants(const renderer::RenderObjectConstants& actual, const renderer::RenderObjectConstants& expected) {
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            if (!near(actual.model_view_projection.at(row, column), expected.model_view_projection.at(row, column)) ||
                !near(actual.normal_matrix.at(row, column), expected.normal_matrix.at(row, column)))
                return false;
    return true;
}
}  // namespace

int main() {
    using namespace aeg;
    auto device_result = rhi::create_device({rhi::BackendType::kVulkan, "Runtime object Vulkan smoke"});
    if (!device_result)
        return 1;
    auto device = std::move(device_result.value);

    const std::array<rhi::BindingLayoutEntry, 1> entries{{{{0, 0},
                                                           rhi::BindingType::kUniformBuffer,
                                                           rhi::BindingArrayMode::kSingle,
                                                           1,
                                                           rhi::ShaderStage::kVertex,
                                                           rhi::BindingFlags::kNone}}};
    const auto layout = device->create_binding_layout({entries, rhi::DebugName{"Runtime.Object.Layout"}});
    if (!layout)
        return 2;

    render_graph::FrameContext frame(*device, {.frames_in_flight = 2});
    application::runtime::RuntimeAssetResolver resolver;
    resolver.set_upload_context(
        {.device = device.get(), .frame_context = &frame, .object_binding_layout = layout.value});

    renderer::RenderInstance instance{};
    instance.object = renderer::RenderObjectId{7};
    instance.world_transform = math::translation({1.0f, 2.0f, 3.0f}) * math::scale({2.0f, 3.0f, 4.0f});
    renderer::RenderView view{};
    view.id = renderer::RenderViewId{11};
    view.camera.view = math::identity();
    view.camera.projection = math::identity();

    renderer::RenderObjectConstants expected{};
    expected.model_view_projection = instance.world_transform;
    expected.normal_matrix = math::identity();
    expected.normal_matrix.at(0, 0) = 0.5f;
    expected.normal_matrix.at(1, 1) = 1.0f / 3.0f;
    expected.normal_matrix.at(2, 2) = 0.25f;
    expected.normal_matrix.at(3, 0) = -0.5f;
    expected.normal_matrix.at(3, 1) = -2.0f / 3.0f;
    expected.normal_matrix.at(3, 2) = -0.75f;

    if (!check(frame.begin_frame(), "begin first frame"))
        return 3;
    const auto first = resolver.resolve_object(instance, view);
    if (!first || !first.value.is_valid() || first.value.binding_group != 0 || first.value.transform_offset != 0 ||
        first.value.transform_size != sizeof(renderer::RenderObjectConstants))
        return 4;
    const auto again = resolver.resolve_object(instance, view);
    if (!again || again.value.transform_buffer != first.value.transform_buffer ||
        again.value.binding_set != first.value.binding_set)
        return 5;
    renderer::RenderObjectConstants readback{};
    if (!check(device->read_buffer(first.value.transform_buffer, 0, std::as_writable_bytes(std::span{&readback, 1})),
               "read first constants") ||
        !check_constants(readback, expected))
        return 6;
    if (!check(frame.end_frame(), "end first frame"))
        return 7;

    if (!check(frame.begin_frame(), "begin second frame"))
        return 8;
    if (!resolver.resolve_object(instance, view))
        return 9;
    if (!check(frame.end_frame(), "end second frame"))
        return 10;

    instance.world_transform = math::translation({5.0f, 6.0f, 7.0f}) * math::scale({3.0f, 4.0f, 5.0f});
    expected.model_view_projection = instance.world_transform;
    expected.normal_matrix = math::identity();
    expected.normal_matrix.at(0, 0) = 1.0f / 3.0f;
    expected.normal_matrix.at(1, 1) = 0.25f;
    expected.normal_matrix.at(2, 2) = 0.2f;
    expected.normal_matrix.at(3, 0) = -5.0f / 3.0f;
    expected.normal_matrix.at(3, 1) = -1.5f;
    expected.normal_matrix.at(3, 2) = -1.4f;
    if (!check(frame.begin_frame(), "begin reused frame"))
        return 11;
    const auto reused = resolver.resolve_object(instance, view);
    if (!reused || reused.value.transform_buffer != first.value.transform_buffer)
        return 12;
    readback = {};
    if (!check(device->read_buffer(reused.value.transform_buffer, 0, std::as_writable_bytes(std::span{&readback, 1})),
               "read reused constants") ||
        !check_constants(readback, expected))
        return 13;

    instance.world_transform = math::scale({0.0f, 1.0f, 1.0f});
    const auto singular = resolver.resolve_object(instance, view);
    if (singular || singular.result.code != foundation::ResultCode::kInvalidArgument)
        return 14;
    if (!check(frame.end_frame(), "end reused frame") || !check(resolver.retire_gpu_resources(), "retire resources"))
        return 15;
    if (!check(device->destroy_binding_layout(layout.value), "destroy object layout"))
        return 16;
    std::cout << "runtime object Vulkan resolution passed\n";
    return 0;
}
