#include "runtime_bootstrap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <sstream>
#include <optional>
#include <system_error>

#include "Components/component_render_extraction.hpp"
#include "application_platform.hpp"
#include "asset_package_reader.hpp"
#include "concurrency.hpp"
#include "deferred_lighting_resources.hpp"
#include "engine_frame_orchestrator.hpp"
#include "engine_frame_pass_registry.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_world_commands.hpp"
#include "frame_context.hpp"
#include "feature_graph_runtime_host.hpp"
#include "../../../Engine/Feature/private/feature_clock_admission.hpp"
#include "feature_world_commands.hpp"
#include "runtime_feature_control.hpp"
#include "game_build_manifest.hpp"
#include "input_system.hpp"
#include "light_culling.hpp"
#include "clustered_forward.hpp"
#include "module.hpp"
#include "module_api.h"
#include "module_bundle.hpp"
#include "dynamic_module.hpp"
#include "post_process.hpp"
#include "render_graph.hpp"
#include "render_feature_history.hpp"
#include "render_feature_draw.hpp"
#include "render_pass.hpp"
#include "render_pass_registry_execution.hpp"
#include "renderer_variants_internal.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"
#include "runtime_scene_loader.hpp"
#include "scene_payload.hpp"
#include "../../../Engine/Shader/private/shader_pipeline_layout_access.hpp"
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

foundation::ResultValue<std::filesystem::path> resolve_cooked_module_manifest(
    const std::filesystem::path& delivery_root, std::string_view relative_name) {
    const std::filesystem::path relative(relative_name);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        return {foundation::invalid_argument("Cooked Feature module manifest path must be relative"), {}};
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized.begin()->generic_string() != "module")
        return {foundation::invalid_argument("Cooked Feature module manifest must be under module/"), {}};
    for (const auto& component : normalized) {
        if (component == "..")
            return {foundation::invalid_argument("Cooked Feature module manifest path escapes delivery root"), {}};
    }
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(delivery_root, error);
    if (error)
        return {foundation::invalid_argument("Cooked delivery root could not be canonicalized"), {}};
    const auto candidate = std::filesystem::weakly_canonical(delivery_root / normalized, error);
    if (error)
        return {foundation::invalid_argument("Cooked Feature module manifest path could not be canonicalized"), {}};
    const auto relative_to_root = candidate.lexically_relative(root);
    if (relative_to_root.empty() || relative_to_root.begin()->generic_string() == "..")
        return {foundation::invalid_argument("Cooked Feature module manifest path escapes delivery root"), {}};
    return {foundation::Result::success(), candidate};
}

foundation::ResultValue<asset::GameBuildManifest> read_binary_manifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {foundation::not_found("Binary game manifest was not found"), {}};
    const auto end = input.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > asset::kMaxGameBuildManifestBytes)
        return {foundation::invalid_argument("Binary game manifest size is invalid"), {}};
    input.seekg(0);
    if (!input)
        return {foundation::invalid_argument("Binary game manifest cannot be seeked"), {}};
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.read(reinterpret_cast<char*>(bytes.data()), end);
    if (!input)
        return {foundation::invalid_argument("Binary game manifest cannot be read"), {}};
    return asset::decode_game_build_manifest(bytes);
}

RuntimeWorldCommandBinding runtime_world_command_binding(const asset::WorldCommandRoute& route) {
    RuntimeWorldCommandBinding binding{};
    binding.graph_asset_uuid = route.graph_asset_uuid;
    binding.node_id = route.node_id;
    binding.owner_path = route.owner_path;
    binding.scene_asset = route.scene_asset;
    binding.scene_entity = route.scene_entity;
    binding.effect = route.effect;
    binding.version = route.version;
    binding.observation_port = route.observation_port;
    return binding;
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

foundation::ResultValue<std::filesystem::path> resolve_manifest_selector(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::Result::success(), path};
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("manifest = \"") || !line.ends_with("\""))
        return {foundation::Result::success(), path};
    const auto relative = std::filesystem::path(line.substr(12, line.size() - 13));
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        return {foundation::invalid_argument("Game manifest selector path must be relative"), {}};
    const auto normalized = relative.lexically_normal();
    for (const auto& component : normalized) {
        if (component == "..")
            return {foundation::invalid_argument("Game manifest selector escapes output directory"), {}};
    }
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(path.parent_path(), error);
    if (error)
        return {foundation::invalid_argument("Game manifest selector root could not be canonicalized"), {}};
    const auto candidate = std::filesystem::weakly_canonical(path.parent_path() / normalized, error);
    if (error)
        return {foundation::invalid_argument("Selected game manifest could not be canonicalized"), {}};
    const auto relative_to_root = candidate.lexically_relative(root);
    if (relative_to_root.empty() || relative_to_root.begin()->generic_string() == "..")
        return {foundation::invalid_argument("Game manifest selector escapes output directory"), {}};
    return {foundation::Result::success(), candidate};
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
    foundation::AssetId startup_scene_id{};
    std::unique_ptr<module::ModuleRegistry> modules{};
    std::unique_ptr<module::DynamicModuleLoader> dynamic_loader{};
    module::DynamicModuleHandle dynamic_module{};
    std::unique_ptr<pipeline::PipelineRegistry> pipelines{};
    std::unique_ptr<reflection::TypeRegistry> reflection{};
    std::unique_ptr<pipeline::RenderPassRegistry> passes{};
    std::size_t startup_feature_graph_count{};
    std::unique_ptr<pipeline::PipelineScheduler> scheduler{};
    std::unique_ptr<feature::FeatureRegistry> feature_registry{};
    feature::detail::ClockHostContextPtr clock_host{};
    feature::FeatureRuntimeOptions feature_runtime_options{};
    std::unique_ptr<feature::FeatureRuntime> feature_runtime{};
    std::unique_ptr<FeatureGraphRuntimeHost> feature_graph_host{};
    std::unique_ptr<engine::detail::FeatureWorldCommandConsumer> world_commands{};
    std::unique_ptr<engine::EngineFramePipelineHost> pipeline_host{};
    std::unique_ptr<engine::EngineFramePassRegistry> pass_registry_host{};
    std::unique_ptr<engine::EngineFrameOrchestrator> orchestrator{};
    std::unique_ptr<platform::IWindowSystem> owned_window_system{};
    std::shared_ptr<platform::IWindow> owned_window{};
    std::unique_ptr<rhi::Device> owned_device{};
    rhi::SurfaceHandle owned_surface{};
    rhi::SwapchainHandle owned_swapchain{};
    rhi::SwapchainDesc owned_swapchain_desc{};
    std::unique_ptr<render_graph::FrameContext> owned_frame_context{};
    std::unique_ptr<render_graph::RenderGraph> owned_render_graph{};
    std::unique_ptr<render_graph::RenderFeatureHistory> render_history{};
    std::unique_ptr<renderer::Renderer> owned_renderer{};
    std::unique_ptr<resource::ResourceUploader> owned_resource_uploader{};
    std::unique_ptr<resource::TextureUploader> owned_texture_uploader{};
    std::unique_ptr<shader::ShaderManager> owned_shader_manager{};
    std::unique_ptr<shader::ShaderProgram> owned_material_shader_program{};
    std::unique_ptr<shader::ShaderPipelineLayout> owned_shader_layout{};
    std::unique_ptr<shader::ShaderPipelineLayout> owned_depth_shader_layout{};
    std::unique_ptr<shader::ShaderPipelineLayout> owned_gbuffer_shader_layout{};
    std::unique_ptr<shader::ShaderPipelineLayout> owned_shadow_shader_layout{};
    std::unique_ptr<renderer::ShadowResources> owned_shadow_resources{};
    std::unique_ptr<renderer::LightCullingResources> owned_light_culling{};
    std::unique_ptr<renderer::DeferredLightingResources> owned_deferred_lighting{};
    std::unique_ptr<renderer::PostProcessResources> owned_post_process{};
    rhi::BindingLayoutHandle owned_object_layout{};
    rhi::BindingLayoutHandle owned_material_layout{};
    rhi::BindingLayoutHandle owned_lighting_layout{};
    rhi::PipelineHandle owned_material_pipeline{};
    rhi::PipelineHandle owned_material_depth_pipeline{};
    rhi::PipelineHandle owned_shadow_pipeline{};
    renderer::RenderMaterialShaderContract owned_shadow_shader_contract{};
    renderer::RenderMaterialAttachmentContract owned_shadow_pipeline_contract{};
    rhi::PipelineHandle owned_clustered_pipeline{};
    renderer::RenderMaterialAttachmentContract owned_clustered_contract{};
    renderer::RendererProfilePsoContract owned_clustered_profile_contract{};
    renderer::RenderMaterialPipelineVariant owned_unshadowed_clustered{};
    rhi::PipelineHandle owned_depth_pipeline{};
    renderer::RendererProfilePsoContract owned_depth_profile_contract{};
    rhi::PipelineHandle owned_gbuffer_pipeline{};
    renderer::RendererProfilePsoContract owned_gbuffer_profile_contract{};
    RuntimeAssetUploadHost owned_upload_context{};
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
    [[nodiscard]] foundation::Result bind_world_commands();
    void destroy_owned_graphics_resources() noexcept;
};

