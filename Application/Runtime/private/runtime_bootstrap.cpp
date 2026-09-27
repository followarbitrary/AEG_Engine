#include "runtime_bootstrap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>

#include "Components/component_render_extraction.hpp"
#include "application_platform.hpp"
#include "asset_package_reader.hpp"
#include "deferred_lighting_resources.hpp"
#include "engine_frame_orchestrator.hpp"
#include "frame_context.hpp"
#include "game_build_manifest.hpp"
#include "input_system.hpp"
#include "light_culling.hpp"
#include "module.hpp"
#include "module_api.h"
#include "module_bundle.hpp"
#include "pipeline_asset.hpp"
#include "pipeline_authoring.hpp"
#include "post_process.hpp"
#include "render_graph.hpp"
#include "render_pass.hpp"
#include "renderer_variants.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_scene_loader.hpp"
#include "scene_payload.hpp"
#include "shader_contract.hpp"
#include "shadow_rendering.hpp"
#include "texture_uploader.hpp"

namespace aeg::application::runtime {
namespace {
struct Manifest final {
    std::filesystem::path package{};
    std::string build_id{};
    std::string target{};
    std::string configuration{};
};

foundation::ResultValue<std::string> file_digest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::not_found("Runtime package is missing"), {}};
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return module::module_content_sha256(bytes);
}

foundation::ResultValue<asset::GameBuildManifest> read_binary_manifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {foundation::not_found("Binary game manifest was not found"), {}};
    const auto end = input.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > 64u * 1024u * 1024u)
        return {foundation::invalid_argument("Binary game manifest size is invalid"), {}};
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), end);
    if (!input)
        return {foundation::invalid_argument("Binary game manifest cannot be read"), {}};
    return asset::decode_game_build_manifest(bytes);
}

