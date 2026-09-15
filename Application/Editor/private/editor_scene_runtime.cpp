/**
 * @file editor_scene_runtime.cpp
 * @brief Implements the minimum editable scene lifecycle and offscreen preview.
 */
#include "editor_scene_runtime.hpp"

#include <render_feature_flow.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Components/component_bundle.hpp"
#include "Components/component_ids.hpp"
#include "Components/component_render_extraction.hpp"
#include "editor_asset_pipeline.hpp"
#include "editor_font_pipeline.hpp"
#include "material.hpp"
#include "opaque_render_feature.hpp"
#include "resource_uploader.hpp"
#include "shader_contract.hpp"
#include "texture_uploader.hpp"

namespace aeg::application {
namespace {
using aeg::engine::components::Camera;
using aeg::engine::components::DirectionalLight;
using aeg::engine::components::MeshRenderer;
using aeg::engine::components::TextRenderer;
using aeg::engine::components::Transform;
using foundation::Result;
using foundation::ResultCode;
using pipeline::RenderGraphBuffer;
using pipeline::RenderGraphTexture;
using pipeline::RenderResourceId;
using rhi::BindingLayoutHandle;
using rhi::BindingResource;
using rhi::BindingSetDesc;
using rhi::BindingSetHandle;
using rhi::BindingSetLifetime;
using rhi::BindingType;
using rhi::BindingWrite;
using rhi::BufferCapability;
using rhi::BufferDesc;
using rhi::BufferHandle;
using rhi::BufferRange;
using rhi::ClearColorValue;
using rhi::CullMode;
using rhi::DebugName;
using rhi::Device;
using rhi::Format;
using rhi::GraphicsPipelineState;
using rhi::MemoryClass;
using rhi::PipelineStage;
using rhi::PipelineType;
using rhi::PrimitiveTopology;
using rhi::QueueClass;
using rhi::ResourceState;
using rhi::ResourceUsage;
using rhi::ShaderStage;
using rhi::TextureCapability;
using rhi::TextureDesc;
using rhi::TextureHandle;
using rhi::TextureViewDimension;
using rhi::TextureViewHandle;
using rhi::TextureViewKind;
using rhi::VertexAttributeDesc;
using rhi::VertexBindingDesc;
using rhi::VertexInputRate;

inline constexpr RenderResourceId kSceneColor{"Editor.SceneColor"};
inline constexpr RenderResourceId kSceneDepth{"Editor.SceneDepth"};
inline constexpr RenderResourceId kLightingBuffer{"Editor.SceneLighting"};
inline constexpr RenderResourceId kMaterialParameterRed{"Editor.MaterialParameter.Red"};
inline constexpr RenderResourceId kMaterialParameterGreen{"Editor.MaterialParameter.Green"};
inline constexpr RenderResourceId kMaterialTextureRed{"Editor.MaterialTexture.Red"};
inline constexpr RenderResourceId kMaterialTextureGreen{"Editor.MaterialTexture.Green"};
inline constexpr uint32_t kFramesInFlight = 3;
inline constexpr uint32_t kMaxObjects = 16;
inline constexpr uint32_t kMaterialCount = 2;
inline constexpr uint64_t kMeshAssetA = 1;
inline constexpr uint64_t kMeshAssetB = 2;

inline constexpr pipeline::FeatureId kEditorViewportReadbackFeatureId{"Editor.ViewportReadbackFeature"};
inline constexpr pipeline::StageId kEditorViewportReadbackStageId{"Editor.ViewportReadbackFeature.Build"};
inline constexpr pipeline::RenderResourceId kEditorViewportReadbackBuffer{"Editor.ViewportReadback.Buffer"};

struct ViewportReadbackState final {
    bool requested{false};
    bool pass_built{false};
    bool pass_executed{false};
    /** The renderer graph reached its final Present state for this frame. */
    bool present_connected{true};
    render_graph::FrameAllocation allocation{};
    BufferDesc desc{};
    ResourceState initial_state{};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<std::byte> pixels{};
};
inline constexpr uint64_t kMaterialAssetRed = 1;
inline constexpr uint64_t kMaterialAssetGreen = 2;
inline constexpr scene::SceneEntityId kCameraEntity{1, 1};
inline constexpr scene::SceneEntityId kLightEntity{1, 2};
inline constexpr scene::SceneEntityId kLeftMeshEntity{1, 3};
inline constexpr scene::SceneEntityId kRightMeshEntity{1, 4};

[[nodiscard]] uint64_t font_texture_asset_id(uint64_t font_asset_id) noexcept {
    return 0x2000000000000000ull | (font_asset_id & 0x1fffffffffffffffull);
}

[[nodiscard]] uint64_t combine_asset_hash(uint64_t lhs, uint64_t rhs) noexcept {
    lhs ^= rhs + 0x9e3779b97f4a7c15ull + (lhs << 6u) + (lhs >> 2u);
    lhs ^= lhs >> 30u;
    lhs *= 0xbf58476d1ce4e5b9ull;
    lhs ^= lhs >> 27u;
    return lhs == 0 ? 1 : lhs;
}

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

constexpr std::array<Vertex, 4> kQuadVertices{{
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

constexpr std::array<uint16_t, 6> kQuadIndices{{0, 1, 2, 0, 2, 3}};

[[nodiscard]] ResourceState undefined_state() noexcept {
    return {ResourceUsage::kUndefined, ShaderStage::kAll, QueueClass::kAuto, false, PipelineStage::kAllCommands};
}

[[nodiscard]] ResourceState sampled_state() noexcept {
    return {ResourceUsage::kSampledRead, ShaderStage::kFragment, QueueClass::kGraphics, false,
            PipelineStage::kFragmentShader};
}

[[nodiscard]] ResourceState uniform_state() noexcept {
    return {ResourceUsage::kUniformRead, ShaderStage::kFragment, QueueClass::kGraphics, false,
            PipelineStage::kFragmentShader};
}

[[nodiscard]] ResourceState host_write_state() noexcept {
    return {ResourceUsage::kHostWrite, ShaderStage::kAll, QueueClass::kGraphics, false, PipelineStage::kHost};
}

[[nodiscard]] foundation::ResultValue<ResourceState> query_texture_initial_state(Device& device,
                                                                                 TextureHandle texture) {
    const auto snapshot = device.query_texture_state(texture);
    if (!snapshot) {
        return {snapshot.result, {}};
    }
    if (snapshot.value.subresource_states.empty()) {
        return {foundation::invalid_state("Editor texture state snapshot is empty"), {}};
    }
    return {Result::success(), snapshot.value.subresource_states.front()};
}

[[nodiscard]] RenderGraphTexture to_pipeline_handle(render_graph::RenderGraphTexture value) noexcept {
    return {value.index, value.generation, value.graph_id};
}

[[nodiscard]] RenderGraphBuffer to_pipeline_handle(render_graph::RenderGraphBuffer value) noexcept {
    return {value.index, value.generation, value.graph_id};
}

[[nodiscard]] BufferDesc make_uniform_desc(uint64_t size, std::string_view name) {
    BufferDesc desc{};
    desc.size = size;
    desc.capabilities = BufferCapability::kUniform;
    desc.memory = MemoryClass::kUpload;
    desc.initial_usage = ResourceUsage::kUniformRead;
    desc.initial_stages = PipelineStage::kAllCommands;
    desc.initial_queue = QueueClass::kGraphics;
    desc.name = DebugName{name};
    return desc;
}

[[nodiscard]] TextureDesc make_material_texture_desc(std::string_view name) {
    TextureDesc desc{};
    desc.extent = {2, 2, 1};
    desc.format = Format::kRgba8Unorm;
    desc.mip_levels = 1;
    desc.array_layers = 1;
    desc.capabilities = TextureCapability::kSampled | TextureCapability::kTransferDestination;
    desc.memory = MemoryClass::kDeviceLocal;
    desc.initial_usage = ResourceUsage::kUndefined;
    desc.initial_stages = PipelineStage::kAllCommands;
    desc.initial_queue = QueueClass::kAuto;
    desc.name = DebugName{name};
    return desc;
}

[[nodiscard]] TextureDesc make_target_desc(uint32_t width, uint32_t height, Format format,
                                           TextureCapability capabilities, std::string_view name) {
    TextureDesc desc{};
    desc.extent = {width, height, 1};
    desc.format = format;
    desc.mip_levels = 1;
    desc.array_layers = 1;
    desc.capabilities = capabilities;
    desc.memory = MemoryClass::kDeviceLocal;
    desc.initial_usage = ResourceUsage::kUndefined;
    desc.initial_stages = PipelineStage::kAllCommands;
    desc.initial_queue = QueueClass::kAuto;
    desc.name = DebugName{name};
    return desc;
}

[[nodiscard]] foundation::Result add_clear_pass(render_graph::RenderGraph& graph,
                                                render_graph::RenderGraphTexture texture, ClearColorValue color,
                                                std::string_view name) {
    const auto pass = graph.add_pass(
        name, QueueClass::kGraphics,
        [texture, color](render_graph::PassBuilder& builder) {
            return builder.use_color_attachment(texture, rhi::LoadOp::kClear, rhi::StoreOp::kStore, color,
                                                ResourceUsage::kSampledRead);
        },
        [](render_graph::RenderPassCommandContext& pass) {
            if (const Result result = pass.begin_rendering(); !result) {
                return result;
            }
            return pass.end_rendering();
        });
    return pass ? Result::success() : pass.result;
}

[[nodiscard]] std::vector<std::byte> bytes_of(const auto& value) {
    std::vector<std::byte> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

[[nodiscard]] bool finite_vec3(const math::Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool finite_quat(const math::Quat& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] TextureDesc make_asset_texture_desc(uint32_t width, uint32_t height, std::string_view name) {
    TextureDesc desc{};
    desc.extent = {width, height, 1};
    desc.format = Format::kRgba8Unorm;
    desc.mip_levels = 1;
    desc.array_layers = 1;
    desc.capabilities = TextureCapability::kSampled | TextureCapability::kTransferDestination;
    desc.memory = MemoryClass::kDeviceLocal;
    desc.initial_usage = ResourceUsage::kUndefined;
    desc.initial_stages = PipelineStage::kAllCommands;
    desc.initial_queue = QueueClass::kAuto;
    desc.name = DebugName{name};
    return desc;
}

[[nodiscard]] BufferDesc make_asset_buffer_desc(uint64_t size, BufferCapability capability, std::string_view name) {
    BufferDesc desc{};
    desc.size = size;
    desc.capabilities = capability | BufferCapability::kTransferDestination;
    desc.memory = MemoryClass::kDeviceLocal;
    desc.initial_usage =
        capability == BufferCapability::kVertex ? ResourceUsage::kVertexInput : ResourceUsage::kIndexInput;
    desc.initial_stages = PipelineStage::kVertexInput;
    desc.initial_queue = QueueClass::kGraphics;
    desc.name = DebugName{name};
    return desc;
}

[[nodiscard]] bool finite_matrix(const math::Mat4& value) noexcept {
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (!std::isfinite(value.at(row, column))) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] math::Quat rotation_from_matrix(const math::Mat4& value) noexcept {
    const float m00 = value.at(0, 0);
    const float m01 = value.at(0, 1);
    const float m02 = value.at(0, 2);
    const float m10 = value.at(1, 0);
    const float m11 = value.at(1, 1);
    const float m12 = value.at(1, 2);
    const float m20 = value.at(2, 0);
    const float m21 = value.at(2, 1);
    const float m22 = value.at(2, 2);
    const float trace = m00 + m11 + m22;
    math::Quat result{};
    if (trace > 0.0f) {
        const float scale = std::sqrt(trace + 1.0f) * 2.0f;
        result.w = 0.25f * scale;
        result.x = (m21 - m12) / scale;
        result.y = (m02 - m20) / scale;
        result.z = (m10 - m01) / scale;
    } else if (m00 > m11 && m00 > m22) {
        const float scale = std::sqrt(std::max(0.0f, 1.0f + m00 - m11 - m22)) * 2.0f;
        result.w = (m21 - m12) / scale;
        result.x = 0.25f * scale;
        result.y = (m01 + m10) / scale;
        result.z = (m02 + m20) / scale;
    } else if (m11 > m22) {
        const float scale = std::sqrt(std::max(0.0f, 1.0f + m11 - m00 - m22)) * 2.0f;
        result.w = (m02 - m20) / scale;
        result.x = (m01 + m10) / scale;
        result.y = 0.25f * scale;
        result.z = (m12 + m21) / scale;
    } else {
        const float scale = std::sqrt(std::max(0.0f, 1.0f + m22 - m00 - m11)) * 2.0f;
        result.w = (m10 - m01) / scale;
        result.x = (m02 + m20) / scale;
        result.y = (m12 + m21) / scale;
        result.z = 0.25f * scale;
    }
    return math::normalize(result);
}

[[nodiscard]] bool decompose_affine_transform(const math::Mat4& matrix, Transform& result) noexcept {
    if (!finite_matrix(matrix)) {
        return false;
    }
    math::Vec3 basis_x{matrix.at(0, 0), matrix.at(1, 0), matrix.at(2, 0)};
    math::Vec3 basis_y{matrix.at(0, 1), matrix.at(1, 1), matrix.at(2, 1)};
    math::Vec3 basis_z{matrix.at(0, 2), matrix.at(1, 2), matrix.at(2, 2)};
    float scale_x = math::length(basis_x);
    float scale_y = math::length(basis_y);
    float scale_z = math::length(basis_z);
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || !std::isfinite(scale_z) || scale_x <= math::kEpsilon ||
        scale_y <= math::kEpsilon || scale_z <= math::kEpsilon) {
        return false;
    }
    basis_x /= scale_x;
    basis_y /= scale_y;
    basis_z /= scale_z;
    const float orthogonality = std::max({std::abs(math::dot(basis_x, basis_y)), std::abs(math::dot(basis_x, basis_z)),
                                          std::abs(math::dot(basis_y, basis_z))});
    if (!std::isfinite(orthogonality) || orthogonality > 2.0e-3f) {
        return false;
    }
    if (math::dot(math::cross(basis_x, basis_y), basis_z) < 0.0f) {
        scale_x = -scale_x;
        basis_x = -basis_x;
    }
    math::Mat4 rotation = math::identity();
    rotation.at(0, 0) = basis_x.x;
    rotation.at(1, 0) = basis_x.y;
    rotation.at(2, 0) = basis_x.z;
    rotation.at(0, 1) = basis_y.x;
    rotation.at(1, 1) = basis_y.y;
    rotation.at(2, 1) = basis_y.z;
    rotation.at(0, 2) = basis_z.x;
    rotation.at(1, 2) = basis_z.y;
    rotation.at(2, 2) = basis_z.z;
    result.translation = {matrix.at(0, 3), matrix.at(1, 3), matrix.at(2, 3)};
    result.rotation = rotation_from_matrix(rotation);
    result.scale = {scale_x, scale_y, scale_z};
    return finite_vec3(result.translation) && finite_quat(result.rotation) && finite_vec3(result.scale);
}

[[nodiscard]] foundation::ResultValue<renderer::RenderLightingConstants> make_editor_lighting_constants(
    const ecs::WorldSnapshot& snapshot) {
    std::vector<renderer::RenderLight> lights;
    snapshot.for_each<DirectionalLight>([&lights](ecs::EntityId entity, const DirectionalLight& light) {
        if (!light.enabled) {
            return;
        }
        lights.push_back({renderer::make_render_light_id(entity),
                          renderer::RenderLightType::kDirectional,
                          {},
                          light.direction,
                          light.color,
                          light.intensity,
                          0.0f,
                          1.0f,
                          0.0f,
                          0xffffffffu,
                          false});
    });
    return renderer::make_render_lighting_constants(lights);
}

/** @brief Rejects editor payloads that would make the next committed render snapshot invalid. */
[[nodiscard]] Result validate_component_payload(ecs::ComponentTypeId component, std::span<const std::byte> bytes) {
    if (component == ecs::ComponentTypeId(aeg::engine::components::kTransformName)) {
        if (bytes.size() != sizeof(Transform)) {
            return foundation::invalid_argument("Transform payload size is invalid");
        }
        Transform value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        if (!finite_vec3(value.translation) || !finite_quat(value.rotation) || !finite_vec3(value.scale)) {
            return foundation::invalid_argument("Transform contains non-finite editor values");
        }
        return Result::success();
    }
    if (component == ecs::ComponentTypeId(aeg::engine::components::kCameraName)) {
        if (bytes.size() != sizeof(Camera)) {
            return foundation::invalid_argument("Camera payload size is invalid");
        }
        Camera value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        if (!std::isfinite(value.vertical_fov_radians) || value.vertical_fov_radians <= 0.0f ||
            value.vertical_fov_radians >= math::kPi || !std::isfinite(value.near_plane) ||
            !std::isfinite(value.far_plane) || value.near_plane <= 0.0f || value.far_plane <= value.near_plane) {
            return foundation::invalid_argument("Camera projection parameters are invalid");
        }
        return Result::success();
    }
    if (component == ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName)) {
        if (bytes.size() != sizeof(MeshRenderer)) {
            return foundation::invalid_argument("MeshRenderer payload size is invalid");
        }
        MeshRenderer value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        const auto in_unit_range = [](float scalar) {
            return std::isfinite(scalar) && scalar >= 0.0f && scalar <= 1.0f;
        };
        if (!in_unit_range(value.tint.x) || !in_unit_range(value.tint.y) || !in_unit_range(value.tint.z) ||
            !in_unit_range(value.tint_alpha) || !finite_vec3(value.bounds_center) ||
            !std::isfinite(value.bounds_radius) || value.bounds_radius < 0.0f) {
            return foundation::invalid_argument("MeshRenderer material or bounds parameters are invalid");
        }
        return Result::success();
    }
    if (component == ecs::ComponentTypeId(aeg::engine::components::kTextRendererName)) {
        if (bytes.size() != sizeof(TextRenderer)) {
            return foundation::invalid_argument("TextRenderer payload size is invalid");
        }
        TextRenderer value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        if (value.text.size() > 4096 || !std::isfinite(value.world_size) || value.world_size <= 0.0f ||
            !finite_vec3(value.color) || value.color.x < 0.0f || value.color.x > 1.0f || value.color.y < 0.0f ||
            value.color.y > 1.0f || value.color.z < 0.0f || value.color.z > 1.0f || !std::isfinite(value.color_alpha) ||
            value.color_alpha < 0.0f || value.color_alpha > 1.0f) {
            return foundation::invalid_argument("TextRenderer contains invalid editor values");
        }
        return Result::success();
    }
    if (component == ecs::ComponentTypeId(aeg::engine::components::kDirectionalLightName)) {
        if (bytes.size() != sizeof(DirectionalLight)) {
            return foundation::invalid_argument("Directional Light payload size is invalid");
        }
        DirectionalLight value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        const auto in_unit_range = [](float scalar) {
            return std::isfinite(scalar) && scalar >= 0.0f && scalar <= 1.0f;
        };
        math::Vec3 normalized_direction{};
        if (!finite_vec3(value.direction) || !in_unit_range(value.color.x) || !in_unit_range(value.color.y) ||
            !in_unit_range(value.color.z) || !std::isfinite(value.intensity) || value.intensity < 0.0f ||
            (value.intensity > 0.0f && !math::try_normalize(value.direction, normalized_direction))) {
            return foundation::invalid_argument("Directional Light contains invalid editor values");
        }
        return Result::success();
    }
    return Result::success();
}

[[nodiscard]] math::Vec3 gizmo_axis(ViewportGizmoAxis axis) noexcept {
    switch (axis) {
        case ViewportGizmoAxis::kX:
            return {1.0f, 0.0f, 0.0f};
        case ViewportGizmoAxis::kY:
            return {0.0f, 1.0f, 0.0f};
        case ViewportGizmoAxis::kZ:
            return {0.0f, 0.0f, 1.0f};
    }
    return {};
}

[[nodiscard]] bool valid_gizmo_axis(ViewportGizmoAxis axis) noexcept {
    return axis == ViewportGizmoAxis::kX || axis == ViewportGizmoAxis::kY || axis == ViewportGizmoAxis::kZ;
}

[[nodiscard]] Result write_bytes_atomically(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    if (path.empty()) {
        return foundation::invalid_argument("Scene output path is empty");
    }
    std::error_code error;
    if (std::filesystem::is_directory(path, error) && !error) {
        return foundation::invalid_state("Scene output path is a directory");
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return Result::failure(ResultCode::kUnknown, "Scene directory could not be created");
        }
    }
    const std::filesystem::path temporary = path.string() + ".aegtmp";
    const std::filesystem::path backup = path.string() + ".aegbak";
    std::filesystem::remove(temporary, error);
    error.clear();
    std::filesystem::remove(backup, error);
    error.clear();
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Result::failure(ResultCode::kUnknown, "Scene temporary file could not be opened");
        }
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            std::filesystem::remove(temporary, error);
            return Result::failure(ResultCode::kUnknown, "Scene temporary file write failed");
        }
    }

    const bool had_target = std::filesystem::exists(path, error) && !error;
    if (had_target) {
        std::filesystem::rename(path, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return Result::failure(ResultCode::kUnknown, "Existing scene file could not be staged for replacement");
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        if (had_target) {
            std::error_code restore_error;
            std::filesystem::rename(backup, path, restore_error);
        }
        std::filesystem::remove(temporary, error);
        return Result::failure(ResultCode::kUnknown, "Scene file replacement failed");
    }
    if (had_target) {
        std::filesystem::remove(backup, error);
    }
    return Result::success();
}

[[nodiscard]] bool intersect_ray_plane(const math::Vec3& origin, const math::Vec3& direction,
                                       const math::Vec3& plane_point, const math::Vec3& plane_normal,
                                       math::Vec3& hit) noexcept {
    const float denominator = math::dot(direction, plane_normal);
    if (!std::isfinite(denominator) || std::abs(denominator) <= math::kEpsilon) {
        return false;
    }
    const float distance = math::dot(plane_point - origin, plane_normal) / denominator;
    if (!std::isfinite(distance)) {
        return false;
    }
    hit = origin + direction * distance;
    return finite_vec3(hit);
}

/** @brief Tests the actual editor Quad instead of using a loose projected sphere. */
[[nodiscard]] bool intersect_editor_quad(const math::Vec3& ray_origin, const math::Vec3& ray_direction,
                                         const Transform& transform, uint64_t mesh_asset_id, float& distance) noexcept {
    if (mesh_asset_id != kMeshAssetA && mesh_asset_id != kMeshAssetB) {
        return false;
    }

    const math::Mat4 world = transform.to_matrix();
    math::Mat4 inverse_world{};
    if (!math::try_inverse(world, inverse_world)) {
        return false;
    }
    const math::Vec3 local_origin = math::transform_point(inverse_world, ray_origin);
    const math::Vec3 local_direction = math::transform_vector(inverse_world, ray_direction);
    if (!finite_vec3(local_origin) || !finite_vec3(local_direction) || std::abs(local_direction.z) <= math::kEpsilon) {
        return false;
    }

    const float local_distance = -local_origin.z / local_direction.z;
    if (!std::isfinite(local_distance) || local_distance < 0.0f) {
        return false;
    }
    const math::Vec3 local_hit = local_origin + local_direction * local_distance;
    constexpr float kQuadExtent = 1.0f;
    if (!finite_vec3(local_hit) || local_hit.x < -kQuadExtent || local_hit.x > kQuadExtent ||
        local_hit.y < -kQuadExtent || local_hit.y > kQuadExtent) {
        return false;
    }

    const math::Vec3 world_hit = math::transform_point(world, local_hit);
    distance = math::dot(world_hit - ray_origin, ray_direction);
    return finite_vec3(world_hit) && std::isfinite(distance) && distance >= 0.0f;
}

class EditorViewportReadbackFeature final : public pipeline::RenderFeature {
public:
    [[nodiscard]] const pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const pipeline::FeatureDescriptor kDescriptor{
            .id = kEditorViewportReadbackFeatureId,
            .name = "Editor.ViewportReadbackFeature",
            .domain = pipeline::FeatureDomain::kRender,
            .domain_mask = pipeline::to_mask(pipeline::FeatureDomain::kRender),
            .supported_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame),
            .version = {1, 0, 0},
        };
        return kDescriptor;
    }

    [[nodiscard]] foundation::Result build_render_graph(pipeline::RenderGraphBuilder& builder) const override {
        const renderer::RenderFeatureContext* context = renderer::render_feature_context(builder);
        if (context == nullptr || !context->is_valid()) {
            return foundation::invalid_state("viewport readback requires a valid renderer context");
        }
        auto* state = static_cast<ViewportReadbackState*>(context->user_data);
        if (state == nullptr) {
            return foundation::invalid_state("viewport output requires a state object");
        }
        // This frame feature is scheduled inside the Render pipeline, before the
        // runtime appends its terminal SceneColor composite.  For a serial state
        // chain, read the directly declared state input so a readback observes the
        // same frame's output rather than the prior SceneColor contents.  The
        // visible viewport itself samples SceneColor after terminal composition.
        pipeline::RenderGraphTexture color = builder.state_input();
        if (!color.is_valid()) {
            const auto scene_color = builder.find_texture(context->scene_snapshot->view.color_target);
            if (!scene_color) {
                return scene_color.result;
            }
            color = scene_color.value;
        }
        if (!state->present_connected) {
            const auto clear = builder.add_graphics_pass(
                "Editor.Viewport.NoPresent",
                [color](pipeline::RenderPassBuilder& pass) {
                    return pass.use_color_attachment(color, rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                                                     ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f},
                                                     ResourceUsage::kSampledRead);
                },
                [](pipeline::RenderPassExecutionContext& pass) {
                    if (const Result result = pass.begin_rendering(); !result) {
                        return result;
                    }
                    return pass.end_rendering();
                });
            if (!clear) {
                return clear.result;
            }
        }
        if (!state->requested) {
            return foundation::Result::success();
        }
        if (!state->allocation.is_valid() || state->width == 0 || state->height == 0) {
            return foundation::invalid_state("viewport readback was requested without a valid allocation");
        }
        const auto readback = builder.import_buffer(kEditorViewportReadbackBuffer, state->allocation.buffer,
                                                    state->desc, state->initial_state);
        if (!readback) {
            return readback.result;
        }
        const uint64_t pixel_bytes = static_cast<uint64_t>(state->width) * state->height * 4u;
        const render_graph::FrameAllocation allocation = state->allocation;
        const uint32_t width = state->width;
        const uint32_t height = state->height;
        state->pass_built = false;
        state->pass_executed = false;
        const auto pass = builder.add_graphics_pass(
            "Editor.ViewportReadback",
            [color, readback = readback.value, allocation,
             pixel_bytes](pipeline::RenderPassBuilder& pass) {
                if (const foundation::Result result =
                        pass.read(color, ResourceUsage::kCopySource, PipelineStage::kCopy, ShaderStage::kAll);
                    !result) {
                    return result;
                }
                if (const foundation::Result result =
                        pass.write(readback, ResourceUsage::kCopyDestination, PipelineStage::kCopy, ShaderStage::kAll,
                                   pipeline::RenderGraphBufferRange{allocation.offset, pixel_bytes}, true);
                    !result) {
                    return result;
                }
                pass.set_side_effects();
                return foundation::Result::success();
            },
            [state, color, readback = readback.value, allocation, width,
             height](pipeline::RenderPassExecutionContext& pass) {
                state->pass_executed = true;
                return pass.copy_texture_to_buffer(color, rhi::TextureSubresourceLayers{rhi::Aspect::kColor, 0, 0, 1},
                                                   rhi::Offset3D{0, 0, 0}, rhi::Extent3D{width, height, 1}, readback,
                                                   allocation.offset, 0, 0);
            });
        if (!pass) {
            return pass.result;
        }
        state->pass_built = true;
        return foundation::Result::success();
    }