foundation::Result RuntimeBootstrap::Impl::bind_world_commands() {
    if (request.world_command_bindings.empty()) return foundation::Result::success();
    if (!feature_runtime || !feature_graph_host || !reflection || !startup_scene || !startup_scene->scope ||
        !startup_scene->scope->is_initialized() || startup_scene_id == foundation::AssetId{} ||
        startup_scene->instance.asset() != startup_scene->asset.id())
        return foundation::invalid_state("explicit World commands require a published FeatureGraph and loaded Scene World");
    try {
        auto consumer = std::make_unique<engine::detail::FeatureWorldCommandConsumer>(*feature_runtime, *reflection);
        for (const auto& binding : request.world_command_bindings) {
            const auto graph_asset = foundation::AssetId::parse(binding.graph_asset_uuid);
            if (!graph_asset || graph_asset.value == foundation::AssetId{} || !binding.node_id.is_valid() ||
                !binding.effect.is_valid() || binding.version == 0 || binding.scene_entity == foundation::Uuid{} ||
                std::any_of(binding.owner_path.nodes.begin(), binding.owner_path.nodes.end(),
                            [](feature::FeatureNodeId node) { return !node.is_valid(); }))
                return foundation::invalid_argument("World command binding has an invalid authored identity or protocol");
            if (binding.scene_asset != startup_scene_id)
                return foundation::invalid_argument("World command binding targets another Scene asset");
            const auto execution = feature_graph_host->inspect_execution(binding.graph_asset_uuid);
            if (!execution) return execution.result;
            const FeatureRuntimeExecutionProvenance* selected = nullptr;
            for (const auto& candidate : execution.value.provenance) {
                if (candidate.authored_node_id != binding.node_id ||
                    candidate.authored_instance_path.nodes != binding.owner_path.nodes) continue;
                if (selected)
                    return foundation::invalid_argument("World command binding has ambiguous authored owner attribution");
                selected = &candidate;
            }
            if (!selected)
                return foundation::invalid_argument("World command binding has no exact published authored owner");
            const auto entity = startup_scene->instance.runtime_entity(RuntimeSceneLoader::scene_entity_id(binding.scene_entity));
            if (!entity) return entity.result;
            if (const auto result = consumer->bind(selected->runtime.instance_id, *startup_scene->scope,
                                                   entity.value, binding.effect, binding.version,
                                                   binding.observation_port); !result)
                return result;
        }
        world_commands = std::move(consumer);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Runtime World command binding allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("Runtime World command binding threw an exception");
    }
}

inline constexpr feature::RenderResourceId kRuntimeSceneColor{"Renderer.SceneColor"};
inline constexpr feature::RenderResourceId kRuntimeSceneDepth{"Renderer.SceneDepth"};
inline constexpr feature::RenderResourceId kRuntimeShadowDepth{"Renderer.ShadowDepth"};
inline constexpr feature::RenderResourceId kRuntimeSceneLighting{"Renderer.SceneLighting"};

inline constexpr std::array kRuntimeGBufferTargets{feature::RenderResourceId("Renderer.GBuffer.Albedo"),
    feature::RenderResourceId("Renderer.GBuffer.Normal"), feature::RenderResourceId("Renderer.GBuffer.Material")};
