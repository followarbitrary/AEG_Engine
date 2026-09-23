/** @brief Reads GPU tile lists to prove spatial/depth rejection and empty-tile clearing. */
#include <array>
#include <iostream>

#include "light_culling.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"
#include "renderer_variants.hpp"

namespace {
using namespace aeg;
constexpr pipeline::RenderResourceId kDepth{"CullingTest.Depth"}, kClusters{"CullingTest.Clusters"};
constexpr pipeline::FeatureId kFeature{"CullingTest.Feature"};
bool check(const foundation::Result& r) {
    if (!r)
        std::cerr << r.message << '\n';
    return bool(r);
}
class CullingFeature final : public pipeline::RenderFeature {
public:
    const pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const pipeline::FeatureDescriptor kDesc{
            .id = kFeature,
            .name = "CullingTest.Feature",
            .domain = pipeline::FeatureDomain::kRender,
            .domain_mask = pipeline::to_mask(pipeline::FeatureDomain::kRender),
            .supported_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame)};
        return kDesc;
    }
    foundation::Result build_render_graph(pipeline::RenderGraphBuilder& builder) const override {
        const auto* context = renderer::render_feature_context(builder);
        const auto depth = builder.find_texture(kDepth);
        const auto clusters = builder.find_buffer(kClusters);
        if (!context || !depth || !clusters)
            return foundation::invalid_state("culling test inputs missing");
        auto& resource = *static_cast<renderer::RenderLightCullingResource*>(context->user_data);
        return renderer::build_light_culling_render_graph(builder, *context, depth.value, clusters.value, resource);
    }

protected:
    foundation::Result setup(pipeline::RenderGraphBuilder& builder) const override {
        const auto stage = builder.add_stage({.id = pipeline::StageId("CullingTest.Build"),
                                              .phase = pipeline::EnginePhase::kRenderBuild,
                                              .execution_domain = pipeline::ExecutionDomain::kRenderThread});
        return stage ? foundation::Result::success() : foundation::invalid_state("culling test stage failed");
    }
};
}  // namespace

int main() {
    auto created = rhi::create_device({rhi::BackendType::kVulkan, "Light culling GPU readback"});
    if (!created)
        return 1;
    auto& device = *created.value;
    renderer::LightCullingResources resources;
    if (!check(resources.initialize(device, AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH)))
        return 2;
    render_graph::FrameContext frame(device, {.frames_in_flight = 2});
    render_graph::RenderGraph graph(device);
    pipeline::PipelineRegistry registry;
    if (!check(renderer::register_render_pipeline(registry)) ||
        !check(registry.register_feature([] { return std::make_unique<CullingFeature>(); })) ||
        !check(registry.bind_feature({.feature = kFeature,
                                      .pipeline = pipeline::PipelineId("RenderPipeline"),
                                      .scope = pipeline::PipelineScope::kFrame,
                                      .point = pipeline::FeaturePointId("RenderPipeline.Shadow")})))
        return 3;
    pipeline::PipelineScopeHost host(pipeline::PipelineScope::kFrame, pipeline::PipelineInstanceId("CullingTest"));
    if (!check(host.bind_pipeline(pipeline::PipelineId("RenderPipeline"))) || !check(host.rebuild(registry)))
        return 4;
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
    for (uint32_t iteration = 0; iteration != 2; ++iteration) {
        if (!check(frame.begin_frame()))
            return 6;
        auto resource = resources.for_frame(frame, scene, shadow);
        if (!resource) {
            check(resource.result);
            return 7;
        }
        if (resource.value.cluster_bytes != 40 || resource.value.tile_count_x != 2 || resource.value.tile_count_y != 1)
            return 8;
        rhi::TextureDesc depth_desc{};
        depth_desc.extent = {32, 16, 1};
        depth_desc.format = rhi::Format::kD32Float;
        depth_desc.capabilities = rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kSampled;
        const auto depth = graph.create_texture(depth_desc);
        rhi::BufferDesc cluster_desc{};
        cluster_desc.size = resource.value.cluster_bytes;
        cluster_desc.capabilities = rhi::BufferCapability::kStorage | rhi::BufferCapability::kTransferSource;
        const auto clusters = graph.create_buffer(cluster_desc);
        const auto readback = frame.allocate_readback(40, 256);
        if (!depth || !clusters || !readback)
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
        const renderer::RenderFeatureContext context{&scene, nullptr, &resource.value, &passes};
        const std::array<render_graph::RenderGraphFeatureTextureBinding, 1> textures{{{kDepth, depth.value}}};
        const std::array<render_graph::RenderGraphFeatureBufferBinding, 1> buffers{{{kClusters, clusters.value}}};
        render_graph::RenderGraphFeatureRuntime runtime(graph, host.active_plan_snapshot(), registry,
                                                        {buffers, textures, nullptr, &context});
        if (!check(runtime.build()))
            return 12;
        const auto copy = graph.add_pass(
            "Read tile lists", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                if (const auto r =
                        pass.read(clusters.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy);
                    !r)
                    return r;
                pass.set_side_effects();
                return pass.write(destination.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
            },
            [&](render_graph::RenderPassCommandContext& command) {
                return command.copy_buffer(clusters.value, destination.value, 0, readback.value.offset, 40);
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
        std::array<uint32_t, 10> lists{};
        if (!check(frame.read_readback(readback.value, std::as_writable_bytes(std::span{lists}))))
            return 16;
        if (iteration == 0 ? (lists[0] != 1 || lists[1] != 0 || lists[5] != 1 || lists[6] != 1)
                           : (lists[0] != 0 || lists[5] != 0)) {
            std::cerr << "Unexpected tile lists: " << lists[0] << ',' << lists[1] << " / " << lists[5] << ','
                      << lists[6] << '\n';
            return 17;
        }
        const auto stale = resource.value.prepare_compute({}, {});
        if (stale || stale.result.code != foundation::ResultCode::kInvalidState)
            return 18;
        if (!check(frame.drain()))
            return 19;
        graph.reset();
    }
    std::cout << "GPU tile culling, depth rejection, empty tiles and frame retirement passed\n";
}