protected:
    [[nodiscard]] foundation::Result setup(pipeline::RenderGraphBuilder& builder) const override {
        const auto stage = builder.add_stage({
            .id = kEditorViewportReadbackStageId,
            .phase = pipeline::EnginePhase::kRenderBuild,
            .execution_domain = pipeline::ExecutionDomain::kRenderThread,
            .time_policy = pipeline::TimePolicy::kFrame,
            .parallelizable = false,
        });
        return stage ? foundation::Result::success()
                     : foundation::invalid_state("viewport readback feature stage declaration failed");
    }
};

}  // namespace

struct EditorSceneRuntime::Impl final {
    struct ViewportRay final {
        math::Vec3 origin{};
        math::Vec3 direction{};
    };

    struct EditorComponentValue final {
        scene::SceneEntityId entity{};
        ecs::ComponentTypeId component{};
        std::vector<std::byte> bytes{};
    };

    struct EditorCommand final {
        std::vector<EditorComponentValue> before{};
        std::vector<EditorComponentValue> after{};
        std::optional<scene::SceneAsset> before_scene{};
        std::optional<scene::SceneAsset> after_scene{};
        bool rebuild_edit_world{false};
    };

    struct EditTransaction final {
        bool active{false};
        ViewportGizmoMode mode{ViewportGizmoMode::kTranslate};
        ViewportGizmoAxis axis{ViewportGizmoAxis::kX};
        scene::SceneEntityId entity{};
        ViewportPoint start_pixel{};
        Transform initial{};
        math::Vec3 initial_plane_hit{};
        math::Vec3 plane_normal{};
        math::Vec3 axis_vector{};
        Transform current{};
        EditorCommand command{};
    };

    struct MaterialGpu final {
        uint64_t asset_id{0};
        uint64_t source_hash{0};
        bool from_baked_format{false};
        bool transient{false};
        uint64_t texture_asset_id{0};
        bool owns_texture{false};
        resource::GpuTexture texture_gpu{};
        TextureHandle texture{};
        TextureViewHandle sampled_view{};
        TextureDesc texture_desc{};
        rhi::SamplerHandle sampler{};
        std::array<BufferHandle, kFramesInFlight> parameter_buffers{};
        std::array<BindingSetHandle, kFramesInFlight> binding_sets{};
        BufferDesc parameter_desc{};
        pipeline::RenderGraphTexture graph_texture{};
        pipeline::RenderGraphBuffer graph_parameter{};
        std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct ImportedMeshGpu final {
        uint64_t source_hash{0};
        bool from_baked_format{false};
        bool transient{false};
        resource::GpuMesh gpu{};
        renderer::RenderMeshResource render{};
        math::Vec3 bounds_center{};
        float bounds_radius{0.0f};
    };

    struct ImportedTextureGpu final {
        uint64_t source_hash{0};
        bool from_baked_format{false};
        bool transient{false};
        resource::GpuTexture gpu{};
        TextureDesc desc{};
        pipeline::RenderGraphTexture graph_texture{};
    };

    struct CachedFont final {
        uint64_t source_hash{0};
        asset_pipeline::FontAtlas atlas{};
    };

    struct Resolver final : renderer::RenderResourceResolver {
        explicit Resolver(Impl& owner) noexcept : owner_(&owner) {}

