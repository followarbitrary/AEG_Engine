/** @brief Checks actual caster occlusion in both builtin renderer variants. */
#include <array>
#include <cmath>
#include <iostream>
#include <map>

#include "deferred_lighting_resources.hpp"
#include "light_culling.hpp"
#include "post_process.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"
#include "renderer_variants.hpp"
#include "shader_contract.hpp"
#include "shadow_rendering.hpp"

namespace {
using namespace aeg;
constexpr uint32_t kSize = 128;
constexpr pipeline::RenderResourceId kColor{"ShadowTest.Color"}, kDepth{"ShadowTest.Depth"}, kLight{"ShadowTest.Light"};
bool check(const foundation::Result& r) {
    if (!r)
        std::cerr << r.message << '\n';
    return bool(r);
}
rhi::BufferDesc upload_desc(uint64_t size, rhi::BufferCapability capability, rhi::ResourceUsage usage) {
    rhi::BufferDesc desc{};
    desc.size = size;
    desc.capabilities = capability;
    desc.memory = rhi::MemoryClass::kUpload;
    desc.initial_usage = usage;
    desc.initial_queue = rhi::QueueClass::kGraphics;
    return desc;
}
struct Resolver final : renderer::RenderResourceResolver {
    rhi::Device& device;
    render_graph::FrameContext& frame;
    renderer::ShadowResources& shadows;
    renderer::DeferredLightingResources& deferred;
    renderer::LightCullingResources& culling;
    renderer::PostProcessResources& post;
    renderer::RenderPostProcessSettings post_settings{};
    renderer::RenderMeshResource mesh{};
    renderer::RenderMaterialResource material{};
    renderer::RenderLightingResource lighting{};
    rhi::BindingLayoutHandle object_layout{};
    mutable std::map<std::pair<uint64_t, uint64_t>, renderer::RenderObjectResource> objects{};
    mutable renderer::RenderShadowResource last_shadow{};
    mutable renderer::RenderLightCullingResource last_culling{};
    mutable renderer::RenderPostProcessResource last_post{};
    Resolver(rhi::Device& d, render_graph::FrameContext& f, renderer::ShadowResources& s,
             renderer::DeferredLightingResources& l, renderer::LightCullingResources& c,
             renderer::PostProcessResources& p)
        : device(d), frame(f), shadows(s), deferred(l), culling(c), post(p) {}
    foundation::ResultValue<renderer::RenderPostProcessResource> resolve_post_process(
        const renderer::RenderView& view) const override {
        const auto result = post.for_frame(frame, view, post_settings);
        if (result)
            last_post = result.value;
        return result;
    }
    foundation::ResultValue<renderer::RenderMeshResource> resolve_mesh(renderer::MeshAssetId) const override {
        return {foundation::Result::success(), mesh};
    }
    foundation::ResultValue<renderer::RenderMaterialResource> resolve_material(
        renderer::MaterialInstanceId) const override {
        return {foundation::Result::success(), material};
    }
    foundation::ResultValue<renderer::RenderLightingResource> resolve_lighting(
        const renderer::RenderSceneSnapshot&, const renderer::RenderView&) const override {
        return {foundation::Result::success(), lighting};
    }
    foundation::ResultValue<renderer::RenderDeferredLightingResource> resolve_deferred_lighting(
        const renderer::RenderView&) const override {
        return deferred.for_frame(frame);
    }
    foundation::ResultValue<renderer::RenderShadowResource> resolve_shadow(
        const renderer::RenderShadowView& shadow) const override {
        const auto result = shadows.for_frame(frame, shadow.constants);
        if (result)
            last_shadow = result.value;
        return result;
    }
    foundation::ResultValue<renderer::RenderLightCullingResource> resolve_light_culling(
        const renderer::RenderSceneSnapshot& scene, const renderer::RenderShadowView& shadow) const override {
        const auto result = culling.for_frame(frame, scene, shadow);
        if (result)
            last_culling = result.value;
        return result;
    }
    foundation::ResultValue<renderer::RenderObjectResource> resolve_object(
        const renderer::RenderInstance& instance, const renderer::RenderView& view) const override {
        const auto key = std::pair{view.id.value(), instance.object.value()};
        if (auto found = objects.find(key); found != objects.end())
            return {foundation::Result::success(), found->second};
        const renderer::RenderObjectConstants constants{view.camera.view_projection() * instance.world_transform,
                                                        math::identity()};
        const auto desc =
            upload_desc(sizeof(constants), rhi::BufferCapability::kUniform, rhi::ResourceUsage::kUniformRead);
        const auto buffer = device.create_buffer(desc);
        const auto set = device.create_binding_set({object_layout, rhi::BindingSetLifetime::kPersistent});
        if (!buffer || !set)
            return {foundation::invalid_state("object allocation failed"), {}};
        rhi::BindingWrite write{};
        write.location = {0, 0};
        write.resource.type = rhi::BindingType::kUniformBuffer;
        write.resource.buffer_range = {buffer.value, 0, sizeof(constants)};
        if (const auto r = device.write_buffer(buffer.value, 0, std::as_bytes(std::span{&constants, 1})); !r)
            return {r, {}};
        if (const auto r = device.update_binding_set(set.value, std::span{&write, 1}); !r)
            return {r, {}};
        if (const auto r = frame.defer_destroy_binding_set(set.value); !r)
            return {r, {}};
        if (const auto r = frame.defer_destroy_buffer(buffer.value); !r)
            return {r, {}};
        renderer::RenderObjectResource resource{buffer.value, desc, object_layout, set.value, 0, 0, sizeof(constants)};
        objects.emplace(key, resource);
        return {foundation::Result::success(), resource};
    }
};
}  // namespace