foundation::ResultValue<Manifest> read_manifest(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        return {foundation::not_found("Game manifest was not found"), {}};
    Manifest result{};
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        const auto trim = [](std::string& text) {
            const auto first = text.find_first_not_of(" \t\r\n");
            const auto last = text.find_last_not_of(" \t\r\n");
            text = first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
        };
        trim(key);
        trim(value);
        if (!value.empty() && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (key == "package")
            result.package = value;
        else if (key == "build_id")
            result.build_id = value;
        else if (key == "target")
            result.target = value;
        else if (key == "configuration")
            result.configuration = value;
    }
    if (result.package.empty())
        return {foundation::invalid_argument("Game manifest has no package"), {}};
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace

struct RuntimeBootstrap::Impl final {
    static void context_quit(void* opaque) noexcept {
        if (opaque) {
            auto* impl = static_cast<Impl*>(opaque);
            impl->quit_requested = true;
            impl->frame.quit_requested = true;
        }
    }
    static const RuntimeFrameContext* context_frame(void* opaque) noexcept {
        return opaque ? &static_cast<Impl*>(opaque)->frame : nullptr;
    }
    RuntimeBootRequest request{};
    RuntimeBuildIdentity identity{};
    std::unique_ptr<asset::AssetPackageReader> package{};
    std::unique_ptr<RuntimeAssetResolver> asset_resolver{};
    std::unique_ptr<RuntimeSceneLoadResult> startup_scene{};
    asset::AssetId startup_scene_id{};
    std::unique_ptr<module::ModuleRegistry> modules{};
    std::unique_ptr<pipeline::PipelineRegistry> pipelines{};
    std::unique_ptr<reflection::TypeRegistry> reflection{};
    std::unique_ptr<pipeline::RenderPassRegistry> passes{};
    pipeline::TypeCatalog catalog{};
    std::vector<pipeline::PreparedPipelineDocument> startup_pipelines{};
    std::unique_ptr<pipeline::PipelineScheduler> scheduler{};
    std::vector<std::unique_ptr<pipeline::PipelineScopeHost>> pipeline_hosts{};
    std::unique_ptr<engine::EngineFrameOrchestrator> orchestrator{};
    std::unique_ptr<platform::IWindowSystem> owned_window_system{};
    std::shared_ptr<platform::IWindow> owned_window{};
    std::unique_ptr<rhi::Device> owned_device{};
    rhi::SurfaceHandle owned_surface{};
    rhi::SwapchainHandle owned_swapchain{};
    rhi::SwapchainDesc owned_swapchain_desc{};
    std::unique_ptr<render_graph::FrameContext> owned_frame_context{};
    std::unique_ptr<render_graph::RenderGraph> owned_render_graph{};
    std::unique_ptr<renderer::Renderer> owned_renderer{};
    std::unique_ptr<resource::ResourceUploader> owned_resource_uploader{};
    std::unique_ptr<resource::TextureUploader> owned_texture_uploader{};
    std::unique_ptr<shader::ShaderManager> owned_shader_manager{};
    std::unique_ptr<shader::ShaderProgram> owned_material_shader_program{};
    std::unique_ptr<shader::ShaderPipelineLayout> owned_shader_layout{};
    std::unique_ptr<renderer::ShadowResources> owned_shadow_resources{};
    std::unique_ptr<renderer::LightCullingResources> owned_light_culling{};
    std::unique_ptr<renderer::DeferredLightingResources> owned_deferred_lighting{};
    std::unique_ptr<renderer::PostProcessResources> owned_post_process{};
    rhi::BindingLayoutHandle owned_object_layout{};
    rhi::BindingLayoutHandle owned_material_layout{};
    rhi::BindingLayoutHandle owned_lighting_layout{};
    rhi::PipelineHandle owned_material_pipeline{};
    rhi::PipelineHandle owned_shadow_pipeline{};
    rhi::PipelineHandle owned_clustered_pipeline{};
    rhi::PipelineHandle owned_depth_pipeline{};
    rhi::PipelineHandle owned_gbuffer_pipeline{};
    RuntimeAssetResolver::UploadContext owned_upload_context{};
    rhi::SwapchainInfo owned_swapchain_info{};
    RuntimeFrameHostResources owned_frame_host{};
    bool owns_host{false};
    platform::WindowExtent pending_owned_resize{};
    bool owned_resize_pending{false};
    bool owned_surface_suspended{false};
    uint32_t frames{};
    bool initialized{};
    GameRuntimeCallbacks callbacks{};
    RuntimeMode mode{RuntimeMode::kStopped};
    RuntimeFrameContext frame{};
    input::InputSnapshot input{};
    input::InputSystem input_system{};
    float accumulator{0.0f};
    bool quit_requested{false};
    bool started{false};
    bool shutdown_started{false};
    bool world_registered_with_orchestrator{false};
    bool world_frame_registered_with_orchestrator{false};
    double last_time{-1.0};
    GameRuntimeContext game_context{};

    [[nodiscard]] foundation::Result recreate_owned_swapchain(platform::WindowExtent extent);
    [[nodiscard]] foundation::Result initialize_owned_graphics_resources();
    void destroy_owned_graphics_resources() noexcept;
};

inline constexpr pipeline::RenderResourceId kRuntimeSceneColor{"Renderer.SceneColor"};
inline constexpr pipeline::RenderResourceId kRuntimeSceneDepth{"Renderer.SceneDepth"};

foundation::Result bind_owned_scene_depth(const engine::EngineFramePreparationContext& context,
                                          renderer::RenderExtent extent) {
    rhi::TextureDesc depth_desc{};
    depth_desc.dimension = rhi::TextureDimension::kTex2D;
    depth_desc.extent = {extent.width, extent.height, 1};
    depth_desc.format = rhi::Format::kD32Float;
    depth_desc.capabilities = rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kSampled;
    depth_desc.initial_usage = rhi::ResourceUsage::kUndefined;
    depth_desc.name = rhi::DebugName{"Runtime.SceneDepth"};
    if (depth_desc.extent.width == 0 || depth_desc.extent.height == 0)
        return foundation::invalid_argument("Runtime owned SceneDepth extent is invalid");
    const auto depth = context.render_graph.create_texture(depth_desc, render_graph::ResourceLifetime::kTransient);
    if (!depth)
        return depth.result;
    return context.bind_feature_texture(kRuntimeSceneDepth, depth.value);
}

foundation::Result RuntimeBootstrap::Impl::recreate_owned_swapchain(platform::WindowExtent extent) {
    if (!owns_host || owned_device == nullptr || !owned_swapchain.is_valid())
        return foundation::invalid_state("owned runtime swapchain is not available for recreation");
    if (!extent.is_valid()) {
        owned_surface_suspended = true;
        pending_owned_resize = extent;
        owned_resize_pending = true;
        return foundation::Result::success();
    }
    if (owned_frame_context != nullptr && owned_frame_context->is_frame_active())
        return foundation::invalid_state("owned runtime swapchain cannot be recreated during an active frame");

    owned_swapchain_desc.extent = {extent.width, extent.height};
    if (const auto result = owned_device->recreate_swapchain(owned_swapchain, owned_swapchain_desc); !result)
        return result;
    const auto info = owned_device->get_swapchain_info(owned_swapchain);
    if (!info)
        return info.result;
    if (info.value.extent.width == 0 || info.value.extent.height == 0)
        return foundation::invalid_state("owned runtime swapchain recreation returned an invalid extent");
    if (info.value.format != owned_swapchain_info.format) {
        // The opaque, forward-shadow, and post-process pipelines were built
        // against the previous output format. Preserve that format contract
        // and keep retrying at the recreation boundary so no subsequent tick
        // can render with those pipelines against the replacement swapchain.
        pending_owned_resize = extent;
        owned_resize_pending = true;
        owned_surface_suspended = true;
        return foundation::unsupported(
            "Owned runtime swapchain format changed; graphics resources require reinitialization");
    }
    owned_swapchain_info = info.value;
    pending_owned_resize = {};
    owned_resize_pending = false;
    owned_surface_suspended = false;
    return foundation::Result::success();
}

foundation::Result RuntimeBootstrap::Impl::initialize_owned_graphics_resources() {
    std::filesystem::path package_path = request.material_shader_package;
#if defined(AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH)
    if (package_path.empty())
        package_path = AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH;
#endif
    std::filesystem::path depth_mesh_package = request.depth_mesh_shader_package;
#if defined(AEG_DEPTH_MESH_SHADER_PACKAGE_PATH)
    if (depth_mesh_package.empty())
        depth_mesh_package = AEG_DEPTH_MESH_SHADER_PACKAGE_PATH;
#endif
    std::filesystem::path gbuffer_mesh_package = request.gbuffer_mesh_shader_package;
#if defined(AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH)
    if (gbuffer_mesh_package.empty())
        gbuffer_mesh_package = AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH;
#endif
    std::filesystem::path forward_shadow_package = request.forward_shadow_shader_package;
#if defined(AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH)
    if (forward_shadow_package.empty())
        forward_shadow_package = AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH;
#endif
    std::filesystem::path forward_clustered_package = request.forward_clustered_shader_package;
#if defined(AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH)
    if (forward_clustered_package.empty())
        forward_clustered_package = AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH;
#endif
    std::filesystem::path light_culling_package = request.light_culling_shader_package;
#if defined(AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH)
    if (light_culling_package.empty())
        light_culling_package = AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH;
#endif
    std::filesystem::path deferred_lighting_package = request.deferred_lighting_shader_package;
#if defined(AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH)
    if (deferred_lighting_package.empty())
        deferred_lighting_package = AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH;
#endif
    std::filesystem::path deferred_shadow_package = request.deferred_shadow_shader_package;
#if defined(AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH)
    if (deferred_shadow_package.empty())
        deferred_shadow_package = AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH;
#endif
    std::filesystem::path post_process_package = request.post_process_shader_package;
#if defined(AEG_POST_PROCESS_SHADER_PACKAGE_PATH)
    if (post_process_package.empty())
        post_process_package = AEG_POST_PROCESS_SHADER_PACKAGE_PATH;
#endif
    if (package_path.empty()) {
        if (request.material_shader != asset::AssetId{})
            return foundation::invalid_state("Owned runtime material shader requires an opaque shader package");
        // An empty authored scene may still use the windowed host as a clear/
        // presentation loop. Keep the host usable while leaving GPU material
        // resolution fail-closed until the game supplies a shader package.
        owned_resource_uploader = std::make_unique<resource::ResourceUploader>(*owned_device);
        owned_texture_uploader = std::make_unique<resource::TextureUploader>(*owned_device);
        owned_upload_context.device = owned_device.get();
        owned_upload_context.frame_context = owned_frame_context.get();
        owned_upload_context.graph = owned_render_graph.get();
        owned_upload_context.uploader = owned_resource_uploader.get();
        owned_upload_context.texture_uploader = owned_texture_uploader.get();
        owned_frame_host.upload_context = &owned_upload_context;
        owned_frame_host.require_resolved_render_resources = true;
        return foundation::Result::success();
    }
    const auto program = shader::load_shader_program(package_path);
    if (!program)
        return program.result;

    owned_material_shader_program = std::make_unique<shader::ShaderProgram>(std::move(program.value));
    owned_shader_manager = std::make_unique<shader::ShaderManager>(*owned_device);
    auto layout = owned_shader_manager->build_pipeline_layout(*owned_material_shader_program,
                                                              rhi::DebugName{"Runtime.Opaque.Layout"});
    if (!layout)
        return layout.result;
    owned_shader_layout = std::make_unique<shader::ShaderPipelineLayout>(std::move(layout.value));
    const auto object_layout = owned_shader_manager->binding_layout(*owned_shader_layout, 0);
    const auto material_layout = owned_shader_manager->binding_layout(*owned_shader_layout, 1);
    const auto lighting_layout = owned_shader_manager->binding_layout(*owned_shader_layout, 2);
    if (!object_layout || !material_layout || !lighting_layout)
        return foundation::invalid_state("Owned runtime opaque shader binding layouts are incomplete");
    owned_object_layout = object_layout.value;
    owned_material_layout = material_layout.value;
    owned_lighting_layout = lighting_layout.value;

    const std::array<rhi::VertexBindingDesc, 1> bindings{{
        {0, sizeof(float) * 9u, rhi::VertexInputRate::kVertex},
    }};
    const std::array<rhi::VertexAttributeDesc, 3> attributes{{
        {0, 0, rhi::Format::kRgba32Float, 0},
        {1, 0, rhi::Format::kRg32Float, sizeof(float) * 4u},
        {2, 0, rhi::Format::kRgb32Float, sizeof(float) * 6u},
    }};
    const std::array<rhi::Format, 1> color_formats{{owned_swapchain_info.format}};
    rhi::GraphicsPipelineState graphics{};
    graphics.vertex_bindings = bindings;
    graphics.vertex_attributes = attributes;
    graphics.topology = rhi::PrimitiveTopology::kTriangleList;
    graphics.cull_mode = rhi::CullMode::kNone;
    graphics.depth_test_enable = true;
    graphics.depth_write_enable = true;
    graphics.color_formats = color_formats;
    graphics.depth_stencil_format = rhi::Format::kD32Float;
    const auto pipeline = owned_shader_manager->create_pipeline(
        *owned_material_shader_program, *owned_shader_layout,
        shader::ShaderPipelineDesc{rhi::PipelineType::kGraphics, graphics, rhi::DebugName{"Runtime.Opaque.Pipeline"}});
    if (!pipeline)
        return pipeline.result;
    owned_material_pipeline = pipeline.value;

    if (!depth_mesh_package.empty()) {
        const auto depth_program = shader::load_shader_program(depth_mesh_package);
        if (!depth_program)
            return depth_program.result;
        const auto depth_layout =
            owned_shader_manager->build_pipeline_layout(depth_program.value, rhi::DebugName{"Runtime.Depth.Layout"});
        if (!depth_layout)
            return depth_layout.result;
        auto depth_graphics = graphics;
        depth_graphics.vertex_attributes = std::span<const rhi::VertexAttributeDesc>{attributes.data(), 1};
        depth_graphics.color_formats = {};
        depth_graphics.depth_test_enable = true;
        depth_graphics.depth_write_enable = true;
        const auto depth_pipeline = owned_shader_manager->create_pipeline(
            depth_program.value, depth_layout.value,
            shader::ShaderPipelineDesc{rhi::PipelineType::kGraphics, depth_graphics,
                                       rhi::DebugName{"Runtime.Depth.Pipeline"}});
        if (!depth_pipeline)
            return depth_pipeline.result;
        owned_depth_pipeline = depth_pipeline.value;
    }

    if (!gbuffer_mesh_package.empty()) {
        const auto gbuffer_program = shader::load_shader_program(gbuffer_mesh_package);
        if (!gbuffer_program)
            return gbuffer_program.result;
        const auto gbuffer_layout = owned_shader_manager->build_pipeline_layout(
            gbuffer_program.value, rhi::DebugName{"Runtime.GBuffer.Layout"});
        if (!gbuffer_layout)
            return gbuffer_layout.result;
        const std::array gbuffer_formats{rhi::Format::kRgba16Float, rhi::Format::kRgba16Float,
                                         rhi::Format::kRgba8Unorm};
        auto gbuffer_graphics = graphics;
        gbuffer_graphics.color_formats = gbuffer_formats;
        const auto gbuffer_pipeline = owned_shader_manager->create_pipeline(
            gbuffer_program.value, gbuffer_layout.value,
            shader::ShaderPipelineDesc{rhi::PipelineType::kGraphics, gbuffer_graphics,
                                       rhi::DebugName{"Runtime.GBuffer.Pipeline"}});
        if (!gbuffer_pipeline)
            return gbuffer_pipeline.result;
        owned_gbuffer_pipeline = gbuffer_pipeline.value;
    }

    // Forward/deferred/postprocess resources are host-owned, just like the
    // fixed opaque material pipeline above.  Keep their pipeline layouts
    // inside the owning resource objects or ShaderManager cache and publish
    // only the frame-safe resolver callbacks below.
    if (!forward_shadow_package.empty()) {
        const auto shadow_program = shader::load_shader_program(forward_shadow_package);
        if (!shadow_program)
            return shadow_program.result;
        const auto shadow_layout = owned_shader_manager->build_pipeline_layout(
            shadow_program.value, rhi::DebugName{"Runtime.ForwardShadow.Layout"});
        if (!shadow_layout)
            return shadow_layout.result;
        const auto shadow_pipeline = owned_shader_manager->create_pipeline(
            shadow_program.value, shadow_layout.value,
            shader::ShaderPipelineDesc{rhi::PipelineType::kGraphics, graphics,
                                       rhi::DebugName{"Runtime.ForwardShadow.Pipeline"}});
        if (!shadow_pipeline)
            return shadow_pipeline.result;
        const auto shadow_inputs = owned_shader_manager->binding_layout(shadow_layout.value, 3);
        if (!shadow_inputs)
            return shadow_inputs.result;
        owned_shadow_pipeline = shadow_pipeline.value;
        owned_shadow_resources = std::make_unique<renderer::ShadowResources>();
        if (const auto result = owned_shadow_resources->initialize(*owned_device, shadow_inputs.value); !result)
            return result;
    }

    if (!forward_clustered_package.empty()) {
        const auto clustered_program = shader::load_shader_program(forward_clustered_package);
        if (!clustered_program)
            return clustered_program.result;
        const auto clustered_layout = owned_shader_manager->build_pipeline_layout(
            clustered_program.value, rhi::DebugName{"Runtime.ForwardClustered.Layout"});
        if (!clustered_layout)
            return clustered_layout.result;
        const std::array hdr_formats{renderer::kSceneLightingFormat};
        auto clustered_graphics = graphics;
        clustered_graphics.color_formats = hdr_formats;
        const auto clustered_pipeline = owned_shader_manager->create_pipeline(
            clustered_program.value, clustered_layout.value,
            shader::ShaderPipelineDesc{rhi::PipelineType::kGraphics, clustered_graphics,
                                       rhi::DebugName{"Runtime.ForwardClustered.Pipeline"}});
        if (!clustered_pipeline)
            return clustered_pipeline.result;
        const auto clustered_inputs = owned_shader_manager->binding_layout(clustered_layout.value, 3);
        if (!clustered_inputs)
            return clustered_inputs.result;
        owned_clustered_pipeline = clustered_pipeline.value;
        if (!light_culling_package.empty()) {
            owned_light_culling = std::make_unique<renderer::LightCullingResources>();
            if (const auto result =
                    owned_light_culling->initialize(*owned_device, light_culling_package, clustered_inputs.value);
                !result)
                return result;
        }
    }

    if (!deferred_lighting_package.empty()) {
        owned_deferred_lighting = std::make_unique<renderer::DeferredLightingResources>();
        if (const auto result = owned_deferred_lighting->initialize(
                *owned_device, deferred_lighting_package, renderer::kSceneLightingFormat, deferred_shadow_package);
            !result)
            return result;
    }
    if (!post_process_package.empty()) {
        owned_post_process = std::make_unique<renderer::PostProcessResources>();
        if (const auto result =
                owned_post_process->initialize(*owned_device, post_process_package, owned_swapchain_info.format);
            !result)
            return result;
    }

    owned_resource_uploader = std::make_unique<resource::ResourceUploader>(*owned_device);
    owned_texture_uploader = std::make_unique<resource::TextureUploader>(*owned_device);
    owned_upload_context = {};
    owned_upload_context.device = owned_device.get();
    owned_upload_context.frame_context = owned_frame_context.get();
    owned_upload_context.graph = owned_render_graph.get();
    owned_upload_context.uploader = owned_resource_uploader.get();
    owned_upload_context.texture_uploader = owned_texture_uploader.get();
    owned_upload_context.material_binding_layout = owned_material_layout;
    owned_upload_context.material_pipeline = owned_material_pipeline;
    owned_upload_context.material_shader = request.material_shader;
    owned_upload_context.material_shader_program = owned_material_shader_program.get();
    owned_upload_context.material_sampler = rhi::SamplerDesc{
        .min_filter = rhi::Filter::kNearest,
        .mag_filter = rhi::Filter::kNearest,
        .mip_filter = rhi::Filter::kNearest,
        .address_u = rhi::AddressMode::kClampToEdge,
        .address_v = rhi::AddressMode::kClampToEdge,
        .address_w = rhi::AddressMode::kClampToEdge,
        .name = rhi::DebugName{"Runtime.Material.Sampler"},
    };
    owned_upload_context.material_binding_group = 1;
    owned_upload_context.object_binding_layout = owned_object_layout;
    owned_upload_context.lighting_binding_layout = owned_lighting_layout;
    if (owned_deferred_lighting) {
        owned_upload_context.deferred_lighting_resolver = [this](const renderer::RenderView&) {
            if (!owned_deferred_lighting || !owned_frame_context)
                return foundation::ResultValue<renderer::RenderDeferredLightingResource>{
                    foundation::not_ready("Owned deferred-lighting resources are unavailable"), {}};
            return owned_deferred_lighting->for_frame(*owned_frame_context);
        };
    }
    if (owned_shadow_resources) {
        owned_upload_context.shadow_resolver = [this](const renderer::RenderShadowView& shadow) {
            if (!owned_shadow_resources || !owned_frame_context)
                return foundation::ResultValue<renderer::RenderShadowResource>{
                    foundation::not_ready("Owned shadow resources are unavailable"), {}};
            return owned_shadow_resources->for_frame(*owned_frame_context, shadow.constants);
        };
    }
    if (owned_light_culling) {
        owned_upload_context.light_culling_resolver = [this](const renderer::RenderSceneSnapshot& scene,
                                                             const renderer::RenderShadowView& shadow) {
            if (!owned_light_culling || !owned_frame_context)
                return foundation::ResultValue<renderer::RenderLightCullingResource>{
                    foundation::not_ready("Owned light-culling resources are unavailable"), {}};
            return owned_light_culling->for_frame(*owned_frame_context, scene, shadow);
        };
    }
    if (owned_post_process) {
        owned_upload_context.post_process_resolver = [this](const renderer::RenderView& view) {
            if (!owned_post_process || !owned_frame_context)
                return foundation::ResultValue<renderer::RenderPostProcessResource>{
                    foundation::not_ready("Owned post-process resources are unavailable"), {}};
            return owned_post_process->for_frame(*owned_frame_context, view, view.post_process);
        };
    }
    owned_upload_context.shadow_pipeline = owned_shadow_pipeline;
    owned_upload_context.clustered_pipeline = owned_clustered_pipeline;
    owned_upload_context.depth_pipeline = owned_depth_pipeline;
    owned_upload_context.gbuffer_pipeline = owned_gbuffer_pipeline;
    owned_frame_host.upload_context = &owned_upload_context;
    owned_frame_host.require_resolved_render_resources = true;
    return foundation::Result::success();
}

void RuntimeBootstrap::Impl::destroy_owned_graphics_resources() noexcept {
    // Resolver callbacks are frame-borrowed. The caller invokes this only
    // after resolver retirement and device idle, so release callback state
    // before tearing down the resource owners they capture.
    // RuntimeAssetResolver stores a copy of UploadContext. Clear that copy
    // before destroying the frame/device objects; otherwise its destructor
    // would retain dangling host pointers and attempt a second retirement.
    if (asset_resolver != nullptr)
        asset_resolver->set_upload_context({});
    owned_upload_context = {};
    owned_frame_host.upload_context = nullptr;
    owned_post_process.reset();
    owned_deferred_lighting.reset();
    owned_light_culling.reset();
    owned_shadow_resources.reset();
    if (owned_device != nullptr && owned_material_pipeline.is_valid()) {
        (void)owned_device->destroy_pipeline(owned_material_pipeline);
    }
    owned_material_pipeline = {};
    if (owned_device != nullptr && owned_shadow_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_shadow_pipeline);
    owned_shadow_pipeline = {};
    if (owned_device != nullptr && owned_clustered_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_clustered_pipeline);
    owned_clustered_pipeline = {};
    if (owned_device != nullptr && owned_depth_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_depth_pipeline);
    owned_depth_pipeline = {};
    if (owned_device != nullptr && owned_gbuffer_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_gbuffer_pipeline);
    owned_gbuffer_pipeline = {};
    owned_object_layout = {};
    owned_material_layout = {};
    owned_lighting_layout = {};
    owned_shader_layout.reset();
    owned_material_shader_program.reset();
    owned_shader_manager.reset();
    owned_texture_uploader.reset();
    owned_resource_uploader.reset();
}

double default_clock(void*) noexcept {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

void RuntimeFrameHostResources::apply_frame_desc(engine::EngineFrameDesc& frame_desc,
                                                 const renderer::RenderResourceResolver* resolver) const noexcept {
    frame_desc.render_resource_resolver = resolver;
    frame_desc.require_resolved_render_resources = require_resolved_render_resources;
}

RuntimeBootstrap::RuntimeBootstrap() : impl_(std::make_unique<Impl>()) {}
RuntimeBootstrap::~RuntimeBootstrap() {
    (void)shutdown();
}

foundation::Result RuntimeBootstrap::initialize(const RuntimeBootRequest& request, module::ModuleDefinition definition,
                                                const RuntimeBuildIdentity& identity) {
    return initialize(request, std::move(definition), identity, {});
}

foundation::Result RuntimeBootstrap::initialize(const RuntimeBootRequest& request, module::ModuleDefinition definition,
                                                const RuntimeBuildIdentity& identity,
                                                const GameRuntimeCallbacks& callbacks) {
    if (!impl_ || impl_->initialized)
        return foundation::invalid_state("Runtime bootstrap is already initialized");
    if (impl_->package != nullptr || impl_->orchestrator != nullptr || impl_->owns_host ||
        !impl_->pipeline_hosts.empty()) {
        // A failed initialization must not leave borrowed registry references
        // or owned GPU/window resources behind when the caller retries.  A
        // previous failed shutdown is retryable; never raw-reset these owners.
        impl_->shutdown_started = false;
        if (const auto cleanup = shutdown(); !cleanup)
            return cleanup;
    }
    if (identity.build_id.empty() || identity.target.empty() || identity.configuration.empty())
        return foundation::invalid_argument("Runtime build identity is incomplete");
    if (!definition.status || !definition.descriptor.id.is_valid())
        return foundation::invalid_argument("Static game module definition is invalid");
    if (request.owned_host && (request.headless || request.offscreen))
        return foundation::invalid_argument("owned runtime host requires a windowed run");
    if (request.owned_host && (request.frame_host != nullptr || request.window_system != nullptr))
        return foundation::invalid_argument("owned runtime host cannot be combined with borrowed host resources");
    if (!request.readback_path.empty() && !request.owned_host)
        return foundation::unsupported("Runtime framebuffer readback requires an owned windowed host");
    if (!std::isfinite(request.fixed_delta_seconds) || request.fixed_delta_seconds <= 0.0f ||
        !std::isfinite(request.max_frame_delta_seconds) || request.max_frame_delta_seconds <= 0.0f ||
        request.max_fixed_steps_per_frame == 0)
        return foundation::invalid_argument("Runtime timing configuration is invalid");

    // A bootstrap instance may be initialized again after shutdown. Start each
    // session from a clean frame clock and lifecycle state.
    impl_->frames = 0;
    impl_->frame = {};
    impl_->input = {};
    impl_->accumulator = 0.0f;
    impl_->quit_requested = false;
    impl_->started = false;
    impl_->shutdown_started = false;
    impl_->world_registered_with_orchestrator = false;
    impl_->world_frame_registered_with_orchestrator = false;
    impl_->input_system.reset();
    impl_->mode = RuntimeMode::kStopped;
    impl_->last_time = -1.0;
    impl_->startup_scene_id = {};
    impl_->startup_scene.reset();
    impl_->destroy_owned_graphics_resources();
    impl_->owned_window_system.reset();
    impl_->owned_window.reset();
    impl_->owned_device.reset();
    impl_->owned_surface = {};
    impl_->owned_swapchain = {};
    impl_->owned_swapchain_desc = {};
    impl_->owned_frame_context.reset();
    impl_->owned_render_graph.reset();
    impl_->owned_renderer.reset();
    impl_->owned_resource_uploader.reset();
    impl_->owned_texture_uploader.reset();
    impl_->owned_shader_layout.reset();
    impl_->owned_material_shader_program.reset();
    impl_->owned_shader_manager.reset();
    impl_->owned_upload_context = {};
    impl_->owned_material_pipeline = {};
    impl_->owned_shadow_pipeline = {};
    impl_->owned_clustered_pipeline = {};
    impl_->owned_depth_pipeline = {};
    impl_->owned_gbuffer_pipeline = {};
    impl_->owned_object_layout = {};
    impl_->owned_material_layout = {};
    impl_->owned_lighting_layout = {};
    impl_->owned_swapchain_info = {};
    impl_->pending_owned_resize = {};
    impl_->owned_resize_pending = false;
    impl_->owned_surface_suspended = false;
    impl_->owned_frame_host = {};
    impl_->owns_host = false;
    impl_->asset_resolver.reset();

    RuntimeBootRequest effective = request;
    impl_->startup_scene_id = effective.startup_scene;
    // Keep a host-owned copy for package validation and preparation.  The
    // definition itself is moved into ModuleRegistry below, so runtime
    // startup never needs to retain a pointer into the caller's object.
    std::filesystem::path manifest = effective.manifest_path;
    if (manifest.empty())
        manifest = std::filesystem::current_path() / "game.manifest";
    foundation::ResultValue<Manifest> manifest_result{};
    if (manifest.extension() == ".bin") {
        manifest_result.result = foundation::Result::success();
    } else {
        manifest_result = read_manifest(manifest);
        if (!manifest_result)
            return manifest_result.result;
    }
    const auto binary_path = manifest.extension() == ".bin" ? manifest : manifest.parent_path() / "game.manifest.bin";
    if (std::filesystem::exists(binary_path)) {
        const auto binary = read_binary_manifest(binary_path);
        if (!binary)
            return binary.result;
        if (binary.value.build_id != identity.build_id || binary.value.target != identity.target ||
            binary.value.configuration != identity.configuration ||
            pipeline::ModuleId(binary.value.module_id).value() != definition.descriptor.id.value() ||
            binary.value.module_contract_digest.empty())
            return foundation::invalid_argument("Binary game manifest identity does not match executable");
        module::ModuleMetadata module_metadata{};
        module_metadata.module = definition.descriptor;
        module_metadata.descriptor = definition.descriptor;
        module_metadata.catalog = definition.catalog;
        module_metadata.abi_major = AEG_MODULE_ABI_MAJOR;
        module_metadata.abi_minor = AEG_MODULE_ABI_MINOR;
        const auto contract = module::module_contract_sha256(module_metadata);
        if (!contract || contract.value != binary.value.module_contract_digest) {
            return foundation::invalid_argument(
                "Binary game manifest module contract does not match executable (expected " +
                binary.value.module_contract_digest + ", executable " +
                (contract ? contract.value : std::string("<unavailable>")) + ")");
        }
        if (manifest_result.value.build_id != binary.value.build_id ||
            (!manifest_result.value.target.empty() && manifest_result.value.target != binary.value.target) ||
            (!manifest_result.value.configuration.empty() &&
             manifest_result.value.configuration != binary.value.configuration))
            return foundation::invalid_argument("Text and binary game manifests disagree");
        manifest_result.value.package = binary.value.package_name;
        impl_->startup_scene_id = binary.value.startup_scene;
    }
    if (!manifest_result.value.build_id.empty() && manifest_result.value.build_id != identity.build_id)
        return foundation::invalid_argument("Game manifest build identity does not match executable");
    if (!manifest_result.value.target.empty() && manifest_result.value.target != identity.target)
        return foundation::invalid_argument("Game manifest target does not match executable");
    if (!manifest_result.value.configuration.empty() && manifest_result.value.configuration != identity.configuration)
        return foundation::invalid_argument("Game manifest configuration does not match executable");
    if (effective.package_path.empty())
        effective.package_path = manifest.parent_path() / manifest_result.value.package;

    const auto package = asset::AssetPackageReader::open(effective.package_path);
    if (!package)
        return package.result;
    if (std::filesystem::exists(binary_path)) {
        const auto binary = read_binary_manifest(binary_path);
        if (!binary)
            return binary.result;
        const auto package_digest = file_digest(effective.package_path);
        if (!package_digest)
            return package_digest.result;
        if (package_digest.value != binary.value.package_digest)
            return foundation::invalid_argument("Asset package digest does not match game manifest");
        const auto scene = package.value.read_cooked(binary.value.startup_scene);
        if (!scene || scene.value.kind != asset::AssetKind::kScene)
            return foundation::not_found("Startup scene is missing from asset package");
        const auto decoded_scene = asset::decode_scene_payload(scene.value.payload);
        if (!decoded_scene)
            return decoded_scene.result;
        for (const auto& reference : decoded_scene.value.references) {
            const auto referenced = package.value.read(reference.asset);
            if (!referenced)
                return referenced.result;
        }
        for (const auto id : binary.value.startup_pipelines) {
            const auto pipeline = package.value.read_cooked(id);
            if (!pipeline || pipeline.value.kind != asset::AssetKind::kPipeline)
                return foundation::not_found("Startup pipeline is missing from asset package");
        }
    }
    impl_->pipelines = std::make_unique<pipeline::PipelineRegistry>();
    impl_->reflection = std::make_unique<reflection::TypeRegistry>();
    impl_->passes = std::make_unique<pipeline::RenderPassRegistry>();
    impl_->modules =
        std::make_unique<module::ModuleRegistry>(*impl_->pipelines, impl_->reflection.get(), impl_->passes.get());
    if (const foundation::Result builtins = renderer::register_builtin_rendering(*impl_->pipelines, *impl_->passes);
        !builtins) {
        impl_->modules.reset();
        impl_->passes.reset();
        impl_->reflection.reset();
        impl_->pipelines.reset();
        return builtins;
    }
    if (const foundation::Result builtins_catalog = renderer::register_renderer_authoring_types(impl_->catalog);
        !builtins_catalog) {
        impl_->modules.reset();
        impl_->passes.reset();
        impl_->reflection.reset();
        impl_->pipelines.reset();
        return builtins_catalog;
    }
    for (const pipeline::TypeCatalogEntry& entry : definition.catalog.entries()) {
        if (const foundation::Result added = impl_->catalog.add(entry); !added) {
            impl_->modules.reset();
            impl_->passes.reset();
            impl_->reflection.reset();
            impl_->pipelines.reset();
            return added;
        }
    }
    if (const auto result = module::register_static_module(*impl_->modules, std::move(definition)); !result) {
        impl_->modules.reset();
        impl_->passes.reset();
        impl_->reflection.reset();
        impl_->pipelines.reset();
        return result;
    }
    if (std::filesystem::exists(binary_path)) {
        const auto binary = read_binary_manifest(binary_path);
        if (!binary)
            return binary.result;
        impl_->startup_pipelines.reserve(binary.value.startup_pipelines.size());
        for (const auto id : binary.value.startup_pipelines) {
            const auto cooked = package.value.read_cooked(id);
            if (!cooked || cooked.value.kind != asset::AssetKind::kPipeline)
                return foundation::not_found("Startup pipeline is missing from asset package");
            const auto decoded = asset::decode_pipeline_asset(cooked.value.payload);
            if (!decoded)
                return decoded.result;
            const auto prepared =
                pipeline::prepare_pipeline_document(decoded.value, impl_->catalog, *impl_->pipelines, *impl_->passes);
            if (!prepared)
                return prepared.result;
            impl_->startup_pipelines.push_back(std::move(prepared.value));
        }
    }
    // A prepared document is still data until each authored Pipeline instance
    // is bound to a Scope host, compiled against the registered module types,
    // and activated. Keep this work in the runtime bootstrap so static games
    // and dynamic/editor hosts consume the same prepared configuration.
    impl_->scheduler = std::make_unique<pipeline::PipelineScheduler>(pipeline::PipelineSchedulerDesc{1, 1, 1, 1});
    for (const auto& document : impl_->startup_pipelines) {
        for (const auto& configuration : document.configurations) {
            if (!configuration) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return foundation::invalid_argument("prepared startup Pipeline configuration is null");
            }
            // The decoded startup Scene owns its World-scope host. Do not
            // create a second host here; the orchestrator will register the
            // single WorldScope returned by RuntimeSceneLoader.
            if (impl_->startup_scene_id != asset::AssetId{} &&
                configuration->scope == pipeline::PipelineScope::kWorld) {
                continue;
            }
            auto host = std::make_unique<pipeline::PipelineScopeHost>(configuration->scope, configuration->instance);
            if (const auto result = host->bind_pipeline(configuration->pipeline); !result) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return result;
            }
            if (const auto result = host->set_configuration(configuration); !result) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return result;
            }
            if (const auto result = host->rebuild(*impl_->pipelines); !result) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return result;
            }
            if (const auto result = host->activate({}); !result) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return result;
            }
            impl_->pipeline_hosts.push_back(std::move(host));
        }
    }
    impl_->request = std::move(effective);
    impl_->identity = identity;
    impl_->callbacks = callbacks;
    impl_->game_context = {impl_.get(), &Impl::context_quit, &Impl::context_frame};
    impl_->mode = RuntimeMode::kPlaying;
    impl_->frame.fixed_delta_seconds = impl_->request.fixed_delta_seconds;
    const auto rollback_initialize = [this](foundation::Result result) {
        (void)shutdown();
        return result;
    };
    impl_->package = std::make_unique<asset::AssetPackageReader>(std::move(package.value));
    impl_->asset_resolver = std::make_unique<RuntimeAssetResolver>(*impl_->package);
    if (impl_->request.frame_host != nullptr && impl_->request.frame_host->upload_context != nullptr)
        impl_->asset_resolver->set_upload_context(*impl_->request.frame_host->upload_context);
    if (impl_->startup_scene_id != asset::AssetId{}) {
        // Older packages may carry an explicitly encoded, empty ScenePayload
        // as a placeholder. Preserve that package-only boot contract while
        // still rejecting malformed or unmarked payloads in the loader.
        bool legacy_empty_scene = false;
        if (const auto cooked_scene = impl_->package->read_cooked(impl_->startup_scene_id); cooked_scene) {
            if (const auto scene_payload = asset::decode_scene_payload(cooked_scene.value.payload); scene_payload)
                legacy_empty_scene = scene_payload.value.serialized_scene.empty();
        }
        world::WorldScopeDesc startup_world_scope{};
        for (const auto& document : impl_->startup_pipelines) {
            for (const auto& configuration : document.configurations) {
                if (configuration && configuration->scope == pipeline::PipelineScope::kWorld) {
                    startup_world_scope.pipeline = configuration->pipeline;
                    startup_world_scope.pipeline_instance = configuration->instance;
                    break;
                }
            }
            if (startup_world_scope.pipeline.is_valid())
                break;
        }
        auto loaded_scene = legacy_empty_scene
                                ? foundation::ResultValue<RuntimeSceneLoadResult>{foundation::Result::success(), {}}
                                : RuntimeSceneLoader::load_startup_scene(
                                      *impl_->package, impl_->startup_scene_id, *impl_->pipelines, *impl_->reflection,
                                      startup_world_scope, impl_->asset_resolver.get());
        if (!loaded_scene) {
            for (auto iterator = impl_->pipeline_hosts.rbegin(); iterator != impl_->pipeline_hosts.rend(); ++iterator) {
                if (*iterator && (*iterator)->is_active())
                    static_cast<void>((*iterator)->deactivate({}));
            }
            impl_->pipeline_hosts.clear();
            impl_->scheduler.reset();
            impl_->modules.reset();
            impl_->passes.reset();
            impl_->reflection.reset();
            impl_->pipelines.reset();
            impl_->startup_pipelines.clear();
            impl_->catalog = {};
            impl_->asset_resolver.reset();
            impl_->package.reset();
            impl_->startup_scene_id = {};
            return loaded_scene.result;
        }
        impl_->startup_scene = std::make_unique<RuntimeSceneLoadResult>(std::move(loaded_scene.value));
    }
    if (impl_->request.owned_host) {
#if defined(_WIN32)
        auto window_system = application::create_window_system();
        if (!window_system)
            return rollback_initialize(window_system.result);
        impl_->owned_window_system = std::move(window_system.value);
        impl_->owns_host = true;
        platform::WindowDesc window_desc{};
        window_desc.title = impl_->request.window_title;
        window_desc.extent = {impl_->request.width, impl_->request.height};
        window_desc.visible = true;
        auto window = impl_->owned_window_system->create_window(window_desc);
        if (!window)
            return rollback_initialize(window.result);
        impl_->owned_window = std::move(window.value);
        const auto binding = impl_->owned_window->surface_binding();
        if (!binding.is_valid())
            return rollback_initialize(foundation::invalid_state("owned runtime window has no surface binding"));
        auto device = rhi::create_device({rhi::BackendType::kVulkan, "AEG Runtime", UINT32_MAX, true, true});
        if (!device)
            return rollback_initialize(device.result);
        impl_->owned_device = std::move(device.value);
        rhi::SurfaceDesc surface_desc{};
        surface_desc.native_display_handle = binding.display_handle;
        surface_desc.native_window_handle = binding.window_handle;
        surface_desc.name = rhi::DebugName{"Runtime.Surface"};
        auto surface = impl_->owned_device->create_surface(surface_desc);
        if (!surface)
            return rollback_initialize(surface.result);
        impl_->owned_surface = surface.value;
        rhi::SwapchainDesc swapchain_desc{};
        swapchain_desc.surface = impl_->owned_surface;
        swapchain_desc.extent = {impl_->request.width, impl_->request.height};
        swapchain_desc.present_mode = impl_->request.vsync ? rhi::PresentMode::kVsync : rhi::PresentMode::kImmediate;
        swapchain_desc.min_image_count = 2;
        swapchain_desc.frames_in_flight = 2;
        swapchain_desc.image_capabilities =
            rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kTransferSource;
        swapchain_desc.name = rhi::DebugName{"Runtime.Swapchain"};
        impl_->owned_swapchain_desc = swapchain_desc;
        auto swapchain = impl_->owned_device->create_swapchain(swapchain_desc);
        if (!swapchain)
            return rollback_initialize(swapchain.result);
        impl_->owned_swapchain = swapchain.value;
        auto swapchain_info = impl_->owned_device->get_swapchain_info(impl_->owned_swapchain);
        if (!swapchain_info || swapchain_info.value.extent.width == 0 || swapchain_info.value.extent.height == 0)
            return rollback_initialize(swapchain_info
                                           ? foundation::invalid_state("owned runtime swapchain extent is invalid")
                                           : swapchain_info.result);
        impl_->owned_swapchain_info = swapchain_info.value;
        impl_->owned_frame_context = std::make_unique<render_graph::FrameContext>(*impl_->owned_device);
        impl_->owned_render_graph = std::make_unique<render_graph::RenderGraph>(*impl_->owned_device);
        impl_->owned_renderer = std::make_unique<renderer::Renderer>();
        if (const auto result = impl_->owned_renderer->register_builtin_extractors(); !result)
            return rollback_initialize(result);
        // The built-in extractors consume renderer-native components. Runtime
        // scenes carry the Game.* component bundle, so install the adapters
        // that turn authored MeshRenderer and light components into the
        // renderer snapshot consumed by the frame graph.
        if (const auto result = engine::components::register_mesh_renderer_extractor(*impl_->owned_renderer); !result)
            return rollback_initialize(result.result);
        if (const auto result = engine::components::register_directional_light_extractor(*impl_->owned_renderer);
            !result)
            return rollback_initialize(result.result);
        if (const auto result = engine::components::register_local_light_extractor(*impl_->owned_renderer); !result)
            return rollback_initialize(result.result);
        impl_->owned_frame_host.frame_context = impl_->owned_frame_context.get();
        impl_->owned_frame_host.render_graph = impl_->owned_render_graph.get();
        impl_->owned_frame_host.renderer = impl_->owned_renderer.get();
        if (const auto result = impl_->initialize_owned_graphics_resources(); !result)
            return rollback_initialize(result);
        impl_->asset_resolver->set_upload_context(impl_->owned_upload_context);
        impl_->request.window_system = impl_->owned_window_system.get();
        impl_->request.frame_host = &impl_->owned_frame_host;
#else
        return rollback_initialize(foundation::unsupported("owned runtime host is only available on Windows"));
#endif
    }
    if (impl_->request.frame_host != nullptr && impl_->request.frame_host->frame_context != nullptr &&
        impl_->request.frame_host->render_graph != nullptr) {
        impl_->orchestrator = std::make_unique<engine::EngineFrameOrchestrator>(
            *impl_->pipelines, *impl_->scheduler, *impl_->request.frame_host->frame_context,
            *impl_->request.frame_host->render_graph);
        for (const auto& host : impl_->pipeline_hosts) {
            if (host == nullptr)
                return rollback_initialize(foundation::invalid_state("runtime Pipeline host is not initialized"));
            if (const auto result = impl_->orchestrator->register_pipeline(*host); !result)
                return rollback_initialize(result);
        }
        if (impl_->startup_scene != nullptr && impl_->startup_scene->scope != nullptr) {
            if (const auto result = impl_->orchestrator->register_world(*impl_->startup_scene->scope); !result)
                return rollback_initialize(result);
            impl_->world_registered_with_orchestrator = true;
            pipeline::PipelineScopeHost* frame_host = nullptr;
            for (const auto& host : impl_->pipeline_hosts) {
                if (host != nullptr && host->scope() == pipeline::PipelineScope::kFrame) {
                    frame_host = host.get();
                    break;
                }
            }
            if (frame_host != nullptr) {
                if (const auto result =
                        impl_->orchestrator->register_world_frame(*impl_->startup_scene->scope, *frame_host, 0);
                    !result)
                    return rollback_initialize(result);
                impl_->world_frame_registered_with_orchestrator = true;
            }
        }
        if (const auto result = impl_->orchestrator->initialize(); !result)
            return rollback_initialize(result);
    }
    impl_->initialized = true;
    if (impl_->callbacks.start) {
        const auto status = impl_->callbacks.start(&impl_->game_context, impl_->callbacks.user_data);
        if (status.code != 0) {
            impl_->initialized = false;
            impl_->mode = RuntimeMode::kStopped;
            impl_->started = false;
            return rollback_initialize(
                foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            status.message ? status.message : "Game runtime start callback failed"));
        }
    }
    impl_->started = true;
    return foundation::Result::success();
}

