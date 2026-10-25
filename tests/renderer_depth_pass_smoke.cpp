/** @file renderer_depth_pass_smoke.cpp @brief Verifies geometry, not just a clear, reaches depth. */
#include "feature_clock_test_fixture.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>

#include "feature_render_test_fixture.hpp"
#include "feature_runtime.hpp"
#include "render_feature_draw.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "../Engine/Shader/private/shader_pipeline_layout_access.hpp"
#include "renderer.hpp"
#include "renderer_variants_internal.hpp"
#include "render_pass_registry_execution.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"
#include "shader_contract.hpp"

namespace {
using namespace aeg;
constexpr feature::RenderResourceId kDepth{"DepthSmoke.Target"};
using feature::RenderDrawDepthScene;
constexpr uint32_t kExtent = 32;

bool check(const foundation::Result& result) {
    if (!result)
        std::cerr << result.message << '\n';
    return static_cast<bool>(result);
}

struct Resolver final : renderer::RenderResourceResolver {
    renderer::RenderMeshResource mesh{};
    renderer::RenderMeshResource far_mesh{};
    renderer::RenderObjectResource object{};
    const application::runtime::RuntimeAssetResolver* object_resolver{};
    mutable renderer::RenderView last_object_view{};
    rhi::PipelineHandle depth_pipeline{};
    renderer::RenderMaterialAttachmentContract depth_pipeline_contract{rhi::Format::kUnknown,
        rhi::SampleCount::kX1, rhi::Format::kD32Float};
    renderer::RendererProfilePsoContract depth_profile_contract{};
    uint64_t variant_cache_epoch{1};
    foundation::ResultValue<renderer::RenderMeshResource> resolve_mesh(renderer::MeshAssetId id) const override {
        return {foundation::Result::success(), id == renderer::MeshAssetId(2) ? far_mesh : mesh};
    }
    foundation::ResultValue<renderer::RenderMaterialResource> resolve_material(
        renderer::MaterialInstanceId) const override {
        renderer::RenderMaterialResource material{};
        material.depth_pipeline = depth_pipeline;
        material.depth_pipeline_contract = depth_pipeline_contract;
        material.depth_profile_contract = depth_profile_contract;
        material.variant_cache_epoch = variant_cache_epoch;
        return {foundation::Result::success(), material};
    }
    foundation::ResultValue<renderer::RenderObjectResource> resolve_object(const renderer::RenderInstance& instance,
                                                                           const renderer::RenderView& view) const override {
        if (object_resolver) {
            last_object_view = view;
            return object_resolver->resolve_object(instance, view);
        }
        return {foundation::Result::success(), object};
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
    const auto far_vertex_buffer = device->create_buffer(vertex_desc);
    const auto index_buffer = device->create_buffer(index_desc);
    const auto object_buffer = device->create_buffer(object_desc);
    const auto object_set = device->create_binding_set({object_layout.value, rhi::BindingSetLifetime::kPersistent});
    if (!vertex_buffer || !far_vertex_buffer || !index_buffer || !object_buffer || !object_set)
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
    auto far_vertices = vertices;
    for (auto& vertex : far_vertices) vertex[2] = 0.75f;
    if (!check(device->write_buffer(far_vertex_buffer.value, 0, std::as_bytes(std::span{far_vertices})))) return 7;
    Resolver resolver;
    resolver.depth_pipeline = gpu_pipeline.value;
    const renderer::RendererProfilePsoContract depth_profile_contract{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kShadow,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(layout.value),
         shader::detail::ShaderPipelineLayoutAccess::cache_epoch(layout.value)},
        {rhi::Format::kUnknown, graphics.samples, graphics.depth_stencil_format},
        shader::detail::ShaderPipelineLayoutAccess::cache_epoch(layout.value)};
    resolver.depth_profile_contract = depth_profile_contract;
    resolver.variant_cache_epoch = depth_profile_contract.cache_epoch;
    resolver.mesh = {.vertex_buffer = vertex_buffer.value,
                     .vertex_buffer_desc = vertex_desc,
                     .index_buffer = index_buffer.value,
                     .index_buffer_desc = index_desc,
                     .vertex_stride = 16,
                     .vertex_count = 3,
                     .index_count = 3};
    resolver.object = {object_buffer.value, object_desc, object_layout.value, object_set.value, 0, 0,
                       sizeof(constants)};
    resolver.far_mesh = resolver.mesh;
    resolver.far_mesh.vertex_buffer = far_vertex_buffer.value;

    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    pipeline::RenderPassRegistry native_passes;
    if (!check(renderer::register_builtin_render_passes(native_passes))) return 12;
    const render_graph::RenderPassExecutionHost pass_host{&native_passes,
        pipeline::detail::RenderPassRegistryExecution::execution_epoch(native_passes)};
    feature::FeatureRuntime runtime(registry);
    uint32_t scenario = 0;
    auto source = render_feature_test::definition();
    source.contract.permissions.capability_mask = feature::to_mask(feature::RenderIntentCapability::kDrawScene) |
        feature::to_mask(feature::RenderIntentCapability::kWriteExternal) |
        feature::to_mask(feature::RenderIntentCapability::kReadExternal) |
        feature::to_mask(feature::RenderIntentCapability::kExportOutput);
    source.execute = [&scenario](const feature::InvocationToken& token, const feature::StateSnapshot&,
                                std::span<const uint8_t>, std::span<const uint8_t>) {
        using namespace feature;
        const RenderTextureReference target{RenderResourceId(kDepth.value()), 1, true};
        const RenderDrawDepthScene geometry{{RenderResourceId("Depth.Scene"), 1},
            {target, RenderAttachmentLoad::kClear, 1.f}};
        const RenderDrawDepthScene empty{{RenderResourceId("Empty.Scene"), 1},
            {target, RenderAttachmentLoad::kClear, 0.4f}};
        auto shadow = geometry;
        shadow.projection = RenderDepthSceneProjection::kDirectionalShadow;
        auto empty_shadow = empty;
        empty_shadow.projection = RenderDepthSceneProjection::kDirectionalShadow;
        auto no_light = shadow;
        no_light.scene.id = RenderResourceId("NoLight.Scene");
        no_light.depth.load = RenderAttachmentLoad::kLoad;
        RenderIntent intent{};
        intent.depth_output = target;
        intent.operations = {geometry};
        switch (scenario) {
            case 1: intent.operations = {empty}; break;
            case 2: {
                RenderDrawScene seed{{RenderResourceId("Empty.Scene"), 1},
                    {RenderResourceId("Depth.Color"), 1, true}};
                seed.depth = empty.depth;
                auto load = geometry;
                load.depth.load = RenderAttachmentLoad::kLoad;
                intent.operations = {seed, load};
                break;
            }
            case 3: {
                auto seed = empty;
                seed.depth.clear_depth = 0.1f;
                auto load = geometry;
                load.depth.load = RenderAttachmentLoad::kLoad;
                intent.operations = {seed, load};
                break;
            }
            case 4: {
                auto load = empty;
                load.depth.load = RenderAttachmentLoad::kLoad;
                intent.operations = {geometry, load};
                break;
            }
            case 5: {
                auto clear = empty;
                clear.depth.clear_depth = 0.6f;
                intent.operations = {geometry, clear};
                break;
            }
            case 7: case 8: intent.operations = {shadow}; break;
            case 9: shadow.depth.clear_depth = .4f; intent.operations = {shadow}; break;
            case 10: intent.operations = {shadow, no_light}; break;
            case 11: empty_shadow.depth.clear_depth = .1f; intent.operations = {empty_shadow, no_light}; break;
            case 12: empty_shadow.depth.clear_depth = .6f; intent.operations = {empty_shadow}; break;
            case 13: intent.operations = {geometry, shadow}; break;
            case 14: shadow.depth.clear_depth = .8f; intent.operations = {shadow}; break;
            case 15: shadow.depth.clear_depth = .7f; intent.operations = {shadow}; break;
        }
        StateDelta delta{token.state_version};
        delta.render_intent = std::move(intent);
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    if (!check(aeg::feature::test::register_clock_test_definition(registry, source))) return 8;
    auto document = render_feature_test::document(source);
    document.nodes.resize(1);
    auto compiled = feature::FeatureGraphCompiler{}.compile(document, registry);
    if (!compiled || !check(runtime.publish_graph(document, compiled.value))) return 9;
    render_graph::FrameContext frame(*device, {.frames_in_flight = 1});
    render_graph::RenderGraph graph(*device);
    application::runtime::RuntimeAssetResolver object_resolver;
    application::runtime::RuntimeAssetResolver::UploadContext upload_context{
        .device = device.get(), .frame_context = &frame, .object_binding_layout = object_layout.value};
    object_resolver.set_upload_context(upload_context);
    for (scenario = 0; scenario < 16; ++scenario) {
        resolver.object_resolver = scenario >= 7 ? &object_resolver : nullptr;
        if (!check(frame.begin_frame())) return 10;
        rhi::TextureDesc depth_desc{};
        depth_desc.extent = {kExtent, kExtent, 1};
        depth_desc.format = rhi::Format::kD32Float;
        depth_desc.capabilities = rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kTransferSource;
        const auto depth = graph.create_texture(depth_desc);
        auto color_desc = depth_desc;
        color_desc.format = rhi::Format::kRgba8Unorm;
        color_desc.capabilities = rhi::TextureCapability::kColorAttachment;
        const auto color = graph.create_texture(color_desc);
        if (!depth || !color) return 10;
        const auto readback = frame.allocate_readback(kExtent * kExtent * sizeof(float), 256);
        if (!readback) return 11;
        rhi::BufferDesc readback_desc{};
        readback_desc.size = readback.value.buffer_size;
        readback_desc.capabilities = rhi::BufferCapability::kTransferDestination;
        readback_desc.memory = rhi::MemoryClass::kReadback;
        const auto destination = graph.import_buffer(readback.value.buffer, readback_desc);
        if (!destination) return 12;
        auto snapshot = std::make_shared<renderer::RenderSceneSnapshot>();
        snapshot->world = {0, 1};
        snapshot->world_revision = 1;
        snapshot->frame_number = frame.frame_number();
        snapshot->view.world = snapshot->world;
        snapshot->view.id = renderer::RenderViewId("DepthSmoke.View");
        snapshot->view.output_extent = {kExtent, kExtent};
        snapshot->instances.push_back({.object = renderer::RenderObjectId(1),
            .mesh = renderer::MeshAssetId(1), .material = renderer::MaterialInstanceId(1)});
        snapshot->instances.push_back({.object = renderer::RenderObjectId(2),
            .mesh = renderer::MeshAssetId(2), .material = renderer::MaterialInstanceId(1)});
        if (scenario >= 7) {
            snapshot->instances[0].bounds_center = {0, 0, .25f};
            snapshot->instances[1].bounds_center = {0, 0, .75f};
            for (auto& instance : snapshot->instances) instance.bounds_radius = 1.25f;
            snapshot->lights.push_back({.light = renderer::RenderLightId(1),
                .type = renderer::RenderLightType::kDirectional, .direction = {1, 0, -1},
                .intensity = 1.f, .casts_shadow = true});
            if (scenario == 8) {
                snapshot->shadow_casters = snapshot->instances;
                for (auto& caster : snapshot->shadow_casters) caster.world_transform = math::translation({3, 0, 0});
                snapshot->instances.clear();
            }
            if (scenario == 9) snapshot->lights.clear();
            if (scenario == 14) snapshot->lights.front().casts_shadow = false;
            if (scenario == 15) snapshot->lights.front().intensity = 0.f;
        }
        auto empty = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
        empty->instances.clear();
        empty->shadow_casters.clear();
        auto no_light = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
        no_light->lights.clear();
        std::array scenes{
            renderer::RenderFeatureSceneBinding{feature::RenderResourceId("Depth.Scene"), 1, {}, snapshot,
                scenario == 9 || scenario >= 14 ? nullptr : &resolver},
            renderer::RenderFeatureSceneBinding{feature::RenderResourceId("Empty.Scene"), 1, {}, empty, nullptr},
            renderer::RenderFeatureSceneBinding{feature::RenderResourceId("NoLight.Scene"), 1, {}, no_light, nullptr}};
        renderer::RenderFeatureDrawLowerer lowerer(scenes, pass_host);
        const std::array textures{
            render_graph::RenderGraphFeatureTextureBinding{kDepth, depth.value},
            render_graph::RenderGraphFeatureTextureBinding{feature::RenderResourceId("Depth.Color"), color.value}};
        if (!check(runtime.tick(source.contract.clock.id))) return 13;
        auto captured = runtime.capture_render_outputs(compiled.value);
        if (!captured) return 13;
        auto outputs = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(captured.value));
        if (scenario == 0) {
            std::array<render_graph::RenderGraphTexture, 7> incompatible{};
            for (uint32_t index = 0; index < incompatible.size(); ++index) {
                auto desc = depth_desc;
                switch (index) {
                    case 0: desc.format = rhi::Format::kRgba8Unorm;
                            desc.capabilities = rhi::TextureCapability::kColorAttachment; break;
                    case 1: desc.capabilities = rhi::TextureCapability::kTransferSource; break;
                    case 2: ++desc.extent.width; break;
                    case 3: desc.mip_levels = 2; break;
                    case 4: desc.array_layers = 2; break;
                    case 5: desc.samples = rhi::SampleCount::kX4; break;
                    case 6: desc.dimension = rhi::TextureDimension::kTex3D; desc.extent.depth = 2; break;
                }
                const auto target = graph.create_texture(desc);
                if (!target) return 19;
                incompatible[index] = target.value;
            }
            if (!check(graph.compile())) return 19;
            const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
            for (uint32_t test_case = 0; test_case < 59; ++test_case) {
                auto rejected_outputs = *outputs;
                auto& output = rejected_outputs.front();
                auto draw = std::get<feature::RenderDrawDepthScene>(output.intent.operations.front());
                draw.scene.id = feature::RenderResourceId("Invalid.Scene");
                draw.depth.target.id = feature::RenderResourceId("Invalid.Depth");
                std::vector<renderer::RenderFeatureSceneBinding> invalid_scenes(scenes.begin(), scenes.end());
                invalid_scenes.push_back(scenes.front());
                invalid_scenes.back().id = draw.scene.id;
                std::vector<render_graph::RenderGraphFeatureTextureBinding> bindings(textures.begin(), textures.end());
                bindings.push_back({feature::RenderResourceId(draw.depth.target.id.value()), depth.value});
                auto invalid_resolver = resolver;
                if (test_case >= 45)
                    invalid_scenes.back().resolver = &invalid_resolver;
                std::shared_ptr<renderer::RenderSceneSnapshot> invalid_shadow;
                if (test_case >= 39 && test_case <= 44) {
                    draw.projection = feature::RenderDepthSceneProjection::kDirectionalShadow;
                    invalid_shadow = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                    invalid_shadow->lights.push_back({.light = renderer::RenderLightId(1),
                        .type = renderer::RenderLightType::kDirectional, .direction = {1, 0, -1},
                        .intensity = 1.f, .casts_shadow = true});
                    for (auto& instance : invalid_shadow->instances) instance.bounds_radius = 1.25f;
                    invalid_scenes.back().snapshot = invalid_shadow;
                }
                switch (test_case) {
                    case 0: invalid_scenes.pop_back(); break;
                    case 1: ++draw.scene.version; break;
                    case 2: invalid_scenes.back().owner = feature::FeatureInstanceId(output.instance_id.value() + 100); break;
                    case 3: {
                        auto stale = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                        ++stale->frame_number;
                        invalid_scenes.back().snapshot = stale;
                        break;
                    }
                    case 4: invalid_scenes.back().resolver = nullptr; break;
                    case 5: bindings.pop_back(); break;
                    case 6: ++draw.depth.target.version; break;
                    case 7: bindings.back().owner = feature::FeatureInstanceId(output.instance_id.value() + 100); break;
                    case 8: bindings.push_back(bindings.back()); break;
                    case 9: draw.depth.target.external = false; break;
                    case 10: draw.depth.clear_depth = std::numeric_limits<float>::quiet_NaN(); break;
                    case 11: draw.depth.clear_depth = -.1f; break;
                    case 12: draw.depth.clear_depth = 1.1f; break;
                    case 13: draw.depth.load = static_cast<feature::RenderAttachmentLoad>(99); break;
                    case 14: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kDrawScene); break;
                    case 15: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kWriteExternal); break;
                    case 16: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kReadExternal);
                             draw.depth.load = feature::RenderAttachmentLoad::kLoad; break;
                    case 17: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kExportOutput); break;
                    case 18: output.intent.depth_output->external = false; break;
                    case 19: output.intent.depth_output->id = feature::RenderResourceId("Depth.Color"); break;
                    case 20: output.intent.final_output = output.intent.depth_output; break;
                    case 28: resolver.depth_pipeline = {}; break;
                    case 29: resolver.depth_pipeline_contract.color_format = rhi::Format::kRgba8Unorm; break;
                    case 30: resolver.depth_pipeline_contract.samples = rhi::SampleCount::kX4; break;
                    case 31: resolver.depth_pipeline_contract.depth_stencil_format = rhi::Format::kUnknown; break;
                    case 32: resolver.mesh.vertex_layout.position_format = rhi::Format::kRgb32Float; break;
                    case 33: resolver.object.transform_size = object_desc.size + 4; break;
                    case 34: ++bindings.back().texture.generation; break;
                    case 35: ++output.intent.depth_output->version; break;
                    case 36: break; // no lowerer
                    case 37: invalid_scenes.push_back(invalid_scenes.back()); break;
                    case 38: draw.projection = static_cast<feature::RenderDepthSceneProjection>(99); break;
                    case 39: invalid_shadow->lights.front().direction = {}; break;
                    case 40: invalid_shadow->view.camera.projection = {}; break;
                    case 41: invalid_shadow->instances.front().bounds_radius = std::numeric_limits<float>::quiet_NaN(); break;
                    case 42: invalid_shadow->instances.front().bounds_radius = -1.f; break;
                    case 43: invalid_shadow->instances.front().world_transform.at(0, 3) = std::numeric_limits<float>::quiet_NaN(); break;
                    case 44: invalid_shadow->shadow_casters = invalid_shadow->instances;
                             invalid_shadow->instances.clear(); invalid_scenes.back().resolver = nullptr; break;
                    case 45: invalid_resolver.depth_profile_contract = {}; break;
                    case 46: invalid_resolver.depth_profile_contract.shader.profile = renderer::RendererProfile::kGBuffer; break;
                    case 47: ++invalid_resolver.depth_profile_contract.shader.version; break;
                    case 48: invalid_resolver.depth_profile_contract.shader.layout_hash = 0; break;
                    case 49: invalid_resolver.depth_profile_contract.shader.cache_epoch = 0; break;
                    case 50: invalid_resolver.depth_profile_contract.cache_epoch = 0; break;
                    case 51: ++invalid_resolver.depth_profile_contract.cache_epoch; break;
                    case 52: ++invalid_resolver.depth_profile_contract.shader.cache_epoch;
                             ++invalid_resolver.depth_profile_contract.cache_epoch; break;
                    case 53: invalid_resolver.variant_cache_epoch = 0; break;
                    case 54: invalid_resolver.depth_profile_contract.attachment.color_format = rhi::Format::kRgba8Unorm; break;
                    case 55: invalid_resolver.depth_profile_contract.attachment.samples = rhi::SampleCount::kX4; break;
                    case 56: invalid_resolver.depth_profile_contract.attachment.depth_stencil_format = rhi::Format::kUnknown; break;
                    case 57: invalid_resolver.depth_profile_contract.attachment.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
                    case 58: invalid_resolver.depth_profile_contract.attachment.depth_stencil_format = rhi::Format::kD24UnormS8Uint; break;
                    default: bindings.back().texture = incompatible[test_case - 21]; break;
                }
                output.intent.operations.push_back(draw);
                renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes, pass_host);
                const auto rejected = render_graph::build_render_graph_features(graph,
                    std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(rejected_outputs)),
                    {.textures = bindings, .draw_lowerer = test_case == 36 ? nullptr : &invalid_lowerer}, frame.frame_number());
                resolver.depth_pipeline = gpu_pipeline.value;
                resolver.depth_pipeline_contract = {rhi::Format::kUnknown, rhi::SampleCount::kX1, rhi::Format::kD32Float};
                resolver.depth_profile_contract = depth_profile_contract;
                resolver.variant_cache_epoch = depth_profile_contract.cache_epoch;
                resolver.mesh.vertex_layout.position_format = rhi::Format::kRgba32Float;
                resolver.object.transform_size = sizeof(constants);
                if (rejected || !check(graph.compile()) || aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != baseline.pass_count ||
                    aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != baseline.resource_count) {
                    std::cerr << "Invalid standalone depth case accepted or leaked: " << test_case << '\n';
                    return 19;
                }
            }
        }
        if (!check(render_graph::build_render_graph_features(graph, outputs,
            {.textures = textures, .draw_lowerer = &lowerer}, frame.frame_number())) || !check(graph.compile())) return 13;
        if (scenario == 13) {
            const auto camera_object = object_resolver.resolve_object(snapshot->instances.front(), snapshot->view);
            const auto shadow_object = object_resolver.resolve_object(snapshot->instances.front(), resolver.last_object_view);
            if (!camera_object || !shadow_object || resolver.last_object_view.id == snapshot->view.id ||
                camera_object.value.transform_buffer == shadow_object.value.transform_buffer ||
                camera_object.value.binding_set == shadow_object.value.binding_set) return 23;
        }
        bool tagged = false;
        for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
            if (!pass.feature_work) continue;
            const auto& work = *pass.feature_work;
            if (work.feature_definition_id != source.id || work.owner_module != source.owner_module ||
                work.node_id != outputs->front().node_id || work.instance_id != outputs->front().instance_id ||
                work.invocation.invocation_id != outputs->front().invocation.invocation_id ||
                work.frame_index != frame.frame_number()) return 20;
            tagged |= pass.live;
        }
        if (!tagged || aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.empty()) return 20; // depth_output must anchor the draw itself.
        const auto copy = graph.add_pass("Depth.Readback", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                if (const auto r = pass.read(depth.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !r) return r;
                if (const auto r = pass.write(destination.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy); !r) return r;
                pass.set_side_effects();
                return foundation::Result::success();
            }, [&](render_graph::RenderPassCommandContext& command) {
                return command.copy_texture_to_buffer(depth.value, {rhi::Aspect::kDepth, 0, 0, 1}, {},
                    {kExtent, kExtent, 1}, destination.value, readback.value.offset, 0, 0);
            });
        if (!copy || !check(graph.compile())) return 14;
        const auto executed = graph.execute(frame);
        if (!executed || !check(frame.end_frame()) || !check(device->wait_idle())) return 15;
        std::array<std::byte, kExtent * kExtent * sizeof(float)> pixels{};
        if (!check(frame.read_readback(readback.value, pixels))) return 16;
        float center{}, corner{};
        std::memcpy(&center, pixels.data() + ((kExtent / 2) * kExtent + kExtent / 2) * sizeof(float), sizeof(float));
        std::memcpy(&corner, pixels.data(), sizeof(float));
        constexpr std::array expected_center{.25f, .4f, .25f, .1f, .25f, .6f, .25f};
        constexpr std::array expected_corner{1.f, .4f, .4f, .1f, 1.f, .6f, 1.f};
        if (scenario < 7 && (std::abs(center - expected_center[scenario]) > .001f || std::abs(corner - expected_corner[scenario]) > .001f)) {
            std::cerr << "Depth scenario " << scenario << " got " << center << '/' << corner << '\n';
            return 17;
        }
        if (scenario == 1 || scenario == 3 || scenario == 5)
            for (std::size_t offset = 0; offset < pixels.size(); offset += sizeof(float)) {
                float value{};
                std::memcpy(&value, pixels.data() + offset, sizeof(float));
                if (std::abs(value - expected_center[scenario]) > .001f) return 17;
            }
        if (scenario >= 7) {
            // Light direction (1,0,-1), bound radius R=1.05*sqrt(86)/4.
            // At pixel u=1/32, the z=.75 plane wins at .5+u/2-.25/(sqrt(2)*R).
            constexpr std::array shadow_center{.4430066332f, .4430066332f, .4f, .4430066332f,
                .1f, .6f, .4430066332f, .8f, .7f};
            constexpr std::array shadow_background{1.f, 1.f, .4f, 1.f, .1f, .6f, 1.f, .8f, .7f};
            float side{};
            std::memcpy(&side, pixels.data() + ((kExtent / 2) * kExtent + 20) * sizeof(float), sizeof(float));
            const auto index = scenario - 7;
            if (!std::isfinite(center) || !std::isfinite(corner) || !std::isfinite(side) ||
                std::abs(center - shadow_center[index]) > .001f ||
                std::abs(corner - shadow_background[index]) > .001f ||
                std::abs(side - shadow_background[index]) > .001f) {
                std::cerr << "Shadow projection scenario " << scenario << " got " << center << '/' << corner << '/' << side << '\n';
                return 24;
            }
            if (scenario == 9 || scenario == 11 || scenario == 12 || scenario >= 14)
                for (std::size_t offset = 0; offset < pixels.size(); offset += sizeof(float)) {
                    float value{};
                    std::memcpy(&value, pixels.data() + offset, sizeof(float));
                    if (!std::isfinite(value) || std::abs(value - shadow_center[index]) > .001f) return 24;
                }
        }
        std::weak_ptr<const void> weak_outputs = outputs;
        std::weak_ptr<const void> weak_scene = scenario == 1 || scenario == 11 || scenario == 12 ? empty : snapshot;
        graph.reset();
        outputs.reset();
        snapshot.reset();
        empty.reset();
        no_light.reset();
        for (auto& scene : scenes) scene.snapshot.reset();
        if (weak_outputs.expired() || weak_scene.expired()) return 21;
        if (!check(frame.drain()) || !weak_outputs.expired() || !weak_scene.expired()) return 22;
    }
    if (!check(object_resolver.retire_gpu_resources())) return 25;
    if (!check(device->destroy_binding_set(object_set.value)) || !check(device->destroy_pipeline(gpu_pipeline.value)) ||
        !check(device->destroy_buffer(vertex_buffer.value)) || !check(device->destroy_buffer(far_vertex_buffer.value)) ||
        !check(device->destroy_buffer(index_buffer.value)) || !check(device->destroy_buffer(object_buffer.value))) return 18;
    std::cout << "Feature camera/shadow depth, off-camera casters, Load/Clear and retirement readback passed\n";
    return 0;
}
