/** @file renderer_depth_pass_smoke.cpp @brief Verifies geometry, not just a clear, reaches depth. */
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

#include "opaque_render_feature.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "renderer_variants.hpp"
#include "shader_contract.hpp"

namespace {
using namespace aeg;
constexpr pipeline::RenderResourceId kDepth{"DepthSmoke.Target"};
constexpr pipeline::FeatureId kFeature{"DepthSmoke.Feature"};
constexpr uint32_t kExtent = 32;

bool check(const foundation::Result& result) {
    if (!result)
        std::cerr << result.message << '\n';
    return static_cast<bool>(result);
}

struct Resolver final : renderer::RenderResourceResolver {
    renderer::RenderMeshResource mesh{};
    renderer::RenderObjectResource object{};
    rhi::PipelineHandle depth_pipeline{};
    foundation::ResultValue<renderer::RenderMeshResource> resolve_mesh(renderer::MeshAssetId) const override {
        return {foundation::Result::success(), mesh};
    }
    foundation::ResultValue<renderer::RenderMaterialResource> resolve_material(
        renderer::MaterialInstanceId) const override {
        renderer::RenderMaterialResource material{};
        material.depth_pipeline = depth_pipeline;
        return {foundation::Result::success(), material};
    }
    foundation::ResultValue<renderer::RenderObjectResource> resolve_object(const renderer::RenderInstance&,
                                                                           const renderer::RenderView&) const override {
        return {foundation::Result::success(), object};
    }
};

class DepthFeature final : public pipeline::RenderFeature {
public:
    const pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const pipeline::FeatureDescriptor kDescriptor{
            .id = kFeature,
            .name = "DepthSmoke.Feature",
            .domain = pipeline::FeatureDomain::kRender,
            .domain_mask = pipeline::to_mask(pipeline::FeatureDomain::kRender),
            .supported_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame)};
        return kDescriptor;
    }
    foundation::Result build_render_graph(pipeline::RenderGraphBuilder& builder) const override {
        const auto* context = renderer::render_feature_context(builder);
        const auto target = builder.find_texture(kDepth);
        if (!context || !target)
            return foundation::invalid_state("Depth smoke context/target missing");
        return renderer::build_depth_render_graph(builder, *context, context->scene_snapshot->view, target.value,
                                                  "Renderer.ZPrepassPass");
    }

protected:
    foundation::Result setup(pipeline::RenderGraphBuilder& builder) const override {
        const auto stage = builder.add_stage({.id = pipeline::StageId("DepthSmoke.Build"),
                                              .phase = pipeline::EnginePhase::kRenderBuild,
                                              .execution_domain = pipeline::ExecutionDomain::kRenderThread});
        return stage ? foundation::Result::success() : foundation::invalid_state("Depth stage failed");
    }
};
}  // namespace