void RuntimeBootstrap::set_callbacks(const GameRuntimeCallbacks& callbacks) noexcept {
    if (impl_ && !impl_->initialized)
        impl_->callbacks = callbacks;
}

RuntimeMode RuntimeBootstrap::mode() const noexcept {
    return impl_ ? impl_->mode : RuntimeMode::kStopped;
}

void RuntimeBootstrap::request_quit() noexcept {
    if (impl_) {
        impl_->quit_requested = true;
        impl_->frame.quit_requested = true;
    }
}

foundation::Result RuntimeBootstrap::submit_event(const platform::WindowEvent& event) {
    if (!impl_ || !impl_->initialized)
        return foundation::invalid_state("Runtime bootstrap is not initialized");
    if (!event.window.is_valid() && (impl_->request.headless || impl_->request.offscreen)) {
        // Headless/offscreen hosts have no native WindowId. Use a stable
        // synthetic identity so semantic events still follow the same input
        // path as windowed events.
        auto headless_event = event;
        headless_event.window = platform::WindowId{0, 1};
        return impl_->input_system.submit_event(headless_event);
    }
    if (impl_->owns_host && impl_->owned_window != nullptr && event.type == platform::WindowEventType::kWindowResized &&
        event.window == impl_->owned_window->id()) {
        impl_->pending_owned_resize = event.extent;
        impl_->owned_resize_pending = true;
        if (!event.extent.is_valid()) {
            impl_->owned_surface_suspended = true;
            return impl_->input_system.submit_event(event);
        }
    }
    return impl_->input_system.submit_event(event);
}