foundation::Result bind_owned_render_target(const engine::EngineFramePreparationContext& context,
                                          renderer::RenderExtent extent,
                                          feature::RenderResourceId target = kRuntimeSceneDepth,
                                          rhi::Format format = rhi::Format::kD32Float,
                                          rhi::TextureCapability capabilities = rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kSampled) {
    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::kTex2D;
    desc.extent = {extent.width, extent.height, 1};
    desc.format = format;
    desc.capabilities = capabilities;
    desc.initial_usage = rhi::ResourceUsage::kUndefined;
    desc.name = rhi::DebugName{"Runtime.FeatureAttachment"};
    if (desc.extent.width == 0 || desc.extent.height == 0)
        return foundation::invalid_argument("Runtime owned attachment extent is invalid");
    const auto texture = context.render_graph.create_texture(desc, render_graph::ResourceLifetime::kTransient);
    if (!texture) return texture.result;
    return context.bind_feature_texture(target, texture.value);
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
    std::filesystem::path unshadowed_clustered_package = request.forward_clustered_unshadowed_shader_package;
#if defined(AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH)
    if (unshadowed_clustered_package.empty())
        unshadowed_clustered_package = AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH;
#endif
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
        if (request.material_shader != foundation::AssetId{})
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
    auto opaque_graphics = graphics;
    opaque_graphics.depth_test_enable = false;
    opaque_graphics.depth_write_enable = false;
    opaque_graphics.depth_stencil_format = rhi::Format::kUnknown;
    const auto pipeline = owned_shader_manager->create_pipeline(
        *owned_material_shader_program, *owned_shader_layout,
        shader::ShaderPipelineDesc{rhi::PipelineType::kGraphics, opaque_graphics, rhi::DebugName{"Runtime.Opaque.Pipeline"}});
    if (!pipeline)
        return pipeline.result;
    owned_material_pipeline = pipeline.value;
    const auto material_depth_pipeline = owned_shader_manager->create_pipeline(
        *owned_material_shader_program, *owned_shader_layout,
        shader::ShaderPipelineDesc{rhi::PipelineType::kGraphics, graphics, rhi::DebugName{"Runtime.Opaque.DepthAttachment.Pipeline"}});
    if (!material_depth_pipeline) return material_depth_pipeline.result;
    owned_material_depth_pipeline = material_depth_pipeline.value;

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
        owned_depth_shader_layout = std::make_unique<shader::ShaderPipelineLayout>(depth_layout.value);
        const auto epoch = shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*owned_depth_shader_layout);
        owned_depth_profile_contract = {
            {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kShadow,
             shader::detail::ShaderPipelineLayoutAccess::contract_hash(*owned_depth_shader_layout), epoch},
            {rhi::Format::kUnknown, depth_graphics.samples, depth_graphics.depth_stencil_format}, epoch};
    }

    renderer::RenderMaterialAttachmentContract gbuffer_contract{};
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
        gbuffer_contract = {gbuffer_graphics.color_formats[0], gbuffer_graphics.samples,
            gbuffer_graphics.depth_stencil_format, {gbuffer_graphics.color_formats[1], gbuffer_graphics.color_formats[2]}};
        owned_gbuffer_shader_layout = std::make_unique<shader::ShaderPipelineLayout>(gbuffer_layout.value);
        const auto epoch = shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*owned_gbuffer_shader_layout);
        owned_gbuffer_profile_contract = {
            {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kGBuffer,
             shader::detail::ShaderPipelineLayoutAccess::contract_hash(*owned_gbuffer_shader_layout), epoch},
            gbuffer_contract, epoch};
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
        owned_shadow_shader_layout = std::make_unique<shader::ShaderPipelineLayout>(shadow_layout.value);
        owned_shadow_shader_contract = {
            renderer::kRenderMaterialShaderContractVersion,
            shader::detail::ShaderPipelineLayoutAccess::contract_hash(*owned_shadow_shader_layout),
            shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*owned_shadow_shader_layout)};
        owned_shadow_pipeline_contract = {graphics.color_formats.front(), graphics.samples,
                                          graphics.depth_stencil_format};
        owned_shadow_resources = std::make_unique<renderer::ShadowResources>();
        if (const auto result = owned_shadow_resources->initialize(*owned_device, shadow_inputs.value); !result)
            return result;
    }

    if (!forward_clustered_package.empty()) {
        const auto variants = renderer::create_clustered_forward_pipelines(
            *owned_device, *owned_shader_manager, graphics, forward_clustered_package, unshadowed_clustered_package);
        if (!variants) return variants.result;
        owned_clustered_pipeline = variants.value.shadowed.pipeline;
        owned_unshadowed_clustered = variants.value.unshadowed;
        owned_clustered_contract = variants.value.shadowed.attachment_contract;
        owned_clustered_profile_contract = variants.value.shadowed.profile_contract;
        if (!light_culling_package.empty()) {
            owned_light_culling = std::make_unique<renderer::LightCullingResources>();
            if (const auto result =
                    owned_light_culling->initialize(*owned_device, light_culling_package, variants.value.shadowed_inputs,
                                                   variants.value.unshadowed_inputs);
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
    owned_upload_context.material_attachment_contract = {owned_swapchain_info.format};
    owned_upload_context.depth_attachment_variant = renderer::RenderMaterialPipelineVariant{
        owned_material_depth_pipeline,
        {owned_swapchain_info.format, rhi::SampleCount::kX1, rhi::Format::kD32Float},
        {renderer::kRenderMaterialShaderContractVersion,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(*owned_shader_layout),
         shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*owned_shader_layout)}};
    owned_upload_context.material_shader = request.material_shader;
    owned_upload_context.material_shader_program = owned_material_shader_program.get();
    owned_upload_context.material_shader_layout = owned_shader_layout.get();
    owned_upload_context.material_shader_contract = {
        renderer::kRenderMaterialShaderContractVersion,
        shader::detail::ShaderPipelineLayoutAccess::contract_hash(*owned_shader_layout),
        shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*owned_shader_layout)};
    owned_upload_context.variant_cache_epoch =
        shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*owned_shader_layout);
    owned_upload_context.depth_attachment_variant->cache_epoch = owned_upload_context.variant_cache_epoch;
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
    owned_upload_context.shadow_pipeline_contract = owned_shadow_pipeline_contract;
    owned_upload_context.shadow_shader_contract = owned_shadow_shader_contract;
    owned_upload_context.shadow_shader_layout = owned_shadow_shader_layout.get();
    owned_upload_context.shadow_pipeline_cache_epoch = owned_shadow_pipeline.is_valid()
        ? owned_upload_context.variant_cache_epoch : 0;
    owned_upload_context.clustered_pipeline = owned_clustered_pipeline;
    owned_upload_context.clustered_pipeline_contract = owned_clustered_contract;
    owned_upload_context.clustered_profile_contract = owned_clustered_profile_contract;
    if (owned_unshadowed_clustered.pipeline.is_valid())
        owned_upload_context.unshadowed_clustered_variant = owned_unshadowed_clustered;
    owned_upload_context.depth_pipeline = owned_depth_pipeline;
    owned_upload_context.depth_pipeline_contract = owned_depth_profile_contract.attachment;
    owned_upload_context.depth_profile_contract = owned_depth_profile_contract;
    owned_upload_context.depth_shader_layout = owned_depth_shader_layout.get();
    owned_upload_context.gbuffer_pipeline = owned_gbuffer_pipeline;
    owned_upload_context.gbuffer_pipeline_contract = gbuffer_contract;
    owned_upload_context.gbuffer_profile_contract = owned_gbuffer_profile_contract;
    owned_upload_context.gbuffer_shader_layout = owned_gbuffer_shader_layout.get();
    owned_frame_host.upload_context = &owned_upload_context;
    owned_frame_host.require_resolved_render_resources = true;
    return foundation::Result::success();
}

void RuntimeBootstrap::Impl::destroy_owned_graphics_resources() noexcept {
    // Resolver callbacks are frame-borrowed. The caller invokes this only
    // after resolver retirement and device idle, so release callback state
    // before tearing down the resource owners they capture.
    // RuntimeAssetResolver borrows this host. Clear the borrowed pointer before
    // destroying the frame/device objects; otherwise its destructor could
    // retain a dangling host pointer and attempt a second retirement.
    if (asset_resolver != nullptr)
        asset_resolver->set_upload_context(nullptr);
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
    if (owned_device != nullptr && owned_material_depth_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_material_depth_pipeline);
    owned_material_depth_pipeline = {};
    if (owned_device != nullptr && owned_shadow_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_shadow_pipeline);
    owned_shadow_pipeline = {};
    owned_shadow_shader_contract = {};
    owned_shadow_pipeline_contract = {};
    if (owned_device != nullptr && owned_clustered_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_clustered_pipeline);
    owned_clustered_pipeline = {};
    owned_clustered_contract = {};
    owned_clustered_profile_contract = {};
    if (owned_device != nullptr && owned_unshadowed_clustered.pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_unshadowed_clustered.pipeline);
    owned_unshadowed_clustered = {};
    if (owned_device != nullptr && owned_depth_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_depth_pipeline);
    owned_depth_pipeline = {};
    owned_depth_profile_contract = {};
    if (owned_device != nullptr && owned_gbuffer_pipeline.is_valid())
        (void)owned_device->destroy_pipeline(owned_gbuffer_pipeline);
    owned_gbuffer_pipeline = {};
    owned_gbuffer_profile_contract = {};
    owned_object_layout = {};
    owned_material_layout = {};
    owned_lighting_layout = {};
    owned_shader_layout.reset();
    owned_depth_shader_layout.reset();
    owned_gbuffer_shader_layout.reset();
    owned_shadow_shader_layout.reset();
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
    if (render_feature_preparation)
        frame_desc.render_feature_preparation = std::cref(render_feature_preparation);
}

