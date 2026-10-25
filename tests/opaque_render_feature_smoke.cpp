/**
 * @file opaque_render_feature_smoke.cpp
 * @brief Executes the Runtime MeshRenderer-to-opaque-draw path and verifies its pixels.
 */
#include "feature_clock_test_fixture.hpp"
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Components/component_registration.hpp"
#include "Components/component_render_extraction.hpp"
#include "deferred_lighting_resources.hpp"
#include "engine_frame_pass_registry.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_orchestrator.hpp"
#include "light_culling.hpp"
#include "clustered_forward.hpp"
#include "material.hpp"
#include "pipeline.hpp"
#include "post_process.hpp"
#include "render_graph.hpp"

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "../Engine/Pipeline/private/render_pass_registry_execution.hpp"
#include "../Engine/Shader/private/shader_pipeline_layout_access.hpp"
#include "renderer.hpp"
#include "renderer_variants_internal.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "shader_contract.hpp"
#include "texture_uploader.hpp"
#include "world_scope.hpp"
#include "feature_render_test_fixture.hpp"
#include "render_scene_provenance_fixture.hpp"
#include "render_feature_draw.hpp"
#include "renderer_feature_profiles.hpp"
#include "native_scene_pass_source.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"
#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "material_payload.hpp"
#include <cstring>
#include <filesystem>

namespace {
struct RegisteredSceneObservation final {
    struct PreparedView final {
        aeg::render_graph::RenderSceneOperationSource source{};
        std::weak_ptr<const aeg::pipeline::RenderPassRuntimeData> data{};
    };
    uint32_t setups{0}, executions{0};
    uint32_t view_setup_swaps{0}, view_execute_swaps{0}, actual_view_draws{0};
    bool actual_geometry_work{false};
    std::vector<aeg::pipeline::RenderPassConfiguration> setup_configurations{}, execute_configurations{};
    std::vector<aeg::render_graph::RenderSceneOperationSource> setup_sources{}, execute_sources{};
    std::weak_ptr<const aeg::pipeline::RenderPassRuntimeData> prepared{};
    std::weak_ptr<const aeg::pipeline::RenderPass> native_delegate{};
    std::vector<PreparedView> prepared_views{};
};

class ObservedScenePass final : public aeg::pipeline::RenderPass {
public:
    ObservedScenePass(std::shared_ptr<const aeg::pipeline::RenderPass> delegate, RegisteredSceneObservation& observation)
        : delegate_(std::move(delegate)), observation_(observation) {}
    const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override { return delegate_->descriptor(); }
    aeg::foundation::Result setup(aeg::pipeline::RenderPassBuilder& builder,
        const aeg::pipeline::RenderPassConfiguration& configuration) const override {
        ++observation_.setups;
        const auto source = aeg::renderer::detail::NativeScenePassInspection::source(configuration);
        if (!source) return source.result;
        observation_.setup_sources.push_back(source.value);
        observation_.prepared = configuration.runtime_data;
        auto wrong_instance = configuration;
        wrong_instance.instance = aeg::pipeline::RenderPassInstanceId(configuration.instance.value() + 1);
        auto wrong_source = configuration;
        wrong_source.source += "/foreign";
        auto unprepared = configuration;
        unprepared.runtime_data.reset();
        if (delegate_->setup(builder, wrong_instance) || delegate_->setup(builder, wrong_source) ||
            delegate_->setup(builder, unprepared))
            return aeg::foundation::invalid_state("native Scene setup accepted a replaced prepared identity");
        if (source.value.view_key != 0) {
            for (const auto& previous : observation_.prepared_views) {
                if (previous.source.view_key == source.value.view_key) continue;
                auto swapped = configuration;
                swapped.runtime_data = previous.data.lock();
                if (!swapped.runtime_data)
                    return aeg::foundation::invalid_state("first View prepared commands retired before second View setup");
                const auto rejected = delegate_->setup(builder, swapped);
                if (rejected || rejected.code != aeg::foundation::ResultCode::kInvalidArgument)
                    return aeg::foundation::invalid_state("native Scene setup accepted another actual View's prepared commands");
                ++observation_.view_setup_swaps;
            }
            observation_.prepared_views.push_back({source.value, configuration.runtime_data});
        }
        auto recorded = configuration;
        recorded.runtime_data.reset();
        observation_.setup_configurations.push_back(std::move(recorded));
        return delegate_->setup(builder, configuration);
    }
    aeg::foundation::Result execute(aeg::pipeline::RenderPassExecutionContext& command,
        const aeg::pipeline::RenderPassConfiguration& configuration) const override {
        ++observation_.executions;
        const auto source = aeg::renderer::detail::NativeScenePassInspection::source(configuration);
        if (!source) return source.result;
        observation_.execute_sources.push_back(source.value);
        auto wrong_instance = configuration;
        wrong_instance.instance = aeg::pipeline::RenderPassInstanceId(configuration.instance.value() + 1);
        auto wrong_source = configuration;
        wrong_source.source += "/foreign";
        if (delegate_->execute(command, wrong_instance) || delegate_->execute(command, wrong_source))
            return aeg::foundation::invalid_state("native Scene execution accepted a replaced prepared identity");
        if (source.value.view_key != 0) {
            for (const auto& previous : observation_.prepared_views) {
                if (previous.source.view_key == source.value.view_key) continue;
                auto swapped = configuration;
                swapped.runtime_data = previous.data.lock();
                if (!swapped.runtime_data)
                    return aeg::foundation::invalid_state("other View prepared commands retired before native execution");
                const auto rejected = delegate_->execute(command, swapped);
                if (rejected || rejected.code != aeg::foundation::ResultCode::kInvalidArgument)
                    return aeg::foundation::invalid_state("native Scene execution accepted another actual View's prepared commands");
                ++observation_.view_execute_swaps;
            }
        }
        auto recorded = configuration;
        recorded.runtime_data.reset();
        observation_.execute_configurations.push_back(std::move(recorded));
        return delegate_->execute(command, configuration);
    }
private:
    std::shared_ptr<const aeg::pipeline::RenderPass> delegate_;
    RegisteredSceneObservation& observation_;
};

aeg::renderer::RenderFeatureDrawLowerer make_scene_lowerer(
    std::span<const aeg::renderer::RenderFeatureSceneBinding> scenes,
    aeg::render_graph::RenderPassExecutionHost host, bool registered) {
    return registered ? aeg::renderer::RenderFeatureDrawLowerer(scenes, host)
                      : aeg::renderer::RenderFeatureDrawLowerer(scenes);
}

using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::RenderGraphBuffer;
using aeg::pipeline::RenderGraphTexture;
using aeg::pipeline::RenderResourceId;
using aeg::render_graph::FrameAllocation;
using aeg::rhi::BindingLayoutHandle;
using aeg::rhi::BindingResource;
using aeg::rhi::BindingSetDesc;
using aeg::rhi::BindingSetHandle;
using aeg::rhi::BindingSetLifetime;
using aeg::rhi::BindingType;
using aeg::rhi::BindingWrite;
using aeg::rhi::BufferCapability;
using aeg::rhi::BufferDesc;
using aeg::rhi::BufferHandle;
using aeg::rhi::BufferRange;
using aeg::rhi::CullMode;
using aeg::rhi::DebugName;
using aeg::rhi::Device;
using aeg::rhi::DeviceCreateDesc;
using aeg::rhi::Extent3D;
using aeg::rhi::Format;
using aeg::rhi::GraphicsPipelineState;
using aeg::rhi::MemoryClass;
using aeg::rhi::Offset3D;
using aeg::rhi::PipelineHandle;
using aeg::rhi::PipelineStage;
using aeg::rhi::PipelineType;
using aeg::rhi::PrimitiveTopology;
using aeg::rhi::QueueClass;
using aeg::rhi::ResourceState;
using aeg::rhi::ResourceUsage;
using aeg::rhi::ShaderStage;
using aeg::rhi::TextureCapability;
using aeg::rhi::TextureDesc;
using aeg::rhi::TextureHandle;
using aeg::rhi::TextureSubresourceLayers;
using aeg::rhi::TextureViewDimension;
using aeg::rhi::TextureViewHandle;
using aeg::rhi::TextureViewKind;
using aeg::rhi::VertexAttributeDesc;
using aeg::rhi::VertexBindingDesc;
using aeg::rhi::VertexInputRate;

/** @brief Logical target identity shared by the Scene draw and technical readback pass. */
inline constexpr RenderResourceId kOpaqueSceneColor{"OpaqueSceneColor"};
/** @brief Render output resolution used for the offscreen verification target. */
constexpr uint32_t kTargetWidth = 64;
/** @brief Render output resolution used for the offscreen verification target. */
constexpr uint32_t kTargetHeight = 64;
/** @brief Number of bytes copied from the RGBA8 offscreen target. */
constexpr std::size_t kReadbackSize = static_cast<std::size_t>(kTargetWidth) * kTargetHeight * 4u;

/** @brief Vertex layout reflected by unlit_mesh.vert.hlsl. */
struct Vertex final {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};
    float u{0.0f};
    float v{0.0f};
    float nx{0.0f};
    float ny{0.0f};
    float nz{1.0f};
};