const RuntimeFrameContext& RuntimeBootstrap::last_frame_context() const noexcept {
    static const RuntimeFrameContext kEmpty{};
    return impl_ ? impl_->frame : kEmpty;
}

foundation::Result RuntimeBootstrap::tick() {
    if (!impl_ || !impl_->initialized)
        return foundation::invalid_state("Runtime bootstrap is not initialized");
    if (impl_->quit_requested)
        return foundation::Result::success();
    if (impl_->request.window_system != nullptr) {
        foundation::Result poll_result = foundation::Result::success();
        const foundation::Result result =
            impl_->request.window_system->poll_events([this, &poll_result](const platform::WindowEvent& event) {
                if (!poll_result) {
                    return;
                }
                poll_result = submit_event(event);
            });
        if (!result) {
            return result;
        }
        if (!poll_result) {
            return poll_result;
        }
    }
    if (const auto result = impl_->input_system.begin_frame(); !result)
        return result;
    struct InputFrameGuard final {
        input::InputSystem& input;
        RuntimeFrameContext& frame;
        ~InputFrameGuard() {
            input.end_frame();
            frame.events = {};
        }
    } input_frame_guard{impl_->input_system, impl_->frame};
    impl_->input = impl_->input_system.snapshot();
    impl_->frame.events = impl_->input_system.frame_events();
    for (const auto& event : impl_->frame.events) {
        if (event.type == platform::WindowEventType::kWindowCloseRequested)
            impl_->quit_requested = true;
    }
    // Frame numbers are one-based externally and must match the Snapshot
    // committed by the World path below.
    impl_->frame.frame_number = impl_->frames + 1u;
    const auto now = impl_->request.clock.now_seconds ? impl_->request.clock.now_seconds(impl_->request.clock.user_data)
                                                      : default_clock(nullptr);
    impl_->frame.real_delta_seconds = impl_->last_time < 0.0
                                          ? impl_->request.fixed_delta_seconds
                                          : static_cast<float>(std::max(0.0, now - impl_->last_time));
    impl_->last_time = now;
    impl_->frame.delta_seconds = (std::min)(impl_->frame.real_delta_seconds, impl_->request.max_frame_delta_seconds);
    impl_->frame.input = &impl_->input;
    impl_->frame.quit_requested = impl_->quit_requested;
    impl_->accumulator += impl_->frame.delta_seconds;
    impl_->frame.fixed_steps = 0;
    while (impl_->accumulator >= impl_->request.fixed_delta_seconds &&
           impl_->frame.fixed_steps < impl_->request.max_fixed_steps_per_frame) {
        FixedUpdateContext fixed{&impl_->frame, impl_->request.fixed_delta_seconds};
        if (impl_->callbacks.fixed_update) {
            const auto status = impl_->callbacks.fixed_update(&impl_->game_context, &fixed, impl_->callbacks.user_data);
            if (status.code != 0)
                return foundation::Result::failure(
                    foundation::ResultCode::kUnknown,
                    status.message ? status.message : "Game runtime fixed update failed");
        }
        impl_->accumulator -= impl_->request.fixed_delta_seconds;
        ++impl_->frame.fixed_steps;
        ++impl_->frame.fixed_tick;
    }
    impl_->frame.interpolation_alpha = impl_->accumulator / impl_->request.fixed_delta_seconds;
    UpdateContext update{&impl_->frame, impl_->frame.delta_seconds};
    if (impl_->callbacks.update) {
        const auto status = impl_->callbacks.update(&impl_->game_context, &update, impl_->callbacks.user_data);
        if (status.code != 0)
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               status.message ? status.message : "Game runtime update failed");
    }
    if (impl_->owns_host && !impl_->owned_resize_pending && impl_->owned_swapchain_info.needs_recreate) {
        impl_->pending_owned_resize = {impl_->owned_swapchain_info.extent.width,
                                       impl_->owned_swapchain_info.extent.height};
        impl_->owned_resize_pending = true;
    }
    if (impl_->owns_host && impl_->owned_resize_pending) {
        if (!impl_->pending_owned_resize.is_valid()) {
            // A minimized window has no drawable extent. Keep polling and
            // delivering input while deferring swapchain work until restore.
            impl_->owned_surface_suspended = true;
        } else if (const auto result = impl_->recreate_owned_swapchain(impl_->pending_owned_resize); !result) {
            return result;
        }
    }
    if (impl_->owns_host && impl_->owned_surface_suspended)
        return foundation::Result::success();
    if (impl_->orchestrator != nullptr) {
        render_graph::FrameAllocation readback_allocation{};
        bool readback_executed = false;
        const auto readback_format = impl_->owned_swapchain_info.format;
        engine::EngineFrameDesc frame_desc{};
        frame_desc.fixed_tick = impl_->frame.fixed_tick;
        frame_desc.render_output_extent = {
            impl_->owned_swapchain_info.extent.width != 0 ? impl_->owned_swapchain_info.extent.width
                                                          : impl_->request.width,
            impl_->owned_swapchain_info.extent.height != 0 ? impl_->owned_swapchain_info.extent.height
                                                           : impl_->request.height};
        frame_desc.user_data = &impl_->frame;
        frame_desc.renderer = impl_->request.frame_host->renderer;
        frame_desc.render_passes = impl_->passes.get();
        if (impl_->world_frame_registered_with_orchestrator && frame_desc.renderer != nullptr) {
            frame_desc.render_view_resolver = [impl = impl_.get()](const engine::EngineFrameRenderViewContext& context)
                -> foundation::ResultValue<renderer::RenderView> {
                ecs::EntityId camera =
                    impl->startup_scene != nullptr ? impl->startup_scene->startup_camera : ecs::EntityId{};
                if (!camera.is_valid() || context.world_snapshot.get<engine::components::Camera>(camera) == nullptr) {
                    camera = {};
                    context.world_snapshot.for_each<engine::components::Camera>(
                        [&camera](ecs::EntityId entity, const engine::components::Camera&) {
                            if (!camera.is_valid())
                                camera = entity;
                        });
                }
                if (!camera.is_valid()) {
                    return {foundation::not_found(
                                "Runtime startup World has no Camera component for its registered render view"),
                            {}};
                }
                auto view = engine::components::make_render_view(context.world_snapshot,
                                                                 engine::components::RuntimeRenderViewDesc{
                                                                     camera,
                                                                     context.view_id,
                                                                     context.output_extent,
                                                                     0xffffffffu,
                                                                     kRuntimeSceneColor,
                                                                     kRuntimeSceneDepth,
                                                                     false,
                                                                     math::ClipSpaceConvention{.y_down = true},
                                                                 });
                if (!view)
                    return view;
                // Owned windowed runs bind the renderer's logical scene color
                // directly to the acquired swapchain image. The final
                // post-process pass therefore leaves it in Present state.
                view.value.color_target_final_usage = rhi::ResourceUsage::kPresent;
                return view;
            };
        }
        impl_->request.frame_host->apply_frame_desc(frame_desc, impl_->asset_resolver.get());
        if (impl_->owns_host) {
            frame_desc.acquire_swapchain_image = true;
            const auto plan_has_present = [](const pipeline::PipelineScopeHost* host) {
                if (host == nullptr || host->active_plan() == nullptr)
                    return false;
                return std::any_of(host->active_plan()->nodes.begin(), host->active_plan()->nodes.end(),
                                   [](const auto& node) { return node.phase == pipeline::EnginePhase::kPresent; });
            };
            bool pipeline_has_present =
                std::any_of(impl_->pipeline_hosts.begin(), impl_->pipeline_hosts.end(),
                            [&plan_has_present](const auto& host) { return plan_has_present(host.get()); });
            if (!pipeline_has_present && impl_->startup_scene != nullptr && impl_->startup_scene->scope != nullptr)
                pipeline_has_present = plan_has_present(&impl_->startup_scene->scope->pipeline_host());
            frame_desc.present_acquired_frame = true;
            frame_desc.swapchain = impl_->owned_swapchain;
            frame_desc.swapchain_image_desc.extent = {frame_desc.render_output_extent.width,
                                                      frame_desc.render_output_extent.height, 1};
            frame_desc.swapchain_image_desc.format = impl_->owned_swapchain_info.format;
            frame_desc.swapchain_image_desc.capabilities = impl_->owned_swapchain_desc.image_capabilities;
            if (pipeline_has_present) {
                const auto depth_extent = frame_desc.render_output_extent;
                frame_desc.render_feature_preparation = [depth_extent](
                                                            const engine::EngineFramePreparationContext& context) {
                    if (context.acquired_frame == nullptr || !context.swapchain_resource.is_valid())
                        return foundation::invalid_state("Runtime owned render requires an acquired swapchain image");
                    if (const auto color = context.bind_feature_texture(kRuntimeSceneColor, context.swapchain_resource);
                        !color)
                        return color;
                    return bind_owned_scene_depth(context, depth_extent);
                };
            }
            if (!pipeline_has_present) {
                const auto depth_extent = frame_desc.render_output_extent;
                frame_desc.render_feature_preparation = [depth_extent](
                                                            const engine::EngineFramePreparationContext& context) {
                    if (context.acquired_frame == nullptr || !context.swapchain_resource.is_valid())
                        return foundation::invalid_state("Runtime owned Present requires an acquired swapchain image");
                    if (const auto color = context.bind_feature_texture(kRuntimeSceneColor, context.swapchain_resource);
                        !color)
                        return color;
                    if (const auto depth = bind_owned_scene_depth(context, depth_extent); !depth)
                        return depth;
                    const auto clear = context.render_graph.add_pass(
                        "Runtime.OwnedHost.ClearSwapchain", rhi::QueueClass::kGraphics,
                        [resource = context.swapchain_resource](render_graph::PassBuilder& builder) {
                            return builder.use_color_attachment(resource, rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                                                                rhi::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f},
                                                                rhi::ResourceUsage::kColorAttachmentWrite);
                        },
                        [](render_graph::RenderPassCommandContext& pass) {
                            if (const auto result = pass.begin_rendering(); !result)
                                return result;
                            return pass.end_rendering();
                        });
                    if (!clear)
                        return clear.result;
                    const auto present = context.render_graph.add_pass(
                        "Runtime.OwnedHost.Present", rhi::QueueClass::kGraphics,
                        [resource = context.swapchain_resource](render_graph::PassBuilder& builder) {
                            return builder.use_color_attachment(resource, rhi::LoadOp::kLoad, rhi::StoreOp::kStore,
                                                                rhi::ClearColorValue{}, rhi::ResourceUsage::kPresent);
                        },
                        [](render_graph::RenderPassCommandContext& pass) {
                            if (const auto result = pass.begin_rendering(); !result)
                                return result;
                            return pass.end_rendering();
                        });
                    if (!present)
                        return present.result;
                    return context.render_graph.add_dependency(clear.value, present.value);
                };
            }
        }
        if (!impl_->request.readback_path.empty()) {
            if (readback_format != rhi::Format::kRgba8Unorm && readback_format != rhi::Format::kRgba8Srgb &&
                readback_format != rhi::Format::kBgra8Unorm)
                return foundation::unsupported("Runtime framebuffer readback requires an RGBA8 or BGRA8 swapchain");
            const auto extent = frame_desc.render_output_extent;
            const uint64_t pixel_count = static_cast<uint64_t>(extent.width) * extent.height;
            if (pixel_count == 0 || pixel_count > (std::numeric_limits<std::size_t>::max)() / 4u)
                return foundation::invalid_argument("Runtime framebuffer readback extent is invalid");
            frame_desc.render_feature_post_build = [&readback_allocation, &readback_executed, extent,
                                                    pixel_count](const engine::EngineFramePostBuildContext& context) {
                if (context.acquired_frame == nullptr || !context.swapchain_resource.is_valid())
                    return foundation::invalid_state("Runtime framebuffer readback requires an acquired image");
                const auto allocation = context.frame_context.allocate_readback(pixel_count * 4u, 256);
                if (!allocation)
                    return allocation.result;
                readback_allocation = allocation.value;
                rhi::BufferDesc desc{};
                desc.size = allocation.value.buffer_size;
                desc.capabilities =
                    rhi::BufferCapability::kTransferSource | rhi::BufferCapability::kTransferDestination;
                desc.memory = rhi::MemoryClass::kReadback;
                desc.initial_usage = rhi::ResourceUsage::kHostRead;
                desc.initial_stages = rhi::PipelineStage::kHost;
                desc.initial_queue = rhi::QueueClass::kGraphics;
                desc.name = rhi::DebugName{"Runtime.FramebufferReadback"};
                const auto buffer = context.render_graph.import_buffer(
                    allocation.value.buffer, desc,
                    rhi::ResourceState{rhi::ResourceUsage::kHostRead, rhi::ShaderStage::kAll,
                                       rhi::QueueClass::kGraphics, false, rhi::PipelineStage::kHost});
                if (!buffer)
                    return buffer.result;
                const auto pass = context.render_graph.add_pass(
                    "Runtime.FramebufferReadback", rhi::QueueClass::kGraphics,
                    [color = context.swapchain_resource, buffer = buffer.value,
                     allocation = allocation.value](render_graph::PassBuilder& builder) {
                        if (const auto result =
                                builder.read_to(color, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy,
                                                rhi::ShaderStage::kAll, rhi::ResourceUsage::kPresent);
                            !result)
                            return result;
                        if (const auto result = builder.write_to(
                                buffer, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                rhi::ShaderStage::kAll, rhi::ResourceUsage::kHostRead,
                                render_graph::RenderGraphBufferRange{allocation.offset, allocation.size}, true);
                            !result)
                            return result;
                        builder.set_side_effects();
                        return foundation::Result::success();
                    },
                    [color = context.swapchain_resource, buffer = buffer.value, allocation = allocation.value, extent,
                     &readback_executed](render_graph::RenderPassCommandContext& command) {
                        const auto result = command.copy_texture_to_buffer(
                            color, rhi::TextureSubresourceLayers{rhi::Aspect::kColor, 0, 0, 1}, {},
                            rhi::Extent3D{extent.width, extent.height, 1}, buffer, allocation.offset, 0, 0);
                        readback_executed = bool(result);
                        return result;
                    });
                return pass ? foundation::Result::success() : pass.result;
            };
        }
        if (const auto result = impl_->orchestrator->run_frame(frame_desc); !result) {
            if (impl_->owns_host && (result.code == foundation::ResultCode::kOutOfDate ||
                                     result.code == foundation::ResultCode::kSurfaceLost)) {
                const platform::WindowExtent window_extent =
                    impl_->owned_window != nullptr ? impl_->owned_window->state().extent : platform::WindowExtent{};
                const platform::WindowExtent retry_extent{
                    window_extent.is_valid()
                        ? window_extent.width
                        : (impl_->owned_swapchain_info.extent.width != 0 ? impl_->owned_swapchain_info.extent.width
                                                                         : impl_->request.width),
                    window_extent.is_valid()
                        ? window_extent.height
                        : (impl_->owned_swapchain_info.extent.height != 0 ? impl_->owned_swapchain_info.extent.height
                                                                          : impl_->request.height)};
                if (const auto recreate = impl_->recreate_owned_swapchain(retry_extent); !recreate)
                    return foundation::Result::failure(recreate.code,
                                                       "Owned runtime swapchain recovery failed: " + recreate.message);
                // The frame was aborted by EngineFrameOrchestrator. Let the
                // next tick render against the fresh image set.
                return foundation::Result::success();
            }
            return result;
        }
        if (!impl_->request.readback_path.empty()) {
            if (!readback_allocation.is_valid() || !readback_executed)
                return foundation::invalid_state("Runtime framebuffer readback pass did not execute");
            if (impl_->owned_frame_context == nullptr)
                return foundation::invalid_state("Owned framebuffer readback has no FrameContext");
            auto& frame_context = *impl_->owned_frame_context;
            if (const auto result = frame_context.drain(); !result)
                return result;
            std::vector<std::byte> pixels(static_cast<std::size_t>(readback_allocation.size));
            if (const auto result = frame_context.read_readback(readback_allocation, pixels); !result)
                return result;
            const bool bgra = readback_format == rhi::Format::kBgra8Unorm;
            std::vector<std::byte> rgb(pixels.size() / 4u * 3u);
            for (std::size_t pixel = 0; pixel < pixels.size() / 4u; ++pixel) {
                rgb[pixel * 3u] = pixels[pixel * 4u + (bgra ? 2u : 0u)];
                rgb[pixel * 3u + 1u] = pixels[pixel * 4u + 1u];
                rgb[pixel * 3u + 2u] = pixels[pixel * 4u + (bgra ? 0u : 2u)];
            }
            std::ofstream readback(impl_->request.readback_path, std::ios::binary | std::ios::trunc);
            if (!readback)
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                   "Cannot open runtime framebuffer readback output");
            readback << "P6\n"
                     << frame_desc.render_output_extent.width << ' ' << frame_desc.render_output_extent.height
                     << "\n255\n";
            readback.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
            if (!readback)
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                   "Cannot write runtime framebuffer readback output");
        }
    } else {
        if (impl_->startup_scene != nullptr && impl_->startup_scene->scope != nullptr) {
            auto& scope = *impl_->startup_scene->scope;
            if (const auto result = scope.apply_commands(scope.command_buffer()); !result)
                return result;
            if (const auto committed = scope.commit(impl_->frame.frame_number, impl_->frame.fixed_tick); !committed)
                return committed.result;
            // A scene may legitimately be loaded without a World-scope
            // pipeline (for example an empty scaffold scene paired with a
            // frame-only render pipeline). In that case the World still
            // commits its snapshot, but there is no executable host to run.
            if (scope.pipeline_host().is_active() && scope.pipeline_host().active_plan() != nullptr) {
                pipeline::PipelineExecutionContext world_context(&impl_->frame, scope.pipeline_host().scope(),
                                                                 &scope.pipeline_host().artifacts());
                if (const auto result = scope.pipeline_host().execute(*impl_->scheduler, world_context); !result)
                    return result;
            }
        }
        for (const auto& host : impl_->pipeline_hosts) {
            if (!host || !impl_->scheduler) {
                return foundation::invalid_state("runtime Pipeline host is not initialized");
            }
            pipeline::PipelineExecutionContext context(&impl_->frame, host->scope(), &host->artifacts());
            if (const auto result = host->execute(*impl_->scheduler, context); !result) {
                return result;
            }
        }
    }
    ++impl_->frames;
    if (!impl_->request.trace_path.empty()) {
        std::ofstream trace(impl_->request.trace_path, std::ios::app);
        if (!trace)
            return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write runtime trace");
        trace << "frame " << impl_->frames << "\n";
    }
    return foundation::Result::success();
}