        [[nodiscard]] foundation::ResultValue<renderer::RenderMeshResource> resolve_mesh(
            renderer::MeshAssetId mesh) const override {
            if (owner_ == nullptr) {
                return {foundation::invalid_state("Editor scene resolver is detached"), {}};
            }
            const uint64_t id = mesh.value();
            if (id == kMeshAssetA || id == kMeshAssetB) {
                return {Result::success(), owner_->meshes_[id == kMeshAssetA ? 0 : 1]};
            }
            const auto imported = owner_->imported_meshes_.find(id);
            const auto* asset = owner_->asset_database_.find(id);
            const bool can_use_last_good =
                asset != nullptr && asset->kind == EditorAssetKind::kMesh &&
                (asset->status == EditorAssetStatus::kImported || asset->status == EditorAssetStatus::kFailed);
            if (imported != owner_->imported_meshes_.end() && imported->second.render.is_valid() &&
                (imported->second.transient || can_use_last_good)) {
                return {Result::success(), imported->second.render};
            }
            owner_->diagnostic_ = asset != nullptr && asset->status == EditorAssetStatus::kImported
                                      ? "Mesh asset is still loading or failed; using editor fallback mesh."
                                  : asset != nullptr && asset->status == EditorAssetStatus::kFailed
                                      ? "Mesh reimport failed; using editor fallback mesh."
                                      : "Mesh Asset ID is missing; using editor fallback mesh.";
            return {Result::success(), owner_->meshes_[0]};
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderMaterialResource> resolve_material(
            renderer::MaterialInstanceId material) const override {
            if (owner_ == nullptr) {
                return {foundation::invalid_state("Editor scene resolver is detached"), {}};
            }
            const uint64_t id = material.value();
            const auto imported = owner_->imported_materials_.find(id);
            const auto* asset = owner_->asset_database_.find(id);
            const bool can_use_last_good =
                asset != nullptr && asset->kind == EditorAssetKind::kMaterial &&
                (asset->status == EditorAssetStatus::kImported || asset->status == EditorAssetStatus::kFailed);
            if (imported != owner_->imported_materials_.end() && (imported->second.transient || can_use_last_good)) {
                const MaterialGpu& gpu = imported->second;
                return {Result::success(),
                        renderer::RenderMaterialResource{
                            owner_->pipeline_, gpu.binding_sets[owner_->current_slot_], 1, gpu.graph_texture,
                            gpu.parameter_buffers[owner_->current_slot_], gpu.parameter_desc, gpu.graph_parameter, 0,
                            sizeof(resource::TextureMaterialParameters)}};
            }
            const uint32_t index = id == kMaterialAssetGreen ? 1u : 0u;
            if (id != kMaterialAssetRed && id != kMaterialAssetGreen) {
                owner_->diagnostic_ =
                    asset != nullptr && asset->status == EditorAssetStatus::kImported
                        ? "Imported material metadata is available; editor preview uses the fallback material."
                    : asset != nullptr && asset->status == EditorAssetStatus::kFailed
                        ? "Material reimport failed; using editor fallback material."
                        : "Material Asset ID is missing; using editor fallback material.";
            }
            const MaterialGpu& gpu = owner_->materials_[index];
            return {Result::success(),
                    renderer::RenderMaterialResource{owner_->pipeline_, gpu.binding_sets[owner_->current_slot_], 1,
                                                     gpu.graph_texture, gpu.parameter_buffers[owner_->current_slot_],
                                                     gpu.parameter_desc, gpu.graph_parameter, 0,
                                                     sizeof(resource::TextureMaterialParameters)}};
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderObjectResource> resolve_object(
            const renderer::RenderInstance& instance, const renderer::RenderView& view) const override {
            if (owner_ == nullptr) {
                return {foundation::invalid_state("Editor scene resolver is detached"), {}};
            }
            const uint32_t object_slot = static_cast<uint32_t>(instance.object.value() % kMaxObjects);
            const renderer::RenderObjectConstants constants{
                view.camera.view_projection() * instance.world_transform,
                math::identity(),
            };
            const uint32_t frame_slot = owner_->frame_context_->frame_slot_index();
            const BufferHandle buffer = owner_->object_buffers_[frame_slot][object_slot];
            if (const Result result = owner_->device_->write_buffer(buffer, 0, std::as_bytes(std::span{&constants, 1}));
                !result) {
                return {result, {}};
            }
            return {Result::success(), owner_->object_resources_[frame_slot][object_slot]};
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderLightingResource> resolve_lighting(
            const renderer::RenderSceneSnapshot& snapshot, const renderer::RenderView&) const override {
            if (owner_ == nullptr) {
                return {foundation::invalid_state("Editor scene resolver is detached"), {}};
            }
            const auto constants = renderer::make_render_lighting_constants(snapshot.lights);
            if (!constants) {
                return {constants.result, {}};
            }
            const uint32_t frame_slot = owner_->frame_context_->frame_slot_index();
            return {Result::success(), renderer::RenderLightingResource{owner_->lighting_sets_[frame_slot], 2,
                                                                        owner_->lighting_buffers_[frame_slot],
                                                                        owner_->lighting_desc_, owner_->lighting_graph_,
                                                                        0, sizeof(renderer::RenderLightingConstants)}};
        }

        Impl* owner_{nullptr};
    };

    Device* device_{nullptr};
    reflection::TypeRegistry reflection_registry_{};
    pipeline::PipelineRegistry registry_{};
    pipeline::PipelineScheduler scheduler_{};
    std::unique_ptr<render_graph::FrameContext> frame_context_{};
    std::unique_ptr<render_graph::RenderGraph> render_graph_{};
    renderer::Renderer renderer_{};
    pipeline::PipelineScopeHost frame_host_{pipeline::PipelineScope::kFrame,
                                            pipeline::PipelineInstanceId("Editor.Scene.Frame")};
    std::unique_ptr<engine::EngineFrameOrchestrator> orchestrator_{};
    ecs::WorldManager world_manager_{};
    std::shared_ptr<ecs::World> edit_state_{};
    std::shared_ptr<ecs::World> runtime_state_{};
    std::unique_ptr<world::WorldScope> edit_scope_{};
    std::unique_ptr<world::WorldScope> runtime_scope_{};
    world::WorldScope* active_scope_{nullptr};
    scene::SceneAsset scene_asset_{};
    EditorProjectManager project_manager_{};
    EditorAssetDatabase asset_database_{};
    asset_pipeline::EditorAssetPipeline asset_pipeline_{};
    bool asset_pipeline_ready_{false};
    scene::SceneInstance edit_instance_{};
    scene::SceneInstance runtime_instance_{};
    scene::SceneEntityId selected_entity_{kLeftMeshEntity};
    std::vector<scene::SceneEntityId> selected_entities_{kLeftMeshEntity};
    scene::SceneEntityId active_camera_scene_{kCameraEntity};
    ecs::EntityId edit_camera_runtime_{};
    ecs::EntityId runtime_camera_runtime_{};
    std::filesystem::path scene_path_{"editor_scene.aegscene"};
    std::string diagnostic_{};
    bool dirty_{false};
    EditorSceneRuntime::Mode mode_{EditorSceneRuntime::Mode::kEdit};
    uint32_t viewport_width_{640};
    uint32_t viewport_height_{360};
    uint32_t requested_width_{640};
    uint32_t requested_height_{360};
    bool target_resize_pending_{true};
    bool initialized_{false};
    bool materials_ready_{false};
    bool target_has_previous_frame_{false};
    uint32_t current_slot_{0};
    std::vector<render_graph::RenderStateLinkOverride> render_state_link_overrides_{};
    std::vector<render_graph::RenderStateBlendOverride> render_state_blend_overrides_{};
    pipeline::PipelinePlanHandle project_renderer_pipeline_{};
    EditTransaction transform_edit_{};
    EditorCommand component_edit_command_{};
    bool component_edit_active_{false};
    std::vector<EditorCommand> history_{};
    std::size_t history_cursor_{0};
    std::size_t clean_history_cursor_{0};
    bool scene_dirty_base_{false};
    float runtime_demo_time_{0.0f};

    shader::ShaderManager* shader_manager_{nullptr};
    std::unique_ptr<shader::ShaderManager> shader_manager_owner_{};
    shader::ShaderPipelineLayout shader_layout_{};
    shader::ShaderPipelineLayout state_composite_layout_{};
    BindingLayoutHandle object_layout_{};
    BindingLayoutHandle material_layout_{};
    BindingLayoutHandle lighting_layout_{};
    BindingLayoutHandle state_composite_binding_layout_{};
    rhi::PipelineHandle pipeline_{};
    std::array<rhi::PipelineHandle, 3> state_composite_pipelines_{};
    /** State-composite descriptors are valid only until their frame has been submitted. */
    std::vector<BindingSetHandle> state_composite_sets_{};
    std::array<renderer::RenderMeshResource, 2> meshes_{};
    std::array<BufferHandle, 2> mesh_vertex_buffers_{};
    std::array<BufferDesc, 2> mesh_vertex_descs_{};
    BufferHandle index_buffer_{};
    BufferDesc index_buffer_desc_{};
    std::array<std::array<BufferHandle, kMaxObjects>, kFramesInFlight> object_buffers_{};
    std::array<std::array<renderer::RenderObjectResource, kMaxObjects>, kFramesInFlight> object_resources_{};
    std::array<BufferHandle, kFramesInFlight> lighting_buffers_{};
    std::array<BindingSetHandle, kFramesInFlight> lighting_sets_{};
    BufferDesc lighting_desc_{};
    pipeline::RenderGraphBuffer lighting_graph_{};
    std::array<MaterialGpu, kMaterialCount> materials_{};
    std::unordered_map<uint64_t, ImportedMeshGpu> imported_meshes_{};
    std::unordered_map<uint64_t, ImportedTextureGpu> imported_textures_{};
    std::unordered_map<uint64_t, MaterialGpu> imported_materials_{};
    std::unordered_map<uint64_t, pipeline::RenderGraphTexture> frame_texture_graphs_{};
    std::unordered_map<uint64_t, pipeline::RenderGraphBuffer> frame_vertex_graphs_{};
    std::unordered_map<uint64_t, pipeline::RenderGraphBuffer> frame_index_graphs_{};
    std::unordered_map<uint64_t, CachedFont> cached_fonts_{};
    ViewportReadbackState viewport_readback_{};
    TextureHandle color_texture_{};
    TextureViewHandle color_view_{};
    TextureViewHandle color_sampled_view_{};
    /** Exact RHI state exported to the editor UI after the latest scene frame. */
    ResourceState viewport_export_state_{sampled_state()};
    TextureDesc color_desc_{};
    TextureHandle depth_texture_{};
    TextureViewHandle depth_view_{};
    TextureDesc depth_desc_{};
    rhi::SamplerHandle viewport_sampler_{};
    std::unique_ptr<resource::TextureUploader> texture_uploader_{};
    std::unique_ptr<resource::ResourceUploader> mesh_uploader_{};
    std::filesystem::path shader_package_path_{};
    Resolver resolver_;

    explicit Impl() : resolver_(*this) {}

    [[nodiscard]] Result initialize_device(Device& device);
    [[nodiscard]] Result initialize_graphics_resources();
    /** Creates one descriptor set for a state-composite pass. */
    [[nodiscard]] foundation::ResultValue<BindingSetHandle> create_state_composite_set();
    /** Releases submitted state-composite descriptors before their transient source views are recycled. */
    [[nodiscard]] Result release_state_composite_sets();
    [[nodiscard]] Result create_static_meshes();
    [[nodiscard]] Result create_object_and_lighting_resources();
    [[nodiscard]] Result create_targets();
    [[nodiscard]] Result resize_targets();
    [[nodiscard]] Result create_orchestrator_for(world::WorldScope& scope);
    [[nodiscard]] Result rebuild_edit_world();
    [[nodiscard]] Result instantiate_world(scene::SceneAsset const& asset, std::shared_ptr<ecs::World>& state,
                                           std::unique_ptr<world::WorldScope>& scope, scene::SceneInstance& instance,
                                           ecs::EntityId& camera_runtime, std::string_view name);
    [[nodiscard]] Result make_default_scene();
    [[nodiscard]] Result make_empty_project_scene();
    [[nodiscard]] Result sync_scene_from_edit_world();
    [[nodiscard]] Result commit_edit_commands();
    [[nodiscard]] Result prepare_frame(const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result synchronize_asset_pipeline();
    [[nodiscard]] Result consume_asset(const asset_pipeline::CpuAsset& asset,
                                       const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result ensure_fast_path_assets(const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result upload_mesh_asset(const asset_pipeline::CpuAsset& asset,
                                           const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result upload_texture_asset(const asset_pipeline::CpuAsset& asset,
                                              const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result rebuild_material_asset(const asset_pipeline::CpuAsset& asset,
                                                const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result create_material_bindings(MaterialGpu& material, std::string_view name);
    [[nodiscard]] Result defer_material_bindings(MaterialGpu& material);
    [[nodiscard]] Result import_frame_asset_graphs(const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result refresh_material_texture_bindings(const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result prepare_text_assets(const ecs::WorldSnapshot& snapshot,
                                             const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result retire_missing_assets(const engine::EngineFramePreparationContext& context);
    [[nodiscard]] Result advance_runtime_demo();
    [[nodiscard]] Result destroy_resources();
    [[nodiscard]] foundation::ResultValue<ecs::EntityId> runtime_entity_in(const scene::SceneInstance& instance,
                                                                           scene::SceneEntityId entity) const;
    [[nodiscard]] const void* component_value_in(const scene::SceneInstance& instance, const ecs::World* state,
                                                 scene::SceneEntityId entity, ecs::ComponentTypeId type) const;
    [[nodiscard]] foundation::ResultValue<ViewportRay> make_viewport_ray(ViewportPoint pixel, uint32_t width,
                                                                         uint32_t height) const;
    [[nodiscard]] foundation::ResultValue<renderer::RenderView> make_edit_view(uint32_t width, uint32_t height) const;
    [[nodiscard]] foundation::Result enqueue_values(const std::vector<EditorComponentValue>& values);
    [[nodiscard]] foundation::Result commit_history(EditorCommand command);
    [[nodiscard]] foundation::Result apply_history_command(const EditorCommand& command, bool before);
    [[nodiscard]] foundation::Result reparent_entities(std::span<const scene::SceneEntityId> entities,
                                                       scene::SceneEntityId parent, std::size_t sibling_index,
                                                       HierarchyTransformPolicy transform_policy);
    [[nodiscard]] foundation::Result create_scene_entity(scene::SceneEntityId parent, std::string_view name);
    [[nodiscard]] foundation::Result rename_entity(scene::SceneEntityId entity, std::string_view name);
    [[nodiscard]] foundation::Result set_entity_tag(scene::SceneEntityId entity, std::string_view tag);
    [[nodiscard]] foundation::Result duplicate_entities(std::span<const scene::SceneEntityId> entities);
    [[nodiscard]] foundation::Result add_component(std::span<const scene::SceneEntityId> entities,
                                                   ecs::ComponentTypeId component);
    [[nodiscard]] foundation::Result remove_component(std::span<const scene::SceneEntityId> entities,
                                                      ecs::ComponentTypeId component);
    [[nodiscard]] foundation::Result remove_entities(std::span<const scene::SceneEntityId> entities,
                                                     scene::SceneEntityRemovalPolicy policy);
    [[nodiscard]] foundation::Result drop_resource(EditorResourceKind kind, uint64_t id);
    [[nodiscard]] foundation::Result reset_scene_for_project();
    void reset_history(bool scene_dirty);
    void refresh_dirty() noexcept;
};

// The implementation is initialized with a real device before these members are used. This helper keeps the
// public class small while retaining the existing FrameContext ownership model.
EditorSceneRuntime::EditorSceneRuntime() : impl_(nullptr) {}
EditorSceneRuntime::~EditorSceneRuntime() {
    static_cast<void>(shutdown());
}

scene::SceneEntityId EditorSceneRuntime::selected_entity() const noexcept {
    return impl_ == nullptr ? scene::SceneEntityId{} : impl_->selected_entity_;
}

std::span<const scene::SceneEntityId> EditorSceneRuntime::selected_entities() const noexcept {
    return impl_ == nullptr ? std::span<const scene::SceneEntityId>{} : impl_->selected_entities_;
}

bool EditorSceneRuntime::is_selected(scene::SceneEntityId entity) const noexcept {
    if (impl_ == nullptr || !entity.is_valid()) {
        return false;
    }
    return std::find(impl_->selected_entities_.begin(), impl_->selected_entities_.end(), entity) !=
           impl_->selected_entities_.end();
}

std::string_view EditorSceneRuntime::scene_name() const noexcept {
    return impl_ == nullptr ? std::string_view{} : impl_->scene_asset_.name();
}

const std::filesystem::path& EditorSceneRuntime::scene_path() const noexcept {
    static const std::filesystem::path kEmptyPath{};
    return impl_ == nullptr ? kEmptyPath : impl_->scene_path_;
}

bool EditorSceneRuntime::dirty() const noexcept {
    return impl_ != nullptr && impl_->dirty_;
}

EditorSceneRuntime::Mode EditorSceneRuntime::mode() const noexcept {
    return impl_ == nullptr ? Mode::kEdit : impl_->mode_;
}

std::string_view EditorSceneRuntime::diagnostic() const noexcept {
    return impl_ == nullptr ? std::string_view{} : impl_->diagnostic_;
}

bool EditorSceneRuntime::project_loaded() const noexcept {
    return impl_ != nullptr && impl_->project_manager_.loaded();
}

std::string_view EditorSceneRuntime::project_name() const noexcept {
    return impl_ == nullptr ? std::string_view{} : impl_->project_manager_.descriptor().name;
}

const std::filesystem::path& EditorSceneRuntime::project_root() const noexcept {
    static const std::filesystem::path kEmptyPath{};
    return impl_ == nullptr ? kEmptyPath : impl_->project_manager_.descriptor().root;
}

foundation::ResultValue<renderer::RenderView> EditorSceneRuntime::Impl::make_edit_view(uint32_t width,
                                                                                       uint32_t height) const {
    if (edit_scope_ == nullptr || !edit_camera_runtime_.is_valid()) {
        return {foundation::invalid_state("Editor edit World is not initialized"), {}};
    }
    if (width == 0 || height == 0) {
        return {foundation::invalid_argument("Viewport extent must be positive"), {}};
    }
    const auto snapshot = edit_scope_->latest_snapshot();
    if (!snapshot) {
        return {snapshot.result, {}};
    }
    return aeg::engine::components::make_render_view(
        *snapshot.value, aeg::engine::components::RuntimeRenderViewDesc{
                             edit_camera_runtime_, 0, {width, height}, 0xffffffffu, {}, {}, false, {}});
}

foundation::ResultValue<EditorSceneRuntime::Impl::ViewportRay> EditorSceneRuntime::Impl::make_viewport_ray(
    ViewportPoint pixel, uint32_t width, uint32_t height) const {
    const auto view = make_edit_view(width, height);
    if (!view) {
        return {view.result, {}};
    }
    const auto snapshot = edit_scope_->latest_snapshot();
    if (!snapshot) {
        return {snapshot.result, {}};
    }
    const Transform* transform = snapshot.value->get<Transform>(edit_camera_runtime_);
    const Camera* parameters = snapshot.value->get<Camera>(edit_camera_runtime_);
    if (transform == nullptr || parameters == nullptr) {
        return {foundation::not_found("Editor viewport camera components are missing"), {}};
    }
    if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y) || pixel.x < 0.0f || pixel.y < 0.0f ||
        pixel.x >= static_cast<float>(width) || pixel.y >= static_cast<float>(height)) {
        return {foundation::invalid_argument("Viewport pixel is outside the image bounds"), {}};
    }
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float tangent = std::tan(parameters->vertical_fov_radians * 0.5f);
    const float ndc_x = pixel.x / static_cast<float>(width) * 2.0f - 1.0f;
    const float ndc_y = 1.0f - pixel.y / static_cast<float>(height) * 2.0f;
    const math::Vec3 local_direction{ndc_x * aspect * tangent, ndc_y * tangent, -1.0f};
    math::Vec3 direction = math::rotate(transform->rotation, local_direction);
    if (!math::try_normalize(direction, direction)) {
        return {foundation::invalid_argument("Editor viewport camera cannot produce a ray direction"), {}};
    }
    return {Result::success(), ViewportRay{transform->translation, direction}};
}

void EditorSceneRuntime::Impl::refresh_dirty() noexcept {
    dirty_ = scene_dirty_base_ || history_cursor_ != clean_history_cursor_ || transform_edit_.active ||
             component_edit_active_;
}

void EditorSceneRuntime::Impl::reset_history(bool scene_dirty) {
    history_.clear();
    history_cursor_ = 0;
    clean_history_cursor_ = 0;
    scene_dirty_base_ = scene_dirty;
    refresh_dirty();
}

foundation::Result EditorSceneRuntime::Impl::enqueue_values(const std::vector<EditorComponentValue>& values) {
    if (edit_scope_ == nullptr || edit_state_ == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    if (values.empty()) {
        return foundation::invalid_argument("Editor component update is empty");
    }
    struct QueuedValue final {
        ecs::EntityId entity{};
        ecs::ComponentTypeId component{};
        std::vector<std::byte> bytes{};
    };
    try {
        std::vector<QueuedValue> queued;
        queued.reserve(values.size());
        for (const EditorComponentValue& value : values) {
            const auto runtime = edit_instance_.runtime_entity(value.entity);
            if (!runtime) {
                return runtime.result;
            }
            const auto descriptor = edit_state_->find_component_descriptor(value.component);
            if (!descriptor) {
                return descriptor.result;
            }
            if (value.bytes.size() != descriptor.value.size) {
                return foundation::invalid_argument("Editor component payload size does not match ECS storage");
            }
            if (const Result result = validate_component_payload(value.component, value.bytes); !result) {
                return result;
            }
            queued.push_back(QueuedValue{runtime.value, value.component, value.bytes});
        }
        const Result result =
            edit_scope_->command_buffer().enqueue([queued = std::move(queued)](ecs::World& world) mutable {
                for (const QueuedValue& value : queued) {
                    const auto destination = world.get_component_data(value.entity, value.component);
                    if (!destination) {
                        return destination.result;
                    }
                    std::memcpy(destination.value, value.bytes.data(), value.bytes.size());
                }
                return Result::success();
            });
        if (result) {
            refresh_dirty();
        }
        return result;
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor component update allocation failed");
    }
}

foundation::Result EditorSceneRuntime::Impl::commit_history(EditorCommand command) {
    if (command.before.size() != command.after.size()) {
        return foundation::invalid_argument("Editor history command value counts do not match");
    }
    if (command.before_scene.has_value() != command.after_scene.has_value()) {
        return foundation::invalid_argument("Editor history scene snapshots are incomplete");
    }
    bool changed = false;
    for (std::size_t index = 0; index < command.before.size(); ++index) {
        if (command.before[index].entity != command.after[index].entity ||
            command.before[index].component != command.after[index].component ||
            command.before[index].bytes != command.after[index].bytes) {
            changed = true;
            break;
        }
    }
    if (command.before_scene.has_value()) {
        changed = true;
    }
    if (!changed) {
        return Result::success();
    }
    try {
        if (history_cursor_ < history_.size()) {
            history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_cursor_), history_.end());
        }
        history_.push_back(std::move(command));
        history_cursor_ = history_.size();
        refresh_dirty();
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor history allocation failed");
    }
}

foundation::Result EditorSceneRuntime::Impl::apply_history_command(const EditorCommand& command, bool before) {
    const std::optional<scene::SceneAsset>& snapshot = before ? command.before_scene : command.after_scene;
    if (snapshot.has_value()) {
        try {
            scene_asset_ = *snapshot;
        } catch (const std::bad_alloc&) {
            return foundation::out_of_memory("Editor history Scene Asset allocation failed");
        }
        if (command.rebuild_edit_world) {
            if (const Result result = rebuild_edit_world(); !result) {
                return result;
            }
        }
        selected_entities_.erase(
            std::remove_if(selected_entities_.begin(), selected_entities_.end(),
                           [this](scene::SceneEntityId entity) {
                               return std::find_if(scene_asset_.entities().begin(), scene_asset_.entities().end(),
                                                   [entity](const auto& record) { return record.id == entity; }) ==
                                      scene_asset_.entities().end();
                           }),
            selected_entities_.end());
        if (std::find(selected_entities_.begin(), selected_entities_.end(), selected_entity_) ==
            selected_entities_.end()) {
            selected_entity_ = selected_entities_.empty() ? scene::SceneEntityId{} : selected_entities_.front();
        }
    }
    if (command.before.empty() && command.after.empty()) {
        refresh_dirty();
        return Result::success();
    }
    return enqueue_values(before ? command.before : command.after);
}

const reflection::TypeRegistry& EditorSceneRuntime::reflection_registry() const noexcept {
    static const reflection::TypeRegistry kEmptyRegistry{};
    return impl_ == nullptr ? kEmptyRegistry : impl_->reflection_registry_;
}

std::vector<std::pair<std::string, pipeline::PipelinePlanHandle>> EditorSceneRuntime::pipeline_snapshots() const {
    std::vector<std::pair<std::string, pipeline::PipelinePlanHandle>> result;
    if (!impl_) return result;
    if (impl_->active_scope_) {
        auto plan = impl_->active_scope_->pipeline_host().active_plan_snapshot();
        if (plan) result.emplace_back("Game / World", std::move(plan));
    }
    if (!impl_->project_manager_.loaded()) {
        auto plan = impl_->frame_host_.active_plan_snapshot();
        if (plan) result.emplace_back("Game / Renderer", std::move(plan));
    } else if (impl_->project_renderer_pipeline_) {
        result.emplace_back("Game / Renderer", impl_->project_renderer_pipeline_);
    }
    return result;
}

foundation::Result EditorSceneRuntime::create_project_renderer_pipeline() {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (!impl_->project_manager_.loaded()) {
        return foundation::invalid_state("A game project must be open before creating its Renderer Pipeline");
    }
    if (impl_->project_renderer_pipeline_) {
        return Result::success();
    }
    const pipeline::PipelineId renderer_pipeline("RenderPipeline");
    const pipeline::FeaturePointId opaque_point("RenderPipeline.Opaque");
    const pipeline::FeatureInstanceId opaque_instance = pipeline::make_feature_instance_id(
        renderer::kOpaqueRenderFeatureId, renderer_pipeline, pipeline::PipelineScope::kFrame, opaque_point);
    const Result unbound = impl_->registry_.unbind_feature(opaque_instance);
    const bool removed_opaque = static_cast<bool>(unbound);
    if (!unbound && unbound.code != ResultCode::kNotFound) {
        return unbound;
    }
    const auto restore_opaque = [impl = impl_.get(), removed_opaque]() {
        if (!removed_opaque) {
            return;
        }
        static_cast<void>(renderer::bind_opaque_render_feature(impl->registry_));
        if (impl->frame_host_.rebuild(impl->registry_)) {
            static_cast<void>(impl->frame_host_.apply_pending_plan());
        }
    };
    if (const Result result = impl_->frame_host_.rebuild(impl_->registry_); !result) {
        restore_opaque();
        return result;
    }
    if (const Result result = impl_->frame_host_.apply_pending_plan(); !result) {
        restore_opaque();
        return result;
    }
    const auto runtime_plan = impl_->frame_host_.active_plan_snapshot();
    if (!runtime_plan) {
        restore_opaque();
        return foundation::invalid_state("The renderer has no compiled Pipeline plan");
    }
    const auto present = std::find_if(runtime_plan->nodes.begin(), runtime_plan->nodes.end(), [](const auto& node) {
        return node.phase == pipeline::EnginePhase::kPresent;
    });
    if (present == runtime_plan->nodes.end()) {
        restore_opaque();
        return foundation::invalid_state("The renderer Pipeline has no Present output stage");
    }
    try {
        auto plan = std::make_shared<pipeline::PipelinePlan>();
        plan->revision = runtime_plan->revision;
        plan->pipeline = pipeline::PipelineId("Project.RendererPipeline");
        plan->instance = pipeline::PipelineInstanceId("Project.RendererPipeline.Editor");
        plan->scope = pipeline::PipelineScope::kFrame;
        plan->nodes.push_back(*present);
        plan->topological_order.push_back(0);
        impl_->project_renderer_pipeline_ = std::move(plan);
    } catch (const std::bad_alloc&) {
        restore_opaque();
        return foundation::out_of_memory("Project Renderer Pipeline allocation failed");
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::add_pipeline_runtime_ordering(PipelineTarget target, uint32_t source_node,
                                                                      uint32_t destination_node) {
    if (impl_ == nullptr || !impl_->initialized_) return foundation::invalid_state("Editor scene runtime is not initialized");
    if (target == PipelineTarget::kRenderer) return impl_->frame_host_.add_runtime_ordering(source_node, destination_node);
    if (impl_->active_scope_ == nullptr) return foundation::invalid_state("No active World pipeline is available");
    return impl_->active_scope_->pipeline_host().add_runtime_ordering(source_node, destination_node);
}

foundation::Result EditorSceneRuntime::remove_pipeline_runtime_ordering(PipelineTarget target, uint32_t source_node,
                                                                         uint32_t destination_node) {
    if (impl_ == nullptr || !impl_->initialized_) return foundation::invalid_state("Editor scene runtime is not initialized");
    if (target == PipelineTarget::kRenderer) return impl_->frame_host_.remove_runtime_ordering(source_node, destination_node);
    if (impl_->active_scope_ == nullptr) return foundation::invalid_state("No active World pipeline is available");
    return impl_->active_scope_->pipeline_host().remove_runtime_ordering(source_node, destination_node);
}

foundation::Result EditorSceneRuntime::add_render_state_link(uint32_t source_node, uint32_t destination_node) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    const auto plan = impl_->frame_host_.active_plan_snapshot();
    if (!plan || source_node >= plan->nodes.size() || destination_node >= plan->nodes.size() ||
        source_node == destination_node) {
        return foundation::invalid_argument("Render-state link endpoints are invalid");
    }
    const auto contribution_for_node = [&plan](uint32_t node) -> const pipeline::PipelineFeatureContributionRecord* {
        const uint64_t stage_index = plan->nodes[node].definition_stage_index;
        const auto found = std::find_if(plan->feature_contributions.begin(), plan->feature_contributions.end(),
                                        [stage_index](const auto& contribution) {
                                            return contribution.domain == pipeline::FeatureDomain::kRender &&
                                                   stage_index >= contribution.first_stage_index &&
                                                   stage_index < contribution.first_stage_index + contribution.stage_count;
                                        });
        return found == plan->feature_contributions.end() ? nullptr : &*found;
    };
    const auto source_contribution = contribution_for_node(source_node);
    const bool destination_accepts_state = plan->nodes[destination_node].phase == pipeline::EnginePhase::kPresent ||
                                           contribution_for_node(destination_node) != nullptr;
    if (source_contribution == nullptr || !destination_accepts_state) {
        return foundation::invalid_argument("State Link requires a RenderFeature source and a RenderFeature or Present target");
    }
    const auto previous = impl_->render_state_link_overrides_;
    const auto found = std::find_if(impl_->render_state_link_overrides_.begin(), impl_->render_state_link_overrides_.end(),
                                    [source_node, destination_node](const auto& link) {
                                        return link.source_node == source_node && link.destination_node == destination_node;
                                    });
    if (found == impl_->render_state_link_overrides_.end()) {
        impl_->render_state_link_overrides_.push_back({source_node, destination_node, true});
    } else {
        found->connected = true;
    }
    if (render_graph::build_render_state_flow(*plan, impl_->render_state_link_overrides_).has_cycle) {
        impl_->render_state_link_overrides_ = previous;
        return foundation::invalid_argument("Render-state link would create a cycle");
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::remove_render_state_link(uint32_t source_node, uint32_t destination_node) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    const auto plan = impl_->frame_host_.active_plan_snapshot();
    if (!plan || source_node >= plan->nodes.size() || destination_node >= plan->nodes.size() ||
        source_node == destination_node) {
        return foundation::invalid_argument("Render-state link endpoints are invalid");
    }
    const auto found = std::find_if(impl_->render_state_link_overrides_.begin(), impl_->render_state_link_overrides_.end(),
                                    [source_node, destination_node](const auto& link) {
                                        return link.source_node == source_node && link.destination_node == destination_node;
                                    });
    if (found == impl_->render_state_link_overrides_.end()) {
        impl_->render_state_link_overrides_.push_back({source_node, destination_node, false});
    } else {
        found->connected = false;
    }
    return Result::success();
}

std::vector<render_graph::RenderStateLinkOverride> EditorSceneRuntime::render_state_link_overrides() const {
    return impl_ == nullptr ? std::vector<render_graph::RenderStateLinkOverride>{} : impl_->render_state_link_overrides_;
}

foundation::Result EditorSceneRuntime::set_render_state_blend(uint32_t node, pipeline::RenderStateBlendMode blend_mode) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    const auto plan = impl_->frame_host_.active_plan_snapshot();
    if (!plan || node >= plan->nodes.size() || static_cast<uint32_t>(blend_mode) >
                                                 static_cast<uint32_t>(pipeline::RenderStateBlendMode::kAdd)) {
        return foundation::invalid_argument("Render-state blend target is invalid");
    }
    const uint64_t stage_index = plan->nodes[node].definition_stage_index;
    bool writes_state = false;
    for (const auto& contribution : plan->feature_contributions) {
        if (contribution.domain != pipeline::FeatureDomain::kRender || stage_index < contribution.first_stage_index ||
            stage_index >= contribution.first_stage_index + contribution.stage_count) {
            continue;
        }
        const auto feature = impl_->registry_.find_feature(contribution.feature);
        writes_state = feature && feature.value->uses_render_state_output();
        break;
    }
    if (!writes_state) {
        return foundation::invalid_argument("Only a render-state output Feature can select a blend mode");
    }
    const auto found = std::find_if(impl_->render_state_blend_overrides_.begin(),
                                    impl_->render_state_blend_overrides_.end(), [node](const auto& override) {
                                        return override.node == node;
                                    });
    if (found == impl_->render_state_blend_overrides_.end()) {
        impl_->render_state_blend_overrides_.push_back({node, blend_mode});
    } else {
        found->blend_mode = blend_mode;
    }
    return Result::success();
}

std::vector<render_graph::RenderStateBlendOverride> EditorSceneRuntime::render_state_blend_overrides() const {
    return impl_ == nullptr ? std::vector<render_graph::RenderStateBlendOverride>{} : impl_->render_state_blend_overrides_;
}

foundation::Result EditorSceneRuntime::initialize(rhi::Device& device,
                                                  const std::filesystem::path& shader_package_path) {
    if (impl_ != nullptr && impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is already initialized");
    }
    try {
        impl_ = std::make_unique<Impl>();
        impl_->shader_package_path_ = shader_package_path;
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor scene runtime allocation failed");
    }
    return impl_->initialize_device(device);
}

foundation::Result EditorSceneRuntime::Impl::initialize_device(Device& device) {
    device_ = &device;
    // Reconstruct the device-borrowing members only after the actual Device exists.
    frame_context_ =
        std::make_unique<render_graph::FrameContext>(device, render_graph::FrameContextDesc{kFramesInFlight});
    render_graph_ = std::make_unique<render_graph::RenderGraph>(device);
    mesh_uploader_ = std::make_unique<resource::ResourceUploader>(device);
    texture_uploader_ = std::make_unique<resource::TextureUploader>(device);

    if (const Result result = aeg::engine::components::register_render_component_reflection(reflection_registry_);
        !result) {
        return result;
    }
    if (const Result result = renderer::register_render_pipeline(registry_); !result) {
        return result;
    }
    if (const Result result = renderer::register_opaque_render_feature(registry_); !result) {
        return result;
    }
    if (const Result result = renderer::bind_opaque_render_feature(registry_); !result) {
        return result;
    }
    if (const Result result =
            registry_.register_feature([] { return std::make_unique<EditorViewportReadbackFeature>(); });
        !result) {
        return result;
    }
    if (const Result result = registry_.bind_feature({
            .feature = kEditorViewportReadbackFeatureId,
            .pipeline = pipeline::PipelineId("RenderPipeline"),
            .scope = pipeline::PipelineScope::kFrame,
            .point = pipeline::FeaturePointId("RenderPipeline.Opaque"),
            .priority = 100,
            .enabled = true,
        });
        !result) {
        return result;
    }
    if (const auto extractor = aeg::engine::components::register_mesh_renderer_extractor(renderer_); !extractor) {
        return extractor.result;
    }
    if (const auto extractor = aeg::engine::components::register_text_renderer_extractor(renderer_); !extractor) {
        return extractor.result;
    }
    if (const auto extractor = aeg::engine::components::register_directional_light_extractor(renderer_); !extractor) {
        return extractor.result;
    }
    if (const Result result = frame_host_.bind_pipeline(pipeline::PipelineId("RenderPipeline")); !result) {
        return result;
    }
    if (const Result result = initialize_graphics_resources(); !result) {
        return result;
    }
    if (const Result result = make_default_scene(); !result) {
        return result;
    }
    if (const Result result = rebuild_edit_world(); !result) {
        return result;
    }
    reset_history(true);
    initialized_ = true;
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::initialize_graphics_resources() {
    std::filesystem::path package_path = shader_package_path_;
#ifdef AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH
    std::error_code package_error;
    if (package_path.empty() || !std::filesystem::is_regular_file(package_path, package_error)) {
        package_path = AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH;
    }
#endif
    if (package_path.empty()) {
        return foundation::invalid_state("Opaque shader package path is not configured for the editor");
    }
    auto program = shader::load_shader_program(package_path);
    if (!program) {
        return program.result;
    }
    shader_manager_owner_ = std::make_unique<shader::ShaderManager>(*device_);
    shader_manager_ = shader_manager_owner_.get();
    auto layout = shader_manager_->build_pipeline_layout(program.value, DebugName{"Editor.Opaque.Layout"});
    if (!layout) {
        return layout.result;
    }
    shader_layout_ = std::move(layout.value);
    auto object_layout = shader_manager_->binding_layout(shader_layout_, 0);
    auto material_layout = shader_manager_->binding_layout(shader_layout_, 1);
    auto lighting_layout = shader_manager_->binding_layout(shader_layout_, 2);
    if (!object_layout || !material_layout || !lighting_layout) {
        return foundation::invalid_state("Editor opaque binding layouts are incomplete");
    }
    object_layout_ = object_layout.value;
    material_layout_ = material_layout.value;
    lighting_layout_ = lighting_layout.value;
    const std::array<VertexBindingDesc, 1> bindings{{{0, sizeof(Vertex), VertexInputRate::kVertex}}};
    const std::array<VertexAttributeDesc, 3> attributes{{
        {0, 0, Format::kRgba32Float, 0},
        {1, 0, Format::kRg32Float, sizeof(float) * 4u},
        {2, 0, Format::kRgb32Float, sizeof(float) * 6u},
    }};
    const std::array<Format, 1> color_formats{{Format::kRgba8Unorm}};
    const std::array<rhi::BlendAttachmentDesc, 1> blend_attachments{
        {{true, rhi::BlendFactor::kSourceAlpha, rhi::BlendFactor::kOneMinusSourceAlpha, rhi::BlendOp::kAdd,
          rhi::BlendFactor::kOne, rhi::BlendFactor::kOneMinusSourceAlpha, rhi::BlendOp::kAdd,
          rhi::ColorComponent::kAll}}};
    GraphicsPipelineState graphics{};
    graphics.vertex_bindings = bindings;
    graphics.vertex_attributes = attributes;
    graphics.topology = PrimitiveTopology::kTriangleList;
    graphics.cull_mode = CullMode::kNone;
    graphics.depth_test_enable = true;
    graphics.depth_write_enable = true;
    graphics.color_formats = color_formats;
    graphics.depth_stencil_format = Format::kD32Float;
    graphics.blend_attachments = blend_attachments;
    auto pipeline = shader_manager_->create_pipeline(
        program.value, shader_layout_,
        shader::ShaderPipelineDesc{PipelineType::kGraphics, graphics, DebugName{"Editor.Opaque.Pipeline"}});
    if (!pipeline) {
        return pipeline.result;
    }
    pipeline_ = pipeline.value;

#ifndef AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH
    return foundation::invalid_state("Render-state composite shader package path is not configured for the editor");
#else
    auto composite_program = shader::load_shader_program(AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH);
    if (!composite_program) {
        return composite_program.result;
    }
    auto composite_layout =
        shader_manager_->build_pipeline_layout(composite_program.value, DebugName{"Editor.StateComposite.Layout"});
    if (!composite_layout) {
        return composite_layout.result;
    }
    state_composite_layout_ = std::move(composite_layout.value);
    const auto composite_binding_layout = shader_manager_->binding_layout(state_composite_layout_, 0);
    if (!composite_binding_layout) {
        return composite_binding_layout.result;
    }
    state_composite_binding_layout_ = composite_binding_layout.value;
    const std::array<Format, 1> composite_color_formats{{Format::kRgba8Unorm}};
    const auto create_composite_pipeline =
        [this, &composite_program, &composite_color_formats](pipeline::RenderStateBlendMode mode,
                                                              std::string_view name) -> foundation::ResultValue<rhi::PipelineHandle> {
        rhi::BlendAttachmentDesc blend{};
        blend.color_write_mask = rhi::ColorComponent::kAll;
        switch (mode) {
            case pipeline::RenderStateBlendMode::kReplace:
                break;
            case pipeline::RenderStateBlendMode::kOver:
                blend.blend_enable = true;
                blend.source_color_factor = rhi::BlendFactor::kSourceAlpha;
                blend.destination_color_factor = rhi::BlendFactor::kOneMinusSourceAlpha;
                blend.source_alpha_factor = rhi::BlendFactor::kOne;
                blend.destination_alpha_factor = rhi::BlendFactor::kOneMinusSourceAlpha;
                break;
            case pipeline::RenderStateBlendMode::kAdd:
                blend.blend_enable = true;
                blend.source_color_factor = rhi::BlendFactor::kOne;
                blend.destination_color_factor = rhi::BlendFactor::kOne;
                blend.source_alpha_factor = rhi::BlendFactor::kOne;
                blend.destination_alpha_factor = rhi::BlendFactor::kOne;
                break;
        }
        const std::array<rhi::BlendAttachmentDesc, 1> blend_attachments{{blend}};
        GraphicsPipelineState graphics{};
        graphics.topology = PrimitiveTopology::kTriangleList;
        graphics.cull_mode = CullMode::kNone;
        graphics.color_formats = composite_color_formats;
        graphics.blend_attachments = blend_attachments;
        return shader_manager_->create_pipeline(
            composite_program.value, state_composite_layout_,
            shader::ShaderPipelineDesc{PipelineType::kGraphics, graphics, DebugName{name}});
    };
    constexpr std::array<pipeline::RenderStateBlendMode, 3> kCompositeModes{
        pipeline::RenderStateBlendMode::kReplace, pipeline::RenderStateBlendMode::kOver,
        pipeline::RenderStateBlendMode::kAdd};
    constexpr std::array<std::string_view, 3> kCompositeNames{
        "Editor.StateComposite.Replace", "Editor.StateComposite.Over", "Editor.StateComposite.Add"};
    for (uint32_t index = 0; index < kCompositeModes.size(); ++index) {
        auto composite_pipeline = create_composite_pipeline(kCompositeModes[index], kCompositeNames[index]);
        if (!composite_pipeline) {
            return composite_pipeline.result;
        }
        state_composite_pipelines_[index] = composite_pipeline.value;
    }
#endif
    if (const Result result = create_static_meshes(); !result) {
        return result;
    }
    return create_object_and_lighting_resources();
}

foundation::ResultValue<BindingSetHandle> EditorSceneRuntime::Impl::create_state_composite_set() {
    if (!state_composite_binding_layout_.is_valid() || !viewport_sampler_.is_valid()) {
        return {foundation::invalid_state("Render-state composite bindings are not initialized"), {}};
    }
    auto set = device_->create_binding_set(
        BindingSetDesc{state_composite_binding_layout_, BindingSetLifetime::kPersistent,
                       DebugName{"Editor.StateComposite.Set"}});
    if (!set) {
        return set;
    }
    try {
        state_composite_sets_.push_back(set.value);
    } catch (const std::bad_alloc&) {
        static_cast<void>(device_->destroy_binding_set(set.value));
        return {foundation::out_of_memory("Render-state composite descriptor tracking allocation failed"), {}};
    }
    return set;
}

foundation::Result EditorSceneRuntime::Impl::release_state_composite_sets() {
    Result first_result = Result::success();
    for (BindingSetHandle set : state_composite_sets_) {
        if (!set.is_valid()) {
            continue;
        }
        const Result result = device_->destroy_binding_set(set);
        if (!result && result.code != foundation::ResultCode::kNotFound && first_result) {
            first_result = result;
        }
    }
    state_composite_sets_.clear();
    return first_result;
}

foundation::Result EditorSceneRuntime::Impl::create_static_meshes() {
    const std::array<std::span<const std::byte>, 2> vertices{std::as_bytes(std::span{kQuadVertices}),
                                                             std::as_bytes(std::span{kMirroredQuadVertices})};
    for (uint32_t index = 0; index < 2; ++index) {
        BufferDesc desc{};
        desc.size = vertices[index].size();
        desc.capabilities = BufferCapability::kVertex;
        desc.memory = MemoryClass::kUpload;
        desc.initial_usage = ResourceUsage::kVertexInput;
        desc.initial_stages = PipelineStage::kVertexInput;
        desc.initial_queue = QueueClass::kGraphics;
        desc.name = DebugName{index == 0 ? "Editor.Mesh.VertexA" : "Editor.Mesh.VertexB"};
        auto buffer = device_->create_buffer(desc);
        if (!buffer) {
            return buffer.result;
        }
        if (const Result result = device_->write_buffer(buffer.value, 0, vertices[index]); !result) {
            return result;
        }
        mesh_vertex_buffers_[index] = buffer.value;
        mesh_vertex_descs_[index] = desc;
    }
    index_buffer_desc_ = BufferDesc{};
    index_buffer_desc_.size = sizeof(kQuadIndices);
    index_buffer_desc_.capabilities = BufferCapability::kIndex;
    index_buffer_desc_.memory = MemoryClass::kUpload;
    index_buffer_desc_.initial_usage = ResourceUsage::kIndexInput;
    index_buffer_desc_.initial_stages = PipelineStage::kVertexInput;
    index_buffer_desc_.initial_queue = QueueClass::kGraphics;
    index_buffer_desc_.name = DebugName{"Editor.Mesh.Indices"};
    auto indices = device_->create_buffer(index_buffer_desc_);
    if (!indices) {
        return indices.result;
    }
    if (const Result result = device_->write_buffer(indices.value, 0, std::as_bytes(std::span{kQuadIndices}));
        !result) {
        return result;
    }
    index_buffer_ = indices.value;
    for (uint32_t index = 0; index < 2; ++index) {
        meshes_[index] = {
            mesh_vertex_buffers_[index],
            mesh_vertex_descs_[index],
            index_buffer_,
            index_buffer_desc_,
            sizeof(Vertex),
            4,
            6,
            rhi::IndexFormat::kUint16,
            {0, Format::kRgba32Float, sizeof(float) * 4u, Format::kRg32Float, sizeof(float) * 6u, Format::kRgb32Float},
        };
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::create_object_and_lighting_resources() {
    const std::array<rhi::BufferCapability, 1> unused{};
    (void)unused;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        lighting_desc_ = make_uniform_desc(sizeof(renderer::RenderLightingConstants), "Editor.Lighting.Buffer");
        auto lighting_buffer = device_->create_buffer(lighting_desc_);
        if (!lighting_buffer) {
            return lighting_buffer.result;
        }
        lighting_buffers_[frame] = lighting_buffer.value;
        auto lighting_set = device_->create_binding_set(
            BindingSetDesc{lighting_layout_, BindingSetLifetime::kPersistent, DebugName{"Editor.Lighting.Set"}});
        if (!lighting_set) {
            return lighting_set.result;
        }
        BindingResource lighting_resource{};
        lighting_resource.type = BindingType::kUniformBuffer;
        lighting_resource.buffer_range =
            BufferRange{lighting_buffers_[frame], 0, sizeof(renderer::RenderLightingConstants)};
        const BindingWrite lighting_write{{2, 0}, 0, lighting_resource};
        if (const Result result = device_->update_binding_set(lighting_set.value, std::span{&lighting_write, 1});
            !result) {
            return result;
        }
        lighting_sets_[frame] = lighting_set.value;
        for (uint32_t object = 0; object < kMaxObjects; ++object) {
            const BufferDesc object_desc =
                make_uniform_desc(sizeof(renderer::RenderObjectConstants), "Editor.Object.Buffer");
            auto object_buffer = device_->create_buffer(object_desc);
            if (!object_buffer) {
                return object_buffer.result;
            }
            object_buffers_[frame][object] = object_buffer.value;
            auto object_set = device_->create_binding_set(
                BindingSetDesc{object_layout_, BindingSetLifetime::kPersistent, DebugName{"Editor.Object.Set"}});
            if (!object_set) {
                return object_set.result;
            }
            BindingResource object_resource{};
            object_resource.type = BindingType::kUniformBuffer;
            object_resource.buffer_range =
                BufferRange{object_buffers_[frame][object], 0, sizeof(renderer::RenderObjectConstants)};
            const BindingWrite object_write{{0, 0}, 0, object_resource};
            if (const Result result = device_->update_binding_set(object_set.value, std::span{&object_write, 1});
                !result) {
                return result;
            }
            object_resources_[frame][object] = {
                object_buffers_[frame][object],         object_desc, object_layout_, object_set.value, 0, 0,
                sizeof(renderer::RenderObjectConstants)};
        }
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::create_targets() {
    color_desc_ = make_target_desc(
        viewport_width_, viewport_height_, Format::kRgba8Unorm,
        TextureCapability::kColorAttachment | TextureCapability::kSampled | TextureCapability::kTransferSource |
            TextureCapability::kTransferDestination,
        "Editor.Scene.Color");
    auto color = device_->create_texture(color_desc_);
    if (!color) {
        return color.result;
    }
    color_texture_ = color.value;
    auto color_view = device_->create_texture_view({color_texture_,
                                                    TextureViewKind::kColorAttachment,
                                                    TextureViewDimension::kTex2D,
                                                    color_desc_.format,
                                                    {rhi::Aspect::kColor, 0, 1, 0, 1},
                                                    {},
                                                    DebugName{"Editor.Scene.ColorView"}});
    if (!color_view) {
        return color_view.result;
    }
    color_view_ = color_view.value;
    auto sampled_view = device_->create_texture_view({color_texture_,
                                                      TextureViewKind::kSampled,
                                                      TextureViewDimension::kTex2D,
                                                      color_desc_.format,
                                                      {rhi::Aspect::kColor, 0, 1, 0, 1},
                                                      {},
                                                      DebugName{"Editor.Scene.SampledView"}});
    if (!sampled_view) {
        return sampled_view.result;
    }
    color_sampled_view_ = sampled_view.value;
    depth_desc_ = make_target_desc(viewport_width_, viewport_height_, Format::kD32Float,
                                   TextureCapability::kDepthStencil, "Editor.Scene.Depth");
    auto depth = device_->create_texture(depth_desc_);
    if (!depth) {
        return depth.result;
    }
    depth_texture_ = depth.value;
    auto depth_view = device_->create_texture_view({depth_texture_,
                                                    TextureViewKind::kDepthStencil,
                                                    TextureViewDimension::kTex2D,
                                                    depth_desc_.format,
                                                    {rhi::Aspect::kDepth, 0, 1, 0, 1},
                                                    {},
                                                    DebugName{"Editor.Scene.DepthView"}});
    if (!depth_view) {
        return depth_view.result;
    }
    depth_view_ = depth_view.value;
    if (!viewport_sampler_.is_valid()) {
        auto sampler = device_->create_sampler({rhi::Filter::kLinear, rhi::Filter::kLinear, rhi::Filter::kLinear,
                                                rhi::AddressMode::kClampToEdge, rhi::AddressMode::kClampToEdge,
                                                rhi::AddressMode::kClampToEdge, 0.0f, 0.0f, 1000.0f, false, 1.0f,
                                                DebugName{"Editor.Scene.Sampler"}});
        if (!sampler) {
            return sampler.result;
        }
        viewport_sampler_ = sampler.value;
    }
    target_resize_pending_ = false;
    target_has_previous_frame_ = false;
    viewport_export_state_ = undefined_state();
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::resize_targets() {
    if (!target_resize_pending_) {
        return Result::success();
    }
    if (const Result result = device_->wait_idle(); !result) {
        return result;
    }
    if (const Result result = release_state_composite_sets(); !result) {
        return result;
    }
    // EngineFrameOrchestrator caches graph-build state. It cannot survive a
    // RenderGraph reset that invalidates the imported viewport resources.
    orchestrator_.reset();
    // The scene RenderGraph owns cached attachment and sampled views for the
    // imported viewport targets. Those views outlive a submitted frame, so
    // detach them before destroying the externally-owned SceneColor texture.
    // Waiting above guarantees that no command list can still consume them.
    render_graph_->reset();
    if (color_view_.is_valid()) {
        if (const Result result = device_->destroy_texture_view(color_view_); !result) {
            return result;
        }
        color_view_ = {};
    }
    if (color_sampled_view_.is_valid()) {
        if (const Result result = device_->destroy_texture_view(color_sampled_view_); !result) {
            return result;
        }
        color_sampled_view_ = {};
    }
    if (color_texture_.is_valid()) {
        if (const Result result = device_->destroy_texture(color_texture_); !result) {
            return result;
        }
        color_texture_ = {};
    }
    if (depth_view_.is_valid()) {
        if (const Result result = device_->destroy_texture_view(depth_view_); !result) {
            return result;
        }
        depth_view_ = {};
    }
    if (depth_texture_.is_valid()) {
        if (const Result result = device_->destroy_texture(depth_texture_); !result) {
            return result;
        }
        depth_texture_ = {};
    }
    viewport_width_ = requested_width_;
    viewport_height_ = requested_height_;
    if (const Result result = create_targets(); !result) {
        return result;
    }
    if (active_scope_ == nullptr) {
        return foundation::invalid_state("Editor scene has no active WorldScope after viewport resize");
    }
    return create_orchestrator_for(*active_scope_);
}

foundation::Result EditorSceneRuntime::Impl::make_default_scene() {
    scene_asset_ = scene::SceneAsset{};
    if (const Result result = scene_asset_.set_identity(scene::SceneAssetId{0xAE62026u, 1u}, "Editor Sample Scene", 1);
        !result) {
        return result;
    }
    const std::array<scene::SceneEntityRecord, 4> entities{{
        {kCameraEntity, {}, "Camera", scene::SceneEntityFlags::kNone},
        {kLightEntity, {}, "Directional Light", scene::SceneEntityFlags::kNone},
        {kLeftMeshEntity, {}, "Left Mesh", scene::SceneEntityFlags::kNone},
        {kRightMeshEntity, {}, "Right Mesh", scene::SceneEntityFlags::kNone},
    }};
    for (const auto& entity : entities) {
        if (const Result result = scene_asset_.add_entity(entity); !result) {
            return result;
        }
    }
    Transform camera_transform{};
    camera_transform.translation = {0.0f, 0.0f, 3.0f};
    Camera camera{};
    Transform light_transform{};
    DirectionalLight light{};
    Transform left_transform{};
    left_transform.translation = {-0.62f, 0.0f, 0.0f};
    left_transform.scale = {0.52f, 0.78f, 1.0f};
    MeshRenderer left_mesh{};
    left_mesh.mesh_asset_id = kMeshAssetA;
    left_mesh.material_asset_id = kMaterialAssetRed;
    Transform right_transform{};
    right_transform.translation = {0.62f, 0.0f, 0.0f};
    right_transform.scale = {0.52f, 0.78f, 1.0f};
    MeshRenderer right_mesh{};
    right_mesh.mesh_asset_id = kMeshAssetB;
    right_mesh.material_asset_id = kMaterialAssetGreen;
    const auto add = [this](scene::SceneEntityId entity, ecs::ComponentTypeId component, reflection::TypeId schema,
                            const void* value) {
        return scene_asset_.add_reflected_component(entity, component, reflection_registry_, schema, value);
    };
    if (const Result result = add(kCameraEntity, ecs::ComponentTypeId(aeg::engine::components::kTransformName),
                                  reflection::TypeId(aeg::engine::components::kTransformName), &camera_transform);
        !result) {
        return result;
    }
    if (const Result result = add(kCameraEntity, ecs::ComponentTypeId(aeg::engine::components::kCameraName),
                                  reflection::TypeId(aeg::engine::components::kCameraName), &camera);
        !result) {
        return result;
    }
    if (const Result result = add(kLightEntity, ecs::ComponentTypeId(aeg::engine::components::kTransformName),
                                  reflection::TypeId(aeg::engine::components::kTransformName), &light_transform);
        !result) {
        return result;
    }
    if (const Result result = add(kLightEntity, ecs::ComponentTypeId(aeg::engine::components::kDirectionalLightName),
                                  reflection::TypeId(aeg::engine::components::kDirectionalLightName), &light);
        !result) {
        return result;
    }
    if (const Result result = add(kLeftMeshEntity, ecs::ComponentTypeId(aeg::engine::components::kTransformName),
                                  reflection::TypeId(aeg::engine::components::kTransformName), &left_transform);
        !result) {
        return result;
    }
    if (const Result result = add(kLeftMeshEntity, ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName),
                                  reflection::TypeId(aeg::engine::components::kMeshRendererName), &left_mesh);
        !result) {
        return result;
    }
    if (const Result result = add(kRightMeshEntity, ecs::ComponentTypeId(aeg::engine::components::kTransformName),
                                  reflection::TypeId(aeg::engine::components::kTransformName), &right_transform);
        !result) {
        return result;
    }
    return add(kRightMeshEntity, ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName),
               reflection::TypeId(aeg::engine::components::kMeshRendererName), &right_mesh);
}

foundation::Result EditorSceneRuntime::Impl::make_empty_project_scene() {
    scene_asset_ = scene::SceneAsset{};
    active_camera_scene_ = {};
    project_renderer_pipeline_.reset();
    return scene_asset_.set_identity(scene::SceneAssetId{0xAE62026u, 2u}, "Main Scene", 1);
}

foundation::Result EditorSceneRuntime::Impl::instantiate_world(scene::SceneAsset const& asset,
                                                               std::shared_ptr<ecs::World>& state,
                                                               std::unique_ptr<world::WorldScope>& scope,
                                                               scene::SceneInstance& instance,
                                                               ecs::EntityId& camera_runtime,
                                                               std::string_view name) {
    const auto handle = world_manager_.create_world({std::string(name), true});
    if (!handle) {
        return handle.result;
    }
    const auto state_result = world_manager_.find(handle.value);
    if (!state_result) {
        return state_result.result;
    }
    state = state_result.value;
    if (const Result result = aeg::engine::components::register_render_component_bundle(*state, reflection_registry_);
        !result) {
        return result;
    }
    try {
        scope = std::make_unique<world::WorldScope>(state, registry_);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor World Scope allocation failed");
    }
    if (const Result result = scope->initialize(); !result) {
        return result;
    }
    const auto instance_result = asset.instantiate(*state, reflection_registry_);
    if (!instance_result) {
        return instance_result.result;
    }
    instance = std::move(instance_result.value);
    camera_runtime = {};
    const auto camera = instance.runtime_entity(active_camera_scene_);
    if (camera) {
        camera_runtime = camera.value;
    } else {
        const auto editor_camera = state->create_entity();
        if (!editor_camera) {
            return editor_camera.result;
        }
        Transform transform{};
        transform.translation = {0.0f, 0.0f, 3.0f};
        if (const auto result = state->emplace<Transform>(editor_camera.value, transform); !result) {
            return result.result;
        }
        if (const auto result = state->emplace<Camera>(editor_camera.value, Camera{}); !result) {
            return result.result;
        }
        camera_runtime = editor_camera.value;
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::create_orchestrator_for(world::WorldScope& scope) {
    render_graph_->reset();
    orchestrator_.reset();
    try {
        orchestrator_ =
            std::make_unique<engine::EngineFrameOrchestrator>(registry_, scheduler_, *frame_context_, *render_graph_);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor scene orchestrator allocation failed");
    }
    if (const Result result = orchestrator_->register_world_frame(scope, frame_host_, 0); !result) {
        return result;
    }
    return orchestrator_->initialize();
}

foundation::Result EditorSceneRuntime::Impl::rebuild_edit_world() {
    orchestrator_.reset();
    edit_scope_.reset();
    edit_state_.reset();
    const Result result = instantiate_world(scene_asset_, edit_state_, edit_scope_, edit_instance_,
                                            edit_camera_runtime_, "Editor.EditWorld");
    if (!result) {
        return result;
    }
    if (mode_ == EditorSceneRuntime::Mode::kEdit || mode_ == EditorSceneRuntime::Mode::kPause) {
        active_scope_ = edit_scope_.get();
        return create_orchestrator_for(*active_scope_);
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::commit_edit_commands() {
    if (edit_scope_ == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    if (const Result result = edit_scope_->apply_commands(edit_scope_->command_buffer()); !result) {
        return result;
    }
    if (const auto snapshot = edit_scope_->commit(); !snapshot) {
        return snapshot.result;
    }
    return sync_scene_from_edit_world();
}

foundation::Result EditorSceneRuntime::Impl::sync_scene_from_edit_world() {
    if (edit_scope_ == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    const auto snapshot_result = edit_scope_->latest_snapshot();
    if (!snapshot_result) {
        return snapshot_result.result;
    }
    scene::SceneAsset next(scene_asset_);
    next.clear();
    for (const auto& entity : scene_asset_.entities()) {
        if (const Result result = next.add_entity(entity); !result) {
            return result;
        }
    }
    const ecs::WorldSnapshot& snapshot = *snapshot_result.value;
    for (std::size_t component_index = 0; component_index < snapshot.component_type_count(); ++component_index) {
        const ecs::ComponentTypeId component_type = snapshot.component_type_at(component_index);
        const reflection::TypeId schema_type = snapshot.component_schema_type(component_type);
        if (!schema_type.is_valid()) {
            continue;
        }
        for (std::size_t value_index = 0; value_index < snapshot.component_value_count(component_type); ++value_index) {
            const ecs::EntityId runtime_entity = snapshot.component_entity_at(component_type, value_index);
            scene::SceneEntityId scene_entity{};
            for (const auto& binding : edit_instance_.entities()) {
                if (binding.runtime_entity == runtime_entity) {
                    scene_entity = binding.scene_entity;
                    break;
                }
            }
            const void* value = snapshot.component_value_at(component_type, value_index);
            if (!scene_entity.is_valid() || value == nullptr) {
                continue;
            }
            if (const Result result = next.add_reflected_component(scene_entity, component_type, reflection_registry_,
                                                                   schema_type, value);
                !result) {
                return result;
            }
        }
    }
    scene_asset_ = std::move(next);
    refresh_dirty();
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::advance_runtime_demo() {
    if (mode_ != EditorSceneRuntime::Mode::kPlay || runtime_scope_ == nullptr || runtime_state_ == nullptr) {
        return Result::success();
    }
    const auto runtime_entity = runtime_instance_.runtime_entity(kRightMeshEntity);
    if (!runtime_entity) {
        // The motion is only an editor-sample affordance. Project scenes do
        // not contain the sample entity and must still be able to enter Play.
        return runtime_entity.result.code == ResultCode::kNotFound ? Result::success() : runtime_entity.result;
    }
    runtime_demo_time_ += 1.0f / 60.0f;
    const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
    const float angle = std::sin(runtime_demo_time_ * 1.4f) * 0.35f;
    const Result queued = runtime_scope_->command_buffer().enqueue(
        [entity = runtime_entity.value, transform_type, angle](ecs::World& world) {
            const auto value = world.get_component_data(entity, transform_type);
            if (!value) {
                return value.result;
            }
            auto* transform = static_cast<Transform*>(value.value);
            transform->rotation = math::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, angle);
            return Result::success();
        });
    if (!queued) {
        return queued;
    }
    if (const Result result = runtime_scope_->apply_commands(runtime_scope_->command_buffer()); !result) {
        return result;
    }
    const auto snapshot = runtime_scope_->commit(frame_context_->frame_number());
    return snapshot ? Result::success() : snapshot.result;
}

foundation::Result EditorSceneRuntime::Impl::synchronize_asset_pipeline() {
    if (!project_manager_.loaded()) {
        asset_pipeline_.shutdown();
        asset_pipeline_ready_ = false;
        return Result::success();
    }
    const std::filesystem::path expected_derived_root = project_manager_.descriptor().root / ".aeg" / "DerivedData";
    if (asset_pipeline_ready_ && asset_pipeline_.derived_root() != expected_derived_root) {
        asset_pipeline_.shutdown();
        asset_pipeline_ready_ = false;
    }
    if (!asset_pipeline_ready_) {
        if (const Result result = asset_pipeline_.initialize(project_manager_.descriptor().root); !result) {
            return result;
        }
        asset_pipeline_ready_ = true;
    }
    return asset_pipeline_.synchronize(asset_database_.assets());
}

foundation::Result EditorSceneRuntime::Impl::defer_material_bindings(MaterialGpu& material) {
    if (frame_context_ == nullptr || !frame_context_->is_frame_active()) {
        return foundation::invalid_state("Material retirement requires an active FrameContext frame");
    }
    for (BindingSetHandle& set : material.binding_sets) {
        if (set.is_valid()) {
            if (const Result result = frame_context_->defer_destroy_binding_set(set); !result) {
                return result;
            }
            set = {};
        }
    }
    for (BufferHandle& buffer : material.parameter_buffers) {
        if (buffer.is_valid()) {
            if (const Result result = frame_context_->defer_destroy_buffer(buffer); !result) {
                return result;
            }
            buffer = {};
        }
    }
    if (material.sampler.is_valid()) {
        if (const Result result = frame_context_->defer_destroy_sampler(material.sampler); !result) {
            return result;
        }
        material.sampler = {};
    }
    material.parameter_desc = {};
    material.graph_parameter = {};
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::create_material_bindings(MaterialGpu& material, std::string_view name) {
    if (!material.sampled_view.is_valid()) {
        return foundation::invalid_argument("Material texture view is invalid");
    }
    auto sampler =
        device_->create_sampler({rhi::Filter::kLinear, rhi::Filter::kLinear, rhi::Filter::kLinear,
                                 rhi::AddressMode::kClampToEdge, rhi::AddressMode::kClampToEdge,
                                 rhi::AddressMode::kClampToEdge, 0.0f, 0.0f, 1000.0f, false, 1.0f, DebugName{name}});
    if (!sampler) {
        return sampler.result;
    }
    material.sampler = sampler.value;
    material.parameter_desc = make_uniform_desc(sizeof(resource::TextureMaterialParameters), name);
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        auto parameter = device_->create_buffer(material.parameter_desc);
        if (!parameter) {
            return parameter.result;
        }
        material.parameter_buffers[frame] = parameter.value;
        auto set = device_->create_binding_set(
            BindingSetDesc{material_layout_, BindingSetLifetime::kPersistent, DebugName{name}});
        if (!set) {
            return set.result;
        }
        const std::array<BindingWrite, 3> writes{
            BindingWrite{
                {1, 0}, 0, BindingResource{BindingType::kSampledTexture, material.sampled_view, {}, {}, {}, {}}},
            BindingWrite{{1, 1}, 0, BindingResource{BindingType::kSampler, {}, {}, {}, material.sampler, {}}},
            BindingWrite{{1, 2},
                         0,
                         BindingResource{BindingType::kUniformBuffer,
                                         {},
                                         {},
                                         BufferRange{material.parameter_buffers[frame], 0,
                                                     sizeof(resource::TextureMaterialParameters)},
                                         {},
                                         {}}},
        };
        if (const Result result = device_->update_binding_set(set.value, writes); !result) {
            return result;
        }
        material.binding_sets[frame] = set.value;
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::upload_mesh_asset(const asset_pipeline::CpuAsset& asset,
                                                               const engine::EngineFramePreparationContext& context) {
    if (asset.vertex_data.empty() || asset.index_data.empty() || asset.vertex_stride == 0 || asset.vertex_count == 0 ||
        asset.index_count == 0) {
        return foundation::invalid_argument("Imported mesh has no drawable vertex or index data");
    }
    const auto existing = imported_meshes_.find(asset.asset_id);
    if (existing != imported_meshes_.end() && existing->second.source_hash == asset.source_hash &&
        existing->second.render.is_valid() && (existing->second.from_baked_format || !asset.from_baked_format)) {
        return Result::success();
    }
    resource::MeshData mesh_data{
        std::span<const std::byte>(asset.vertex_data.data(), asset.vertex_data.size()),
        asset.vertex_stride,
        asset.vertex_count,
        std::span<const std::byte>(asset.index_data.data(), asset.index_data.size()),
        asset.indices_are_uint32 ? rhi::IndexFormat::kUint32 : rhi::IndexFormat::kUint16,
        asset.index_count,
    };
    const auto upload =
        mesh_uploader_->upload_mesh(context.frame_context, context.render_graph, mesh_data, "Editor.Asset.Mesh");
    if (!upload) {
        return upload.result;
    }
    ImportedMeshGpu replacement{};
    replacement.source_hash = asset.source_hash;
    replacement.from_baked_format = asset.from_baked_format;
    replacement.transient = asset.transient;
    replacement.gpu = upload.value.mesh;
    replacement.bounds_center = {asset.bounds_center[0], asset.bounds_center[1], asset.bounds_center[2]};
    replacement.bounds_radius = asset.bounds_radius;
    replacement.render = {
        replacement.gpu.vertex_buffer,
        make_asset_buffer_desc(replacement.gpu.vertex_buffer_size, BufferCapability::kVertex,
                               "Editor.Asset.Mesh.Vertex"),
        replacement.gpu.index_buffer,
        make_asset_buffer_desc(replacement.gpu.index_buffer_size, BufferCapability::kIndex, "Editor.Asset.Mesh.Index"),
        replacement.gpu.vertex_stride,
        replacement.gpu.vertex_count,
        replacement.gpu.index_count,
        replacement.gpu.index_format,
        {0, Format::kRgba32Float, sizeof(float) * 4u, Format::kRg32Float, sizeof(float) * 6u, Format::kRgb32Float},
        to_pipeline_handle(upload.value.vertex_resource),
        to_pipeline_handle(upload.value.index_resource),
    };
    if (existing != imported_meshes_.end()) {
        if (const Result result = mesh_uploader_->defer_destroy_mesh(context.frame_context, existing->second.gpu);
            !result) {
            static_cast<void>(mesh_uploader_->destroy_mesh(replacement.gpu));
            return result;
        }
        existing->second = std::move(replacement);
    } else {
        imported_meshes_.emplace(asset.asset_id, std::move(replacement));
    }
    frame_vertex_graphs_[asset.asset_id] = to_pipeline_handle(upload.value.vertex_resource);
    frame_index_graphs_[asset.asset_id] = to_pipeline_handle(upload.value.index_resource);
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::upload_texture_asset(
    const asset_pipeline::CpuAsset& asset, const engine::EngineFramePreparationContext& context) {
    if (asset.width == 0 || asset.height == 0 ||
        asset.pixels.size() != static_cast<std::size_t>(asset.width) * asset.height * 4u) {
        return foundation::invalid_argument("Imported texture has invalid RGBA8 pixel data");
    }
    const auto existing = imported_textures_.find(asset.asset_id);
    if (existing != imported_textures_.end() && existing->second.source_hash == asset.source_hash &&
        existing->second.gpu.is_valid() && (existing->second.from_baked_format || !asset.from_baked_format)) {
        return Result::success();
    }
    const TextureDesc desc = make_asset_texture_desc(asset.width, asset.height, "Editor.Asset.Texture");
    const resource::TextureSubresourceData subresource{
        std::span<const std::byte>(asset.pixels.data(), asset.pixels.size()), asset.width * 4u,
        asset.width * asset.height * 4u};
    const resource::TextureData texture_data{desc, std::span<const resource::TextureSubresourceData>(&subresource, 1)};
    const auto upload = texture_uploader_->upload_texture(context.frame_context, context.render_graph, texture_data,
                                                          "Editor.Asset.Texture");
    if (!upload) {
        return upload.result;
    }
    ImportedTextureGpu replacement{};
    replacement.source_hash = asset.source_hash;
    replacement.from_baked_format = asset.from_baked_format;
    replacement.transient = asset.transient;
    replacement.gpu = upload.value.texture;
    replacement.desc = desc;
    replacement.graph_texture = to_pipeline_handle(upload.value.target);
    if (existing != imported_textures_.end()) {
        if (const Result result = texture_uploader_->defer_destroy_texture(context.frame_context, existing->second.gpu);
            !result) {
            static_cast<void>(texture_uploader_->destroy_texture(replacement.gpu));
            return result;
        }
        existing->second = std::move(replacement);
    } else {
        imported_textures_.emplace(asset.asset_id, std::move(replacement));
    }
    frame_texture_graphs_[asset.asset_id] = to_pipeline_handle(upload.value.target);
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::rebuild_material_asset(
    const asset_pipeline::CpuAsset& asset, const engine::EngineFramePreparationContext& context) {
    MaterialGpu candidate{};
    candidate.asset_id = asset.asset_id;
    candidate.source_hash = asset.source_hash;
    candidate.from_baked_format = asset.from_baked_format;
    candidate.transient = asset.transient;
    candidate.texture_asset_id = asset.texture_asset_id;
    candidate.tint = asset.tint;
    const auto texture = imported_textures_.find(asset.texture_asset_id);
    const MaterialGpu& fallback = materials_[0];
    if (texture != imported_textures_.end() && texture->second.gpu.has_sampled_view()) {
        candidate.texture = texture->second.gpu.texture;
        candidate.sampled_view = texture->second.gpu.sampled_view;
        candidate.texture_desc = texture->second.desc;
    } else {
        candidate.texture = fallback.texture;
        candidate.sampled_view = fallback.sampled_view;
        candidate.texture_desc = fallback.texture_desc;
    }
    if (const Result result = create_material_bindings(candidate, "Editor.Asset.Material"); !result) {
        return result;
    }
    const auto existing = imported_materials_.find(asset.asset_id);
    if (existing != imported_materials_.end()) {
        if (const Result result = defer_material_bindings(existing->second); !result) {
            return result;
        }
        existing->second = std::move(candidate);
    } else {
        imported_materials_.emplace(asset.asset_id, std::move(candidate));
    }
    (void)context;
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::consume_asset(const asset_pipeline::CpuAsset& asset,
                                                           const engine::EngineFramePreparationContext& context) {
    const EditorAssetRecord* record = asset_database_.find(asset.asset_id);
    if (record == nullptr || record->content_hash != asset.source_hash) {
        return Result::success();
    }
    if (!asset.success) {
        diagnostic_ = "Asset " + record->relative_path + " failed to import: " + asset.diagnostic;
        return Result::success();
    }
    if (asset.kind == EditorAssetKind::kMesh) {
        return upload_mesh_asset(asset, context);
    }
    if (asset.kind == EditorAssetKind::kTexture) {
        return upload_texture_asset(asset, context);
    }
    if (asset.kind == EditorAssetKind::kMaterial) {
        return rebuild_material_asset(asset, context);
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::ensure_fast_path_assets(
    const engine::EngineFramePreparationContext& context) {
    if (!asset_pipeline_ready_) {
        return Result::success();
    }
    for (const EditorAssetRecord& record : asset_database_.assets()) {
        if (record.status != EditorAssetStatus::kImported ||
            (record.kind != EditorAssetKind::kMesh && record.kind != EditorAssetKind::kTexture &&
             record.kind != EditorAssetKind::kMaterial)) {
            continue;
        }
        bool already_loaded = false;
        if (record.kind == EditorAssetKind::kMesh) {
            const auto found = imported_meshes_.find(record.id);
            already_loaded = found != imported_meshes_.end() && found->second.source_hash == record.content_hash;
        } else if (record.kind == EditorAssetKind::kTexture) {
            const auto found = imported_textures_.find(record.id);
            already_loaded = found != imported_textures_.end() && found->second.source_hash == record.content_hash;
        } else {
            const auto found = imported_materials_.find(record.id);
            already_loaded = found != imported_materials_.end() && found->second.source_hash == record.content_hash;
        }
        if (already_loaded) {
            continue;
        }
        asset_pipeline::CpuAsset fast{};
        const Result result = asset_pipeline_.load_fast_path(record.id, fast);
        if (!result) {
            if ((record.kind == EditorAssetKind::kMesh && imported_meshes_.contains(record.id)) ||
                (record.kind == EditorAssetKind::kTexture && imported_textures_.contains(record.id)) ||
                (record.kind == EditorAssetKind::kMaterial && imported_materials_.contains(record.id))) {
                diagnostic_ = "Reimport failed for " + record.relative_path +
                              "; retaining last-good GPU resource: " + result.message;
            } else {
                diagnostic_ =
                    "Initial import failed for " + record.relative_path + "; using fallback: " + result.message;
            }
            continue;
        }
        if (const Result consume = consume_asset(fast, context); !consume) {
            if ((record.kind == EditorAssetKind::kMesh && imported_meshes_.contains(record.id)) ||
                (record.kind == EditorAssetKind::kTexture && imported_textures_.contains(record.id)) ||
                (record.kind == EditorAssetKind::kMaterial && imported_materials_.contains(record.id))) {
                diagnostic_ = "Reimport failed for " + record.relative_path +
                              "; retaining last-good GPU resource: " + consume.message;
            } else {
                diagnostic_ =
                    "Initial import failed for " + record.relative_path + "; using fallback: " + consume.message;
            }
        }
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::retire_missing_assets(
    const engine::EngineFramePreparationContext& context) {
    const auto should_retire = [this](uint64_t id, EditorAssetKind kind) {
        const auto* record = asset_database_.find(id);
        return record == nullptr || record->kind != kind || record->status == EditorAssetStatus::kMissing ||
               record->status == EditorAssetStatus::kUnsupported;
    };

    for (auto iterator = imported_meshes_.begin(); iterator != imported_meshes_.end();) {
        if (!iterator->second.transient && should_retire(iterator->first, EditorAssetKind::kMesh)) {
            frame_vertex_graphs_.erase(iterator->first);
            frame_index_graphs_.erase(iterator->first);
            if (const Result result = mesh_uploader_->defer_destroy_mesh(context.frame_context, iterator->second.gpu);
                !result) {
                return result;
            }
            iterator = imported_meshes_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = imported_textures_.begin(); iterator != imported_textures_.end();) {
        if (!iterator->second.transient && should_retire(iterator->first, EditorAssetKind::kTexture)) {
            frame_texture_graphs_.erase(iterator->first);
            if (const Result result =
                    texture_uploader_->defer_destroy_texture(context.frame_context, iterator->second.gpu);
                !result) {
                return result;
            }
            iterator = imported_textures_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = imported_materials_.begin(); iterator != imported_materials_.end();) {
        if (!iterator->second.transient && should_retire(iterator->first, EditorAssetKind::kMaterial)) {
            if (const Result result = defer_material_bindings(iterator->second); !result) {
                return result;
            }
            iterator = imported_materials_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::import_frame_asset_graphs(
    const engine::EngineFramePreparationContext& context) {
    for (auto& [id, mesh] : imported_meshes_) {
        if (!frame_vertex_graphs_.contains(id)) {
            const auto vertex = context.render_graph.import_buffer(
                mesh.gpu.vertex_buffer, mesh.render.vertex_buffer_desc,
                mesh.render.vertex_buffer_desc.initial_usage == ResourceUsage::kUndefined
                    ? undefined_state()
                    : ResourceState{mesh.render.vertex_buffer_desc.initial_usage, ShaderStage::kVertex,
                                    QueueClass::kGraphics, false, PipelineStage::kVertexInput});
            if (!vertex) {
                return vertex.result;
            }
            frame_vertex_graphs_[id] = to_pipeline_handle(vertex.value);
        }
        if (!frame_index_graphs_.contains(id)) {
            const auto index = context.render_graph.import_buffer(mesh.gpu.index_buffer, mesh.render.index_buffer_desc,
                                                                  undefined_state());
            if (!index) {
                return index.result;
            }
            frame_index_graphs_[id] = to_pipeline_handle(index.value);
        }
        mesh.render.vertex_resource = frame_vertex_graphs_[id];
        mesh.render.index_resource = frame_index_graphs_[id];
    }
    for (auto& [id, texture] : imported_textures_) {
        if (!frame_texture_graphs_.contains(id)) {
            const auto state = query_texture_initial_state(*device_, texture.gpu.texture);
            if (!state) {
                return state.result;
            }
            const auto imported = context.render_graph.import_texture(texture.gpu.texture, texture.gpu.sampled_view,
                                                                      texture.desc, state.value);
            if (!imported) {
                return imported.result;
            }
            frame_texture_graphs_[id] = to_pipeline_handle(imported.value);
        }
        texture.graph_texture = frame_texture_graphs_[id];
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::refresh_material_texture_bindings(
    const engine::EngineFramePreparationContext& context) {
    for (auto& [id, material] : imported_materials_) {
        TextureHandle texture = materials_[0].texture;
        TextureViewHandle sampled_view = materials_[0].sampled_view;
        TextureDesc texture_desc = materials_[0].texture_desc;
        const auto found = imported_textures_.find(material.texture_asset_id);
        if (found != imported_textures_.end() && found->second.gpu.has_sampled_view()) {
            texture = found->second.gpu.texture;
            sampled_view = found->second.gpu.sampled_view;
            texture_desc = found->second.desc;
        }
        if (material.sampled_view == sampled_view) {
            material.graph_texture =
                found != imported_textures_.end() ? found->second.graph_texture : materials_[0].graph_texture;
            continue;
        }
        MaterialGpu candidate = material;
        candidate.texture = texture;
        candidate.sampled_view = sampled_view;
        candidate.texture_desc = texture_desc;
        candidate.sampler = {};
        candidate.parameter_buffers = {};
        candidate.binding_sets = {};
        if (const Result result = create_material_bindings(candidate, "Editor.Asset.Material"); !result) {
            return result;
        }
        if (const Result result = defer_material_bindings(material); !result) {
            return result;
        }
        material = std::move(candidate);
        material.graph_texture =
            found != imported_textures_.end() ? found->second.graph_texture : materials_[0].graph_texture;
        (void)id;
    }
    (void)context;
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::prepare_text_assets(const ecs::WorldSnapshot& snapshot,
                                                                 const engine::EngineFramePreparationContext& context) {
    foundation::Result result = Result::success();
    snapshot.for_each<TextRenderer>([this, &context, &result](ecs::EntityId, const TextRenderer& text) {
        if (!result || !text.visible || text.text.empty()) {
            return;
        }
        asset_pipeline::CpuAsset font{};
        if (text.font_asset_id != 0 && asset_pipeline_ready_) {
            const Result loaded = asset_pipeline_.load_fast_path(text.font_asset_id, font);
            if (!loaded) {
                diagnostic_ = "Font import failed; using bundled fallback font: " + loaded.message;
                font = {};
            }
        }
        if (font.font_data.empty()) {
            const std::array<std::filesystem::path, 3> fallback_paths{
#ifdef AEG_ENGINE_SOURCE_ROOT
                std::filesystem::path(AEG_ENGINE_SOURCE_ROOT) / "ThirdParty" / "Fonts" / "Inter" / "Inter-Variable.ttf",
#endif
                std::filesystem::current_path() / "ThirdParty" / "Fonts" / "Inter" / "Inter-Variable.ttf",
                std::filesystem::path("ThirdParty/Fonts/Inter/Inter-Variable.ttf")};
            for (const auto& path : fallback_paths) {
                std::ifstream stream(path, std::ios::binary);
                if (!stream) {
                    continue;
                }
                const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                font.font_data.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                                      reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
                break;
            }
        }
        if (font.font_data.empty()) {
            result = foundation::not_found("No fallback font is available for TextRenderer");
            return;
        }
        const uint64_t font_hash =
            font.source_hash != 0 ? font.source_hash : combine_asset_hash(text.font_asset_id, font.font_data.size());
        CachedFont* cached = nullptr;
        const auto cached_it = cached_fonts_.find(text.font_asset_id);
        if (cached_it != cached_fonts_.end() && cached_it->second.source_hash == font_hash) {
            cached = &cached_it->second;
        } else {
            CachedFont replacement{};
            replacement.source_hash = font_hash;
            if (const Result atlas_result = asset_pipeline::build_font_atlas(
                    std::span<const std::byte>(font.font_data.data(), font.font_data.size()), replacement.atlas);
                !atlas_result) {
                result = atlas_result;
                return;
            }
            cached_fonts_[text.font_asset_id] = std::move(replacement);
            cached = &cached_fonts_[text.font_asset_id];
        }

        const uint64_t texture_id = font_texture_asset_id(text.font_asset_id);
        asset_pipeline::CpuAsset texture{};
        texture.asset_id = texture_id;
        texture.source_hash = font_hash;
        texture.kind = EditorAssetKind::kTexture;
        texture.success = true;
        texture.transient = true;
        texture.width = cached->atlas.width;
        texture.height = cached->atlas.height;
        texture.pixels = cached->atlas.rgba;
        if (const Result texture_result = upload_texture_asset(texture, context); !texture_result) {
            result = texture_result;
            return;
        }

        struct Vertex final {
            float x, y, z, w;
            float u, v;
            float nx, ny, nz;
        };
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        float cursor_x = 0.0f;
        float cursor_y = 0.0f;
        const float scale = text.world_size / cached->atlas.pixel_height;
        for (const unsigned char byte : text.text) {
            const int character =
                (byte >= static_cast<unsigned char>(asset_pipeline::kFontFirstGlyph) &&
                 byte < static_cast<unsigned char>(asset_pipeline::kFontFirstGlyph + asset_pipeline::kFontGlyphCount))
                    ? byte
                    : '?';
            stbtt_aligned_quad quad{};
            stbtt_GetBakedQuad(cached->atlas.glyphs.data(), static_cast<int>(cached->atlas.width),
                               static_cast<int>(cached->atlas.height), character - asset_pipeline::kFontFirstGlyph,
                               &cursor_x, &cursor_y, &quad, 1);
            const uint16_t base = static_cast<uint16_t>(vertices.size());
            vertices.push_back({quad.x0 * scale, -quad.y1 * scale, 0.0f, 1.0f, quad.s0, quad.t1, 0.0f, 0.0f, 1.0f});
            vertices.push_back({quad.x1 * scale, -quad.y1 * scale, 0.0f, 1.0f, quad.s1, quad.t1, 0.0f, 0.0f, 1.0f});
            vertices.push_back({quad.x1 * scale, -quad.y0 * scale, 0.0f, 1.0f, quad.s1, quad.t0, 0.0f, 0.0f, 1.0f});
            vertices.push_back({quad.x0 * scale, -quad.y0 * scale, 0.0f, 1.0f, quad.s0, quad.t0, 0.0f, 0.0f, 1.0f});
            indices.insert(indices.end(), {base, static_cast<uint16_t>(base + 1u), static_cast<uint16_t>(base + 2u),
                                           base, static_cast<uint16_t>(base + 2u), static_cast<uint16_t>(base + 3u)});
        }
        const uint64_t mesh_id =
            aeg::engine::components::text_mesh_asset_id(text.font_asset_id, text.text, text.world_size);
        asset_pipeline::CpuAsset mesh{};
        mesh.asset_id = mesh_id;
        mesh.source_hash = combine_asset_hash(
            font_hash, aeg::engine::components::text_asset_hash(text.font_asset_id, text.text, text.world_size));
        mesh.kind = EditorAssetKind::kMesh;
        mesh.success = true;
        mesh.transient = true;
        mesh.vertex_stride = sizeof(Vertex);
        mesh.vertex_count = static_cast<uint32_t>(vertices.size());
        mesh.index_count = static_cast<uint32_t>(indices.size());
        mesh.indices_are_uint32 = false;
        mesh.vertex_data.resize(vertices.size() * sizeof(Vertex));
        mesh.index_data.resize(indices.size() * sizeof(uint16_t));
        std::memcpy(mesh.vertex_data.data(), vertices.data(), mesh.vertex_data.size());
        std::memcpy(mesh.index_data.data(), indices.data(), mesh.index_data.size());
        mesh.bounds_center = {static_cast<float>(text.text.size()) * text.world_size * 0.25f, 0.0f, 0.0f};
        mesh.bounds_radius = std::max(text.world_size, static_cast<float>(text.text.size()) * text.world_size * 0.5f);
        if (const Result mesh_result = upload_mesh_asset(mesh, context); !mesh_result) {
            result = mesh_result;
            return;
        }

        uint64_t material_hash = mesh.source_hash;
        const std::array<float, 4> tint{text.color.x, text.color.y, text.color.z, text.color_alpha};
        for (const float component : tint) {
            uint32_t bits = 0;
            std::memcpy(&bits, &component, sizeof(bits));
            material_hash = combine_asset_hash(material_hash, bits);
        }
        asset_pipeline::CpuAsset material{};
        material.asset_id =
            aeg::engine::components::text_material_asset_id(text.font_asset_id, text.text, text.world_size);
        material.source_hash = material_hash;
        material.kind = EditorAssetKind::kMaterial;
        material.success = true;
        material.transient = true;
        material.texture_asset_id = texture_id;
        material.tint = tint;
        const auto old = imported_materials_.find(material.asset_id);
        if (old == imported_materials_.end() || old->second.source_hash != material.source_hash) {
            if (const Result material_result = rebuild_material_asset(material, context); !material_result) {
                result = material_result;
                return;
            }
        }
    });
    return result;
}

foundation::Result EditorSceneRuntime::Impl::prepare_frame(const engine::EngineFramePreparationContext& context) {
    current_slot_ = context.frame_context.frame_slot_index();
    // RenderGraph handles are frame-local. Re-import every retained GPU asset
    // into the current graph, even when its GPU payload did not change.
    frame_texture_graphs_.clear();
    frame_vertex_graphs_.clear();
    frame_index_graphs_.clear();
    if (active_scope_ == nullptr) {
        return foundation::invalid_state("Editor scene has no active World scope");
    }
    if (const Result result = advance_runtime_demo(); !result) {
        return result;
    }
    const auto snapshot = active_scope_->latest_snapshot();
    if (!snapshot) {
        return snapshot.result;
    }
    const auto lighting = make_editor_lighting_constants(*snapshot.value);
    if (!lighting) {
        return lighting.result;
    }
    if (const Result result =
            device_->write_buffer(lighting_buffers_[current_slot_], 0, std::as_bytes(std::span{&lighting.value, 1}));
        !result) {
        return result;
    }
    ResourceState color_initial_state = undefined_state();
    if (target_has_previous_frame_) {
        const auto queried_state = query_texture_initial_state(*device_, color_texture_);
        if (!queried_state) {
            return queried_state.result;
        }
        color_initial_state = queried_state.value;
    }
    const auto color =
        context.render_graph.import_texture(color_texture_, color_view_, color_desc_, color_initial_state);
    const auto depth = context.render_graph.import_texture(depth_texture_, depth_view_, depth_desc_, undefined_state());
    if (!color || !depth) {
        return color ? depth.result : color.result;
    }
    if (const Result result = context.bind_feature_texture(kSceneColor, color.value); !result) {
        return result;
    }
    if (const Result result = context.bind_feature_texture(kSceneDepth, depth.value); !result) {
        return result;
    }

    if (viewport_readback_.requested) {
        const uint64_t pixel_count = static_cast<uint64_t>(viewport_width_) * viewport_height_;
        const auto allocation = context.frame_context.allocate_readback(pixel_count * 4u, 256);
        if (!allocation) {
            return allocation.result;
        }
        viewport_readback_.allocation = allocation.value;
        viewport_readback_.width = viewport_width_;
        viewport_readback_.height = viewport_height_;
        viewport_readback_.desc = {};
        viewport_readback_.desc.size = allocation.value.buffer_size;
        viewport_readback_.desc.capabilities =
            rhi::BufferCapability::kTransferSource | rhi::BufferCapability::kTransferDestination;
        viewport_readback_.desc.memory = MemoryClass::kReadback;
        viewport_readback_.desc.initial_usage = ResourceUsage::kHostRead;
        viewport_readback_.desc.initial_stages = PipelineStage::kHost;
        viewport_readback_.desc.initial_queue = QueueClass::kGraphics;
        viewport_readback_.desc.name = DebugName{"Editor.ViewportReadback"};
        viewport_readback_.initial_state = ResourceState{ResourceUsage::kHostRead, ShaderStage::kAll,
                                                         QueueClass::kGraphics, false, PipelineStage::kHost};
    }

    frame_texture_graphs_.clear();
    frame_vertex_graphs_.clear();
    frame_index_graphs_.clear();

    snapshot.value->for_each<MeshRenderer>([this](ecs::EntityId, const MeshRenderer& mesh) {
        const std::array<float, 4> tint{mesh.tint.x, mesh.tint.y, mesh.tint.z, mesh.tint_alpha};
        if (mesh.material_asset_id == kMaterialAssetRed || mesh.material_asset_id == kMaterialAssetGreen) {
            materials_[mesh.material_asset_id == kMaterialAssetGreen ? 1u : 0u].tint = tint;
        } else if (const auto found = imported_materials_.find(mesh.material_asset_id);
                   found != imported_materials_.end()) {
            found->second.tint = tint;
        }
    });
    if (!materials_ready_) {
        const std::array<std::byte, 16> red{{std::byte{220}, std::byte{40}, std::byte{40}, std::byte{255},
                                             std::byte{180}, std::byte{20}, std::byte{20}, std::byte{255},
                                             std::byte{220}, std::byte{40}, std::byte{40}, std::byte{255},
                                             std::byte{180}, std::byte{20}, std::byte{20}, std::byte{255}}};
        const std::array<std::byte, 16> green{{std::byte{30}, std::byte{210}, std::byte{60}, std::byte{255},
                                               std::byte{20}, std::byte{160}, std::byte{40}, std::byte{255},
                                               std::byte{30}, std::byte{210}, std::byte{60}, std::byte{255},
                                               std::byte{20}, std::byte{160}, std::byte{40}, std::byte{255}}};
        const std::array<std::span<const std::byte>, 2> data{std::span{red}, std::span{green}};
        for (uint32_t index = 0; index < kMaterialCount; ++index) {
            // The subresource descriptor must outlive the upload call.
            resource::TextureSubresourceData subresource{data[index], 8, 16};
            const resource::TextureData upload_data{
                make_material_texture_desc(index == 0 ? "Editor.Material.Red" : "Editor.Material.Green"),
                std::span{&subresource, 1}};
            const auto upload =
                texture_uploader_->upload_texture(context.frame_context, context.render_graph, upload_data,
                                                  index == 0 ? "Editor.Material.Red" : "Editor.Material.Green");
            if (!upload) {
                return upload.result;
            }
            MaterialGpu& material = materials_[index];
            material.asset_id = index == 0 ? kMaterialAssetRed : kMaterialAssetGreen;
            material.owns_texture = true;
            material.texture_gpu = upload.value.texture;
            material.texture = upload.value.texture.texture;
            material.sampled_view = upload.value.texture.sampled_view;
            material.texture_desc = upload_data.desc;
            material.graph_texture = to_pipeline_handle(upload.value.target);
            if (const Result result =
                    create_material_bindings(material, index == 0 ? "Editor.Material.Red" : "Editor.Material.Green");
                !result) {
                return result;
            }
        }
        materials_ready_ = true;
    } else {
        for (uint32_t index = 0; index < kMaterialCount; ++index) {
            const auto queried_state = query_texture_initial_state(*device_, materials_[index].texture);
            if (!queried_state) {
                return queried_state.result;
            }
            auto imported =
                context.render_graph.import_texture(materials_[index].texture, materials_[index].sampled_view,
                                                    materials_[index].texture_desc, queried_state.value);
            if (!imported) {
                return imported.result;
            }
            materials_[index].graph_texture = to_pipeline_handle(imported.value);
        }
    }

    if (asset_pipeline_ready_) {
        std::vector<asset_pipeline::CpuAsset> completed;
        if (const Result result = asset_pipeline_.poll(completed); !result) {
            return result;
        }
        bool asset_database_changed = false;
        for (const asset_pipeline::CpuAsset& asset : completed) {
            const EditorAssetRecord* record = asset_database_.find(asset.asset_id);
            if (record != nullptr && record->content_hash != asset.source_hash) {
                asset_database_changed = true;
                break;
            }
        }
        // The watcher owns its own snapshot so it can stay off the render
        // thread. Refresh the editor database only when a completed bake
        // proves that the source hash changed, then consume the completion
        // against the new stable record.
        if (asset_database_changed) {
            if (const Result result = asset_database_.refresh(); !result) {
                return result;
            }
            if (const Result result = synchronize_asset_pipeline(); !result) {
                return result;
            }
        }
        if (const Result result = retire_missing_assets(context); !result) {
            return result;
        }
        for (const asset_pipeline::CpuAsset& asset : completed) {
            if (asset.from_baked_format &&
                ((asset.kind == EditorAssetKind::kMesh && imported_meshes_.contains(asset.asset_id) &&
                  imported_meshes_[asset.asset_id].source_hash == asset.source_hash &&
                  imported_meshes_[asset.asset_id].from_baked_format) ||
                 (asset.kind == EditorAssetKind::kTexture && imported_textures_.contains(asset.asset_id) &&
                  imported_textures_[asset.asset_id].source_hash == asset.source_hash &&
                  imported_textures_[asset.asset_id].from_baked_format) ||
                 (asset.kind == EditorAssetKind::kMaterial && imported_materials_.contains(asset.asset_id) &&
                  imported_materials_[asset.asset_id].source_hash == asset.source_hash &&
                  imported_materials_[asset.asset_id].from_baked_format))) {
                continue;
            }
            if (const Result result = consume_asset(asset, context); !result) {
                diagnostic_ = "Background asset replacement failed: " + result.message;
            }
        }
        if (const Result result = ensure_fast_path_assets(context); !result) {
            return result;
        }
    }

    if (const Result result = prepare_text_assets(*snapshot.value, context); !result) {
        return result;
    }

    if (const Result result = refresh_material_texture_bindings(context); !result) {
        return result;
    }
    if (const Result result = import_frame_asset_graphs(context); !result) {
        return result;
    }
    for (auto& [id, material] : imported_materials_) {
        const auto texture = imported_textures_.find(material.texture_asset_id);
        material.graph_texture =
            texture != imported_textures_.end() ? texture->second.graph_texture : materials_[0].graph_texture;
        (void)id;
    }
    for (uint32_t index = 0; index < kMaterialCount; ++index) {
        MaterialGpu& material = materials_[index];
        resource::TextureMaterialParameters parameters{};
        parameters.tint = material.tint;
        if (const Result result = device_->write_buffer(material.parameter_buffers[current_slot_], 0,
                                                        std::as_bytes(std::span{&parameters, 1}));
            !result) {
            return result;
        }
        const RenderResourceId parameter_id = index == 0 ? kMaterialParameterRed : kMaterialParameterGreen;
        const auto parameter_graph = context.render_graph.import_buffer(material.parameter_buffers[current_slot_],
                                                                        material.parameter_desc, host_write_state());
        if (!parameter_graph) {
            return parameter_graph.result;
        }
        material.graph_parameter = to_pipeline_handle(parameter_graph.value);
        (void)parameter_id;
    }
    for (auto& [id, material] : imported_materials_) {
        resource::TextureMaterialParameters parameters{};
        parameters.tint = material.tint;
        if (const Result result = device_->write_buffer(material.parameter_buffers[current_slot_], 0,
                                                        std::as_bytes(std::span{&parameters, 1}));
            !result) {
            return result;
        }
        const auto parameter_graph = context.render_graph.import_buffer(material.parameter_buffers[current_slot_],
                                                                        material.parameter_desc, host_write_state());
        if (!parameter_graph) {
            return parameter_graph.result;
        }
        material.graph_parameter = to_pipeline_handle(parameter_graph.value);
        (void)id;
    }
    const auto lighting_graph =
        context.render_graph.import_buffer(lighting_buffers_[current_slot_], lighting_desc_, host_write_state());
    if (!lighting_graph) {
        return lighting_graph.result;
    }
    lighting_graph_ = to_pipeline_handle(lighting_graph.value);
    target_has_previous_frame_ = true;
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::destroy_resources() {
    if (device_ == nullptr) {
        return Result::success();
    }
    asset_pipeline_.shutdown();
    asset_pipeline_ready_ = false;
    (void)device_->wait_idle();
    renderer_.clear_extractors();
    for (auto& [id, material] : imported_materials_) {
        for (BindingSetHandle set : material.binding_sets) {
            if (set.is_valid()) {
                (void)device_->destroy_binding_set(set);
            }
        }
        for (BufferHandle buffer : material.parameter_buffers) {
            if (buffer.is_valid()) {
                (void)device_->destroy_buffer(buffer);
            }
        }
        if (material.sampler.is_valid()) {
            (void)device_->destroy_sampler(material.sampler);
        }
        (void)id;
    }
    imported_materials_.clear();
    for (auto& [id, texture] : imported_textures_) {
        (void)texture_uploader_->destroy_texture(texture.gpu);
        (void)id;
    }
    imported_textures_.clear();
    for (auto& [id, mesh] : imported_meshes_) {
        (void)mesh_uploader_->destroy_mesh(mesh.gpu);
        (void)id;
    }
    imported_meshes_.clear();
    for (auto& material : materials_) {
        for (auto set : material.binding_sets) {
            if (set.is_valid()) {
                (void)device_->destroy_binding_set(set);
            }
        }
        for (auto buffer : material.parameter_buffers) {
            if (buffer.is_valid()) {
                (void)device_->destroy_buffer(buffer);
            }
        }
        if (material.sampler.is_valid()) {
            (void)device_->destroy_sampler(material.sampler);
        }
        if (material.sampled_view.is_valid()) {
            (void)device_->destroy_texture_view(material.sampled_view);
        }
        if (material.texture.is_valid()) {
            (void)device_->destroy_texture(material.texture);
        }
        material = {};
    }
    for (auto& frame : object_resources_) {
        for (auto& object : frame) {
            if (object.binding_set.is_valid()) {
                (void)device_->destroy_binding_set(object.binding_set);
            }
        }
    }
    for (auto& frame : object_buffers_) {
        for (auto& buffer : frame) {
            if (buffer.is_valid()) {
                (void)device_->destroy_buffer(buffer);
            }
        }
    }
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        if (lighting_sets_[frame].is_valid()) {
            (void)device_->destroy_binding_set(lighting_sets_[frame]);
        }
        if (lighting_buffers_[frame].is_valid()) {
            (void)device_->destroy_buffer(lighting_buffers_[frame]);
        }
    }
    for (auto buffer : mesh_vertex_buffers_) {
        if (buffer.is_valid()) {
            (void)device_->destroy_buffer(buffer);
        }
    }
    if (index_buffer_.is_valid()) {
        (void)device_->destroy_buffer(index_buffer_);
    }
    if (pipeline_.is_valid()) {
        (void)device_->destroy_pipeline(pipeline_);
    }
    for (rhi::PipelineHandle pipeline : state_composite_pipelines_) {
        if (pipeline.is_valid()) {
            (void)device_->destroy_pipeline(pipeline);
        }
    }
    static_cast<void>(release_state_composite_sets());
    if (color_view_.is_valid()) {
        (void)device_->destroy_texture_view(color_view_);
        (void)device_->destroy_texture_view(color_sampled_view_);
        (void)device_->destroy_texture(color_texture_);
    }
    if (depth_view_.is_valid()) {
        (void)device_->destroy_texture_view(depth_view_);
        (void)device_->destroy_texture(depth_texture_);
    }
    if (viewport_sampler_.is_valid()) {
        (void)device_->destroy_sampler(viewport_sampler_);
    }
    shader_manager_owner_.reset();
    shader_manager_ = nullptr;
    return Result::success();
}

foundation::Result EditorSceneRuntime::shutdown() {
    if (impl_ == nullptr) {
        return Result::success();
    }
    impl_->orchestrator_.reset();
    if (impl_->edit_scope_ != nullptr) {
        static_cast<void>(impl_->edit_scope_->shutdown());
    }
    if (impl_->runtime_scope_ != nullptr) {
        static_cast<void>(impl_->runtime_scope_->shutdown());
    }
    impl_->edit_scope_.reset();
    impl_->runtime_scope_.reset();
    const Result result = impl_->destroy_resources();
    impl_->initialized_ = false;
    impl_.reset();
    return result;
}

foundation::Result EditorSceneRuntime::request_viewport_extent(uint32_t width, uint32_t height) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (width == 0 || height == 0) {
        return foundation::invalid_argument("Editor viewport extent must be positive");
    }
    if (impl_->requested_width_ != width || impl_->requested_height_ != height) {
        impl_->requested_width_ = width;
        impl_->requested_height_ = height;
        impl_->target_resize_pending_ = true;
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::request_viewport_readback() {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    impl_->viewport_readback_.requested = true;
    impl_->viewport_readback_.pass_built = false;
    impl_->viewport_readback_.pass_executed = false;
    return Result::success();
}

foundation::ResultValue<std::vector<std::byte>> EditorSceneRuntime::read_viewport_pixels() const {
    if (impl_ == nullptr || !impl_->initialized_) {
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    }
    if (impl_->viewport_readback_.pixels.empty()) {
        return {foundation::not_found("No viewport readback is available"), {}};
    }
    return {Result::success(), impl_->viewport_readback_.pixels};
}

editor::ImGuiTextureBinding EditorSceneRuntime::viewport_binding() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return {impl_->color_sampled_view_, impl_->viewport_sampler_, impl_->color_texture_, impl_->color_desc_,
            impl_->viewport_export_state_};
}

bool EditorSceneRuntime::viewport_ready() const noexcept {
    return impl_ != nullptr && impl_->initialized_ && impl_->color_texture_.is_valid() &&
           impl_->color_sampled_view_.is_valid() && impl_->viewport_sampler_.is_valid();
}

bool EditorSceneRuntime::viewport_present_connected() const noexcept {
    if (impl_ == nullptr || !impl_->initialized_) {
        return true;
    }
    const auto plan = impl_->frame_host_.active_plan_snapshot();
    if (!plan) {
        return true;
    }
    const render_graph::RenderStateFlow state_flow =
        render_graph::build_render_state_flow(*plan, impl_->render_state_link_overrides_);
    if (state_flow.has_cycle) {
        return false;
    }
    for (std::size_t index = 0; index < plan->feature_contributions.size(); ++index) {
        const auto& contribution = plan->feature_contributions[index];
        if (contribution.domain != pipeline::FeatureDomain::kRender || !state_flow.contributions[index].reaches_present) {
            continue;
        }
        const auto feature = impl_->registry_.find_feature(contribution.feature);
        if (feature && feature.value->uses_render_state_output()) {
            return true;
        }
    }
    return false;
}

bool EditorSceneRuntime::viewport_resize_pending() const noexcept {
    return impl_ != nullptr && impl_->initialized_ && impl_->target_resize_pending_;
}

foundation::ResultValue<ViewportPickResult> EditorSceneRuntime::pick_viewport(ViewportPoint pixel, uint32_t width,
                                                                              uint32_t height) const {
    if (impl_ == nullptr || !impl_->initialized_) {
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    }
    const auto ray = impl_->make_viewport_ray(pixel, width, height);
    if (!ray) {
        return {ray.result, {}};
    }
    const auto snapshot = impl_->edit_scope_->latest_snapshot();
    if (!snapshot) {
        return {snapshot.result, {}};
    }

    ViewportPickResult result{};
    float nearest = std::numeric_limits<float>::infinity();
    snapshot.value->for_each<MeshRenderer>([&](ecs::EntityId runtime_entity, const MeshRenderer& mesh) {
        if (!mesh.visible || mesh.mesh_asset_id == 0 || mesh.material_asset_id == 0) {
            return;
        }
        const Transform* transform = snapshot.value->get<Transform>(runtime_entity);
        if (transform == nullptr || !finite_vec3(transform->translation) || !finite_vec3(transform->scale)) {
            return;
        }
        const float scale =
            std::max({std::abs(transform->scale.x), std::abs(transform->scale.y), std::abs(transform->scale.z)});
        // The editor fallback mesh is a unit quad. A zero serialized bound therefore remains pickable.
        const float local_radius = mesh.bounds_radius > math::kEpsilon ? mesh.bounds_radius : 1.0f;
        const float radius = local_radius * std::max(scale, math::kEpsilon);
        const math::Vec3 center = transform->transform_point(mesh.bounds_center);
        float distance = 0.0f;
        const bool built_in_quad = mesh.mesh_asset_id == kMeshAssetA || mesh.mesh_asset_id == kMeshAssetB;
        const bool hit =
            intersect_editor_quad(ray.value.origin, ray.value.direction, *transform, mesh.mesh_asset_id, distance);
        if (!hit && built_in_quad) {
            return;
        }
        if (!hit) {
            // Unknown editor assets do not expose CPU geometry yet. Keep the conservative
            // sphere fallback for those assets, but never use it for the built-in Quads.
            const math::Vec3 offset = ray.value.origin - center;
            const float b = math::dot(offset, ray.value.direction);
            const float c = math::dot(offset, offset) - radius * radius;
            const float discriminant = b * b - c;
            if (!std::isfinite(discriminant) || discriminant < 0.0f) {
                return;
            }
            distance = -b - std::sqrt(discriminant);
            if (distance < 0.0f) {
                distance = -b + std::sqrt(discriminant);
            }
        }
        if (!std::isfinite(distance) || distance < 0.0f || distance >= nearest) {
            return;
        }
        const auto scene_entity =
            std::find_if(impl_->edit_instance_.entities().begin(), impl_->edit_instance_.entities().end(),
                         [runtime_entity](const auto& binding) { return binding.runtime_entity == runtime_entity; });
        if (scene_entity == impl_->edit_instance_.entities().end()) {
            return;
        }
        nearest = distance;
        result.entity = scene_entity->scene_entity;
        result.distance = distance;
        result.hit = true;
    });
    return {Result::success(), result};
}

foundation::ResultValue<ViewportGizmoProjection> EditorSceneRuntime::selected_gizmo_projection(uint32_t width,
                                                                                               uint32_t height) const {
    if (impl_ == nullptr || !impl_->initialized_) {
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    }
    if (impl_->mode_ != Mode::kEdit) {
        return {Result::success(), {}};
    }
    const auto view = impl_->make_edit_view(width, height);
    if (!view) {
        return {view.result, {}};
    }
    const auto snapshot = impl_->edit_scope_->latest_snapshot();
    if (!snapshot) {
        return {snapshot.result, {}};
    }
    const auto runtime = impl_->edit_instance_.runtime_entity(impl_->selected_entity_);
    if (!runtime) {
        return {runtime.result, {}};
    }
    const auto* mesh = snapshot.value->get<MeshRenderer>(runtime.value);
    if (mesh != nullptr && !mesh->visible) {
        return {Result::success(), {}};
    }
    const Transform* transform = snapshot.value->get<Transform>(runtime.value);
    if (transform == nullptr) {
        return {foundation::not_found("Selected entity has no Transform component"), {}};
    }
    const float distance = math::length(transform->translation - view.value.camera.position);
    const float length = std::max(0.4f, std::isfinite(distance) ? distance * 0.18f : 0.75f);
    const math::Mat4 view_projection = view.value.camera.view_projection();
    const auto project = [width, height, &view_projection](math::Vec3 point, ViewportPoint& result) {
        const math::Vec4 clip = view_projection * math::Vec4{point.x, point.y, point.z, 1.0f};
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) || !std::isfinite(clip.w) ||
            clip.w <= math::kEpsilon) {
            return false;
        }
        const float ndc_x = clip.x / clip.w;
        const float ndc_y = clip.y / clip.w;
        result = {((ndc_x + 1.0f) * 0.5f) * static_cast<float>(width),
                  ((1.0f - ndc_y) * 0.5f) * static_cast<float>(height)};
        return std::isfinite(result.x) && std::isfinite(result.y);
    };

    ViewportGizmoProjection result{};
    const math::Vec3 origin = transform->translation;
    result.valid = project(origin, result.center) && project(origin + math::Vec3{length, 0.0f, 0.0f}, result.x_axis) &&
                   project(origin + math::Vec3{0.0f, length, 0.0f}, result.y_axis) &&
                   project(origin + math::Vec3{0.0f, 0.0f, length}, result.z_axis);
    return {Result::success(), result};
}

foundation::Result EditorSceneRuntime::begin_transform_edit(ViewportGizmoMode mode, ViewportGizmoAxis axis,
                                                            ViewportPoint pixel, uint32_t width, uint32_t height) {
    if (impl_ == nullptr || !impl_->initialized_ || impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Transform editing requires an initialized Edit World");
    }
    if (impl_->transform_edit_.active) {
        return foundation::invalid_state("A Transform edit transaction is already active");
    }
    if (!valid_gizmo_axis(axis)) {
        return foundation::invalid_argument("Transform Gizmo axis is invalid");
    }
    const auto runtime = impl_->edit_instance_.runtime_entity(impl_->selected_entity_);
    if (!runtime) {
        return runtime.result;
    }
    const auto snapshot = impl_->edit_scope_->latest_snapshot();
    if (!snapshot) {
        return snapshot.result;
    }
    const Transform* transform = snapshot.value->get<Transform>(runtime.value);
    if (transform == nullptr) {
        return foundation::not_found("Selected entity has no Transform component");
    }
    const auto ray = impl_->make_viewport_ray(pixel, width, height);
    if (!ray) {
        return ray.result;
    }
    const Transform* camera_transform = snapshot.value->get<Transform>(impl_->edit_camera_runtime_);
    if (camera_transform == nullptr) {
        return foundation::not_found("Editor viewport camera Transform is missing");
    }
    const math::Vec3 axis_vector = gizmo_axis(axis);
    const math::Vec3 camera_forward = math::rotate(camera_transform->rotation, {0.0f, 0.0f, -1.0f});
    const math::Vec3 camera_up = math::rotate(camera_transform->rotation, {0.0f, 1.0f, 0.0f});
    math::Vec3 camera_right = math::cross(camera_forward, camera_up);
    if (!math::try_normalize(camera_right, camera_right)) {
        return foundation::invalid_state("Viewport camera basis is invalid for Transform editing");
    }
    const std::array<math::Vec3, 3> candidates{{math::cross(axis_vector, camera_forward),
                                                math::cross(axis_vector, camera_up),
                                                math::cross(axis_vector, camera_right)}};
    math::Vec3 plane_normal{};
    float best_alignment = 0.0f;
    for (math::Vec3 candidate : candidates) {
        if (!math::try_normalize(candidate, candidate)) {
            continue;
        }
        const float alignment = std::abs(math::dot(candidate, ray.value.direction));
        if (alignment > best_alignment) {
            best_alignment = alignment;
            plane_normal = candidate;
        }
    }
    if (best_alignment <= math::kEpsilon) {
        // A view-aligned axis has no unique screen plane at the object center. A
        // plane perpendicular to that axis still gives a stable depth delta.
        plane_normal = camera_forward;
        if (!math::try_normalize(plane_normal, plane_normal) ||
            std::abs(math::dot(plane_normal, ray.value.direction)) <= math::kEpsilon) {
            return foundation::invalid_state("Transform Gizmo axis is parallel to the viewport camera");
        }
    }
    math::Vec3 initial_hit{};
    if (!intersect_ray_plane(ray.value.origin, ray.value.direction, transform->translation, plane_normal,
                             initial_hit)) {
        return foundation::invalid_state("Viewport ray cannot start a Transform edit");
    }
    const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
    try {
        Impl::EditTransaction transaction{true,        mode,       axis,        impl_->selected_entity_,
                                          pixel,       *transform, initial_hit, plane_normal,
                                          axis_vector, *transform, {}};
        transaction.command.before.push_back(
            Impl::EditorComponentValue{impl_->selected_entity_, transform_type, bytes_of(*transform)});
        impl_->history_.reserve(impl_->history_.size() + 1u);
        impl_->transform_edit_ = std::move(transaction);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Transform edit transaction allocation failed");
    }
    impl_->refresh_dirty();
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::reset_scene_for_project() {
    if (const Result result = make_empty_project_scene(); !result) {
        return result;
    }
    transform_edit_ = {};
    selected_entity_ = {};
    selected_entities_.clear();
    mode_ = EditorSceneRuntime::Mode::kEdit;
    runtime_scope_.reset();
    runtime_state_.reset();
    runtime_camera_runtime_ = {};
    active_scope_ = nullptr;
    scene_path_ = project_manager_.loaded() ? project_manager_.default_scene_file()
                                            : std::filesystem::path{"editor_scene.aegscene"};
    const Result result = rebuild_edit_world();
    if (result) {
        reset_history(true);
    }
    return result;
}

foundation::Result EditorSceneRuntime::update_transform_edit(ViewportPoint pixel, uint32_t width, uint32_t height) {
    if (impl_ == nullptr || !impl_->initialized_ || !impl_->transform_edit_.active) {
        return foundation::invalid_state("No Transform edit transaction is active");
    }
    const auto ray = impl_->make_viewport_ray(pixel, width, height);
    if (!ray) {
        return ray.result;
    }
    Transform next = impl_->transform_edit_.initial;
    if (impl_->transform_edit_.mode == ViewportGizmoMode::kTranslate) {
        math::Vec3 current_hit{};
        if (!intersect_ray_plane(ray.value.origin, ray.value.direction, impl_->transform_edit_.initial.translation,
                                 impl_->transform_edit_.plane_normal, current_hit)) {
            return foundation::invalid_state("Viewport ray cannot update the Transform translation");
        }
        const float delta =
            math::dot(current_hit - impl_->transform_edit_.initial_plane_hit, impl_->transform_edit_.axis_vector);
        next.translation += impl_->transform_edit_.axis_vector * delta;
    } else if (impl_->transform_edit_.mode == ViewportGizmoMode::kRotate) {
        const float delta_pixels =
            (pixel.x - impl_->transform_edit_.start_pixel.x) + (impl_->transform_edit_.start_pixel.y - pixel.y);
        const math::Quat delta_rotation =
            math::quat_from_axis_angle(impl_->transform_edit_.axis_vector, delta_pixels * 0.01f);
        next.rotation = math::normalize(delta_rotation * impl_->transform_edit_.initial.rotation);
    } else {
        const float delta_pixels =
            (pixel.x - impl_->transform_edit_.start_pixel.x) - (pixel.y - impl_->transform_edit_.start_pixel.y);
        const float factor = std::max(0.01f, 1.0f + delta_pixels * 0.01f);
        const std::size_t axis_index = static_cast<std::size_t>(impl_->transform_edit_.axis);
        next.scale[axis_index] = impl_->transform_edit_.initial.scale[axis_index] * factor;
    }
    const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
    std::vector<Impl::EditorComponentValue> values;
    values.push_back(Impl::EditorComponentValue{impl_->transform_edit_.entity, transform_type, bytes_of(next)});
    const Result queued = impl_->enqueue_values(values);
    if (!queued) {
        return queued;
    }
    impl_->transform_edit_.current = next;
    impl_->transform_edit_.command.after = values;
    return Result::success();
}

foundation::Result EditorSceneRuntime::commit_transform_edit() {
    if (impl_ == nullptr || !impl_->transform_edit_.active) {
        return foundation::invalid_state("No Transform edit transaction is active");
    }
    if (impl_->transform_edit_.command.after.empty()) {
        const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
        impl_->transform_edit_.command.after.push_back(Impl::EditorComponentValue{
            impl_->transform_edit_.entity, transform_type, bytes_of(impl_->transform_edit_.current)});
    }
    const Result result = impl_->commit_history(std::move(impl_->transform_edit_.command));
    if (!result) {
        return result;
    }
    impl_->transform_edit_.active = false;
    impl_->refresh_dirty();
    return Result::success();
}

foundation::Result EditorSceneRuntime::cancel_transform_edit() {
    if (impl_ == nullptr || !impl_->transform_edit_.active) {
        return foundation::invalid_state("No Transform edit transaction is active");
    }
    const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
    std::vector<Impl::EditorComponentValue> values;
    values.push_back(Impl::EditorComponentValue{impl_->transform_edit_.entity, transform_type,
                                                bytes_of(impl_->transform_edit_.initial)});
    const Result queued = impl_->enqueue_values(values);
    if (!queued) {
        return queued;
    }
    impl_->transform_edit_.active = false;
    impl_->refresh_dirty();
    return Result::success();
}

bool EditorSceneRuntime::transform_edit_active() const noexcept {
    return impl_ != nullptr && impl_->transform_edit_.active;
}

foundation::Result EditorSceneRuntime::render_frame(uint64_t frame_number) {
    if (impl_ == nullptr || !impl_->initialized_ || impl_->active_scope_ == nullptr ||
        impl_->orchestrator_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not ready to render");
    }
    // The RenderFeature state descriptor is copied when the frame begins. Resize
    // the viewport before creating it so the transient state texture and the
    // imported SceneColor target always have identical dimensions and format.
    if (const Result result = impl_->resize_targets(); !result) {
        return result;
    }
    impl_->current_slot_ = impl_->frame_context_->frame_slot_index();
    impl_->viewport_readback_.present_connected = viewport_present_connected();
    engine::EngineFrameDesc desc{};
    desc.fixed_tick = frame_number;
    desc.render_output_extent = {impl_->requested_width_, impl_->requested_height_};
    desc.render_feature_state = {
        .enabled = true,
        .texture_desc = impl_->color_desc_,
        .present_target_id = kSceneColor,
        .present_extent = {impl_->requested_width_, impl_->requested_height_, 1},
        .link_overrides = impl_->render_state_link_overrides_,
        .blend_overrides = impl_->render_state_blend_overrides_,
        .compose_terminal_states = [impl = impl_.get()](pipeline::RenderGraphBuilder& builder,
                                                        std::span<const render_graph::RenderStateCompositeInput> inputs,
                                                        pipeline::RenderGraphTexture destination) -> Result {
            if (inputs.empty() || !destination.is_valid()) {
                return foundation::invalid_argument("Render-state terminal composition requires valid state inputs");
            }
            const auto extent = rhi::Extent3D{impl->requested_width_, impl->requested_height_, 1};
            for (uint32_t index = 0; index < inputs.size(); ++index) {
                const pipeline::RenderGraphTexture source = inputs[index].texture;
                const uint32_t pipeline_index = static_cast<uint32_t>(inputs[index].blend_mode);
                if (pipeline_index >= impl->state_composite_pipelines_.size() ||
                    !impl->state_composite_pipelines_[pipeline_index].is_valid()) {
                    return foundation::invalid_state("Render-state blend pipeline is unavailable");
                }
                const auto binding_set_result = impl->create_state_composite_set();
                if (!binding_set_result) {
                    return binding_set_result.result;
                }
                const BindingSetHandle binding_set = binding_set_result.value;
                const rhi::LoadOp load_op = index == 0 ? rhi::LoadOp::kClear : rhi::LoadOp::kLoad;
                const auto pass_result = builder.add_graphics_pass(
                    "RenderState.Composite",
                    [source, destination, load_op](pipeline::RenderPassBuilder& pass) {
                        if (const Result result = pass.read(source, rhi::ResourceUsage::kSampledRead,
                                                            rhi::PipelineStage::kFragmentShader,
                                                            rhi::ShaderStage::kFragment);
                            !result) {
                            return result;
                        }
                        if (const Result result = pass.use_color_attachment(
                                destination, load_op, rhi::StoreOp::kStore,
                                ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f}, rhi::ResourceUsage::kSampledRead);
                            !result) {
                            return result;
                        }
                        pass.set_side_effects();
                        return Result::success();
                    },
                    [impl, source, binding_set, extent, pipeline_index](pipeline::RenderPassExecutionContext& pass) {
                        const auto sampled_view = pass.texture_view(source, TextureViewKind::kSampled,
                                                                    {rhi::Aspect::kColor, 0, 1, 0, 1},
                                                                    Format::kRgba8Unorm, TextureViewDimension::kTex2D);
                        if (!sampled_view) {
                            return sampled_view.result;
                        }
                        const std::array<BindingWrite, 2> writes{
                            BindingWrite{{0, 0}, 0,
                                         BindingResource{BindingType::kSampledTexture, sampled_view.value, {}, {}, {}, {}}},
                            BindingWrite{{0, 1}, 0,
                                         BindingResource{BindingType::kSampler, {}, {}, {}, impl->viewport_sampler_, {}}},
                        };
                        if (const Result result = impl->device_->update_binding_set(binding_set, writes); !result) {
                            return result;
                        }
                        if (const Result result = pass.bind_pipeline(impl->state_composite_pipelines_[pipeline_index]);
                            !result) {
                            return result;
                        }
                        if (const Result result = pass.bind_binding_set(0, binding_set); !result) {
                            return result;
                        }
                        if (const Result result = pass.begin_rendering(); !result) {
                            return result;
                        }
                        if (const Result result = pass.set_viewport(
                                rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                              static_cast<float>(extent.height), 0.0f, 1.0f});
                            !result) {
                            return result;
                        }
                        if (const Result result = pass.set_scissor(rhi::ScissorRect{0, 0, extent.width, extent.height});
                            !result) {
                            return result;
                        }
                        if (const Result result = pass.draw(rhi::DrawDesc{3, 1, 0, 0}); !result) {
                            return result;
                        }
                        return pass.end_rendering();
                    });
                if (!pass_result) {
                    return pass_result.result;
                }
            }
            return Result::success();
        },
    };
    desc.renderer = &impl_->renderer_;
    desc.render_view_resolver = [impl = impl_.get()](const engine::EngineFrameRenderViewContext& context) {
        const ecs::EntityId camera = impl->mode_ == Mode::kEdit ? impl->edit_camera_runtime_
                                                                : impl->runtime_camera_runtime_;
        auto view = aeg::engine::components::make_render_view(
            context.world_snapshot, aeg::engine::components::RuntimeRenderViewDesc{camera,
                                                                                   0,
                                                                                   context.output_extent,
                                                                                   0xffffffffu,
                                                                                   kSceneColor,
                                                                                   kSceneDepth,
                                                                                   false,
                                                                                   {}});
        if (view) {
            view.value.color_target_final_usage = ResourceUsage::kSampledRead;
        }
        return view;
    };
    desc.render_resource_resolver = &impl_->resolver_;
    // Asset uploads happen in RenderBuild preparation. Extraction therefore
    // accepts stable IDs first and the Resolver resolves fast-path/baked GPU
    // resources after preparation has had a chance to publish them.
    desc.require_resolved_render_resources = false;
    desc.user_data = &impl_->viewport_readback_;
    desc.render_feature_preparation = [impl = impl_.get()](const engine::EngineFramePreparationContext& context) {
        return impl->prepare_frame(context);
    };
    const Result frame_result = impl_->orchestrator_->run_frame(desc);
    const Result composite_release_result = impl_->release_state_composite_sets();
    if (!frame_result) {
        impl_->viewport_readback_.requested = false;
        impl_->viewport_readback_.allocation = {};
        return frame_result;
    }
    if (!composite_release_result) {
        return composite_release_result;
    }
    const auto viewport_state = query_texture_initial_state(*impl_->device_, impl_->color_texture_);
    if (!viewport_state) {
        return viewport_state.result;
    }
    impl_->viewport_export_state_ = viewport_state.value;
    if (impl_->viewport_readback_.requested) {
        if (!impl_->viewport_readback_.allocation.is_valid()) {
            impl_->viewport_readback_.requested = false;
            return foundation::invalid_state("Viewport readback was requested but not prepared");
        }
        if (!impl_->viewport_readback_.pass_built || !impl_->viewport_readback_.pass_executed) {
            impl_->diagnostic_ = "Viewport readback pass was not built or executed";
        }
        if (const Result wait_result = impl_->device_->wait_idle(); !wait_result) {
            impl_->viewport_readback_.requested = false;
            return wait_result;
        }
        std::vector<std::byte> pixels(static_cast<std::size_t>(impl_->viewport_readback_.allocation.size));
        if (const Result read_result =
                impl_->frame_context_->read_readback(impl_->viewport_readback_.allocation, pixels);
            !read_result) {
            impl_->viewport_readback_.requested = false;
            return read_result;
        }
        impl_->viewport_readback_.pixels = std::move(pixels);
        impl_->viewport_readback_.requested = false;
        impl_->viewport_readback_.allocation = {};
    }
    return frame_result;
}

std::span<const scene::SceneEntityRecord> EditorSceneRuntime::entities() const noexcept {
    return impl_ == nullptr ? std::span<const scene::SceneEntityRecord>{} : impl_->scene_asset_.entities();
}

foundation::Result EditorSceneRuntime::select(scene::SceneEntityId entity, SelectionOperation operation) {
    if (impl_ == nullptr || !entity.is_valid()) {
        return foundation::invalid_argument("Editor selection identity is invalid");
    }
    const auto found = std::find_if(impl_->scene_asset_.entities().begin(), impl_->scene_asset_.entities().end(),
                                    [entity](const auto& candidate) { return candidate.id == entity; });
    if (found == impl_->scene_asset_.entities().end()) {
        return foundation::not_found("Editor selection is not present in the Scene Asset");
    }
    const auto selected = std::find(impl_->selected_entities_.begin(), impl_->selected_entities_.end(), entity);
    if (operation == SelectionOperation::kReplace) {
        impl_->selected_entities_.clear();
        impl_->selected_entities_.push_back(entity);
    } else if (operation == SelectionOperation::kAdd) {
        if (selected == impl_->selected_entities_.end()) {
            impl_->selected_entities_.push_back(entity);
        }
    } else if (selected == impl_->selected_entities_.end()) {
        impl_->selected_entities_.push_back(entity);
    } else {
        impl_->selected_entities_.erase(selected);
    }
    impl_->selected_entity_ =
        impl_->selected_entities_.empty() ? scene::SceneEntityId{} : impl_->selected_entities_.front();
    return Result::success();
}

foundation::ResultValue<std::vector<scene::SceneEntityId>> EditorSceneRuntime::children(
    scene::SceneEntityId parent) const {
    if (impl_ == nullptr) {
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    }
    return impl_->scene_asset_.children(parent);
}

foundation::ResultValue<std::string> EditorSceneRuntime::hierarchy_path(scene::SceneEntityId entity) const {
    if (impl_ == nullptr || !entity.is_valid()) {
        return {foundation::invalid_argument("Editor hierarchy entity identity is invalid"), {}};
    }
    std::vector<std::string_view> names;
    scene::SceneEntityId current = entity;
    try {
        while (current.is_valid()) {
            const auto found =
                std::find_if(impl_->scene_asset_.entities().begin(), impl_->scene_asset_.entities().end(),
                             [current](const auto& value) { return value.id == current; });
            if (found == impl_->scene_asset_.entities().end()) {
                return {foundation::not_found("Editor hierarchy entity is not present in the Scene Asset"), {}};
            }
            names.push_back(found->name);
            current = found->parent;
            if (names.size() > impl_->scene_asset_.entities().size()) {
                return {foundation::invalid_state("Editor hierarchy contains a cycle"), {}};
            }
        }
        std::string path;
        for (auto iterator = names.rbegin(); iterator != names.rend(); ++iterator) {
            if (!path.empty()) {
                path.append("/");
            }
            path.append(*iterator);
        }
        return {Result::success(), std::move(path)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Editor hierarchy path allocation failed"), {}};
    }
}

foundation::Result EditorSceneRuntime::Impl::reparent_entities(std::span<const scene::SceneEntityId> entities,
                                                               scene::SceneEntityId parent, std::size_t sibling_index,
                                                               HierarchyTransformPolicy transform_policy) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Hierarchy editing is only available in Edit mode");
    }
    if (transform_edit_.active) {
        return foundation::invalid_state("Hierarchy editing cannot run during a Transform edit");
    }
    if (entities.empty()) {
        return foundation::invalid_argument("Hierarchy reparent operation has no entities");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset_;
        scene::SceneAsset after_scene = scene_asset_;
        if (const Result result = after_scene.reparent_entities(entities, parent, sibling_index); !result) {
            return result;
        }
        command.after_scene = after_scene;

        if (transform_policy == HierarchyTransformPolicy::kKeepWorld) {
            const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
            const auto world_matrix =
                [this, transform_type](scene::SceneEntityId entity) -> foundation::ResultValue<math::Mat4> {
                std::vector<scene::SceneEntityId> stack;
                std::function<foundation::ResultValue<math::Mat4>(scene::SceneEntityId)> resolve =
                    [&](scene::SceneEntityId current) -> foundation::ResultValue<math::Mat4> {
                    if (std::find(stack.begin(), stack.end(), current) != stack.end()) {
                        return {foundation::invalid_state("Editor hierarchy contains a transform cycle"), {}};
                    }
                    const auto record = std::find_if(scene_asset_.entities().begin(), scene_asset_.entities().end(),
                                                     [current](const auto& value) { return value.id == current; });
                    if (record == scene_asset_.entities().end()) {
                        return {foundation::not_found("Editor hierarchy transform entity is not present"), {}};
                    }
                    stack.push_back(current);
                    Transform local{};
                    if (const void* value =
                            component_value_in(edit_instance_, edit_state_.get(), current, transform_type);
                        value != nullptr) {
                        local = *static_cast<const Transform*>(value);
                    }
                    math::Mat4 result = local.to_matrix();
                    if (record->parent.is_valid()) {
                        const auto parent_matrix = resolve(record->parent);
                        if (!parent_matrix) {
                            stack.pop_back();
                            return {parent_matrix.result, {}};
                        }
                        result = parent_matrix.value * result;
                    }
                    stack.pop_back();
                    return {Result::success(), result};
                };
                return resolve(entity);
            };

            std::unordered_set<scene::SceneEntityId, scene::Guid128Hash<scene::SceneEntityIdTag>> source_set;
            source_set.reserve(entities.size());
            for (const scene::SceneEntityId entity : entities) {
                source_set.emplace(entity);
            }
            for (const scene::SceneEntityId entity : entities) {
                const void* current_value =
                    component_value_in(edit_instance_, edit_state_.get(), entity, transform_type);
                if (current_value == nullptr) {
                    continue;
                }
                const auto old_world = world_matrix(entity);
                if (!old_world) {
                    return old_world.result;
                }
                math::Mat4 parent_world = math::identity();
                if (parent.is_valid()) {
                    const auto resolved_parent = world_matrix(parent);
                    if (!resolved_parent) {
                        return resolved_parent.result;
                    }
                    parent_world = resolved_parent.value;
                }
                math::Mat4 inverse_parent = math::identity();
                if (!math::try_inverse(parent_world, inverse_parent)) {
                    return foundation::invalid_state("Hierarchy parent transform is degenerate");
                }
                Transform next{};
                if (!decompose_affine_transform(inverse_parent * old_world.value, next)) {
                    return foundation::invalid_state(
                        "Hierarchy reparent would produce a non-representable or degenerate Transform");
                }
                command.before.push_back(EditorComponentValue{entity, transform_type,
                                                              bytes_of(*static_cast<const Transform*>(current_value))});
                command.after.push_back(EditorComponentValue{entity, transform_type, bytes_of(next)});
            }
        }
        history_.reserve(history_.size() + 1u);
        if (!command.after.empty()) {
            if (const Result result = enqueue_values(command.after); !result) {
                return result;
            }
        }
        scene_asset_ = std::move(after_scene);
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Hierarchy reparent transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::reparent_entities(std::span<const scene::SceneEntityId> entities,
                                                         scene::SceneEntityId parent, std::size_t sibling_index,
                                                         HierarchyTransformPolicy transform_policy) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->reparent_entities(entities, parent, sibling_index, transform_policy);
}

foundation::Result EditorSceneRuntime::Impl::create_scene_entity(scene::SceneEntityId parent, std::string_view name) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Scene entity creation is only available in Edit mode");
    }
    if (parent.is_valid() &&
        std::find_if(scene_asset_.entities().begin(), scene_asset_.entities().end(),
                     [parent](const auto& entity) { return entity.id == parent; }) == scene_asset_.entities().end()) {
        return foundation::not_found("Scene entity creation parent is not present");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset_;
        scene::SceneAsset after_scene = scene_asset_;
        uint64_t next_id = 1;
        scene::SceneEntityId id{};
        for (;;) {
            if (next_id == 0) {
                return foundation::invalid_state("Scene entity ID space is exhausted");
            }
            id = scene::SceneEntityId{after_scene.id().high(), next_id};
            if (std::find_if(after_scene.entities().begin(), after_scene.entities().end(),
                             [id](const auto& entity) { return entity.id == id; }) == after_scene.entities().end()) {
                break;
            }
            next_id = next_id == std::numeric_limits<uint64_t>::max() ? 0 : next_id + 1u;
        }
        const std::string entity_name = name.empty() ? "Entity" : std::string(name);
        if (const Result result = after_scene.add_entity({id, parent, entity_name, scene::SceneEntityFlags::kNone});
            !result) {
            return result;
        }
        Transform transform{};
        if (const Result result = after_scene.add_reflected_component(
                id, ecs::ComponentTypeId(aeg::engine::components::kTransformName), reflection_registry_,
                reflection::TypeId(aeg::engine::components::kTransformName), &transform);
            !result) {
            return result;
        }
        command.after_scene = after_scene;
        command.rebuild_edit_world = true;
        history_.reserve(history_.size() + 1u);
        scene_asset_ = std::move(after_scene);
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset_ = *command.before_scene;
            static_cast<void>(rebuild_edit_world());
            return result;
        }
        selected_entities_.clear();
        selected_entities_.push_back(id);
        selected_entity_ = id;
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Scene entity creation transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::create_scene_entity(scene::SceneEntityId parent, std::string_view name) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->create_scene_entity(parent, name);
}

foundation::Result EditorSceneRuntime::Impl::rename_entity(scene::SceneEntityId entity, std::string_view name) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Entity name editing is only available in Edit mode");
    }
    if (component_edit_active_ || transform_edit_.active) {
        return foundation::invalid_state("Entity metadata editing cannot overlap another editor transaction");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset_;
        scene::SceneAsset after_scene = scene_asset_;
        if (const Result result = after_scene.set_entity_name(entity, std::string(name)); !result) {
            return result;
        }
        command.after_scene = std::move(after_scene);
        history_.reserve(history_.size() + 1u);
        scene_asset_ = *command.after_scene;
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Entity name edit transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::rename_entity(scene::SceneEntityId entity, std::string_view name) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->rename_entity(entity, name);
}

foundation::Result EditorSceneRuntime::Impl::set_entity_tag(scene::SceneEntityId entity, std::string_view tag) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Entity tag editing is only available in Edit mode");
    }
    if (component_edit_active_ || transform_edit_.active) {
        return foundation::invalid_state("Entity metadata editing cannot overlap another editor transaction");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset_;
        scene::SceneAsset after_scene = scene_asset_;
        if (const Result result = after_scene.set_entity_tag(entity, std::string(tag)); !result) {
            return result;
        }
        command.after_scene = std::move(after_scene);
        history_.reserve(history_.size() + 1u);
        scene_asset_ = *command.after_scene;
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Entity tag edit transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::set_entity_tag(scene::SceneEntityId entity, std::string_view tag) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->set_entity_tag(entity, tag);
}

foundation::Result EditorSceneRuntime::Impl::add_component(std::span<const scene::SceneEntityId> entities,
                                                           ecs::ComponentTypeId component) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Component addition is only available in Edit mode");
    }
    if (!component.is_valid() || entities.empty()) {
        return foundation::invalid_argument("Component addition input is invalid");
    }
    if (component_edit_active_ || transform_edit_.active) {
        return foundation::invalid_state("Component addition overlaps another editor transaction");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset_;
        scene::SceneAsset after_scene = scene_asset_;
        std::unordered_set<scene::SceneEntityId, scene::Guid128Hash<scene::SceneEntityIdTag>> seen;
        seen.reserve(entities.size());
        for (const scene::SceneEntityId entity : entities) {
            if (!seen.emplace(entity).second) {
                return foundation::invalid_argument("Component addition contains duplicate entities");
            }
            if (std::find_if(after_scene.entities().begin(), after_scene.entities().end(), [entity](const auto& value) {
                    return value.id == entity;
                }) == after_scene.entities().end()) {
                return foundation::not_found("Component addition entity is not present");
            }
            if (std::find_if(after_scene.components().begin(), after_scene.components().end(),
                             [entity, component](const auto& value) {
                                 return value.entity == entity && value.component_type == component;
                             }) != after_scene.components().end()) {
                return foundation::invalid_state("Component is already present on the entity");
            }
            if (component == ecs::ComponentTypeId(aeg::engine::components::kTransformName)) {
                Transform value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry_,
                        reflection::TypeId(aeg::engine::components::kTransformName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kCameraName)) {
                Camera value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry_,
                        reflection::TypeId(aeg::engine::components::kCameraName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName)) {
                MeshRenderer value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry_,
                        reflection::TypeId(aeg::engine::components::kMeshRendererName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kTextRendererName)) {
                TextRenderer value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry_,
                        reflection::TypeId(aeg::engine::components::kTextRendererName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kDirectionalLightName)) {
                DirectionalLight value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry_,
                        reflection::TypeId(aeg::engine::components::kDirectionalLightName), &value);
                    !result) {
                    return result;
                }
            } else {
                return foundation::unsupported("Component schema is not supported by the Inspector");
            }
        }
        command.after_scene = std::move(after_scene);
        command.rebuild_edit_world = true;
        history_.reserve(history_.size() + 1u);
        scene_asset_ = *command.after_scene;
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset_ = *command.before_scene;
            static_cast<void>(rebuild_edit_world());
            return result;
        }
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Component addition transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::add_component(std::span<const scene::SceneEntityId> entities,
                                                     ecs::ComponentTypeId component) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->add_component(entities, component);
}

foundation::Result EditorSceneRuntime::Impl::remove_component(std::span<const scene::SceneEntityId> entities,
                                                              ecs::ComponentTypeId component) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Component removal is only available in Edit mode");
    }
    if (!component.is_valid() || entities.empty()) {
        return foundation::invalid_argument("Component removal input is invalid");
    }
    if (component_edit_active_ || transform_edit_.active) {
        return foundation::invalid_state("Component removal overlaps another editor transaction");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset_;
        scene::SceneAsset after_scene = scene_asset_;
        std::unordered_set<scene::SceneEntityId, scene::Guid128Hash<scene::SceneEntityIdTag>> seen;
        seen.reserve(entities.size());
        for (const scene::SceneEntityId entity : entities) {
            if (!seen.emplace(entity).second) {
                return foundation::invalid_argument("Component removal contains duplicate entities");
            }
            if (const Result result = after_scene.remove_component(entity, component); !result) {
                return result;
            }
        }
        command.after_scene = std::move(after_scene);
        command.rebuild_edit_world = true;
        history_.reserve(history_.size() + 1u);
        scene_asset_ = *command.after_scene;
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset_ = *command.before_scene;
            static_cast<void>(rebuild_edit_world());
            return result;
        }
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Component removal transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::remove_component(std::span<const scene::SceneEntityId> entities,
                                                        ecs::ComponentTypeId component) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->remove_component(entities, component);
}

foundation::Result EditorSceneRuntime::Impl::duplicate_entities(std::span<const scene::SceneEntityId> entities) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Scene duplication is only available in Edit mode");
    }
    if (entities.empty()) {
        return foundation::invalid_argument("Scene duplicate operation has no entities");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset_;
        scene::SceneAsset after_scene = scene_asset_;
        std::unordered_set<scene::SceneEntityId, scene::Guid128Hash<scene::SceneEntityIdTag>> source_set;
        source_set.reserve(entities.size());
        for (const scene::SceneEntityId entity : entities) {
            if (!entity.is_valid() || std::find_if(scene_asset_.entities().begin(), scene_asset_.entities().end(),
                                                   [entity](const auto& value) { return value.id == entity; }) ==
                                          scene_asset_.entities().end()) {
                return foundation::not_found("Scene duplicate source is not present in the asset");
            }
            if (!source_set.emplace(entity).second) {
                return foundation::invalid_argument("Scene duplicate sources contain a duplicate entity");
            }
        }
        for (const scene::SceneEntityId source : entities) {
            scene::SceneEntityId current = source;
            while (current.is_valid()) {
                const auto found = std::find_if(scene_asset_.entities().begin(), scene_asset_.entities().end(),
                                                [current](const auto& value) { return value.id == current; });
                if (found == scene_asset_.entities().end()) {
                    return foundation::not_found("Scene duplicate hierarchy is invalid");
                }
                current = found->parent;
                if (source_set.contains(current)) {
                    return foundation::invalid_argument("Scene duplicate sources cannot contain nested entities");
                }
            }
        }
        uint64_t next_id = 1;
        const auto allocate_id = [&]() -> foundation::ResultValue<scene::SceneEntityId> {
            while (next_id != 0) {
                const scene::SceneEntityId candidate{after_scene.id().high(), next_id};
                if (std::find_if(after_scene.entities().begin(), after_scene.entities().end(),
                                 [candidate](const auto& entity) { return entity.id == candidate; }) ==
                    after_scene.entities().end()) {
                    next_id = next_id == std::numeric_limits<uint64_t>::max() ? 0 : next_id + 1u;
                    return {Result::success(), candidate};
                }
                next_id = next_id == std::numeric_limits<uint64_t>::max() ? 0 : next_id + 1u;
            }
            return {foundation::invalid_state("Scene entity ID space is exhausted"), {}};
        };
        std::unordered_map<scene::SceneEntityId, scene::SceneEntityId, scene::Guid128Hash<scene::SceneEntityIdTag>>
            remap;
        std::vector<scene::SceneEntityRecord> clone_records;
        std::vector<scene::SceneEntityId> duplicated;
        for (const auto& entity : scene_asset_.entities()) {
            bool in_subtree = source_set.contains(entity.id);
            if (!in_subtree) {
                scene::SceneEntityId current = entity.parent;
                while (current.is_valid()) {
                    if (source_set.contains(current)) {
                        in_subtree = true;
                        break;
                    }
                    const auto parent = std::find_if(scene_asset_.entities().begin(), scene_asset_.entities().end(),
                                                     [current](const auto& value) { return value.id == current; });
                    if (parent == scene_asset_.entities().end()) {
                        break;
                    }
                    current = parent->parent;
                }
            }
            if (!in_subtree) {
                continue;
            }
            const auto new_id = allocate_id();
            if (!new_id) {
                return new_id.result;
            }
            remap.emplace(entity.id, new_id.value);
            clone_records.push_back(entity);
            duplicated.push_back(new_id.value);
        }
        for (const auto& entity : clone_records) {
            const auto mapped_id = remap.find(entity.id);
            if (mapped_id == remap.end()) {
                return foundation::invalid_state("Scene duplicate ID remap is incomplete");
            }
            const auto parent = remap.contains(entity.parent) ? remap.at(entity.parent) : entity.parent;
            if (const Result result =
                    after_scene.add_entity({mapped_id->second, parent, entity.name + " Copy", entity.flags});
                !result) {
                return result;
            }
        }
        for (const auto& component : scene_asset_.components()) {
            const auto mapped = remap.find(component.entity);
            if (mapped == remap.end()) {
                continue;
            }
            if (const Result result =
                    after_scene.add_component({mapped->second, component.component_type, component.schema_type,
                                               component.schema_version, component.payload});
                !result) {
                return result;
            }
        }
        command.after_scene = after_scene;
        command.rebuild_edit_world = true;
        history_.reserve(history_.size() + 1u);
        scene_asset_ = std::move(after_scene);
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset_ = *command.before_scene;
            static_cast<void>(rebuild_edit_world());
            return result;
        }
        selected_entities_ = std::move(duplicated);
        selected_entity_ = selected_entities_.empty() ? scene::SceneEntityId{} : selected_entities_.front();
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Scene duplicate transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::duplicate_entities(std::span<const scene::SceneEntityId> entities) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->duplicate_entities(entities);
}

foundation::Result EditorSceneRuntime::Impl::remove_entities(std::span<const scene::SceneEntityId> entities,
                                                             scene::SceneEntityRemovalPolicy policy) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Scene entity removal is only available in Edit mode");
    }
    if (std::find(entities.begin(), entities.end(), active_camera_scene_) != entities.end()) {
        return foundation::invalid_state("The active editor camera cannot be removed");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset_;
        scene::SceneAsset after_scene = scene_asset_;
        if (const Result result = after_scene.remove_entities(entities, policy); !result) {
            return result;
        }
        command.after_scene = after_scene;
        command.rebuild_edit_world = true;
        history_.reserve(history_.size() + 1u);
        scene_asset_ = std::move(after_scene);
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset_ = *command.before_scene;
            static_cast<void>(rebuild_edit_world());
            return result;
        }
        selected_entities_.erase(
            std::remove_if(selected_entities_.begin(), selected_entities_.end(),
                           [this](scene::SceneEntityId id) {
                               return std::find_if(scene_asset_.entities().begin(), scene_asset_.entities().end(),
                                                   [id](const auto& entity) { return entity.id == id; }) ==
                                      scene_asset_.entities().end();
                           }),
            selected_entities_.end());
        selected_entity_ = selected_entities_.empty() ? scene::SceneEntityId{} : selected_entities_.front();
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Scene entity removal transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::remove_entities(std::span<const scene::SceneEntityId> entities,
                                                       scene::SceneEntityRemovalPolicy policy) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->remove_entities(entities, policy);
}

foundation::Result EditorSceneRuntime::Impl::drop_resource(EditorResourceKind kind, uint64_t id) {
    if (mode_ != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Resource binding is only available in Edit mode");
    }
    if (kind != EditorResourceKind::kMesh && kind != EditorResourceKind::kMaterial) {
        return foundation::invalid_argument("Only Mesh and Material assets can be dropped into the Viewport");
    }
    const EditorAssetKind asset_kind =
        kind == EditorResourceKind::kMesh ? EditorAssetKind::kMesh : EditorAssetKind::kMaterial;
    const bool built_in = kind == EditorResourceKind::kMesh ? id == kMeshAssetA || id == kMeshAssetB
                                                            : id == kMaterialAssetRed || id == kMaterialAssetGreen;
    if (!built_in && !asset_database_.is_valid(id, asset_kind)) {
        return foundation::not_found("Dropped Asset is missing or has failed import");
    }
    if (transform_edit_.active || component_edit_active_) {
        return foundation::invalid_state("Resource binding overlaps another editor transaction");
    }

    std::vector<scene::SceneEntityId> targets(selected_entities_.begin(), selected_entities_.end());
    if (targets.empty()) {
        std::string name = kind == EditorResourceKind::kMesh ? "Mesh Entity" : "Material Entity";
        if (const auto* asset = asset_database_.find(id); asset != nullptr) {
            name = asset->display_name;
        }
        if (const Result result = create_scene_entity({}, name); !result) {
            return result;
        }
        targets.push_back(selected_entity_);
    }

    const ecs::ComponentTypeId mesh_type(aeg::engine::components::kMeshRendererName);
    for (const scene::SceneEntityId entity : targets) {
        if (component_value_in(edit_instance_, edit_state_.get(), entity, mesh_type) == nullptr) {
            const std::array<scene::SceneEntityId, 1> single{{entity}};
            if (const Result result = add_component(single, mesh_type); !result) {
                return result;
            }
        }
    }

    std::vector<EditorComponentValue> before_values;
    std::vector<EditorComponentValue> after_values;
    try {
        before_values.reserve(targets.size());
        after_values.reserve(targets.size());
        for (const scene::SceneEntityId entity : targets) {
            const auto* current = static_cast<const MeshRenderer*>(
                component_value_in(edit_instance_, edit_state_.get(), entity, mesh_type));
            if (current == nullptr) {
                return foundation::not_found("Dropped resource target has no MeshRenderer component");
            }
            MeshRenderer next = *current;
            if (kind == EditorResourceKind::kMesh) {
                next.mesh_asset_id = id;
            } else {
                next.material_asset_id = id;
            }
            before_values.push_back({entity, mesh_type, bytes_of(*current)});
            after_values.push_back({entity, mesh_type, bytes_of(next)});
        }
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Resource binding transaction allocation failed");
    }
    if (const Result result = enqueue_values(after_values); !result) {
        return result;
    }
    EditorCommand command{};
    command.before = std::move(before_values);
    command.after = std::move(after_values);
    try {
        history_.reserve(history_.size() + 1u);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Resource binding history allocation failed");
    }
    return commit_history(std::move(command));
}

foundation::Result EditorSceneRuntime::drop_resource(EditorResourceKind kind, uint64_t id) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->drop_resource(kind, id);
}

foundation::ResultValue<ecs::EntityId> EditorSceneRuntime::Impl::runtime_entity_in(const scene::SceneInstance& instance,
                                                                                   scene::SceneEntityId entity) const {
    return instance.runtime_entity(entity);
}

foundation::ResultValue<ecs::EntityId> EditorSceneRuntime::runtime_entity(scene::SceneEntityId entity) const {
    if (impl_ == nullptr || impl_->edit_scope_ == nullptr) {
        return {foundation::invalid_state("Editor edit World is not initialized"), {}};
    }
    return impl_->runtime_entity_in(impl_->edit_instance_, entity);
}

const ecs::World* EditorSceneRuntime::edit_world() const noexcept {
    return impl_ == nullptr || impl_->edit_state_ == nullptr ? nullptr : impl_->edit_state_.get();
}

foundation::ResultValue<ecs::ComponentDescriptor> EditorSceneRuntime::component_descriptor(
    ecs::ComponentTypeId type) const {
    if (impl_ == nullptr || impl_->edit_state_ == nullptr) {
        return {foundation::invalid_state("Editor edit World is not initialized"), {}};
    }
    return impl_->edit_state_->find_component_descriptor(type);
}

const void* EditorSceneRuntime::Impl::component_value_in(const scene::SceneInstance& instance, const ecs::World* state,
                                                         scene::SceneEntityId entity, ecs::ComponentTypeId type) const {
    if (state == nullptr) {
        return nullptr;
    }
    const auto runtime = instance.runtime_entity(entity);
    return runtime ? state->get_component_data(runtime.value, type).value : nullptr;
}

const void* EditorSceneRuntime::component_value(scene::SceneEntityId entity, ecs::ComponentTypeId type) const {
    return impl_ == nullptr ? nullptr
                            : impl_->component_value_in(impl_->edit_instance_, impl_->edit_state_.get(), entity, type);
}

std::vector<ecs::ComponentTypeId> EditorSceneRuntime::selected_component_types() const {
    std::vector<ecs::ComponentTypeId> result;
    if (impl_ == nullptr || impl_->edit_state_ == nullptr || impl_->selected_entities_.empty()) {
        return result;
    }
    const auto runtime = impl_->edit_instance_.runtime_entity(impl_->selected_entities_.front());
    if (!runtime) {
        return result;
    }
    const auto snapshot = impl_->edit_scope_->latest_snapshot();
    if (!snapshot) {
        return result;
    }
    for (std::size_t index = 0; index < snapshot.value->component_type_count(); ++index) {
        const ecs::ComponentTypeId type = snapshot.value->component_type_at(index);
        bool common = true;
        for (const scene::SceneEntityId entity : impl_->selected_entities_) {
            const auto selected_runtime = impl_->edit_instance_.runtime_entity(entity);
            if (!selected_runtime || !impl_->edit_state_->get_component_data(selected_runtime.value, type)) {
                common = false;
                break;
            }
        }
        if (common) {
            result.push_back(type);
        }
    }
    return result;
}

std::vector<EditorResourceDescriptor> EditorSceneRuntime::resource_descriptors() const {
    std::vector<EditorResourceDescriptor> result{};
    if (impl_ == nullptr || !impl_->project_manager_.loaded()) {
        result = {
            {EditorResourceKind::kMesh, kMeshAssetA, "Mesh A", true, EditorAssetStatus::kImported, {}},
            {EditorResourceKind::kMesh, kMeshAssetB, "Mesh B", true, EditorAssetStatus::kImported, {}},
            {EditorResourceKind::kMaterial, kMaterialAssetRed, "Material Red", true, EditorAssetStatus::kImported, {}},
            {EditorResourceKind::kMaterial, kMaterialAssetGreen, "Material Green", true,
             EditorAssetStatus::kImported, {}},
        };
    }
    if (impl_ == nullptr) {
        return result;
    }
    const auto map_kind = [](EditorAssetKind kind) {
        switch (kind) {
            case EditorAssetKind::kMesh:
                return EditorResourceKind::kMesh;
            case EditorAssetKind::kMaterial:
                return EditorResourceKind::kMaterial;
            case EditorAssetKind::kTexture:
                return EditorResourceKind::kTexture;
            case EditorAssetKind::kScene:
                return EditorResourceKind::kScene;
            case EditorAssetKind::kFont:
                return EditorResourceKind::kFont;
            case EditorAssetKind::kUnknown:
                return EditorResourceKind::kUnknown;
        }
        return EditorResourceKind::kUnknown;
    };
    for (const auto& asset : impl_->asset_database_.assets()) {
        result.push_back({map_kind(asset.kind), asset.id, asset.display_name,
                          asset.status == EditorAssetStatus::kImported, asset.status, asset.relative_path});
    }
    return result;
}

bool EditorSceneRuntime::is_resource_valid(EditorResourceKind kind, uint64_t id) const noexcept {
    if (kind == EditorResourceKind::kMesh) {
        return id == kMeshAssetA || id == kMeshAssetB ||
               (impl_ != nullptr && impl_->asset_database_.is_valid(id, EditorAssetKind::kMesh));
    }
    if (kind == EditorResourceKind::kMaterial) {
        return id == kMaterialAssetRed || id == kMaterialAssetGreen ||
               (impl_ != nullptr && impl_->asset_database_.is_valid(id, EditorAssetKind::kMaterial));
    }
    if (kind == EditorResourceKind::kTexture) {
        return impl_ != nullptr && impl_->asset_database_.is_valid(id, EditorAssetKind::kTexture);
    }
    if (kind == EditorResourceKind::kScene) {
        return impl_ != nullptr && impl_->asset_database_.is_valid(id, EditorAssetKind::kScene);
    }
    if (kind == EditorResourceKind::kFont) {
        return impl_ != nullptr && impl_->asset_database_.is_valid(id, EditorAssetKind::kFont);
    }
    return false;
}

foundation::Result EditorSceneRuntime::queue_component_update(scene::SceneEntityId entity,
                                                              ecs::ComponentTypeId component,
                                                              std::span<const std::byte> bytes) {
    const ComponentUpdateView update{entity, component, bytes};
    return queue_component_updates(std::span{&update, 1});
}

foundation::Result EditorSceneRuntime::queue_component_updates(std::span<const ComponentUpdateView> updates) {
    const Result begin = begin_component_edit(updates);
    if (!begin || !impl_->component_edit_active_) {
        return begin;
    }
    return commit_component_edit();
}

foundation::Result EditorSceneRuntime::begin_component_edit(std::span<const ComponentUpdateView> updates) {
    if (impl_ == nullptr || impl_->edit_scope_ == nullptr || impl_->edit_state_ == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    if (impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Component editing is only available in Edit mode");
    }
    if (impl_->transform_edit_.active || impl_->component_edit_active_) {
        return foundation::invalid_state("Component edit overlaps another editor transaction");
    }
    if (updates.empty()) {
        return foundation::invalid_argument("Editor component edit is empty");
    }
    try {
        Impl::EditorCommand command{};
        command.before.reserve(updates.size());
        command.after.reserve(updates.size());
        std::unordered_set<uint64_t> identities;
        identities.reserve(updates.size());
        for (const ComponentUpdateView& update : updates) {
            const uint64_t identity = update.entity.low() ^ (update.entity.high() * 0x9e3779b97f4a7c15ull) ^
                                      (update.component.value() * 0xc2b2ae3d27d4eb4full);
            if (!identities.emplace(identity).second) {
                return foundation::invalid_argument("Editor component edit contains duplicate fields");
            }
            const auto runtime = impl_->edit_instance_.runtime_entity(update.entity);
            if (!runtime) {
                return runtime.result;
            }
            const auto descriptor = impl_->edit_state_->find_component_descriptor(update.component);
            if (!descriptor) {
                return descriptor.result;
            }
            if (update.bytes.size() != descriptor.value.size) {
                return foundation::invalid_argument("Editor component payload size does not match ECS storage");
            }
            if (const Result result = validate_component_payload(update.component, update.bytes); !result) {
                return result;
            }
            const auto current = impl_->edit_state_->get_component_data(runtime.value, update.component);
            if (!current) {
                return current.result;
            }
            command.before.push_back(Impl::EditorComponentValue{
                update.entity, update.component,
                std::vector<std::byte>(static_cast<const std::byte*>(current.value),
                                       static_cast<const std::byte*>(current.value) + descriptor.value.size)});
            command.after.push_back(Impl::EditorComponentValue{
                update.entity, update.component, std::vector<std::byte>(update.bytes.begin(), update.bytes.end())});
        }
        bool changed = false;
        for (std::size_t index = 0; index < command.before.size(); ++index) {
            if (command.before[index].bytes != command.after[index].bytes) {
                changed = true;
                break;
            }
        }
        if (!changed) {
            return Result::success();
        }
        impl_->history_.reserve(impl_->history_.size() + 1u);
        if (const Result result = impl_->enqueue_values(command.after); !result) {
            return result;
        }
        impl_->component_edit_command_ = std::move(command);
        impl_->component_edit_active_ = true;
        impl_->refresh_dirty();
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor component edit allocation failed");
    }
}

foundation::Result EditorSceneRuntime::update_component_edit(std::span<const ComponentUpdateView> updates) {
    if (impl_ == nullptr || !impl_->component_edit_active_) {
        return foundation::invalid_state("No Inspector component edit transaction is active");
    }
    if (updates.size() != impl_->component_edit_command_.after.size()) {
        return foundation::invalid_argument("Inspector component edit field count changed during a transaction");
    }
    try {
        std::vector<Impl::EditorComponentValue> next;
        next.reserve(updates.size());
        for (std::size_t index = 0; index < updates.size(); ++index) {
            const ComponentUpdateView& update = updates[index];
            const Impl::EditorComponentValue& original = impl_->component_edit_command_.after[index];
            if (update.entity != original.entity || update.component != original.component) {
                return foundation::invalid_argument("Inspector component edit fields changed during a transaction");
            }
            const auto descriptor = impl_->edit_state_->find_component_descriptor(update.component);
            if (!descriptor) {
                return descriptor.result;
            }
            if (update.bytes.size() != descriptor.value.size) {
                return foundation::invalid_argument("Editor component payload size does not match ECS storage");
            }
            if (const Result result = validate_component_payload(update.component, update.bytes); !result) {
                return result;
            }
            next.push_back(Impl::EditorComponentValue{
                update.entity, update.component, std::vector<std::byte>(update.bytes.begin(), update.bytes.end())});
        }
        bool changed = false;
        for (std::size_t index = 0; index < next.size(); ++index) {
            if (next[index].bytes != impl_->component_edit_command_.after[index].bytes) {
                changed = true;
                break;
            }
        }
        if (!changed) {
            return Result::success();
        }
        if (const Result result = impl_->enqueue_values(next); !result) {
            return result;
        }
        impl_->component_edit_command_.after = std::move(next);
        impl_->refresh_dirty();
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor component edit update allocation failed");
    }
}

foundation::Result EditorSceneRuntime::commit_component_edit() {
    if (impl_ == nullptr || !impl_->component_edit_active_) {
        return foundation::invalid_state("No Inspector component edit transaction is active");
    }
    Impl::EditorCommand command = std::move(impl_->component_edit_command_);
    impl_->component_edit_command_ = {};
    impl_->component_edit_active_ = false;
    const Result result = impl_->commit_history(std::move(command));
    impl_->refresh_dirty();
    return result;
}

foundation::Result EditorSceneRuntime::cancel_component_edit() {
    if (impl_ == nullptr || !impl_->component_edit_active_) {
        return foundation::invalid_state("No Inspector component edit transaction is active");
    }
    const Result queued = impl_->enqueue_values(impl_->component_edit_command_.before);
    if (!queued) {
        return queued;
    }
    impl_->component_edit_command_ = {};
    impl_->component_edit_active_ = false;
    impl_->refresh_dirty();
    return Result::success();
}

bool EditorSceneRuntime::component_edit_active() const noexcept {
    return impl_ != nullptr && impl_->component_edit_active_;
}

foundation::Result EditorSceneRuntime::new_project(const std::filesystem::path& root, std::string_view name) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode_ != Mode::kEdit || impl_->transform_edit_.active || impl_->component_edit_active_) {
        return foundation::invalid_state("New Project requires Edit mode without an active edit transaction");
    }
    if (const Result result = impl_->project_manager_.new_project(root, name); !result) {
        return result;
    }
    if (const Result result = impl_->asset_database_.initialize(impl_->project_manager_.descriptor().root); !result) {
        static_cast<void>(impl_->project_manager_.close_project());
        return result;
    }
    if (const Result result = impl_->synchronize_asset_pipeline(); !result) {
        static_cast<void>(impl_->project_manager_.close_project());
        return result;
    }
    if (const Result result = impl_->reset_scene_for_project(); !result) {
        return result;
    }
    return save_scene();
}

foundation::Result EditorSceneRuntime::import_project(const std::filesystem::path& root_or_project_file) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode_ != Mode::kEdit || impl_->transform_edit_.active || impl_->component_edit_active_) {
        return foundation::invalid_state("Import Project requires Edit mode without an active edit transaction");
    }
    if (const Result result = impl_->project_manager_.import_project(root_or_project_file); !result) {
        return result;
    }
    // A freshly imported project owns no visible Renderer Pipeline until its
    // scene sidecar is restored by the editor application.
    impl_->project_renderer_pipeline_.reset();
    if (const Result result = impl_->asset_database_.initialize(impl_->project_manager_.descriptor().root); !result) {
        static_cast<void>(impl_->project_manager_.close_project());
        return result;
    }
    if (const Result result = impl_->synchronize_asset_pipeline(); !result) {
        static_cast<void>(impl_->project_manager_.close_project());
        return result;
    }
    const std::filesystem::path remembered_scene = impl_->project_manager_.recent_scene();
    const std::filesystem::path default_scene = impl_->project_manager_.default_scene_file();
    if (!remembered_scene.empty()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(remembered_scene, error);
        if (error) {
            return foundation::invalid_state("Could not inspect the recent project scene: " + error.message());
        }
        if (exists) {
            return open_scene(remembered_scene);
        }
    }
    {
        std::error_code error;
        const bool exists = std::filesystem::exists(default_scene, error);
        if (error) {
            return foundation::invalid_state("Could not inspect the default project scene: " + error.message());
        }
        if (exists) {
            return open_scene(default_scene);
        }
    }
    if (const Result result = impl_->reset_scene_for_project(); !result) {
        return result;
    }
    return save_scene();
}

foundation::Result EditorSceneRuntime::new_scene() {
    if (impl_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->component_edit_active_) {
        if (const Result result = cancel_component_edit(); !result) {
            return result;
        }
    }
    if (impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("New Scene requires Edit mode");
    }
    const Result scene_result = impl_->project_manager_.loaded() ? impl_->make_empty_project_scene()
                                                                 : impl_->make_default_scene();
    if (!scene_result) {
        return scene_result;
    }
    if (!impl_->project_manager_.loaded()) {
        impl_->active_camera_scene_ = kCameraEntity;
    }
    impl_->transform_edit_ = {};
    impl_->selected_entity_ = impl_->project_manager_.loaded() ? scene::SceneEntityId{} : kLeftMeshEntity;
    impl_->selected_entities_.clear();
    if (impl_->selected_entity_.is_valid()) {
        impl_->selected_entities_.push_back(impl_->selected_entity_);
    }
    impl_->mode_ = Mode::kEdit;
    impl_->runtime_scope_.reset();
    impl_->runtime_state_.reset();
    impl_->active_scope_ = nullptr;
    const Result result = impl_->rebuild_edit_world();
    if (result) {
        impl_->reset_history(true);
        impl_->scene_path_ = impl_->project_manager_.loaded() ? impl_->project_manager_.default_scene_file()
                                                              : std::filesystem::path{"editor_scene.aegscene"};
    }
    return result;
}

foundation::Result EditorSceneRuntime::open_scene(const std::filesystem::path& path) {
    if (impl_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Open Scene requires Edit mode");
    }
    std::filesystem::path resolved_path = path;
    if (impl_->project_manager_.loaded()) {
        const auto resolved = impl_->project_manager_.resolve_scene_path(path);
        if (!resolved) {
            return resolved.result;
        }
        resolved_path = resolved.value;
    } else if (resolved_path.is_relative()) {
        std::error_code path_error;
        resolved_path = std::filesystem::weakly_canonical(resolved_path, path_error);
        if (path_error) {
            return foundation::invalid_argument("Scene path is invalid");
        }
    }
    std::ifstream file(resolved_path, std::ios::binary);
    if (!file) {
        return foundation::not_found("Scene file could not be opened");
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0) {
        return foundation::invalid_argument("Scene file is empty");
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) {
        return Result::failure(ResultCode::kUnknown, "Scene file read failed");
    }
    serialization::BinaryReader reader(bytes);
    const auto asset = scene::SceneSerializer::deserialize(reader);
    if (!asset) {
        return asset.result;
    }
    if (const Result result = asset.value.validate(); !result) {
        return result;
    }
    const scene::SceneAsset previous_asset = impl_->scene_asset_;
    const std::filesystem::path previous_path = impl_->scene_path_;
    const std::vector<scene::SceneEntityId> previous_selection = impl_->selected_entities_;
    const scene::SceneEntityId previous_selected_entity = impl_->selected_entity_;
    const scene::SceneEntityId previous_camera = impl_->active_camera_scene_;
    if (impl_->component_edit_active_) {
        if (const Result result = cancel_component_edit(); !result) {
            return result;
        }
    }
    impl_->transform_edit_ = {};
    impl_->scene_asset_ = asset.value;
    impl_->scene_path_ = resolved_path;
    impl_->active_camera_scene_ = {};
    const ecs::ComponentTypeId camera_type(aeg::engine::components::kCameraName);
    for (const scene::SceneComponentRecord& component : impl_->scene_asset_.components()) {
        if (component.component_type == camera_type) {
            impl_->active_camera_scene_ = component.entity;
            break;
        }
    }
    impl_->selected_entity_ =
        impl_->scene_asset_.entities().empty() ? scene::SceneEntityId{} : impl_->scene_asset_.entities().front().id;
    impl_->selected_entities_.clear();
    if (impl_->selected_entity_.is_valid()) {
        impl_->selected_entities_.push_back(impl_->selected_entity_);
    }
    impl_->mode_ = Mode::kEdit;
    impl_->runtime_scope_.reset();
    impl_->runtime_state_.reset();
    impl_->active_scope_ = nullptr;
    const Result result = impl_->rebuild_edit_world();
    if (result) {
        impl_->reset_history(false);
        if (impl_->project_manager_.loaded()) {
            static_cast<void>(impl_->project_manager_.remember_scene(impl_->scene_path_));
        }
    } else {
        impl_->scene_asset_ = previous_asset;
        impl_->scene_path_ = previous_path;
        impl_->selected_entities_ = previous_selection;
        impl_->selected_entity_ = previous_selected_entity;
        impl_->active_camera_scene_ = previous_camera;
        impl_->mode_ = Mode::kEdit;
        impl_->runtime_scope_.reset();
        impl_->runtime_state_.reset();
        impl_->active_scope_ = nullptr;
        static_cast<void>(impl_->rebuild_edit_world());
    }
    return result;
}

foundation::Result EditorSceneRuntime::save_scene() {
    if (impl_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->transform_edit_.active) {
        return foundation::invalid_state("Save requires the active Transform edit to be committed or cancelled");
    }
    if (impl_->component_edit_active_) {
        if (const Result result = commit_component_edit(); !result) {
            return result;
        }
    }
    if (const Result result = impl_->commit_edit_commands(); !result) {
        return result;
    }
    serialization::BinaryWriter writer;
    if (const Result result = scene::SceneSerializer::serialize(impl_->scene_asset_, writer); !result) {
        return result;
    }
    if (impl_->project_manager_.loaded()) {
        const auto resolved = impl_->project_manager_.resolve_scene_path(impl_->scene_path_);
        if (!resolved) {
            return resolved.result;
        }
        impl_->scene_path_ = resolved.value;
    }
    if (const Result result = write_bytes_atomically(impl_->scene_path_, writer.data()); !result) {
        return result;
    }
    impl_->clean_history_cursor_ = impl_->history_cursor_;
    impl_->scene_dirty_base_ = false;
    impl_->refresh_dirty();
    if (impl_->project_manager_.loaded()) {
        static_cast<void>(impl_->project_manager_.remember_scene(impl_->scene_path_));
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::save_scene_as(const std::filesystem::path& path) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return aeg::foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (path.empty()) {
        return foundation::invalid_argument("Scene Save As path is empty");
    }
    std::filesystem::path resolved_path = path;
    if (impl_->project_manager_.loaded()) {
        const auto resolved = impl_->project_manager_.resolve_scene_path(path);
        if (!resolved) {
            return resolved.result;
        }
        resolved_path = resolved.value;
    } else if (resolved_path.is_relative()) {
        std::error_code path_error;
        resolved_path = std::filesystem::absolute(resolved_path, path_error);
        if (path_error) {
            return foundation::invalid_argument("Scene Save As path is invalid");
        }
    }
    const std::filesystem::path previous_path = impl_->scene_path_;
    impl_->scene_path_ = resolved_path;
    const Result result = save_scene();
    if (!result) {
        impl_->scene_path_ = previous_path;
    }
    return result;
}

foundation::Result EditorSceneRuntime::refresh_assets() {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Asset database changes are only available in Edit mode");
    }
    if (!impl_->project_manager_.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    const Result result = impl_->asset_database_.refresh();
    if (!result) {
        return result;
    }
    return impl_->synchronize_asset_pipeline();
}

foundation::Result EditorSceneRuntime::reimport_asset(uint64_t id) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Asset database changes are only available in Edit mode");
    }
    if (!impl_->project_manager_.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    const Result result = impl_->asset_database_.reimport(id);
    if (!result) {
        return result;
    }
    if (const Result sync = impl_->synchronize_asset_pipeline(); !sync) {
        return sync;
    }
    return impl_->asset_pipeline_.request_reimport(id);
}

foundation::Result EditorSceneRuntime::rename_asset(uint64_t id, std::string_view new_name) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Asset database changes are only available in Edit mode");
    }
    if (!impl_->project_manager_.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    return impl_->asset_database_.rename(id, new_name);
}

foundation::Result EditorSceneRuntime::remove_asset(uint64_t id) {
    if (impl_ == nullptr || !impl_->initialized_) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Asset database changes are only available in Edit mode");
    }
    if (!impl_->project_manager_.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    return impl_->asset_database_.remove(id);
}

foundation::Result EditorSceneRuntime::play() {
    if (impl_ == nullptr || impl_->edit_scope_ == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    if (impl_->mode_ == Mode::kPlay) {
        return Result::success();
    }
    if (impl_->mode_ == Mode::kPause) {
        impl_->mode_ = Mode::kPlay;
        return Result::success();
    }
    if (impl_->transform_edit_.active) {
        return foundation::invalid_state("Play requires the active Transform edit to be committed or cancelled");
    }
    if (impl_->component_edit_active_) {
        if (const Result result = commit_component_edit(); !result) {
            return result;
        }
    }
    if (const Result result = impl_->commit_edit_commands(); !result) {
        return result;
    }
    impl_->runtime_scope_.reset();
    impl_->runtime_state_.reset();
    impl_->runtime_camera_runtime_ = {};
    impl_->runtime_demo_time_ = 0.0f;
    if (const Result result =
            impl_->instantiate_world(impl_->scene_asset_, impl_->runtime_state_, impl_->runtime_scope_,
                                     impl_->runtime_instance_, impl_->runtime_camera_runtime_, "Editor.RuntimeWorld");
        !result) {
        return result;
    }
    impl_->mode_ = Mode::kPlay;
    impl_->active_scope_ = impl_->runtime_scope_.get();
    return impl_->create_orchestrator_for(*impl_->active_scope_);
}

foundation::Result EditorSceneRuntime::pause() {
    if (impl_ == nullptr || impl_->mode_ != Mode::kPlay) {
        return foundation::invalid_state("Editor runtime is not playing");
    }
    impl_->mode_ = Mode::kPause;
    return Result::success();
}

foundation::Result EditorSceneRuntime::stop() {
    if (impl_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode_ == Mode::kEdit) {
        if (impl_->transform_edit_.active) {
            return cancel_transform_edit();
        }
        if (impl_->component_edit_active_) {
            return cancel_component_edit();
        }
        return Result::success();
    }
    impl_->transform_edit_ = {};
    impl_->orchestrator_.reset();
    impl_->runtime_scope_.reset();
    impl_->runtime_state_.reset();
    impl_->runtime_camera_runtime_ = {};
    impl_->runtime_demo_time_ = 0.0f;
    impl_->mode_ = Mode::kEdit;
    impl_->active_scope_ = impl_->edit_scope_.get();
    return impl_->create_orchestrator_for(*impl_->active_scope_);
}

bool EditorSceneRuntime::can_undo() const noexcept {
    return impl_ != nullptr && impl_->mode_ == Mode::kEdit && !impl_->transform_edit_.active &&
           impl_->history_cursor_ > 0;
}

bool EditorSceneRuntime::can_redo() const noexcept {
    return impl_ != nullptr && impl_->mode_ == Mode::kEdit && !impl_->transform_edit_.active &&
           impl_->history_cursor_ < impl_->history_.size();
}

foundation::Result EditorSceneRuntime::undo() {
    if (impl_ == nullptr || impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Undo is only available in Edit mode");
    }
    if (impl_->transform_edit_.active) {
        return foundation::invalid_state("Undo cannot run during an active Transform edit");
    }
    if (impl_->history_cursor_ == 0) {
        return foundation::not_found("Editor Undo history is empty");
    }
    const Impl::EditorCommand& command = impl_->history_[impl_->history_cursor_ - 1];
    if (const Result result = impl_->apply_history_command(command, true); !result) {
        return result;
    }
    --impl_->history_cursor_;
    impl_->refresh_dirty();
    return Result::success();
}

foundation::Result EditorSceneRuntime::redo() {
    if (impl_ == nullptr || impl_->mode_ != Mode::kEdit) {
        return foundation::invalid_state("Redo is only available in Edit mode");
    }
    if (impl_->transform_edit_.active) {
        return foundation::invalid_state("Redo cannot run during an active Transform edit");
    }
    if (impl_->history_cursor_ >= impl_->history_.size()) {
        return foundation::not_found("Editor Redo history is empty");
    }
    const Impl::EditorCommand& command = impl_->history_[impl_->history_cursor_];
    if (const Result result = impl_->apply_history_command(command, false); !result) {
        return result;
    }
    ++impl_->history_cursor_;
    impl_->refresh_dirty();
    return Result::success();
}
}  // namespace aeg::application
