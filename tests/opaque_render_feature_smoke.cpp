/**
 * @file opaque_render_feature_smoke.cpp
 * @brief Executes the Runtime MeshRenderer-to-opaque-draw path and verifies its pixels.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>

#include "Components/component_registration.hpp"
#include "Components/component_render_extraction.hpp"
#include "deferred_lighting_resources.hpp"
#include "engine_frame_orchestrator.hpp"
#include "light_culling.hpp"
#include "material.hpp"
#include "opaque_render_feature.hpp"
#include "pipeline.hpp"
#include "post_process.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "renderer_variants.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "shader_contract.hpp"
#include "texture_uploader.hpp"
#include "world_scope.hpp"

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::pipeline::PipelineId;
using aeg::pipeline::PipelineInstanceId;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::PipelineScope;
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

/** @brief Logical target identity shared by the opaque and readback RenderFeatures. */
inline constexpr RenderResourceId kOpaqueSceneColor{"OpaqueSceneColor"};
/** @brief Stable identity of the test-only readback RenderFeature. */
inline constexpr aeg::pipeline::FeatureId kOpaqueReadbackFeatureId{"Renderer.OpaqueReadbackSmokeFeature"};
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

/** @brief Stores frame-local RenderGraph resources shared with the test readback feature. */
struct OpaqueSmokeState final {
    FrameAllocation readback{};
    RenderGraphBuffer readback_resource{};
    bool invalid_texture_binding_rejected{false};
    bool color_target_bound{false};
    bool render_view_received_output_extent{false};
    uint32_t readback_feature_build_count{0};
    aeg::resource::GpuTexture material_texture{};
    aeg::resource::GpuTexture replacement_texture{};
    aeg::resource::TextureMaterial material{};
    BufferHandle material_parameter_buffer{};
    BufferDesc material_parameter_buffer_desc{};
    RenderGraphBuffer material_parameter_resource{};
    BufferHandle lighting_buffer{};
    BindingSetHandle lighting_binding_set{};
};

/** @brief Adds a graph-ordered readback pass immediately after the opaque graphics feature. */
class OpaqueReadbackFeature final : public aeg::pipeline::RenderFeature {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const aeg::pipeline::FeatureDescriptor kDescriptor{
            .id = kOpaqueReadbackFeatureId,
            .name = "Renderer.OpaqueReadbackSmokeFeature",
            .domain = aeg::pipeline::FeatureDomain::kRender,
            .domain_mask = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender),
            .supported_scopes = aeg::pipeline::to_mask(PipelineScope::kFrame),
            .version = {1, 0, 0},
        };
        return kDescriptor;
    }

    [[nodiscard]] Result build_render_graph(aeg::pipeline::RenderGraphBuilder& builder) const override {
        auto* state = static_cast<OpaqueSmokeState*>(builder.user_data());
        if (state == nullptr || !state->readback.is_valid() || !state->readback_resource.is_valid()) {
            return aeg::foundation::invalid_state("opaque smoke readback state is incomplete");
        }
        const aeg::foundation::ResultValue<RenderGraphTexture> target = builder.find_texture(kOpaqueSceneColor);
        if (!target) {
            return target.result;
        }

        const RenderGraphBuffer readback_resource = state->readback_resource;
        const FrameAllocation readback = state->readback;
        const aeg::foundation::ResultValue<aeg::pipeline::RenderGraphPass> pass = builder.add_graphics_pass(
            "ReadbackOpaque",
            [target = target.value, readback_resource, readback](aeg::pipeline::RenderPassBuilder& pass_builder) {
                if (const Result result =
                        pass_builder.read(target, ResourceUsage::kCopySource, PipelineStage::kCopy, ShaderStage::kAll);
                    !result) {
                    return result;
                }
                if (const Result result = pass_builder.write(
                        readback_resource, ResourceUsage::kCopyDestination, PipelineStage::kCopy, ShaderStage::kAll,
                        aeg::pipeline::RenderGraphBufferRange{readback.offset, kReadbackSize});
                    !result) {
                    return result;
                }
                pass_builder.set_side_effects();
                return Result::success();
            },
            [target = target.value, readback_resource, readback](aeg::pipeline::RenderPassExecutionContext& execution) {
                return execution.copy_texture_to_buffer(
                    target, TextureSubresourceLayers{aeg::rhi::Aspect::kColor, 0, 0, 1}, Offset3D{0, 0, 0},
                    Extent3D{kTargetWidth, kTargetHeight, 1}, readback_resource, readback.offset, 0, 0);
            });
        if (!pass) {
            return pass.result;
        }
        ++state->readback_feature_build_count;
        return Result::success();
    }

