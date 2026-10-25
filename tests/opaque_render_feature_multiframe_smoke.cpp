/**
 * @file opaque_render_feature_multiframe_smoke.cpp
 * @brief Verifies multi-entity materials, frame-versioned bindings, and readback pixels.
 */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "engine_frame_orchestrator.hpp"
#include "feature_render_test_fixture.hpp"
#include "feature_runtime.hpp"
#include "render_feature_draw.hpp"
#include "render_scene_provenance_fixture.hpp"
#include "native_scene_pass_source.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "../Engine/Pipeline/private/render_pass_registry_execution.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "material.hpp"
#include "pipeline.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "renderer_variants_internal.hpp"
#include "../Engine/RenderGraph/private/render_graph_resource_access.hpp"
#include "../Engine/Shader/private/shader_pipeline_layout_access.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "shader_contract.hpp"
#include "texture_uploader.hpp"
#include "world_scope.hpp"

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::RenderGraphBuffer;
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

inline constexpr RenderResourceId kSceneColor{"OpaqueMultiFrameSceneColor"};
constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr std::size_t kReadbackSize = static_cast<std::size_t>(kWidth) * kHeight * 4u;
constexpr uint32_t kMaterialCount = 3;
constexpr uint32_t kObjectCount = 4;
constexpr uint32_t kFrameCount = 3;

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