foundation::Result RuntimeBootstrap::shutdown() {
    if (!impl_)
        return foundation::Result::success();
    if (impl_->shutdown_started)
        return foundation::Result::success();
    impl_->shutdown_started = true;
    foundation::Result first_error = foundation::Result::success();
    if (impl_->initialized && impl_->started && impl_->callbacks.stop) {
        const auto status = impl_->callbacks.stop(&impl_->game_context, impl_->callbacks.user_data);
        if (status.code != 0 && first_error)
            first_error =
                foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            status.message ? status.message : "Game runtime stop callback failed");
    }
    if (impl_->orchestrator != nullptr && impl_->orchestrator->is_initialized()) {
        if (const auto result = impl_->orchestrator->shutdown(); !result) {
            if (first_error)
                first_error = result;
            const auto* frame_context =
                impl_->owned_frame_context != nullptr
                    ? impl_->owned_frame_context.get()
                    : (impl_->request.frame_host != nullptr ? impl_->request.frame_host->frame_context : nullptr);
            if (frame_context != nullptr && frame_context->is_frame_active()) {
                // Keep the orchestrator, pipeline hosts, and GPU resources
                // intact so the caller can retry after resolving the active
                // frame instead of tearing down live command state.
                impl_->shutdown_started = false;
                return first_error;
            }
        }
    } else {
        for (auto iterator = impl_->pipeline_hosts.rbegin(); iterator != impl_->pipeline_hosts.rend(); ++iterator) {
            if (*iterator && (*iterator)->is_active()) {
                if (const auto result = (*iterator)->deactivate({}); !result && first_error) {
                    first_error = result;
                }
            }
        }
    }
    if (impl_->asset_resolver != nullptr) {
        if (const auto result = impl_->asset_resolver->retire_gpu_resources(); !result) {
            if (first_error)
                first_error = result;
            // Keep the resolver and every borrowed/owned host dependency alive
            // so the caller can end an active frame and retry shutdown.
            impl_->shutdown_started = false;
            return first_error;
        }
    }
    if (impl_->world_registered_with_orchestrator && impl_->orchestrator != nullptr &&
        impl_->startup_scene != nullptr && impl_->startup_scene->scope != nullptr) {
        if (impl_->world_frame_registered_with_orchestrator) {
            if (const auto result =
                    impl_->orchestrator->unregister_world_frame(impl_->startup_scene->scope->handle(), 0);
                !result && first_error)
                first_error = result;
            impl_->world_frame_registered_with_orchestrator = false;
        }
        if (const auto result = impl_->orchestrator->unregister_world(impl_->startup_scene->scope->handle());
            !result && first_error) {
            first_error = result;
        }
        impl_->world_registered_with_orchestrator = false;
    }
    if (impl_->startup_scene != nullptr && impl_->startup_scene->scope != nullptr) {
        if (const auto result = impl_->startup_scene->scope->shutdown(); !result && first_error)
            first_error = result;
    }
    impl_->startup_scene.reset();
    impl_->world_frame_registered_with_orchestrator = false;
    impl_->orchestrator.reset();
    impl_->pipeline_hosts.clear();
    impl_->scheduler.reset();
    if (impl_->owns_host) {
        if (impl_->owned_device != nullptr) {
            if (const auto result = impl_->owned_device->wait_idle(); !result && first_error)
                first_error = result;
            impl_->destroy_owned_graphics_resources();
            if (impl_->owned_swapchain.is_valid()) {
                if (const auto result = impl_->owned_device->destroy_swapchain(impl_->owned_swapchain);
                    !result && first_error)
                    first_error = result;
                impl_->owned_swapchain = {};
            }
            if (impl_->owned_surface.is_valid()) {
                if (const auto result = impl_->owned_device->destroy_surface(impl_->owned_surface);
                    !result && first_error)
                    first_error = result;
                impl_->owned_surface = {};
            }
        }
        impl_->owned_render_graph.reset();
        impl_->owned_frame_context.reset();
        impl_->owned_renderer.reset();
        impl_->owned_device.reset();
        if (impl_->owned_window != nullptr && impl_->owned_window_system != nullptr) {
            if (const auto result = impl_->owned_window_system->destroy_window(impl_->owned_window->id());
                !result && first_error)
                first_error = result;
        }
        impl_->owned_window.reset();
        impl_->owned_window_system.reset();
        impl_->owned_swapchain_info = {};
        impl_->owned_swapchain_desc = {};
        impl_->pending_owned_resize = {};
        impl_->owned_resize_pending = false;
        impl_->owned_surface_suspended = false;
        impl_->owned_frame_host = {};
        impl_->owns_host = false;
        impl_->request.window_system = nullptr;
        impl_->request.frame_host = nullptr;
    }
    impl_->modules.reset();
    impl_->passes.reset();
    impl_->reflection.reset();
    impl_->pipelines.reset();
    impl_->startup_pipelines.clear();
    impl_->catalog = {};
    impl_->package.reset();
    impl_->asset_resolver.reset();
    impl_->startup_scene_id = {};
    impl_->started = false;
    impl_->input_system.reset();
    impl_->frame.events = {};
    impl_->mode = RuntimeMode::kStopped;
    impl_->initialized = false;
    impl_->last_time = -1.0;
    return first_error;
}