RuntimeBootstrap::RuntimeBootstrap() : impl_(std::make_unique<Impl>()) {}
RuntimeBootstrap::~RuntimeBootstrap() {
    (void)shutdown();
}

foundation::Result RuntimeBootstrap::initialize(const RuntimeBootRequest& request,
                                                std::optional<module::FeatureModuleDefinition> definition,
                                                const RuntimeBuildIdentity& identity) {
    return initialize(request, std::move(definition), identity, {});
}

foundation::Result RuntimeBootstrap::initialize(const RuntimeBootRequest& request,
                                                std::optional<module::FeatureModuleDefinition> definition,
                                                const RuntimeBuildIdentity& identity,
                                                const GameRuntimeCallbacks& callbacks) {
    if (!impl_ || impl_->initialized)
        return foundation::invalid_state("Runtime bootstrap is already initialized");
    if (impl_->package != nullptr || impl_->orchestrator != nullptr || impl_->world_commands != nullptr || impl_->owns_host) {
        // A failed initialization must not leave borrowed registry references
        // or owned GPU/window resources behind when the caller retries.  A
        // previous failed shutdown is retryable; never raw-reset these owners.
        impl_->shutdown_started = false;
        if (const auto cleanup = shutdown(); !cleanup)
            return cleanup;
    }
    if (identity.build_id.empty() || identity.target.empty() || identity.configuration.empty())
        return foundation::invalid_argument("Runtime build identity is incomplete");
    if (definition && (!definition->status || !definition->descriptor.id.is_valid()))
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
    impl_->world_commands.reset();
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
    impl_->owned_depth_shader_layout.reset();
    impl_->owned_gbuffer_shader_layout.reset();
    impl_->owned_shadow_shader_layout.reset();
    impl_->owned_material_shader_program.reset();
    impl_->owned_shader_manager.reset();
    impl_->owned_upload_context = {};
    impl_->owned_material_pipeline = {};
    impl_->owned_material_depth_pipeline = {};
    impl_->owned_shadow_pipeline = {};
    impl_->owned_shadow_shader_contract = {};
    impl_->owned_shadow_pipeline_contract = {};
    impl_->owned_clustered_pipeline = {};
    impl_->owned_clustered_contract = {};
    impl_->owned_clustered_profile_contract = {};
    impl_->owned_unshadowed_clustered = {};
    impl_->owned_depth_pipeline = {};
    impl_->owned_depth_profile_contract = {};
    impl_->owned_gbuffer_pipeline = {};
    impl_->owned_gbuffer_profile_contract = {};
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
    if (manifest.empty()) {
        manifest = std::filesystem::current_path() / "game.manifest";
        if (!std::filesystem::exists(manifest) && std::filesystem::exists(manifest.parent_path() / "current.toml"))
            manifest = manifest.parent_path() / "current.toml";
    }
    const auto selected_manifest = resolve_manifest_selector(manifest);
    if (!selected_manifest)
        return selected_manifest.result;
    manifest = selected_manifest.value;
    foundation::ResultValue<Manifest> manifest_result{};
    // Accept one snapshot for all identity, package, module, startup roots, and routing decisions.
    std::optional<asset::GameBuildManifest> cooked_manifest{};
    if (manifest.extension() == ".bin") {
        manifest_result.result = foundation::Result::success();
    } else {
        manifest_result = read_manifest(manifest);
        if (!manifest_result)
            return manifest_result.result;
    }
    const auto binary_path = manifest.extension() == ".bin" ? manifest : manifest.parent_path() / "game.manifest.bin";
    if (std::filesystem::exists(binary_path)) {
        auto binary = read_binary_manifest(binary_path);
        if (!binary)
            return binary.result;
        cooked_manifest = std::move(binary.value);
        const auto& accepted = *cooked_manifest;
        if (accepted.build_id != identity.build_id || accepted.target != identity.target ||
            accepted.configuration != identity.configuration ||
            accepted.module_contract_digest.empty() ||
            (!definition && (accepted.module_id.empty() || accepted.module_manifest_name.empty() ||
                             accepted.module_manifest_sha256.empty())) ||
            (definition && pipeline::ModuleId(accepted.module_id).value() != definition->descriptor.id.value()))
            return foundation::invalid_argument("Binary game manifest identity does not match executable");
        module::ModuleMetadata module_metadata{};
        // Match the canonical metadata contract emitted by the module tooling.
        // These fields participate in module_contract_sha256 and therefore
        // must not be left at ModuleMetadata's legacy defaults when validating
        // a cooked binary manifest.
        module_metadata.schema_version = module::kModuleMetadataSchemaVersion;
        if (!definition) {
            // Dynamic module metadata is validated after its DLL factory is loaded.
        } else {
            module_metadata.module = definition->descriptor;
            module_metadata.feature_catalog = definition->feature_catalog;
        }
        module_metadata.abi_major = AEG_MODULE_ABI_MAJOR;
        module_metadata.abi_minor = AEG_MODULE_ABI_MINOR;
        module_metadata.sdk_contract_major = module::kModuleSdkContractMajor;
        module_metadata.sdk_contract_minor = module::kModuleSdkContractMinor;
        const auto contract = module::module_contract_sha256(module_metadata);
        if (definition && (!contract || contract.value != accepted.module_contract_digest)) {
            return foundation::invalid_argument(
                "Binary game manifest module contract does not match executable (expected " +
                accepted.module_contract_digest + ", executable " +
                (contract ? contract.value : std::string("<unavailable>")) + ")");
        }
        if ((!manifest_result.value.build_id.empty() && manifest_result.value.build_id != accepted.build_id) ||
            (!manifest_result.value.target.empty() && manifest_result.value.target != accepted.target) ||
            (!manifest_result.value.configuration.empty() &&
             manifest_result.value.configuration != accepted.configuration))
            return foundation::invalid_argument("Text and binary game manifests disagree");
        manifest_result.value.package = accepted.package_name;
        impl_->startup_scene_id = accepted.startup_scene;
        if (!accepted.world_command_routes.empty()) {
            if (!effective.world_command_bindings.empty())
                return foundation::invalid_argument("Cooked World command routes cannot be combined with host bindings");
            effective.world_command_bindings.reserve(accepted.world_command_routes.size());
            for (const auto& route : accepted.world_command_routes)
                effective.world_command_bindings.push_back(runtime_world_command_binding(route));
        }
    }
    if (!manifest_result.value.build_id.empty() && manifest_result.value.build_id != identity.build_id)
        return foundation::invalid_argument("Game manifest build identity does not match executable");
    if (!manifest_result.value.target.empty() && manifest_result.value.target != identity.target)
        return foundation::invalid_argument("Game manifest target does not match executable");
    if (!manifest_result.value.configuration.empty() && manifest_result.value.configuration != identity.configuration)
        return foundation::invalid_argument("Game manifest configuration does not match executable");
    if (effective.package_path.empty())
        effective.package_path = manifest.parent_path() / manifest_result.value.package;

    auto package = asset::AssetPackageReader::open(effective.package_path);
    if (!package)
        return package.result;
    if (cooked_manifest) {
        const auto& accepted = *cooked_manifest;
        const auto package_digest = file_digest(effective.package_path);
        if (!package_digest)
            return package_digest.result;
        if (package_digest.value != accepted.package_digest)
            return foundation::invalid_argument("Asset package digest does not match game manifest");
        const auto scene = package.value.read_cooked(accepted.startup_scene);
        if (!scene || scene.value.kind != foundation::AssetKind::kScene)
            return foundation::not_found("Startup scene is missing from asset package");
        const auto decoded_scene = asset::decode_scene_payload(scene.value.payload);
        if (!decoded_scene)
            return decoded_scene.result;
        for (const auto& reference : decoded_scene.value.references) {
            const auto referenced = package.value.read(reference.asset);
            if (!referenced)
                return referenced.result;
        }
        for (const auto id : accepted.startup_feature_graphs) {
            const auto pipeline = package.value.read_cooked(id);
            if (!pipeline || pipeline.value.kind != foundation::AssetKind::kFeatureGraph)
                return foundation::not_found("Startup FeatureGraph is missing from asset package");
        }
    }
    const auto rollback_initialize = [this](foundation::Result result) {
        (void)shutdown();
        return result;
    };
    impl_->package = std::make_unique<asset::AssetPackageReader>(std::move(package.value));
    impl_->pipelines = std::make_unique<pipeline::PipelineRegistry>();
    if (!impl_->clock_host) {
        const auto clock_host = feature::detail::make_clock_host_context(feature::detail::ClockHostLimits{});
        if (!clock_host) return rollback_initialize(clock_host.result);
        impl_->clock_host = clock_host.value;
    }
    impl_->feature_registry = std::make_unique<feature::FeatureRegistry>(impl_->clock_host);
    impl_->feature_runtime = std::make_unique<feature::FeatureRuntime>(
        *impl_->feature_registry,
        [state = impl_.get()](foundation::AssetId id) -> foundation::ResultValue<foundation::AssetKind> {
            if (!state->package)
                return {foundation::invalid_state("Feature asset package is not available"), {}};
            const auto cooked = state->package->read_cooked(id);
            return cooked ? foundation::ResultValue<foundation::AssetKind>{foundation::Result::success(), cooked.value.kind}
                          : foundation::ResultValue<foundation::AssetKind>{cooked.result, {}};
        }, impl_->feature_runtime_options);
    if (const auto result = impl_->feature_runtime->enable_async_execution(); !result)
        return rollback_initialize(result);
    impl_->feature_graph_host = std::make_unique<FeatureGraphRuntimeHost>(
        *impl_->feature_runtime, *impl_->feature_registry);
    impl_->reflection = std::make_unique<reflection::TypeRegistry>();
    impl_->passes = std::make_unique<pipeline::RenderPassRegistry>();
    impl_->modules = std::make_unique<module::ModuleRegistry>(
        *impl_->feature_registry, impl_->reflection.get());
    if (const foundation::Result builtins = renderer::register_builtin_rendering(*impl_->pipelines, *impl_->passes);
        !builtins) {
        return rollback_initialize(builtins);
    }
    if (definition) {
        if (const auto result = module::register_static_module(*impl_->modules, std::move(*definition)); !result)
            return rollback_initialize(result);
    } else {
        if (!cooked_manifest)
            return rollback_initialize(foundation::invalid_state("Dynamic Feature module requires a binary game manifest"));
        const auto module_manifest_result =
            resolve_cooked_module_manifest(manifest.parent_path(), cooked_manifest->module_manifest_name);
        if (!module_manifest_result)
            return rollback_initialize(module_manifest_result.result);
        const auto module_digest = file_digest(module_manifest_result.value);
        if (!module_digest || module_digest.value != cooked_manifest->module_manifest_sha256)
            return rollback_initialize(
                foundation::invalid_argument("Cooked Feature module manifest digest does not match game manifest"));
        const auto bundle = module::validate_module_bundle(module_manifest_result.value);
        if (!bundle) return rollback_initialize(bundle.result);
        if (bundle.value.metadata.module.id != module::ModuleId(cooked_manifest->module_id) ||
            bundle.value.metadata.build_id != cooked_manifest->build_id ||
            bundle.value.metadata.target != cooked_manifest->target ||
            bundle.value.metadata.configuration != cooked_manifest->configuration ||
            bundle.value.metadata.contract_digest != cooked_manifest->module_contract_digest)
            return rollback_initialize(
                foundation::invalid_argument("Cooked Feature module identity does not match game manifest"));
        impl_->dynamic_loader = std::make_unique<module::DynamicModuleLoader>(*impl_->modules);
        if (const auto result = impl_->dynamic_loader->load(bundle.value, impl_->dynamic_module); !result)
            return rollback_initialize(result);
        const auto loaded_definition = impl_->dynamic_loader->make_feature_module(impl_->dynamic_module);
        if (!loaded_definition) return rollback_initialize(loaded_definition.result);
        if (const auto result = module::register_static_module(*impl_->modules, std::move(loaded_definition.value)); !result)
            return rollback_initialize(result);
    }
    if (cooked_manifest) {
        for (const auto id : cooked_manifest->startup_feature_graphs) {
            const auto cooked = impl_->package->read_cooked(id);
            if (!cooked || cooked.value.kind != foundation::AssetKind::kFeatureGraph)
                return rollback_initialize(foundation::not_found("Startup FeatureGraph is missing from asset package"));
            if (const auto staged = impl_->feature_graph_host->stage_asset(cooked.value.payload); !staged)
                return rollback_initialize(staged);
        }
        if (!cooked_manifest->startup_feature_graphs.empty()) {
            if (const auto applied = impl_->feature_graph_host->apply_pending(); !applied)
                return rollback_initialize(applied);
        }
        impl_->startup_feature_graph_count = impl_->feature_graph_host->active_documents().size();
    }
    // The scheduler remains an internal Frame/World execution service. FeatureGraph
    // startup publication above owns the author-facing graph and does not create
    // a second Pipeline configuration container.
    impl_->scheduler = std::make_unique<pipeline::PipelineScheduler>();
    impl_->request = std::move(effective);
    impl_->identity = identity;
    impl_->callbacks = callbacks;
    impl_->game_context = {impl_.get(), &Impl::context_quit, &Impl::context_frame};
    impl_->mode = RuntimeMode::kPlaying;
    impl_->frame.fixed_delta_seconds = impl_->request.fixed_delta_seconds;
    impl_->asset_resolver = std::make_unique<RuntimeAssetResolver>(*impl_->package);
    if (impl_->request.frame_host != nullptr && impl_->request.frame_host->upload_context != nullptr) {
        if (const auto result = impl_->asset_resolver->set_upload_context(impl_->request.frame_host->upload_context);
            !result)
            return rollback_initialize(result);
    }
    if (impl_->startup_scene_id != foundation::AssetId{}) {
        // Older packages may carry an explicitly encoded, empty ScenePayload
        // as a placeholder. Preserve that package-only boot contract while
        // still rejecting malformed or unmarked payloads in the loader.
        bool legacy_empty_scene = false;
        if (const auto cooked_scene = impl_->package->read_cooked(impl_->startup_scene_id); cooked_scene) {
            if (const auto scene_payload = asset::decode_scene_payload(cooked_scene.value.payload); scene_payload)
                legacy_empty_scene = scene_payload.value.serialized_scene.empty();
        }
        auto loaded_scene = legacy_empty_scene
                                ? foundation::ResultValue<RuntimeSceneLoadResult>{foundation::Result::success(), {}}
                                : RuntimeSceneLoader::load_startup_scene(
                                      *impl_->package, impl_->startup_scene_id, *impl_->reflection,
                                      impl_->asset_resolver.get());
        if (!loaded_scene)
            return rollback_initialize(loaded_scene.result);
        impl_->startup_scene = std::make_unique<RuntimeSceneLoadResult>(std::move(loaded_scene.value));
    }
    if (const auto result = impl_->bind_world_commands(); !result)
        return rollback_initialize(result);
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
        if (const auto result = impl_->asset_resolver->set_upload_context(&impl_->owned_upload_context); !result)
            return rollback_initialize(result);
        impl_->request.window_system = impl_->owned_window_system.get();
        impl_->request.frame_host = &impl_->owned_frame_host;
#else
        return rollback_initialize(foundation::unsupported("owned runtime host is only available on Windows"));
#endif
    }
    if (impl_->request.frame_host != nullptr && impl_->request.frame_host->frame_context != nullptr &&
        impl_->request.frame_host->render_graph != nullptr) {
        impl_->pass_registry_host.reset();
        if (impl_->passes != nullptr)
            impl_->pass_registry_host = std::make_unique<engine::EngineFramePassRegistry>(*impl_->passes);
        impl_->pipeline_host = std::make_unique<engine::EngineFramePipelineHost>(
            *impl_->pipelines, *impl_->scheduler, impl_->pass_registry_host.get());
        impl_->orchestrator = std::make_unique<engine::EngineFrameOrchestrator>(
            *impl_->pipeline_host, *impl_->request.frame_host->frame_context,
            *impl_->request.frame_host->render_graph);
        impl_->render_history = std::make_unique<render_graph::RenderFeatureHistory>(
            impl_->request.frame_host->frame_context->device());
        if (impl_->startup_scene != nullptr && impl_->startup_scene->scope != nullptr) {
            if (const auto result = impl_->orchestrator->register_world(*impl_->startup_scene->scope); !result)
                return rollback_initialize(result);
            impl_->world_registered_with_orchestrator = true;
            if (impl_->request.frame_host->renderer != nullptr) {
                if (const auto result = impl_->orchestrator->register_world_frame(*impl_->startup_scene->scope, 0); !result)
                    return rollback_initialize(result);
                impl_->world_frame_registered_with_orchestrator = true;
            }
        }
        if (impl_->world_commands != nullptr) {
            if (const auto result = engine::detail::EngineFrameWorldCommands::attach(
                    *impl_->orchestrator, *impl_->world_commands); !result)
                return rollback_initialize(result);
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
    // A renderer attempt can be aborted while an observed World invocation waits.
    // Use its actual next frame identity; completed-frame budgeting remains separate.
    const auto* render_frame = impl_->orchestrator != nullptr && impl_->request.frame_host != nullptr
                                   ? impl_->request.frame_host->frame_context : nullptr;
    impl_->frame.frame_number = render_frame != nullptr ? render_frame->frame_number() + 1u : impl_->frames + 1u;
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
    // FeatureGraph execution is clock-driven.  The runtime owns the active
    // plans and advances each registered clock after game input/update have
    // produced the frame's immutable inputs, before any Render projection is
    // built.  A domain may later provide a finer-grained clock pump; this
    // boundary keeps Feature execution live in the current bootstrap path.
    if (impl_->world_commands != nullptr) {
        if (const auto result = impl_->world_commands->set_frame_delta(impl_->frame.delta_seconds); !result)
            return result;
    }
    if (impl_->feature_runtime != nullptr) {
        if (const auto result = impl_->feature_runtime->advance(impl_->frame.delta_seconds); !result)
            return result;
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
            frame_desc.present_acquired_frame = true;
            frame_desc.swapchain = impl_->owned_swapchain;
            frame_desc.swapchain_image_desc.extent = {frame_desc.render_output_extent.width,
                                                      frame_desc.render_output_extent.height, 1};
            frame_desc.swapchain_image_desc.format = impl_->owned_swapchain_info.format;
            frame_desc.swapchain_image_desc.capabilities = impl_->owned_swapchain_desc.image_capabilities;
            const auto depth_extent = frame_desc.render_output_extent;
            frame_desc.render_feature_preparation = [depth_extent](
                                                        const engine::EngineFramePreparationContext& context) {
                if (context.acquired_frame == nullptr || !context.swapchain_resource.is_valid())
                    return foundation::invalid_state("Runtime owned Present requires an acquired swapchain image");
                if (const auto color = context.bind_feature_texture(kRuntimeSceneColor, context.swapchain_resource);
                    !color)
                    return color;
                if (const auto depth = bind_owned_render_target(context, depth_extent); !depth)
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
        bool requires_render_scene = false;
        if (impl_->feature_graph_host != nullptr && impl_->feature_graph_host->active() != nullptr) {
            const auto captured = impl_->feature_runtime->capture_render_outputs(*impl_->feature_graph_host->active());
            if (!captured) return captured.result;
            bool requires_shadow_depth = false;
            bool requires_lighting = false;
            std::array<bool, 3> requires_gbuffer{};
            const auto uses_shadow_depth = [](const feature::RenderTextureReference& target) {
                return target.external && target.id.value() == kRuntimeShadowDepth.value();
            };
            for (const auto& output : captured.value) {
                requires_shadow_depth = requires_shadow_depth ||
                    (output.intent.depth_output && uses_shadow_depth(*output.intent.depth_output));
                for (const auto& operation : output.intent.operations) {
                    requires_render_scene = requires_render_scene || std::holds_alternative<feature::RenderDrawScene>(operation) ||
                        std::holds_alternative<feature::RenderDrawDepthScene>(operation) ||
                        std::holds_alternative<feature::RenderDrawGBufferScene>(operation) ||
                        std::holds_alternative<feature::RenderDeferredLightingScene>(operation) ||
                        std::holds_alternative<feature::RenderPostProcessScene>(operation) ||
                        std::holds_alternative<feature::RenderLightCullingScene>(operation) ||
                        std::holds_alternative<feature::RenderDrawClusteredScene>(operation);
                    if (const auto* depth = std::get_if<feature::RenderDrawDepthScene>(&operation))
                        requires_shadow_depth = requires_shadow_depth ||
                            depth->projection == feature::RenderDepthSceneProjection::kDirectionalShadow;
                    if (const auto* draw = std::get_if<feature::RenderDrawScene>(&operation); draw && draw->depth)
                        requires_shadow_depth = requires_shadow_depth || uses_shadow_depth(draw->depth->target);
                    if (const auto* draw = std::get_if<feature::RenderDrawScene>(&operation); draw && draw->directional_shadow)
                        requires_shadow_depth = true;
                    if (const auto* draw = std::get_if<feature::RenderDrawGBufferScene>(&operation)) {
                        requires_shadow_depth = requires_shadow_depth || uses_shadow_depth(draw->depth.target);
                        for (const auto& target : {draw->albedo.target, draw->normal.target, draw->material.target})
                            for (size_t index = 0; index < requires_gbuffer.size(); ++index)
                                requires_gbuffer[index] = requires_gbuffer[index] ||
                                    (target.external && target.id.value() == kRuntimeGBufferTargets[index].value());
                    }
                    if (const auto* draw = std::get_if<feature::RenderDeferredLightingScene>(&operation)) {
                        requires_lighting = requires_lighting ||
                            (draw->hdr.target.external && draw->hdr.target.id.value() == kRuntimeSceneLighting.value());
                        requires_shadow_depth = requires_shadow_depth || draw->directional_shadow.has_value();
                        for (const auto& target : {draw->albedo, draw->normal})
                            for (size_t index = 0; index < requires_gbuffer.size(); ++index)
                                requires_gbuffer[index] = requires_gbuffer[index] ||
                                    (target.external && target.id.value() == kRuntimeGBufferTargets[index].value());
                    }
                    if (const auto* cull = std::get_if<feature::RenderLightCullingScene>(&operation))
                        requires_shadow_depth = requires_shadow_depth || cull->directional_shadow.has_value();
                    if (const auto* draw = std::get_if<feature::RenderDrawClusteredScene>(&operation)) {
                        requires_lighting = requires_lighting ||
                            (draw->hdr.target.external && draw->hdr.target.id.value() == kRuntimeSceneLighting.value());
                        requires_shadow_depth = requires_shadow_depth || draw->directional_shadow.has_value();
                    }
                    if (const auto* draw = std::get_if<feature::RenderPostProcessScene>(&operation))
                        requires_lighting = requires_lighting ||
                            (draw->hdr.external && draw->hdr.id.value() == kRuntimeSceneLighting.value());
                }
            }
            if (impl_->owns_host && (requires_shadow_depth || requires_lighting || std::any_of(requires_gbuffer.begin(), requires_gbuffer.end(),
                [](bool required) { return required; }))) {
                auto prepare = std::move(frame_desc.render_feature_preparation);
                const auto depth_extent = frame_desc.render_output_extent;
                frame_desc.render_feature_preparation = [prepare = std::move(prepare), depth_extent, requires_shadow_depth, requires_gbuffer, requires_lighting]
                    (const engine::EngineFramePreparationContext& context) {
                    if (prepare) if (const auto result = prepare(context); !result) return result;
                    if (requires_shadow_depth) {
                        bool bound_shadow = false;
                        for (std::size_t index = 0; index < context.render_scenes.size(); ++index) {
                            const auto& scene = context.render_scenes[index];
                            if (!scene)
                                continue;
                            const auto extent = scene->view.shadow_extent.is_valid() ? scene->view.shadow_extent
                                                                                      : scene->view.output_extent;
                            const auto target = index == 0
                                                    ? kRuntimeShadowDepth
                                                    : renderer::make_view_scoped_render_resource_id(
                                                          kRuntimeShadowDepth, scene->view.id);
                            if (const auto result = bind_owned_render_target(context, extent, target); !result)
                                return result;
                            bound_shadow = true;
                        }
                        if (!bound_shadow)
                            if (const auto result = bind_owned_render_target(context, depth_extent, kRuntimeShadowDepth); !result)
                                return result;
                    }
                    if (requires_lighting)
                        if (const auto result = bind_owned_render_target(context, depth_extent, kRuntimeSceneLighting,
                            renderer::kSceneLightingFormat, rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kSampled);
                            !result) return result;
                    for (size_t index = 0; index < requires_gbuffer.size(); ++index) {
                        if (!requires_gbuffer[index]) continue;
                        const auto format = index < 2 ? rhi::Format::kRgba16Float : rhi::Format::kRgba8Unorm;
                        if (const auto result = bind_owned_render_target(context, depth_extent, kRuntimeGBufferTargets[index], format,
                            rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kSampled); !result) return result;
                    }
                    return foundation::Result::success();
                };
            }
            const auto captured_owners = impl_->feature_runtime->capture_render_resource_owners(*impl_->feature_graph_host->active());
            if (!captured_owners) return captured_owners.result;
            {
                auto outputs = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(captured.value);
                auto owners = std::make_shared<const std::vector<feature::RenderResourceOwner>>(captured_owners.value);
                auto post_build = std::move(frame_desc.render_feature_post_build);
                frame_desc.render_feature_post_build = [outputs = std::move(outputs), post_build = std::move(post_build),
                                                        owners = std::move(owners), history = impl_->render_history.get(),
                                                        pass_host = render_graph::RenderPassExecutionHost{
                                                            impl_->passes.get(), pipeline::detail::RenderPassRegistryExecution::
                                                                execution_epoch(*impl_->passes)}]
                    (const engine::EngineFramePostBuildContext& context) {
                    if (!history) return foundation::invalid_state("Render Feature history store is unavailable");
                    if (context.render_scenes.empty()) {
                        if (const auto result = renderer::build_render_feature_outputs(
                                context.render_graph, context.frame_context, outputs, *owners,
                                context.feature_textures, {}, *history, 0, pass_host); !result) return result;
                    } else {
                        for (std::size_t index = 0; index < context.render_scenes.size(); ++index) {
                            const auto& scene = context.render_scenes[index];
                            if (!scene)
                                continue;
                            const bool canonical = index == 0;
                            const auto scene_id = canonical
                                                      ? renderer::kRuntimeSceneResourceId
                                                      : renderer::make_view_scoped_render_resource_id(
                                                            renderer::kRuntimeSceneResourceId, scene->view.id);
                            std::vector<renderer::RenderFeatureSceneBinding> scenes{{
                                scene_id, 1, {}, scene, context.render_resource_resolver}};
                            std::vector<render_graph::RenderGraphFeatureTextureBinding> scoped_textures(
                                context.feature_textures.begin(), context.feature_textures.end());
                            if (!canonical) {
                                for (const auto& binding : context.feature_textures) {
                                    const auto alias = renderer::make_view_scoped_render_resource_id(binding.id, scene->view.id);
                                    const bool exists = std::any_of(scoped_textures.begin(), scoped_textures.end(),
                                        [&](const auto& candidate) { return candidate.id == alias; });
                                    if (!exists)
                                        scoped_textures.push_back({alias, binding.texture, binding.version, binding.owner});
                                }
                            }
                            std::vector<feature::RenderFeatureOutput> scoped_outputs;
                            scoped_outputs.reserve(outputs->size());
                            for (const auto& output : *outputs)
                                scoped_outputs.push_back(renderer::scope_render_feature_output(output, scene->view.id, canonical));
                            auto scoped_snapshot = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(
                                std::move(scoped_outputs));
                            if (const auto result = renderer::build_render_feature_outputs(
                                    context.render_graph, context.frame_context, std::move(scoped_snapshot), *owners,
                                    scoped_textures, scenes, *history, scene->view.id.value(), pass_host); !result)
                                return result;
                        }
                    }
                    return post_build ? post_build(context) : foundation::Result::success();
                };
            }
        }
        // A pure FeatureGraph clear/copy/history frame needs no camera or scene resources.
        if (!requires_render_scene) {
            frame_desc.renderer = nullptr;
            frame_desc.render_view_resolver = {};
            frame_desc.render_views = {};
        }
        if (const auto result = impl_->orchestrator->run_frame(frame_desc); !result) {
            if (result.code == foundation::ResultCode::kNotReady && impl_->world_commands != nullptr &&
                impl_->world_commands->observation_waiting() && !impl_->orchestrator->is_frame_active())
                return foundation::Result::success();
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
            if (impl_->world_commands != nullptr) {
                if (const auto result = impl_->world_commands->consume(scope); !result) {
                    if (result.code == foundation::ResultCode::kNotReady && impl_->world_commands->observation_waiting())
                        return foundation::Result::success();
                    return result;
                }
            }
            if (const auto committed = scope.commit(impl_->frame.frame_number, impl_->frame.fixed_tick); !committed)
                return committed.result;
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
    // Join Feature work before game stop/context, world, or module teardown.
    if (impl_->feature_runtime) {
        if (const auto result = impl_->feature_runtime->shutdown_execution(); !result) {
            impl_->shutdown_started = false;
            return result;
        }
    }
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
    if (impl_->render_history) {
        // A history-only borrowed host may have no resolver GPU resources to
        // trigger its drain. Reuse FrameContext's idempotent retirement barrier.
        auto* frame_context = impl_->request.frame_host->frame_context;
        if (const auto result = frame_context->drain(); !result) {
            impl_->shutdown_started = false;
            return result;
        }
        impl_->render_history.reset();
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
    // The orchestrator borrows the fixed consumer, whose routes refer to this Scene generation.
    impl_->orchestrator.reset();
    impl_->world_commands.reset();
    if (impl_->startup_scene != nullptr && impl_->startup_scene->scope != nullptr) {
        if (const auto result = impl_->startup_scene->scope->shutdown(); !result && first_error)
            first_error = result;
    }
    impl_->startup_scene.reset();
    impl_->world_frame_registered_with_orchestrator = false;
    impl_->pass_registry_host.reset();
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
    if (impl_->modules && impl_->dynamic_loader && impl_->dynamic_module.loaded) {
        const auto module_id = impl_->dynamic_module.bundle.metadata.module.id;
        if (impl_->modules->contains(module_id)) {
            if (const auto result = impl_->modules->unregister_module(module_id); !result && first_error)
                first_error = result;
        }
        if (const auto result = impl_->dynamic_loader->unload(impl_->dynamic_module); !result && first_error)
            first_error = result;
    }
    impl_->dynamic_loader.reset();
    impl_->modules.reset();
    impl_->feature_graph_host.reset();
    impl_->feature_runtime.reset();
    impl_->feature_registry.reset();
    impl_->passes.reset();
    impl_->reflection.reset();
    impl_->pipelines.reset();
    impl_->startup_feature_graph_count = 0;
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

std::size_t RuntimeBootstrap::startup_feature_graph_count() const noexcept {
    return impl_ ? impl_->startup_feature_graph_count : 0;
}

world::WorldScope* RuntimeBootstrap::startup_world_scope() const noexcept {
    return impl_ && impl_->startup_scene ? impl_->startup_scene->scope.get() : nullptr;
}

const RuntimeFrameHostResources* RuntimeBootstrap::frame_host_resources() const noexcept {
    return impl_ && impl_->initialized ? impl_->request.frame_host : nullptr;
}

FeatureGraphRuntimeHost* RuntimeFeatureControl::host(const RuntimeBootstrap& bootstrap) noexcept {
    return bootstrap.impl_ && bootstrap.impl_->initialized ? bootstrap.impl_->feature_graph_host.get() : nullptr;
}

foundation::Result RuntimeFeatureControl::set_transport_time_source(
    RuntimeBootstrap& bootstrap, std::function<uint64_t()> now_ns) {
    if (!bootstrap.impl_ || bootstrap.impl_->initialized || bootstrap.impl_->feature_runtime != nullptr ||
        bootstrap.impl_->mode != RuntimeMode::kStopped)
        return foundation::invalid_state("transport clock configuration requires a stopped Runtime");
    bootstrap.impl_->feature_runtime_options.now_ns = std::move(now_ns);
    return foundation::Result::success();
}

foundation::ResultValue<FeatureRuntimeNodeSnapshot> RuntimeFeatureControl::inspect_node(
    const RuntimeBootstrap& bootstrap, std::string_view asset_uuid, feature::FeatureNodeId node_id) {
    auto* active = host(bootstrap);
    return active ? active->inspect_node(asset_uuid, node_id) :
        foundation::ResultValue<FeatureRuntimeNodeSnapshot>{foundation::not_ready("Runtime Feature host is stopped"), {}};
}

foundation::Result RuntimeFeatureControl::control_process(RuntimeBootstrap& bootstrap, const RuntimeProcessCommand& command) {
    auto* active = host(bootstrap);
    return active ? active->control_process(command) : foundation::not_ready("Runtime Feature host is stopped");
}

foundation::ResultValue<std::vector<feature::RenderFeatureOutput>> RuntimeFeatureControl::capture_render_outputs(
    const RuntimeBootstrap& bootstrap) {
    auto* active = host(bootstrap);
    return active ? active->capture_render_outputs() :
        foundation::ResultValue<std::vector<feature::RenderFeatureOutput>>{foundation::not_ready("Runtime Feature host is stopped"), {}};
}

foundation::Result RuntimeFeatureControl::render_publication_status(const RuntimeBootstrap& bootstrap, feature::ClockId clock) {
    auto* active = host(bootstrap);
    return active ? active->render_publication_status(clock) : foundation::not_ready("Runtime Feature host is stopped");
}

int run_game(int argc, char** argv, std::optional<module::FeatureModuleDefinition> definition, const RuntimeBuildIdentity& identity,
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
    while (bootstrap.frame_count() < frames) {
        const auto completed = bootstrap.frame_count();
        if (const auto result = bootstrap.tick(); !result)
            return report_failure(result);
        if (bootstrap.last_frame_context().quit_requested)
            break;
        if (bootstrap.frame_count() == completed)
            static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    if (const auto result = bootstrap.shutdown(); !result)
        return report_failure(result);
    return 0;
}

}  // namespace aeg::application::runtime


namespace aeg::application::runtime {
int run_game(int argc, char** argv, const RuntimeBuildIdentity& identity, GameConfigurationCallback configure) {
    return run_game(argc, argv, std::nullopt, identity, configure);
}
}  // namespace aeg::application::runtime