constexpr std::array<Vertex, 3> kTriangleVertices{{
    {-0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {0.75f, -0.75f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {0.0f, 0.75f, 0.0f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f},
}};
constexpr std::array<uint16_t, 3> kTriangleIndices{{0, 1, 2}};

struct MaterialFrameInput final {
    std::array<std::byte, 4> texture{};
    aeg::resource::TextureMaterialParameters parameters{};
};

constexpr auto kFrames = [] {
    std::array<std::array<MaterialFrameInput, kMaterialCount>, kFrameCount> frames{};
    frames[0][0] = {{{std::byte{200}, std::byte{40}, std::byte{40}, std::byte{255}}}, {{0.5f, 1.0f, 0.5f, 1.0f}}};
    frames[0][1] = {{{std::byte{40}, std::byte{200}, std::byte{40}, std::byte{255}}}, {{1.0f, 0.5f, 1.0f, 1.0f}}};
    frames[0][2] = {{{std::byte{40}, std::byte{40}, std::byte{200}, std::byte{255}}}, {{0.5f, 1.0f, 0.75f, 1.0f}}};
    frames[1][0] = {{{std::byte{240}, std::byte{80}, std::byte{30}, std::byte{255}}}, {{0.25f, 0.75f, 1.0f, 1.0f}}};
    frames[1][1] = {{{std::byte{30}, std::byte{220}, std::byte{120}, std::byte{255}}}, {{0.75f, 0.5f, 0.5f, 1.0f}}};
    frames[1][2] = {{{std::byte{180}, std::byte{30}, std::byte{220}, std::byte{255}}}, {{0.5f, 0.25f, 0.5f, 1.0f}}};
    frames[2][0] = {{{std::byte{60}, std::byte{200}, std::byte{220}, std::byte{255}}}, {{1.0f, 0.4f, 0.25f, 1.0f}}};
    frames[2][1] = {{{std::byte{220}, std::byte{100}, std::byte{30}, std::byte{255}}}, {{0.2f, 1.0f, 0.75f, 1.0f}}};
    frames[2][2] = {{{std::byte{100}, std::byte{220}, std::byte{70}, std::byte{255}}}, {{0.8f, 0.5f, 1.0f, 1.0f}}};
    return frames;
}();

struct MaterialState final {
    aeg::resource::GpuTexture texture{};
    aeg::resource::TextureMaterial material{};
    BufferHandle parameter_buffer{};
    BufferDesc parameter_buffer_desc{};
    RenderGraphBuffer parameter_resource{};
};

struct SmokeState final {
    std::array<MaterialState, kMaterialCount> materials{};
    std::array<FrameAllocation, kFrameCount> readbacks{};
    std::array<RenderGraphBuffer, kFrameCount> readback_resources{};
    uint32_t frame_index{0};
    uint32_t current_readback{0};
    uint32_t readback_build_count{0};
    bool missing_texture{false};
    BufferHandle lighting_buffer{};
    BindingSetHandle lighting_binding_set{};
};

template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

[[nodiscard]] ResourceState make_host_read_state() {
    return ResourceState{ResourceUsage::kHostRead, ShaderStage::kAll, QueueClass::kCopy, false, PipelineStage::kHost};
}

[[nodiscard]] BufferDesc make_readback_desc(uint64_t size) {
    BufferDesc desc{};
    desc.size = size;
    desc.capabilities = BufferCapability::kTransferSource | BufferCapability::kTransferDestination |
                        BufferCapability::kVertex | BufferCapability::kIndex | BufferCapability::kUniform;
    desc.memory = MemoryClass::kReadback;
    desc.initial_usage = ResourceUsage::kHostRead;
    desc.initial_stages = PipelineStage::kHost;
    desc.initial_queue = QueueClass::kCopy;
    desc.name = DebugName{"Renderer.OpaqueMultiFrame.Readback"};
    return desc;
}

[[nodiscard]] aeg::pipeline::RenderGraphBuffer to_pipeline_handle(
    aeg::render_graph::RenderGraphBuffer handle) noexcept {
    return {handle.index, handle.generation, handle.graph_id};
}

[[nodiscard]] aeg::pipeline::RenderGraphTexture to_pipeline_handle(
    aeg::render_graph::RenderGraphTexture handle) noexcept {
    return {handle.index, handle.generation, handle.graph_id};
}

struct NativeFrameObservation final {
    struct Prepared final {
        aeg::render_graph::RenderSceneOperationSource source{};
        std::weak_ptr<const aeg::pipeline::RenderPassRuntimeData> commands{};
    };
    std::vector<Prepared> prepared{};
    std::vector<aeg::render_graph::RenderSceneOperationSource> executed{};
    std::weak_ptr<const aeg::pipeline::RenderPass> delegate{};
    uint32_t stale_prepared_rejections{0};
};

class ObservedMultiFramePass final : public aeg::pipeline::RenderPass {
public:
    ObservedMultiFramePass(std::shared_ptr<const aeg::pipeline::RenderPass> delegate,
                          NativeFrameObservation& observation)
        : delegate_(std::move(delegate)), observation_(observation) {}

    [[nodiscard]] const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override {
        return delegate_->descriptor();
    }

    [[nodiscard]] Result setup(aeg::pipeline::RenderPassBuilder& builder,
                              const aeg::pipeline::RenderPassConfiguration& configuration) const override {
        const auto source = aeg::renderer::detail::NativeScenePassInspection::source(configuration);
        if (!source) return source.result;
        if (!observation_.prepared.empty()) {
            auto previous_frame = configuration;
            previous_frame.runtime_data = observation_.prepared.back().commands.lock();
            if (previous_frame.runtime_data) {
                const auto rejected = delegate_->setup(builder, previous_frame);
                if (rejected || rejected.code != ResultCode::kInvalidArgument)
                    return aeg::foundation::invalid_state("native multi-frame setup accepted previous-frame commands");
                ++observation_.stale_prepared_rejections;
            }
        }
        if (const auto result = delegate_->setup(builder, configuration); !result) return result;
        observation_.prepared.push_back({source.value, configuration.runtime_data});
        return Result::success();
    }

    [[nodiscard]] Result execute(aeg::pipeline::RenderPassExecutionContext& command,
                                const aeg::pipeline::RenderPassConfiguration& configuration) const override {
        const auto source = aeg::renderer::detail::NativeScenePassInspection::source(configuration);
        if (!source) return source.result;
        if (const auto result = delegate_->execute(command, configuration); !result) return result;
        observation_.executed.push_back(source.value);
        return Result::success();
    }

private:
    std::shared_ptr<const aeg::pipeline::RenderPass> delegate_;
    NativeFrameObservation& observation_;
};

class MultiMaterialResolver final : public aeg::renderer::RenderResourceResolver {
public:
    MultiMaterialResolver(Device& device, aeg::renderer::MeshAssetId mesh_id,
                          std::array<aeg::renderer::MaterialInstanceId, kMaterialCount> material_ids,
                          std::array<aeg::renderer::RenderObjectId, kObjectCount> object_ids,
                          aeg::renderer::RenderMeshResource mesh,
                          std::array<aeg::renderer::RenderObjectResource, kObjectCount> objects)
        : device_(&device),
          mesh_id_(mesh_id),
          material_ids_(material_ids),
          object_ids_(object_ids),
          mesh_(mesh),
          objects_(objects) {}

    void set_material(uint32_t index, aeg::renderer::RenderMaterialResource resource) noexcept {
        materials_[index] = resource;
    }

    void set_pipeline(PipelineHandle pipeline) noexcept {
        for (auto& material : materials_) {
            material.pipeline = pipeline;
        }
    }

    void set_missing_texture(bool missing) noexcept { missing_texture_ = missing; }
    void set_lighting(aeg::renderer::RenderLightingResource resource) const noexcept { lighting_ = resource; }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMeshResource> resolve_mesh(
        aeg::renderer::MeshAssetId mesh) const override {
        if (mesh != mesh_id_) {
            return {aeg::foundation::not_found("multi-material mesh id is unknown"), {}};
        }
        ++mesh_resolve_count;
        return {Result::success(), mesh_};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        aeg::renderer::MaterialInstanceId material) const override {
        for (uint32_t index = 0; index < kMaterialCount; ++index) {
            if (material != material_ids_[index]) {
                continue;
            }
            ++material_resolve_count;
            auto resource = materials_[index];
            if (missing_texture_) {
                resource.material_texture = {};
            }
            return {Result::success(), resource};
        }
        return {aeg::foundation::not_found("multi-material material id is unknown"), {}};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderObjectResource> resolve_object(
        const aeg::renderer::RenderInstance& instance, const aeg::renderer::RenderView& view) const override {
        if (!view.is_valid() || instance.mesh != mesh_id_) {
            return {aeg::foundation::invalid_argument("multi-material object input is invalid"), {}};
        }
        for (uint32_t index = 0; index < kObjectCount; ++index) {
            if (instance.object != object_ids_[index]) {
                continue;
            }
            const aeg::renderer::RenderObjectConstants constants{
                view.camera.view_projection() * instance.world_transform, aeg::math::identity()};
            const auto& object = objects_[index];
            const Result write_result = device_->write_buffer(object.transform_buffer, object.transform_offset,
                                                              std::as_bytes(std::span{&constants, 1}));
            if (!write_result) {
                return {write_result, {}};
            }
            ++object_resolve_count;
            return {Result::success(), object};
        }
        return {aeg::foundation::not_found("multi-material object id is unknown"), {}};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderLightingResource> resolve_lighting(
        const aeg::renderer::RenderSceneSnapshot&, const aeg::renderer::RenderView&) const override {
        return {Result::success(), lighting_};
    }

    mutable uint32_t mesh_resolve_count{0};
    mutable uint32_t material_resolve_count{0};
    mutable uint32_t object_resolve_count{0};

private:
    Device* device_{nullptr};
    aeg::renderer::MeshAssetId mesh_id_{};
    std::array<aeg::renderer::MaterialInstanceId, kMaterialCount> material_ids_{};
    std::array<aeg::renderer::RenderObjectId, kObjectCount> object_ids_{};
    aeg::renderer::RenderMeshResource mesh_{};
    std::array<aeg::renderer::RenderMaterialResource, kMaterialCount> materials_{};
    std::array<aeg::renderer::RenderObjectResource, kObjectCount> objects_{};
    mutable aeg::renderer::RenderLightingResource lighting_{};
    bool missing_texture_{false};
};

[[nodiscard]] Result add_scene_instances(const aeg::renderer::RenderExtractContext& context,
                                         aeg::renderer::MeshAssetId mesh_id,
                                         const std::array<aeg::renderer::MaterialInstanceId, kMaterialCount>& materials,
                                         const std::array<aeg::renderer::RenderObjectId, kObjectCount>& objects) {
    constexpr std::array<aeg::math::Vec3, kObjectCount> positions{
        aeg::math::Vec3{-0.55f, 0.45f, 0.0f},
        aeg::math::Vec3{-0.55f, -0.45f, 0.0f},
        aeg::math::Vec3{0.0f, 0.0f, 0.0f},
        aeg::math::Vec3{0.55f, 0.0f, 0.0f},
    };
    constexpr std::array<uint32_t, kObjectCount> material_indices{0, 0, 1, 2};
    for (uint32_t index = 0; index < kObjectCount; ++index) {
        const aeg::math::Mat4 transform = aeg::math::trs(positions[index], aeg::math::Quat{}, {0.28f, 0.28f, 1.0f});
        if (const Result result = context.builder.add_instance({objects[index],
                                                                mesh_id,
                                                                materials[material_indices[index]],
                                                                transform,
                                                                transform,
                                                                {},
                                                                1.0f,
                                                                0xffffffffu});
            !result) {
            return result;
        }
    }
    return Result::success();
}

[[nodiscard]] bool near_color(const std::array<std::byte, 4>& pixel, std::array<unsigned, 3> expected) {
    constexpr unsigned kTolerance = 12;
    for (uint32_t channel = 0; channel < 3; ++channel) {
        const unsigned actual = std::to_integer<unsigned char>(pixel[channel]);
        if (actual + kTolerance < expected[channel] || expected[channel] + kTolerance < actual) {
            return false;
        }
    }
    return std::to_integer<unsigned char>(pixel[3]) >= 240u;
}

[[nodiscard]] uint32_t count_color(const std::array<std::byte, kReadbackSize>& pixels, uint32_t x_begin, uint32_t x_end,
                                   std::array<unsigned, 3> expected) {
    uint32_t count = 0;
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = x_begin; x < x_end; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            const std::array<std::byte, 4> pixel{pixels[offset], pixels[offset + 1], pixels[offset + 2],
                                                 pixels[offset + 3]};
            count += near_color(pixel, expected) ? 1u : 0u;
        }
    }
    return count;
}

[[nodiscard]] std::array<unsigned, 3> expected_color(const MaterialFrameInput& input) {
    return {
        static_cast<unsigned>(std::to_integer<unsigned char>(input.texture[0]) * input.parameters.tint[0]),
        static_cast<unsigned>(std::to_integer<unsigned char>(input.texture[1]) * input.parameters.tint[1]),
        static_cast<unsigned>(std::to_integer<unsigned char>(input.texture[2]) * input.parameters.tint[2]),
    };
}
}  // namespace

int main() {
    const aeg::renderer::MeshAssetId mesh_id{"SharedTriangleMesh"};
    const std::array<aeg::renderer::MaterialInstanceId, kMaterialCount> material_ids{
        aeg::renderer::MaterialInstanceId{"SharedMaterial"}, aeg::renderer::MaterialInstanceId{"GreenMaterial"},
        aeg::renderer::MaterialInstanceId{"BlueMaterial"}};
    const std::array<aeg::renderer::RenderObjectId, kObjectCount> object_ids{
        aeg::renderer::RenderObjectId{1}, aeg::renderer::RenderObjectId{2}, aeg::renderer::RenderObjectId{3},
        aeg::renderer::RenderObjectId{4}};

    PipelineRegistry registry;
    aeg::pipeline::RenderPassRegistry passes;
    NativeFrameObservation native_frames;
    if (!require(aeg::renderer::register_builtin_render_passes(passes), "native pass registration failed")) {
        return 1;
    }
    {
        const auto real = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(
            passes, aeg::renderer::kForwardOpaquePassTypeId);
        if (!require(real, "native multi-frame pass lookup failed") ||
            !require(passes.unregister_pass(aeg::renderer::kForwardOpaquePassTypeId), "native pass replacement failed") ||
            !require(passes.register_pass([delegate = real.value, &native_frames] {
                return std::make_unique<ObservedMultiFramePass>(delegate, native_frames);
            }), "observed native pass registration failed")) return 1;
        native_frames.delegate = real.value;
    }

    aeg::ecs::WorldManager world_manager;
    const auto world_handle = world_manager.create_world({"OpaqueMultiFrameWorld", true});
    if (!require(world_handle, "world creation failed")) {
        return 2;
    }
    const auto world_result = world_manager.find(world_handle.value);
    if (!require(world_result, "world lookup failed")) {
        return 3;
    }
    if (!require(world_result.value->create_entity(), "dummy entity creation failed") ||
        !require(world_result.value->commit(1, 1), "world commit failed")) {
        return 4;
    }

    aeg::world::WorldScope world_scope(world_result.value);
    if (!require(world_scope.initialize(), "WorldScope initialization failed")) {
        return 5;
    }
    aeg::renderer::Renderer renderer;
    const auto extractor = renderer.register_extractor(
        [mesh_id, material_ids, object_ids](const aeg::renderer::RenderExtractContext& context) {
            return add_scene_instances(context, mesh_id, material_ids, object_ids);
        });
    if (!require(extractor, "scene extractor registration failed")) {
        return 7;
    }

    DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG Opaque Multi-Frame Smoke";
    device_desc.enable_validation = true;
    device_desc.enable_debug = true;
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "device creation failed")) {
        return 8;
    }
    std::unique_ptr<Device> device = std::move(device_result.value);

    auto shader_program_result = aeg::shader::load_shader_program(AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH);
    if (!require(shader_program_result, "shader package loading failed")) {
        return 9;
    }
    aeg::shader::ShaderManager shader_manager(*device);
    auto shader_layout_result = shader_manager.build_pipeline_layout(shader_program_result.value,
                                                                     DebugName{"Renderer.OpaqueMultiFrame.Layout"});
    if (!require(shader_layout_result, "shader layout creation failed")) {
        return 10;
    }
    aeg::shader::ShaderPipelineLayout shader_layout = std::move(shader_layout_result.value);
    const uint64_t material_pso_epoch = shader_manager.cache_epoch();
    const aeg::renderer::RenderMaterialShaderContract material_shader_contract{
        aeg::renderer::kRenderMaterialShaderContractVersion,
        aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(shader_layout),
        aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(shader_layout)};
    const auto object_layout_result = shader_manager.binding_layout(shader_layout, 0);
    const auto material_layout_result = shader_manager.binding_layout(shader_layout, 1);
    const auto lighting_layout_result = shader_manager.binding_layout(shader_layout, 2);
    if (!require(object_layout_result, "object layout lookup failed") ||
        !require(material_layout_result, "material layout lookup failed") ||
        !require(lighting_layout_result, "lighting layout lookup failed")) {
        return 11;
    }
    const BindingLayoutHandle object_layout = object_layout_result.value;
    const BindingLayoutHandle material_layout = material_layout_result.value;
    const BindingLayoutHandle lighting_layout = lighting_layout_result.value;

    TextureDesc target_desc{};
    target_desc.extent = {kWidth, kHeight, 1};
    target_desc.format = Format::kRgba8Unorm;
    target_desc.mip_levels = 1;
    target_desc.array_layers = 1;
    target_desc.capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
    target_desc.memory = MemoryClass::kDeviceLocal;
    target_desc.initial_usage = ResourceUsage::kUndefined;
    target_desc.initial_stages = PipelineStage::kAllCommands;
    target_desc.initial_queue = QueueClass::kAuto;
    target_desc.name = DebugName{"Renderer.OpaqueMultiFrame.Target"};
    const auto target_result = device->create_texture(target_desc);
    if (!require(target_result, "target texture creation failed")) {
        return 12;
    }
    TextureHandle target_texture = target_result.value;
    const auto target_view_result = device->create_texture_view({
        .texture = target_texture,
        .kind = TextureViewKind::kColorAttachment,
        .dimension = TextureViewDimension::kTex2D,
        .format = target_desc.format,
        .subresources = {aeg::rhi::Aspect::kColor, 0, 1, 0, 1},
        .name = DebugName{"Renderer.OpaqueMultiFrame.TargetView"},
    });
    if (!require(target_view_result, "target view creation failed")) {
        return 13;
    }
    TextureViewHandle target_view = target_view_result.value;

    BufferDesc vertex_desc{};
    vertex_desc.size = sizeof(kTriangleVertices);
    vertex_desc.capabilities = BufferCapability::kVertex;
    vertex_desc.memory = MemoryClass::kUpload;
    vertex_desc.initial_usage = ResourceUsage::kVertexInput;
    vertex_desc.initial_stages = PipelineStage::kVertexInput;
    vertex_desc.initial_queue = QueueClass::kGraphics;
    vertex_desc.name = DebugName{"Renderer.OpaqueMultiFrame.Vertex"};
    const auto vertex_result = device->create_buffer(vertex_desc);
    if (!require(vertex_result, "vertex buffer creation failed") ||
        !require(device->write_buffer(vertex_result.value, 0, std::as_bytes(std::span{kTriangleVertices})),
                 "vertex buffer upload failed")) {
        return 14;
    }
    BufferHandle vertex_buffer = vertex_result.value;

    BufferDesc index_desc{};
    index_desc.size = sizeof(kTriangleIndices);
    index_desc.capabilities = BufferCapability::kIndex;
    index_desc.memory = MemoryClass::kUpload;
    index_desc.initial_usage = ResourceUsage::kIndexInput;
    index_desc.initial_stages = PipelineStage::kVertexInput;
    index_desc.initial_queue = QueueClass::kGraphics;
    index_desc.name = DebugName{"Renderer.OpaqueMultiFrame.Index"};
    const auto index_result = device->create_buffer(index_desc);
    if (!require(index_result, "index buffer creation failed") ||
        !require(device->write_buffer(index_result.value, 0, std::as_bytes(std::span{kTriangleIndices})),
                 "index buffer upload failed")) {
        return 15;
    }
    BufferHandle index_buffer = index_result.value;

    std::array<BufferHandle, kObjectCount> transform_buffers{};
    std::array<aeg::renderer::RenderObjectResource, kObjectCount> object_resources{};
    for (uint32_t index = 0; index < kObjectCount; ++index) {
        BufferDesc transform_desc{};
        transform_desc.size = sizeof(aeg::renderer::RenderObjectConstants);
        transform_desc.capabilities = BufferCapability::kUniform;
        transform_desc.memory = MemoryClass::kUpload;
        transform_desc.initial_usage = ResourceUsage::kUniformRead;
        transform_desc.initial_stages = PipelineStage::kVertexShader;
        transform_desc.initial_queue = QueueClass::kGraphics;
        transform_desc.name = DebugName{"Renderer.OpaqueMultiFrame.Transform"};
        const auto transform_result = device->create_buffer(transform_desc);
        if (!require(transform_result, "transform buffer creation failed")) {
            return 16;
        }
        transform_buffers[index] = transform_result.value;
        const auto binding_result = device->create_binding_set(BindingSetDesc{
            object_layout, BindingSetLifetime::kPersistent, DebugName{"Renderer.OpaqueMultiFrame.Object"}});
        if (!require(binding_result, "object binding set creation failed")) {
            return 17;
        }
        const BindingSetHandle object_set = binding_result.value;
        aeg::rhi::BindingWrite write{};
        write.location = {0, 0};
        write.resource = BindingResource{};
        write.resource.type = BindingType::kUniformBuffer;
        write.resource.buffer_range =
            BufferRange{transform_buffers[index], 0, sizeof(aeg::renderer::RenderObjectConstants)};
        if (!require(device->update_binding_set(object_set, std::span{&write, 1}),
                     "object binding set update failed")) {
            return 18;
        }
        object_resources[index] = {transform_buffers[index],
                                   transform_desc,
                                   object_layout,
                                   object_set,
                                   0,
                                   0,
                                   sizeof(aeg::renderer::RenderObjectConstants)};
    }

    const std::array<VertexBindingDesc, 1> vertex_bindings{
        VertexBindingDesc{0, sizeof(Vertex), VertexInputRate::kVertex}};
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
    graphics_state.color_formats = color_formats;
    const aeg::renderer::RenderMaterialAttachmentContract material_attachment_contract{
        graphics_state.color_formats[0], graphics_state.samples, graphics_state.depth_stencil_format};
    const auto pipeline_result = shader_manager.create_pipeline(
        shader_program_result.value, shader_layout,
        aeg::shader::ShaderPipelineDesc{PipelineType::kGraphics, graphics_state,
                                        DebugName{"Renderer.OpaqueMultiFrame.Pipeline"}});
    if (!require(pipeline_result, "graphics pipeline creation failed")) {
        return 19;
    }
    PipelineHandle graphics_pipeline = pipeline_result.value;

    aeg::renderer::RenderMeshResource mesh_resource{vertex_buffer,
                                                    vertex_desc,
                                                    index_buffer,
                                                    index_desc,
                                                    sizeof(Vertex),
                                                    static_cast<uint32_t>(kTriangleVertices.size()),
                                                    static_cast<uint32_t>(kTriangleIndices.size()),
                                                    aeg::rhi::IndexFormat::kUint16,
                                                    {.position_offset = 0,
                                                     .position_format = Format::kRgba32Float,
                                                     .uv_offset = sizeof(float) * 4u,
                                                     .uv_format = Format::kRg32Float,
                                                     .normal_offset = sizeof(float) * 6u,
                                                     .normal_format = Format::kRgb32Float}};
    MultiMaterialResolver resolver(*device, mesh_id, material_ids, object_ids, mesh_resource, object_resources);
    resolver.set_pipeline(graphics_pipeline);

    // Every frame projects a fresh committed AtomicFeature invocation into the
    // registered native pass using the actual current Renderer snapshot.
    aeg::feature::FeatureRegistry feature_registry{aeg::feature::test::clock_test_host()};
    aeg::feature::FeatureRuntime feature_runtime(feature_registry);
    auto scene_feature = render_feature_test::definition();
    scene_feature.id = aeg::feature::FeatureDefinitionId("Renderer.MultiFrame.SceneDraw");
    scene_feature.contract.definition_id = scene_feature.id;
    scene_feature.owner_module = aeg::feature::ModuleIdentity("Renderer.MultiFrame.Fixture");
    scene_feature.execute = [](const aeg::feature::InvocationToken& token, const aeg::feature::StateSnapshot&,
        std::span<const uint8_t>, std::span<const uint8_t>) {
        using namespace aeg::feature;
        const RenderTextureReference target{RenderResourceId(kSceneColor.value()), 1, true};
        StateDelta delta{token.state_version};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {RenderDrawScene{{RenderResourceId("MultiFrame.Scene"), 1}, target,
            RenderAttachmentLoad::kClear}};
        delta.render_intent->final_output = target;
        return aeg::foundation::ResultValue<StateDelta>{Result::success(), std::move(delta)};
    };
    if (!require(aeg::feature::test::register_clock_test_definition(feature_registry, scene_feature), "actual Scene Feature registration failed")) return 39;
    auto feature_document = render_feature_test::document(scene_feature);
    feature_document.nodes.resize(1);
    auto feature_plan = aeg::feature::FeatureGraphCompiler{}.compile(feature_document, feature_registry);
    if (!require(feature_plan, "actual Scene Feature compile failed") ||
        !require(feature_runtime.publish_graph(feature_document, feature_plan.value), "actual Scene Feature publication failed")) return 39;
    std::shared_ptr<const std::vector<aeg::feature::RenderFeatureOutput>> current_outputs;
    aeg::renderer::RenderViewId current_view{};
    std::vector<aeg::feature::InvocationToken> committed_tokens;
    std::vector<std::weak_ptr<const std::vector<aeg::feature::RenderFeatureOutput>>> submitted_outputs;
    std::vector<std::weak_ptr<const aeg::renderer::RenderSceneSnapshot>> submitted_scenes;

    aeg::pipeline::PipelineScheduler scheduler;
    aeg::engine::EngineFramePipelineHost pipeline_host(registry, scheduler);
    auto frame_context = std::make_unique<aeg::render_graph::FrameContext>(
        *device, aeg::render_graph::FrameContextDesc{4, 4u * 1024u * 1024u, 4u * 1024u * 1024u, 256,
                                                     std::numeric_limits<uint64_t>::max()});
    aeg::render_graph::RenderGraph render_graph(*device);
    std::optional<aeg::engine::EngineFrameOrchestrator> orchestrator;
    orchestrator.emplace(pipeline_host, *frame_context, render_graph);
    if (!require(orchestrator->register_world_frame(world_scope, 1),
                 "world frame registration failed") ||
        !require(orchestrator->initialize(), "orchestrator initialization failed")) {
        return 20;
    }

    SmokeState smoke_state{};
    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.fixed_tick = 1;
    frame_desc.render_output_extent = {kWidth, kHeight};
    frame_desc.renderer = &renderer;
    frame_desc.render_resource_resolver = &resolver;
    frame_desc.require_resolved_render_resources = true;
    frame_desc.user_data = &smoke_state;
    frame_desc.render_view_resolver = [](const aeg::engine::EngineFrameRenderViewContext& context)
        -> aeg::foundation::ResultValue<aeg::renderer::RenderView> {
        aeg::renderer::RenderView view{};
        view.id = aeg::renderer::make_render_view_id(context.world_snapshot.world(), context.view_id);
        view.world = context.world_snapshot.world();
        view.view_id = context.view_id;
        view.output_extent = context.output_extent;
        view.camera.view = aeg::math::identity();
        view.camera.projection = aeg::math::identity();
        view.color_target = kSceneColor;
        return {Result::success(), view};
    };
    frame_desc.render_feature_preparation = [&smoke_state, &resolver, device_ptr = device.get(), material_layout,
                                             lighting_layout, graphics_pipeline, target_texture, target_view,
                                             target_desc, material_shader_contract, material_attachment_contract,
                                             material_pso_epoch](const aeg::engine::EngineFramePreparationContext& context) {
        const auto target_resource =
            context.render_graph.import_texture(target_texture, target_view, target_desc,
                                                ResourceState{ResourceUsage::kUndefined, ShaderStage::kAll,
                                                              QueueClass::kAuto, false, PipelineStage::kAllCommands});
        if (!target_resource) {
            return target_resource.result;
        }
        if (const Result result = context.bind_feature_texture(kSceneColor, target_resource.value); !result) {
            return result;
        }
        if (smoke_state.missing_texture) {
            return Result::success();
        }

        const uint32_t frame_index = smoke_state.frame_index < kFrameCount ? smoke_state.frame_index : kFrameCount - 1;
        aeg::resource::TextureUploader texture_uploader(*device_ptr);
        aeg::resource::ResourceUploader resource_uploader(*device_ptr);
        for (uint32_t material_index = 0; material_index < kMaterialCount; ++material_index) {
            const MaterialFrameInput& input = kFrames[frame_index][material_index];
            const aeg::resource::TextureSubresourceData subresource{
                std::span<const std::byte>{input.texture.data(), input.texture.size()}, 0, 0};
            TextureDesc texture_desc{};
            texture_desc.extent = {1, 1, 1};
            texture_desc.format = Format::kRgba8Unorm;
            texture_desc.mip_levels = 1;
            texture_desc.array_layers = 1;
            texture_desc.capabilities = TextureCapability::kSampled;
            texture_desc.memory = MemoryClass::kDeviceLocal;
            texture_desc.name = DebugName{"Renderer.OpaqueMultiFrame.MaterialTexture"};
            const aeg::resource::TextureData texture_data{
                texture_desc, std::span<const aeg::resource::TextureSubresourceData>{&subresource, 1}};
            const auto texture_upload = texture_uploader.upload_texture(
                context.frame_context, context.render_graph, texture_data, "Renderer.OpaqueMultiFrame.Texture");
            if (!texture_upload) {
                return texture_upload.result;
            }

            BufferDesc parameter_desc{};
            parameter_desc.size = sizeof(input.parameters);
            parameter_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kUniform;
            parameter_desc.memory = MemoryClass::kDeviceLocal;
            parameter_desc.initial_usage = ResourceUsage::kUndefined;
            parameter_desc.initial_stages = PipelineStage::kAllCommands;
            parameter_desc.initial_queue = QueueClass::kAuto;
            parameter_desc.name = DebugName{"Renderer.OpaqueMultiFrame.Parameters"};
            const auto parameter_upload = resource_uploader.upload_buffer(
                context.frame_context, context.render_graph,
                aeg::resource::BufferUploadDesc{std::as_bytes(std::span{&input.parameters, 1}), parameter_desc, 0, 16});
            if (!parameter_upload) {
                return parameter_upload.result;
            }

            auto& state = smoke_state.materials[material_index];
            if (smoke_state.frame_index == 0) {
                aeg::rhi::SamplerDesc sampler_desc{};
                sampler_desc.min_filter = aeg::rhi::Filter::kNearest;
                sampler_desc.mag_filter = aeg::rhi::Filter::kNearest;
                sampler_desc.mip_filter = aeg::rhi::Filter::kNearest;
                sampler_desc.address_u = aeg::rhi::AddressMode::kClampToEdge;
                sampler_desc.address_v = aeg::rhi::AddressMode::kClampToEdge;
                sampler_desc.address_w = aeg::rhi::AddressMode::kClampToEdge;
                sampler_desc.max_lod = 0.0f;
                sampler_desc.name = DebugName{"Renderer.OpaqueMultiFrame.Sampler"};
                const auto material_result = aeg::resource::create_texture_material(
                    *device_ptr, aeg::resource::TextureMaterialDesc{texture_upload.value.texture.sampled_view,
                                                                    sampler_desc, material_layout, 1,
                                                                    DebugName{"Renderer.OpaqueMultiFrame.Material"}});
                if (!material_result) {
                    return material_result.result;
                }
                state.material = material_result.value;
                if (const Result result = aeg::resource::update_texture_material_parameters(
                        *device_ptr, state.material,
                        BufferRange{parameter_upload.value.destination, 0, sizeof(input.parameters)});
                    !result) {
                    return result;
                }
            } else {
                const aeg::resource::GpuTexture old_texture = state.texture;
                const BufferHandle old_parameter_buffer = state.parameter_buffer;
                const BindingSetHandle old_material_set = state.material.binding_set;
                if (const Result result = aeg::resource::update_texture_material_for_frame(
                        *device_ptr, context.frame_context, state.material, texture_upload.value.texture.sampled_view);
                    !result) {
                    return result;
                }
                const BindingSetHandle texture_material_set = state.material.binding_set;
                if (const Result result = aeg::resource::update_texture_material_parameters_for_frame(
                        *device_ptr, context.frame_context, state.material,
                        BufferRange{parameter_upload.value.destination, 0, sizeof(input.parameters)});
                    !result) {
                    return result;
                }
                if (old_material_set == texture_material_set || texture_material_set == state.material.binding_set ||
                    state.material.texture_view != texture_upload.value.texture.sampled_view ||
                    state.material.parameter_buffer.buffer != parameter_upload.value.destination)
                    return aeg::foundation::invalid_state("frame material update reused a submitted descriptor version");
                aeg::resource::GpuTexture retired_texture = old_texture;
                if (const Result result =
                        texture_uploader.defer_destroy_texture(context.frame_context, retired_texture);
                    !result) {
                    return result;
                }
                if (const Result result = context.frame_context.defer_destroy_buffer(old_parameter_buffer); !result) {
                    return result;
                }
            }

            state.texture = texture_upload.value.texture;
            state.parameter_buffer = parameter_upload.value.destination;
            state.parameter_buffer_desc = parameter_desc;
            state.parameter_resource = to_pipeline_handle(parameter_upload.value.target);

            aeg::renderer::RenderMaterialResource material_resource{
                graphics_pipeline, state.material.binding_set, state.material.binding_group,
                to_pipeline_handle(texture_upload.value.target), state.parameter_buffer,
                state.parameter_buffer_desc, state.parameter_resource, 0, sizeof(input.parameters)};
            material_resource.shader_contract = material_shader_contract;
            material_resource.attachment_contract = material_attachment_contract;
            material_resource.variant_cache_epoch = material_pso_epoch;
            resolver.set_material(material_index, material_resource);
        }

        const aeg::renderer::RenderLightingConstants lighting_constants{
            .directional_direction_intensity = {0.0f, 0.0f, -1.0f, 0.0f},
            .directional_color = {0.0f, 0.0f, 0.0f, 1.0f},
            .ambient_color = {1.0f, 1.0f, 1.0f, 1.0f},
        };
        BufferDesc lighting_desc{};
        lighting_desc.size = sizeof(lighting_constants);
        lighting_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kUniform;
        lighting_desc.memory = MemoryClass::kDeviceLocal;
        lighting_desc.initial_usage = ResourceUsage::kUndefined;
        lighting_desc.initial_stages = PipelineStage::kAllCommands;
        lighting_desc.initial_queue = QueueClass::kAuto;
        lighting_desc.name = DebugName{"Renderer.OpaqueMultiFrame.Lighting"};
        const auto lighting_upload = resource_uploader.upload_buffer(
            context.frame_context, context.render_graph,
            aeg::resource::BufferUploadDesc{std::as_bytes(std::span{&lighting_constants, 1}), lighting_desc, 0, 16});
        if (!lighting_upload) {
            return lighting_upload.result;
        }
        const auto lighting_set_result = device_ptr->create_binding_set(BindingSetDesc{
            lighting_layout, BindingSetLifetime::kPersistent, DebugName{"Renderer.OpaqueMultiFrame.LightingSet"}});
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
        const BufferHandle previous_lighting_buffer = smoke_state.lighting_buffer;
        const BindingSetHandle previous_lighting_set = smoke_state.lighting_binding_set;
        smoke_state.lighting_buffer = lighting_upload.value.destination;
        smoke_state.lighting_binding_set = lighting_set_result.value;
        resolver.set_lighting(aeg::renderer::RenderLightingResource{
            .binding_set = lighting_set_result.value,
            .binding_group = 2,
            .lighting_buffer = lighting_upload.value.destination,
            .lighting_buffer_desc = lighting_desc,
            .lighting_resource = to_pipeline_handle(lighting_upload.value.target),
            .lighting_offset = 0,
            .lighting_size = sizeof(lighting_constants),
        });
        if (previous_lighting_set.is_valid()) {
            if (const Result result = context.frame_context.defer_destroy_binding_set(previous_lighting_set); !result) {
                return result;
            }
        }
        if (previous_lighting_buffer.is_valid()) {
            if (const Result result = context.frame_context.defer_destroy_buffer(previous_lighting_buffer); !result) {
                return result;
            }
        }

        const auto readback = context.frame_context.allocate_readback(kReadbackSize, 256);
        if (!readback) {
            return readback.result;
        }
        const auto readback_resource = context.render_graph.import_buffer(
            readback.value.buffer, make_readback_desc(readback.value.buffer_size), make_host_read_state());
        if (!readback_resource) {
            return readback_resource.result;
        }
        smoke_state.current_readback = smoke_state.frame_index;
        smoke_state.readbacks[smoke_state.frame_index] = readback.value;
        smoke_state.readback_resources[smoke_state.frame_index] = to_pipeline_handle(readback_resource.value);
        return Result::success();
    };

    frame_desc.render_feature_post_build = [&](const aeg::engine::EngineFramePostBuildContext& context) {
        if (context.render_scenes.size() != 1 || !context.render_scenes.front()->is_valid() ||
            context.render_scenes.front()->frame_number != context.frame_context.frame_number() ||
            context.render_scenes.front()->instances.size() != kObjectCount)
            return aeg::foundation::invalid_state("actual current Renderer snapshot is missing");
        current_view = context.render_scenes.front()->view.id;
        if (const auto result = feature_runtime.tick(scene_feature.contract.clock.id); !result) return result;
        const auto captured = feature_runtime.capture_render_outputs(feature_plan.value);
        if (!captured) return captured.result;
        if (captured.value.size() != 1) return aeg::foundation::invalid_state("actual Scene Feature capture has another owner count");
        const auto& captured_output = captured.value.front();
        const auto* owner = feature_runtime.find_instance(captured_output.instance_id);
        const auto provenance = std::find_if(feature_plan.value.provenance.begin(), feature_plan.value.provenance.end(),
            [&](const auto& entry) { return entry.instance_id == captured_output.instance_id; });
        if (!owner || !owner->last_invocation_committed ||
            owner->last_invocation_committed->attempt.token != captured_output.invocation ||
            owner->state.commit_count != committed_tokens.size() + 1 ||
            captured_output.resource_owner.definition_digest != owner->definition_digest ||
            captured_output.resource_owner.resource_epoch != owner->render_resource_epoch ||
            provenance == feature_plan.value.provenance.end() ||
            provenance->definition_id != captured_output.contract.definition_id ||
            provenance->owner_module != captured_output.owner_module || provenance->node_id != captured_output.node_id ||
            !captured_output.invocation.invocation_id.is_valid() ||
            std::find(committed_tokens.begin(), committed_tokens.end(), captured_output.invocation) != committed_tokens.end())
            return aeg::foundation::invalid_state("multi-frame capture has no fresh actual committed Feature owner");
        committed_tokens.push_back(captured_output.invocation);
        const std::vector<aeg::feature::RenderFeatureOutput> scoped{
            aeg::renderer::scope_render_feature_output(captured_output, current_view, true)};
        current_outputs = std::make_shared<const std::vector<aeg::feature::RenderFeatureOutput>>(scoped);
        const auto& output = current_outputs->front();
        const auto& draw = std::get<aeg::feature::RenderDrawScene>(output.intent.operations.front());
        const std::array scenes{aeg::renderer::RenderFeatureSceneBinding{
            draw.scene.id, draw.scene.version, output.instance_id, context.render_scenes.front(),
            context.render_resource_resolver}};
        const aeg::render_graph::RenderPassExecutionHost pass_host{
            &passes, aeg::pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)};
        aeg::renderer::RenderFeatureDrawLowerer lowerer(scenes, pass_host);
        if (const auto result = aeg::render_graph::build_render_graph_features(context.render_graph, current_outputs,
                {.textures = context.feature_textures, .draw_lowerer = &lowerer}, context.frame_context.frame_number());
            !result) return result;

        const auto target = std::find_if(context.feature_textures.begin(), context.feature_textures.end(),
            [&](const auto& binding) { return binding.id == draw.target.id && binding.version == draw.target.version; });
        if (target == context.feature_textures.end() || smoke_state.current_readback >= kFrameCount)
            return aeg::foundation::invalid_state("multi-frame readback target is missing");
        const auto readback = smoke_state.readbacks[smoke_state.current_readback];
        const auto resource = smoke_state.readback_resources[smoke_state.current_readback];
        const aeg::render_graph::RenderGraphBuffer buffer{resource.index, resource.generation, resource.graph_id};
        // Allocation frame_number is the zero-based slot generation; the
        // active FrameContext exposes the next, one-based frame number.
        if (!readback.is_valid() || readback.frame_number + 1 != context.frame_context.frame_number() ||
            readback.frame_slot != context.frame_context.frame_slot_index() ||
            !aeg::render_graph::detail::RenderGraphResourceAccess::contains_buffer(context.render_graph, buffer))
            return aeg::foundation::invalid_state("multi-frame readback allocation is stale");
        const auto copied = context.render_graph.add_pass("ReadbackOpaqueMultiFrame", QueueClass::kGraphics,
            [target = target->texture, buffer, readback](aeg::render_graph::PassBuilder& pass) {
                if (const auto result = pass.read(target, ResourceUsage::kCopySource, PipelineStage::kCopy); !result)
                    return result;
                if (const auto result = pass.write(buffer, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                        ShaderStage::kAll, {readback.offset, kReadbackSize}); !result) return result;
                pass.set_side_effects();
                return Result::success();
            }, [target = target->texture, buffer, readback](aeg::render_graph::PassExecutionContext& command) {
                return command.copy_texture_to_buffer(target, TextureSubresourceLayers{aeg::rhi::Aspect::kColor, 0, 0, 1},
                    Offset3D{0, 0, 0}, Extent3D{kWidth, kHeight, 1}, buffer, readback.offset, 0, 0);
            });
        if (!copied) return copied.result;
        ++smoke_state.readback_build_count;
        submitted_outputs.emplace_back(current_outputs);
        submitted_scenes.emplace_back(context.render_scenes.front());
        return Result::success();
    };
    const auto actual_geometry_has_owner = [&] {
        if (orchestrator->draw_call_count() < kObjectCount || !current_outputs || current_outputs->size() != 1) return false;
        const auto& output = current_outputs->front();
        const auto actual = render_feature_test::check_scene_draw_provenance(render_graph, *current_outputs,
            frame_context->frame_number());
        if (!actual || actual.value != 1 || native_frames.prepared.empty() || native_frames.executed.empty()) return false;
        const auto& prepared = native_frames.prepared.back().source;
        const auto& executed = native_frames.executed.back();
        if (!current_view.is_valid() || output.view_key != current_view.value() ||
            prepared.view_key != output.view_key || executed.view_key != output.view_key ||
            prepared.owner != output.resource_owner || executed.owner != output.resource_owner ||
            prepared.kind != aeg::render_graph::RenderSceneOperationKind::kDrawScene || executed.kind != prepared.kind)
            return false;
        const auto same_work = [](const auto& source, const auto& work) {
            return source.id == work.id && source.domain == work.domain && source.kind == work.kind &&
                source.feature_definition_id == work.feature_definition_id && source.owner_module == work.owner_module &&
                source.node_id == work.node_id && source.instance_id == work.instance_id &&
                source.invocation == work.invocation && source.frame_index == work.frame_index &&
                source.operation_index == work.operation_index;
        };
        size_t native_draws = 0;
        for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(render_graph).pass_plans) {
            const auto inspected = aeg::render_graph::detail::RenderGraphExecution::inspect_pass(render_graph, pass.pass);
            if (!inspected) return false;
            if (!pass.live || (inspected.value.name != "DrawOpaque" && inspected.value.name != "Renderer.ForwardOpaquePass")) continue;
            ++native_draws;
            if (!pass.feature_work || !same_work(prepared.work, *pass.feature_work) ||
                !same_work(executed.work, *pass.feature_work)) return false;
        }
        return native_draws == actual.value;
    };

    for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
        smoke_state.frame_index = frame;
        const Result frame_result = orchestrator->run_frame(frame_desc);
        if (!frame_result) {
            std::cerr << "multi-material frame execution failed: " << frame_result.message << '\n';
        }
        if (!require(frame_result, "multi-material frame execution failed")) {
            return 21;
        }
        if (!require(actual_geometry_has_owner(), "actual multi-material geometry has no current compiled Feature invocation")) return 42;
    }
    if (!require(smoke_state.readback_build_count == kFrameCount, "technical readback did not run once per frame") ||
        !require(native_frames.prepared.size() == kFrameCount && native_frames.executed.size() == kFrameCount,
                 "multi-material frames bypassed their registered native pass") ||
        !require(shader_manager.cache_epoch() == material_pso_epoch &&
                     aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(shader_layout) == material_pso_epoch,
                 "multi-material frame changed its shader contract epoch") ||
        !require(resolver.mesh_resolve_count >= kObjectCount * kFrameCount &&
                     resolver.material_resolve_count >= kObjectCount * kFrameCount &&
                     resolver.object_resolve_count >= kObjectCount * kFrameCount,
                 "resolver did not serve every multi-entity draw")) {
        return 22;
    }
    if (!require(device->wait_idle(), "multi-material GPU wait failed")) {
        return 23;
    }

    for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
        std::array<std::byte, kReadbackSize> pixels{};
        if (!require(frame_context->read_readback(smoke_state.readbacks[frame], pixels),
                     "multi-material pixel readback failed")) {
            return 24;
        }
        const std::array<unsigned, 3> first = expected_color(kFrames[frame][0]);
        const std::array<unsigned, 3> second = expected_color(kFrames[frame][1]);
        const std::array<unsigned, 3> third = expected_color(kFrames[frame][2]);
        if (!require(count_color(pixels, 0, 24, first) > 20, "shared material region is incorrect") ||
            !require(count_color(pixels, 24, 43, second) > 20, "second material region is incorrect") ||
            !require(count_color(pixels, 43, 64, third) > 20, "third material region is incorrect")) {
            return 25;
        }
    }

    const BindingSetHandle material_set_before_failure = smoke_state.materials[0].material.binding_set;
    const TextureViewHandle texture_before_failure = smoke_state.materials[0].material.texture_view;
    const BufferRange parameter_before_failure = smoke_state.materials[0].material.parameter_buffer;
    if (!require(!aeg::resource::validate_texture_material_parameters(
                     aeg::resource::TextureMaterialParameters{{1.1f, 0.0f, 0.0f, 1.0f}}),
                 "out-of-range Tint was accepted")) {
        return 26;
    }
    if (!require(frame_context->begin_frame(), "failure-path frame begin failed") ||
        !require(!aeg::resource::update_texture_material_for_frame(*device, *frame_context,
                                                                   smoke_state.materials[0].material, {}),
                 "missing texture update was accepted") ||
        !require(!aeg::resource::update_texture_material_parameters_for_frame(
                     *device, *frame_context, smoke_state.materials[0].material,
                     BufferRange{smoke_state.materials[0].parameter_buffer, std::numeric_limits<uint64_t>::max() - 8u,
                                 sizeof(aeg::resource::TextureMaterialParameters)}),
                 "out-of-range parameter buffer was accepted") ||
        !require(smoke_state.materials[0].material.binding_set == material_set_before_failure &&
                     smoke_state.materials[0].material.texture_view == texture_before_failure &&
                     smoke_state.materials[0].material.parameter_buffer.buffer == parameter_before_failure.buffer &&
                     smoke_state.materials[0].material.parameter_buffer.offset == parameter_before_failure.offset &&
                     smoke_state.materials[0].material.parameter_buffer.size == parameter_before_failure.size,
                 "failed material update changed the active version")) {
        (void)frame_context->abort_frame();
        return 27;
    }
    if (!require(frame_context->abort_frame(), "failure-path frame abort failed")) {
        return 28;
    }

    const auto stale_texture_resource =
        render_graph.import_texture(target_texture, target_view, target_desc,
                                    ResourceState{ResourceUsage::kUndefined, ShaderStage::kAll, QueueClass::kAuto,
                                                  false, PipelineStage::kAllCommands});
    if (!require(stale_texture_resource, "stale-handle setup import failed")) {
        return 29;
    }
    render_graph.reset();
    current_outputs.reset();
    if (!require(!aeg::render_graph::detail::RenderGraphResourceAccess::contains_texture(
                         render_graph, stale_texture_resource.value),
                 "stale RenderGraph handle was accepted")) {
        return 30;
    }
    for (size_t frame = 0; frame < submitted_outputs.size(); ++frame) {
        if (!require(!submitted_outputs[frame].expired() && !submitted_scenes[frame].expired() &&
                         !native_frames.prepared[frame].commands.expired(),
                     "graph reset retired a submitted multi-frame source before its frame slot")) return 43;
    }

    smoke_state.missing_texture = true;
    resolver.set_missing_texture(true);
    const Result missing_texture_result = orchestrator->run_frame(frame_desc);
    if (!missing_texture_result) {
        std::cerr << "expected missing texture failure: " << missing_texture_result.message << '\n';
    }
    if (!require(!missing_texture_result && missing_texture_result.code == ResultCode::kInvalidState &&
                     missing_texture_result.message.find("material texture") != std::string::npos,
                 "missing material texture did not fail the frame") ||
        !require(smoke_state.materials[0].material.binding_set == material_set_before_failure &&
                     smoke_state.materials[0].material.texture_view == texture_before_failure &&
                     smoke_state.materials[0].material.parameter_buffer.buffer == parameter_before_failure.buffer &&
                     smoke_state.readback_build_count == kFrameCount && native_frames.prepared.size() == kFrameCount &&
                     native_frames.executed.size() == kFrameCount,
                 "failed material frame submitted work or replaced its active material version")) {
        return 31;
    }
    resolver.set_missing_texture(false);
    smoke_state.missing_texture = false;
    const Result recovery_result = orchestrator->run_frame(frame_desc);
    if (!recovery_result) {
        std::cerr << "material recovery failed: " << recovery_result.message << '\n';
    }
    if (!require(recovery_result, "material did not recover after a failed frame")) {
        return 32;
    }
    if (!require(actual_geometry_has_owner(), "recovery geometry has no current compiled Feature invocation")) return 42;
    if (!require(smoke_state.readback_build_count == kFrameCount + 1 &&
                     native_frames.prepared.size() == kFrameCount + 1 && native_frames.executed.size() == kFrameCount + 1 &&
                     native_frames.stale_prepared_rejections >= kFrameCount - 1,
                 "recovery did not use a fresh registered native frame") ||
        !require(device->wait_idle(), "recovery GPU wait failed")) return 44;
    std::array<std::byte, kReadbackSize> recovery_pixels{};
    if (!require(frame_context->read_readback(smoke_state.readbacks[smoke_state.current_readback], recovery_pixels),
                 "recovery pixel readback failed") ||
        !require(count_color(recovery_pixels, 0, 24, expected_color(kFrames.back()[0])) > 20,
                 "recovery shared material region is incorrect") ||
        !require(count_color(recovery_pixels, 24, 43, expected_color(kFrames.back()[1])) > 20,
                 "recovery second material region is incorrect") ||
        !require(count_color(recovery_pixels, 43, 64, expected_color(kFrames.back()[2])) > 20,
                 "recovery third material region is incorrect")) return 45;

    const auto incompatible_layout_result = device->create_binding_layout(
        {std::array<aeg::rhi::BindingLayoutEntry, 2>{aeg::rhi::BindingLayoutEntry{{1, 0},
                                                                                  BindingType::kSampledTexture,
                                                                                  aeg::rhi::BindingArrayMode::kSingle,
                                                                                  1,
                                                                                  ShaderStage::kFragment,
                                                                                  aeg::rhi::BindingFlags::kNone},
                                                     aeg::rhi::BindingLayoutEntry{{1, 1},
                                                                                  BindingType::kSampler,
                                                                                  aeg::rhi::BindingArrayMode::kSingle,
                                                                                  1,
                                                                                  ShaderStage::kFragment,
                                                                                  aeg::rhi::BindingFlags::kNone}},
         DebugName{"Renderer.OpaqueMultiFrame.IncompatibleLayout"}});
    if (!require(incompatible_layout_result, "incompatible layout creation failed")) {
        return 33;
    }
    const auto incompatible_material = aeg::resource::create_texture_material(
        *device, aeg::resource::TextureMaterialDesc{smoke_state.materials[0].texture.sampled_view,
                                                    {},
                                                    incompatible_layout_result.value,
                                                    1,
                                                    DebugName{"Renderer.OpaqueMultiFrame.IncompatibleMaterial"}});
    auto incompatible_material_value = incompatible_material.value;
    if (!require(incompatible_material, "incompatible material creation failed") ||
        !require(!aeg::resource::update_texture_material_parameters(
                     *device, incompatible_material_value,
                     BufferRange{smoke_state.materials[0].parameter_buffer, 0,
                                 sizeof(aeg::resource::TextureMaterialParameters)}),
                 "incompatible material layout accepted parameter binding")) {
        return 34;
    }
    if (!require(aeg::resource::destroy_texture_material(*device, incompatible_material_value),
                 "incompatible material destruction failed") ||
        !require(device->destroy_binding_layout(incompatible_layout_result.value),
                 "incompatible layout destruction failed")) {
        return 35;
    }

    frame_desc.render_feature_post_build = {};
    current_outputs.reset();
    render_graph.reset();
    if (!require(passes.unregister_pass(aeg::renderer::kForwardOpaquePassTypeId), "native pass retirement failed") ||
        !require(!native_frames.delegate.expired(), "native multi-frame pass retired before submitted work")) return 46;
    for (size_t frame = kFrameCount - 1; frame < submitted_outputs.size(); ++frame) {
        if (!require(!submitted_outputs[frame].expired() && !submitted_scenes[frame].expired() &&
                         !native_frames.prepared[frame].commands.expired(),
                     "multi-frame source or prepared commands retired before GPU drain")) return 46;
    }
    const auto drained = frame_context->drain();
    if (!drained) std::cerr << "multi-frame GPU drain: " << drained.message << '\n';
    if (!require(drained, "multi-frame GPU drain failed") ||
        !require(native_frames.delegate.expired(), "native multi-frame pass survived GPU drain")) return 47;
    for (size_t frame = 0; frame < submitted_outputs.size(); ++frame) {
        if (!require(submitted_outputs[frame].expired() && submitted_scenes[frame].expired() &&
                         native_frames.prepared[frame].commands.expired(),
                     "multi-frame source or prepared commands survived GPU drain")) return 47;
    }

    if (!require(orchestrator->shutdown(), "orchestrator shutdown failed") ||
        !require(orchestrator->unregister_world_frame(world_scope.handle(), 1), "world frame unregister failed") ||
        !require(orchestrator->unregister_world(world_scope.handle()), "world unregister failed") ||
        !require(world_scope.shutdown(), "WorldScope shutdown failed") ||
        !require(renderer.unregister_extractor(extractor.value), "extractor unregister failed")) {
        return 36;
    }
    orchestrator.reset();
    frame_context.reset();
    for (auto& material : smoke_state.materials) {
        if (!require(aeg::resource::destroy_texture_material(*device, material.material),
                     "material destruction failed")) {
            return 37;
        }
        if (material.texture.is_valid()) {
            (void)aeg::resource::TextureUploader(*device).destroy_texture(material.texture);
        }
        if (material.parameter_buffer.is_valid()) {
            (void)device->destroy_buffer(material.parameter_buffer);
        }
    }
    for (uint32_t index = 0; index < kObjectCount; ++index) {
        (void)device->destroy_binding_set(object_resources[index].binding_set);
        (void)device->destroy_buffer(transform_buffers[index]);
    }
    (void)device->destroy_binding_set(smoke_state.lighting_binding_set);
    (void)device->destroy_buffer(smoke_state.lighting_buffer);
    (void)device->destroy_pipeline(graphics_pipeline);
    (void)device->destroy_buffer(index_buffer);
    (void)device->destroy_buffer(vertex_buffer);
    (void)device->destroy_texture_view(target_view);
    (void)device->destroy_texture(target_texture);
    if (!require(shader_manager.clear_layout_cache(), "shader layout cache cleanup failed")) {
        return 38;
    }
    return 0;
}