uint32_t RuntimeBootstrap::frame_count() const noexcept {
    return impl_ ? impl_->frames : 0;
}

std::size_t RuntimeBootstrap::startup_pipeline_count() const noexcept {
    return impl_ ? impl_->startup_pipelines.size() : 0;
}

world::WorldScope* RuntimeBootstrap::startup_world_scope() const noexcept {
    return impl_ && impl_->startup_scene ? impl_->startup_scene->scope.get() : nullptr;
}

const RuntimeFrameHostResources* RuntimeBootstrap::frame_host_resources() const noexcept {
    return impl_ && impl_->initialized ? impl_->request.frame_host : nullptr;
}

int run_game(int argc, char** argv, module::ModuleDefinition definition, const RuntimeBuildIdentity& identity,
             GameConfigurationCallback configure) {
    const auto report_failure = [](const foundation::Result& result) {
        std::cerr << "AEG runtime startup failed [" << foundation::result_code_name(result.code)
                  << "]: " << result.message << '\n';
        return static_cast<int>(result.code) + 1;
    };
    RuntimeBootRequest request{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--manifest" && index + 1 < argc)
            request.manifest_path = argv[++index];
        else if (argument == "--package" && index + 1 < argc)
            request.package_path = argv[++index];
        else if (argument == "--frames" && index + 1 < argc) {
            try {
                const unsigned long frames = std::stoul(argv[++index]);
                if (frames > UINT32_MAX) {
                    std::cerr << "AEG runtime argument error: --frames exceeds UINT32_MAX\n";
                    return 2;
                }
                request.max_frames = static_cast<uint32_t>(frames);
            } catch (...) {
                std::cerr << "AEG runtime argument error: --frames must be an unsigned integer\n";
                return 2;
            }
        } else if (argument == "--headless")
            request.headless = true;
        else if (argument == "--offscreen")
            request.offscreen = true;
        else if (argument == "--trace" && index + 1 < argc)
            request.trace_path = argv[++index];
        else if (argument == "--readback" && index + 1 < argc)
            request.readback_path = argv[++index];
    }
    if (configure) {
        if (const auto result = configure(request); !result)
            return report_failure(result);
    }
    RuntimeBootstrap bootstrap;
    if (const auto result = bootstrap.initialize(request, std::move(definition), identity); !result)
        return report_failure(result);
    const uint32_t frames = request.max_frames == 0 ? 1 : request.max_frames;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (const auto result = bootstrap.tick(); !result)
            return report_failure(result);
        if (bootstrap.last_frame_context().quit_requested)
            break;
    }
    if (const auto result = bootstrap.shutdown(); !result)
        return report_failure(result);
    return 0;
}
}  // namespace aeg::application::runtime