/** @brief The clip-space triangle rendered by the built-in opaque feature. */
constexpr std::array<Vertex, 3> kTriangleVertices{{
    {-0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {0.75f, -0.75f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {0.0f, 0.75f, 0.0f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f},
}};
/** @brief Indexed topology consumed by the opaque RenderFeature. */
constexpr std::array<uint16_t, 3> kTriangleIndices{{0, 1, 2}};

/** @brief Reports a failed contract assertion while preserving a compact test body. */
template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

/** @brief Returns the host-readable state required by an imported frame readback buffer. */
[[nodiscard]] ResourceState make_host_read_state() {
    return ResourceState{ResourceUsage::kHostRead, ShaderStage::kAll, QueueClass::kCopy, false, PipelineStage::kHost};
}

/** @brief Builds the complete capability contract for a FrameContext readback allocation. */
[[nodiscard]] BufferDesc make_readback_desc(uint64_t size) {
    BufferDesc desc{};
    desc.size = size;
    desc.capabilities = BufferCapability::kTransferSource | BufferCapability::kTransferDestination |
                        BufferCapability::kVertex | BufferCapability::kIndex | BufferCapability::kUniform;
    desc.memory = MemoryClass::kReadback;
    desc.initial_usage = ResourceUsage::kHostRead;
    desc.initial_stages = PipelineStage::kHost;
    desc.initial_queue = QueueClass::kCopy;
    desc.name = DebugName{"Renderer.OpaqueSmoke.Readback"};
    return desc;
}

/** @brief Stores frame-local RenderGraph resources shared with the technical readback pass. */
struct OpaqueSmokeState final {
    FrameAllocation readback{};
    FrameAllocation second_view_readback{};
    RenderGraphBuffer readback_resource{};
    bool invalid_texture_binding_rejected{false};
    bool color_target_bound{false};
    bool render_view_received_output_extent{false};
    std::vector<uint32_t> resolved_view_ids{};
    uint32_t readback_pass_build_count{0};
    std::vector<aeg::render_graph::RenderGraphFeatureTextureBinding> scene_textures{};
    aeg::resource::GpuTexture material_texture{};
    aeg::resource::GpuTexture replacement_texture{};
    aeg::resource::TextureMaterial material{};
    BufferHandle material_parameter_buffer{};
    BufferDesc material_parameter_buffer_desc{};
    RenderGraphBuffer material_parameter_resource{};
    BufferHandle lighting_buffer{};
    BindingSetHandle lighting_binding_set{};
};

/** @brief Binds the real frame targets consumed by the built-in Scene Feature profiles. */
Result bind_scene_targets(aeg::render_graph::RenderGraph& graph,
    aeg::render_graph::RenderGraphTexture color, aeg::render_graph::RenderGraphTexture depth,
    const TextureDesc& output, bool forward, bool deferred,
    std::vector<aeg::render_graph::RenderGraphFeatureTextureBinding>& textures) {
    textures = {{RenderResourceId{"Renderer.SceneColor"}, color},
        {RenderResourceId{"Renderer.SceneDepth"}, depth}};
    const auto bind_target = [&](const char* id, Format format, TextureCapability capabilities) {
        auto description = output;
        description.format = format;
        description.capabilities = capabilities;
        description.name = DebugName{id};
        const auto texture = graph.create_texture(description);
        if (!texture) return texture.result;
        textures.push_back({RenderResourceId{id}, texture.value});
        return Result::success();
    };
    if (forward || deferred) {
        if (const auto result = bind_target("Renderer.ShadowDepth", Format::kD32Float,
                TextureCapability::kDepthStencil | TextureCapability::kSampled); !result) return result;
        if (const auto result = bind_target("Renderer.SceneLighting", aeg::renderer::kSceneLightingFormat,
                TextureCapability::kColorAttachment | TextureCapability::kSampled); !result) return result;
    }
    if (deferred) {
        for (const auto name : {"Renderer.GBuffer.Albedo", "Renderer.GBuffer.Normal", "Renderer.GBuffer.Material"}) {
            const auto format = std::string_view(name) == "Renderer.GBuffer.Material"
                ? Format::kRgba8Unorm : Format::kRgba16Float;
            if (const auto result = bind_target(name, format,
                    TextureCapability::kColorAttachment | TextureCapability::kSampled); !result) return result;
        }
    }
    return Result::success();
}

/** @brief Resolves the fixed test mesh, material pipeline, object constants, and frame lighting. */
class OpaqueSmokeResolver final : public aeg::renderer::RenderResourceResolver {
public:
    aeg::application::runtime::RuntimeAssetResolver* cold_assets{};
    aeg::renderer::MeshAssetId cold_mesh{};
    aeg::renderer::MaterialInstanceId cold_material{};
    OpaqueSmokeResolver(Device& device, aeg::renderer::MeshAssetId mesh_id,
                        aeg::renderer::MaterialInstanceId material_id, aeg::renderer::RenderMeshResource mesh,
                        aeg::renderer::RenderMaterialResource material, aeg::renderer::RenderObjectResource object)
        : device_(device),
          mesh_id_(mesh_id),
          material_id_(material_id),
          mesh_(mesh),
          material_(material),
          object_(object) {}

    void set_material(aeg::renderer::RenderMaterialResource material) const noexcept { material_ = material; }
    void set_lighting(aeg::renderer::RenderLightingResource lighting) const noexcept { lighting_ = lighting; }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMeshResource> resolve_mesh(
        aeg::renderer::MeshAssetId mesh) const override {
        if (mesh != mesh_id_) {
            return {aeg::foundation::Result::failure(ResultCode::kNotFound, "opaque smoke mesh id is unknown"), {}};
        }
        ++mesh_resolve_count;
        if (cold_assets) return cold_assets->resolve_mesh(cold_mesh);
        return {Result::success(), mesh_};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        aeg::renderer::MaterialInstanceId material) const override {
        if (material != material_id_) {
            return {aeg::foundation::Result::failure(ResultCode::kNotFound, "opaque smoke material id is unknown"), {}};
        }
        ++material_resolve_count;
        if (cold_assets) return cold_assets->resolve_material(cold_material);
        return {Result::success(), material_};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderObjectResource> resolve_object(
        const aeg::renderer::RenderInstance& instance, const aeg::renderer::RenderView& view) const override {
        if (instance.mesh != mesh_id_ || instance.material != material_id_ || !view.is_valid()) {
            return {aeg::foundation::Result::failure(ResultCode::kInvalidArgument,
                                                     "opaque smoke object resolution received invalid draw inputs"),
                    {}};
        }

        const aeg::renderer::RenderObjectConstants constants{view.camera.view_projection() * instance.world_transform,
                                                             aeg::math::identity()};
        const Result upload_result = device_.write_buffer(object_.transform_buffer, object_.transform_offset,
                                                          std::as_bytes(std::span{&constants, 1}));
        if (!upload_result) {
            return {upload_result, {}};
        }
        ++object_resolve_count;
        return {Result::success(), object_};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderLightingResource> resolve_lighting(
        const aeg::renderer::RenderSceneSnapshot&, const aeg::renderer::RenderView&) const override {
        return {Result::success(), lighting_};
    }

    mutable uint32_t mesh_resolve_count{0};
    aeg::renderer::DeferredLightingResources* deferred_resources{};
    aeg::renderer::LightCullingResources* culling_resources{};
    aeg::renderer::PostProcessResources* post_resources{};
    aeg::render_graph::FrameContext* deferred_frame{};
    mutable aeg::renderer::RenderDeferredLightingResource last_deferred_binding{};
    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderDeferredLightingResource> resolve_deferred_lighting(
        const aeg::renderer::RenderView&) const override {
        if (!deferred_resources || !deferred_frame)
            return {aeg::foundation::not_found("Deferred test resources not configured"), {}};
        const auto resource = deferred_resources->for_frame(*deferred_frame);
        if (resource)
            last_deferred_binding = resource.value;
        return resource;
    }
    mutable uint32_t material_resolve_count{0};
    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderPostProcessResource> resolve_post_process(
        const aeg::renderer::RenderView& view) const override {
        if (!post_resources || !deferred_frame)
            return {aeg::foundation::not_found("Postprocess test resources not configured"), {}};
        return post_resources->for_frame(*deferred_frame, view);
    }
    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderLightCullingResource> resolve_light_culling(
        const aeg::renderer::RenderSceneSnapshot& scene, const aeg::renderer::RenderShadowView& shadow) const override {
        if (!culling_resources || !deferred_frame)
            return {aeg::foundation::not_found("Forward test resources not configured"), {}};
        return culling_resources->for_frame(*deferred_frame, scene, shadow);
    }
    mutable uint32_t object_resolve_count{0};

private:
    Device& device_;
    aeg::renderer::MeshAssetId mesh_id_{};
    aeg::renderer::MaterialInstanceId material_id_{};
    aeg::renderer::RenderMeshResource mesh_{};
    mutable aeg::renderer::RenderMaterialResource material_{};
    aeg::renderer::RenderObjectResource object_{};
    mutable aeg::renderer::RenderLightingResource lighting_{};
};
}  // namespace

int main(int argc, char** argv) {
    static_assert(sizeof(Vertex) == sizeof(float) * 9u);
    const bool neutral_clear = argc == 2 && std::string_view(argv[1]) == "--neutral-clear";
    const bool cold = argc == 2 && std::string_view(argv[1]) == "--neutral-cold";
    const bool cold_retry = argc == 2 && std::string_view(argv[1]) == "--neutral-retry";
    const bool neutral_registered = argc == 2 && std::string_view(argv[1]) == "--neutral-registered";
    const bool neutral = neutral_registered || neutral_clear || cold || cold_retry ||
        (argc == 2 && std::string_view(argv[1]) == "--neutral");
    const bool forward = argc == 2 && std::string_view(argv[1]) == "--forward";
    const bool backlit = argc == 2 && std::string_view(argv[1]) == "--deferred-backlit";
    const bool deferred = backlit || (argc == 2 && std::string_view(argv[1]) == "--deferred");
    constexpr uint32_t kViewId = 3;
    constexpr uint32_t kSecondViewId = 4;
    const aeg::renderer::MeshAssetId mesh_id{0x1111ull};
    const aeg::renderer::MaterialInstanceId material_id{0x2222ull};

    PipelineRegistry registry;
    aeg::pipeline::RenderPassRegistry passes;
    if (!aeg::renderer::register_builtin_render_passes(passes))
        return 2;
    RegisteredSceneObservation registered_scene;
    const auto observed_type = deferred ? aeg::renderer::kGBufferPassTypeId : aeg::renderer::kForwardOpaquePassTypeId;
    if (neutral_registered || !neutral) {
        const auto real = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
            passes, observed_type);
        if (!real || !passes.unregister_pass(observed_type) ||
            !passes.register_pass([delegate = real.value, &registered_scene] {
                return std::make_unique<ObservedScenePass>(delegate, registered_scene);
            })) return 60;
        registered_scene.native_delegate = real.value;
    }
    const aeg::render_graph::RenderPassExecutionHost scene_pass_host{
        &passes, aeg::pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)};
    aeg::ecs::WorldManager world_manager;
    const auto world_handle = world_manager.create_world({"OpaqueRenderFeatureSmokeWorld", true});
    if (!require(world_handle, "opaque smoke World creation failed")) {
        return 2;
    }
    const auto world_result = world_manager.find(world_handle.value);
    if (!require(world_result, "opaque smoke World lookup failed") ||
        !require(aeg::engine::components::register_render_components(*world_result.value),
                 "opaque smoke Runtime component registration failed")) {
        return 3;
    }

    const auto mesh_entity = world_result.value->create_entity();
    aeg::engine::components::MeshRenderer mesh_component{};
    mesh_component.mesh_asset_id = mesh_id.value();
    mesh_component.material_asset_id = material_id.value();
    mesh_component.bounds_radius = 1.0f;
    if (!require(mesh_entity, "opaque smoke mesh entity creation failed") ||
        !require(world_result.value->emplace<aeg::engine::components::Transform>(mesh_entity.value,
                                                                                 aeg::engine::components::Transform{}),
                 "opaque smoke mesh Transform insertion failed") ||
        !require(world_result.value->emplace<aeg::engine::components::MeshRenderer>(mesh_entity.value, mesh_component),
                 "opaque smoke MeshRenderer insertion failed")) {
        return 4;
    }

    aeg::world::WorldScope world_scope(world_result.value);
    if (!require(world_scope.initialize(), "opaque smoke WorldScope initialization failed")) {
        return 5;
    }
    aeg::renderer::Renderer renderer;
    const auto extractor = aeg::engine::components::register_mesh_renderer_extractor(renderer);
    if (!require(extractor, "opaque smoke Runtime mesh extractor registration failed")) {
        return 7;
    }

    DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG Opaque Render Feature Smoke";
    device_desc.enable_validation = true;
    device_desc.enable_debug = true;
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "opaque smoke RHI device creation failed")) {
        return 8;
    }
    std::unique_ptr<Device> device = std::move(device_result.value);

    auto shader_program_result = aeg::shader::load_shader_program(AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH);
    if (!require(shader_program_result, "opaque smoke shader package loading failed")) {
        return 9;
    }
    aeg::shader::ShaderProgram shader_program = std::move(shader_program_result.value);
    aeg::shader::ShaderManager shader_manager(*device);
    const auto shader_layout_result =
        shader_manager.build_pipeline_layout(shader_program, DebugName{"Renderer.OpaqueSmoke.Layout"});
    if (!require(shader_layout_result, "opaque smoke shader layout creation failed")) {
        return 10;
    }
    aeg::shader::ShaderPipelineLayout shader_layout = std::move(shader_layout_result.value);
    const uint64_t material_pso_epoch = shader_manager.cache_epoch();
    const aeg::renderer::RenderMaterialShaderContract material_shader_contract{
        aeg::renderer::kRenderMaterialShaderContractVersion,
        aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(shader_layout),
        aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(shader_layout)};
    const auto binding_layout_result = shader_manager.binding_layout(shader_layout, 0);
    if (!require(binding_layout_result, "opaque smoke object binding layout lookup failed")) {
        return 11;
    }
    const BindingLayoutHandle binding_layout = binding_layout_result.value;
    const auto material_binding_layout_result = shader_manager.binding_layout(shader_layout, 1);
    if (!require(material_binding_layout_result, "opaque smoke material binding layout lookup failed")) {
        return 12;
    }
    const BindingLayoutHandle material_binding_layout = material_binding_layout_result.value;
    const auto lighting_binding_layout_result = shader_manager.binding_layout(shader_layout, 2);
    if (!require(lighting_binding_layout_result, "opaque smoke lighting binding layout lookup failed")) {
        return 13;
    }
    const BindingLayoutHandle lighting_binding_layout = lighting_binding_layout_result.value;

    TextureDesc target_desc{};
    target_desc.extent = {kTargetWidth, kTargetHeight, 1};
    target_desc.format = Format::kRgba8Unorm;
    target_desc.mip_levels = 1;
    target_desc.array_layers = 1;
    target_desc.capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
    target_desc.memory = MemoryClass::kDeviceLocal;
    target_desc.initial_usage = ResourceUsage::kUndefined;
    target_desc.initial_stages = PipelineStage::kAllCommands;
    target_desc.initial_queue = QueueClass::kAuto;
    target_desc.name = DebugName{"Renderer.OpaqueSmoke.Target"};
    const auto target_texture_result = device->create_texture(target_desc);
    if (!require(target_texture_result, "opaque smoke target texture creation failed")) {
        return 13;
    }
    TextureHandle target_texture = target_texture_result.value;
    const auto target_view_result = device->create_texture_view({
        .texture = target_texture,
        .kind = TextureViewKind::kColorAttachment,
        .dimension = TextureViewDimension::kTex2D,
        .format = target_desc.format,
        .subresources = {aeg::rhi::Aspect::kColor, 0, 1, 0, 1},
        .name = DebugName{"Renderer.OpaqueSmoke.TargetView"},
    });
    if (!require(target_view_result, "opaque smoke target view creation failed")) {
        return 14;
    }
    TextureViewHandle target_view = target_view_result.value;

    TextureDesc depth_desc = target_desc;
    depth_desc.format = Format::kD32Float;
    depth_desc.capabilities = TextureCapability::kDepthStencil | TextureCapability::kSampled;
    depth_desc.name = DebugName{"Renderer.OpaqueSmoke.Depth"};
    const auto depth_texture_result = device->create_texture(depth_desc);
    if (!require(depth_texture_result, "opaque smoke depth texture creation failed")) {
        return 15;
    }
    TextureHandle depth_texture = depth_texture_result.value;
    const auto depth_view_result = device->create_texture_view({
        .texture = depth_texture,
        .kind = TextureViewKind::kDepthStencil,
        .dimension = TextureViewDimension::kTex2D,
        .format = depth_desc.format,
        .subresources = {aeg::rhi::Aspect::kDepth, 0, 1, 0, 1},
        .name = DebugName{"Renderer.OpaqueSmoke.DepthView"},
    });
    if (!require(depth_view_result, "opaque smoke depth view creation failed")) {
        return 16;
    }
    TextureViewHandle depth_view = depth_view_result.value;

    BufferDesc vertex_desc{};
    vertex_desc.size = sizeof(kTriangleVertices);
    vertex_desc.capabilities = BufferCapability::kVertex;
    vertex_desc.memory = MemoryClass::kUpload;
    vertex_desc.initial_usage = ResourceUsage::kVertexInput;
    vertex_desc.initial_stages = PipelineStage::kVertexInput;
    vertex_desc.initial_queue = QueueClass::kGraphics;
    vertex_desc.name = DebugName{"Renderer.OpaqueSmoke.VertexBuffer"};
    const auto vertex_buffer_result = device->create_buffer(vertex_desc);
    if (!require(vertex_buffer_result, "opaque smoke vertex buffer creation failed")) {
        return 14;
    }
    BufferHandle vertex_buffer = vertex_buffer_result.value;
    auto vertices = kTriangleVertices;
    if (backlit)
        for (auto& vertex : vertices)
            vertex.nz = -1.0f;
    if (!require(device->write_buffer(vertex_buffer, 0, std::as_bytes(std::span{vertices})),
                 "opaque smoke vertex upload failed")) {
        return 15;
    }

    BufferDesc index_desc{};
    index_desc.size = sizeof(kTriangleIndices);
    index_desc.capabilities = BufferCapability::kIndex;
    index_desc.memory = MemoryClass::kUpload;
    index_desc.initial_usage = ResourceUsage::kIndexInput;
    index_desc.initial_stages = PipelineStage::kVertexInput;
    index_desc.initial_queue = QueueClass::kGraphics;
    index_desc.name = DebugName{"Renderer.OpaqueSmoke.IndexBuffer"};
    const auto index_buffer_result = device->create_buffer(index_desc);
    if (!require(index_buffer_result, "opaque smoke index buffer creation failed")) {
        return 16;
    }
    BufferHandle index_buffer = index_buffer_result.value;
    if (!require(device->write_buffer(index_buffer, 0, std::as_bytes(std::span{kTriangleIndices})),
                 "opaque smoke index upload failed")) {
        return 17;
    }

    BufferDesc transform_desc{};
    transform_desc.size = sizeof(aeg::renderer::RenderObjectConstants);
    transform_desc.capabilities = BufferCapability::kUniform;
    transform_desc.memory = MemoryClass::kUpload;
    transform_desc.initial_usage = ResourceUsage::kUniformRead;
    transform_desc.initial_stages = PipelineStage::kVertexShader;
    transform_desc.initial_queue = QueueClass::kGraphics;
    transform_desc.name = DebugName{"Renderer.OpaqueSmoke.TransformBuffer"};
    const auto transform_buffer_result = device->create_buffer(transform_desc);
    if (!require(transform_buffer_result, "opaque smoke transform buffer creation failed")) {
        return 18;
    }
    BufferHandle transform_buffer = transform_buffer_result.value;
    const auto binding_set_result = device->create_binding_set(
        BindingSetDesc{binding_layout, BindingSetLifetime::kPersistent, DebugName{"Renderer.OpaqueSmoke.ObjectSet"}});
    if (!require(binding_set_result, "opaque smoke binding set creation failed")) {
        return 19;
    }
    BindingSetHandle binding_set = binding_set_result.value;
    BindingWrite transform_write{};
    transform_write.location = aeg::rhi::BindingLocation{0, 0};
    transform_write.resource = BindingResource{};
    transform_write.resource.type = BindingType::kUniformBuffer;
    transform_write.resource.buffer_range =
        BufferRange{transform_buffer, 0, sizeof(aeg::renderer::RenderObjectConstants)};
    if (!require(device->update_binding_set(binding_set, std::span{&transform_write, 1}),
                 "opaque smoke binding set update failed")) {
        return 20;
    }

    const std::array<VertexBindingDesc, 1> vertex_bindings{
        {VertexBindingDesc{0, sizeof(Vertex), VertexInputRate::kVertex}}};
    const std::array<VertexAttributeDesc, 3> vertex_attributes{
        VertexAttributeDesc{0, 0, Format::kRgba32Float, 0},
        VertexAttributeDesc{1, 0, Format::kRg32Float, sizeof(float) * 4u},
        VertexAttributeDesc{2, 0, Format::kRgb32Float, sizeof(float) * 6u}};
    const std::array<Format, 1> color_formats{{target_desc.format}};
    GraphicsPipelineState graphics_state{};
    graphics_state.vertex_bindings = vertex_bindings;
    graphics_state.vertex_attributes = vertex_attributes;
    graphics_state.topology = PrimitiveTopology::kTriangleList;
    graphics_state.cull_mode = CullMode::kNone;
    graphics_state.depth_test_enable = true;
    graphics_state.depth_write_enable = true;
    graphics_state.depth_stencil_format = depth_desc.format;
    graphics_state.color_formats = color_formats;
    if (neutral) {
        graphics_state.depth_test_enable = false;
        graphics_state.depth_write_enable = false;
        graphics_state.depth_stencil_format = Format::kUnknown;
    }
    const auto graphics_pipeline_result =
        shader_manager.create_pipeline(shader_program, shader_layout,
                                       aeg::shader::ShaderPipelineDesc{
                                           .type = PipelineType::kGraphics,
                                           .graphics = graphics_state,
                                           .name = DebugName{"Renderer.OpaqueSmoke.Pipeline"},
                                       });
    if (!require(graphics_pipeline_result, "opaque smoke graphics pipeline creation failed")) {
        return 21;
    }
    PipelineHandle graphics_pipeline = graphics_pipeline_result.value;
    PipelineHandle gbuffer_pipeline{};
    aeg::renderer::RendererProfilePsoContract gbuffer_profile_contract{};
    if (deferred) {
        const auto program = aeg::shader::load_shader_program(AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH);
        if (!require(program, "deferred geometry shader load failed"))
            return 34;
        const auto layout = shader_manager.build_pipeline_layout(program.value);
        if (!require(layout, "deferred geometry layout failed"))
            return 35;
        auto graphics = graphics_state;
        const std::array<Format, 3> formats{Format::kRgba16Float, Format::kRgba16Float, Format::kRgba8Unorm};
        graphics.color_formats = formats;
        const auto pipeline = shader_manager.create_pipeline(
            program.value, layout.value, {PipelineType::kGraphics, graphics, DebugName{"DeferredSmoke.GBuffer"}});
        if (!require(pipeline, "deferred geometry pipeline failed"))
            return 36;
        gbuffer_pipeline = pipeline.value;
        const auto epoch = aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(layout.value);
        gbuffer_profile_contract = {
            {aeg::renderer::kRendererProfileContractVersion, aeg::renderer::RendererProfile::kGBuffer,
             aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(layout.value), epoch},
            {graphics.color_formats[0], graphics.samples, graphics.depth_stencil_format,
             {graphics.color_formats[1], graphics.color_formats[2]}}, epoch};
    }
    PipelineHandle depth_pipeline{};
    aeg::renderer::RendererProfilePsoContract depth_profile_contract{};
    PipelineHandle clustered_pipeline{};
    aeg::renderer::RenderMaterialAttachmentContract clustered_contract{};
    aeg::renderer::RendererProfilePsoContract clustered_profile_contract{};
    aeg::renderer::RenderMaterialPipelineVariant unshadowed_clustered{};
    aeg::renderer::LightCullingResources culling_resources;
    if (forward) {
        const auto variants = aeg::renderer::create_clustered_forward_pipelines(*device, shader_manager, graphics_state,
            AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH, AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH);
        if (!require(variants, "forward clustered pipeline variants failed")) return 45;
        clustered_pipeline = variants.value.shadowed.pipeline;
        clustered_contract = variants.value.shadowed.attachment_contract;
        clustered_profile_contract = variants.value.shadowed.profile_contract;
        unshadowed_clustered = variants.value.unshadowed;
        if (!require(culling_resources.initialize(*device, AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH,
                     variants.value.shadowed_inputs, variants.value.unshadowed_inputs),
                     "forward culling initialization failed"))
            return 46;
        const auto depth_program = aeg::shader::load_shader_program(AEG_DEPTH_MESH_SHADER_PACKAGE_PATH);
        if (!require(depth_program, "forward depth shader load failed"))
            return 30;
        const auto depth_layout = shader_manager.build_pipeline_layout(depth_program.value);
        if (!require(depth_layout, "forward depth layout failed"))
            return 31;
        auto depth_graphics = graphics_state;
        depth_graphics.color_formats = {};
        depth_graphics.vertex_attributes = std::span<const VertexAttributeDesc>{vertex_attributes.data(), 1};
        const auto depth_pipeline_result =
            shader_manager.create_pipeline(depth_program.value, depth_layout.value,
                                           {PipelineType::kGraphics, depth_graphics, DebugName{"ForwardSmoke.Depth"}});
        if (!require(depth_pipeline_result, "forward depth pipeline failed"))
            return 32;
        depth_pipeline = depth_pipeline_result.value;
        depth_profile_contract = {
            {aeg::renderer::kRendererProfileContractVersion, aeg::renderer::RendererProfile::kShadow,
             aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(depth_layout.value),
             aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(depth_layout.value)},
            {Format::kUnknown, depth_graphics.samples, depth_graphics.depth_stencil_format},
            aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(depth_layout.value)};
    }

    OpaqueSmokeResolver resolver(*device, mesh_id, material_id,
                                 aeg::renderer::RenderMeshResource{
                                     .vertex_buffer = vertex_buffer,
                                     .vertex_buffer_desc = vertex_desc,
                                     .index_buffer = index_buffer,
                                     .index_buffer_desc = index_desc,
                                     .vertex_stride = sizeof(Vertex),
                                     .vertex_count = static_cast<uint32_t>(kTriangleVertices.size()),
                                     .index_count = static_cast<uint32_t>(kTriangleIndices.size()),
                                     .index_format = aeg::rhi::IndexFormat::kUint16,
                                     .vertex_layout =
                                         {
                                             .position_offset = 0,
                                             .position_format = Format::kRgba32Float,
                                             .uv_offset = sizeof(float) * 4u,
                                             .uv_format = Format::kRg32Float,
                                             .normal_offset = sizeof(float) * 6u,
                                             .normal_format = Format::kRgb32Float,
                                         },
                                 },
                                 aeg::renderer::RenderMaterialResource{.pipeline = graphics_pipeline},
                                 aeg::renderer::RenderObjectResource{
                                     .transform_buffer = transform_buffer,
                                     .transform_buffer_desc = transform_desc,
                                     .binding_layout = binding_layout,
                                     .binding_set = binding_set,
                                     .binding_group = 0,
                                     .transform_offset = 0,
                                     .transform_size = sizeof(aeg::renderer::RenderObjectConstants),
                                 });

    aeg::pipeline::PipelineScheduler scheduler;
    aeg::render_graph::FrameContext frame_context(*device);
    resolver.deferred_frame = &frame_context;
    resolver.culling_resources = &culling_resources;
    aeg::renderer::PostProcessResources post_resources;
    if (forward || deferred) {
        if (!require(post_resources.initialize(*device, AEG_POST_PROCESS_SHADER_PACKAGE_PATH, target_desc.format),
                     "postprocess initialization failed"))
            return 48;
        resolver.post_resources = &post_resources;
    }
    aeg::renderer::DeferredLightingResources deferred_resources;
    if (deferred) {
        const auto initialized = deferred_resources.initialize(*device, AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH,
                                                               aeg::renderer::kSceneLightingFormat);
        if (!initialized) {
            std::cerr << "deferred lighting initialization failed: " << initialized.message << '\n';
            return 38;
        }
        resolver.deferred_resources = &deferred_resources;
        resolver.deferred_frame = &frame_context;
        const auto inactive = deferred_resources.for_frame(frame_context);
        if (!require(!inactive && inactive.result.code == ResultCode::kInvalidState,
                     "deferred resources accepted an inactive frame"))
            return 39;
    }
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::resource::ResourceUploader cold_buffers(*device);
    aeg::resource::TextureUploader cold_textures(*device);
    aeg::application::runtime::RuntimeAssetUploadHost cold_upload_context{};
    std::optional<aeg::application::runtime::RuntimeAssetResolver> cold_assets;
    std::filesystem::path cold_package;
    if (cold || cold_retry) {
        const auto mesh_uuid = aeg::foundation::AssetId::parse("00000000-0000-4000-8000-000000000011").value;
        const auto material_uuid = aeg::foundation::AssetId::parse("00000000-0000-4000-8000-000000000012").value;
        const auto texture_uuid = aeg::foundation::AssetId::parse("00000000-0000-4000-8000-000000000013").value;
        const auto shader_uuid = aeg::foundation::AssetId::parse("00000000-0000-4000-8000-000000000014").value;
        std::vector<std::byte> mesh_bytes(16 + 32 * 3 + 12);
        const std::array<uint32_t, 4> header{3, 3, 32, 0};
        std::memcpy(mesh_bytes.data(), header.data(), 16);
        for (size_t i = 0; i < kTriangleVertices.size(); ++i) {
            const auto& v = kTriangleVertices[i];
            const std::array<float, 8> vertex{v.x, v.y, v.z, v.nx, v.ny, v.nz, v.u, v.v};
            std::memcpy(mesh_bytes.data() + 16 + i * 32, vertex.data(), 32);
        }
        const std::array<uint32_t, 3> indices{0, 1, 2};
        std::memcpy(mesh_bytes.data() + 112, indices.data(), 12);
        std::vector<std::byte> texture_bytes(16);
        const std::array<uint32_t, 3> texture_header{1, 1, 4};
        const std::array<uint8_t, 4> pixel{196, 48, 86, 255};
        std::memcpy(texture_bytes.data(), texture_header.data(), 12);
        std::memcpy(texture_bytes.data() + 12, pixel.data(), 4);
        aeg::asset::MaterialPayload material{};
        material.shader = shader_uuid;
        material.textures = {{1, texture_uuid}};
        material.parameters = {{5955903723798558642ull, std::vector<double>{.5, .75, .25, 1.}}};
        const auto material_bytes = aeg::asset::encode_material_payload(material);
        const auto mesh_entry = aeg::asset::encode_cooked_asset({mesh_uuid, aeg::foundation::AssetKind::kMesh, 1, {}, mesh_bytes});
        const auto material_entry = aeg::asset::encode_cooked_asset({material_uuid, aeg::foundation::AssetKind::kMaterial, 1, {}, material_bytes.value});
        const auto texture_entry = aeg::asset::encode_cooked_asset({texture_uuid, aeg::foundation::AssetKind::kTexture, 1, {}, texture_bytes});
        cold_package = std::filesystem::temp_directory_path() / (cold_retry ? "aeg_scene_retry.aegbin" : "aeg_scene_cold.aegbin");
        if (!require(material_bytes && mesh_entry && material_entry && texture_entry, "cold asset encoding failed") ||
            !require(aeg::tooling::AssetPackageWriter{}.write(cold_package,
                {{mesh_uuid, aeg::foundation::AssetKind::kMesh, 1, mesh_entry.value},
                 {material_uuid, aeg::foundation::AssetKind::kMaterial, 1, material_entry.value},
                 {texture_uuid, aeg::foundation::AssetKind::kTexture, 1, texture_entry.value}}), "cold asset package failed")) return 57;
        auto opened = aeg::application::runtime::RuntimeAssetResolver::open(cold_package);
        if (!require(opened, "cold resolver open failed")) return 58;
        cold_assets.emplace(std::move(opened.value));
        resolver.cold_assets = &*cold_assets;
        resolver.cold_mesh = cold_assets->bind_mesh(mesh_uuid).value;
        resolver.cold_material = cold_assets->bind_material(material_uuid).value;
        const auto material_epoch = aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(shader_layout);
        cold_upload_context = {
            .device = device.get(), .frame_context = &frame_context, .graph = &render_graph,
            .uploader = &cold_buffers, .texture_uploader = &cold_textures,
            .material_binding_layout = material_binding_layout, .material_pipeline = graphics_pipeline,
            .material_attachment_contract = {target_desc.format},
            .material_shader_contract = {aeg::renderer::kRenderMaterialShaderContractVersion,
                aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(shader_layout), material_epoch},
            .variant_cache_epoch = material_epoch, .material_shader_layout = &shader_layout,
            .material_shader = shader_uuid,
            .material_shader_program = &shader_program};
        if (!require(cold_assets->set_upload_context(cold_upload_context), "cold upload host publication failed"))
            return 58;
    }
    const aeg::engine::EngineFramePassRegistry pass_host(passes);
    aeg::engine::EngineFramePipelineHost pipeline_host(registry, scheduler, &pass_host);
    aeg::engine::EngineFrameOrchestrator orchestrator(pipeline_host, frame_context, render_graph);
    if (!require(orchestrator.register_world_frame(world_scope, kViewId),
                 "opaque smoke World frame registration failed") ||
        (neutral_registered && !require(orchestrator.register_world_frame(world_scope, kSecondViewId),
                                        "opaque smoke second World frame View registration failed")) ||
        !require(orchestrator.initialize(), "opaque smoke orchestrator initialization failed")) {
        return 22;
    }

    OpaqueSmokeState smoke_state{};
    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.fixed_tick = 1;
    frame_desc.render_output_extent = {kTargetWidth, kTargetHeight};
    frame_desc.renderer = &renderer;
    frame_desc.render_resource_resolver = &resolver;
    frame_desc.require_resolved_render_resources = !cold && !cold_retry;
    frame_desc.user_data = &smoke_state;
    frame_desc.render_view_resolver = [&smoke_state, neutral](const aeg::engine::EngineFrameRenderViewContext& context)
        -> aeg::foundation::ResultValue<aeg::renderer::RenderView> {
        aeg::renderer::RenderView view{};
        view.id = aeg::renderer::make_render_view_id(context.world_snapshot.world(), context.view_id);
        view.world = context.world_snapshot.world();
        view.view_id = context.view_id;
        smoke_state.resolved_view_ids.push_back(context.view_id);
        view.output_extent = context.output_extent;
        view.color_target = kOpaqueSceneColor;
        if (!neutral) view.depth_target = RenderResourceId{"OpaqueSceneDepth"};
        smoke_state.render_view_received_output_extent =
            context.output_extent == aeg::renderer::RenderExtent{kTargetWidth, kTargetHeight};
        return {Result::success(), view};
    };
    std::shared_ptr<const std::vector<aeg::feature::RenderFeatureOutput>> raw_scene_outputs;
    frame_desc.render_feature_preparation = [&smoke_state, &resolver, &raw_scene_outputs, scene_pass_host,
                                             device_ptr = device.get(),
                                             material_binding_layout, lighting_binding_layout, graphics_pipeline,
                                             target_texture, target_view, target_desc, depth_texture, depth_view,
                                             depth_desc, depth_pipeline, depth_profile_contract, gbuffer_pipeline,
                                             gbuffer_profile_contract, clustered_pipeline,
                                             clustered_contract, clustered_profile_contract, unshadowed_clustered,
                                             material_shader_contract, material_pso_epoch, neutral,
                                             deferred, forward](const aeg::engine::EngineFramePreparationContext& context) {
        const Result invalid_binding = context.bind_feature_texture(kOpaqueSceneColor, {});
        if (invalid_binding || invalid_binding.code != ResultCode::kInvalidArgument) {
            return aeg::foundation::invalid_state("opaque smoke accepted an invalid feature texture binding");
        }
        smoke_state.invalid_texture_binding_rejected = true;

        const auto target_resource =
            context.render_graph.import_texture(target_texture, target_view, target_desc,
                                                ResourceState{ResourceUsage::kUndefined, ShaderStage::kAll,
                                                              QueueClass::kAuto, false, PipelineStage::kAllCommands});
        if (!target_resource) {
            return target_resource.result;
        }
        if (const Result result = context.bind_feature_texture(kOpaqueSceneColor, target_resource.value); !result) {
            return result;
        }
        const auto depth_resource =
            context.render_graph.import_texture(depth_texture, depth_view, depth_desc,
                                                ResourceState{ResourceUsage::kUndefined, ShaderStage::kAll,
                                                              QueueClass::kAuto, false, PipelineStage::kAllCommands});
        if (!depth_resource) {
            return depth_resource.result;
        }
        if (const Result result =
                context.bind_feature_texture(RenderResourceId{"OpaqueSceneDepth"}, depth_resource.value);
            !result) {
            return result;
        }
        if (!neutral) {
            if (const auto result = bind_scene_targets(context.render_graph, target_resource.value,
                    depth_resource.value, target_desc, forward, deferred, smoke_state.scene_textures); !result) return result;
        }

        constexpr std::array<std::byte, 4> kMaterialPixel{std::byte{38}, std::byte{204}, std::byte{64}, std::byte{255}};
        const aeg::resource::TextureSubresourceData material_subresource{
            std::span<const std::byte>{kMaterialPixel.data(), kMaterialPixel.size()}, 0, 0};
        aeg::rhi::TextureDesc material_desc{};
        material_desc.extent = {1, 1, 1};
        material_desc.format = Format::kRgba8Unorm;
        material_desc.mip_levels = 1;
        material_desc.array_layers = 1;
        material_desc.capabilities = TextureCapability::kSampled;
        material_desc.memory = MemoryClass::kDeviceLocal;
        material_desc.initial_usage = ResourceUsage::kUndefined;
        material_desc.initial_stages = PipelineStage::kAllCommands;
        material_desc.initial_queue = QueueClass::kAuto;
        const aeg::resource::TextureData material_data{
            material_desc, std::span<const aeg::resource::TextureSubresourceData>{&material_subresource, 1}};
        aeg::resource::TextureUploader uploader(*device_ptr);
        const auto upload = uploader.upload_texture(context.frame_context, context.render_graph, material_data,
                                                    "Renderer.OpaqueSmoke.Material");
        if (!upload) {
            return upload.result;
        }
        smoke_state.material_texture = upload.value.texture;
        const aeg::resource::TextureMaterialDesc material_desc_resource{
            .texture_view = smoke_state.material_texture.sampled_view,
            .sampler =
                {
                    .min_filter = aeg::rhi::Filter::kNearest,
                    .mag_filter = aeg::rhi::Filter::kNearest,
                    .mip_filter = aeg::rhi::Filter::kNearest,
                    .address_u = aeg::rhi::AddressMode::kClampToEdge,
                    .address_v = aeg::rhi::AddressMode::kClampToEdge,
                    .address_w = aeg::rhi::AddressMode::kClampToEdge,
                    .name = DebugName{"Renderer.OpaqueSmoke.MaterialSampler"},
                },
            .binding_layout = material_binding_layout,
            .binding_group = 1,
            .name = DebugName{"Renderer.OpaqueSmoke.Material"},
        };
        const auto material_result = aeg::resource::create_texture_material(*device_ptr, material_desc_resource);
        if (!material_result) {
            return material_result.result;
        }
        smoke_state.material = material_result.value;

        const aeg::resource::TextureMaterialParameters material_parameters{{0.5f, 0.75f, 0.25f, 1.0f}};
        BufferDesc material_parameter_desc{};
        material_parameter_desc.size = sizeof(material_parameters);
        material_parameter_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kUniform;
        material_parameter_desc.memory = MemoryClass::kDeviceLocal;
        material_parameter_desc.initial_usage = ResourceUsage::kUndefined;
        material_parameter_desc.initial_stages = PipelineStage::kAllCommands;
        material_parameter_desc.initial_queue = QueueClass::kAuto;
        material_parameter_desc.name = DebugName{"Renderer.OpaqueSmoke.MaterialParameters"};
        const aeg::resource::BufferUploadDesc material_parameter_upload_desc{
            .data = std::as_bytes(std::span{&material_parameters, 1}),
            .destination = material_parameter_desc,
            .destination_offset = 0,
            .alignment = 16,
        };
        aeg::resource::ResourceUploader resource_uploader(*device_ptr);
        const auto material_parameter_upload = resource_uploader.upload_buffer(
            context.frame_context, context.render_graph, material_parameter_upload_desc);
        if (!material_parameter_upload) {
            return material_parameter_upload.result;
        }
        smoke_state.material_parameter_buffer = material_parameter_upload.value.destination;
        smoke_state.material_parameter_buffer_desc = material_parameter_desc;
        smoke_state.material_parameter_resource = {
            material_parameter_upload.value.target.index,
            material_parameter_upload.value.target.generation,
            material_parameter_upload.value.target.graph_id,
        };
        if (const Result result = aeg::resource::update_texture_material_parameters(
                *device_ptr, smoke_state.material,
                BufferRange{smoke_state.material_parameter_buffer, 0, sizeof(material_parameters)});
            !result) {
            return result;
        }

        constexpr std::array<std::byte, 4> kReplacementPixel{std::byte{196}, std::byte{48}, std::byte{86},
                                                             std::byte{255}};
        const aeg::resource::TextureSubresourceData replacement_subresource{
            std::span<const std::byte>{kReplacementPixel.data(), kReplacementPixel.size()}, 0, 0};
        const aeg::resource::TextureData replacement_data{
            material_desc, std::span<const aeg::resource::TextureSubresourceData>{&replacement_subresource, 1}};
        const auto replacement_upload = uploader.upload_texture(context.frame_context, context.render_graph,
                                                                replacement_data, "Renderer.OpaqueSmoke.Replacement");
        if (!replacement_upload) {
            return replacement_upload.result;
        }
        smoke_state.replacement_texture = replacement_upload.value.texture;
        const BindingSetHandle binding_set_before_update = smoke_state.material.binding_set;
        if (const Result result = aeg::resource::update_texture_material(*device_ptr, smoke_state.material,
                                                                         smoke_state.replacement_texture.sampled_view);
            !result) {
            return result;
        }
        if (smoke_state.material.binding_set != binding_set_before_update) {
            return aeg::foundation::invalid_state("opaque smoke material update recreated its binding set");
        }
        aeg::renderer::RenderMaterialResource material_resource{};
        material_resource.pipeline = graphics_pipeline;
        material_resource.attachment_contract = {target_desc.format, target_desc.samples,
            neutral ? Format::kUnknown : depth_desc.format};
        material_resource.shader_contract = material_shader_contract;
        material_resource.variant_cache_epoch = material_pso_epoch;
        material_resource.depth_pipeline = depth_pipeline;
        if (depth_pipeline.is_valid()) {
            material_resource.depth_pipeline_contract = depth_profile_contract.attachment;
            material_resource.depth_profile_contract = depth_profile_contract;
            material_resource.variant_cache_epoch = depth_profile_contract.cache_epoch;
        }
        material_resource.gbuffer_pipeline = gbuffer_pipeline;
        if (gbuffer_pipeline.is_valid()) {
            material_resource.gbuffer_pipeline_contract = gbuffer_profile_contract.attachment;
            material_resource.gbuffer_profile_contract = gbuffer_profile_contract;
            material_resource.variant_cache_epoch = gbuffer_profile_contract.cache_epoch;
        }
        material_resource.clustered_pipeline = clustered_pipeline;
        material_resource.clustered_pipeline_contract = clustered_contract;
        material_resource.clustered_profile_contract = clustered_profile_contract;
        if (unshadowed_clustered.pipeline.is_valid()) material_resource.unshadowed_clustered_variant = unshadowed_clustered;
        material_resource.binding_set = smoke_state.material.binding_set;
        material_resource.binding_group = smoke_state.material.binding_group;
        material_resource.material_texture = {replacement_upload.value.target.index,
                                              replacement_upload.value.target.generation,
                                              replacement_upload.value.target.graph_id};
        material_resource.material_parameter_buffer = smoke_state.material_parameter_buffer;
        material_resource.material_parameter_buffer_desc = smoke_state.material_parameter_buffer_desc;
        material_resource.material_parameter_resource = smoke_state.material_parameter_resource;
        material_resource.material_parameter_size = sizeof(material_parameters);
        resolver.set_material(material_resource);

        const aeg::renderer::RenderLightingConstants lighting_constants{
            .directional_direction_intensity = {0.0f, 0.0f, -1.0f, deferred ? 0.5f : 0.0f},
            .directional_color = {1.0f, 0.5f, 0.0f, 1.0f},
            .ambient_color =
                deferred ? aeg::math::Vec4{0.1f, 0.1f, 0.1f, 1.0f} : aeg::math::Vec4{1.0f, 1.0f, 1.0f, 1.0f},
        };
        BufferDesc lighting_desc{};
        lighting_desc.size = sizeof(lighting_constants);
        lighting_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kUniform;
        lighting_desc.memory = MemoryClass::kDeviceLocal;
        lighting_desc.initial_usage = ResourceUsage::kUndefined;
        lighting_desc.initial_stages = PipelineStage::kAllCommands;
        lighting_desc.initial_queue = QueueClass::kAuto;
        lighting_desc.name = DebugName{"Renderer.OpaqueSmoke.Lighting"};
        const auto lighting_upload = resource_uploader.upload_buffer(
            context.frame_context, context.render_graph,
            aeg::resource::BufferUploadDesc{std::as_bytes(std::span{&lighting_constants, 1}), lighting_desc, 0, 16});
        if (!lighting_upload) {
            return lighting_upload.result;
        }
        const auto lighting_set_result = device_ptr->create_binding_set(BindingSetDesc{
            lighting_binding_layout, BindingSetLifetime::kPersistent, DebugName{"Renderer.OpaqueSmoke.LightingSet"}});
        if (!lighting_set_result) {
            return lighting_set_result.result;
        }
        BindingResource lighting_resource{};
        lighting_resource.type = BindingType::kUniformBuffer;
        lighting_resource.buffer_range = BufferRange{lighting_upload.value.destination, 0, sizeof(lighting_constants)};
        const BindingWrite lighting_write{{2, 0}, 0, lighting_resource};
        if (const Result result =
                device_ptr->update_binding_set(lighting_set_result.value, std::span{&lighting_write, 1});
            !result) {
            (void)device_ptr->destroy_binding_set(lighting_set_result.value);
            return result;
        }
        smoke_state.lighting_buffer = lighting_upload.value.destination;
        smoke_state.lighting_binding_set = lighting_set_result.value;
        resolver.set_lighting(aeg::renderer::RenderLightingResource{
            .binding_set = lighting_set_result.value,
            .binding_group = 2,
            .lighting_buffer = lighting_upload.value.destination,
            .lighting_buffer_desc = lighting_desc,
            .lighting_resource = {lighting_upload.value.target.index, lighting_upload.value.target.generation,
                                  lighting_upload.value.target.graph_id},
            .lighting_offset = 0,
            .lighting_size = sizeof(lighting_constants),
        });

        const auto readback_allocation = context.frame_context.allocate_readback(kReadbackSize, 256);
        if (!readback_allocation) {
            return readback_allocation.result;
        }
        const auto readback_resource = context.render_graph.import_buffer(
            readback_allocation.value.buffer, make_readback_desc(readback_allocation.value.buffer_size),
            make_host_read_state());
        if (!readback_resource) {
            return readback_resource.result;
        }
        smoke_state.readback = readback_allocation.value;
        smoke_state.readback_resource = {
            readback_resource.value.index,
            readback_resource.value.generation,
            readback_resource.value.graph_id,
        };
        smoke_state.color_target_bound = true;
        if (!neutral) {
            if (context.render_scenes.size() != 1 || !require(context.render_graph.compile(),
                    "renderer profile contract baseline compile failed"))
                return aeg::foundation::invalid_state("renderer profile contract fixture has no scene or baseline");
            const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph);
            if (!raw_scene_outputs || raw_scene_outputs->size() != 1)
                return aeg::foundation::invalid_state("renderer profile contract fixture lost its committed capture");
            const auto scoped_outputs = std::make_shared<const std::vector<aeg::feature::RenderFeatureOutput>>(
                std::vector{aeg::renderer::scope_render_feature_output(raw_scene_outputs->front(),
                    context.render_scenes.front()->view.id, true)});
            const std::array scenes{aeg::renderer::RenderFeatureSceneBinding{
                aeg::renderer::kRuntimeSceneResourceId, 1, {}, context.render_scenes.front(), &resolver}};
            const aeg::renderer::RenderFeatureDrawLowerer lowerer(scenes, scene_pass_host);
            const auto profile_name = forward ? "Forward depth" : deferred ? "Deferred GBuffer" : "Opaque material";
            const unsigned case_count = forward ? 2u : deferred ? 16u : 8u;
            for (unsigned test_case = 0; test_case < case_count; ++test_case) {
                auto incompatible_material = material_resource;
                if (forward) {
                    auto& attachment = incompatible_material.depth_pipeline_contract;
                    if (test_case == 0)
                        attachment.depth_stencil_format = Format::kD24UnormS8Uint;
                    else
                        attachment.samples = aeg::rhi::SampleCount::kX4;
                    incompatible_material.depth_profile_contract.attachment = attachment;
                    if (!incompatible_material.depth_profile_contract.is_valid(aeg::renderer::RendererProfile::kShadow))
                        return aeg::foundation::invalid_state("forward depth mismatch fixture has an invalid profile");
                } else if (deferred) {
                    auto& profile = incompatible_material.gbuffer_profile_contract;
                    auto& attachment = incompatible_material.gbuffer_pipeline_contract;
                    switch (test_case) {
                        case 0: profile = {}; break;
                        case 1: profile.shader.profile = aeg::renderer::RendererProfile::kDeferredLighting; break;
                        case 2: ++profile.shader.version; break;
                        case 3: profile.shader.layout_hash = 0; break;
                        case 4: profile.shader.cache_epoch = 0; break;
                        case 5: profile.cache_epoch = 0; break;
                        case 6: ++profile.cache_epoch; break;
                        case 7: ++profile.shader.cache_epoch; ++profile.cache_epoch; break;
                        case 8: incompatible_material.variant_cache_epoch = 0; break;
                        case 9: profile.attachment.additional_color_formats[1] = Format::kRgba16Float; break;
                        case 10: attachment.additional_color_formats[0] = Format::kRgba8Unorm; break;
                        case 11: attachment.additional_color_formats[1] = Format::kRgba16Float; break;
                        case 12: attachment.samples = aeg::rhi::SampleCount::kX4; break;
                        case 13: attachment.depth_stencil_format = Format::kD24UnormS8Uint; break;
                        case 14: attachment.color_format = Format::kRgba8Unorm; break;
                        case 15: std::swap(attachment.additional_color_formats[0], attachment.additional_color_formats[1]); break;
                    }
                    if (test_case >= 10) {
                        profile.attachment = attachment;
                        if (!profile.is_valid(aeg::renderer::RendererProfile::kGBuffer))
                            return aeg::foundation::invalid_state("GBuffer target mismatch fixture has an invalid profile");
                    }
                } else {
                    switch (test_case) {
                        case 0: ++incompatible_material.shader_contract.version; break;
                        case 1: incompatible_material.shader_contract.layout_hash = 0; break;
                        case 2: incompatible_material.shader_contract.cache_epoch = 0; break;
                        case 3: incompatible_material.attachment_contract.additional_color_formats[0] = Format::kRgba8Unorm; break;
                        case 4: incompatible_material.attachment_contract.additional_color_formats[1] = Format::kRgba8Unorm; break;
                        case 5: incompatible_material.attachment_contract.color_format = Format::kD32Float; break;
                        case 6: incompatible_material.attachment_contract.samples = static_cast<aeg::rhi::SampleCount>(0); break;
                        case 7: incompatible_material.attachment_contract.depth_stencil_format = Format::kRgba16Float; break;
                    }
                }
                resolver.set_material(incompatible_material);
                const auto mesh_calls = resolver.mesh_resolve_count;
                const auto object_calls = resolver.object_resolve_count;
                const auto rejected = aeg::render_graph::build_render_graph_features(context.render_graph,
                    scoped_outputs, {.textures = smoke_state.scene_textures, .draw_lowerer = &lowerer,
                        .current_frame_owner = &context.frame_context},
                    context.frame_context.frame_number());
                resolver.set_material(material_resource);
                if (rejected || rejected.code != ResultCode::kInvalidArgument) {
                    std::cerr << profile_name << " builtin accepted incompatible profile or target metadata: " << test_case << '\n';
                    return aeg::foundation::invalid_state("renderer builtin profile or target contract was not rejected");
                }
                if (resolver.mesh_resolve_count != mesh_calls || resolver.object_resolve_count != object_calls) {
                    std::cerr << "Rejected " << profile_name << " contract consumed mesh/object resources: " << test_case << '\n';
                    return aeg::foundation::invalid_state("rejected renderer profile contract caused resource resolution");
                }
                if (const auto result = context.render_graph.compile(); !result)
                    return result;
                const auto restored = aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph);
                if (restored.pass_count != baseline.pass_count || restored.resource_count != baseline.resource_count ||
                    smoke_state.readback_pass_build_count != 0)
                    return aeg::foundation::invalid_state("rejected renderer profile contract leaked partial graph changes");
            }
        }
        return Result::success();
    };
    aeg::feature::FeatureRegistry feature_registry{aeg::feature::test::clock_test_host()};
    aeg::feature::FeatureRuntime feature_runtime(feature_registry);
    std::shared_ptr<const std::vector<aeg::feature::RenderFeatureOutput>> captured_scene_outputs;
    aeg::renderer::RenderViewId captured_scene_view{};
    std::weak_ptr<const std::vector<aeg::feature::RenderFeatureOutput>> weak_outputs;
    std::weak_ptr<const std::vector<aeg::feature::RenderFeatureOutput>> weak_second_outputs;
    std::weak_ptr<const aeg::renderer::RenderSceneSnapshot> weak_scene;
    std::weak_ptr<const aeg::renderer::RenderSceneSnapshot> weak_second_scene;
    if (neutral) {
        auto source = render_feature_test::definition();
        source.contract.permissions.capability_mask &= ~(aeg::feature::to_mask(aeg::feature::RenderIntentCapability::kCreateTransient) |
            aeg::feature::to_mask(aeg::feature::RenderIntentCapability::kCreateHistory));
        source.execute = [neutral_clear](const aeg::feature::InvocationToken& token, const aeg::feature::StateSnapshot&,
                            std::span<const uint8_t>, std::span<const uint8_t>) {
            using namespace aeg::feature;
            const RenderTextureReference target{RenderResourceId("OpaqueSceneColor"), 1, true};
            RenderIntent intent{};
            intent.operations = {RenderClearColor{target, {0.f, 0.f, 1.f, 1.f}},
                                 RenderDrawScene{{RenderResourceId("Smoke.Scene"), 1}, target, RenderAttachmentLoad::kLoad}};
            if (neutral_clear)
                intent.operations = {RenderDrawScene{{RenderResourceId("Smoke.Scene"), 1}, target,
                    RenderAttachmentLoad::kClear, {0.f, 0.f, 1.f, 1.f}}};
            intent.final_output = target;
            StateDelta delta{token.state_version};
            delta.render_intent = std::move(intent);
            return aeg::foundation::ResultValue<StateDelta>{Result::success(), std::move(delta)};
        };
        if (!require(aeg::feature::test::register_clock_test_definition(feature_registry, source), "neutral Feature registration failed")) return 50;
        auto document = render_feature_test::document(source);
        document.nodes.resize(1);
        auto compiled = aeg::feature::FeatureGraphCompiler{}.compile(document, feature_registry);
        if (!require(compiled, "neutral compile failed") ||
            !require(feature_runtime.publish_graph(document, compiled.value), "neutral publication failed") ||
            !require(feature_runtime.tick(source.contract.clock.id), "neutral execute failed")) return 51;
        const auto captured = feature_runtime.capture_render_outputs(compiled.value);
        if (!require(captured, "neutral owner capture failed")) return 52;
        auto outputs = std::make_shared<const std::vector<aeg::feature::RenderFeatureOutput>>(captured.value);
        weak_outputs = outputs;
        frame_desc.render_feature_post_build = [outputs, &weak_outputs, &weak_second_outputs,
            &weak_scene, &weak_second_scene, &smoke_state, &registered_scene,
            neutral_clear, neutral_registered, scene_pass_host, &passes, cold, cold_retry](
            const aeg::engine::EngineFramePostBuildContext& context) {
            if (context.render_scenes.size() != (neutral_registered ? 2u : 1u))
                return aeg::foundation::invalid_state("neutral scene extraction missing");
            weak_scene = context.render_scenes.front();
            const std::array scenes{aeg::renderer::RenderFeatureSceneBinding{
                aeg::feature::RenderResourceId("Smoke.Scene"), 1, {}, context.render_scenes.front(),
                context.render_resource_resolver}};
            auto lowerer = make_scene_lowerer(scenes, scene_pass_host, neutral_registered);
            // Invalid later operations must leave the prepared uploads and baseline graph intact.
            const auto baseline = context.render_graph.compile();
            if (!baseline) return baseline;
            const auto baseline_info = aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph);
            if (neutral_registered) {
                aeg::pipeline::RenderPassRegistry retired_registry;
                if (const auto result = aeg::renderer::register_builtin_render_passes(retired_registry); !result) return result;
                const aeg::render_graph::RenderPassExecutionHost retired_host{&retired_registry,
                    aeg::pipeline::detail::RenderPassRegistryExecution::execution_epoch(retired_registry)};
                if (const auto result = retired_registry.unregister_pass(aeg::renderer::kForwardOpaquePassTypeId); !result) return result;
                for (const auto host : {aeg::render_graph::RenderPassExecutionHost{nullptr, 0}, retired_host}) {
                    auto rejected_lowerer = make_scene_lowerer(scenes, host, true);
                    const auto rejected = aeg::render_graph::build_render_graph_features(context.render_graph, outputs,
                        {.textures = context.feature_textures, .draw_lowerer = &rejected_lowerer}, context.frame_context.frame_number());
                    if (rejected || rejected.code != ResultCode::kInvalidState)
                        return aeg::foundation::invalid_state("registered Scene draw fell back after a null or retired host");
                    if (const auto result = context.render_graph.compile(); !result) return result;
                    const auto restored = aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph);
                    if (restored.pass_count != baseline_info.pass_count || restored.resource_count != baseline_info.resource_count)
                        return aeg::foundation::invalid_state("invalid native host leaked a partial graph");
                }
            }
            for (unsigned test_case = 0; test_case < (cold ? 0u : 10u); ++test_case) {
                auto rejected_outputs = *outputs;
                auto& output = rejected_outputs.front();
                auto draw = std::get<aeg::feature::RenderDrawScene>(output.intent.operations.back());
                std::vector<aeg::renderer::RenderFeatureSceneBinding> invalid_scenes(scenes.begin(), scenes.end());
                invalid_scenes.push_back(scenes.front());
                invalid_scenes.back().id = aeg::feature::RenderResourceId("Invalid.Scene");
                if (test_case >= 2 && test_case <= 6) draw.scene.id = invalid_scenes.back().id;
                std::shared_ptr<aeg::renderer::RenderSceneSnapshot> altered;
                switch (test_case) {
                    case 0: draw.scene.id = aeg::feature::RenderResourceId("Missing.Scene"); break;
                    case 1: ++draw.scene.version; break;
                    case 2: invalid_scenes.back().owner = aeg::feature::FeatureInstanceId(output.instance_id.value() + 100); break;
                    case 3: altered = std::make_shared<aeg::renderer::RenderSceneSnapshot>(*invalid_scenes.back().snapshot);
                            altered->world_revision = 0; invalid_scenes.back().snapshot = altered; break;
                    case 4: altered = std::make_shared<aeg::renderer::RenderSceneSnapshot>(*invalid_scenes.back().snapshot);
                            ++altered->frame_number; invalid_scenes.back().snapshot = altered; break;
                    case 5: invalid_scenes.back().resolver = nullptr; break;
                    case 6: altered = std::make_shared<aeg::renderer::RenderSceneSnapshot>(*invalid_scenes.back().snapshot);
                            ++altered->view.output_extent.width; invalid_scenes.back().snapshot = altered; break;
                    case 7: output.contract.permissions.capability_mask &= ~aeg::feature::to_mask(aeg::feature::RenderIntentCapability::kDrawScene); break;
                    case 8: output.contract.permissions.capability_mask &= ~aeg::feature::to_mask(aeg::feature::RenderIntentCapability::kWriteExternal); break;
                    case 9: output.contract.permissions.capability_mask &= ~aeg::feature::to_mask(aeg::feature::RenderIntentCapability::kReadExternal);
                            draw.load = aeg::feature::RenderAttachmentLoad::kLoad; break;
                }
                output.intent.operations.push_back(draw);
                auto invalid_lowerer = make_scene_lowerer(
                    invalid_scenes, scene_pass_host, neutral_registered);
                const auto rejected = aeg::render_graph::build_render_graph_features(context.render_graph,
                    std::make_shared<const std::vector<aeg::feature::RenderFeatureOutput>>(std::move(rejected_outputs)),
                    {.textures = context.feature_textures, .draw_lowerer = &invalid_lowerer}, context.frame_context.frame_number());
                if (rejected) return aeg::foundation::invalid_state("neutral draw accepted invalid scene/permission");
                if (const auto r = context.render_graph.compile(); !r) return r;
                if (aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph).pass_count != baseline_info.pass_count ||
                    aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph).resource_count != baseline_info.resource_count)
                    return aeg::foundation::invalid_state("rejected neutral draw leaked partial graph changes");
            }
            if (neutral_registered) {
                const auto& first_scene = context.render_scenes.front();
                const auto& second_scene = context.render_scenes.back();
                if (!first_scene->view.id.is_valid() || !second_scene->view.id.is_valid() ||
                    first_scene->view.id == second_scene->view.id ||
                    first_scene->view.view_id == second_scene->view.view_id ||
                    first_scene->view.world != second_scene->view.world)
                    return aeg::foundation::invalid_state("registered Scene fixture did not extract two actual World Views");
                weak_second_scene = second_scene;
                const auto allocation = context.frame_context.allocate_readback(kReadbackSize, 256);
                if (!allocation) return allocation.result;
                const auto second_buffer = context.render_graph.import_buffer(allocation.value.buffer,
                    make_readback_desc(allocation.value.buffer_size), make_host_read_state());
                if (!second_buffer) return second_buffer.result;
                smoke_state.second_view_readback = allocation.value;
                const auto target = context.feature_textures.front().texture;
                for (size_t view_index = 0; view_index < context.render_scenes.size(); ++view_index) {
                    const auto& scene = context.render_scenes[view_index];
                    auto scoped_outputs = std::make_shared<const std::vector<aeg::feature::RenderFeatureOutput>>(
                        std::vector{aeg::renderer::scope_render_feature_output(outputs->front(), scene->view.id,
                            view_index == 0)});
                    if (view_index == 0) weak_outputs = scoped_outputs;
                    else weak_second_outputs = scoped_outputs;
                    const auto& output = scoped_outputs->front();
                    const auto& draw = std::get<aeg::feature::RenderDrawScene>(output.intent.operations.back());
                    if (output.view_key != scene->view.id.value() || output.invocation != outputs->front().invocation)
                        return aeg::foundation::invalid_state("actual View scoping changed the captured Feature invocation");
                    const std::array view_scenes{aeg::renderer::RenderFeatureSceneBinding{
                        draw.scene.id, draw.scene.version, {}, scene, context.render_resource_resolver}};
                    const std::array view_textures{aeg::render_graph::RenderGraphFeatureTextureBinding{
                        draw.target.id, target, draw.target.version}};
                    auto view_lowerer = make_scene_lowerer(
                        view_scenes, scene_pass_host, true);
                    const auto pass_offset = aeg::render_graph::detail::RenderGraphExecution::compile_info(
                        context.render_graph).pass_count;
                    const auto built = aeg::render_graph::build_render_graph_features(context.render_graph,
                        scoped_outputs, {.textures = view_textures, .draw_lowerer = &view_lowerer},
                        context.frame_context.frame_number());
                    if (!built) return built;
                    const auto readback = view_index == 0 ? smoke_state.readback : allocation.value;
                    const auto buffer = view_index == 0 ? aeg::render_graph::RenderGraphBuffer{
                        smoke_state.readback_resource.index, smoke_state.readback_resource.generation,
                        smoke_state.readback_resource.graph_id} : second_buffer.value;
                    const auto copied = context.render_graph.add_pass(
                        view_index == 0 ? "Neutral.Readback" : "Neutral.SecondViewReadback",
                        aeg::rhi::QueueClass::kGraphics,
                        [target, buffer, readback](aeg::render_graph::PassBuilder& p) {
                            if (const auto r = p.read(target, ResourceUsage::kCopySource, PipelineStage::kCopy); !r)
                                return r;
                            if (const auto r = p.write(buffer, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                                ShaderStage::kAll, {readback.offset, kReadbackSize}); !r) return r;
                            p.set_side_effects();
                            return Result::success();
                        }, [target, buffer, readback](aeg::render_graph::PassExecutionContext& command) {
                            return command.copy_texture_to_buffer(target, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {},
                                {kTargetWidth, kTargetHeight, 1}, buffer, readback.offset, 0, 0);
                        });
                    if (!copied) return copied.result;
                    if (const auto compiled = context.render_graph.compile(); !compiled) return compiled;
                    unsigned draws = 0;
                    for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(
                            context.render_graph).pass_plans) {
                        if (pass.pass.index < pass_offset || !pass.feature_work ||
                            pass.feature_work->operation_index != 1) continue;
                        const auto& work = *pass.feature_work;
                        if (!pass.live || registered_scene.setup_sources.empty())
                            return aeg::foundation::invalid_state("actual View geometry draw was culled or has no native source");
                        const auto& source = registered_scene.setup_sources.back();
                        if (work.feature_definition_id != output.contract.definition_id ||
                            work.owner_module != output.owner_module || work.node_id != output.node_id ||
                            work.instance_id != output.instance_id || work.invocation != output.invocation ||
                            work.frame_index != context.frame_context.frame_number() || source.work.id != work.id ||
                            source.work.feature_definition_id != work.feature_definition_id ||
                            source.work.owner_module != work.owner_module || source.work.node_id != work.node_id ||
                            source.work.instance_id != work.instance_id || source.work.invocation != work.invocation ||
                            source.work.operation_index != work.operation_index ||
                            source.work.frame_index != work.frame_index || source.owner != output.resource_owner ||
                            source.view_key != output.view_key || source.view_key != scene->view.id.value())
                            return aeg::foundation::invalid_state("native prepared source differs from its actual scoped View Feature work");
                        ++draws;
                    }
                    if (draws != 1)
                        return aeg::foundation::invalid_state("actual View did not retain exactly one live Feature geometry draw");
                    ++registered_scene.actual_view_draws;
                }
                registered_scene.actual_geometry_work = registered_scene.actual_view_draws == 2;
                ++smoke_state.readback_pass_build_count;
                return passes.unregister_pass(aeg::renderer::kForwardOpaquePassTypeId);
            }
            const auto built = aeg::render_graph::build_render_graph_features(context.render_graph, outputs,
                {.textures = context.feature_textures, .draw_lowerer = &lowerer}, context.frame_context.frame_number());
            if (!built) return built;
            const auto plan = context.render_graph.compile();
            if (!plan) return plan;
            unsigned draws = 0;
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(context.render_graph).pass_plans) {
                if (!pass.feature_work) continue;
                const auto& work = *pass.feature_work;
                if (work.operation_index != (neutral_clear ? 0u : 1u)) continue;
                const auto& output = outputs->front();
                if (work.feature_definition_id != output.contract.definition_id || work.node_id != output.node_id ||
                    work.owner_module != output.owner_module || work.instance_id != output.instance_id ||
                    work.invocation.invocation_id != output.invocation.invocation_id ||
                    work.invocation.generation != output.invocation.generation ||
                    work.frame_index != context.frame_context.frame_number())
                    return aeg::foundation::invalid_state("neutral draw lost Feature provenance");
                if (neutral_registered && pass.live && work.invocation == output.invocation) {
                    if (registered_scene.setup_sources.empty())
                        return aeg::foundation::invalid_state("registered Scene draw has no prepared source observation");
                    const auto& source = registered_scene.setup_sources.back();
                    if (source.work.id != work.id || source.work.feature_definition_id != work.feature_definition_id ||
                        source.work.owner_module != work.owner_module || source.work.node_id != work.node_id ||
                        source.work.instance_id != work.instance_id || source.work.invocation != output.invocation ||
                        source.work.operation_index != work.operation_index || source.work.frame_index != work.frame_index ||
                        source.owner != output.resource_owner || source.view_key != output.view_key)
                        return aeg::foundation::invalid_state("native prepared source differs from the actual captured Feature work");
                    registered_scene.actual_geometry_work = true;
                }
                ++draws;
            }
            if (draws != ((cold || cold_retry) ? 3u : 1u))
                return aeg::foundation::invalid_state("neutral draw/upload expansion lost provenance or retry uploads");
            const auto target = context.feature_textures.front().texture;
            const auto readback = smoke_state.readback;
            const aeg::render_graph::RenderGraphBuffer buffer{smoke_state.readback_resource.index,
                smoke_state.readback_resource.generation, smoke_state.readback_resource.graph_id};
            const auto pass = context.render_graph.add_pass("Neutral.Readback", aeg::rhi::QueueClass::kGraphics,
                [target, buffer, readback](aeg::render_graph::PassBuilder& p) {
                    if (const auto r = p.read(target, ResourceUsage::kCopySource, PipelineStage::kCopy); !r) return r;
                    if (const auto r = p.write(buffer, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                        ShaderStage::kAll, {readback.offset, kReadbackSize}); !r) return r;
                    p.set_side_effects();
                    return Result::success();
                }, [target, buffer, readback](aeg::render_graph::PassExecutionContext& command) {
                    return command.copy_texture_to_buffer(target, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {},
                        {kTargetWidth, kTargetHeight, 1}, buffer, readback.offset, 0, 0);
                });
            ++smoke_state.readback_pass_build_count;
            if (neutral_registered && pass)
                if (const auto removed = passes.unregister_pass(aeg::renderer::kForwardOpaquePassTypeId); !removed) return removed;
            return pass ? Result::success() : pass.result;
        };
    } else {
        const auto profile = deferred ? aeg::renderer::SceneRenderProfile::kDeferred
                           : forward ? aeg::renderer::SceneRenderProfile::kForward
                                     : aeg::renderer::SceneRenderProfile::kOpaque;
        if (!require(aeg::renderer::register_scene_render_profiles(feature_registry),
                "Scene provenance oracle profile registration failed")) return 72;
        const auto document = aeg::renderer::scene_render_profile_graph(profile);
        if (!require(document, "Scene provenance oracle graph missing")) return 72;
        auto compiled = aeg::feature::FeatureGraphCompiler{}.compile(document.value, feature_registry);
        if (!require(compiled, "Scene provenance oracle compile failed") ||
            !require(feature_runtime.publish_graph(document.value, compiled.value), "Scene provenance oracle publish failed") ||
            !require(feature_runtime.tick(aeg::renderer::kSceneRenderProfileClock), "Scene provenance oracle tick failed")) return 72;
        const auto captured = feature_runtime.capture_render_outputs(compiled.value);
        if (!require(captured && captured.value.size() == 1, "Scene provenance oracle capture failed")) return 72;
        const auto& output = captured.value.front();
        const auto* owner = feature_runtime.find_instance(output.instance_id);
        if (!require(owner && owner->last_invocation_committed &&
                owner->last_invocation_committed->attempt.token == output.invocation && owner->state.commit_count == 1 &&
                output.resource_owner.definition_digest == owner->definition_digest &&
                output.resource_owner.resource_epoch == owner->render_resource_epoch,
                "Scene provenance oracle did not capture an actual committed Runtime owner")) return 72;
        const auto outputs = std::make_shared<const std::vector<aeg::feature::RenderFeatureOutput>>(captured.value);
        raw_scene_outputs = outputs;
        frame_desc.render_feature_post_build = [outputs, &captured_scene_outputs, &captured_scene_view,
            &smoke_state, scene_pass_host](
            const aeg::engine::EngineFramePostBuildContext& context) {
            if (context.render_scenes.size() != 1 || !context.render_scenes.front()->is_valid())
                return aeg::foundation::invalid_state("Scene provenance oracle has no actual extracted View");
            captured_scene_view = context.render_scenes.front()->view.id;
            captured_scene_outputs = std::make_shared<const std::vector<aeg::feature::RenderFeatureOutput>>(
                std::vector{aeg::renderer::scope_render_feature_output(outputs->front(), captured_scene_view, true)});
            const std::array scenes{aeg::renderer::RenderFeatureSceneBinding{
                aeg::renderer::kRuntimeSceneResourceId, 1, {}, context.render_scenes.front(),
                context.render_resource_resolver}};
            const aeg::renderer::RenderFeatureDrawLowerer lowerer(scenes, scene_pass_host);
            const auto built = aeg::render_graph::build_render_graph_features(context.render_graph,
                captured_scene_outputs, {.textures = smoke_state.scene_textures, .draw_lowerer = &lowerer,
                    .current_frame_owner = &context.frame_context}, context.frame_context.frame_number());
            if (!built) return built;
            const auto target = smoke_state.scene_textures.front().texture;
            const auto readback = smoke_state.readback;
            const aeg::render_graph::RenderGraphBuffer buffer{smoke_state.readback_resource.index,
                smoke_state.readback_resource.generation, smoke_state.readback_resource.graph_id};
            const auto copied = context.render_graph.add_pass("ReadbackOpaque", QueueClass::kGraphics,
                [target, buffer, readback](aeg::render_graph::PassBuilder& pass) {
                    if (const auto result = pass.read(target, ResourceUsage::kCopySource, PipelineStage::kCopy); !result)
                        return result;
                    if (const auto result = pass.write(buffer, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                            ShaderStage::kAll, {readback.offset, kReadbackSize}); !result) return result;
                    pass.set_side_effects();
                    return Result::success();
                }, [target, buffer, readback](aeg::render_graph::PassExecutionContext& execution) {
                    return execution.copy_texture_to_buffer(target, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {},
                        {kTargetWidth, kTargetHeight, 1}, buffer, readback.offset, 0, 0);
                });
            if (!copied) return copied.result;
            ++smoke_state.readback_pass_build_count;
            return Result::success();
        };
    }
    const Result frame_result = orchestrator.run_frame(frame_desc);
    if (!frame_result) {
        std::cerr << "opaque smoke frame execution failed: " << frame_result.message << "\n";
        return 23;
    }
    if (!require(smoke_state.invalid_texture_binding_rejected && smoke_state.color_target_bound &&
                     smoke_state.render_view_received_output_extent && smoke_state.readback.is_valid() &&
                     smoke_state.readback_pass_build_count == 1,
                 "opaque smoke frame preparation or readback feature contract failed") ||
        !require(resolver.mesh_resolve_count >= (cold ? 1u : 2u) &&
                     resolver.material_resolve_count >= (cold ? 1u : 2u) &&
                     resolver.object_resolve_count == (cold ? 1u : neutral_registered ? 9u : neutral ? 8u : forward ? 2u : 1u),
                 "opaque smoke resource resolver did not serve extraction and graphics build")) {
        return 24;
    }

    if (!require(device->wait_idle(), "opaque smoke GPU wait failed")) {
        return 25;
    }
    std::array<std::byte, kReadbackSize> pixels{};
    if (!require(frame_context.read_readback(smoke_state.readback, pixels), "opaque smoke pixel readback failed")) {
        return 26;
    }
    const std::size_t center_pixel =
        (static_cast<std::size_t>(kTargetHeight / 2u) * kTargetWidth + kTargetWidth / 2u) * 4u;
    const unsigned center_red = std::to_integer<unsigned char>(pixels[center_pixel + 0]);
    const unsigned center_green = std::to_integer<unsigned char>(pixels[center_pixel + 1]);
    const unsigned center_blue = std::to_integer<unsigned char>(pixels[center_pixel + 2]);
    if (!require(backlit    ? (center_red >= 8u && center_red <= 12u && center_green >= 2u && center_green <= 5u &&
                               center_blue >= 1u && center_blue <= 4u)
                 : deferred ? (center_red >= 48u && center_red <= 69u && center_green >= 9u && center_green <= 18u &&
                               center_blue >= 1u && center_blue <= 4u)
                            : (center_red >= 80u && center_red <= 115u && center_green >= 25u && center_green <= 50u &&
                               center_blue >= 10u && center_blue <= 35u),
                 "opaque smoke center pixel does not contain the updated material output")) {
        return 27;
    }
    if (!neutral) {
        if (!require(captured_scene_outputs && captured_scene_outputs->size() == 1 && captured_scene_view.is_valid(),
                "actual Scene provenance oracle disappeared before geometry verification")) return 73;
        const auto& output = captured_scene_outputs->front();
        if (!require(output.view_key == captured_scene_view.value(), "actual Scene provenance oracle lost its extracted View"))
            return 73;
        const auto provenance = render_feature_test::check_scene_draw_provenance(
            render_graph, *captured_scene_outputs, frame_context.frame_number());
        if (!require(provenance && provenance.value != 0,
                "actual live Scene geometry differs from the committed projected Feature work")) return 73;
        unsigned geometry_passes = 0;
        for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(render_graph).pass_plans) {
            const auto inspected = aeg::render_graph::detail::RenderGraphExecution::inspect_pass(render_graph, pass.pass);
            if (!require(inspected && inspected.value.pass == pass.pass, "actual Scene Pass handle inspection failed")) return 73;
            const bool geometry = deferred
                ? (inspected.value.name == "Renderer.GBufferPass" || inspected.value.name == "Renderer.GBufferScene")
                : (inspected.value.name == "DrawOpaque" || inspected.value.name == "Renderer.ForwardOpaquePass");
            if (!geometry || !pass.live) continue;
            ++geometry_passes;
            if (!pass.feature_work) {
                std::cerr << "Actual live Scene geometry " << inspected.value.name
                          << " bypassed captured Feature work\n";
                return 73;
            }
            const auto& work = *pass.feature_work;
            const bool actual_operation = work.operation_index < output.intent.operations.size() &&
                (deferred ? std::holds_alternative<aeg::feature::RenderDrawGBufferScene>(output.intent.operations[work.operation_index])
                 : forward ? std::holds_alternative<aeg::feature::RenderDrawClusteredScene>(output.intent.operations[work.operation_index])
                           : std::holds_alternative<aeg::feature::RenderDrawScene>(output.intent.operations[work.operation_index]));
            if (!require(inspected.value.queue == QueueClass::kGraphics && actual_operation && work.id.is_valid() &&
                    work.kind == aeg::render_graph::RenderWorkItemKind::kExecute && work.domain == aeg::feature::FeatureDomain::kRender &&
                    work.feature_definition_id == output.contract.definition_id && work.owner_module == output.owner_module &&
                    work.node_id == output.node_id && work.instance_id == output.instance_id && work.invocation == output.invocation &&
                    work.frame_index == frame_context.frame_number(),
                    "actual live Scene geometry differs from its captured Feature definition/module/node/instance/token/frame/View"))
                return 73;
            const auto actual_source = [&](const aeg::render_graph::RenderSceneOperationSource& source) {
                return source.work.id == work.id && source.work.kind == work.kind && source.work.domain == work.domain &&
                    source.work.feature_definition_id == work.feature_definition_id && source.work.owner_module == work.owner_module &&
                    source.work.node_id == work.node_id && source.work.instance_id == work.instance_id &&
                    source.work.invocation == work.invocation && source.work.frame_index == work.frame_index &&
                    source.work.operation_index == work.operation_index && source.owner == output.resource_owner &&
                    source.view_key == captured_scene_view.value() &&
                    source.kind == (deferred ? aeg::render_graph::RenderSceneOperationKind::kDrawGBufferScene
                        : forward ? aeg::render_graph::RenderSceneOperationKind::kDrawClusteredScene
                                  : aeg::render_graph::RenderSceneOperationKind::kDrawScene);
            };
            if (!require(std::find_if(registered_scene.setup_sources.begin(), registered_scene.setup_sources.end(), actual_source) !=
                        registered_scene.setup_sources.end() &&
                    std::find_if(registered_scene.execute_sources.begin(), registered_scene.execute_sources.end(), actual_source) !=
                        registered_scene.execute_sources.end(),
                    "actual native Scene setup/execute lost the committed operation, owner or extracted View")) return 73;
        }
        if (!require(geometry_passes != 0, "actual live Scene geometry provenance check was vacuous")) return 73;
    }
    if (neutral_registered && !require(registered_scene.actual_geometry_work && registered_scene.setups != 0 &&
        registered_scene.executions != 0 && !registered_scene.setup_configurations.empty() &&
        !registered_scene.execute_configurations.empty(), "ActualFeatureSceneDraw bypassed native registry")) return 61;
    if (neutral_registered) {
        if (!require(smoke_state.resolved_view_ids.size() == 2 &&
                smoke_state.resolved_view_ids.front() == kViewId && smoke_state.resolved_view_ids.back() == kSecondViewId &&
                registered_scene.actual_view_draws == 2 && registered_scene.executions == 2 &&
                registered_scene.prepared_views.size() == 2 && registered_scene.view_setup_swaps == 1 &&
                registered_scene.view_execute_swaps == 2 && registered_scene.execute_configurations.size() == 2 &&
                registered_scene.execute_sources.size() == 2,
                "actual multi-View draws or prepared-data replacement rejection missing")) return 65;
        for (size_t index = 0; index < registered_scene.execute_configurations.size(); ++index) {
            const auto& configuration = registered_scene.execute_configurations[index];
            const auto& source = registered_scene.execute_sources[index];
            if (!require(configuration.type == aeg::renderer::kForwardOpaquePassTypeId && configuration.instance.is_valid() &&
                configuration.instance.value() != configuration.type.value() && configuration.queue == aeg::rhi::QueueClass::kGraphics &&
                configuration.enabled && configuration.parameters.empty() && configuration.bindings.empty() && !configuration.source.empty() &&
                source.kind == aeg::render_graph::RenderSceneOperationKind::kDrawScene &&
                source.owner.instance_id == source.work.instance_id && source.owner.node_id == source.work.node_id &&
                source.owner.definition_digest != 0 && source.owner.resource_epoch != 0 && source.work.operation_index == 1 &&
                source.view_key != 0,
                "actual native Scene configuration lost immutable operation identity")) return 62;
        }
        const auto& first_source = registered_scene.execute_sources.front();
        const auto& second_source = registered_scene.execute_sources.back();
        if (!require(first_source.view_key != second_source.view_key &&
                first_source.work.id == second_source.work.id && first_source.work.invocation == second_source.work.invocation &&
                first_source.owner == second_source.owner && first_source.work.operation_index == second_source.work.operation_index &&
                registered_scene.execute_configurations.front().instance != registered_scene.execute_configurations.back().instance,
                "same Feature operation did not receive distinct actual View native instances")) return 66;
        std::array<std::byte, kReadbackSize> second_pixels{};
        if (!require(frame_context.read_readback(smoke_state.second_view_readback, second_pixels),
                "actual second View pixel readback failed")) return 67;
        const auto second_red = std::to_integer<unsigned char>(second_pixels[center_pixel]);
        const auto second_green = std::to_integer<unsigned char>(second_pixels[center_pixel + 1]);
        const auto second_blue = std::to_integer<unsigned char>(second_pixels[center_pixel + 2]);
        if (!require(second_red >= 80u && second_red <= 115u && second_green >= 25u && second_green <= 50u &&
                second_blue >= 10u && second_blue <= 35u,
                "actual second View center pixel lacks the updated material draw")) return 68;
        for (const auto offset : {size_t{0}, size_t{(kTargetWidth - 1) * 4},
                                 size_t{(kTargetHeight - 1) * kTargetWidth * 4}, size_t{kReadbackSize - 4}})
            if (!require(second_pixels[offset] == std::byte{0} && second_pixels[offset + 1] == std::byte{0} &&
                    second_pixels[offset + 2] == std::byte{255} && second_pixels[offset + 3] == std::byte{255},
                    "actual second View draw discarded its loaded background")) return 69;
    }
    if (deferred) {
        if (!require(pixels[0] == std::byte{0} && pixels[1] == std::byte{0} && pixels[2] == std::byte{0} &&
                         pixels[3] == std::byte{0},
                     "deferred lighting must leave background transparent"))
            return 40;
        const auto stale = resolver.last_deferred_binding.prepare_inputs({});
        if (!require(!stale && stale.result.code == ResultCode::kInvalidState,
                     "deferred bindings accepted execution outside their frame"))
            return 41;
    }

    if (neutral) {
        for (const auto offset : {size_t{0}, size_t{(kTargetWidth - 1) * 4},
                                 size_t{(kTargetHeight - 1) * kTargetWidth * 4}, size_t{kReadbackSize - 4}})
            if (!require(pixels[offset] == std::byte{0} && pixels[offset + 1] == std::byte{0} &&
                         pixels[offset + 2] == std::byte{255} && pixels[offset + 3] == std::byte{255},
                         "neutral draw discarded loaded background")) return 53;
        frame_desc.render_feature_post_build = {};
        render_graph.reset();
        if (!require(!weak_outputs.expired() && !weak_scene.expired(), "neutral leases retired before GPU frame")) return 54;
        if (neutral_registered && !require(!registered_scene.prepared.expired() && !registered_scene.native_delegate.expired(),
                "resolved native Scene commands/pass retired before their GPU frame")) return 63;
        if (neutral_registered) {
            if (!require(!weak_second_outputs.expired() && !weak_second_scene.expired(),
                    "actual second View leases retired before their GPU frame")) return 70;
            for (const auto& prepared : registered_scene.prepared_views)
                if (!require(!prepared.data.expired(), "actual View prepared commands retired before their GPU frame")) return 70;
        }
        if (!require(frame_context.drain(), "neutral frame drain failed") ||
            !require(weak_outputs.expired() && weak_scene.expired(), "neutral leases survived frame retirement")) return 55;
        if (neutral_registered && !require(registered_scene.prepared.expired() && registered_scene.native_delegate.expired(),
                "resolved native Scene commands/pass survived frame retirement")) return 64;
        if (neutral_registered) {
            if (!require(weak_second_outputs.expired() && weak_second_scene.expired(),
                    "actual second View leases survived frame retirement")) return 71;
            for (const auto& prepared : registered_scene.prepared_views)
                if (!require(prepared.data.expired(), "actual View prepared commands survived frame retirement")) return 71;
        }
    }

    aeg::engine::EngineFrameDesc missing_resolver_desc = frame_desc;
    missing_resolver_desc.require_resolved_render_resources = false;
    missing_resolver_desc.render_resource_resolver = nullptr;
    missing_resolver_desc.render_feature_preparation = [target_texture, target_view, target_desc, depth_texture,
                                                        depth_view, depth_desc, &smoke_state, neutral, forward, deferred](
                                                           const aeg::engine::EngineFramePreparationContext& context) {
        const auto target_resource =
            context.render_graph.import_texture(target_texture, target_view, target_desc,
                                                ResourceState{ResourceUsage::kUndefined, ShaderStage::kAll,
                                                              QueueClass::kAuto, false, PipelineStage::kAllCommands});
        if (!target_resource) {
            return target_resource.result;
        }
        if (const auto r = context.bind_feature_texture(kOpaqueSceneColor, target_resource.value); !r)
            return r;
        const auto depth_resource = context.render_graph.import_texture(depth_texture, depth_view, depth_desc);
        if (!depth_resource)
            return depth_resource.result;
        if (const auto result = context.bind_feature_texture(RenderResourceId{"OpaqueSceneDepth"}, depth_resource.value); !result)
            return result;
        return neutral ? Result::success() : bind_scene_targets(context.render_graph, target_resource.value,
            depth_resource.value, target_desc, forward, deferred, smoke_state.scene_textures);
    };
    if (neutral) missing_resolver_desc.render_feature_post_build = [](const auto&) {
        return aeg::foundation::invalid_state("missing resolver");
    };
    const Result missing_resolver_result = orchestrator.run_frame(missing_resolver_desc);
    if (!require(!missing_resolver_result && missing_resolver_result.code == ResultCode::kInvalidState,
                 "opaque RenderFeature accepted a scene without a resource resolver")) {
        return 28;
    }

    // Retire descriptors from both compute and graphics before releasing resources.
    if (!require(frame_context.drain(), "frame retirement failed"))
        return 42;
    if (cold_assets && !require(cold_assets->retire_gpu_resources(), "cold asset retirement failed")) return 59;
    if (clustered_pipeline.is_valid() &&
        !require(device->destroy_pipeline(clustered_pipeline), "forward clustered cleanup failed"))
        return 47;
    if (unshadowed_clustered.pipeline.is_valid() &&
        !require(device->destroy_pipeline(unshadowed_clustered.pipeline), "forward unshadowed cleanup failed")) return 47;
    if (depth_pipeline.is_valid() && !require(device->destroy_pipeline(depth_pipeline), "forward depth cleanup failed"))
        return 33;
    if (gbuffer_pipeline.is_valid() &&
        !require(device->destroy_pipeline(gbuffer_pipeline), "deferred geometry cleanup failed"))
        return 37;
    if (!require(orchestrator.shutdown(), "opaque smoke orchestrator shutdown failed") ||
        !require(orchestrator.unregister_world_frame(world_scope.handle(), kViewId),
                 "opaque smoke World frame unregistration failed") ||
        (neutral_registered && !require(orchestrator.unregister_world_frame(world_scope.handle(), kSecondViewId),
                                        "opaque smoke second World frame View unregistration failed")) ||
        !require(orchestrator.unregister_world(world_scope.handle()), "opaque smoke World unregistration failed") ||
        !require(world_scope.shutdown(), "opaque smoke WorldScope shutdown failed") ||
        !require(renderer.unregister_extractor(extractor.value), "opaque smoke mesh extractor unregistration failed") ||
        !require(aeg::resource::destroy_texture_material(*device, smoke_state.material),
                 "opaque smoke material destruction failed") ||
        !require(aeg::resource::TextureUploader(*device).destroy_texture(smoke_state.material_texture),
                 "opaque smoke material texture destruction failed") ||
        !require(aeg::resource::TextureUploader(*device).destroy_texture(smoke_state.replacement_texture),
                 "opaque smoke replacement texture destruction failed") ||
        !require(device->destroy_buffer(smoke_state.material_parameter_buffer),
                 "opaque smoke material parameter buffer destruction failed") ||
        !require(device->destroy_binding_set(smoke_state.lighting_binding_set),
                 "opaque smoke lighting binding set destruction failed") ||
        !require(device->destroy_buffer(smoke_state.lighting_buffer),
                 "opaque smoke lighting buffer destruction failed") ||
        !require(device->destroy_binding_set(binding_set), "opaque smoke binding set destruction failed") ||
        !require(device->destroy_pipeline(graphics_pipeline), "opaque smoke pipeline destruction failed") ||
        !require(device->destroy_buffer(transform_buffer), "opaque smoke transform buffer destruction failed") ||
        !require(device->destroy_buffer(index_buffer), "opaque smoke index buffer destruction failed") ||
        !require(device->destroy_buffer(vertex_buffer), "opaque smoke vertex buffer destruction failed") ||
        !require(device->destroy_texture_view(target_view), "opaque smoke target view destruction failed") ||
        !require(device->destroy_texture(target_texture), "opaque smoke target texture destruction failed") ||
        !require(device->destroy_texture_view(depth_view), "opaque smoke depth view destruction failed") ||
        !require(device->destroy_texture(depth_texture), "opaque smoke depth texture destruction failed") ||
        !require(shader_manager.clear_layout_cache(), "opaque smoke shader layout cache destruction failed")) {
        return 29;
    }
    if (!cold_package.empty()) std::filesystem::remove(cold_package);
    return 0;
}
