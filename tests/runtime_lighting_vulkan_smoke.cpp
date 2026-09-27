/** @file runtime_lighting_vulkan_smoke.cpp
 *  @brief Validates RuntimeAssetResolver fixed lighting bindings on Vulkan.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <span>

#include "frame_context.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "resource_uploader.hpp"
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

render_graph::RenderGraphBuffer as_graph(pipeline::RenderGraphBuffer value) {
    return {value.index, value.generation, value.graph_id};
}

}  // namespace

int main() {
    using namespace aeg;
    auto device_result = rhi::create_device({rhi::BackendType::kVulkan, "Runtime lighting Vulkan smoke"});
    if (!device_result) {
        std::cerr << "device creation failed: " << device_result.result.message << '\n';
        return 1;
    }
    auto device = std::move(device_result.value);

    const std::array<rhi::BindingLayoutEntry, 1> entries{{{{2, 0},
                                                           rhi::BindingType::kUniformBuffer,
                                                           rhi::BindingArrayMode::kSingle,
                                                           1,
                                                           rhi::ShaderStage::kFragment,
                                                           rhi::BindingFlags::kNone}}};
    const auto layout = device->create_binding_layout({entries, rhi::DebugName{"Runtime.Lighting.Layout"}});
    if (!layout)
        return 2;

    render_graph::FrameContext frame(*device, {.frames_in_flight = 2});
    render_graph::RenderGraph graph(*device);
    resource::ResourceUploader uploader(*device);
    application::runtime::RuntimeAssetResolver resolver;
    resolver.set_upload_context({.device = device.get(),
                                 .frame_context = &frame,
                                 .graph = &graph,
                                 .uploader = &uploader,
                                 .lighting_binding_layout = layout.value});

    renderer::RenderSceneSnapshot directional_snapshot{};
    directional_snapshot.lights.push_back(renderer::RenderLight{
        .light = renderer::RenderLightId{7},
        .type = renderer::RenderLightType::kDirectional,
        .direction = math::Vec3{0.0f, -2.0f, -2.0f},
        .color = math::Vec3{0.8f, 0.6f, 0.4f},
        .intensity = 3.0f,
    });
    const renderer::RenderView view{};

    const auto inactive = resolver.resolve_lighting(directional_snapshot, view);
    if (inactive || inactive.result.code != foundation::ResultCode::kNotReady) {
        std::cerr << "inactive lighting resolve did not fail with kNotReady\n";
        return 3;
    }
    if (!check(frame.begin_frame(), "begin directional frame"))
        return 3;
    const auto first = resolver.resolve_lighting(directional_snapshot, view);
    if (!first || !first.value.is_valid() || first.value.binding_group != 2 ||
        first.value.lighting_size != sizeof(renderer::RenderLightingConstants))
        return 4;
    const auto active_retirement = resolver.retire_gpu_resources();
    if (active_retirement || active_retirement.code != foundation::ResultCode::kInvalidState)
        return 5;
    const auto again = resolver.resolve_lighting(directional_snapshot, view);
    if (!again || again.value.binding_set != first.value.binding_set ||
        again.value.lighting_buffer != first.value.lighting_buffer)
        return 6;
    const auto expected = renderer::make_render_lighting_constants(directional_snapshot.lights);
    if (!expected)
        return 7;
    const auto directional_readback = frame.allocate_readback(sizeof(renderer::RenderLightingConstants), 256);
    rhi::BufferDesc readback_desc{};
    readback_desc.size = directional_readback ? directional_readback.value.buffer_size : 0;
    readback_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    readback_desc.memory = rhi::MemoryClass::kReadback;
    const auto readback_resource = directional_readback
                                       ? graph.import_buffer(directional_readback.value.buffer, readback_desc)
                                       : foundation::ResultValue<render_graph::RenderGraphBuffer>{};
    if (!directional_readback || !readback_resource)
        return 8;
    const auto copy_directional = graph.add_pass(
        "Read directional lighting", rhi::QueueClass::kGraphics,
        [&](render_graph::PassBuilder& pass) {
            if (const auto result = pass.read(as_graph(first.value.lighting_resource), rhi::ResourceUsage::kCopySource,
                                              rhi::PipelineStage::kCopy);
                !result)
                return result;
            pass.set_side_effects();
            return pass.write(readback_resource.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
        },
        [&](render_graph::RenderPassCommandContext& command) {
            return command.copy_buffer(as_graph(first.value.lighting_resource), readback_resource.value, 0,
                                       directional_readback.value.offset, sizeof(renderer::RenderLightingConstants));
        });
    if (!copy_directional)
        return 9;
    if (!check(graph.compile(), "compile directional graph"))
        return 10;
    const auto execution = graph.execute(frame);
    if (!execution) {
        std::cerr << "directional graph execution failed: " << execution.result.message << '\n';
        return 11;
    }
    if (execution.value.executed_pass_count == 0 || execution.value.submitted_batch_count == 0) {
        std::cerr << "directional graph execution submitted no work\n";
        return 11;
    }
    if (execution.value.completion.is_valid() &&
        !check(device->wait(execution.value.completion, std::numeric_limits<uint64_t>::max()),
               "wait directional upload"))
        return 12;
    if (!check(frame.end_frame(), "end directional frame") || !check(device->wait_idle(), "wait directional frame"))
        return 13;
    renderer::RenderLightingConstants directional_values{};
    if (!check(
            frame.read_readback(directional_readback.value, std::as_writable_bytes(std::span{&directional_values, 1})),
            "read directional constants"))
        return 14;
    if (!near(directional_values.directional_direction_intensity.x, 0.0f) ||
        !near(directional_values.directional_direction_intensity.y, -0.7071067f) ||
        !near(directional_values.directional_direction_intensity.z, -0.7071067f) ||
        !near(directional_values.directional_direction_intensity.w, 3.0f) ||
        !near(directional_values.directional_color.x, 0.8f) || !near(directional_values.directional_color.y, 0.6f) ||
        !near(directional_values.directional_color.z, 0.4f))
        return 15;

    graph.reset();
    if (!check(frame.begin_frame(), "begin ambient frame"))
        return 16;
    renderer::RenderSceneSnapshot ambient_snapshot{};
    const auto ambient = resolver.resolve_lighting(ambient_snapshot, view);
    if (!ambient) {
        std::cerr << "ambient lighting resolve failed: " << ambient.result.message << '\n';
        return 17;
    }
    if (!ambient.value.is_valid()) {
        std::cerr << "ambient lighting resource is invalid\n";
        return 17;
    }
    if (ambient.value.lighting_buffer == first.value.lighting_buffer ||
        ambient.value.lighting_resource.generation == first.value.lighting_resource.generation) {
        std::cerr << "ambient lighting did not allocate a new frame resource: buffer="
                  << ambient.value.lighting_buffer.value << "/" << first.value.lighting_buffer.value
                  << " generation=" << ambient.value.lighting_resource.generation << "/"
                  << first.value.lighting_resource.generation << '\n';
        return 17;
    }
    const auto ambient_readback_allocation = frame.allocate_readback(sizeof(renderer::RenderLightingConstants), 256);
    rhi::BufferDesc ambient_readback_desc{};
    ambient_readback_desc.size = ambient_readback_allocation ? ambient_readback_allocation.value.buffer_size : 0;
    ambient_readback_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    ambient_readback_desc.memory = rhi::MemoryClass::kReadback;
    const auto ambient_readback_resource =
        ambient_readback_allocation
            ? graph.import_buffer(ambient_readback_allocation.value.buffer, ambient_readback_desc)
            : foundation::ResultValue<render_graph::RenderGraphBuffer>{};
    if (!ambient_readback_allocation || !ambient_readback_resource)
        return 18;
    const auto copy_ambient = graph.add_pass(
        "Read ambient lighting", rhi::QueueClass::kGraphics,
        [&](render_graph::PassBuilder& pass) {
            if (const auto result = pass.read(as_graph(ambient.value.lighting_resource),
                                              rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy);
                !result)
                return result;
            pass.set_side_effects();
            return pass.write(ambient_readback_resource.value, rhi::ResourceUsage::kCopyDestination,
                              rhi::PipelineStage::kCopy);
        },
        [&](render_graph::RenderPassCommandContext& command) {
            return command.copy_buffer(as_graph(ambient.value.lighting_resource), ambient_readback_resource.value, 0,
                                       ambient_readback_allocation.value.offset,
                                       sizeof(renderer::RenderLightingConstants));
        });
    if (!copy_ambient)
        return 19;
    if (!check(graph.compile(), "compile ambient graph"))
        return 20;
    const auto ambient_execution = graph.execute(frame);
    if (!ambient_execution) {
        std::cerr << "ambient graph execution failed: " << ambient_execution.result.message << '\n';
        return 21;
    }
    if (ambient_execution.value.submitted_batch_count == 0) {
        std::cerr << "ambient graph execution submitted no work\n";
        return 21;
    }
    if (ambient_execution.value.completion.is_valid() &&
        !check(device->wait(ambient_execution.value.completion, std::numeric_limits<uint64_t>::max()),
               "wait ambient upload"))
        return 22;
    if (!check(frame.end_frame(), "end ambient frame") || !check(device->wait_idle(), "wait ambient frame"))
        return 23;
    renderer::RenderLightingConstants ambient_readback{};
    if (!check(frame.read_readback(ambient_readback_allocation.value,
                                   std::as_writable_bytes(std::span{&ambient_readback, 1})),
               "read ambient constants") ||
        !near(ambient_readback.ambient_color.x, 0.05f) || !near(ambient_readback.ambient_color.y, 0.05f) ||
        !near(ambient_readback.ambient_color.z, 0.05f))
        return 24;
    if (!check(resolver.retire_gpu_resources(), "retire lighting resources"))
        return 25;

    if (!check(frame.begin_frame(), "begin invalid-light frame"))
        return 26;
    renderer::RenderSceneSnapshot invalid_snapshot{};
    invalid_snapshot.lights = directional_snapshot.lights;
    invalid_snapshot.lights.push_back(directional_snapshot.lights.front());
    const auto invalid = resolver.resolve_lighting(invalid_snapshot, view);
    if (invalid || invalid.result.code != foundation::ResultCode::kInvalidState)
        return 27;
    if (!check(frame.end_frame(), "end invalid-light frame"))
        return 28;
    if (!check(resolver.retire_gpu_resources(), "retire after invalid-light frame"))
        return 29;
    if (!check(device->destroy_binding_layout(layout.value), "destroy lighting layout"))
        return 30;
    std::cout << "runtime lighting Vulkan resolution passed\n";
    return 0;
}
