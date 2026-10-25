/**
 * @file opaque_render_feature_uv_depth_smoke.cpp
 * @brief Verifies interpolated UV sampling, multi-material meshes, and depth ordering.
 */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "engine_frame_orchestrator.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "material.hpp"
#include "render_feature_draw.hpp"
#include "feature_render_test_fixture.hpp"
#include "render_feature_runtime.hpp"
#include "pipeline.hpp"
#include "render_graph.hpp"

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "../Engine/RenderGraph/private/render_graph_resource_access.hpp"
#include "../Engine/Shader/private/shader_pipeline_layout_access.hpp"
#include "renderer.hpp"
#include "renderer_variants_internal.hpp"
#include "render_pass_registry_execution.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "shader_contract.hpp"
#include "texture_uploader.hpp"
#include "world_scope.hpp"

namespace {
using aeg::foundation::Result;
using aeg::feature::RenderDepthAttachment;
using aeg::feature::RenderAttachmentLoad;
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

inline constexpr RenderResourceId kSceneColor{"OpaqueUvDepthSceneColor"};
inline constexpr RenderResourceId kSceneDepth{"OpaqueUvDepthSceneDepth"};
constexpr uint32_t kWidth = 96;
constexpr uint32_t kHeight = 96;
constexpr std::size_t kReadbackSize = static_cast<std::size_t>(kWidth) * kHeight * 4u;
constexpr uint32_t kMaterialCount = 3;
constexpr uint32_t kMeshCount = 3;
constexpr uint32_t kObjectCount = 4;
constexpr uint32_t kBaseFrameCount = 3;
constexpr uint32_t kNormalFrameCount = 8;
// The fixture host owns PSO generation independently of ShaderManager's layout cache.
constexpr uint64_t kHostMaterialVariantEpoch = 13;

using ObjectResourceFrames =
    std::array<std::array<aeg::renderer::RenderObjectResource, kObjectCount>, kNormalFrameCount>;

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

constexpr std::array<Vertex, 4> kNormalQuadVertices{{
    {-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
}};

constexpr std::array<Vertex, 4> kMirroredQuadVertices{{
    {-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {-1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
}};

constexpr std::array<Vertex, 4> kReverseNormalQuadVertices{{
    {-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f},
    {1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f},
    {1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f},
    {-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f},
}};

constexpr std::array<uint16_t, 6> kQuadIndices{{0, 1, 2, 0, 2, 3}};

struct MaterialDefinition final {
    std::array<std::byte, 16> pixels{};
    aeg::resource::TextureMaterialParameters tint{};
};

constexpr MaterialDefinition kMaterialDefinitions[kMaterialCount]{
    {{{std::byte{220}, std::byte{30}, std::byte{30}, std::byte{255}, std::byte{220}, std::byte{30}, std::byte{30},
       std::byte{255}, std::byte{220}, std::byte{30}, std::byte{30}, std::byte{255}, std::byte{220}, std::byte{30},
       std::byte{30}, std::byte{255}}},
     {{1.0f, 1.0f, 1.0f, 1.0f}}},
    {{{std::byte{30}, std::byte{220}, std::byte{30}, std::byte{255}, std::byte{30}, std::byte{220}, std::byte{30},
       std::byte{255}, std::byte{30}, std::byte{220}, std::byte{30}, std::byte{255}, std::byte{30}, std::byte{220},
       std::byte{30}, std::byte{255}}},
     {{1.0f, 1.0f, 1.0f, 1.0f}}},
    {{{std::byte{20}, std::byte{40}, std::byte{220}, std::byte{255}, std::byte{220}, std::byte{40}, std::byte{20},
       std::byte{255}, std::byte{20}, std::byte{220}, std::byte{40}, std::byte{255}, std::byte{220}, std::byte{220},
       std::byte{20}, std::byte{255}}},
     {{1.0f, 0.8f, 1.0f, 1.0f}}},
};

using MeshId = aeg::renderer::MeshAssetId;
using MaterialId = aeg::renderer::MaterialInstanceId;
using ObjectId = aeg::renderer::RenderObjectId;

constexpr MeshId kMeshIds[kMeshCount]{MeshId{"OpaqueUvDepth.MeshA"}, MeshId{"OpaqueUvDepth.MeshB"},
                                      MeshId{"OpaqueUvDepth.MeshReverseNormal"}};
constexpr MaterialId kMaterialIds[kMaterialCount]{MaterialId{"OpaqueUvDepth.MaterialRed"},
                                                  MaterialId{"OpaqueUvDepth.MaterialGreen"},
                                                  MaterialId{"OpaqueUvDepth.MaterialUv"}};
constexpr ObjectId kObjectIds[kObjectCount]{ObjectId{1}, ObjectId{2}, ObjectId{3}, ObjectId{4}};

struct MaterialState final {
    aeg::resource::GpuTexture texture{};
    TextureDesc texture_desc{};
    aeg::resource::TextureMaterial material{};
    BufferHandle parameter_buffer{};
    BufferDesc parameter_desc{};
};

struct SmokeState final {
    std::array<MaterialState, kMaterialCount> materials{};
    std::array<FrameAllocation, kNormalFrameCount> readbacks{};
    std::array<RenderGraphBuffer, kNormalFrameCount> readback_resources{};
    uint32_t frame_index{0};
    uint32_t current_readback{0};
    bool resources_ready{false};
    bool invalid_uv{false};
    bool missing_uv{false};
    bool invalid_normal{false};
    bool missing_normal{false};
    bool invalid_parameters{false};
    bool invalid_lighting{false};
    uint32_t lighting_mode{0};
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

[[nodiscard]] ResourceState make_undefined_state() {
    return {ResourceUsage::kUndefined, ShaderStage::kAll, QueueClass::kAuto, false, PipelineStage::kAllCommands};
}

[[nodiscard]] ResourceState make_sampled_state() {
    return {ResourceUsage::kSampledRead, ShaderStage::kFragment, QueueClass::kGraphics, false,
            PipelineStage::kFragmentShader};
}

[[nodiscard]] ResourceState make_uniform_state() {
    return {ResourceUsage::kUniformRead, ShaderStage::kFragment, QueueClass::kGraphics, false,
            PipelineStage::kFragmentShader};
}

[[nodiscard]] ResourceState make_host_read_state() {
    return {ResourceUsage::kHostRead, ShaderStage::kAll, QueueClass::kCopy, false, PipelineStage::kHost};
}

[[nodiscard]] BufferDesc make_readback_desc(uint64_t size) {
    BufferDesc desc{};
    desc.size = size;
    desc.capabilities = BufferCapability::kTransferSource | BufferCapability::kTransferDestination;
    desc.memory = MemoryClass::kReadback;
    desc.initial_usage = ResourceUsage::kHostRead;
    desc.initial_stages = PipelineStage::kHost;
    desc.initial_queue = QueueClass::kCopy;
    desc.name = DebugName{"Renderer.OpaqueUvDepth.Readback"};
    return desc;
}

[[nodiscard]] RenderGraphBuffer to_pipeline_handle(aeg::render_graph::RenderGraphBuffer handle) noexcept {
    return {handle.index, handle.generation, handle.graph_id};
}

[[nodiscard]] RenderGraphTexture to_pipeline_handle(aeg::render_graph::RenderGraphTexture handle) noexcept {
    return {handle.index, handle.generation, handle.graph_id};
}

class UvDepthResolver final : public aeg::renderer::RenderResourceResolver {
public:
    UvDepthResolver(Device& device, std::array<aeg::renderer::RenderMeshResource, kMeshCount> meshes,
                    std::array<aeg::renderer::RenderMaterialResource, kMaterialCount> materials,
                    ObjectResourceFrames objects, const uint32_t* frame_index)
        : device_(&device), meshes_(meshes), materials_(materials), objects_(objects), frame_index_(frame_index) {}

    void set_material(uint32_t index, aeg::renderer::RenderMaterialResource material) const noexcept {
        materials_[index] = material;
    }

    void set_invalid_uv(bool invalid) const noexcept { invalid_uv_ = invalid; }
    void set_missing_uv(bool missing) const noexcept { missing_uv_ = missing; }
    void set_invalid_normal(bool invalid) const noexcept { invalid_normal_ = invalid; }
    void set_missing_normal(bool missing) const noexcept { missing_normal_ = missing; }
    void set_invalid_parameters(bool invalid) const noexcept { invalid_parameters_ = invalid; }
    void set_material_variant_case(uint32_t value) const noexcept { material_variant_case_ = value; }
    void set_lighting(aeg::renderer::RenderLightingResource resource) const noexcept { lighting_ = resource; }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMeshResource> resolve_mesh(
        MeshId mesh) const override {
        for (uint32_t index = 0; index < kMeshCount; ++index) {
            if (mesh != kMeshIds[index]) {
                continue;
            }
            auto resource = meshes_[index];
            if (invalid_uv_) {
                resource.vertex_layout.uv_format = Format::kRgba32Float;
            }
            if (missing_uv_) {
                resource.vertex_layout.uv_offset = resource.vertex_stride;
            }
            if (invalid_normal_) {
                resource.vertex_layout.normal_format = Format::kRgba32Float;
            }
            if (missing_normal_) {
                resource.vertex_layout.normal_offset = resource.vertex_stride;
            }
            ++mesh_resolve_count;
            return {Result::success(), resource};
        }
        return {aeg::foundation::not_found("UV/depth mesh id is unknown"), {}};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        MaterialId material) const override {
        for (uint32_t index = 0; index < kMaterialCount; ++index) {
            if (material != kMaterialIds[index]) {
                continue;
            }
            auto resource = materials_[index];
            if (invalid_parameters_) {
                resource.material_parameter_size = resource.material_parameter_buffer_desc.size + 4u;
            }
            if (material_variant_case_ == 1) resource.depth_attachment_variant.reset();
            if (material_variant_case_ == 2)
                resource.depth_attachment_variant->attachment_contract.color_format = Format::kBgra8Unorm;
            if (material_variant_case_ == 3)
                resource.attachment_contract = resource.depth_attachment_variant->attachment_contract;
            if (material_variant_case_ == 4) resource.depth_attachment_variant->pipeline = {};
            if (material_variant_case_ == 5) ++resource.shader_contract.version;
            if (material_variant_case_ == 6) resource.shader_contract.layout_hash = 0;
            if (material_variant_case_ == 7) resource.shader_contract.cache_epoch = 0;
            if (material_variant_case_ == 8) ++resource.depth_attachment_variant->shader_contract.version;
            if (material_variant_case_ == 9) resource.depth_attachment_variant->shader_contract.layout_hash = 0;
            if (material_variant_case_ == 10) resource.depth_attachment_variant->shader_contract.cache_epoch = 0;
            if (material_variant_case_ == 11) ++resource.depth_attachment_variant->shader_contract.cache_epoch;
            if (material_variant_case_ == 12) resource.depth_attachment_variant->shader_contract.layout_hash ^= 1u;
            if (material_variant_case_ == 13) ++resource.depth_attachment_variant->cache_epoch;
            if (material_variant_case_ == 14) resource.variant_cache_epoch = 0;
            if (material_variant_case_ == 15) resource.shader_contract.layout_hash ^= 1u;
            if (material_variant_case_ == 16) ++resource.variant_cache_epoch;
            if (material_variant_case_ == 17) {
                resource.depth_attachment_variant->shader_contract = {};
                resource.depth_attachment_variant->cache_epoch = 0;
            }
            ++material_resolve_count;
            return {Result::success(), resource};
        }
        return {aeg::foundation::not_found("UV/depth material id is unknown"), {}};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderObjectResource> resolve_object(
        const aeg::renderer::RenderInstance& instance, const aeg::renderer::RenderView& view) const override {
        for (uint32_t index = 0; index < kObjectCount; ++index) {
            if (instance.object != kObjectIds[index]) {
                continue;
            }
            const aeg::renderer::RenderObjectConstants constants{
                view.camera.view_projection() * instance.world_transform, aeg::math::identity()};
            const uint32_t frame = frame_index_ != nullptr ? *frame_index_ % kNormalFrameCount : 0;
            const auto& object = objects_[frame][index];
            if (const Result result = device_->write_buffer(object.transform_buffer, object.transform_offset,
                                                            std::as_bytes(std::span{&constants, 1}));
                !result) {
                return {result, {}};
            }
            ++object_resolve_count;
            return {Result::success(), object};
        }
        return {aeg::foundation::not_found("UV/depth object id is unknown"), {}};
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
    mutable std::array<aeg::renderer::RenderMeshResource, kMeshCount> meshes_{};
    mutable std::array<aeg::renderer::RenderMaterialResource, kMaterialCount> materials_{};
    ObjectResourceFrames objects_{};
    const uint32_t* frame_index_{nullptr};
    mutable bool invalid_uv_{false};
    mutable bool missing_uv_{false};
    mutable bool invalid_normal_{false};
    mutable bool missing_normal_{false};
    mutable bool invalid_parameters_{false};
    mutable uint32_t material_variant_case_{0};
    mutable aeg::renderer::RenderLightingResource lighting_{};
};

[[nodiscard]] Result add_instances(const aeg::renderer::RenderExtractContext& context, uint32_t frame_index,
                                   uint32_t lighting_mode) {
    const auto add = [&context](ObjectId object, MeshId mesh, MaterialId material, aeg::math::Vec3 position,
                                float depth, float scale) {
        const aeg::math::Mat4 transform = aeg::math::trs(aeg::math::Vec3{position.x, position.y, depth},
                                                         aeg::math::Quat{}, aeg::math::Vec3{scale, scale, 1.0f});
        return context.builder.add_instance({object, mesh, material, transform, transform, {}, 1.0f, 0xffffffffu});
    };

    if (lighting_mode != 0) {
        if (const Result result = add(kObjectIds[0], kMeshIds[0], kMaterialIds[0], {-0.5f, 0.0f, 0.0f}, 0.5f, 0.35f);
            !result) {
            return result;
        }
        if (const Result result = add(kObjectIds[1], kMeshIds[2], kMaterialIds[0], {0.5f, 0.0f, 0.0f}, 0.5f, 0.35f);
            !result) {
            return result;
        }
        if (lighting_mode == 1) {
            return Result::success();
        }
        const aeg::math::Vec3 light_color = lighting_mode == 4   ? aeg::math::Vec3{1.0f, 0.0f, 0.0f}
                                            : lighting_mode == 5 ? aeg::math::Vec3{1.5f, 1.0f, 1.0f}
                                                                 : aeg::math::Vec3{1.0f, 1.0f, 1.0f};
        return context.builder.add_light({
            .light = aeg::renderer::RenderLightId{"OpaqueUvDepth.Directional"},
            .type = aeg::renderer::RenderLightType::kDirectional,
            .direction = {0.0f, 0.0f, -1.0f},
            .color = light_color,
            .intensity = lighting_mode == 3 ? 0.5f : 1.0f,
        });
    }

    if (frame_index == 0) {
        if (const Result result = add(kObjectIds[0], kMeshIds[0], kMaterialIds[0], {-0.5f, 0.45f, 0.0f}, 0.5f, 0.35f);
            !result) {
            return result;
        }
        if (const Result result = add(kObjectIds[1], kMeshIds[0], kMaterialIds[1], {0.5f, 0.45f, 0.0f}, 0.5f, 0.35f);
            !result) {
            return result;
        }
        if (const Result result = add(kObjectIds[2], kMeshIds[0], kMaterialIds[2], {-0.5f, -0.45f, 0.0f}, 0.5f, 0.35f);
            !result) {
            return result;
        }
        return add(kObjectIds[3], kMeshIds[1], kMaterialIds[2], {0.5f, -0.45f, 0.0f}, 0.5f, 0.35f);
    }

    const float first_depth = frame_index == 1 ? 0.75f : 0.25f;
    const float second_depth = frame_index == 1 ? 0.25f : 0.75f;
    if (const Result result = add(kObjectIds[0], kMeshIds[0], kMaterialIds[0], {0.0f, 0.0f, 0.0f}, first_depth, 0.7f);
        !result) {
        return result;
    }
    return add(kObjectIds[1], kMeshIds[1], kMaterialIds[1], {0.0f, 0.0f, 0.0f}, second_depth, 0.7f);
}

[[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderLightingConstants> make_lighting_constants(
    uint32_t lighting_mode) {
    const aeg::math::Vec3 ambient =
        lighting_mode == 0 ? aeg::math::Vec3{1.0f, 1.0f, 1.0f} : aeg::math::Vec3{0.1f, 0.1f, 0.1f};
    if (lighting_mode == 0 || lighting_mode == 1) {
        return aeg::renderer::make_render_lighting_constants({}, ambient);
    }
    const aeg::math::Vec3 color = lighting_mode == 4   ? aeg::math::Vec3{1.0f, 0.0f, 0.0f}
                                  : lighting_mode == 5 ? aeg::math::Vec3{1.5f, 1.0f, 1.0f}
                                                       : aeg::math::Vec3{1.0f, 1.0f, 1.0f};
    const aeg::renderer::RenderLight light{
        .light = aeg::renderer::RenderLightId{"OpaqueUvDepth.Directional"},
        .type = aeg::renderer::RenderLightType::kDirectional,
        .direction = {0.0f, 0.0f, -1.0f},
        .color = color,
        .intensity = lighting_mode == 3 ? 0.5f : 1.0f,
    };
    return aeg::renderer::make_render_lighting_constants(std::span{&light, 1}, ambient);
}

[[nodiscard]] std::array<unsigned, 3> pixel_rgb(const std::array<std::byte, kReadbackSize>& pixels, uint32_t x,
                                                uint32_t y) {
    const std::size_t offset = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
    return {std::to_integer<unsigned char>(pixels[offset]), std::to_integer<unsigned char>(pixels[offset + 1]),
            std::to_integer<unsigned char>(pixels[offset + 2])};
}

[[nodiscard]] bool near_rgb(std::array<unsigned, 3> actual, std::array<unsigned, 3> expected) {
    constexpr unsigned tolerance = 18;
    for (uint32_t channel = 0; channel < 3; ++channel) {
        if (actual[channel] + tolerance < expected[channel] || expected[channel] + tolerance < actual[channel]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::array<unsigned, 3> expected_material_pixel(uint32_t material, uint32_t texel) {
    const auto& definition = kMaterialDefinitions[material];
    const std::size_t offset = static_cast<std::size_t>(texel) * 4u;
    return {
        static_cast<unsigned>(std::to_integer<unsigned char>(definition.pixels[offset]) * definition.tint.tint[0]),
        static_cast<unsigned>(std::to_integer<unsigned char>(definition.pixels[offset + 1]) * definition.tint.tint[1]),
        static_cast<unsigned>(std::to_integer<unsigned char>(definition.pixels[offset + 2]) * definition.tint.tint[2]),
    };
}

[[nodiscard]] uint32_t count_rgb(const std::array<std::byte, kReadbackSize>& pixels, uint32_t x_begin, uint32_t x_end,
                                 uint32_t y_begin, uint32_t y_end, std::array<unsigned, 3> expected) {
    uint32_t count = 0;
    for (uint32_t y = y_begin; y < y_end; ++y) {
        for (uint32_t x = x_begin; x < x_end; ++x) {
            count += near_rgb(pixel_rgb(pixels, x, y), expected) ? 1u : 0u;
        }
    }
    return count;
}
}  // namespace

int main(int argc, char** argv) {
    const bool depth_load = argc == 2 && std::string_view(argv[1]) == "--depth-load";
    const bool depth_clear_near = argc == 2 && std::string_view(argv[1]) == "--depth-clear-near";
    static_assert(sizeof(Vertex) == sizeof(float) * 9u);

    PipelineRegistry registry;
    aeg::ecs::WorldManager world_manager;
    const auto world_handle = world_manager.create_world({"OpaqueUvDepthWorld", true});
    if (!require(world_handle, "UV/depth World creation failed")) {
        return 2;
    }
    const auto world_result = world_manager.find(world_handle.value);
    if (!require(world_result, "UV/depth World lookup failed") ||
        !require(world_result.value->create_entity(), "UV/depth dummy entity creation failed") ||
        !require(world_result.value->commit(1, 1), "UV/depth World commit failed")) {
        return 3;
    }

    aeg::world::WorldScope world_scope(world_result.value);
    if (!require(world_scope.initialize(), "UV/depth WorldScope initialization failed")) {
        return 4;
    }
    SmokeState smoke_state{};
    aeg::renderer::Renderer renderer;
    const auto extractor =
        renderer.register_extractor([&smoke_state](const aeg::renderer::RenderExtractContext& context) {
            return add_instances(context, smoke_state.frame_index, smoke_state.lighting_mode);
        });
    if (!require(extractor, "UV/depth extractor registration failed")) {
        return 6;
    }

    DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG Opaque UV Depth Smoke";
    device_desc.enable_validation = true;
    device_desc.enable_debug = true;
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "UV/depth RHI device creation failed")) {
        return 7;
    }
    std::unique_ptr<Device> device = std::move(device_result.value);

    auto shader_program_result = aeg::shader::load_shader_program(AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH);
    if (!require(shader_program_result, "UV/depth shader package loading failed")) {
        return 8;
    }
    aeg::shader::ShaderManager shader_manager(*device);
    auto shader_layout_result =
        shader_manager.build_pipeline_layout(shader_program_result.value, DebugName{"Renderer.OpaqueUvDepth.Layout"});
    if (!require(shader_layout_result, "UV/depth shader layout creation failed")) {
        return 9;
    }
    aeg::shader::ShaderPipelineLayout shader_layout = std::move(shader_layout_result.value);
    const auto object_layout_result = shader_manager.binding_layout(shader_layout, 0);
    const auto material_layout_result = shader_manager.binding_layout(shader_layout, 1);
    const auto lighting_layout_result = shader_manager.binding_layout(shader_layout, 2);
    if (!require(object_layout_result, "UV/depth object layout lookup failed") ||
        !require(material_layout_result, "UV/depth material layout lookup failed") ||
        !require(lighting_layout_result, "UV/depth lighting layout lookup failed")) {
        return 10;
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
    target_desc.name = DebugName{"Renderer.OpaqueUvDepth.Target"};
    const auto target_result = device->create_texture(target_desc);
    if (!require(target_result, "UV/depth target texture creation failed")) {
        return 11;
    }
    TextureHandle target_texture = target_result.value;
    const auto target_view_result = device->create_texture_view({
        .texture = target_texture,
        .kind = TextureViewKind::kColorAttachment,
        .dimension = TextureViewDimension::kTex2D,
        .format = target_desc.format,
        .subresources = {aeg::rhi::Aspect::kColor, 0, 1, 0, 1},
        .name = DebugName{"Renderer.OpaqueUvDepth.TargetView"},
    });
    if (!require(target_view_result, "UV/depth target view creation failed")) {
        return 12;
    }
    TextureViewHandle target_view = target_view_result.value;

    TextureDesc depth_desc = target_desc;
    depth_desc.format = Format::kD32Float;
    depth_desc.capabilities = TextureCapability::kDepthStencil;
    depth_desc.name = DebugName{"Renderer.OpaqueUvDepth.Depth"};
    const auto depth_result = device->create_texture(depth_desc);
    if (!require(depth_result, "UV/depth texture creation failed")) {
        return 13;
    }
    TextureHandle depth_texture = depth_result.value;
    const auto depth_view_result = device->create_texture_view({
        .texture = depth_texture,
        .kind = TextureViewKind::kDepthStencil,
        .dimension = TextureViewDimension::kTex2D,
        .format = depth_desc.format,
        .subresources = {aeg::rhi::Aspect::kDepth, 0, 1, 0, 1},
        .name = DebugName{"Renderer.OpaqueUvDepth.DepthView"},
    });
    if (!require(depth_view_result, "UV/depth view creation failed")) {
        return 14;
    }
    TextureViewHandle depth_view = depth_view_result.value;

    std::array<BufferHandle, kMeshCount> vertex_buffers{};
    std::array<BufferDesc, kMeshCount> vertex_descs{};
    const std::array<std::span<const std::byte>, kMeshCount> vertex_data{
        std::as_bytes(std::span{kNormalQuadVertices}), std::as_bytes(std::span{kMirroredQuadVertices}),
        std::as_bytes(std::span{kReverseNormalQuadVertices})};
    for (uint32_t index = 0; index < kMeshCount; ++index) {
        BufferDesc desc{};
        desc.size = vertex_data[index].size();
        desc.capabilities = BufferCapability::kVertex;
        desc.memory = MemoryClass::kUpload;
        desc.initial_usage = ResourceUsage::kVertexInput;
        desc.initial_stages = PipelineStage::kVertexInput;
        desc.initial_queue = QueueClass::kGraphics;
        desc.name = DebugName{"Renderer.OpaqueUvDepth.Vertex"};
        const auto buffer_result = device->create_buffer(desc);
        if (!require(buffer_result, "UV/depth vertex buffer creation failed") ||
            !require(device->write_buffer(buffer_result.value, 0, vertex_data[index]),
                     "UV/depth vertex buffer upload failed")) {
            return 15;
        }
        vertex_buffers[index] = buffer_result.value;
        vertex_descs[index] = desc;
    }

    BufferDesc index_desc{};
    index_desc.size = sizeof(kQuadIndices);
    index_desc.capabilities = BufferCapability::kIndex;
    index_desc.memory = MemoryClass::kUpload;
    index_desc.initial_usage = ResourceUsage::kIndexInput;
    index_desc.initial_stages = PipelineStage::kVertexInput;
    index_desc.initial_queue = QueueClass::kGraphics;
    index_desc.name = DebugName{"Renderer.OpaqueUvDepth.Index"};
    const auto index_result = device->create_buffer(index_desc);
    if (!require(index_result, "UV/depth index buffer creation failed") ||
        !require(device->write_buffer(index_result.value, 0, std::as_bytes(std::span{kQuadIndices})),
                 "UV/depth index upload failed")) {
        return 16;
    }
    BufferHandle index_buffer = index_result.value;

    std::array<std::array<BufferHandle, kObjectCount>, kNormalFrameCount> transform_buffers{};
    ObjectResourceFrames object_resources{};
    for (uint32_t frame = 0; frame < kNormalFrameCount; ++frame) {
        for (uint32_t index = 0; index < kObjectCount; ++index) {
            BufferDesc desc{};
            desc.size = sizeof(aeg::renderer::RenderObjectConstants);
            desc.capabilities = BufferCapability::kUniform;
            desc.memory = MemoryClass::kUpload;
            desc.initial_usage = ResourceUsage::kUniformRead;
            desc.initial_stages = PipelineStage::kVertexShader;
            desc.initial_queue = QueueClass::kGraphics;
            desc.name = DebugName{"Renderer.OpaqueUvDepth.Transform"};
            const auto buffer_result = device->create_buffer(desc);
            if (!require(buffer_result, "UV/depth transform buffer creation failed")) {
                return 17;
            }
            transform_buffers[frame][index] = buffer_result.value;
            const auto set_result = device->create_binding_set(BindingSetDesc{
                object_layout, BindingSetLifetime::kPersistent, DebugName{"Renderer.OpaqueUvDepth.ObjectSet"}});
            if (!require(set_result, "UV/depth object binding set creation failed")) {
                return 18;
            }
            BindingResource resource{};
            resource.type = BindingType::kUniformBuffer;
            resource.buffer_range =
                BufferRange{transform_buffers[frame][index], 0, sizeof(aeg::renderer::RenderObjectConstants)};
            const aeg::rhi::BindingWrite write{{0, 0}, 0, resource};
            if (!require(device->update_binding_set(set_result.value, std::span{&write, 1}),
                         "UV/depth object binding set update failed")) {
                return 19;
            }
            object_resources[frame][index] = {transform_buffers[frame][index],
                                              desc,
                                              object_layout,
                                              set_result.value,
                                              0,
                                              0,
                                              sizeof(aeg::renderer::RenderObjectConstants)};
        }
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
    graphics_state.depth_test_enable = true;
    graphics_state.depth_write_enable = true;
    graphics_state.color_formats = color_formats;
    graphics_state.depth_stencil_format = depth_desc.format;
    const auto pipeline_result =
        shader_manager.create_pipeline(shader_program_result.value, shader_layout,
                                       aeg::shader::ShaderPipelineDesc{PipelineType::kGraphics, graphics_state,
                                                                       DebugName{"Renderer.OpaqueUvDepth.Pipeline"}});
    if (!require(pipeline_result, "UV/depth graphics pipeline creation failed")) {
        return 20;
    }
    PipelineHandle graphics_pipeline = pipeline_result.value;
    auto color_state = graphics_state;
    color_state.depth_test_enable = false;
    color_state.depth_write_enable = false;
    color_state.depth_stencil_format = Format::kUnknown;
    const auto color_pipeline_result = shader_manager.create_pipeline(shader_program_result.value, shader_layout,
        aeg::shader::ShaderPipelineDesc{PipelineType::kGraphics, color_state,
            DebugName{"Renderer.OpaqueUvDepth.ColorPipeline"}});
    if (!require(color_pipeline_result, "UV/depth color-only pipeline creation failed")) return 52;
    const PipelineHandle color_pipeline = color_pipeline_result.value;
    const aeg::renderer::RenderMaterialShaderContract material_shader_contract{
        aeg::renderer::kRenderMaterialShaderContractVersion,
        aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(shader_layout),
        aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(shader_layout)};
    if (!require(material_shader_contract.is_valid() &&
                 material_shader_contract.cache_epoch != kHostMaterialVariantEpoch,
                 "UV/depth fixture requires independent valid Shader and host PSO epochs")) return 52;

    std::array<aeg::renderer::RenderMeshResource, kMeshCount> meshes{};
    for (uint32_t index = 0; index < kMeshCount; ++index) {
        meshes[index] = {
            .vertex_buffer = vertex_buffers[index],
            .vertex_buffer_desc = vertex_descs[index],
            .index_buffer = index_buffer,
            .index_buffer_desc = index_desc,
            .vertex_stride = sizeof(Vertex),
            .vertex_count = 4,
            .index_count = 6,
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
        };
    }
    std::array<aeg::renderer::RenderMaterialResource, kMaterialCount> material_resources{};
    for (auto& material : material_resources) {
        material.pipeline = graphics_pipeline;
        material.shader_contract = material_shader_contract;
        material.variant_cache_epoch = kHostMaterialVariantEpoch;
    }
    UvDepthResolver resolver(*device, meshes, material_resources, object_resources, &smoke_state.frame_index);

    aeg::pipeline::PipelineScheduler scheduler;
    aeg::engine::EngineFramePipelineHost pipeline_host(registry, scheduler);
    auto frame_context = std::make_unique<aeg::render_graph::FrameContext>(
        *device, aeg::render_graph::FrameContextDesc{8, 4u * 1024u * 1024u, 4u * 1024u * 1024u, 256,
                                                     std::numeric_limits<uint64_t>::max()});
    aeg::render_graph::RenderGraph render_graph(*device);
    std::optional<aeg::engine::EngineFrameOrchestrator> orchestrator;
    orchestrator.emplace(pipeline_host, *frame_context, render_graph);
    if (!require(orchestrator->register_world_frame(world_scope, 1),
                 "UV/depth World frame registration failed") ||
        !require(orchestrator->initialize(), "UV/depth orchestrator initialization failed")) {
        return 21;
    }

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
        view.depth_target = kSceneDepth;
        return {Result::success(), view};
    };
    frame_desc.render_feature_preparation = [&smoke_state, &resolver, device_ptr = device.get(), material_layout,
                                             lighting_layout, graphics_pipeline, color_pipeline, target_texture, target_view,
                                             target_desc, depth_texture, depth_view,
                                             depth_desc, material_shader_contract](const aeg::engine::EngineFramePreparationContext& context) {
        const auto color_resource =
            context.render_graph.import_texture(target_texture, target_view, target_desc, make_undefined_state());
        if (!color_resource) {
            return color_resource.result;
        }
        if (const Result result = context.bind_feature_texture(kSceneColor, color_resource.value); !result) {
            return result;
        }
        const auto depth_resource =
            context.render_graph.import_texture(depth_texture, depth_view, depth_desc, make_undefined_state());
        if (!depth_resource) {
            return depth_resource.result;
        }
        if (const Result result = context.bind_feature_texture(kSceneDepth, depth_resource.value); !result) {
            return result;
        }

        const bool initialize_resources = !smoke_state.resources_ready;
        std::array<aeg::render_graph::RenderGraphTexture, kMaterialCount> texture_resources{};
        std::array<aeg::render_graph::RenderGraphBuffer, kMaterialCount> parameter_resources{};
        aeg::resource::ResourceUploader resource_uploader(*device_ptr);
        if (initialize_resources) {
            aeg::resource::TextureUploader texture_uploader(*device_ptr);
            for (uint32_t index = 0; index < kMaterialCount; ++index) {
                TextureDesc texture_desc{};
                texture_desc.extent = {2, 2, 1};
                texture_desc.format = Format::kRgba8Unorm;
                texture_desc.mip_levels = 1;
                texture_desc.array_layers = 1;
                texture_desc.capabilities = TextureCapability::kSampled;
                texture_desc.memory = MemoryClass::kDeviceLocal;
                texture_desc.initial_usage = ResourceUsage::kUndefined;
                texture_desc.initial_stages = PipelineStage::kAllCommands;
                texture_desc.initial_queue = QueueClass::kAuto;
                texture_desc.name = DebugName{"Renderer.OpaqueUvDepth.MaterialTexture"};
                const aeg::resource::TextureSubresourceData subresource{
                    std::span<const std::byte>{kMaterialDefinitions[index].pixels}, 0, 0};
                const aeg::resource::TextureData texture_data{
                    texture_desc, std::span<const aeg::resource::TextureSubresourceData>{&subresource, 1}};
                const auto upload = texture_uploader.upload_texture(context.frame_context, context.render_graph,
                                                                    texture_data, "Renderer.OpaqueUvDepth.Texture");
                if (!upload) {
                    return upload.result;
                }
                smoke_state.materials[index].texture = upload.value.texture;
                smoke_state.materials[index].texture_desc = texture_desc;
                texture_resources[index] = upload.value.target;

                BufferDesc parameter_desc{};
                parameter_desc.size = sizeof(kMaterialDefinitions[index].tint);
                parameter_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kUniform;
                parameter_desc.memory = MemoryClass::kDeviceLocal;
                parameter_desc.initial_usage = ResourceUsage::kUndefined;
                parameter_desc.initial_stages = PipelineStage::kAllCommands;
                parameter_desc.initial_queue = QueueClass::kAuto;
                parameter_desc.name = DebugName{"Renderer.OpaqueUvDepth.Parameters"};
                const auto parameter_upload = resource_uploader.upload_buffer(
                    context.frame_context, context.render_graph,
                    aeg::resource::BufferUploadDesc{std::as_bytes(std::span{&kMaterialDefinitions[index].tint, 1}),
                                                    parameter_desc, 0, 16});
                if (!parameter_upload) {
                    return parameter_upload.result;
                }
                smoke_state.materials[index].parameter_buffer = parameter_upload.value.destination;
                smoke_state.materials[index].parameter_desc = parameter_desc;
                parameter_resources[index] = parameter_upload.value.target;

                const auto material_result = aeg::resource::create_texture_material(
                    *device_ptr, aeg::resource::TextureMaterialDesc{
                                     .texture_view = upload.value.texture.sampled_view,
                                     .sampler =
                                         {
                                             .min_filter = aeg::rhi::Filter::kNearest,
                                             .mag_filter = aeg::rhi::Filter::kNearest,
                                             .mip_filter = aeg::rhi::Filter::kNearest,
                                             .address_u = aeg::rhi::AddressMode::kClampToEdge,
                                             .address_v = aeg::rhi::AddressMode::kClampToEdge,
                                             .address_w = aeg::rhi::AddressMode::kClampToEdge,
                                             .max_lod = 0.0f,
                                             .name = DebugName{"Renderer.OpaqueUvDepth.Sampler"},
                                         },
                                     .binding_layout = material_layout,
                                     .binding_group = 1,
                                     .name = DebugName{"Renderer.OpaqueUvDepth.Material"},
                                 });
                if (!material_result) {
                    return material_result.result;
                }
                smoke_state.materials[index].material = material_result.value;
                if (const Result result = aeg::resource::update_texture_material_parameters(
                        *device_ptr, smoke_state.materials[index].material,
                        BufferRange{parameter_upload.value.destination, 0, sizeof(kMaterialDefinitions[index].tint)});
                    !result) {
                    return result;
                }
            }
            smoke_state.resources_ready = true;
        }

        for (uint32_t index = 0; index < kMaterialCount; ++index) {
            if (!initialize_resources) {
                const auto texture_resource = context.render_graph.import_texture(
                    smoke_state.materials[index].texture.texture, smoke_state.materials[index].texture.sampled_view,
                    smoke_state.materials[index].texture_desc, make_sampled_state());
                if (!texture_resource) {
                    return texture_resource.result;
                }
                texture_resources[index] = texture_resource.value;
                const auto parameter_resource = context.render_graph.import_buffer(
                    smoke_state.materials[index].parameter_buffer, smoke_state.materials[index].parameter_desc,
                    make_uniform_state());
                if (!parameter_resource) {
                    return parameter_resource.result;
                }
                parameter_resources[index] = parameter_resource.value;
            }
            resolver.set_material(index,
                                  aeg::renderer::RenderMaterialResource{
                                      .pipeline = color_pipeline,
                                      .binding_set = smoke_state.materials[index].material.binding_set,
                                      .binding_group = 1,
                                      .material_texture = to_pipeline_handle(texture_resources[index]),
                                      .material_parameter_buffer = smoke_state.materials[index].parameter_buffer,
                                      .material_parameter_buffer_desc = smoke_state.materials[index].parameter_desc,
                                      .material_parameter_resource = to_pipeline_handle(parameter_resources[index]),
                                      .material_parameter_offset = 0,
                                      .material_parameter_size = sizeof(aeg::resource::TextureMaterialParameters),
                                      .attachment_contract = {Format::kRgba8Unorm, aeg::rhi::SampleCount::kX1, Format::kUnknown},
                                      .depth_attachment_variant = aeg::renderer::RenderMaterialPipelineVariant{
                                          graphics_pipeline, {Format::kRgba8Unorm, aeg::rhi::SampleCount::kX1, Format::kD32Float},
                                          material_shader_contract, {}, kHostMaterialVariantEpoch},
                                      .shader_contract = material_shader_contract,
                                      .variant_cache_epoch = kHostMaterialVariantEpoch,
                                  });
        }

        const auto lighting_constants_result = make_lighting_constants(smoke_state.lighting_mode);
        if (!lighting_constants_result) {
            return lighting_constants_result.result;
        }
        const aeg::renderer::RenderLightingConstants lighting_constants = lighting_constants_result.value;
        BufferDesc lighting_desc{};
        lighting_desc.size = sizeof(lighting_constants);
        lighting_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kUniform;
        lighting_desc.memory = MemoryClass::kDeviceLocal;
        lighting_desc.initial_usage = ResourceUsage::kUndefined;
        lighting_desc.initial_stages = PipelineStage::kAllCommands;
        lighting_desc.initial_queue = QueueClass::kAuto;
        lighting_desc.name = DebugName{"Renderer.OpaqueUvDepth.Lighting"};
        const auto lighting_upload = resource_uploader.upload_buffer(
            context.frame_context, context.render_graph,
            aeg::resource::BufferUploadDesc{std::as_bytes(std::span{&lighting_constants, 1}), lighting_desc, 0, 16});
        if (!lighting_upload) {
            return lighting_upload.result;
        }
        const auto lighting_set_result = device_ptr->create_binding_set(BindingSetDesc{
            lighting_layout, BindingSetLifetime::kPersistent, DebugName{"Renderer.OpaqueUvDepth.LightingSet"}});
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
            .lighting_size =
                smoke_state.invalid_lighting ? sizeof(lighting_constants) + 4u : sizeof(lighting_constants),
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
        smoke_state.current_readback = smoke_state.frame_index < kNormalFrameCount ? smoke_state.frame_index : 0;
        if (smoke_state.frame_index < kNormalFrameCount) {
            smoke_state.readbacks[smoke_state.frame_index] = readback.value;
            smoke_state.readback_resources[smoke_state.frame_index] = to_pipeline_handle(readback_resource.value);
        } else {
            smoke_state.readbacks[0] = readback.value;
            smoke_state.readback_resources[0] = to_pipeline_handle(readback_resource.value);
        }
        return Result::success();
    };

    aeg::feature::FeatureRegistry feature_registry{aeg::feature::test::clock_test_host()};
    aeg::pipeline::RenderPassRegistry native_passes;
    if (!require(aeg::renderer::register_builtin_render_passes(native_passes), "native depth pass registration failed"))
        return 50;
    const aeg::render_graph::RenderPassExecutionHost pass_host{&native_passes,
        aeg::pipeline::detail::RenderPassRegistryExecution::execution_epoch(native_passes)};
    aeg::feature::FeatureRuntime feature_runtime(feature_registry);
    auto source = render_feature_test::definition();
    source.contract.permissions.capability_mask &= ~(aeg::feature::to_mask(aeg::feature::RenderIntentCapability::kCreateTransient) |
        aeg::feature::to_mask(aeg::feature::RenderIntentCapability::kCreateHistory));
    source.execute = [&smoke_state, depth_load, depth_clear_near](const aeg::feature::InvocationToken& token, const aeg::feature::StateSnapshot&,
                                               std::span<const uint8_t>, std::span<const uint8_t>) {
        using namespace aeg::feature;
        const bool load = depth_load && smoke_state.frame_index == 0;
        RenderDrawScene draw{{RenderResourceId("Depth.Scene"), 1}, {RenderResourceId(kSceneColor.value()), 1, true}};
        draw.clear_color = {0.f, 0.f, 1.f, 1.f};
        draw.depth = RenderDepthAttachment{{RenderResourceId(kSceneDepth.value()), 1, true},
            load ? RenderAttachmentLoad::kLoad : RenderAttachmentLoad::kClear,
            depth_clear_near && smoke_state.frame_index == 0 ? 0.1f : 1.f};
        RenderIntent intent{};
        intent.operations = {draw};
        if (smoke_state.frame_index == 0 && !depth_load && !depth_clear_near) {
            auto colour = draw;
            colour.scene.id = RenderResourceId("BaseColour.Scene");
            colour.depth.reset();
            intent.operations.push_back(colour);
        }
        intent.final_output = draw.target;
        StateDelta delta{token.state_version};
        delta.render_intent = std::move(intent);
        return aeg::foundation::ResultValue<StateDelta>{Result::success(), std::move(delta)};
    };
    if (!require(aeg::feature::test::register_clock_test_definition(feature_registry, source), "depth Feature registration failed")) return 50;
    auto document = render_feature_test::document(source);
    document.nodes.resize(1);
    auto compiled = aeg::feature::FeatureGraphCompiler{}.compile(document, feature_registry);
    if (!require(compiled, "depth Feature compile failed") ||
        !require(feature_runtime.publish_graph(document, compiled.value), "depth Feature publication failed")) return 51;
    frame_desc.render_feature_post_build = [&](const aeg::engine::EngineFramePostBuildContext& context) {
        using namespace aeg::feature;
        if (context.render_scenes.size() != 1) return aeg::foundation::invalid_state("depth scene missing");
        if (const auto result = feature_runtime.tick(source.contract.clock.id); !result) return result;
        const auto captured = feature_runtime.capture_render_outputs(compiled.value);
        if (!captured) return captured.result;
        auto base_colour_resolver = resolver;
        base_colour_resolver.set_material_variant_case(17);
        const std::array scenes{
            aeg::renderer::RenderFeatureSceneBinding{RenderResourceId("Depth.Scene"), 1, {}, context.render_scenes.front(), &resolver},
            aeg::renderer::RenderFeatureSceneBinding{RenderResourceId("BaseColour.Scene"), 1, {}, context.render_scenes.front(), &base_colour_resolver}};
        aeg::renderer::RenderFeatureDrawLowerer lowerer(scenes, pass_host);
        const auto depth_binding = std::find_if(context.feature_textures.begin(), context.feature_textures.end(),
            [](const auto& binding) { return binding.id == kSceneDepth; });
        const auto color_binding = std::find_if(context.feature_textures.begin(), context.feature_textures.end(),
            [](const auto& binding) { return binding.id == kSceneColor; });
        if (depth_binding == context.feature_textures.end() || color_binding == context.feature_textures.end())
            return aeg::foundation::invalid_state("depth fixture binding missing");
        if (smoke_state.frame_index == 0) {
            // Native allocation is unnecessary: rejected descriptors never reach graph execution.
            std::array<aeg::render_graph::RenderGraphTexture, 7> incompatible_depths{};
            for (uint32_t index = 0; index < incompatible_depths.size(); ++index) {
                auto description = depth_desc;
                switch (index) {
                    case 0: description.format = Format::kRgba8Unorm;
                            description.capabilities = TextureCapability::kColorAttachment; break;
                    case 1: description.capabilities = TextureCapability::kTransferDestination; break;
                    case 2: description.mip_levels = 2; break;
                    case 3: description.array_layers = 2; break;
                    case 4: description.samples = aeg::rhi::SampleCount::kX4; break;
                    case 5: ++description.extent.width; break;
                    case 6: description.dimension = aeg::rhi::TextureDimension::kTex3D;
                            description.extent.depth = 2; break;
                }
                const auto texture = context.render_graph.create_texture(description);
                if (!texture) return texture.result;
                incompatible_depths[index] = texture.value;
            }
            if (const auto r = context.render_graph.compile(); !r) return r;
            const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph);
            for (uint32_t case_index = 0; case_index < 36; ++case_index) {
                const uint32_t test_case = case_index < 12 ? case_index + 24 : case_index - 12;
                auto rejected_outputs = captured.value;
                auto& output = rejected_outputs.front();
                auto draw = std::get<RenderDrawScene>(output.intent.operations.front());
                draw.depth->target.id = RenderResourceId("Invalid.Depth");
                auto bindings = std::vector<aeg::render_graph::RenderGraphFeatureTextureBinding>(
                    context.feature_textures.begin(), context.feature_textures.end());
                bindings.push_back({aeg::pipeline::RenderResourceId(draw.depth->target.id.value()), depth_binding->texture});
                auto invalid_resolver = resolver;
                auto invalid_scenes = std::vector<aeg::renderer::RenderFeatureSceneBinding>(scenes.begin(), scenes.end());
                const auto material_case = test_case >= 19 && test_case < 23 ? test_case - 18
                    : test_case >= 24 ? test_case - 19 : 0;
                if (material_case != 0) {
                    invalid_resolver.set_material_variant_case(material_case);
                    draw.scene.id = RenderResourceId("Invalid.Material.Scene");
                    invalid_scenes.push_back({draw.scene.id, 1, {}, context.render_scenes.front(), &invalid_resolver});
                }
                switch (test_case) {
                    case 0: bindings.pop_back(); break;
                    case 1: ++draw.depth->target.version; break;
                    case 2: bindings.back().owner = FeatureInstanceId(output.instance_id.value() + 100); break;
                    case 3: bindings.push_back(bindings.back()); break;
                    case 4: draw.depth->target.id = draw.target.id; break;
                    case 5: draw.depth->target.external = false; break;
                    case 6: draw.depth->clear_depth = -0.1f; break;
                    case 7: draw.depth->clear_depth = std::numeric_limits<float>::quiet_NaN(); break;
                    case 8: draw.depth->clear_depth = 1.1f; break;
                    case 9: draw.depth->load = static_cast<RenderAttachmentLoad>(99); break;
                    case 10: output.contract.permissions.capability_mask &= ~to_mask(RenderIntentCapability::kWriteExternal); break;
                    case 11: output.contract.permissions.capability_mask &= ~to_mask(RenderIntentCapability::kReadExternal);
                             draw.depth->load = RenderAttachmentLoad::kLoad; break;
                    default:
                        if (test_case < 19) bindings.back().texture = incompatible_depths[test_case - 12];
                        else if (test_case == 23) bindings.back().texture = color_binding->texture;
                        break;
                }
                output.intent.operations.push_back(draw);
                aeg::renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes, pass_host);
                const auto mesh_calls = invalid_resolver.mesh_resolve_count;
                const auto object_calls = invalid_resolver.object_resolve_count;
                const auto rejected = aeg::render_graph::build_render_graph_features(context.render_graph,
                    std::make_shared<const std::vector<RenderFeatureOutput>>(std::move(rejected_outputs)),
                    {.textures = bindings, .draw_lowerer = &invalid_lowerer}, context.frame_context.frame_number());
                if (rejected) {
                    std::cerr << "depth invalid case accepted: " << test_case << '\n';
                    return aeg::foundation::invalid_state("depth draw accepted an incompatible binding or material");
                }
                if (material_case != 0 &&
                    (invalid_resolver.mesh_resolve_count != mesh_calls || invalid_resolver.object_resolve_count != object_calls)) {
                    std::cerr << "invalid material selection consumed mesh/object resources: " << material_case << '\n';
                    return aeg::foundation::invalid_state("rejected material selection caused resource resolution");
                }
                if (const auto r = context.render_graph.compile(); !r) return r;
                if (aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph).pass_count != baseline.pass_count ||
                    aeg::render_graph::detail::RenderGraphExecution::compile_info(context.render_graph).resource_count != baseline.resource_count)
                    return aeg::foundation::invalid_state("rejected depth draw leaked partial graph changes");
            }
        }
        const bool load = depth_load && smoke_state.frame_index == 0;
        if (load) {
            const auto seed = context.render_graph.add_pass("Host.SeedDepth", QueueClass::kGraphics,
                [target = depth_binding->texture](aeg::render_graph::PassBuilder& pass) {
                    return pass.use_depth_stencil_attachment(target, aeg::rhi::LoadOp::kClear,
                        aeg::rhi::StoreOp::kStore, 0.1f);
                }, [](aeg::render_graph::PassExecutionContext& command) {
                    if (const auto r = command.begin_rendering(); !r) return r;
                    return command.end_rendering();
                });
            if (!seed) return seed.result;
        }
        const auto outputs = std::make_shared<const std::vector<RenderFeatureOutput>>(captured.value);
        if (const auto r = aeg::render_graph::build_render_graph_features(context.render_graph, outputs,
            {.textures = context.feature_textures, .draw_lowerer = &lowerer}, context.frame_context.frame_number()); !r) return r;
        if (const auto r = context.render_graph.compile(); !r) return r;
        bool tagged_draw = false;
        bool live_base_colour = false;
        bool depth_discard = false;
        for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(context.render_graph).pass_plans) {
            if (!pass.feature_work) continue;
            const auto& work = *pass.feature_work;
            const auto& output = outputs->front();
            if (work.feature_definition_id != source.id || work.node_id != output.node_id ||
                work.instance_id != output.instance_id || work.invocation.invocation_id != output.invocation.invocation_id ||
                work.owner_module != source.owner_module || work.frame_index != context.frame_context.frame_number())
                return aeg::foundation::invalid_state("depth Draw provenance missing");
            tagged_draw = true;
            live_base_colour |= pass.live && work.operation_index == 1;
            for (const auto& barrier : pass.barriers)
                if (barrier.resource_kind == aeg::render_graph::RenderGraphResourceKind::kTexture &&
                    barrier.resource_index == depth_binding->texture.index)
                    depth_discard |= barrier.discard_contents;
        }
        if (!tagged_draw || (load && depth_discard))
            return aeg::foundation::invalid_state("depth Draw provenance or Load preservation missing");
        if (smoke_state.frame_index == 0 && !depth_load && !depth_clear_near && !live_base_colour)
            return aeg::foundation::invalid_state("base colour draw with an unused invalid optional variant was not live");
        const auto allocation = smoke_state.readbacks[smoke_state.current_readback];
        const auto destination = smoke_state.readback_resources[smoke_state.current_readback];
        const aeg::render_graph::RenderGraphBuffer buffer{destination.index, destination.generation, destination.graph_id};
        const auto pass = context.render_graph.add_pass("ReadbackFeatureDepth", QueueClass::kGraphics,
            [target = color_binding->texture, buffer, allocation](aeg::render_graph::PassBuilder& pass) {
                if (const auto r = pass.read(target, ResourceUsage::kCopySource, PipelineStage::kCopy); !r) return r;
                if (const auto r = pass.write(buffer, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                    ShaderStage::kAll, {allocation.offset, kReadbackSize}); !r) return r;
                pass.set_side_effects();
                return Result::success();
            }, [target = color_binding->texture, buffer, allocation](aeg::render_graph::PassExecutionContext& command) {
                return command.copy_texture_to_buffer(target, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {},
                    {kWidth, kHeight, 1}, buffer, allocation.offset, 0, 0);
            });
        return pass.result;
    };

    for (uint32_t frame = 0; frame < kBaseFrameCount; ++frame) {
        smoke_state.frame_index = frame;
        const Result frame_result = orchestrator->run_frame(frame_desc);
        if (!frame_result) {
            std::cerr << "UV/depth normal frame " << frame << " failed: " << frame_result.message << '\n';
        }
        if (!require(frame_result, "UV/depth normal frame failed")) {
            return 22;
        }
    }
    if (!require(device->wait_idle(), "UV/depth normal GPU wait failed")) {
        return 23;
    }

    std::array<std::array<std::byte, kReadbackSize>, kNormalFrameCount> pixels{};
    for (uint32_t frame = 0; frame < kBaseFrameCount; ++frame) {
        if (!require(frame_context->read_readback(smoke_state.readbacks[frame], pixels[frame]),
                     "UV/depth readback failed")) {
            return 24;
        }
    }
    const std::array<std::pair<uint32_t, uint32_t>, 4> quad_probe_positions{{{14, 16}, {34, 16}, {14, 36}, {34, 36}}};
    if ((depth_load || depth_clear_near) && count_rgb(pixels[0], 0, kWidth, 0, kHeight, {0, 0, 255}) != kWidth * kHeight) {
        std::cerr << "neutral depth Load/Clear did not use the specified near depth\n";
        return 52;
    }
    for (uint32_t probe = 0; !depth_load && !depth_clear_near && probe < quad_probe_positions.size(); ++probe) {
        const auto [x, y] = quad_probe_positions[probe];
        const std::array<uint32_t, 4> normal_texels{2, 3, 0, 1};
        if (!require(near_rgb(pixel_rgb(pixels[0], x, y + 48), expected_material_pixel(0, probe)),
                     "shared mesh red material UV sample is incorrect") ||
            !require(near_rgb(pixel_rgb(pixels[0], x + 48, y + 48), expected_material_pixel(1, probe)),
                     "shared mesh green material UV sample is incorrect") ||
            !require(near_rgb(pixel_rgb(pixels[0], x, y), expected_material_pixel(2, normal_texels[probe])),
                     "independent mesh shared material UV sample is incorrect") ||
            !require(near_rgb(pixel_rgb(pixels[0], x + 48, y),
                              expected_material_pixel(2, std::array<uint32_t, 4>{3, 2, 1, 0}[probe])),
                     "mirrored UV sample is incorrect")) {
            return 25;
        }
    }
    if (!require(count_rgb(pixels[1], 28, 68, 28, 68, expected_material_pixel(1, 0)) > 500,
                 "depth target did not keep the nearer green entity") ||
        !require(count_rgb(pixels[2], 28, 68, 28, 68, expected_material_pixel(0, 0)) > 500,
                 "depth target did not keep the nearer red entity after order change")) {
        return 26;
    }

    smoke_state.lighting_mode = 1;
    smoke_state.frame_index = 3;
    if (!require(orchestrator->run_frame(frame_desc), "ambient-only lighting frame failed")) {
        return 27;
    }
    smoke_state.lighting_mode = 2;
    smoke_state.frame_index = 4;
    if (!require(orchestrator->run_frame(frame_desc), "full directional lighting frame failed")) {
        return 28;
    }
    smoke_state.lighting_mode = 3;
    smoke_state.frame_index = 5;
    if (!require(orchestrator->run_frame(frame_desc), "half-intensity directional lighting frame failed")) {
        return 29;
    }
    smoke_state.lighting_mode = 4;
    smoke_state.frame_index = 6;
    if (!require(orchestrator->run_frame(frame_desc), "colored directional lighting frame failed")) {
        return 30;
    }
    if (!require(device->wait_idle(), "directional lighting GPU wait failed")) {
        return 31;
    }
    for (uint32_t frame = 3; frame <= 6; ++frame) {
        if (!require(frame_context->read_readback(smoke_state.readbacks[frame], pixels[frame]),
                     "directional lighting readback failed")) {
            return 32;
        }
    }
    const auto ambient_normal = pixel_rgb(pixels[3], 24, 48);
    const auto ambient_reverse = pixel_rgb(pixels[3], 72, 48);
    const auto full_normal = pixel_rgb(pixels[4], 24, 48);
    const auto full_reverse = pixel_rgb(pixels[4], 72, 48);
    const auto half_normal = pixel_rgb(pixels[5], 24, 48);
    const auto red_normal = pixel_rgb(pixels[6], 24, 48);
    if (!require(near_rgb(ambient_normal, {22, 3, 3}), "ambient-only fallback is incorrect") ||
        !require(near_rgb(ambient_reverse, {22, 3, 3}), "ambient fallback differs by normal") ||
        !require(near_rgb(full_normal, {242, 33, 33}), "front-facing Lambert response is incorrect") ||
        !require(near_rgb(full_reverse, {22, 3, 3}), "back-facing Lambert response is incorrect") ||
        !require(near_rgb(half_normal, {132, 18, 18}), "directional intensity response is incorrect") ||
        !require(red_normal[0] > 180 && red_normal[1] < 40 && red_normal[2] < 40,
                 "directional light color response is incorrect")) {
        return 33;
    }

    auto stale_resource = render_graph.import_texture(target_texture, target_view, target_desc, make_undefined_state());
    if (!require(stale_resource, "UV/depth stale-handle setup failed")) {
        return 34;
    }
    render_graph.reset();
    if (!require(!aeg::render_graph::detail::RenderGraphResourceAccess::contains_texture(render_graph, stale_resource.value),
                 "UV/depth stale handle was accepted")) {
        return 35;
    }

    smoke_state.invalid_uv = true;
    smoke_state.frame_index = 7;
    smoke_state.lighting_mode = 2;
    resolver.set_invalid_uv(true);
    if (!require(!orchestrator->run_frame(frame_desc), "UV/depth missing UV layout was accepted")) {
        return 36;
    }
    smoke_state.invalid_uv = false;
    resolver.set_invalid_uv(false);
    smoke_state.missing_uv = true;
    resolver.set_missing_uv(true);
    if (!require(!orchestrator->run_frame(frame_desc), "UV/depth out-of-range UV layout was accepted")) {
        return 37;
    }
    smoke_state.missing_uv = false;
    resolver.set_missing_uv(false);
    smoke_state.invalid_parameters = true;
    resolver.set_invalid_parameters(true);
    if (!require(!orchestrator->run_frame(frame_desc), "UV/depth invalid parameter range was accepted")) {
        return 38;
    }
    smoke_state.invalid_parameters = false;
    resolver.set_invalid_parameters(false);
    smoke_state.invalid_normal = true;
    resolver.set_invalid_normal(true);
    if (!require(!orchestrator->run_frame(frame_desc), "normal vertex format mismatch was accepted")) {
        return 39;
    }
    smoke_state.invalid_normal = false;
    resolver.set_invalid_normal(false);
    smoke_state.missing_normal = true;
    resolver.set_missing_normal(true);
    if (!require(!orchestrator->run_frame(frame_desc), "missing normal vertex attribute was accepted")) {
        return 40;
    }
    smoke_state.missing_normal = false;
    resolver.set_missing_normal(false);
    smoke_state.invalid_lighting = true;
    if (!require(!orchestrator->run_frame(frame_desc), "invalid lighting parameter range was accepted")) {
        return 41;
    }
    smoke_state.invalid_lighting = false;
    smoke_state.lighting_mode = 5;
    if (!require(!orchestrator->run_frame(frame_desc), "invalid directional light color was accepted")) {
        return 42;
    }
    smoke_state.lighting_mode = 2;
    smoke_state.frame_index = 0;
    if (!require(orchestrator->run_frame(frame_desc), "UV/depth recovery frame failed")) {
        return 43;
    }

    if (!require(orchestrator->shutdown(), "UV/depth orchestrator shutdown failed") ||
        !require(orchestrator->unregister_world_frame(world_scope.handle(), 1),
                 "UV/depth World frame unregister failed") ||
        !require(orchestrator->unregister_world(world_scope.handle()), "UV/depth World unregister failed") ||
        !require(world_scope.shutdown(), "UV/depth WorldScope shutdown failed") ||
        !require(renderer.unregister_extractor(extractor.value), "UV/depth extractor unregister failed")) {
        return 44;
    }
    orchestrator.reset();
    frame_context.reset();
    if (!require(device->wait_idle(), "UV/depth final GPU wait failed")) {
        return 45;
    }
    (void)device->destroy_binding_set(smoke_state.lighting_binding_set);
    (void)device->destroy_buffer(smoke_state.lighting_buffer);
    for (auto& material : smoke_state.materials) {
        if (!require(aeg::resource::destroy_texture_material(*device, material.material),
                     "UV/depth material destruction failed")) {
            return 46;
        }
        if (material.texture.is_valid()) {
            (void)aeg::resource::TextureUploader(*device).destroy_texture(material.texture);
        }
        if (material.parameter_buffer.is_valid()) {
            (void)device->destroy_buffer(material.parameter_buffer);
        }
    }
    for (uint32_t frame = 0; frame < kNormalFrameCount; ++frame) {
        for (uint32_t index = 0; index < kObjectCount; ++index) {
            (void)device->destroy_binding_set(object_resources[frame][index].binding_set);
            (void)device->destroy_buffer(transform_buffers[frame][index]);
        }
    }
    for (BufferHandle vertex_buffer : vertex_buffers) {
        (void)device->destroy_buffer(vertex_buffer);
    }
    (void)device->destroy_buffer(index_buffer);
    (void)device->destroy_pipeline(graphics_pipeline);
    (void)device->destroy_pipeline(color_pipeline);
    (void)device->destroy_texture_view(depth_view);
    (void)device->destroy_texture(depth_texture);
    (void)device->destroy_texture_view(target_view);
    (void)device->destroy_texture(target_texture);
    if (!require(shader_manager.clear_layout_cache(), "UV/depth shader layout cleanup failed")) {
        return 47;
    }
    return 0;
}
