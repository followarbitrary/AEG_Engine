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

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "renderer.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"

namespace {
using namespace aeg;

bool check(const foundation::Result& result, const char* label) {
    if (!result)
        std::cerr << label << ": " << result.message << '\n';
    return static_cast<bool>(result);
}

bool near(float actual, float expected) {
    return std::isfinite(actual) && std::abs(actual - expected) < 1.0e-4f;
}

foundation::ResultValue<render_graph::FrameAllocation> add_readback(
    render_graph::FrameContext& frame, render_graph::RenderGraph& graph,
    const renderer::RenderLightingResource& lighting, const char* label) {
    const auto allocation = frame.allocate_readback(sizeof(renderer::RenderLightingConstants), 256);
    if (!allocation) return allocation;
    rhi::BufferDesc desc{};
    desc.size = allocation.value.buffer_size;
    desc.capabilities = rhi::BufferCapability::kTransferDestination;
    desc.memory = rhi::MemoryClass::kReadback;
    desc.initial_usage = rhi::ResourceUsage::kHostRead;
    desc.initial_stages = rhi::PipelineStage::kHost;
    desc.initial_queue = rhi::QueueClass::kCopy;
    const auto imported = graph.import_buffer(allocation.value.buffer, desc);
    if (!imported) return {imported.result, {}};
    const auto pass = graph.add_pass(
        label, rhi::QueueClass::kCopy,
        [lighting, resource = imported.value, range = allocation.value](render_graph::PassBuilder& pass) {
            if (const auto result = pass.read(lighting.lighting_resource, rhi::ResourceUsage::kCopySource,
                                              rhi::PipelineStage::kCopy, rhi::ShaderStage::kNone,
                                              {lighting.lighting_offset, lighting.lighting_size}); !result)
                return result;
            pass.set_side_effects();
            return pass.write(resource, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy, rhi::ShaderStage::kNone,
                              {range.offset, range.size});
        },
        [lighting, resource = imported.value, range = allocation.value](render_graph::RenderPassCommandContext& command) {
            return command.copy_buffer(lighting.lighting_resource, resource, lighting.lighting_offset,
                                       range.offset, sizeof(renderer::RenderLightingConstants));
        });
    return {pass.result, allocation.value};
}

bool matches(const renderer::RenderLightingConstants& actual, const renderer::RenderLightingConstants& expected) {
    const auto components = [](const renderer::RenderLightingConstants& value) {
        const auto& d = value.directional_direction_intensity;
        const auto& c = value.directional_color;
        const auto& a = value.ambient_color;
        return std::array{d.x, d.y, d.z, d.w, c.x, c.y, c.z, c.w, a.x, a.y, a.z, a.w};
    };
    const auto values = components(actual), reference = components(expected);
    for (size_t i = 0; i < values.size(); ++i)
        if (!near(values[i], reference[i])) return false;
    return true;
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
    application::runtime::RuntimeAssetResolver::UploadContext upload_context{
        .device = device.get(),
        .frame_context = &frame,
        .graph = &graph,
        .uploader = &uploader,
        .lighting_binding_layout = layout.value};
    resolver.set_upload_context(upload_context);

    renderer::RenderSceneSnapshot directional_snapshot{};
    directional_snapshot.lights.push_back(renderer::RenderLight{
        .light = renderer::RenderLightId{7},
        .type = renderer::RenderLightType::kDirectional,
        .direction = math::Vec3{0.0f, -2.0f, -2.0f},
        .color = math::Vec3{0.8f, 0.6f, 0.4f},
        .intensity = 3.0f,
    });
    renderer::RenderView view{};
    view.id = renderer::make_render_view_id(0);

    const auto inactive = resolver.resolve_lighting(directional_snapshot, view);
    if (inactive || inactive.result.code != foundation::ResultCode::kNotReady) {
        std::cerr << "inactive lighting resolve did not fail with kNotReady\n";
        return 3;
    }
    if (!check(frame.begin_frame(), "begin directional frame"))
        return 3;
    auto first = resolver.resolve_lighting(directional_snapshot, view);
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
    // Resolve every consumer before recording any readback. Same-view changed
    // lights and equal constants in another view must not rewrite A's buffer.
    auto other_view = view;
    other_view.id = renderer::make_render_view_id(1);
    auto changed_snapshot = directional_snapshot;
    changed_snapshot.lights.front().intensity = 2.0f;
    auto other_snapshot = directional_snapshot;
    other_snapshot.lights.front().direction = {0.0f, 0.0f, -1.0f};
    other_snapshot.lights.front().color = {1.0f, 0.25f, 0.5f};
    other_snapshot.lights.front().intensity = 4.0f;
    const std::array snapshots{directional_snapshot, other_snapshot, changed_snapshot, directional_snapshot};
    const std::array views{view, other_view, view, other_view};
    std::array<renderer::RenderLightingResource, 4> resources{};
    for (size_t i = 0; i < resources.size(); ++i) {
        const auto resolved = resolver.resolve_lighting(snapshots[i], views[i]);
        if (!resolved) return 6;
        resources[i] = resolved.value;
        for (size_t j = 0; j < i; ++j) {
            if (resources[i].lighting_buffer == resources[j].lighting_buffer ||
                resources[i].binding_set == resources[j].binding_set) {
                std::cerr << "same-frame scene/view lighting constants alias\n";
                (void)frame.abort_frame();
                (void)resolver.retire_gpu_resources();
                (void)device->destroy_binding_layout(layout.value);
                return 6;
            }
        }
    }
    const auto unchanged = resolver.resolve_lighting(directional_snapshot, view);
    if (!unchanged || unchanged.value.lighting_buffer != resources[0].lighting_buffer ||
        unchanged.value.binding_set != resources[0].binding_set) return 6;
    auto equivalent_snapshot = directional_snapshot;
    equivalent_snapshot.lights.front().direction.x = -0.f;
    const auto equivalent = resolver.resolve_lighting(equivalent_snapshot, view);
    if (!equivalent || equivalent.value.lighting_buffer != resources[0].lighting_buffer ||
        equivalent.value.binding_set != resources[0].binding_set) return 6;
    // An unsubmitted upload lost by clear_passes must be recorded again even
    // though its imported resource handle still belongs to this graph.
    graph.clear_passes();
    for (size_t i = 0; i < resources.size(); ++i) {
        const auto replay = resolver.resolve_lighting(snapshots[i], views[i]);
        if (!replay || replay.value.lighting_buffer != resources[i].lighting_buffer ||
            replay.value.binding_set != resources[i].binding_set) {
            std::cerr << "lost lighting upload was not replayed into its existing allocation\n";
            return 6;
        }
        resources[i] = replay.value;
    }
    if (!check(graph.compile(), "compile lighting retry") || aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != resources.size())
        return 6;
    first.value = resources[0];
    std::array<render_graph::FrameAllocation, 4> readbacks{};
    for (size_t i = 0; i < resources.size(); ++i) {
        const auto readback = add_readback(frame, graph, resources[i], "Read isolated lighting");
        if (!readback) return 8;
        readbacks[i] = readback.value;
    }
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
    // Independent literal oracle, including reserved alpha and ambient values.
    const renderer::RenderLightingConstants expected_a{{0.0f, -0.7071067f, -0.7071067f, 3.0f},
                                                       {0.8f, 0.6f, 0.4f, 1.0f}, {0.05f, 0.05f, 0.05f, 1.0f}};
    auto expected_c = expected_a;
    expected_c.directional_direction_intensity.w = 2.0f;
    const std::array expected{expected_a,
        renderer::RenderLightingConstants{{0.0f, 0.0f, -1.0f, 4.0f}, {1.0f, 0.25f, 0.5f, 1.0f},
                                          {0.05f, 0.05f, 0.05f, 1.0f}}, expected_c, expected_a};
    for (size_t i = 0; i < readbacks.size(); ++i) {
        renderer::RenderLightingConstants values{};
        if (!check(frame.read_readback(readbacks[i], std::as_writable_bytes(std::span{&values, 1})),
                   "read isolated constants") || !matches(values, expected[i])) return 15;
    }

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
    const auto ambient_readback_allocation = add_readback(frame, graph, ambient.value, "Read ambient lighting");
    if (!ambient_readback_allocation) return 18;
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
