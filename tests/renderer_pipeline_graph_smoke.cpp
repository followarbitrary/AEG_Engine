/**
 * @file renderer_pipeline_graph_smoke.cpp
 * @brief Verifies built-in renderer variants expand into valid RenderGraph topology.
 */
#include <array>
#include <iostream>
#include <memory>
#include <opaque_render_feature.hpp>
#include <post_process.hpp>
#include <render_feature_runtime.hpp>
#include <render_graph.hpp>
#include <renderer.hpp>
#include <renderer_variants.hpp>
#include <rhi.hpp>

#ifndef AEG_POST_PROCESS_SHADER_PACKAGE_PATH
#define AEG_POST_PROCESS_SHADER_PACKAGE_PATH ""
#endif

namespace {
using aeg::ecs::WorldHandle;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::pipeline::PipelinePlanHandle;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::PipelineScope;
using aeg::pipeline::PipelineScopeHost;
using aeg::pipeline::RenderResourceId;
using aeg::renderer::RenderFeatureContext;
using aeg::renderer::RenderSceneSnapshot;
using aeg::renderer::RenderView;
using aeg::rhi::Format;
using aeg::rhi::MemoryClass;
using aeg::rhi::TextureCapability;
using aeg::rhi::TextureDesc;

struct EmptySceneResolver final : aeg::renderer::RenderResourceResolver {
    aeg::renderer::PostProcessResources& post;
    aeg::render_graph::FrameContext& frame;
    EmptySceneResolver(aeg::renderer::PostProcessResources& p, aeg::render_graph::FrameContext& f)
        : post(p), frame(f) {}
    aeg::foundation::ResultValue<aeg::renderer::RenderMeshResource> resolve_mesh(
        aeg::renderer::MeshAssetId) const override {
        return {aeg::foundation::not_found("Empty scene"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        aeg::renderer::MaterialInstanceId) const override {
        return {aeg::foundation::not_found("Empty scene"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderObjectResource> resolve_object(
        const aeg::renderer::RenderInstance&, const RenderView&) const override {
        return {aeg::foundation::not_found("Empty scene"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderPostProcessResource> resolve_post_process(
        const RenderView& view) const override {
        return post.for_frame(frame, view);
    }
};

[[nodiscard]] TextureDesc color_desc() {
    TextureDesc desc{};
    desc.extent = {8, 8, 1};
    desc.format = Format::kRgba8Unorm;
    desc.capabilities =
        TextureCapability::kSampled | TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
    desc.memory = MemoryClass::kDeviceLocal;
    return desc;
}

[[nodiscard]] TextureDesc depth_desc() {
    TextureDesc desc{};
    desc.extent = {8, 8, 1};
    desc.format = Format::kD32Float;
    desc.capabilities = TextureCapability::kSampled | TextureCapability::kDepthStencil;
    desc.memory = MemoryClass::kDeviceLocal;
    return desc;
}

[[nodiscard]] RenderSceneSnapshot make_snapshot(RenderResourceId color, RenderResourceId depth) {
    RenderSceneSnapshot snapshot{};
    snapshot.frame_number = 1;
    snapshot.world_revision = 1;
    snapshot.world = WorldHandle{0, 1};
    snapshot.view.id = aeg::renderer::RenderViewId("Renderer.GraphSmokeView");
    snapshot.view.world = snapshot.world;
    snapshot.view.output_extent = {8, 8};
    snapshot.view.color_target = color;
    snapshot.view.depth_target = depth;
    snapshot.view.post_process.sky_enabled = true;
    snapshot.view.post_process.sky_zenith = snapshot.view.post_process.sky_horizon =
        snapshot.view.post_process.sky_ground = {0.25f, 0.5f, 1};
    return snapshot;
}

[[nodiscard]] bool build_variant(aeg::rhi::Device& device, PipelineRegistry& registry,
                                 aeg::pipeline::PipelineId pipeline_id, RenderResourceId color_id,
                                 RenderResourceId depth_id, uint32_t expected_passes) {
    PipelineScopeHost host(PipelineScope::kFrame, aeg::pipeline::PipelineInstanceId("Renderer.GraphSmoke"));
    if (!host.bind_pipeline(pipeline_id) || !host.rebuild(registry)) {
        return false;
    }
    const PipelinePlanHandle plan = host.active_plan_snapshot();
    if (plan == nullptr) {
        return false;
    }

    aeg::renderer::PostProcessResources post;
    if (!post.initialize(device, AEG_POST_PROCESS_SHADER_PACKAGE_PATH, Format::kRgba8Unorm))
        return false;
    aeg::render_graph::FrameContext frame(device, {.frames_in_flight = 1});
    if (!frame.begin_frame())
        return false;
    EmptySceneResolver resolver(post, frame);
    aeg::render_graph::RenderGraph graph(device);
    const auto color = graph.create_texture(color_desc(), aeg::render_graph::ResourceLifetime::kPersistent);
    const auto depth = graph.create_texture(depth_desc(), aeg::render_graph::ResourceLifetime::kPersistent);
    if (!color || !depth) {
        return false;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 2> bindings{{
        {color_id, color.value},
        {depth_id, depth.value},
    }};
    aeg::pipeline::RenderPassRegistry passes;
    if (!aeg::renderer::register_builtin_render_passes(passes))
        return false;
    struct ObservedPass final : aeg::pipeline::RenderPass {
        std::shared_ptr<const aeg::pipeline::RenderPass> delegate;
        unsigned* setups;
        unsigned* executions;
        const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override {
            return delegate->descriptor();
        }
        static auto invalid_configurations(const aeg::pipeline::RenderPassConfiguration& config) {
            std::array invalid{config, config, config, config};
            invalid[0].type = aeg::pipeline::RenderPassTypeId("Renderer.UnknownPass");
            invalid[1].enabled = false;
            invalid[2].queue = config.queue == aeg::rhi::QueueClass::kCompute ? aeg::rhi::QueueClass::kGraphics
                                                                              : aeg::rhi::QueueClass::kCompute;
            invalid[3].bindings.emplace(1, 2);
            return invalid;
        }
        Result setup(aeg::pipeline::RenderPassBuilder& builder,
                     const aeg::pipeline::RenderPassConfiguration& config) const override {
            ++*setups;
            for (const auto& invalid : invalid_configurations(config))
                if (delegate->setup(builder, invalid).code != ResultCode::kInvalidArgument)
                    return aeg::foundation::invalid_state("Invalid pass configuration reached setup");
            auto unprepared = config;
            unprepared.runtime_data.reset();
            if (delegate->setup(builder, unprepared).code != ResultCode::kInvalidState)
                return aeg::foundation::invalid_state("Unprepared pass setup must fail");
            return delegate->setup(builder, config);
        }
        Result execute(aeg::pipeline::RenderPassExecutionContext& command,
                       const aeg::pipeline::RenderPassConfiguration& config) const override {
            ++*executions;
            for (const auto& invalid : invalid_configurations(config))
                if (delegate->execute(command, invalid).code != ResultCode::kInvalidArgument)
                    return aeg::foundation::invalid_state("Invalid pass configuration reached execution");
            auto unprepared = config;
            unprepared.runtime_data.reset();
            if (delegate->execute(command, unprepared).code != ResultCode::kInvalidState)
                return aeg::foundation::invalid_state("Unprepared pass execution must fail");
            return delegate->execute(command, config);
        }
    };
    constexpr std::array kPassIds{
        aeg::renderer::kShadowPassTypeId,           aeg::renderer::kGBufferPassTypeId,
        aeg::renderer::kDeferredLightingPassTypeId, aeg::renderer::kSkyboxPostProcessPassTypeId,
        aeg::renderer::kZPrepassPassTypeId,         aeg::renderer::kLightCullingPassTypeId,
        aeg::renderer::kForwardOpaquePassTypeId};
    std::array<unsigned, 7> setups{}, executions{};
    for (size_t i = 0; i < kPassIds.size(); ++i) {
        const auto found = passes.find_pass(kPassIds[i]);
        if (!found || !passes.unregister_pass(kPassIds[i]))
            return false;
        if (!passes.register_pass([&, i, owner = found.value] {
                auto pass = std::make_unique<ObservedPass>();
                pass->delegate = owner;
                pass->setups = &setups[i];
                pass->executions = &executions[i];
                return pass;
            }))
            return false;
    }
    const RenderSceneSnapshot snapshot = make_snapshot(color_id, depth_id);
    const RenderFeatureContext feature_context{&snapshot, &resolver, nullptr, &passes};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc desc{
        {}, bindings, nullptr, &feature_context, {},
    };
    // Missing registry/types must abort the feature transaction without leaving partial passes/resources.
    aeg::pipeline::RenderPassRegistry empty_passes;
    for (const auto* unavailable : {static_cast<const aeg::pipeline::RenderPassRegistry*>(nullptr),
                                    static_cast<const aeg::pipeline::RenderPassRegistry*>(&empty_passes)}) {
        auto unavailable_context = feature_context;
        unavailable_context.render_passes = unavailable;
        auto unavailable_desc = desc;
        unavailable_desc.feature_data = &unavailable_context;
        aeg::render_graph::RenderGraphFeatureRuntime unavailable_runtime(graph, plan, registry, unavailable_desc);
        const auto failed = unavailable_runtime.build();
        if (failed || failed.code != (unavailable ? ResultCode::kNotFound : ResultCode::kInvalidState) ||
            !graph.compile() || graph.compile_info().pass_count != 0 || graph.compile_info().resource_count != 2)
            return false;
    }
    aeg::render_graph::RenderGraphFeatureRuntime runtime(graph, plan, registry, desc);
    if (const Result result = runtime.build(); !result) {
        std::cerr << result.message << '\n';
        return false;
    }
    // The graph owns resolved passes even after the host registry changes.
    for (auto id : kPassIds)
        if (!passes.unregister_pass(id))
            return false;
    const auto readback = frame.allocate_readback(8 * 8 * 4, 256);
    if (!readback)
        return false;
    aeg::rhi::BufferDesc read_desc{};
    read_desc.size = readback.value.buffer_size;
    read_desc.memory = MemoryClass::kReadback;
    read_desc.capabilities = aeg::rhi::BufferCapability::kTransferDestination;
    const auto destination = graph.import_buffer(readback.value.buffer, read_desc);
    if (!destination)
        return false;
    const auto copy = graph.add_pass(
        "Read empty sky", aeg::rhi::QueueClass::kGraphics,
        [&](aeg::render_graph::PassBuilder& pass) {
            if (const auto r =
                    pass.read(color.value, aeg::rhi::ResourceUsage::kCopySource, aeg::rhi::PipelineStage::kCopy);
                !r)
                return r;
            pass.set_side_effects();
            return pass.write(destination.value, aeg::rhi::ResourceUsage::kCopyDestination,
                              aeg::rhi::PipelineStage::kCopy);
        },
        [&](aeg::render_graph::RenderPassCommandContext& command) {
            return command.copy_texture_to_buffer(color.value, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {}, {8, 8, 1},
                                                  destination.value, readback.value.offset, 0, 0);
        });
    if (!copy)
        return false;
    if (const Result result = graph.compile(); !result) {
        std::cerr << result.message << '\n';
        return false;
    }
    if (graph.compile_info().pass_count != expected_passes + 1 || graph.compile_info().culled_pass_count != 0 ||
        graph.compile_info().resource_count != (expected_passes == 4 ? 9u : 7u))
        return false;
    const auto execution = graph.execute(frame);
    if (!execution) {
        std::cerr << execution.result.message << '\n';
        return false;
    }
    if (execution.value.executed_pass_count != expected_passes + 1 || !frame.end_frame() || !device.wait_idle())
        return false;
    for (size_t i = 0; i < kPassIds.size(); ++i) {
        const unsigned expected = expected_passes == 4 ? (i < 4 ? 1u : 0u) : (i == 0 || i >= 3 ? 1u : 0u);
        if (setups[i] != expected || executions[i] != expected) {
            std::cerr << "Registered pass " << i << " setup/execute: " << setups[i] << '/' << executions[i] << '\n';
            return false;
        }
    }
    std::array<std::byte, 8 * 8 * 4> pixels{};
    if (!frame.read_readback(readback.value, pixels))
        return false;
    for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
        if (std::abs(int(std::to_integer<unsigned>(pixels[pixel])) - 64) > 1 ||
            std::abs(int(std::to_integer<unsigned>(pixels[pixel + 1])) - 128) > 1 ||
            pixels[pixel + 2] != std::byte{255} || pixels[pixel + 3] != std::byte{255})
            return false;
    }
    return bool(frame.drain());
}

}  // namespace

int main() {
    if (std::string_view(AEG_POST_PROCESS_SHADER_PACKAGE_PATH).empty()) {
        std::cerr << "Renderer GPU graph test requires compiled postprocess shaders\n";
        return 77;
    }
    // The variant ForwardOpaque stage must have a reusable indexed-draw entry
    // point instead of being limited to the legacy RenderPipeline binding.
    const auto opaque_builder = &aeg::renderer::build_opaque_render_graph;
    if (opaque_builder == nullptr) {
        return 11;
    }
    const auto gbuffer_builder = &aeg::renderer::build_gbuffer_render_graph;
    if (gbuffer_builder == nullptr) {
        return 12;
    }

    aeg::rhi::DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG Renderer Pipeline Graph Smoke";
    auto device = aeg::rhi::create_device(device_desc);
    if (!device) {
        return 1;
    }

    PipelineRegistry registry;
    if (!aeg::renderer::register_deferred_render_pipeline(registry) ||
        !aeg::renderer::register_forward_render_pipeline(registry) ||
        !aeg::renderer::register_deferred_render_feature(registry) ||
        !aeg::renderer::register_forward_render_feature(registry) ||
        !aeg::renderer::bind_deferred_render_feature(registry) ||
        !aeg::renderer::bind_forward_render_feature(registry)) {
        return 2;
    }
    if (!build_variant(*device.value, registry, aeg::renderer::kDeferredRenderPipelineId,
                       RenderResourceId("Renderer.SceneColor"), RenderResourceId("Renderer.SceneDepth"), 4)) {
        return 3;
    }
    if (!build_variant(*device.value, registry, aeg::renderer::kForwardRenderPipelineId,
                       RenderResourceId("Renderer.SceneColor"), RenderResourceId("Renderer.SceneDepth"), 5)) {
        return 4;
    }

    PipelineScopeHost host(PipelineScope::kFrame, aeg::pipeline::PipelineInstanceId("Renderer.GraphMissing"));
    if (!host.bind_pipeline(aeg::renderer::kDeferredRenderPipelineId) || !host.rebuild(registry)) {
        return 5;
    }
    const PipelinePlanHandle plan = host.active_plan_snapshot();
    aeg::render_graph::RenderGraph missing_graph(*device.value);
    const auto color = missing_graph.create_texture(color_desc(), aeg::render_graph::ResourceLifetime::kPersistent);
    if (!color) {
        return 6;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> missing_bindings{{
        {RenderResourceId("Renderer.SceneColor"), color.value},
    }};
    const RenderSceneSnapshot missing_snapshot =
        make_snapshot(RenderResourceId("Renderer.SceneColor"), RenderResourceId("Renderer.SceneDepth"));
    const RenderFeatureContext missing_context{&missing_snapshot, nullptr, nullptr};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc missing_desc{
        {}, missing_bindings, nullptr, &missing_context, {},
    };
    aeg::render_graph::RenderGraphFeatureRuntime missing_runtime(missing_graph, plan, registry, missing_desc);
    const Result missing_result = missing_runtime.build();
    if (missing_result || missing_result.code != ResultCode::kNotFound || !missing_graph.compile() ||
        missing_graph.compile_info().pass_count != 0 || missing_graph.compile_info().resource_count != 1) {
        return 7;
    }
    aeg::render_graph::RenderGraph foreign_graph(*device.value);
    const auto foreign_depth = foreign_graph.create_texture(depth_desc());
    if (!foreign_depth)
        return 8;
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 2> foreign_bindings{{
        {RenderResourceId("Renderer.SceneColor"), color.value},
        {RenderResourceId("Renderer.SceneDepth"), foreign_depth.value},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc foreign_desc{
        {}, foreign_bindings, nullptr, &missing_context, {},
    };
    aeg::render_graph::RenderGraphFeatureRuntime foreign_runtime(missing_graph, plan, registry, foreign_desc);
    const auto foreign_result = foreign_runtime.build();
    if (foreign_result || foreign_result.code != ResultCode::kNotFound)
        return 9;
    foreign_graph.reset();
    aeg::render_graph::RenderGraphFeatureRuntime stale_runtime(foreign_graph, plan, registry, foreign_desc);
    if (stale_runtime.build())
        return 10;
    return 0;
}