int main() {
    using namespace aeg;
    auto device_result = rhi::create_device({rhi::BackendType::kVulkan, "Renderer depth smoke"});
    if (!device_result)
        return 1;
    auto device = std::move(device_result.value);
    shader::ShaderManager shaders(*device);
    const auto program = shader::load_shader_program(AEG_DEPTH_MESH_SHADER_PACKAGE_PATH);
    if (!program) {
        check(program.result);
        return 2;
    }
    const auto layout = shaders.build_pipeline_layout(program.value);
    if (!layout)
        return 3;
    const auto object_layout = shaders.binding_layout(layout.value, 0);
    if (!object_layout)
        return 4;
    const std::array<rhi::VertexBindingDesc, 1> bindings{{{0, 16, rhi::VertexInputRate::kVertex}}};
    const std::array<rhi::VertexAttributeDesc, 1> attributes{{{0, 0, rhi::Format::kRgba32Float, 0}}};
    rhi::GraphicsPipelineState graphics{};
    graphics.vertex_bindings = bindings;
    graphics.vertex_attributes = attributes;
    graphics.cull_mode = rhi::CullMode::kNone;
    graphics.depth_test_enable = true;
    graphics.depth_write_enable = true;
    graphics.depth_stencil_format = rhi::Format::kD32Float;
    const auto gpu_pipeline = shaders.create_pipeline(
        program.value, layout.value, {rhi::PipelineType::kGraphics, graphics, rhi::DebugName{"DepthSmoke.Pipeline"}});
    if (!gpu_pipeline) {
        check(gpu_pipeline.result);
        return 5;
    }

    const std::array<std::array<float, 4>, 3> vertices{
        {{-0.8f, -0.8f, 0.25f, 1}, {0.8f, -0.8f, 0.25f, 1}, {0, 0.8f, 0.25f, 1}}};
    const std::array<uint16_t, 3> indices{{0, 1, 2}};
    const renderer::RenderObjectConstants constants{};
    const auto buffer_desc = [](uint64_t size, rhi::BufferCapability capability, rhi::ResourceUsage usage) {
        rhi::BufferDesc desc{};
        desc.size = size;
        desc.capabilities = capability;
        desc.memory = rhi::MemoryClass::kUpload;
        desc.initial_usage = usage;
        desc.initial_queue = rhi::QueueClass::kGraphics;
        return desc;
    };
    const auto vertex_desc =
        buffer_desc(sizeof(vertices), rhi::BufferCapability::kVertex, rhi::ResourceUsage::kVertexInput);
    const auto index_desc =
        buffer_desc(sizeof(indices), rhi::BufferCapability::kIndex, rhi::ResourceUsage::kIndexInput);
    const auto object_desc =
        buffer_desc(sizeof(constants), rhi::BufferCapability::kUniform, rhi::ResourceUsage::kUniformRead);
    const auto vertex_buffer = device->create_buffer(vertex_desc);
    const auto index_buffer = device->create_buffer(index_desc);
    const auto object_buffer = device->create_buffer(object_desc);
    const auto object_set = device->create_binding_set({object_layout.value, rhi::BindingSetLifetime::kPersistent});
    if (!vertex_buffer || !index_buffer || !object_buffer || !object_set)
        return 6;
    rhi::BindingWrite write{};
    write.location = {0, 0};
    write.resource.type = rhi::BindingType::kUniformBuffer;
    write.resource.buffer_range = {object_buffer.value, 0, sizeof(constants)};
    if (!check(device->write_buffer(vertex_buffer.value, 0, std::as_bytes(std::span{vertices}))) ||
        !check(device->write_buffer(index_buffer.value, 0, std::as_bytes(std::span{indices}))) ||
        !check(device->write_buffer(object_buffer.value, 0, std::as_bytes(std::span{&constants, 1}))) ||
        !check(device->update_binding_set(object_set.value, std::span{&write, 1})))
        return 7;
    Resolver resolver;
    resolver.depth_pipeline = gpu_pipeline.value;
    resolver.mesh = {.vertex_buffer = vertex_buffer.value,
                     .vertex_buffer_desc = vertex_desc,
                     .index_buffer = index_buffer.value,
                     .index_buffer_desc = index_desc,
                     .vertex_stride = 16,
                     .vertex_count = 3,
                     .index_count = 3};
    resolver.object = {object_buffer.value, object_desc, object_layout.value, object_set.value, 0, 0,
                       sizeof(constants)};

    pipeline::PipelineRegistry registry;
    if (!check(renderer::register_render_pipeline(registry)) ||
        !check(registry.register_feature([] { return std::make_unique<DepthFeature>(); })) ||
        !check(registry.bind_feature({.feature = kFeature,
                                      .pipeline = pipeline::PipelineId("RenderPipeline"),
                                      .scope = pipeline::PipelineScope::kFrame,
                                      .point = pipeline::FeaturePointId("RenderPipeline.Depth")})))
        return 8;
    pipeline::PipelineScopeHost host(pipeline::PipelineScope::kFrame, pipeline::PipelineInstanceId("DepthSmoke"));
    if (!check(host.bind_pipeline(pipeline::PipelineId("RenderPipeline"))) || !check(host.rebuild(registry)))
        return 9;
    render_graph::FrameContext frame(*device, {.frames_in_flight = 1});
    render_graph::RenderGraph graph(*device);
    rhi::TextureDesc depth_desc{};
    depth_desc.extent = {kExtent, kExtent, 1};
    depth_desc.format = rhi::Format::kD32Float;
    depth_desc.capabilities = rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kTransferSource;
    const auto depth = graph.create_texture(depth_desc);
    if (!depth || !check(frame.begin_frame()))
        return 10;
    const auto readback = frame.allocate_readback(kExtent * kExtent * sizeof(float), 256);
    if (!readback)
        return 11;
    rhi::BufferDesc readback_desc{};
    readback_desc.size = readback.value.buffer_size;
    readback_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    readback_desc.memory = rhi::MemoryClass::kReadback;
    const auto destination = graph.import_buffer(readback.value.buffer, readback_desc);
    if (!destination)
        return 12;
    renderer::RenderSceneSnapshot snapshot{};
    snapshot.world = {0, 1};
    snapshot.world_revision = 1;
    snapshot.view.world = snapshot.world;
    snapshot.view.id = renderer::RenderViewId("DepthSmoke.View");
    snapshot.view.output_extent = {kExtent, kExtent};
    snapshot.instances.push_back({.object = renderer::RenderObjectId(1),
                                  .mesh = renderer::MeshAssetId(1),
                                  .material = renderer::MaterialInstanceId(1)});
    pipeline::RenderPassRegistry passes;
    if (!renderer::register_builtin_render_passes(passes))
        return 1;
    const renderer::RenderFeatureContext context{&snapshot, &resolver, nullptr, &passes};
    const std::array<render_graph::RenderGraphFeatureTextureBinding, 1> textures{{{kDepth, depth.value}}};
    render_graph::RenderGraphFeatureRuntime runtime(graph, host.active_plan_snapshot(), registry,
                                                    {{}, textures, nullptr, &context});
    resolver.depth_pipeline = {};
    const auto missing_pipeline = runtime.build();
    if (missing_pipeline || missing_pipeline.code != foundation::ResultCode::kInvalidState || !check(graph.compile()) ||
        graph.compile_info().pass_count != 0) {
        std::cerr << "Missing depth pipeline must fail without leaving a partial pass\n";
        return 19;
    }
    resolver.depth_pipeline = gpu_pipeline.value;
    if (!check(runtime.build()))
        return 13;
    const auto copy = graph.add_pass(
        "Depth.Readback", rhi::QueueClass::kGraphics,
        [&](render_graph::PassBuilder& pass) {
            if (const auto r = pass.read(depth.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !r)
                return r;
            if (const auto r =
                    pass.write(destination.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
                !r)
                return r;
            pass.set_side_effects();
            return foundation::Result::success();
        },
        [&](render_graph::RenderPassCommandContext& command) {
            return command.copy_texture_to_buffer(depth.value, {rhi::Aspect::kDepth, 0, 0, 1}, {},
                                                  {kExtent, kExtent, 1}, destination.value, readback.value.offset, 0,
                                                  0);
        });
    if (!copy || !check(graph.compile()))
        return 14;
    const auto executed = graph.execute(frame);
    if (!executed || !check(frame.end_frame()) || !check(device->wait_idle()))
        return 15;
    std::array<std::byte, kExtent * kExtent * sizeof(float)> pixels{};
    if (!check(frame.read_readback(readback.value, pixels)))
        return 16;
    float center{}, corner{};
    std::memcpy(&center, pixels.data() + ((kExtent / 2) * kExtent + kExtent / 2) * sizeof(float), sizeof(float));
    std::memcpy(&corner, pixels.data(), sizeof(float));
    if (std::abs(center - 0.25f) > 0.001f || std::abs(corner - 1.0f) > 0.001f) {
        std::cerr << "Expected geometry depth 0.25/background 1; got " << center << '/' << corner << '\n';
        return 17;
    }
    graph.reset();
    if (!check(device->destroy_binding_set(object_set.value)) || !check(device->destroy_pipeline(gpu_pipeline.value)) ||
        !check(device->destroy_buffer(vertex_buffer.value)) || !check(device->destroy_buffer(index_buffer.value)) ||
        !check(device->destroy_buffer(object_buffer.value)))
        return 18;
    std::cout << "Depth pass GPU readback passed\n";
    return 0;
}
