/** @brief Reads GPU tile lists to prove spatial/depth rejection and empty-tile clearing. */
#include "feature_clock_test_fixture.hpp"
#include <array>
#include <iostream>

#include "light_culling.hpp"
#include "native_graph_build_session.hpp"
#include "render_feature_context.hpp"
#include "render_feature_graph_builders.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"
#include "renderer_variants_internal.hpp"
#include "renderer_feature_profiles.hpp"
#include "shader_contract.hpp"

namespace {
using namespace aeg;
constexpr pipeline::RenderResourceId kDepth{"CullingTest.Depth"}, kClusters{"CullingTest.Clusters"},
    kSecondClusters{"CullingTest.SecondClusters"};
struct CullingTestResources {
    renderer::RenderLightCullingResource first, second;
};
bool check(const foundation::Result& r) {
    if (!r)
        std::cerr << r.message << '\n';
    return bool(r);
}
}  // namespace

int main() {
    auto created = rhi::create_device({rhi::BackendType::kVulkan, "Light culling GPU readback"});
    if (!created)
        return 1;
    auto& device = *created.value;
    const auto storage_limit = device.capabilities().max_storage_buffer_bytes;
    if (storage_limit == 0) {
        std::cerr << "RHI does not publish the actual storage descriptor range limit\n";
        return 25;
    }
    renderer::LightCullingResources resources;
    shader::ShaderManager shaders(device);
    const auto shadowed = shader::load_shader_program(AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH);
    const auto unshadowed = shader::load_shader_program(AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH);
    if (!shadowed || !unshadowed) return 31;
    const auto shadowed_layout = shaders.build_pipeline_layout(shadowed.value);
    const auto unshadowed_layout = shaders.build_pipeline_layout(unshadowed.value);
    if (!shadowed_layout || !unshadowed_layout) return 31;
    const auto shadowed_inputs = shaders.binding_layout(shadowed_layout.value, 3);
    const auto unshadowed_inputs = shaders.binding_layout(unshadowed_layout.value, 3);
    if (!shadowed_inputs || !unshadowed_inputs) return 31;
    if (!check(resources.initialize(device, AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH,
        shadowed_inputs.value, unshadowed_inputs.value)))
        return 2;
    render_graph::FrameContext frame(device, {.frames_in_flight = 2});
    render_graph::RenderGraph graph(device);
    pipeline::PipelineRegistry registry;
    if (!check(renderer::register_render_pipeline(registry)))
        return 3;
    renderer::RenderSceneSnapshot scene{};
    scene.world = {0, 1};
    scene.world_revision = 1;
    scene.view.world = scene.world;
    scene.view.id = renderer::RenderViewId("CullingTest.View");
    scene.view.output_extent = {32, 16};
    // Identity camera: the left and right tiles span world X [-1,0] and [0,1], at depth 0.5.
    scene.lights = {{.light = renderer::RenderLightId(1), .position = {-0.5f, 0, 0.5f}, .range = 0.2f},
                    {.light = renderer::RenderLightId(2), .position = {0.5f, 0, 0.5f}, .range = 0.2f},
                    {.light = renderer::RenderLightId(3), .position = {0, 0, 5}, .range = 0.1f},
                    {.light = renderer::RenderLightId(4), .position = {10, 0, 0.5f}, .range = 0.1f}};
    renderer::RenderShadowView shadow{};
    if (resources.for_frame(frame, scene, shadow))
        return 5;
    // The shaders address clusters through uint32 ByteAddressBuffer offsets.
    // Producing metadata must reject an unaddressable list before allocating it.
    if (!check(frame.begin_frame())) return 22;
    auto oversized = scene;
    oversized.view.output_extent = {16384, 16384};
    oversized.lights.assign(renderer::kMaxLocalLights, scene.lights.front());
    const auto rejected = resources.for_frame(frame, oversized, shadow);
    const bool rejected_address_range = !rejected && rejected.result.code == foundation::ResultCode::kUnsupported;
    if (!check(frame.end_frame())) return 23;
    if (!rejected_address_range) {
        std::cerr << "GPU uint32 cluster address overflow was accepted\n";
        return 24;
    }
    // Exactly 2^32 bytes remain addressable by the shaders' last uint offset,
    // but exceed maxStorageBufferRange on both local Vulkan adapters. Only
    // small input uploads are allocated by for_frame; no cluster allocation.
    oversized.lights.resize(renderer::kMaxLocalLights - 1);
    for (uint32_t index = 0; index < oversized.lights.size(); ++index)
        oversized.lights[index].light = renderer::RenderLightId(index + 1);
    constexpr uint64_t addressable_bytes = uint64_t{1} << 32;
    if (!oversized.is_valid() || storage_limit >= addressable_bytes) {
        std::cerr << "storage-limit test does not cross the native descriptor range\n";
        return 26;
    }
    if (!check(frame.begin_frame())) return 27;
    const auto rejected_storage = resources.for_frame(frame, oversized, shadow);
    const bool rejected_storage_range = !rejected_storage &&
        rejected_storage.result.code == foundation::ResultCode::kUnsupported &&
        rejected_storage.result.message == "light culling buffers exceed the device storage descriptor range";
    if (!check(frame.end_frame())) return 28;
    if (!rejected_storage_range) {
        std::cerr << "addressable light list exceeded the actual device storage descriptor range\n";
        return 29;
    }
    for (uint32_t iteration = 0; iteration != 2; ++iteration) {
        if (!check(frame.begin_frame()))
            return 6;
        auto first = resources.for_frame(frame, scene, shadow);
        auto enabled_shadow = shadow;
        enabled_shadow.enabled = true;
        const auto shadowed_resource = resources.for_frame(frame, scene, enabled_shadow);
        if (!first || first.value.shadow_enabled || !shadowed_resource || !shadowed_resource.value.shadow_enabled)
            return 32;
        auto second_scene = scene;
        second_scene.lights = iteration == 0 ? std::vector{scene.lights[1]} : std::vector<renderer::RenderLight>{};
        if (iteration == 0) second_scene.lights.front().type = renderer::RenderLightType::kSpot;
        auto second = resources.for_frame(frame, second_scene, shadow);
        if (!first || !second) {
            check(!first ? first.result : second.result);
            return 7;
        }
        CullingTestResources resource{std::move(first.value), std::move(second.value)};
        if (resource.first.cluster_bytes != 40 || resource.first.tile_count_x != 2 || resource.first.tile_count_y != 1 ||
            resource.second.cluster_bytes != (iteration == 0 ? 16 : 8) || resource.second.lights_desc.size != 64)
            return 8;
        rhi::TextureDesc depth_desc{};
        depth_desc.extent = {32, 16, 1};
        depth_desc.format = rhi::Format::kD32Float;
        depth_desc.capabilities = rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kSampled;
        const auto depth = graph.create_texture(depth_desc);
        rhi::BufferDesc cluster_desc{};
        cluster_desc.size = resource.first.cluster_bytes;
        cluster_desc.capabilities = rhi::BufferCapability::kStorage | rhi::BufferCapability::kTransferSource;
        const auto clusters = graph.create_buffer(cluster_desc);
        cluster_desc.size = resource.second.cluster_bytes;
        const auto second_clusters = graph.create_buffer(cluster_desc);
        const auto readback = frame.allocate_readback(56, 256);
        if (!depth || !clusters || !second_clusters || !readback)
            return 9;
        rhi::BufferDesc destination_desc{};
        destination_desc.size = readback.value.buffer_size;
        destination_desc.memory = rhi::MemoryClass::kReadback;
        destination_desc.capabilities = rhi::BufferCapability::kTransferDestination;
        const auto destination = graph.import_buffer(readback.value.buffer, destination_desc);
        if (!destination)
            return 10;
        const auto clear = graph.add_pass(
            "Test depth", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                return pass.use_depth_stencil_attachment(depth.value, rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                                                         iteration == 0 ? 0.5f : 1.0f, 0,
                                                         rhi::ResourceUsage::kDepthStencilWrite);
            },
            [](render_graph::RenderPassCommandContext& command) {
                if (const auto r = command.begin_rendering(); !r)
                    return r;
                return command.end_rendering();
            });
        if (!clear)
            return 11;
        pipeline::RenderPassRegistry passes;
        if (!renderer::register_builtin_render_passes(passes))
            return 1;
        // The second iteration exercises the neutral direct Compute seam,
        // which must not require a builtin RenderPassRegistry.
        renderer::RenderFeatureContext context{&scene, nullptr, &resource};
        if (iteration == 0)
            context.pass_execution = {
                &passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)};
        const std::array<render_graph::RenderGraphFeatureTextureBinding, 1> textures{{{kDepth, depth.value}}};
        using Flags = pipeline::RenderGraphBuildFlags;
        const auto session = render_graph::NativeGraphBuildSession::begin(
            graph, textures, Flags::kMayCreateResources | Flags::kMayCreateComputePass | Flags::kMayUseDedicatedQueue,
            &context);
        if (!session) {
            check(session.result);
            return 12;
        }
        auto& builder = session.value->builder();
        if (!check(builder.bind_buffer(kClusters, clusters.value)) ||
            !check(builder.bind_buffer(kSecondClusters, second_clusters.value)) ||
            !check(renderer::build_light_culling_render_graph(builder, context, depth.value, clusters.value,
                                                             resource.first)) ||
            !check(renderer::build_light_culling_render_graph(builder, context, depth.value, second_clusters.value,
                                                             resource.second)))
            return 12;
        if (resource.first.constants_graph == resource.second.constants_graph ||
            resource.first.lights_graph == resource.second.lights_graph) {
            std::cerr << "two native light-list uploads aliased one graph import\n";
            return 12;
        }
        session.value->commit();
        const auto second_bytes = resource.second.cluster_bytes;
        const auto copy = graph.add_pass(
            "Read tile lists", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                if (const auto r =
                        pass.read(clusters.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy);
                    !r)
                    return r;
                if (const auto r = pass.read(second_clusters.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !r)
                    return r;
                pass.set_side_effects();
                return pass.write(destination.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
            },
            [&](render_graph::RenderPassCommandContext& command) {
                const auto native_clusters = command.resolve_buffer(clusters.value);
                const auto native_depth = command.resolve_texture_view(depth.value, rhi::TextureViewKind::kSampled,
                    {rhi::Aspect::kDepth, 0, 1, 0, 1});
                if (!native_clusters || !native_depth)
                    return foundation::invalid_state("native culling descriptor fixture resources missing");
                const auto plain = resource.first.prepare_forward({}, native_clusters.value);
                const auto repeated = resource.first.prepare_forward({}, native_clusters.value);
                if (!plain || !repeated || plain.value == repeated.value)
                    return foundation::invalid_state("unshadowed forward binding requires an unused shadow texture");
                const auto invalid_compute = resource.first.prepare_compute({}, native_clusters.value);
                const auto unsolicited_shadow = resource.first.prepare_forward(native_depth.value, native_clusters.value);
                const auto missing_shadow = shadowed_resource.value.prepare_forward({}, native_clusters.value);
                const auto valid_shadow = shadowed_resource.value.prepare_forward(native_depth.value, native_clusters.value);
                if (invalid_compute || invalid_compute.result.code != foundation::ResultCode::kInvalidArgument ||
                    unsolicited_shadow || unsolicited_shadow.result.code != foundation::ResultCode::kInvalidArgument ||
                    missing_shadow || missing_shadow.result.code != foundation::ResultCode::kInvalidArgument || !valid_shadow)
                    return foundation::invalid_state("immutable culling descriptor modes accepted incompatible depth inputs");
                if (const auto r = command.copy_buffer(clusters.value, destination.value, 0, readback.value.offset, 40); !r)
                    return r;
                return command.copy_buffer(second_clusters.value, destination.value, 0, readback.value.offset + 40, second_bytes);
            });
        if (!copy || !check(graph.compile()))
            return 13;
        const auto executed = graph.execute(frame);
        if (!executed) {
            check(executed.result);
            return 14;
        }
        if (!check(frame.end_frame()) || !check(device.wait_idle()))
            return 15;
        std::array<uint32_t, 14> lists{};
        if (!check(frame.read_readback(readback.value, std::as_writable_bytes(std::span{lists}))))
            return 16;
        if (iteration == 0 ? (lists[0] != 1 || lists[1] != 0 || lists[5] != 1 || lists[6] != 1)
                           : (lists[0] != 0 || lists[5] != 0)) {
            std::cerr << "Unexpected tile lists: " << lists[0] << ',' << lists[1] << " / " << lists[5] << ','
                      << lists[6] << '\n';
            return 17;
        }
        if (iteration == 0 ? (lists[10] != 0 || lists[12] != 1 || lists[13] != 0)
                           : (lists[10] != 0 || lists[11] != 0)) {
            std::cerr << "independent spot/empty light list used another bundle's constants or lights\n";
            return 30;
        }
        const auto stale = resource.first.prepare_compute({}, {});
        const auto stale_second = resource.second.prepare_compute({}, {});
        const auto stale_forward = resource.first.prepare_forward({}, {});
        const auto stale_shadowed = shadowed_resource.value.prepare_forward({}, {});
        if (stale || stale.result.code != foundation::ResultCode::kInvalidState ||
            stale_second || stale_second.result.code != foundation::ResultCode::kInvalidState ||
            stale_forward || stale_forward.result.code != foundation::ResultCode::kInvalidState ||
            stale_shadowed || stale_shadowed.result.code != foundation::ResultCode::kInvalidState)
            return 18;
        if (!check(frame.drain()))
            return 19;
        graph.reset();
    }
    std::cout << "GPU tile culling, depth rejection, empty tiles and frame retirement passed\n";
    feature::FeatureRegistry scene_features{aeg::feature::test::clock_test_host()};
    if (!check(renderer::register_scene_render_profiles(scene_features))) return 73;
    for (const auto id : {"Renderer.Scene.Opaque", "Renderer.Scene.Forward", "Renderer.Scene.Deferred"})
        if (!scene_features.find_definition(feature::FeatureDefinitionId(id))) return 73;
    if (scene_features.find_definition(feature::FeatureDefinitionId("CullingTest.Feature"))) {
        std::cerr << "fixed native tile culling still registers a legacy Render Feature\n";
        return 73;
    }
    std::cout << "Three real Scene definitions present; retired CullingTest.Feature absent\n";
    return 0;
}