int main(int argc, char** argv) {
    const bool post_process = argc > 1 && (std::string_view(argv[1]) == "--post-process" ||
                                           std::string_view(argv[1]) == "--deferred-post-process");
    const bool deferred_mode = argc > 1 && (std::string_view(argv[1]) == "--deferred" ||
                                            std::string_view(argv[1]) == "--deferred-post-process");
    const bool local_lights = argc > 1 && std::string_view(argv[1]) == "--local-lights";
    const auto created = rhi::create_device({rhi::BackendType::kVulkan, "Shadow pixels"});
    if (!created)
        return 1;
    auto& device = *created.value;
    shader::ShaderManager shaders(device);
    renderer::DeferredLightingResources deferred;
    renderer::ShadowResources shadows;
    renderer::LightCullingResources culling;
    renderer::PostProcessResources post;
    if (!check(post.initialize(device, AEG_POST_PROCESS_SHADER_PACKAGE_PATH, rhi::Format::kRgba8Unorm)))
        return 32;
    if (!check(deferred.initialize(device, AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH, renderer::kSceneLightingFormat,
                                   AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH)))
        return 2;
    render_graph::FrameContext frame(device, {.frames_in_flight = 2});
    render_graph::RenderGraph graph(device);
    Resolver resolver(device, frame, shadows, deferred, culling, post);
    const std::array<rhi::VertexBindingDesc, 1> bindings{{{0, 36, rhi::VertexInputRate::kVertex}}};
    const std::array<rhi::VertexAttributeDesc, 3> attributes{{{0, 0, rhi::Format::kRgba32Float, 0},
                                                              {1, 0, rhi::Format::kRg32Float, 16},
                                                              {2, 0, rhi::Format::kRgb32Float, 24}}};
    const std::array colors{rhi::Format::kRgba8Unorm};
    rhi::GraphicsPipelineState graphics{};
    graphics.vertex_bindings = bindings;
    graphics.vertex_attributes = attributes;
    graphics.cull_mode = rhi::CullMode::kNone;
    graphics.depth_test_enable = true;
    graphics.depth_write_enable = true;
    graphics.depth_stencil_format = rhi::Format::kD32Float;
    graphics.color_formats = colors;
    const auto opaque_program = shader::load_shader_program(AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH);
    const auto shadow_program = shader::load_shader_program(AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH);
    const auto gbuffer_program = shader::load_shader_program(AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH);
    const auto depth_program = shader::load_shader_program(AEG_DEPTH_MESH_SHADER_PACKAGE_PATH);
    const auto clustered_program = shader::load_shader_program(AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH);
    if (!opaque_program || !shadow_program || !gbuffer_program || !depth_program || !clustered_program)
        return 3;
    const auto opaque_layout = shaders.build_pipeline_layout(opaque_program.value);
    const auto shadow_layout = shaders.build_pipeline_layout(shadow_program.value);
    const auto gbuffer_layout = shaders.build_pipeline_layout(gbuffer_program.value);
    const auto depth_layout = shaders.build_pipeline_layout(depth_program.value);
    const auto clustered_layout = shaders.build_pipeline_layout(clustered_program.value);
    if (!opaque_layout || !shadow_layout || !gbuffer_layout || !depth_layout || !clustered_layout)
        return 4;
    const auto clustered_inputs = shaders.binding_layout(clustered_layout.value, 3);
    if (!clustered_inputs ||
        !check(culling.initialize(device, AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH, clustered_inputs.value)))
        return 4;
    const auto shadow_inputs = shaders.binding_layout(shadow_layout.value, 3);
    if (!shadow_inputs || !check(shadows.initialize(device, shadow_inputs.value)))
        return 4;
    const auto opaque =
        shaders.create_pipeline(opaque_program.value, opaque_layout.value, {rhi::PipelineType::kGraphics, graphics});
    const auto shadow_opaque =
        shaders.create_pipeline(shadow_program.value, shadow_layout.value, {rhi::PipelineType::kGraphics, graphics});
    const std::array hdr_colors{renderer::kSceneLightingFormat};
    graphics.color_formats = hdr_colors;
    const auto clustered = shaders.create_pipeline(clustered_program.value, clustered_layout.value,
                                                   {rhi::PipelineType::kGraphics, graphics});
    const std::array mrt{rhi::Format::kRgba16Float, rhi::Format::kRgba16Float, rhi::Format::kRgba8Unorm};
    graphics.color_formats = mrt;
    const auto gbuffer =
        shaders.create_pipeline(gbuffer_program.value, gbuffer_layout.value, {rhi::PipelineType::kGraphics, graphics});
    graphics.color_formats = {};
    graphics.vertex_attributes = std::span{attributes.data(), 1};
    const auto depth_pipeline =
        shaders.create_pipeline(depth_program.value, depth_layout.value, {rhi::PipelineType::kGraphics, graphics});
    if (!opaque || !shadow_opaque || !gbuffer || !depth_pipeline || !clustered)
        return 5;
    resolver.object_layout = shaders.binding_layout(opaque_layout.value, 0).value;
    const auto material_layout = shaders.binding_layout(opaque_layout.value, 1).value;
    const auto lighting_layout = shaders.binding_layout(opaque_layout.value, 2).value;
    const std::array<std::array<float, 9>, 4> vertices{{{-1, -1, 0, 1, 0, 0, 0, 0, 1},
                                                        {1, -1, 0, 1, 1, 0, 0, 0, 1},
                                                        {1, 1, 0, 1, 1, 1, 0, 0, 1},
                                                        {-1, 1, 0, 1, 0, 1, 0, 0, 1}}};
    const std::array<uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
    const auto vertex_desc =
        upload_desc(sizeof(vertices), rhi::BufferCapability::kVertex, rhi::ResourceUsage::kVertexInput);
    const auto index_desc =
        upload_desc(sizeof(indices), rhi::BufferCapability::kIndex, rhi::ResourceUsage::kIndexInput);
    const auto vertex = device.create_buffer(vertex_desc), index = device.create_buffer(index_desc);
    if (!vertex || !index || !check(device.write_buffer(vertex.value, 0, std::as_bytes(std::span{vertices}))) ||
        !check(device.write_buffer(index.value, 0, std::as_bytes(std::span{indices}))))
        return 6;
    resolver.mesh = {.vertex_buffer = vertex.value,
                     .vertex_buffer_desc = vertex_desc,
                     .index_buffer = index.value,
                     .index_buffer_desc = index_desc,
                     .vertex_stride = 36,
                     .vertex_count = 4,
                     .index_count = 6};
    const auto sampler = device.create_sampler({});
    if (!sampler)
        return 7;
    pipeline::PipelineRegistry registry;
    pipeline::RenderPassRegistry passes;
    if (!check(renderer::register_builtin_rendering(registry, passes)) ||
        !check(deferred_mode ? renderer::bind_deferred_render_feature(registry)
                             : renderer::bind_forward_render_feature(registry)))
        return 8;
    pipeline::PipelineScopeHost host(pipeline::PipelineScope::kFrame, pipeline::PipelineInstanceId("ShadowTest"));
    if (!check(host.bind_pipeline(deferred_mode ? renderer::kDeferredRenderPipelineId
                                                : renderer::kForwardRenderPipelineId)) ||
        !check(host.rebuild(registry)))
        return 9;
    renderer::RenderSceneSnapshot scene{};
    scene.world = {0, 1};
    scene.world_revision = 1;
    scene.view.world = scene.world;
    scene.view.id = renderer::RenderViewId("ShadowTest.Camera");
    scene.view.output_extent = {kSize, kSize};
    scene.view.color_target = kColor;
    scene.view.depth_target = kDepth;
    scene.view.camera.view = math::look_at({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    scene.view.camera.projection = math::orthographic(-1, 1, -1, 1, 0.1f, 5);
    scene.instances = {{.object = renderer::RenderObjectId(1),
                        .mesh = renderer::MeshAssetId(1),
                        .material = renderer::MaterialInstanceId(1),
                        .world_transform = math::scale({0.9f, 0.9f, 1}),
                        .bounds_radius = 1.42f},
                       {.object = renderer::RenderObjectId(2),
                        .mesh = renderer::MeshAssetId(1),
                        .material = renderer::MaterialInstanceId(1),
                        .world_transform = math::translation({-0.45f, 0, 0.6f}) * math::scale({0.15f, 0.15f, 1}),
                        .bounds_radius = 0.22f}};
    scene.lights = {{.light = renderer::RenderLightId(1),
                     .type = renderer::RenderLightType::kDirectional,
                     .direction = {0.5f, 0, -1},
                     .intensity = 0.8f}};
    if (local_lights) {
        scene.instances.resize(1);
        scene.lights[0].intensity = 0;
        scene.lights.push_back(
            {.light = renderer::RenderLightId(2), .position = {-0.15f, 0, 0.4f}, .color = {1, 0, 0}, .range = 0.6f});
        scene.lights.push_back({.light = renderer::RenderLightId(3),
                                .type = renderer::RenderLightType::kSpot,
                                .position = {0.4f, 0, 0.5f},
                                .direction = {0, 0, -1},
                                .color = {0, 0, 1},
                                .range = 1,
                                .inner_cone_cosine = 0.99f,
                                .outer_cone_cosine = 0.95f});
    }
    std::array<int, 3> dark_pixels{}, lit_pixels{}, blue_pixels{}, background_pixels{};
    for (size_t iteration = 0; iteration != 3; ++iteration) {
        if (!check(frame.begin_frame()))
            return 10;
        resolver.objects.clear();
        scene.lights.front().casts_shadow = iteration != 0;
        if (post_process) {
            scene.instances.resize(1);
            scene.lights.front().casts_shadow = false;
            scene.lights.front().intensity = 4;
            scene.lights.front().direction = {0, 0, -1};  // Analytic HDR input: 4 direct + 0.1 ambient.
            resolver.post_settings.sky_enabled = true;
            resolver.post_settings.tone_mapping = true;
            resolver.post_settings.sky_zenith = resolver.post_settings.sky_horizon =
                resolver.post_settings.sky_ground = {0.25f, 0.5f, 1};
            resolver.post_settings.exposure = iteration == 1 ? 0.5f : 1;
            resolver.post_settings.gamma = iteration == 2 ? 2 : 1;
        } else if (local_lights) {
            scene.lights[1].intensity = scene.lights[2].intensity = iteration == 0 ? 0.0f : 1.0f;
            if (iteration == 2) {
                scene.lights[1].position.x = 100;
                scene.lights[2].direction = {0, 0, 1};
            }
        } else if (iteration == 2) {
            // The caster is outside the camera, but its shadow still crosses the visible receiver.
            scene.lights.front().direction = {2, 0, -1};
            scene.shadow_casters = scene.instances;
            scene.shadow_casters[1].world_transform =
                math::translation({-1.35f, 0, 0.6f}) * math::scale({0.15f, 0.15f, 1});
            scene.instances.resize(1);
        }
        rhi::TextureDesc color_desc{};
        color_desc.extent = {kSize, kSize, 1};
        color_desc.format = rhi::Format::kRgba8Unorm;
        color_desc.capabilities = rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kTransferSource;
        const auto color = graph.create_texture(color_desc);
        auto depth_desc = color_desc;
        depth_desc.format = rhi::Format::kD32Float;
        depth_desc.capabilities = rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kSampled;
        const auto depth = graph.create_texture(depth_desc);
        auto white_desc = color_desc;
        white_desc.extent = {1, 1, 1};
        white_desc.capabilities = rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kSampled;
        const auto white = graph.create_texture(white_desc);
        if (!color || !depth || !white)
            return 11;
        // Graph views are allocated at compile; material descriptors are prepared by a preceding pass.
        const auto tint_desc =
            upload_desc(sizeof(math::Vec4), rhi::BufferCapability::kUniform, rhi::ResourceUsage::kUniformRead);
        const auto tint = device.create_buffer(tint_desc);
        const auto lighting_constants = renderer::make_render_lighting_constants(scene.lights, {0.1f, 0.1f, 0.1f});
        const auto light_desc = upload_desc(sizeof(renderer::RenderLightingConstants), rhi::BufferCapability::kUniform,
                                            rhi::ResourceUsage::kUniformRead);
        const auto light = device.create_buffer(light_desc);
        const auto material_set = device.create_binding_set({material_layout, rhi::BindingSetLifetime::kPersistent});
        const auto light_set = device.create_binding_set({lighting_layout, rhi::BindingSetLifetime::kPersistent});
        if (!tint || !light || !material_set || !light_set || !lighting_constants)
            return 12;
        const math::Vec4 tint_value{1, 1, 1, 1};
        if (!check(device.write_buffer(tint.value, 0, std::as_bytes(std::span{&tint_value, 1}))) ||
            !check(device.write_buffer(light.value, 0, std::as_bytes(std::span{&lighting_constants.value, 1}))))
            return 13;
        const auto light_graph = graph.import_buffer(light.value, light_desc);
        const auto tint_graph = graph.import_buffer(tint.value, tint_desc);
        if (!light_graph || !tint_graph)
            return 14;
        rhi::BindingWrite light_write{};
        light_write.location = {2, 0};
        light_write.resource.type = rhi::BindingType::kUniformBuffer;
        light_write.resource.buffer_range = {light.value, 0, sizeof(renderer::RenderLightingConstants)};
        if (!check(device.update_binding_set(light_set.value, std::span{&light_write, 1})))
            return 15;
        resolver.lighting = {light_set.value,
                             2,
                             light.value,
                             light_desc,
                             {light_graph.value.index, light_graph.value.generation, light_graph.value.graph_id},
                             0,
                             sizeof(renderer::RenderLightingConstants)};
        resolver.material = {.pipeline = opaque.value,
                             .binding_set = material_set.value,
                             .material_texture = {white.value.index, white.value.generation, white.value.graph_id},
                             .material_parameter_buffer = tint.value,
                             .material_parameter_buffer_desc = tint_desc,
                             .material_parameter_resource = {tint_graph.value.index, tint_graph.value.generation,
                                                             tint_graph.value.graph_id},
                             .material_parameter_size = sizeof(math::Vec4),
                             .gbuffer_pipeline = gbuffer.value,
                             .depth_pipeline = depth_pipeline.value,
                             .shadow_pipeline = shadow_opaque.value,
                             .clustered_pipeline = clustered.value};
        const auto clear = graph.add_pass(
            "White material", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                return pass.use_color_attachment(white.value, rhi::LoadOp::kClear, rhi::StoreOp::kStore, {1, 1, 1, 1},
                                                 rhi::ResourceUsage::kSampledRead);
            },
            [&](render_graph::RenderPassCommandContext& command) {
                if (auto r = command.begin_rendering(); !r)
                    return r;
                return command.end_rendering();
            });
        if (!clear)
            return 16;
        const auto binding = graph.add_pass(
            "Material inputs", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                pass.set_side_effects();
                return pass.read(white.value, rhi::ResourceUsage::kSampledRead, rhi::PipelineStage::kFragmentShader);
            },
            [&](render_graph::RenderPassCommandContext& command) {
                const auto view = command.resolve_texture_view(white.value);
                if (!view)
                    return view.result;
                std::array<rhi::BindingWrite, 3> writes{};
                writes[0].location = {1, 0};
                writes[0].resource.type = rhi::BindingType::kSampledTexture;
                writes[0].resource.texture_view = view.value;
                writes[1].location = {1, 1};
                writes[1].resource.type = rhi::BindingType::kSampler;
                writes[1].resource.sampler = sampler.value;
                writes[2].location = {1, 2};
                writes[2].resource.type = rhi::BindingType::kUniformBuffer;
                writes[2].resource.buffer_range = {tint.value, 0, sizeof(math::Vec4)};
                return device.update_binding_set(material_set.value, writes);
            });
        if (!binding)
            return 17;
        const renderer::RenderFeatureContext context{&scene, &resolver, nullptr, &passes};
        const std::array<render_graph::RenderGraphFeatureTextureBinding, 2> textures{
            {{kColor, color.value}, {kDepth, depth.value}}};
        render_graph::RenderGraphFeatureRuntime runtime(graph, host.active_plan_snapshot(), registry,
                                                        {{}, textures, nullptr, &context});
        if (!check(runtime.build()))
            return 18;
        const auto readback = frame.allocate_readback(kSize * kSize * 4, 256);
        if (!readback)
            return 19;
        rhi::BufferDesc read_desc{};
        read_desc.size = readback.value.buffer_size;
        read_desc.capabilities = rhi::BufferCapability::kTransferDestination;
        read_desc.memory = rhi::MemoryClass::kReadback;
        const auto destination = graph.import_buffer(readback.value.buffer, read_desc);
        if (!destination)
            return 19;
        const auto copy = graph.add_pass(
            "Shadow pixels", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                if (auto r = pass.read(color.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !r)
                    return r;
                pass.set_side_effects();
                return pass.write(destination.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
            },
            [&](render_graph::RenderPassCommandContext& command) {
                return command.copy_texture_to_buffer(color.value, {rhi::Aspect::kColor, 0, 0, 1}, {},
                                                      {kSize, kSize, 1}, destination.value, readback.value.offset, 0,
                                                      0);
            });
        if (!copy || !check(graph.compile()))
            return 19;
        const auto executed = graph.execute(frame);
        if (!executed) {
            check(executed.result);
            return 20;
        }
        if (!check(frame.defer_destroy_binding_set(material_set.value)) ||
            !check(frame.defer_destroy_binding_set(light_set.value)) ||
            !check(frame.defer_destroy_buffer(tint.value)) || !check(frame.defer_destroy_buffer(light.value)) ||
            !check(frame.end_frame()) || !check(device.wait_idle()))
            return 21;
        std::array<std::byte, kSize * kSize * 4> pixels{};
        if (!check(frame.read_readback(readback.value, pixels)))
            return 22;
        dark_pixels[iteration] = std::to_integer<unsigned>(pixels[(64 * kSize + 54) * 4]);
        lit_pixels[iteration] = std::to_integer<unsigned>(pixels[(64 * kSize + 90) * 4]);
        blue_pixels[iteration] = std::to_integer<unsigned>(pixels[(64 * kSize + 90) * 4 + 2]);
        background_pixels[iteration] = std::to_integer<unsigned>(pixels[0]);
        if (!check(frame.drain()))
            return 23;
        const auto stale_post = resolver.last_post.prepare_inputs({}, {});
        if (stale_post || stale_post.result.code != foundation::ResultCode::kInvalidState)
            return 33;
        if (iteration != 0 && deferred_mode && !post_process) {
            const auto stale = resolver.last_shadow.prepare_inputs({});
            if (stale || stale.result.code != foundation::ResultCode::kInvalidState)
                return 27;
        }
        if (!deferred_mode) {
            const auto stale = resolver.last_culling.prepare_forward({}, {});
            if (stale || stale.result.code != foundation::ResultCode::kInvalidState)
                return 30;
        }
        if (shadows.for_frame(frame, {}))
            return 28;
        graph.reset();
    }
    std::cout << "Shadow samples: " << dark_pixels[0] << ',' << dark_pixels[1] << ',' << dark_pixels[2]
              << "; lit: " << lit_pixels[0] << ',' << lit_pixels[1] << ',' << lit_pixels[2] << '\n';
    if (post_process) {
        std::cout << "Sky samples: " << background_pixels[0] << ',' << background_pixels[1] << ','
                  << background_pixels[2] << '\n';
        if (std::abs(dark_pixels[0] - 205) > 3 || std::abs(dark_pixels[1] - 171) > 3 ||
            std::abs(dark_pixels[2] - 229) > 3 || std::abs(background_pixels[0] - 51) > 2 ||
            std::abs(background_pixels[1] - 28) > 2 || std::abs(background_pixels[2] - 114) > 2)
            return 31;
    } else if (local_lights) {
        std::cout << "Spot samples: " << blue_pixels[0] << ',' << blue_pixels[1] << ',' << blue_pixels[2] << '\n';
        if (dark_pixels[0] < 23 || dark_pixels[0] > 27 || dark_pixels[1] < 45 || dark_pixels[2] < 23 ||
            dark_pixels[2] > 27 || blue_pixels[0] < 23 || blue_pixels[0] > 27 || blue_pixels[1] < 75 ||
            blue_pixels[2] < 23 || blue_pixels[2] > 27 || lit_pixels[1] < 23 || lit_pixels[1] > 27)
            return 29;
    } else if (dark_pixels[0] < 170 || dark_pixels[1] > 45 || dark_pixels[1] < 20 || dark_pixels[2] > 45 ||
               dark_pixels[2] < 20 || std::abs(lit_pixels[0] - lit_pixels[1]) > 3 || lit_pixels[2] < 95)
        return 24;
    if (!check(device.destroy_sampler(sampler.value)) || !check(device.destroy_buffer(vertex.value)) ||
        !check(device.destroy_buffer(index.value)))
        return 25;
    for (auto pipeline : {opaque.value, shadow_opaque.value, gbuffer.value, depth_pipeline.value, clustered.value})
        if (!check(device.destroy_pipeline(pipeline)))
            return 26;
    std::cout << "Shadowed receiver, ambient, lit receiver and off-camera caster passed\n";
}