protected:
    [[nodiscard]] Result setup(aeg::pipeline::RenderGraphBuilder& builder) const override {
        const aeg::pipeline::PipelineStageHandle stage = builder.add_stage({
            .id = aeg::pipeline::StageId("Renderer.OpaqueReadbackSmokeFeature.Build"),
            .phase = aeg::pipeline::EnginePhase::kRenderBuild,
            .execution_domain = aeg::pipeline::ExecutionDomain::kRenderThread,
            .time_policy = aeg::pipeline::TimePolicy::kFrame,
            .parallelizable = false,
        });
        return stage ? Result::success()
                     : aeg::foundation::invalid_state("opaque smoke readback stage declaration failed");
    }
};

/** @brief Resolves the fixed test mesh, material pipeline, object constants, and frame lighting. */
class OpaqueSmokeResolver final : public aeg::renderer::RenderResourceResolver {
public:
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
        return {Result::success(), mesh_};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        aeg::renderer::MaterialInstanceId material) const override {
        if (material != material_id_) {
            return {aeg::foundation::Result::failure(ResultCode::kNotFound, "opaque smoke material id is unknown"), {}};
        }
        ++material_resolve_count;
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
    const bool forward = argc == 2 && std::string_view(argv[1]) == "--forward";
    const bool backlit = argc == 2 && std::string_view(argv[1]) == "--deferred-backlit";
    const bool deferred = backlit || (argc == 2 && std::string_view(argv[1]) == "--deferred");
    const PipelineId pipeline_id = deferred  ? aeg::renderer::kDeferredRenderPipelineId
                                   : forward ? aeg::renderer::kForwardRenderPipelineId
                                             : PipelineId("RenderPipeline");
    const aeg::pipeline::FeaturePointId draw_point(deferred  ? "Renderer.DeferredRenderPipeline.BeforePresent"
                                                   : forward ? "Renderer.ForwardRenderPipeline.BeforePresent"
                                                             : "RenderPipeline.Opaque");

    constexpr uint32_t kViewId = 3;
    const aeg::renderer::MeshAssetId mesh_id{0x1111ull};
    const aeg::renderer::MaterialInstanceId material_id{0x2222ull};

    PipelineRegistry registry;
    aeg::pipeline::RenderPassRegistry passes;
    if (!aeg::renderer::register_builtin_render_passes(passes))
        return 2;
    if (!require(deferred  ? aeg::renderer::register_deferred_render_pipeline(registry)
                 : forward ? aeg::renderer::register_forward_render_pipeline(registry)
                           : aeg::renderer::register_render_pipeline(registry),
                 "RenderPipeline registration failed") ||
        !require(deferred  ? aeg::renderer::register_deferred_render_feature(registry)
                 : forward ? aeg::renderer::register_forward_render_feature(registry)
                           : aeg::renderer::register_opaque_render_feature(registry),
                 "RenderFeature registration failed") ||
        !require(deferred  ? aeg::renderer::bind_deferred_render_feature(registry)
                 : forward ? aeg::renderer::bind_forward_render_feature(registry)
                           : aeg::renderer::bind_opaque_render_feature(registry),
                 "RenderFeature binding failed") ||
        !require(registry.register_feature([] { return std::make_unique<OpaqueReadbackFeature>(); }),
                 "opaque readback RenderFeature registration failed") ||
        !require(registry.bind_feature({
                     .feature = kOpaqueReadbackFeatureId,
                     .pipeline = pipeline_id,
                     .scope = PipelineScope::kFrame,
                     .point = draw_point,
                     .priority = 1,
                     .enabled = true,
                 }),
                 "opaque readback RenderFeature binding failed")) {
        return 1;
    }

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

    aeg::world::WorldScope world_scope(world_result.value, registry, {});
    if (!require(world_scope.initialize(), "opaque smoke WorldScope initialization failed")) {
        return 5;
    }
    aeg::pipeline::PipelineScopeHost frame_host(PipelineScope::kFrame,
                                                PipelineInstanceId("OpaqueRenderFeatureSmokeFrame"));
    if (!require(frame_host.bind_pipeline(pipeline_id), "opaque smoke pipeline binding failed") ||
        !require(frame_host.rebuild(registry), "opaque smoke frame pipeline rebuild failed")) {
        return 6;
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
    }
    PipelineHandle depth_pipeline{};
    PipelineHandle clustered_pipeline{};
    aeg::renderer::LightCullingResources culling_resources;
    if (forward) {
        const auto program = aeg::shader::load_shader_program(AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH);
        if (!require(program, "forward clustered shader load failed"))
            return 43;
        const auto layout = shader_manager.build_pipeline_layout(program.value);
        if (!require(layout, "forward clustered layout failed"))
            return 44;
        const std::array hdr_colors{aeg::renderer::kSceneLightingFormat};
        auto clustered_graphics = graphics_state;
        clustered_graphics.color_formats = hdr_colors;
        const auto pipeline = shader_manager.create_pipeline(
            program.value, layout.value,
            {PipelineType::kGraphics, clustered_graphics, DebugName{"ForwardSmoke.Clustered"}});
        if (!require(pipeline, "forward clustered pipeline failed"))
            return 45;
        clustered_pipeline = pipeline.value;
        const auto inputs = shader_manager.binding_layout(layout.value, 3);
        if (!require(inputs, "forward clustered bindings failed") ||
            !require(culling_resources.initialize(*device, AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH, inputs.value),
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

    aeg::pipeline::PipelineScheduler scheduler({1, 1, 1, 1});
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
    aeg::engine::EngineFrameOrchestrator orchestrator(registry, scheduler, frame_context, render_graph);
    if (!require(orchestrator.register_world_frame(world_scope, frame_host, kViewId),
                 "opaque smoke World frame registration failed") ||
        !require(orchestrator.initialize(), "opaque smoke orchestrator initialization failed")) {
        return 22;
    }

    OpaqueSmokeState smoke_state{};
    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.fixed_tick = 1;
    frame_desc.render_output_extent = {kTargetWidth, kTargetHeight};
    frame_desc.renderer = &renderer;
    frame_desc.render_resource_resolver = &resolver;
    frame_desc.render_passes = &passes;
    frame_desc.require_resolved_render_resources = true;
    frame_desc.user_data = &smoke_state;
    frame_desc.render_view_resolver = [&smoke_state](const aeg::engine::EngineFrameRenderViewContext& context)
        -> aeg::foundation::ResultValue<aeg::renderer::RenderView> {
        aeg::renderer::RenderView view{};
        view.id = aeg::renderer::make_render_view_id(context.world_snapshot.world(), context.view_id);
        view.world = context.world_snapshot.world();
        view.view_id = context.view_id;
        view.output_extent = context.output_extent;
        view.color_target = kOpaqueSceneColor;
        view.depth_target = RenderResourceId{"OpaqueSceneDepth"};
        smoke_state.render_view_received_output_extent =
            context.output_extent == aeg::renderer::RenderExtent{kTargetWidth, kTargetHeight};
        return {Result::success(), view};
    };
    frame_desc.render_feature_preparation = [&smoke_state, &resolver, device_ptr = device.get(),
                                             material_binding_layout, lighting_binding_layout, graphics_pipeline,
                                             target_texture, target_view, target_desc, depth_texture, depth_view,
                                             depth_desc, depth_pipeline, gbuffer_pipeline, clustered_pipeline,
                                             deferred](const aeg::engine::EngineFramePreparationContext& context) {
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
        material_resource.depth_pipeline = depth_pipeline;
        material_resource.gbuffer_pipeline = gbuffer_pipeline;
        material_resource.clustered_pipeline = clustered_pipeline;
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
        return Result::success();
    };
    const Result frame_result = orchestrator.run_frame(frame_desc);
    if (!frame_result) {
        std::cerr << "opaque smoke frame execution failed: " << frame_result.message << "\n";
        return 23;
    }
    if (!require(smoke_state.invalid_texture_binding_rejected && smoke_state.color_target_bound &&
                     smoke_state.render_view_received_output_extent && smoke_state.readback.is_valid() &&
                     smoke_state.readback_feature_build_count == 1,
                 "opaque smoke frame preparation or readback feature contract failed") ||
        !require(resolver.mesh_resolve_count >= 2 && resolver.material_resolve_count >= 2 &&
                     resolver.object_resolve_count == (forward ? 2u : 1u),
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

    aeg::engine::EngineFrameDesc missing_resolver_desc = frame_desc;
    missing_resolver_desc.require_resolved_render_resources = false;
    missing_resolver_desc.render_resource_resolver = nullptr;
    missing_resolver_desc.render_feature_preparation = [target_texture, target_view, target_desc, depth_texture,
                                                        depth_view, depth_desc](
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
        return context.bind_feature_texture(RenderResourceId{"OpaqueSceneDepth"}, depth_resource.value);
    };
    const Result missing_resolver_result = orchestrator.run_frame(missing_resolver_desc);
    if (!require(!missing_resolver_result && missing_resolver_result.code == ResultCode::kInvalidState,
                 "opaque RenderFeature accepted a scene without a resource resolver")) {
        return 28;
    }

    // Retire descriptors from both compute and graphics before releasing resources.
    if (!require(frame_context.drain(), "frame retirement failed"))
        return 42;
    if (clustered_pipeline.is_valid() &&
        !require(device->destroy_pipeline(clustered_pipeline), "forward clustered cleanup failed"))
        return 47;
    if (depth_pipeline.is_valid() && !require(device->destroy_pipeline(depth_pipeline), "forward depth cleanup failed"))
        return 33;
    if (gbuffer_pipeline.is_valid() &&
        !require(device->destroy_pipeline(gbuffer_pipeline), "deferred geometry cleanup failed"))
        return 37;
    if (!require(orchestrator.shutdown(), "opaque smoke orchestrator shutdown failed") ||
        !require(orchestrator.unregister_world_frame(world_scope.handle(), kViewId),
                 "opaque smoke World frame unregistration failed") ||
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
    return 0;
}
