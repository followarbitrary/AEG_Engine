/**
 * @file editor_scene_runtime.cpp
 * @brief Implements the minimum editable scene lifecycle and offscreen preview.
 */
#include "editor_scene_runtime.hpp"
#include "composition/editor_render_graph_inspection.hpp"
#include "composition/editor_feature_graph_document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Components/component_bundle.hpp"
#include "Components/component_ids.hpp"
#include "Components/component_render_extraction.hpp"
#include "deferred_lighting_resources.hpp"
#include "dynamic_module.hpp"
#include "module_bundle.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_orchestrator.hpp"
#include "engine_frame_world_commands.hpp"
#include "feature_execution_control.hpp"
#include "feature_clock_host.hpp"
#include "feature_world_commands.hpp"
#include "feature_world_transform.hpp"
#include "world_command_routes.hpp"
#include "world_scope.hpp"
#include "feature_graph_io.hpp"
#include "editor_renderer_feature_host.hpp"
#include "project_operation.hpp"
#include "editor_asset_pipeline.hpp"
#include "editor_font_pipeline.hpp"
#include "light_culling.hpp"
#include "clustered_forward.hpp"
#include "material.hpp"
#include "post_process.hpp"
#include "project_context.hpp"
#include "render_feature_draw.hpp"
#include "render_feature_history.hpp"
#include "render_pass_registry_execution.hpp"
#include "renderer_variants_internal.hpp"
#include "resource_uploader.hpp"
#include "shader_contract.hpp"
#include "shadow_rendering.hpp"
#include "texture_uploader.hpp"

namespace aeg::application {
namespace {
using aeg::engine::components::Camera;
using aeg::engine::components::DirectionalLight;
using aeg::engine::components::MeshRenderer;
using aeg::engine::components::PointLight;
using aeg::engine::components::SceneEnvironment;
using aeg::engine::components::SpotLight;
using aeg::engine::components::TextRenderer;
using aeg::engine::components::Transform;
using foundation::Result;
using foundation::ResultCode;
using render_graph::RenderGraphBuffer;
using render_graph::RenderGraphTexture;
using feature::RenderResourceId;
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

inline constexpr RenderResourceId kSceneColor{"Renderer.SceneColor"};
inline constexpr RenderResourceId kSceneDepth{"Renderer.SceneDepth"};
inline constexpr RenderResourceId kLightingBuffer{"Editor.SceneLighting"};
inline constexpr RenderResourceId kMaterialParameterRed{"Editor.MaterialParameter.Red"};
inline constexpr RenderResourceId kMaterialParameterGreen{"Editor.MaterialParameter.Green"};
inline constexpr RenderResourceId kMaterialTextureRed{"Editor.MaterialTexture.Red"};
inline constexpr RenderResourceId kMaterialTextureGreen{"Editor.MaterialTexture.Green"};
inline constexpr uint32_t kFramesInFlight = 3;
inline constexpr uint32_t kMaterialCount = 2;
inline constexpr uint64_t kMeshAssetA = 1;
inline constexpr uint64_t kMeshAssetB = 2;

[[nodiscard]] Result reject_retired_scene_authoring(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
    });
    if (extension == ".aegframe" || extension == ".aegpipe" || extension == ".aegpipeline" || extension == ".pipeline")
        return foundation::unsupported("Retired Frame/Pipeline author documents cannot be opened as a Scene");
    if (path.empty()) return Result::success();
    std::error_code error;
    const auto sidecar = std::filesystem::path{path.string() + ".pipeline"};
    const bool exists = std::filesystem::exists(sidecar, error);
    if (error)
        return foundation::invalid_state("Could not inspect retired Scene author sidecar: " + error.message());
    if (exists)
        return foundation::unsupported("Retired Scene author sidecar is unsupported: " + sidecar.string());
    return Result::success();
}

struct ViewportReadbackState final {
    bool requested{false};
    bool pass_built{false};
    bool pass_executed{false};
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

[[nodiscard]] foundation::ResultValue<ResourceState> query_mesh_buffer_initial_state(Device& device,
                                                                                     BufferHandle buffer) {
    const auto snapshot = device.query_buffer_state(buffer);
    if (!snapshot)
        return {snapshot.result, {}};
    if (snapshot.value.segments.empty())
        return {foundation::invalid_state("Editor mesh buffer state snapshot is empty"), {}};
    // Mesh uploads and draws use the complete dedicated buffer. Do not assume
    // an unused upload has already transitioned from Copy to vertex/index input.
    const auto state = snapshot.value.segments.front().state;
    if (std::any_of(snapshot.value.segments.begin(), snapshot.value.segments.end(),
                    [&](const auto& segment) { return segment.state != state; }))
        return {foundation::invalid_state("Editor mesh buffer has mixed whole-buffer states"), {}};
    return {Result::success(), state};
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
        if (!math::is_finite(value.translation) || !math::is_finite(value.rotation) || !math::is_finite(value.scale)) {
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
        if (!math::is_finite(value.vertical_fov_radians) || value.vertical_fov_radians <= 0.0f ||
            value.vertical_fov_radians >= math::kPi || !math::is_finite(value.near_plane) ||
            !math::is_finite(value.far_plane) || value.near_plane <= 0.0f || value.far_plane <= value.near_plane) {
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
            return math::is_finite(scalar) && scalar >= 0.0f && scalar <= 1.0f;
        };
        if (!in_unit_range(value.tint.x) || !in_unit_range(value.tint.y) || !in_unit_range(value.tint.z) ||
            !in_unit_range(value.tint_alpha) || !math::is_finite(value.bounds_center) ||
            !math::is_finite(value.bounds_radius) || value.bounds_radius < 0.0f) {
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
        if (value.text.size() > 4096 || !math::is_finite(value.world_size) || value.world_size <= 0.0f ||
            !math::is_finite(value.color) || value.color.x < 0.0f || value.color.x > 1.0f || value.color.y < 0.0f ||
            value.color.y > 1.0f || value.color.z < 0.0f || value.color.z > 1.0f ||
            !math::is_finite(value.color_alpha) || value.color_alpha < 0.0f || value.color_alpha > 1.0f) {
            return foundation::invalid_argument("TextRenderer contains invalid editor values");
        }
        return Result::success();
    }
    if (component == ecs::ComponentTypeId(aeg::engine::components::kSceneEnvironmentName)) {
        if (bytes.size() != sizeof(SceneEnvironment))
            return foundation::invalid_argument("Scene Environment payload size is invalid");
        SceneEnvironment value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return aeg::engine::components::make_post_process_settings(value).result;
    }
    if (component == ecs::ComponentTypeId(aeg::engine::components::kPointLightName)) {
        if (bytes.size() != sizeof(PointLight))
            return foundation::invalid_argument("Point Light payload size is invalid");
        PointLight value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return aeg::engine::components::validate_point_light(value);
    }
    if (component == ecs::ComponentTypeId(aeg::engine::components::kSpotLightName)) {
        if (bytes.size() != sizeof(SpotLight))
            return foundation::invalid_argument("Spot Light payload size is invalid");
        SpotLight value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return aeg::engine::components::validate_spot_light(value);
    }
    if (component == ecs::ComponentTypeId(aeg::engine::components::kDirectionalLightName)) {
        if (bytes.size() != sizeof(DirectionalLight)) {
            return foundation::invalid_argument("Directional Light payload size is invalid");
        }
        DirectionalLight value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        const auto in_unit_range = [](float scalar) {
            return math::is_finite(scalar) && scalar >= 0.0f && scalar <= 1.0f;
        };
        math::Vec3 normalized_direction{};
        if (!math::is_finite(value.direction) || !in_unit_range(value.color.x) || !in_unit_range(value.color.y) ||
            !in_unit_range(value.color.z) || !math::is_finite(value.intensity) || value.intensity < 0.0f ||
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
            const auto staging_error = "Existing scene file could not be staged for replacement: " +
                path.generic_string() + " (" + std::to_string(error.value()) + ": " + error.message() + ")";
            std::filesystem::remove(temporary, error);
            return Result::failure(ResultCode::kUnknown, staging_error);
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        const auto replacement_error = "Scene file replacement failed: " + temporary.generic_string() +
            " -> " + path.generic_string() + " (" + std::to_string(error.value()) + ": " + error.message() + ")";
        if (had_target) {
            std::error_code restore_error;
            std::filesystem::rename(backup, path, restore_error);
        }
        std::filesystem::remove(temporary, error);
        return Result::failure(ResultCode::kUnknown, replacement_error);
    }
    if (had_target) {
        std::filesystem::remove(backup, error);
    }
    return Result::success();
}

[[nodiscard]] asset::WorldCommandRoute to_world_command_route(const EditorFeatureWorldBinding& binding) {
    return {binding.graph_asset_uuid, binding.node_id, binding.owner_path,
        foundation::AssetId{binding.scene_asset.uuid()}, binding.scene_entity.uuid(),
        binding.effect, binding.version, binding.observation_port};
}

[[nodiscard]] EditorFeatureWorldBinding to_editor_world_binding(asset::WorldCommandRoute route) {
    return {std::move(route.graph_asset_uuid), route.node_id, std::move(route.owner_path),
        scene::SceneAssetId::from_uuid(route.scene_asset.value), scene::SceneEntityId::from_uuid(route.scene_entity),
        route.effect, route.version, route.observation_port};
}

[[nodiscard]] Result write_editor_world_bindings(const std::filesystem::path& scene_path,
                                                 std::span<const EditorFeatureWorldBinding> bindings) {
    if (scene_path.empty() || bindings.size() > asset::kMaxWorldCommandRoutes)
        return foundation::invalid_argument("Editor World binding sidecar has an invalid path or route count");
    try {
        std::vector<asset::WorldCommandRoute> routes;
        routes.reserve(bindings.size());
        for (const auto& binding : bindings) routes.push_back(to_world_command_route(binding));
        const auto encoded = asset::encode_world_command_routes(routes);
        if (!encoded) return encoded.result;
        return write_bytes_atomically(asset::world_command_routes_sidecar_path(scene_path),
            {reinterpret_cast<const uint8_t*>(encoded.value.data()), encoded.value.size()});
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor World binding sidecar allocation failed");
    }
}

[[nodiscard]] foundation::ResultValue<std::vector<EditorFeatureWorldBinding>> read_editor_world_bindings(
    const std::filesystem::path& scene_path, scene::SceneAssetId expected_scene) {
    const auto path = asset::world_command_routes_sidecar_path(scene_path);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error)
        return {foundation::invalid_state("Editor World binding sidecar could not be inspected: " + error.message()), {}};
    if (!exists) return {Result::success(), {}};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {foundation::not_found("Editor World binding sidecar could not be opened"), {}};
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0 || static_cast<uint64_t>(size) > asset::kMaxWorldCommandRoutesBytes)
        return {foundation::invalid_argument("Editor World binding sidecar size is invalid"), {}};
    try {
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) return {Result::failure(ResultCode::kUnknown, "Editor World binding sidecar read failed"), {}};
        auto decoded = asset::decode_world_command_routes(bytes);
        if (!decoded) return {decoded.result, {}};
        std::vector<EditorFeatureWorldBinding> bindings;
        bindings.reserve(decoded.value.size());
        for (auto& route : decoded.value) {
            if (route.scene_asset.value != expected_scene.uuid())
                return {foundation::invalid_argument("Editor World binding sidecar references a different Scene asset"), {}};
            bindings.push_back(to_editor_world_binding(std::move(route)));
        }
        return {Result::success(), std::move(bindings)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Editor World binding sidecar allocation failed"), {}};
    }
}

[[nodiscard]] Result validate_editor_world_binding_targets(
    std::span<const EditorFeatureWorldBinding> bindings, const scene::SceneAsset& asset) {
    const ecs::ComponentTypeId transform_type(engine::components::kTransformName);
    for (const auto& binding : bindings) {
        if (binding.scene_asset != asset.id())
            return foundation::invalid_argument("Editor World binding references a different Scene asset");
        const auto entity = std::find_if(asset.entities().begin(), asset.entities().end(),
            [&](const scene::SceneEntityRecord& candidate) { return candidate.id == binding.scene_entity; });
        const bool has_transform = std::any_of(asset.components().begin(), asset.components().end(),
            [&](const scene::SceneComponentRecord& component) {
                return component.entity == binding.scene_entity && component.component_type == transform_type;
            });
        if (entity == asset.entities().end() || !has_transform)
            return foundation::invalid_argument("Editor World binding references a missing Transform entity");
    }
    return Result::success();
}

[[nodiscard]] bool intersect_ray_plane(const math::Vec3& origin, const math::Vec3& direction,
                                       const math::Vec3& plane_point, const math::Vec3& plane_normal,
                                       math::Vec3& hit) noexcept {
    const float denominator = math::dot(direction, plane_normal);
    if (!math::is_finite(denominator) || std::abs(denominator) <= math::kEpsilon) {
        return false;
    }
    const float distance = math::dot(plane_point - origin, plane_normal) / denominator;
    if (!math::is_finite(distance)) {
        return false;
    }
    hit = origin + direction * distance;
    return math::is_finite(hit);
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
    if (!math::is_finite(local_origin) || !math::is_finite(local_direction) ||
        std::abs(local_direction.z) <= math::kEpsilon) {
        return false;
    }

    const float local_distance = -local_origin.z / local_direction.z;
    if (!math::is_finite(local_distance) || local_distance < 0.0f) {
        return false;
    }
    const math::Vec3 local_hit = local_origin + local_direction * local_distance;
    constexpr float kQuadExtent = 1.0f;
    if (!math::is_finite(local_hit) || local_hit.x < -kQuadExtent || local_hit.x > kQuadExtent ||
        local_hit.y < -kQuadExtent || local_hit.y > kQuadExtent) {
        return false;
    }

    const math::Vec3 world_hit = math::transform_point(world, local_hit);
    distance = math::dot(world_hit - ray_origin, ray_direction);
    return math::is_finite(world_hit) && math::is_finite(distance) && distance >= 0.0f;
}

// Sole viewport readback implementation; declared after legacy composition and Feature projection.
[[nodiscard]] Result build_viewport_readback(render_graph::RenderGraph& graph,
                                            render_graph::RenderGraphTexture color, ViewportReadbackState& state) {
    if (!state.requested) return Result::success();
    if (!state.allocation.is_valid() || state.width == 0 || state.height == 0)
        return foundation::invalid_state("viewport readback was requested without a valid allocation");
    const auto readback = graph.import_buffer(state.allocation.buffer, state.desc, state.initial_state);
    if (!readback) return readback.result;
    const uint64_t pixel_bytes = static_cast<uint64_t>(state.width) * state.height * 4u;
    const auto allocation = state.allocation;
    const auto width = state.width;
    const auto height = state.height;
    state.pass_built = false;
    state.pass_executed = false;
    const auto pass = graph.add_pass(
        "Editor.ViewportReadback", QueueClass::kGraphics,
        [color, readback = readback.value, allocation, pixel_bytes](render_graph::PassBuilder& pass) {
            if (const auto result = pass.read_to(color, ResourceUsage::kCopySource, PipelineStage::kCopy,
                                                ShaderStage::kAll, ResourceUsage::kSampledRead); !result) return result;
            if (const auto result = pass.write(readback, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                                              ShaderStage::kAll, {allocation.offset, pixel_bytes}, true); !result) return result;
            pass.set_side_effects();
            return Result::success();
        },
        [state = &state, color, readback = readback.value, allocation, width, height]
        (render_graph::RenderPassCommandContext& pass) {
            state->pass_executed = true;
            return pass.copy_texture_to_buffer(color, {rhi::Aspect::kColor, 0, 0, 1}, {0, 0, 0},
                                               {width, height, 1}, readback, allocation.offset, 0, 0);
        });
    if (!pass) return pass.result;
    state.pass_built = true;
    return Result::success();
}

Result build_scene_feature_outputs(
    const engine::EngineFramePostBuildContext& context,
    std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> outputs,
    std::span<const feature::RenderResourceOwner> owners, render_graph::RenderFeatureHistory& history,
    render_graph::RenderPassExecutionHost pass_host) {
    if (context.render_scenes.empty())
        return renderer::build_render_feature_outputs(context.render_graph, context.frame_context,
            std::move(outputs), owners, context.feature_textures, {}, history, 0, pass_host);
    for (std::size_t index = 0; index < context.render_scenes.size(); ++index) {
        const auto& scene = context.render_scenes[index];
        if (!scene) continue;
        const bool canonical = index == 0;
        const auto scene_id = canonical ? renderer::kRuntimeSceneResourceId
            : renderer::make_view_scoped_render_resource_id(renderer::kRuntimeSceneResourceId, scene->view.id);
        std::vector<renderer::RenderFeatureSceneBinding> scenes{{scene_id, 1, {}, scene, context.render_resource_resolver}};
        std::vector<render_graph::RenderGraphFeatureTextureBinding> textures(
            context.feature_textures.begin(), context.feature_textures.end());
        if (!canonical) {
            for (const auto& binding : context.feature_textures) {
                const auto alias = renderer::make_view_scoped_render_resource_id(binding.id, scene->view.id);
                if (std::none_of(textures.begin(), textures.end(), [&](const auto& value) { return value.id == alias; }))
                    textures.push_back({alias, binding.texture, binding.version, binding.owner});
            }
        }
        std::vector<feature::RenderFeatureOutput> scoped;
        scoped.reserve(outputs->size());
        for (const auto& output : *outputs)
            scoped.push_back(renderer::scope_render_feature_output(output, scene->view.id, canonical));
        auto snapshot = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(scoped));
        if (const auto result = renderer::build_render_feature_outputs(context.render_graph, context.frame_context,
                std::move(snapshot), owners, textures, scenes, history, scene->view.id.value(), pass_host); !result)
            return result;
    }
    return Result::success();
}

struct ResolvedEditorWorldBinding final {
    feature::FeatureInstanceId instance{};
    ecs::EntityId entity{};
};

foundation::Result validate_editor_world_binding_candidate(
    const EditorFeatureWorldBinding& binding,
    std::span<const feature::FeatureGraphDocument> graphs,
    const feature::FeatureRegistry& registry,
    const scene::SceneAsset& scene_asset,
    const scene::SceneInstance& instance,
    const world::WorldScope& scope) {
    if (binding.graph_asset_uuid.empty() || !binding.node_id.is_valid() || !binding.scene_entity.is_valid() ||
        binding.scene_asset != scene_asset.id() || binding.effect != feature::kWorldTransformCommandEffect ||
        binding.version != feature::kWorldTransformCommandVersion || !scope.is_initialized())
        return foundation::invalid_argument("Editor World binding has an invalid authored identity or protocol");

    try {
        if (const auto result = asset::validate_world_command_route_source(to_world_command_route(binding), graphs,
                registry); !result)
            return result;
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor World binding validation allocation failed");
    }

    const auto entity = instance.runtime_entity(binding.scene_entity);
    if (!entity) return entity.result;
    const auto transform = scope.state().get_component_data(entity.value,
        ecs::ComponentTypeId(engine::components::kTransformName));
    return transform ? Result::success() : transform.result;
}

foundation::ResultValue<ResolvedEditorWorldBinding> resolve_editor_world_binding(
    const EditorFeatureWorldBinding& binding, const runtime::FeatureGraphRuntimeHost& host,
    const scene::SceneAsset& scene_asset, const scene::SceneInstance& instance,
    const world::WorldScope& scope) {
    if (binding.graph_asset_uuid.empty() || !binding.node_id.is_valid() || !binding.scene_entity.is_valid() ||
        binding.scene_asset != scene_asset.id() || binding.effect != feature::kWorldTransformCommandEffect ||
        binding.version != feature::kWorldTransformCommandVersion || !scope.is_initialized())
        return {foundation::invalid_argument("Editor World binding has an invalid authored identity or protocol"), {}};

    const auto execution = host.inspect_execution(binding.graph_asset_uuid);
    if (!execution) return {execution.result, {}};
    const runtime::FeatureRuntimeExecutionProvenance* selected = nullptr;
    for (const auto& candidate : execution.value.provenance) {
        if (candidate.authored_node_id != binding.node_id ||
            candidate.authored_instance_path.nodes != binding.owner_path.nodes) continue;
        if (selected)
            return {foundation::invalid_argument("Editor World binding has ambiguous authored owner attribution"), {}};
        selected = &candidate;
    }
    if (!selected)
        return {foundation::invalid_argument("Editor World binding has no exact published authored owner"), {}};

    const auto node = host.inspect_node(binding.graph_asset_uuid, binding.node_id);
    if (!node) return {node.result, {}};
    const auto owner = std::find_if(node.value.owners.begin(), node.value.owners.end(), [&](const auto& candidate) {
        return candidate.owner.instance_id == selected->runtime.instance_id;
    });
    if (owner == node.value.owners.end())
        return {foundation::invalid_state("Editor World binding owner is absent from the active Feature graph"), {}};
    try {
        if (const auto result = asset::validate_world_command_route_contract(to_world_command_route(binding),
                owner->contract); !result)
            return {result, {}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Editor World binding validation allocation failed"), {}};
    }

    const auto entity = instance.runtime_entity(binding.scene_entity);
    if (!entity) return {entity.result, {}};
    const auto transform = scope.state().get_component_data(entity.value,
        ecs::ComponentTypeId(engine::components::kTransformName));
    if (!transform) return {transform.result, {}};
    return {Result::success(), {selected->runtime.instance_id, entity.value}};
}

foundation::ResultValue<bool> editor_world_binding_has_issued_work(
    const EditorFeatureWorldBinding& binding, const runtime::FeatureGraphRuntimeHost& host,
    const scene::SceneAsset& scene_asset, const scene::SceneInstance& instance,
    const world::WorldScope& scope) {
    const auto resolved = resolve_editor_world_binding(binding, host, scene_asset, instance, scope);
    if (!resolved) return {resolved.result, false};
    const auto node = host.inspect_node(binding.graph_asset_uuid, binding.node_id);
    if (!node) return {node.result, false};
    const auto owner = std::find_if(node.value.owners.begin(), node.value.owners.end(), [&](const auto& candidate) {
        return candidate.owner.instance_id == resolved.value.instance;
    });
    if (owner == node.value.owners.end())
        return {foundation::invalid_state("Editor World binding owner retired before its Apply boundary"), false};
    return {Result::success(), !owner->invocations.open_attempts.empty()};
}

foundation::ResultValue<std::unique_ptr<engine::detail::FeatureWorldCommandConsumer>>
make_editor_world_command_consumer(
    feature::FeatureRuntime& runtime, const reflection::TypeRegistry& schemas,
    const runtime::FeatureGraphRuntimeHost& host, std::span<const EditorFeatureWorldBinding> bindings,
    const scene::SceneAsset& scene_asset, const scene::SceneInstance& instance, world::WorldScope& scope) {
    try {
        auto consumer = std::make_unique<engine::detail::FeatureWorldCommandConsumer>(runtime, schemas);
        for (const auto& binding : bindings) {
            const auto resolved = resolve_editor_world_binding(binding, host, scene_asset, instance, scope);
            if (!resolved) return {resolved.result, {}};
            if (const auto result = consumer->bind(resolved.value.instance, scope, resolved.value.entity,
                    binding.effect, binding.version, binding.observation_port); !result)
                return {result, {}};
        }
        return {Result::success(), std::move(consumer)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Editor World command consumer allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::invalid_state("Editor World command consumer construction threw an exception"), {}};
    }
}

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
        ViewportPoint scale_screen_axis{};
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
        render_graph::RenderGraphTexture graph_texture{};
        render_graph::RenderGraphBuffer graph_parameter{};
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
        render_graph::RenderGraphTexture graph_texture{};
    };

    struct CachedFont final {
        uint64_t source_hash{0};
        asset_pipeline::FontAtlas atlas{};
    };

    struct Resolver final : renderer::RenderResourceResolver {
        explicit Resolver(Impl& owner) noexcept : owner(&owner) {}

        [[nodiscard]] foundation::ResultValue<renderer::RenderMeshResource> resolve_mesh(
            renderer::MeshAssetId mesh) const override {
            if (owner == nullptr) {
                return {foundation::invalid_state("Editor scene resolver is detached"), {}};
            }
            const uint64_t id = mesh.value();
            if (id == kMeshAssetA || id == kMeshAssetB) {
                return {Result::success(), owner->meshes[id == kMeshAssetA ? 0 : 1]};
            }
            const auto imported = owner->imported_meshes.find(id);
            const auto* asset = owner->asset_database.find(id);
            const bool can_use_last_good =
                asset != nullptr && asset->kind == EditorAssetKind::kMesh &&
                (asset->status == EditorAssetStatus::kImported || asset->status == EditorAssetStatus::kFailed);
            if (imported != owner->imported_meshes.end() && imported->second.render.is_valid() &&
                (imported->second.transient || can_use_last_good)) {
                return {Result::success(), imported->second.render};
            }
            owner->diagnostic = asset != nullptr && asset->status == EditorAssetStatus::kImported
                                    ? "Mesh asset is still loading or failed; using editor fallback mesh."
                                : asset != nullptr && asset->status == EditorAssetStatus::kFailed
                                    ? "Mesh reimport failed; using editor fallback mesh."
                                    : "Mesh Asset ID is missing; using editor fallback mesh.";
            return {Result::success(), owner->meshes[0]};
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderMaterialResource> resolve_material(
            renderer::MaterialInstanceId material) const override {
            if (owner == nullptr) {
                return {foundation::invalid_state("Editor scene resolver is detached"), {}};
            }
            const auto resource_for = [&](const MaterialGpu& gpu) {
                renderer::RenderMaterialResource resource{
                    owner->pipeline, gpu.binding_sets[owner->current_slot], 1, gpu.graph_texture,
                    gpu.parameter_buffers[owner->current_slot], gpu.parameter_desc, gpu.graph_parameter, 0,
                    sizeof(resource::TextureMaterialParameters), owner->gbuffer_pipeline, owner->depth_pipeline,
                    owner->shadow_pipeline, owner->clustered_pipeline};
                resource.shader_contract = owner->material_shader_contract;
                resource.attachment_contract = owner->material_attachment_contract;
                resource.shadow_shader_contract = owner->shadow_shader_contract;
                resource.shadow_pipeline_contract = owner->shadow_pipeline_contract;
                resource.shadow_pipeline_cache_epoch = owner->shadow_pipeline.is_valid()
                    ? owner->shadow_shader_contract.cache_epoch : 0;
                resource.clustered_pipeline_contract = owner->clustered_contract;
                resource.clustered_profile_contract = owner->clustered_profile_contract;
                resource.depth_profile_contract = owner->depth_profile_contract;
                resource.depth_pipeline_contract = owner->depth_profile_contract.attachment;
                resource.gbuffer_profile_contract = owner->gbuffer_profile_contract;
                resource.gbuffer_pipeline_contract = owner->gbuffer_profile_contract.attachment;
                resource.variant_cache_epoch = owner->material_shader_contract.cache_epoch;
                if (owner->unshadowed_clustered.pipeline.is_valid())
                    resource.unshadowed_clustered_variant = owner->unshadowed_clustered;
                return resource;
            };
            const uint64_t id = material.value();
            const auto imported = owner->imported_materials.find(id);
            const auto* asset = owner->asset_database.find(id);
            const bool can_use_last_good =
                asset != nullptr && asset->kind == EditorAssetKind::kMaterial &&
                (asset->status == EditorAssetStatus::kImported || asset->status == EditorAssetStatus::kFailed);
            if (imported != owner->imported_materials.end() && (imported->second.transient || can_use_last_good)) {
                const MaterialGpu& gpu = imported->second;
                return {Result::success(), resource_for(gpu)};
            }
            const uint32_t index = id == kMaterialAssetGreen ? 1u : 0u;
            if (id != kMaterialAssetRed && id != kMaterialAssetGreen) {
                owner->diagnostic =
                    asset != nullptr && asset->status == EditorAssetStatus::kImported
                        ? "Imported material metadata is available; editor preview uses the fallback material."
                    : asset != nullptr && asset->status == EditorAssetStatus::kFailed
                        ? "Material reimport failed; using editor fallback material."
                        : "Material Asset ID is missing; using editor fallback material.";
            }
            const MaterialGpu& gpu = owner->materials[index];
            return {Result::success(), resource_for(gpu)};
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderObjectResource> resolve_object(
            const renderer::RenderInstance& instance, const renderer::RenderView& view) const override {
            if (owner == nullptr) {
                return {foundation::invalid_state("Editor scene resolver is detached"), {}};
            }
            const auto key = std::pair{view.id.value(), instance.object.value()};
            const auto found = owner->frame_object_slots.find(key);
            auto& objects = owner->object_resources[owner->frame_context->frame_slot_index()];
            if (found != owner->frame_object_slots.end())
                return {Result::success(), objects[found->second]};
            const size_t object_slot = owner->frame_object_slots.size();
            if (object_slot == objects.size()) {
                const auto desc = make_uniform_desc(sizeof(renderer::RenderObjectConstants), "Editor.Object.Buffer");
                const auto buffer = owner->device->create_buffer(desc);
                if (!buffer)
                    return {buffer.result, {}};
                const auto set =
                    owner->device->create_binding_set({owner->object_layout, BindingSetLifetime::kPersistent});
                if (!set) {
                    (void)owner->device->destroy_buffer(buffer.value);
                    return {set.result, {}};
                }
                BindingWrite write{};
                write.location = {0, 0};
                write.resource.type = BindingType::kUniformBuffer;
                write.resource.buffer_range = {buffer.value, 0, sizeof(renderer::RenderObjectConstants)};
                if (const auto r = owner->device->update_binding_set(set.value, std::span{&write, 1}); !r) {
                    (void)owner->device->destroy_binding_set(set.value);
                    (void)owner->device->destroy_buffer(buffer.value);
                    return {r, {}};
                }
                try {
                    objects.push_back({buffer.value, desc, owner->object_layout, set.value, 0, 0,
                                       sizeof(renderer::RenderObjectConstants)});
                } catch (const std::bad_alloc&) {
                    (void)owner->device->destroy_binding_set(set.value);
                    (void)owner->device->destroy_buffer(buffer.value);
                    return {foundation::out_of_memory("Editor object resource pool allocation failed"), {}};
                }
            }
            math::Mat4 inverse_model_view{};
            if (!math::try_inverse(view.camera.view * instance.world_transform, inverse_model_view))
                return {foundation::invalid_argument("Object model-view transform is not invertible"), {}};
            renderer::RenderObjectConstants constants{};
            constants.model_view_projection = view.camera.view_projection() * instance.world_transform;
            for (size_t row = 0; row < 4; ++row)
                for (size_t column = 0; column < 4; ++column)
                    constants.normal_matrix.at(row, column) = inverse_model_view.at(column, row);
            const BufferHandle buffer = objects[object_slot].transform_buffer;
            if (const Result result = owner->device->write_buffer(buffer, 0, std::as_bytes(std::span{&constants, 1}));
                !result) {
                return {result, {}};
            }
            try {
                owner->frame_object_slots.emplace(key, object_slot);
            } catch (const std::bad_alloc&) {
                return {foundation::out_of_memory("Editor object view cache allocation failed"), {}};
            }
            return {Result::success(), objects[object_slot]};
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderLightingResource> resolve_lighting(
            const renderer::RenderSceneSnapshot& snapshot, const renderer::RenderView& view) const override {
            if (owner == nullptr) {
                return {foundation::invalid_state("Editor scene resolver is detached"), {}};
            }
            auto constants = renderer::make_render_lighting_constants(snapshot.lights);
            if (!constants) {
                return {constants.result, {}};
            }
            const uint32_t frame_slot = owner->frame_context->frame_slot_index();
            auto& direction = constants.value.directional_direction_intensity;
            math::Vec3 view_direction{};
            if (!math::try_normalize(math::transform_vector(view.camera.view, {direction.x, direction.y, direction.z}),
                                     view_direction))
                return {foundation::invalid_argument("Light direction cannot be transformed into view space"), {}};
            direction = {view_direction.x, view_direction.y, view_direction.z, direction.w};
            if (const auto result = owner->device->write_buffer(owner->lighting_buffers[frame_slot], 0,
                                                                std::as_bytes(std::span{&constants.value, 1}));
                !result)
                return {result, {}};
            return {Result::success(), renderer::RenderLightingResource{owner->lighting_sets[frame_slot], 2,
                                                                        owner->lighting_buffers[frame_slot],
                                                                        owner->lighting_desc, owner->lighting_graph, 0,
                                                                        sizeof(renderer::RenderLightingConstants)}};
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderDeferredLightingResource> resolve_deferred_lighting(
            const renderer::RenderView&) const override {
            if (owner == nullptr || !owner->deferred_lighting || !owner->frame_context) {
                return {foundation::invalid_state("Editor deferred lighting resources are unavailable"), {}};
            }
            return owner->deferred_lighting->for_frame(*owner->frame_context);
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderShadowResource> resolve_shadow(
            const renderer::RenderShadowView& shadow) const override {
            if (!owner || !owner->shadows || !owner->frame_context)
                return {foundation::invalid_state("Editor shadow resources are unavailable"), {}};
            return owner->shadows->for_frame(*owner->frame_context, shadow.constants);
        }

        [[nodiscard]] foundation::ResultValue<renderer::RenderLightCullingResource> resolve_light_culling(
            const renderer::RenderSceneSnapshot& scene, const renderer::RenderShadowView& shadow) const override {
            if (!owner || !owner->light_culling || !owner->frame_context)
                return {foundation::invalid_state("Editor light culling resources are unavailable"), {}};
            return owner->light_culling->for_frame(*owner->frame_context, scene, shadow);
        }
        [[nodiscard]] foundation::ResultValue<renderer::RenderPostProcessResource> resolve_post_process(
            const renderer::RenderView& view) const override {
            if (!owner || !owner->post_process || !owner->frame_context)
                return {foundation::invalid_state("Editor postprocess resources are unavailable"), {}};
            return owner->post_process->for_frame(*owner->frame_context, view, view.post_process);
        }

        Impl* owner{nullptr};
    };

    // Application ownership only: execution, registration and DLL leases retain their existing implementations.
    struct FeatureSession final {
        FeatureSession(Device& device, const std::filesystem::path& project_file,
                       const feature::ClockHost& clock_host,
                       const feature::FeatureRuntime* previous)
            : history(device), registry(clock_host), asset_kind_resolver(snapshot_editor_asset_kind_resolver(project_file)),
              execution(previous ? previous->make_reset_runtime(registry, asset_kind_resolver)
                                 : std::make_unique<feature::FeatureRuntime>(registry, asset_kind_resolver)) {}
        render_graph::RenderFeatureHistory history;
        feature::FeatureRegistry registry{};
        module::ModuleRegistry modules{registry};
        module::DynamicModuleLoader loader{modules};
        module::DynamicModuleHandle handle{};
        // Published session authority; authoring refreshes do not replace this immutable directory.
        feature::AssetKindResolver asset_kind_resolver{};
        std::unique_ptr<feature::FeatureRuntime> execution{
            std::make_unique<feature::FeatureRuntime>(registry, asset_kind_resolver)};
        std::unique_ptr<runtime::FeatureGraphRuntimeHost> host{
            std::make_unique<runtime::FeatureGraphRuntimeHost>(*execution, registry)};

        ~FeatureSession() { static_cast<void>(close()); }
        [[nodiscard]] Result close() {
            if (execution != nullptr) {
                if (const auto result = execution->shutdown_execution(); !result) return result;
                history.clear();
                host.reset();
                execution.reset();
            }
            if (handle.loaded) {
                const auto id = handle.bundle.metadata.module.id;
                if (modules.contains(id)) {
                    if (const auto result = modules.unregister_module(id); !result) return result;
                }
                if (const auto result = loader.unload(handle); !result) return result;
            }
            return Result::success();
        }
        [[nodiscard]] Result reset_execution() {
            auto next_execution = execution->make_reset_runtime();
            auto next_host = std::make_unique<runtime::FeatureGraphRuntimeHost>(*next_execution, registry);
            if (const auto result = next_execution->enable_async_execution(); !result) return result;
            for (const auto& graph : host->active_documents()) {
                if (const auto result = next_host->stage(graph); !result) return result;
            }
            if (next_host->has_pending()) {
                if (const auto result = next_host->apply_pending(); !result) return result;
            }
            if (const auto result = execution->shutdown_execution(); !result) return result;
            history.clear();
            host = std::move(next_host);
            execution = std::move(next_execution);
            return Result::success();
        }
    };
    feature::ClockHost clock_host{};
    std::unique_ptr<FeatureSession> feature_session{};
    std::unique_ptr<FeatureSession> pending_feature_session{};
    std::vector<EditorFeatureWorldBinding> feature_world_bindings{};
    std::unique_ptr<engine::detail::FeatureWorldCommandConsumer> world_command_consumer{};
    std::unique_ptr<EditorRendererFeatureHost> preview_renderer{};
    std::unique_ptr<render_graph::RenderFeatureHistory> preview_history{};
    [[nodiscard]] Result close_features() {
        if (world_command_consumer != nullptr) {
            // The orchestrator borrows the consumer, so retire its attachment before the route leases.
            orchestrator.reset();
            world_command_consumer.reset();
        }
        if (pending_feature_session != nullptr) {
            if (const auto result = pending_feature_session->close(); !result) return result;
            pending_feature_session.reset();
        }
        if (feature_session != nullptr) {
            if (const auto result = feature_session->close(); !result) return result;
            feature_session.reset();
        }
        return Result::success();
    }

    Device* device{nullptr};
    reflection::TypeRegistry reflection_registry{};
    pipeline::PipelineRegistry registry{};
    pipeline::PipelineScheduler scheduler{};
    pipeline::RenderPassRegistry scene_passes{};
    engine::EngineFramePipelineHost pipeline_host{registry, scheduler};
    std::unique_ptr<render_graph::FrameContext> frame_context{};
    std::unique_ptr<render_graph::RenderGraph> render_graph{};
    std::shared_ptr<const EditorRenderGraphSnapshot> rendered_inspection{};
    renderer::Renderer renderer{};
    std::unique_ptr<engine::EngineFrameOrchestrator> orchestrator{};
    ecs::WorldManager world_manager{};
    std::shared_ptr<ecs::World> edit_state{};
    std::shared_ptr<ecs::World> runtime_state{};
    std::unique_ptr<world::WorldScope> edit_scope{};
    std::unique_ptr<world::WorldScope> runtime_scope{};
    world::WorldScope* active_scope{nullptr};
    scene::SceneAsset scene_asset{};
    EditorProjectManager project_manager{};
    EditorProjectFeatureAuthoring project_feature_authoring{};
    EditorAssetDatabase asset_database{};
    asset_pipeline::EditorAssetPipeline asset_pipeline{};
    bool asset_pipeline_ready{false};
    scene::SceneInstance edit_instance{};
    scene::SceneInstance runtime_instance{};
    scene::SceneEntityId selected_entity{kLeftMeshEntity};
    std::vector<scene::SceneEntityId> selected_entities{kLeftMeshEntity};
    scene::SceneEntityId active_camera_scene{kCameraEntity};
    ecs::EntityId edit_camera_runtime{};
    ecs::EntityId runtime_camera_runtime{};
    std::filesystem::path scene_path{"editor_scene.aegscene"};
    std::string diagnostic{};
    bool dirty{false};
    EditorSceneRuntime::Mode mode{EditorSceneRuntime::Mode::kEdit};
    uint32_t viewport_width{640};
    uint32_t viewport_height{360};
    uint32_t requested_width{640};
    uint32_t requested_height{360};
    bool target_resize_pending{true};
    bool initialized{false};
    bool materials_ready{false};
    bool target_has_previous_frame{false};
    uint32_t current_slot{0};
    EditTransaction transform_edit{};
    EditorCommand component_edit_command{};
    bool component_edit_active{false};
    std::vector<EditorCommand> history{};
    std::size_t history_cursor{0};
    std::size_t clean_history_cursor{0};
    bool scene_dirty_base{false};

    shader::ShaderManager* shader_manager{nullptr};
    std::unique_ptr<shader::ShaderManager> shader_manager_owner{};
    shader::ShaderPipelineLayout shader_layout{};
    shader::ShaderPipelineLayout depth_shader_layout{};
    shader::ShaderPipelineLayout gbuffer_shader_layout{};
    shader::ShaderPipelineLayout shadow_shader_layout{};
    BindingLayoutHandle object_layout{};
    BindingLayoutHandle material_layout{};
    BindingLayoutHandle lighting_layout{};
    rhi::PipelineHandle pipeline{};
    renderer::RenderMaterialShaderContract material_shader_contract{};
    renderer::RenderMaterialAttachmentContract material_attachment_contract{};
    rhi::PipelineHandle gbuffer_pipeline{};
    renderer::RendererProfilePsoContract gbuffer_profile_contract{};
    rhi::PipelineHandle depth_pipeline{};
    renderer::RendererProfilePsoContract depth_profile_contract{};
    rhi::PipelineHandle shadow_pipeline{};
    renderer::RenderMaterialShaderContract shadow_shader_contract{};
    renderer::RenderMaterialAttachmentContract shadow_pipeline_contract{};
    rhi::PipelineHandle clustered_pipeline{};
    renderer::RenderMaterialAttachmentContract clustered_contract{};
    renderer::RendererProfilePsoContract clustered_profile_contract{};
    renderer::RenderMaterialPipelineVariant unshadowed_clustered{};
    std::unique_ptr<renderer::LightCullingResources> light_culling{};
    std::unique_ptr<renderer::PostProcessResources> post_process{};
    std::unique_ptr<renderer::ShadowResources> shadows{};
    std::unique_ptr<renderer::DeferredLightingResources> deferred_lighting{};
    /** State-composite descriptors are valid only until their frame has been submitted. */
    std::array<renderer::RenderMeshResource, 2> meshes{};
    std::array<BufferHandle, 2> mesh_vertex_buffers{};
    std::array<BufferDesc, 2> mesh_vertex_descs{};
    BufferHandle index_buffer{};
    BufferDesc index_buffer_desc{};
    std::array<std::vector<renderer::RenderObjectResource>, kFramesInFlight> object_resources{};
    std::map<std::pair<uint64_t, uint64_t>, size_t> frame_object_slots{};
    std::array<BufferHandle, kFramesInFlight> lighting_buffers{};
    std::array<BindingSetHandle, kFramesInFlight> lighting_sets{};
    BufferDesc lighting_desc{};
    render_graph::RenderGraphBuffer lighting_graph{};
    std::array<MaterialGpu, kMaterialCount> materials{};
    std::unordered_map<uint64_t, ImportedMeshGpu> imported_meshes{};
    std::unordered_map<uint64_t, ImportedTextureGpu> imported_textures{};
    std::unordered_map<uint64_t, MaterialGpu> imported_materials{};
    std::unordered_map<uint64_t, render_graph::RenderGraphTexture> frame_texture_graphs{};
    std::unordered_map<uint64_t, render_graph::RenderGraphBuffer> frame_vertex_graphs{};
    std::unordered_map<uint64_t, render_graph::RenderGraphBuffer> frame_index_graphs{};
    std::unordered_map<uint64_t, CachedFont> cached_fonts{};
    ViewportReadbackState viewport_readback{};
    TextureHandle color_texture{};
    TextureViewHandle color_view{};
    TextureViewHandle color_sampled_view{};
    /** Exact RHI state exported to the editor UI after the latest scene frame. */
    ResourceState viewport_export_state{sampled_state()};
    TextureDesc color_desc{};
    TextureHandle depth_texture{};
    TextureViewHandle depth_view{};
    TextureDesc depth_desc{};
    rhi::SamplerHandle viewport_sampler{};
    std::unique_ptr<resource::TextureUploader> texture_uploader{};
    std::unique_ptr<resource::ResourceUploader> mesh_uploader{};
    std::filesystem::path shader_package_path{};
    Resolver resolver;

    explicit Impl() : resolver(*this) {}

    [[nodiscard]] Result initialize_device(Device& device);
    [[nodiscard]] Result initialize_graphics_resources();
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
    [[nodiscard]] Result reload_project_feature_authoring();
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
    return impl_ == nullptr ? scene::SceneEntityId{} : impl_->selected_entity;
}

std::span<const scene::SceneEntityId> EditorSceneRuntime::selected_entities() const noexcept {
    return impl_ == nullptr ? std::span<const scene::SceneEntityId>{} : impl_->selected_entities;
}

bool EditorSceneRuntime::is_selected(scene::SceneEntityId entity) const noexcept {
    if (impl_ == nullptr || !entity.is_valid()) {
        return false;
    }
    return std::find(impl_->selected_entities.begin(), impl_->selected_entities.end(), entity) !=
           impl_->selected_entities.end();
}

std::string_view EditorSceneRuntime::scene_name() const noexcept {
    return impl_ == nullptr ? std::string_view{} : impl_->scene_asset.name();
}

const std::filesystem::path& EditorSceneRuntime::scene_path() const noexcept {
    static const std::filesystem::path kEmptyPath{};
    return impl_ == nullptr ? kEmptyPath : impl_->scene_path;
}

bool EditorSceneRuntime::dirty() const noexcept {
    return impl_ != nullptr && impl_->dirty;
}

EditorSceneRuntime::Mode EditorSceneRuntime::mode() const noexcept {
    return impl_ == nullptr ? Mode::kEdit : impl_->mode;
}

std::string_view EditorSceneRuntime::diagnostic() const noexcept {
    if (impl_ == nullptr)
        return {};
    return impl_->diagnostic;
}

bool EditorSceneRuntime::project_loaded() const noexcept {
    return impl_ != nullptr && impl_->project_manager.loaded();
}

uint32_t EditorSceneRuntime::draw_call_count() const noexcept {
    if (impl_ == nullptr || impl_->orchestrator == nullptr) {
        return 0;
    }
    return impl_->orchestrator->draw_call_count();
}

std::string_view EditorSceneRuntime::project_name() const noexcept {
    return impl_ == nullptr ? std::string_view{} : impl_->project_manager.descriptor().name;
}

const std::filesystem::path& EditorSceneRuntime::project_root() const noexcept {
    static const std::filesystem::path kEmptyPath{};
    return impl_ == nullptr ? kEmptyPath : impl_->project_manager.descriptor().root;
}

std::filesystem::path EditorSceneRuntime::project_file() const {
    return impl_ == nullptr ? std::filesystem::path{} : impl_->project_manager.project_file();
}

foundation::ResultValue<renderer::RenderView> EditorSceneRuntime::Impl::make_edit_view(uint32_t width,
                                                                                       uint32_t height) const {
    if (edit_scope == nullptr || !edit_camera_runtime.is_valid()) {
        return {foundation::invalid_state("Editor edit World is not initialized"), {}};
    }
    if (width == 0 || height == 0) {
        return {foundation::invalid_argument("Viewport extent must be positive"), {}};
    }
    const auto snapshot = edit_scope->latest_snapshot();
    if (!snapshot) {
        return {snapshot.result, {}};
    }
    return aeg::engine::components::make_render_view(
        *snapshot.value, aeg::engine::components::RuntimeRenderViewDesc{
                             edit_camera_runtime, 0, {width, height}, 0xffffffffu, {}, {}, false, {}});
}

foundation::ResultValue<EditorSceneRuntime::Impl::ViewportRay> EditorSceneRuntime::Impl::make_viewport_ray(
    ViewportPoint pixel, uint32_t width, uint32_t height) const {
    const auto view = make_edit_view(width, height);
    if (!view) {
        return {view.result, {}};
    }
    const auto snapshot = edit_scope->latest_snapshot();
    if (!snapshot) {
        return {snapshot.result, {}};
    }
    const Transform* transform = snapshot.value->get<Transform>(edit_camera_runtime);
    const Camera* parameters = snapshot.value->get<Camera>(edit_camera_runtime);
    if (transform == nullptr || parameters == nullptr) {
        return {foundation::not_found("Editor viewport camera components are missing"), {}};
    }
    if (!math::is_finite(pixel.x) || !math::is_finite(pixel.y) || pixel.x < 0.0f || pixel.y < 0.0f ||
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
    dirty =
        scene_dirty_base || history_cursor != clean_history_cursor || transform_edit.active || component_edit_active;
}

void EditorSceneRuntime::Impl::reset_history(bool scene_dirty) {
    history.clear();
    history_cursor = 0;
    clean_history_cursor = 0;
    scene_dirty_base = scene_dirty;
    refresh_dirty();
}

foundation::Result EditorSceneRuntime::Impl::enqueue_values(const std::vector<EditorComponentValue>& values) {
    if (edit_scope == nullptr || edit_state == nullptr) {
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
            const auto runtime = edit_instance.runtime_entity(value.entity);
            if (!runtime) {
                return runtime.result;
            }
            const auto descriptor = edit_state->find_component_descriptor(value.component);
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
            edit_scope->command_buffer().enqueue([queued = std::move(queued)](ecs::World& world) mutable {
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
        if (history_cursor < history.size()) {
            history.erase(history.begin() + static_cast<std::ptrdiff_t>(history_cursor), history.end());
        }
        history.push_back(std::move(command));
        history_cursor = history.size();
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
            scene_asset = *snapshot;
        } catch (const std::bad_alloc&) {
            return foundation::out_of_memory("Editor history Scene Asset allocation failed");
        }
        if (command.rebuild_edit_world) {
            if (const Result result = rebuild_edit_world(); !result) {
                return result;
            }
        }
        selected_entities.erase(
            std::remove_if(selected_entities.begin(), selected_entities.end(),
                           [this](scene::SceneEntityId entity) {
                               return std::find_if(scene_asset.entities().begin(), scene_asset.entities().end(),
                                                   [entity](const auto& record) { return record.id == entity; }) ==
                                      scene_asset.entities().end();
                           }),
            selected_entities.end());
        if (std::find(selected_entities.begin(), selected_entities.end(), selected_entity) == selected_entities.end()) {
            selected_entity = selected_entities.empty() ? scene::SceneEntityId{} : selected_entities.front();
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
    return impl_ == nullptr ? kEmptyRegistry : impl_->reflection_registry;
}

std::span<const EditorProjectFeatureGraphDocument> EditorSceneRuntime::authored_feature_graphs() const noexcept {
    return impl_ == nullptr
               ? std::span<const EditorProjectFeatureGraphDocument>{}
               : std::span<const EditorProjectFeatureGraphDocument>{impl_->project_feature_authoring.feature_graphs};
}

const feature::FeatureCatalog& EditorSceneRuntime::authored_feature_catalog() const noexcept {
    static const feature::FeatureCatalog kEmptyCatalog{};
    return impl_ == nullptr ? kEmptyCatalog : impl_->project_feature_authoring.feature_catalog;
}

const feature::FeatureCatalog* EditorSceneRuntime::active_feature_module_catalog() const noexcept {
    if (impl_ == nullptr || impl_->feature_session == nullptr || !impl_->feature_session->handle.loaded)
        return nullptr;
    return &impl_->feature_session->handle.bundle.metadata.feature_catalog;
}

foundation::ResultValue<feature::FeatureCatalog> EditorSceneRuntime::inspect_feature_module_catalog(
    const std::filesystem::path& manifest) const {
    const auto bundle = module::validate_module_bundle(manifest);
    if (!bundle) return {bundle.result, {}};
    return {foundation::Result::success(), bundle.value.metadata.feature_catalog};
}

std::span<const feature::FeatureDefinitionId> EditorSceneRuntime::authored_compiled_features() const noexcept {
    return impl_ == nullptr ? std::span<const feature::FeatureDefinitionId>{}
        : std::span<const feature::FeatureDefinitionId>{impl_->project_feature_authoring.compiled_feature_ids};
}

std::span<const NodeContractFile> EditorSceneRuntime::authored_feature_drafts() const noexcept {
    return impl_ == nullptr ? std::span<const NodeContractFile>{}
                            : std::span<const NodeContractFile>{impl_->project_feature_authoring.drafts};
}

std::span<const NodeDefinitionDraft> EditorSceneRuntime::authored_feature_presentations() const noexcept {
    return impl_ == nullptr ? std::span<const NodeDefinitionDraft>{}
                            : std::span<const NodeDefinitionDraft>{impl_->project_feature_authoring.presentations};
}

std::span<const EditorProjectFeatureConflict> EditorSceneRuntime::authored_feature_conflicts() const noexcept {
    return impl_ == nullptr ? std::span<const EditorProjectFeatureConflict>{}
                            : std::span<const EditorProjectFeatureConflict>{impl_->project_feature_authoring.conflicts};
}

foundation::Result EditorSceneRuntime::restore_default_renderer_feature() {
    if (impl_ == nullptr || !impl_->initialized || impl_->device == nullptr || impl_->preview_renderer == nullptr)
        return foundation::invalid_state("Editor Renderer Feature preview is unavailable");
    if (impl_->mode != Mode::kEdit || impl_->transform_edit.active || impl_->component_edit_active ||
        impl_->frame_context->is_frame_active())
        return foundation::invalid_state("Renderer Feature restoration requires an Edit frame boundary");
    auto candidate = EditorRendererFeatureHost::create();
    if (!candidate) return candidate.result;
    std::unique_ptr<render_graph::RenderFeatureHistory> history;
    try {
        history = std::make_unique<render_graph::RenderFeatureHistory>(*impl_->device);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor Renderer Feature history allocation failed");
    }
    impl_->preview_renderer.swap(candidate.value);
    impl_->preview_history.swap(history);
    return Result::success();
}

foundation::Result EditorSceneRuntime::initialize(rhi::Device& device,
                                                  const std::filesystem::path& shader_package_path) {
    if (impl_ != nullptr && impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is already initialized");
    }
    try {
        impl_ = std::make_unique<Impl>();
        impl_->shader_package_path = shader_package_path;
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor scene runtime allocation failed");
    }
    return impl_->initialize_device(device);
}

foundation::Result EditorSceneRuntime::Impl::initialize_device(Device& device) {
    this->device = &device;
    // Reconstruct the device-borrowing members only after the actual Device exists.
    frame_context =
        std::make_unique<render_graph::FrameContext>(device, render_graph::FrameContextDesc{kFramesInFlight});
    render_graph = std::make_unique<render_graph::RenderGraph>(device);
    mesh_uploader = std::make_unique<resource::ResourceUploader>(device);
    texture_uploader = std::make_unique<resource::TextureUploader>(device);
    if (const auto result = renderer::register_builtin_render_passes(scene_passes); !result)
        return result;

    if (const Result result = aeg::engine::components::register_render_component_reflection(reflection_registry);
        !result) {
        return result;
    }
    if (const auto extractor = aeg::engine::components::register_mesh_renderer_extractor(renderer); !extractor) {
        return extractor.result;
    }
    if (const auto extractor = aeg::engine::components::register_text_renderer_extractor(renderer); !extractor) {
        return extractor.result;
    }
    if (const auto extractor = aeg::engine::components::register_directional_light_extractor(renderer); !extractor) {
        return extractor.result;
    }
    if (const auto extractor = aeg::engine::components::register_local_light_extractor(renderer); !extractor)
        return extractor.result;
    if (const Result result = initialize_graphics_resources(); !result) {
        return result;
    }
    auto preview = EditorRendererFeatureHost::create();
    if (!preview) return preview.result;
    preview_renderer = std::move(preview.value);
    preview_history = std::make_unique<render_graph::RenderFeatureHistory>(device);
    if (const Result result = make_default_scene(); !result) {
        return result;
    }
    if (const Result result = rebuild_edit_world(); !result) {
        return result;
    }
    reset_history(true);
    initialized = true;
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::initialize_graphics_resources() {
    std::filesystem::path package_path = shader_package_path;
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
    shader_manager_owner = std::make_unique<shader::ShaderManager>(*device);
    shader_manager = shader_manager_owner.get();
    auto layout = shader_manager->build_pipeline_layout(program.value, DebugName{"Editor.Opaque.Layout"});
    if (!layout) {
        return layout.result;
    }
    shader_layout = std::move(layout.value);
    auto object_layout = shader_manager->binding_layout(shader_layout, 0);
    auto material_layout = shader_manager->binding_layout(shader_layout, 1);
    auto lighting_layout = shader_manager->binding_layout(shader_layout, 2);
    if (!object_layout || !material_layout || !lighting_layout) {
        return foundation::invalid_state("Editor opaque binding layouts are incomplete");
    }
    this->object_layout = object_layout.value;
    this->material_layout = material_layout.value;
    this->lighting_layout = lighting_layout.value;
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
    auto pipeline = shader_manager->create_pipeline(
        program.value, shader_layout,
        shader::ShaderPipelineDesc{PipelineType::kGraphics, graphics, DebugName{"Editor.Opaque.Pipeline"}});
    if (!pipeline) {
        return pipeline.result;
    }
    this->pipeline = pipeline.value;
    const auto material_identity = shader_manager->pipeline_layout_identity(shader_layout);
    if (!material_identity)
        return material_identity.result;
    material_shader_contract = {
        renderer::kRenderMaterialShaderContractVersion,
        material_identity.value.contract_hash, material_identity.value.cache_epoch};
    material_attachment_contract = {graphics.color_formats.front(), graphics.samples, graphics.depth_stencil_format};

#ifdef AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH
    const auto shadow_program = shader::load_shader_program(AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH);
    if (!shadow_program)
        return shadow_program.result;
    const auto shadow_layout = shader_manager->build_pipeline_layout(shadow_program.value);
    if (!shadow_layout)
        return shadow_layout.result;
    const auto shadow_pipeline =
        shader_manager->create_pipeline(shadow_program.value, shadow_layout.value,
                                        {PipelineType::kGraphics, graphics, DebugName{"Editor.ForwardShadow"}});
    if (!shadow_pipeline)
        return shadow_pipeline.result;
    this->shadow_pipeline = shadow_pipeline.value;
    shadow_shader_layout = shadow_layout.value;
    const auto shadow_identity = shader_manager->pipeline_layout_identity(shadow_shader_layout);
    if (!shadow_identity)
        return shadow_identity.result;
    shadow_shader_contract = {
        renderer::kRenderMaterialShaderContractVersion,
        shadow_identity.value.contract_hash, shadow_identity.value.cache_epoch};
    shadow_pipeline_contract = {graphics.color_formats.front(), graphics.samples, graphics.depth_stencil_format};
    const auto shadow_inputs = shader_manager->binding_layout(shadow_layout.value, 3);
    if (!shadow_inputs)
        return shadow_inputs.result;
    shadows = std::make_unique<renderer::ShadowResources>();
    if (const auto r = shadows->initialize(*device, shadow_inputs.value); !r)
        return r;

    const auto variants = renderer::create_clustered_forward_pipelines(*device, *shader_manager, graphics,
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH, AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH);
    if (!variants) return variants.result;
    clustered_pipeline = variants.value.shadowed.pipeline;
    clustered_contract = variants.value.shadowed.attachment_contract;
    clustered_profile_contract = variants.value.shadowed.profile_contract;
    unshadowed_clustered = variants.value.unshadowed;
    light_culling = std::make_unique<renderer::LightCullingResources>();
    if (const auto r =
            light_culling->initialize(*device, AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH, variants.value.shadowed_inputs,
                                     variants.value.unshadowed_inputs);
        !r)
        return r;
#endif

#ifdef AEG_DEPTH_MESH_SHADER_PACKAGE_PATH
    const auto depth_program = shader::load_shader_program(AEG_DEPTH_MESH_SHADER_PACKAGE_PATH);
    if (!depth_program)
        return depth_program.result;
    const auto depth_layout =
        shader_manager->build_pipeline_layout(depth_program.value, DebugName{"Editor.Depth.Layout"});
    if (!depth_layout)
        return depth_layout.result;
    GraphicsPipelineState depth_graphics{};
    depth_graphics.vertex_bindings = bindings;
    depth_graphics.vertex_attributes = std::span<const VertexAttributeDesc>{attributes.data(), 1};
    depth_graphics.cull_mode = CullMode::kNone;
    depth_graphics.depth_test_enable = true;
    depth_graphics.depth_write_enable = true;
    depth_graphics.depth_stencil_format = Format::kD32Float;
    const auto depth_pipeline =
        shader_manager->create_pipeline(depth_program.value, depth_layout.value,
                                        {PipelineType::kGraphics, depth_graphics, DebugName{"Editor.Depth.Pipeline"}});
    if (!depth_pipeline)
        return depth_pipeline.result;
    this->depth_pipeline = depth_pipeline.value;
    depth_shader_layout = depth_layout.value;
    const auto depth_identity = shader_manager->pipeline_layout_identity(depth_shader_layout);
    if (!depth_identity)
        return depth_identity.result;
    const auto depth_epoch = depth_identity.value.cache_epoch;
    depth_profile_contract = {
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kShadow,
         depth_identity.value.contract_hash, depth_epoch},
        {Format::kUnknown, depth_graphics.samples, depth_graphics.depth_stencil_format}, depth_epoch};
#endif

#ifdef AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH
    auto gbuffer_program = shader::load_shader_program(AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH);
    if (!gbuffer_program) {
        return gbuffer_program.result;
    }
    auto gbuffer_layout =
        shader_manager->build_pipeline_layout(gbuffer_program.value, DebugName{"Editor.GBuffer.Layout"});
    if (!gbuffer_layout) {
        return gbuffer_layout.result;
    }
    gbuffer_shader_layout = std::move(gbuffer_layout.value);
    const std::array<Format, 3> gbuffer_color_formats{
        {Format::kRgba16Float, Format::kRgba16Float, Format::kRgba8Unorm}};
    const std::array<rhi::BlendAttachmentDesc, 3> gbuffer_blend_attachments{{
        {false, rhi::BlendFactor::kOne, rhi::BlendFactor::kZero, rhi::BlendOp::kAdd, rhi::BlendFactor::kOne,
         rhi::BlendFactor::kZero, rhi::BlendOp::kAdd, rhi::ColorComponent::kAll},
        {false, rhi::BlendFactor::kOne, rhi::BlendFactor::kZero, rhi::BlendOp::kAdd, rhi::BlendFactor::kOne,
         rhi::BlendFactor::kZero, rhi::BlendOp::kAdd, rhi::ColorComponent::kAll},
        {false, rhi::BlendFactor::kOne, rhi::BlendFactor::kZero, rhi::BlendOp::kAdd, rhi::BlendFactor::kOne,
         rhi::BlendFactor::kZero, rhi::BlendOp::kAdd, rhi::ColorComponent::kAll},
    }};
    GraphicsPipelineState gbuffer_graphics{};
    gbuffer_graphics.vertex_bindings = bindings;
    gbuffer_graphics.vertex_attributes = attributes;
    gbuffer_graphics.topology = PrimitiveTopology::kTriangleList;
    gbuffer_graphics.cull_mode = CullMode::kNone;
    gbuffer_graphics.depth_test_enable = true;
    gbuffer_graphics.depth_write_enable = true;
    gbuffer_graphics.color_formats = gbuffer_color_formats;
    gbuffer_graphics.depth_stencil_format = Format::kD32Float;
    gbuffer_graphics.blend_attachments = gbuffer_blend_attachments;
    auto gbuffer_pipeline = shader_manager->create_pipeline(
        gbuffer_program.value, gbuffer_shader_layout,
        shader::ShaderPipelineDesc{PipelineType::kGraphics, gbuffer_graphics, DebugName{"Editor.GBuffer.Pipeline"}});
    if (!gbuffer_pipeline) {
        return gbuffer_pipeline.result;
    }
    this->gbuffer_pipeline = gbuffer_pipeline.value;
    const auto gbuffer_identity = shader_manager->pipeline_layout_identity(gbuffer_shader_layout);
    if (!gbuffer_identity)
        return gbuffer_identity.result;
    const auto gbuffer_epoch = gbuffer_identity.value.cache_epoch;
    gbuffer_profile_contract = {
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kGBuffer,
         gbuffer_identity.value.contract_hash, gbuffer_epoch},
        {gbuffer_graphics.color_formats[0], gbuffer_graphics.samples, gbuffer_graphics.depth_stencil_format,
         {gbuffer_graphics.color_formats[1], gbuffer_graphics.color_formats[2]}}, gbuffer_epoch};
#endif

#ifdef AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH
    deferred_lighting = std::make_unique<renderer::DeferredLightingResources>();
    if (const auto result =
            deferred_lighting->initialize(*device, AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH,
                                          renderer::kSceneLightingFormat, AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH);
        !result)
        return result;
#endif

#ifdef AEG_POST_PROCESS_SHADER_PACKAGE_PATH
    post_process = std::make_unique<renderer::PostProcessResources>();
    if (const auto r = post_process->initialize(*device, AEG_POST_PROCESS_SHADER_PACKAGE_PATH, Format::kRgba8Unorm); !r)
        return r;
#endif

    if (const Result result = create_static_meshes(); !result) {
        return result;
    }
    return create_object_and_lighting_resources();
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
        auto buffer = device->create_buffer(desc);
        if (!buffer) {
            return buffer.result;
        }
        if (const Result result = device->write_buffer(buffer.value, 0, vertices[index]); !result) {
            return result;
        }
        mesh_vertex_buffers[index] = buffer.value;
        mesh_vertex_descs[index] = desc;
    }
    index_buffer_desc = BufferDesc{};
    index_buffer_desc.size = sizeof(kQuadIndices);
    index_buffer_desc.capabilities = BufferCapability::kIndex;
    index_buffer_desc.memory = MemoryClass::kUpload;
    index_buffer_desc.initial_usage = ResourceUsage::kIndexInput;
    index_buffer_desc.initial_stages = PipelineStage::kVertexInput;
    index_buffer_desc.initial_queue = QueueClass::kGraphics;
    index_buffer_desc.name = DebugName{"Editor.Mesh.Indices"};
    auto indices = device->create_buffer(index_buffer_desc);
    if (!indices) {
        return indices.result;
    }
    if (const Result result = device->write_buffer(indices.value, 0, std::as_bytes(std::span{kQuadIndices})); !result) {
        return result;
    }
    index_buffer = indices.value;
    for (uint32_t index = 0; index < 2; ++index) {
        meshes[index] = {
            mesh_vertex_buffers[index],
            mesh_vertex_descs[index],
            index_buffer,
            index_buffer_desc,
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
        lighting_desc = make_uniform_desc(sizeof(renderer::RenderLightingConstants), "Editor.Lighting.Buffer");
        auto lighting_buffer = device->create_buffer(lighting_desc);
        if (!lighting_buffer) {
            return lighting_buffer.result;
        }
        lighting_buffers[frame] = lighting_buffer.value;
        auto lighting_set = device->create_binding_set(
            BindingSetDesc{lighting_layout, BindingSetLifetime::kPersistent, DebugName{"Editor.Lighting.Set"}});
        if (!lighting_set) {
            return lighting_set.result;
        }
        BindingResource lighting_resource{};
        lighting_resource.type = BindingType::kUniformBuffer;
        lighting_resource.buffer_range =
            BufferRange{lighting_buffers[frame], 0, sizeof(renderer::RenderLightingConstants)};
        const BindingWrite lighting_write{{2, 0}, 0, lighting_resource};
        if (const Result result = device->update_binding_set(lighting_set.value, std::span{&lighting_write, 1});
            !result) {
            return result;
        }
        lighting_sets[frame] = lighting_set.value;
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::create_targets() {
    color_desc = make_target_desc(viewport_width, viewport_height, Format::kRgba8Unorm,
                                  TextureCapability::kColorAttachment | TextureCapability::kSampled |
                                      TextureCapability::kTransferSource | TextureCapability::kTransferDestination,
                                  "Editor.Scene.Color");
    auto color = device->create_texture(color_desc);
    if (!color) {
        return color.result;
    }
    color_texture = color.value;
    auto color_view = device->create_texture_view({color_texture,
                                                   TextureViewKind::kColorAttachment,
                                                   TextureViewDimension::kTex2D,
                                                   color_desc.format,
                                                   {rhi::Aspect::kColor, 0, 1, 0, 1},
                                                   {},
                                                   DebugName{"Editor.Scene.ColorView"}});
    if (!color_view) {
        return color_view.result;
    }
    this->color_view = color_view.value;
    auto sampled_view = device->create_texture_view({color_texture,
                                                     TextureViewKind::kSampled,
                                                     TextureViewDimension::kTex2D,
                                                     color_desc.format,
                                                     {rhi::Aspect::kColor, 0, 1, 0, 1},
                                                     {},
                                                     DebugName{"Editor.Scene.SampledView"}});
    if (!sampled_view) {
        return sampled_view.result;
    }
    color_sampled_view = sampled_view.value;
    depth_desc = make_target_desc(viewport_width, viewport_height, Format::kD32Float,
                                  TextureCapability::kDepthStencil | TextureCapability::kSampled, "Editor.Scene.Depth");
    auto depth = device->create_texture(depth_desc);
    if (!depth) {
        return depth.result;
    }
    depth_texture = depth.value;
    auto depth_view = device->create_texture_view({depth_texture,
                                                   TextureViewKind::kDepthStencil,
                                                   TextureViewDimension::kTex2D,
                                                   depth_desc.format,
                                                   {rhi::Aspect::kDepth, 0, 1, 0, 1},
                                                   {},
                                                   DebugName{"Editor.Scene.DepthView"}});
    if (!depth_view) {
        return depth_view.result;
    }
    this->depth_view = depth_view.value;
    if (!viewport_sampler.is_valid()) {
        auto sampler = device->create_sampler({rhi::Filter::kLinear, rhi::Filter::kLinear, rhi::Filter::kLinear,
                                               rhi::AddressMode::kClampToEdge, rhi::AddressMode::kClampToEdge,
                                               rhi::AddressMode::kClampToEdge, 0.0f, 0.0f, 1000.0f, false, 1.0f,
                                               DebugName{"Editor.Scene.Sampler"}});
        if (!sampler) {
            return sampler.result;
        }
        viewport_sampler = sampler.value;
    }
    target_resize_pending = false;
    target_has_previous_frame = false;
    viewport_export_state = undefined_state();
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::resize_targets() {
    if (!target_resize_pending) {
        return Result::success();
    }
    if (const Result result = device->wait_idle(); !result) {
        return result;
    }
    // GPU completion alone does not retire descriptor sets that still sample the old depth target.
    if (const Result result = frame_context->drain(); !result) {
        return result;
    }
    // EngineFrameOrchestrator caches graph-build state. It cannot survive a
    // RenderGraph reset that invalidates the imported viewport resources.
    orchestrator.reset();
    // The scene RenderGraph owns cached attachment and sampled views for the
    // imported viewport targets. Those views outlive a submitted frame, so
    // detach them before destroying the externally-owned SceneColor texture.
    // Waiting above guarantees that no command list can still consume them.
    render_graph->reset();
    if (color_view.is_valid()) {
        if (const Result result = device->destroy_texture_view(color_view); !result) {
            return result;
        }
        color_view = {};
    }
    if (color_sampled_view.is_valid()) {
        if (const Result result = device->destroy_texture_view(color_sampled_view); !result) {
            return result;
        }
        color_sampled_view = {};
    }
    if (color_texture.is_valid()) {
        if (const Result result = device->destroy_texture(color_texture); !result) {
            return result;
        }
        color_texture = {};
    }
    if (depth_view.is_valid()) {
        if (const Result result = device->destroy_texture_view(depth_view); !result) {
            return result;
        }
        depth_view = {};
    }
    if (depth_texture.is_valid()) {
        if (const Result result = device->destroy_texture(depth_texture); !result) {
            return result;
        }
        depth_texture = {};
    }
    viewport_width = requested_width;
    viewport_height = requested_height;
    if (const Result result = create_targets(); !result) {
        return result;
    }
    if (active_scope == nullptr) {
        return foundation::invalid_state("Editor scene has no active WorldScope after viewport resize");
    }
    return create_orchestrator_for(*active_scope);
}

foundation::Result EditorSceneRuntime::Impl::make_default_scene() {
    scene_asset = scene::SceneAsset{};
    if (const Result result = scene_asset.set_identity(scene::SceneAssetId{0xAE62026u, 1u}, "Editor Sample Scene", 1);
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
        if (const Result result = scene_asset.add_entity(entity); !result) {
            return result;
        }
    }
    Transform camera_transform{};
    camera_transform.translation = {0.0f, 0.0f, 3.0f};
    Camera camera{};
    Transform light_transform{};
    DirectionalLight light{};
    light.casts_shadow = true;
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
        return scene_asset.add_reflected_component(entity, component, reflection_registry, schema, value);
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
    scene_asset = scene::SceneAsset{};
    active_camera_scene = {};
    return scene_asset.set_identity(scene::SceneAssetId{0xAE62026u, 2u}, "Main Scene", 1);
}

foundation::Result EditorSceneRuntime::Impl::instantiate_world(scene::SceneAsset const& asset,
                                                               std::shared_ptr<ecs::World>& state,
                                                               std::unique_ptr<world::WorldScope>& scope,
                                                               scene::SceneInstance& instance,
                                                               ecs::EntityId& camera_runtime, std::string_view name) {
    const auto handle = world_manager.create_world({std::string(name), true});
    if (!handle) {
        return handle.result;
    }
    const auto state_result = world_manager.find(handle.value);
    if (!state_result) {
        return state_result.result;
    }
    state = state_result.value;
    if (const Result result = aeg::engine::components::register_render_component_bundle(*state, reflection_registry);
        !result) {
        return result;
    }
    try {
        scope = std::make_unique<world::WorldScope>(state);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor World Scope allocation failed");
    }
    if (const Result result = scope->initialize(); !result) {
        return result;
    }
    const auto instance_result = asset.instantiate(*state, reflection_registry);
    if (!instance_result) {
        return instance_result.result;
    }
    instance = std::move(instance_result.value);
    camera_runtime = {};
    const auto camera = instance.runtime_entity(active_camera_scene);
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
    render_graph->reset();
    orchestrator.reset();
    try {
        orchestrator =
        std::make_unique<engine::EngineFrameOrchestrator>(pipeline_host, *frame_context, *render_graph);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor scene orchestrator allocation failed");
    }
    const auto registration = orchestrator->register_world_frame(scope, 0);
    if (!registration) return registration;
    if (mode != EditorSceneRuntime::Mode::kEdit && world_command_consumer != nullptr) {
        if (const auto result = engine::detail::EngineFrameWorldCommands::attach(*orchestrator,
                *world_command_consumer); !result)
            return result;
    }
    return orchestrator->initialize();
}

foundation::Result EditorSceneRuntime::Impl::rebuild_edit_world() {
    orchestrator.reset();
    edit_scope.reset();
    edit_state.reset();
    const Result result =
        instantiate_world(scene_asset, edit_state, edit_scope, edit_instance, edit_camera_runtime, "Editor.EditWorld");
    if (!result) {
        return result;
    }
    if (mode == EditorSceneRuntime::Mode::kEdit || mode == EditorSceneRuntime::Mode::kPause) {
        active_scope = edit_scope.get();
        return create_orchestrator_for(*active_scope);
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::commit_edit_commands() {
    if (edit_scope == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    if (const Result result = edit_scope->apply_commands(edit_scope->command_buffer()); !result) {
        return result;
    }
    if (const auto snapshot = edit_scope->commit(); !snapshot) {
        return snapshot.result;
    }
    return sync_scene_from_edit_world();
}

foundation::Result EditorSceneRuntime::Impl::sync_scene_from_edit_world() {
    if (edit_scope == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    const auto snapshot_result = edit_scope->latest_snapshot();
    if (!snapshot_result) {
        return snapshot_result.result;
    }
    scene::SceneAsset next(scene_asset);
    next.clear();
    for (const auto& entity : scene_asset.entities()) {
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
            for (const auto& binding : edit_instance.entities()) {
                if (binding.runtime_entity == runtime_entity) {
                    scene_entity = binding.scene_entity;
                    break;
                }
            }
            const void* value = snapshot.component_value_at(component_type, value_index);
            if (!scene_entity.is_valid() || value == nullptr) {
                continue;
            }
            if (const Result result =
                    next.add_reflected_component(scene_entity, component_type, reflection_registry, schema_type, value);
                !result) {
                return result;
            }
        }
    }
    scene_asset = std::move(next);
    refresh_dirty();
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::reload_project_feature_authoring() {
    if (!project_manager.loaded()) {
        project_feature_authoring = {};
        return Result::success();
    }
    const std::filesystem::path root = project_manager.descriptor().root;
    const std::filesystem::path project_file = project_manager.project_file();
    const auto project = project::ProjectFileCodec{}.load(project_file);
    if (!project)
        return project.result;
    const auto paths = project::ProjectPaths::resolve(project_file, project.value.config.paths);
    if (!paths)
        return paths.result;
    const auto loaded =
        load_editor_project_feature_authoring(root, paths.value.directory(project::ProjectDirectory::kSource),
                                             project_manager.assets_directory());
    if (!loaded)
        return loaded.result;
    project_feature_authoring = std::move(loaded.value);
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::synchronize_asset_pipeline() {
    if (!project_manager.loaded()) {
        asset_pipeline.shutdown();
        asset_pipeline_ready = false;
        return Result::success();
    }
    const std::filesystem::path expected_derived_root = project_manager.descriptor().root / ".aeg" / "DerivedData";
    if (asset_pipeline_ready && asset_pipeline.derived_root() != expected_derived_root) {
        asset_pipeline.shutdown();
        asset_pipeline_ready = false;
    }
    if (!asset_pipeline_ready) {
        if (const Result result = asset_pipeline.initialize(project_manager.descriptor().root); !result) {
            return result;
        }
        asset_pipeline_ready = true;
    }
    return asset_pipeline.synchronize(asset_database.assets());
}

foundation::Result EditorSceneRuntime::Impl::defer_material_bindings(MaterialGpu& material) {
    if (frame_context == nullptr || !frame_context->is_frame_active()) {
        return foundation::invalid_state("Material retirement requires an active FrameContext frame");
    }
    for (BindingSetHandle& set : material.binding_sets) {
        if (set.is_valid()) {
            if (const Result result = frame_context->defer_destroy_binding_set(set); !result) {
                return result;
            }
            set = {};
        }
    }
    for (BufferHandle& buffer : material.parameter_buffers) {
        if (buffer.is_valid()) {
            if (const Result result = frame_context->defer_destroy_buffer(buffer); !result) {
                return result;
            }
            buffer = {};
        }
    }
    if (material.sampler.is_valid()) {
        if (const Result result = frame_context->defer_destroy_sampler(material.sampler); !result) {
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
        device->create_sampler({rhi::Filter::kLinear, rhi::Filter::kLinear, rhi::Filter::kLinear,
                                rhi::AddressMode::kClampToEdge, rhi::AddressMode::kClampToEdge,
                                rhi::AddressMode::kClampToEdge, 0.0f, 0.0f, 1000.0f, false, 1.0f, DebugName{name}});
    if (!sampler) {
        return sampler.result;
    }
    material.sampler = sampler.value;
    material.parameter_desc = make_uniform_desc(sizeof(resource::TextureMaterialParameters), name);
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        auto parameter = device->create_buffer(material.parameter_desc);
        if (!parameter) {
            return parameter.result;
        }
        material.parameter_buffers[frame] = parameter.value;
        auto set = device->create_binding_set(
            BindingSetDesc{material_layout, BindingSetLifetime::kPersistent, DebugName{name}});
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
        if (const Result result = device->update_binding_set(set.value, writes); !result) {
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
    const auto existing = imported_meshes.find(asset.asset_id);
    if (existing != imported_meshes.end() && existing->second.source_hash == asset.source_hash &&
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
        mesh_uploader->upload_mesh(context.frame_context, context.render_graph, mesh_data, "Editor.Asset.Mesh");
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
    if (existing != imported_meshes.end()) {
        if (const Result result = mesh_uploader->defer_destroy_mesh(context.frame_context, existing->second.gpu);
            !result) {
            static_cast<void>(mesh_uploader->destroy_mesh(replacement.gpu));
            return result;
        }
        existing->second = std::move(replacement);
    } else {
        imported_meshes.emplace(asset.asset_id, std::move(replacement));
    }
    frame_vertex_graphs[asset.asset_id] = to_pipeline_handle(upload.value.vertex_resource);
    frame_index_graphs[asset.asset_id] = to_pipeline_handle(upload.value.index_resource);
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::upload_texture_asset(
    const asset_pipeline::CpuAsset& asset, const engine::EngineFramePreparationContext& context) {
    if (asset.width == 0 || asset.height == 0 ||
        asset.pixels.size() != static_cast<std::size_t>(asset.width) * asset.height * 4u) {
        return foundation::invalid_argument("Imported texture has invalid RGBA8 pixel data");
    }
    const auto existing = imported_textures.find(asset.asset_id);
    if (existing != imported_textures.end() && existing->second.source_hash == asset.source_hash &&
        existing->second.gpu.is_valid() && (existing->second.from_baked_format || !asset.from_baked_format)) {
        return Result::success();
    }
    const TextureDesc desc = make_asset_texture_desc(asset.width, asset.height, "Editor.Asset.Texture");
    const resource::TextureSubresourceData subresource{
        std::span<const std::byte>(asset.pixels.data(), asset.pixels.size()), asset.width * 4u,
        asset.width * asset.height * 4u};
    const resource::TextureData texture_data{desc, std::span<const resource::TextureSubresourceData>(&subresource, 1)};
    const auto upload = texture_uploader->upload_texture(context.frame_context, context.render_graph, texture_data,
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
    if (existing != imported_textures.end()) {
        if (const Result result = texture_uploader->defer_destroy_texture(context.frame_context, existing->second.gpu);
            !result) {
            static_cast<void>(texture_uploader->destroy_texture(replacement.gpu));
            return result;
        }
        existing->second = std::move(replacement);
    } else {
        imported_textures.emplace(asset.asset_id, std::move(replacement));
    }
    frame_texture_graphs[asset.asset_id] = to_pipeline_handle(upload.value.target);
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
    const auto texture = imported_textures.find(asset.texture_asset_id);
    const MaterialGpu& fallback = materials[0];
    if (texture != imported_textures.end() && texture->second.gpu.has_sampled_view()) {
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
    const auto existing = imported_materials.find(asset.asset_id);
    if (existing != imported_materials.end()) {
        if (const Result result = defer_material_bindings(existing->second); !result) {
            return result;
        }
        existing->second = std::move(candidate);
    } else {
        imported_materials.emplace(asset.asset_id, std::move(candidate));
    }
    (void)context;
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::consume_asset(const asset_pipeline::CpuAsset& asset,
                                                           const engine::EngineFramePreparationContext& context) {
    const EditorAssetRecord* record = asset_database.find(asset.asset_id);
    if (record == nullptr || record->content_hash != asset.source_hash) {
        return Result::success();
    }
    if (!asset.success) {
        diagnostic = "Asset " + record->relative_path + " failed to import: " + asset.diagnostic;
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
    if (!asset_pipeline_ready) {
        return Result::success();
    }
    for (const EditorAssetRecord& record : asset_database.assets()) {
        if (record.status != EditorAssetStatus::kImported ||
            (record.kind != EditorAssetKind::kMesh && record.kind != EditorAssetKind::kTexture &&
             record.kind != EditorAssetKind::kMaterial)) {
            continue;
        }
        bool already_loaded = false;
        if (record.kind == EditorAssetKind::kMesh) {
            const auto found = imported_meshes.find(record.id);
            already_loaded = found != imported_meshes.end() && found->second.source_hash == record.content_hash;
        } else if (record.kind == EditorAssetKind::kTexture) {
            const auto found = imported_textures.find(record.id);
            already_loaded = found != imported_textures.end() && found->second.source_hash == record.content_hash;
        } else {
            const auto found = imported_materials.find(record.id);
            already_loaded = found != imported_materials.end() && found->second.source_hash == record.content_hash;
        }
        if (already_loaded) {
            continue;
        }
        asset_pipeline::CpuAsset fast{};
        const Result result = asset_pipeline.load_fast_path(record.id, fast);
        if (!result) {
            if ((record.kind == EditorAssetKind::kMesh && imported_meshes.contains(record.id)) ||
                (record.kind == EditorAssetKind::kTexture && imported_textures.contains(record.id)) ||
                (record.kind == EditorAssetKind::kMaterial && imported_materials.contains(record.id))) {
                diagnostic = "Reimport failed for " + record.relative_path +
                             "; retaining last-good GPU resource: " + result.message;
            } else {
                diagnostic =
                    "Initial import failed for " + record.relative_path + "; using fallback: " + result.message;
            }
            continue;
        }
        if (const Result consume = consume_asset(fast, context); !consume) {
            if ((record.kind == EditorAssetKind::kMesh && imported_meshes.contains(record.id)) ||
                (record.kind == EditorAssetKind::kTexture && imported_textures.contains(record.id)) ||
                (record.kind == EditorAssetKind::kMaterial && imported_materials.contains(record.id))) {
                diagnostic = "Reimport failed for " + record.relative_path +
                             "; retaining last-good GPU resource: " + consume.message;
            } else {
                diagnostic =
                    "Initial import failed for " + record.relative_path + "; using fallback: " + consume.message;
            }
        }
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::retire_missing_assets(
    const engine::EngineFramePreparationContext& context) {
    const auto should_retire = [this](uint64_t id, EditorAssetKind kind) {
        const auto* record = asset_database.find(id);
        return record == nullptr || record->kind != kind || record->status == EditorAssetStatus::kMissing ||
               record->status == EditorAssetStatus::kUnsupported;
    };

    for (auto iterator = imported_meshes.begin(); iterator != imported_meshes.end();) {
        if (!iterator->second.transient && should_retire(iterator->first, EditorAssetKind::kMesh)) {
            frame_vertex_graphs.erase(iterator->first);
            frame_index_graphs.erase(iterator->first);
            if (const Result result = mesh_uploader->defer_destroy_mesh(context.frame_context, iterator->second.gpu);
                !result) {
                return result;
            }
            iterator = imported_meshes.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = imported_textures.begin(); iterator != imported_textures.end();) {
        if (!iterator->second.transient && should_retire(iterator->first, EditorAssetKind::kTexture)) {
            frame_texture_graphs.erase(iterator->first);
            if (const Result result =
                    texture_uploader->defer_destroy_texture(context.frame_context, iterator->second.gpu);
                !result) {
                return result;
            }
            iterator = imported_textures.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = imported_materials.begin(); iterator != imported_materials.end();) {
        if (!iterator->second.transient && should_retire(iterator->first, EditorAssetKind::kMaterial)) {
            if (const Result result = defer_material_bindings(iterator->second); !result) {
                return result;
            }
            iterator = imported_materials.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::import_frame_asset_graphs(
    const engine::EngineFramePreparationContext& context) {
    for (auto& [id, mesh] : imported_meshes) {
        if (!frame_vertex_graphs.contains(id)) {
            const auto state = query_mesh_buffer_initial_state(*device, mesh.gpu.vertex_buffer);
            if (!state)
                return state.result;
            const auto vertex =
                context.render_graph.import_buffer(mesh.gpu.vertex_buffer, mesh.render.vertex_buffer_desc, state.value);
            if (!vertex) {
                return vertex.result;
            }
            frame_vertex_graphs[id] = to_pipeline_handle(vertex.value);
        }
        if (!frame_index_graphs.contains(id)) {
            const auto state = query_mesh_buffer_initial_state(*device, mesh.gpu.index_buffer);
            if (!state)
                return state.result;
            const auto index =
                context.render_graph.import_buffer(mesh.gpu.index_buffer, mesh.render.index_buffer_desc, state.value);
            if (!index) {
                return index.result;
            }
            frame_index_graphs[id] = to_pipeline_handle(index.value);
        }
        mesh.render.vertex_resource = frame_vertex_graphs[id];
        mesh.render.index_resource = frame_index_graphs[id];
    }
    for (auto& [id, texture] : imported_textures) {
        if (!frame_texture_graphs.contains(id)) {
            const auto state = query_texture_initial_state(*device, texture.gpu.texture);
            if (!state) {
                return state.result;
            }
            const auto imported = context.render_graph.import_texture(texture.gpu.texture, texture.gpu.sampled_view,
                                                                      texture.desc, state.value);
            if (!imported) {
                return imported.result;
            }
            frame_texture_graphs[id] = to_pipeline_handle(imported.value);
        }
        texture.graph_texture = frame_texture_graphs[id];
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::refresh_material_texture_bindings(
    const engine::EngineFramePreparationContext& context) {
    for (auto& [id, material] : imported_materials) {
        TextureHandle texture = materials[0].texture;
        TextureViewHandle sampled_view = materials[0].sampled_view;
        TextureDesc texture_desc = materials[0].texture_desc;
        const auto found = imported_textures.find(material.texture_asset_id);
        if (found != imported_textures.end() && found->second.gpu.has_sampled_view()) {
            texture = found->second.gpu.texture;
            sampled_view = found->second.gpu.sampled_view;
            texture_desc = found->second.desc;
        }
        if (material.sampled_view == sampled_view) {
            material.graph_texture =
                found != imported_textures.end() ? found->second.graph_texture : materials[0].graph_texture;
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
            found != imported_textures.end() ? found->second.graph_texture : materials[0].graph_texture;
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
        if (text.font_asset_id != 0 && asset_pipeline_ready) {
            const Result loaded = asset_pipeline.load_fast_path(text.font_asset_id, font);
            if (!loaded) {
                diagnostic = "Font import failed; using bundled fallback font: " + loaded.message;
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
        const auto cached_it = cached_fonts.find(text.font_asset_id);
        if (cached_it != cached_fonts.end() && cached_it->second.source_hash == font_hash) {
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
            cached_fonts[text.font_asset_id] = std::move(replacement);
            cached = &cached_fonts[text.font_asset_id];
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
        const auto old = imported_materials.find(material.asset_id);
        if (old == imported_materials.end() || old->second.source_hash != material.source_hash) {
            if (const Result material_result = rebuild_material_asset(material, context); !material_result) {
                result = material_result;
                return;
            }
        }
    });
    return result;
}

foundation::Result EditorSceneRuntime::Impl::prepare_frame(const engine::EngineFramePreparationContext& context) {
    current_slot = context.frame_context.frame_slot_index();
    frame_object_slots.clear();
    // RenderGraph handles are frame-local. Re-import every retained GPU asset
    // into the current graph, even when its GPU payload did not change.
    frame_texture_graphs.clear();
    frame_vertex_graphs.clear();
    frame_index_graphs.clear();
    if (active_scope == nullptr) {
        return foundation::invalid_state("Editor scene has no active World scope");
    }
    const auto snapshot = active_scope->latest_snapshot();
    if (!snapshot) {
        return snapshot.result;
    }
    const auto lighting = make_editor_lighting_constants(*snapshot.value);
    if (!lighting) {
        return lighting.result;
    }
    if (const Result result =
            device->write_buffer(lighting_buffers[current_slot], 0, std::as_bytes(std::span{&lighting.value, 1}));
        !result) {
        return result;
    }
    ResourceState color_initial_state = undefined_state();
    if (target_has_previous_frame) {
        const auto queried_state = query_texture_initial_state(*device, color_texture);
        if (!queried_state) {
            return queried_state.result;
        }
        color_initial_state = queried_state.value;
    }
    const auto color = context.render_graph.import_texture(color_texture, color_view, color_desc, color_initial_state);
    const auto depth = context.render_graph.import_texture(depth_texture, depth_view, depth_desc, undefined_state());
    if (!color || !depth) {
        return color ? depth.result : color.result;
    }
    if (const Result result = context.bind_feature_texture(kSceneColor, color.value); !result) {
        return result;
    }
    if (const Result result = context.bind_feature_texture(kSceneDepth, depth.value); !result) {
        return result;
    }

    {
        const std::array targets{
            std::pair{RenderResourceId("Renderer.SceneLighting"), Format::kRgba16Float},
            std::pair{RenderResourceId("Renderer.GBuffer.Albedo"), Format::kRgba16Float},
            std::pair{RenderResourceId("Renderer.GBuffer.Normal"), Format::kRgba16Float},
            std::pair{RenderResourceId("Renderer.GBuffer.Material"), Format::kRgba8Unorm},
        };
        for (const auto& scene : context.render_scenes) {
            if (!scene) continue;
            const bool canonical = scene == context.render_scenes.front();
            for (const auto& [id, format] : targets) {
                TextureDesc desc{};
                desc.dimension = rhi::TextureDimension::kTex2D;
                desc.format = format;
                desc.extent = {scene->view.output_extent.width, scene->view.output_extent.height, 1};
                desc.mip_levels = 1;
                desc.array_layers = 1;
                desc.capabilities = TextureCapability::kColorAttachment | TextureCapability::kSampled;
                desc.initial_queue = QueueClass::kGraphics;
                const auto texture = context.render_graph.create_texture(desc);
                if (!texture) return texture.result;
                const auto target = canonical ? id : renderer::make_view_scoped_render_resource_id(id, scene->view.id);
                if (const auto result = context.bind_feature_texture(target, texture.value); !result) return result;
            }
        }
    }
    // External directional ShadowDepth targets are scoped per extracted View.
    // The first view keeps the legacy identity; additional views use a stable
    // renderer-scoped identity so their depth maps cannot alias each other.
    for (std::size_t index = 0; index < context.render_scenes.size(); ++index) {
        const auto& scene = context.render_scenes[index];
        if (!scene)
            continue;
        const auto extent = scene->view.shadow_extent.is_valid() ? scene->view.shadow_extent
                                                                  : scene->view.output_extent;
        if (!extent.is_valid())
            return foundation::invalid_argument("Editor ShadowDepth view extent is invalid");
        const auto target = index == 0
                                ? feature::RenderResourceId{"Renderer.ShadowDepth"}
                                : renderer::make_view_scoped_render_resource_id(
                                      feature::RenderResourceId{"Renderer.ShadowDepth"}, scene->view.id);
        const auto shadow = context.render_graph.create_texture(
            make_target_desc(extent.width, extent.height, Format::kD32Float,
                             TextureCapability::kDepthStencil | TextureCapability::kSampled,
                             "Editor.ShadowDepth"),
            render_graph::ResourceLifetime::kTransient);
        if (!shadow)
            return shadow.result;
        if (const Result result = context.bind_feature_texture(target, shadow.value); !result)
            return result;
    }

    if (viewport_readback.requested) {
        const uint64_t pixel_count = static_cast<uint64_t>(viewport_width) * viewport_height;
        const auto allocation = context.frame_context.allocate_readback(pixel_count * 4u, 256);
        if (!allocation) {
            return allocation.result;
        }
        viewport_readback.allocation = allocation.value;
        viewport_readback.width = viewport_width;
        viewport_readback.height = viewport_height;
        viewport_readback.desc = {};
        viewport_readback.desc.size = allocation.value.buffer_size;
        viewport_readback.desc.capabilities =
            rhi::BufferCapability::kTransferSource | rhi::BufferCapability::kTransferDestination;
        viewport_readback.desc.memory = MemoryClass::kReadback;
        viewport_readback.desc.initial_usage = ResourceUsage::kHostRead;
        viewport_readback.desc.initial_stages = PipelineStage::kHost;
        viewport_readback.desc.initial_queue = QueueClass::kGraphics;
        viewport_readback.desc.name = DebugName{"Editor.ViewportReadback"};
        viewport_readback.initial_state = ResourceState{ResourceUsage::kHostRead, ShaderStage::kAll,
                                                        QueueClass::kGraphics, false, PipelineStage::kHost};
    }

    frame_texture_graphs.clear();
    frame_vertex_graphs.clear();
    frame_index_graphs.clear();

    snapshot.value->for_each<MeshRenderer>([this](ecs::EntityId, const MeshRenderer& mesh) {
        const std::array<float, 4> tint{mesh.tint.x, mesh.tint.y, mesh.tint.z, mesh.tint_alpha};
        if (mesh.material_asset_id == kMaterialAssetRed || mesh.material_asset_id == kMaterialAssetGreen) {
            materials[mesh.material_asset_id == kMaterialAssetGreen ? 1u : 0u].tint = tint;
        } else if (const auto found = imported_materials.find(mesh.material_asset_id);
                   found != imported_materials.end()) {
            found->second.tint = tint;
        }
    });
    if (!materials_ready) {
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
                texture_uploader->upload_texture(context.frame_context, context.render_graph, upload_data,
                                                 index == 0 ? "Editor.Material.Red" : "Editor.Material.Green");
            if (!upload) {
                return upload.result;
            }
            MaterialGpu& material = materials[index];
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
        materials_ready = true;
    } else {
        for (uint32_t index = 0; index < kMaterialCount; ++index) {
            const auto queried_state = query_texture_initial_state(*device, materials[index].texture);
            if (!queried_state) {
                return queried_state.result;
            }
            auto imported = context.render_graph.import_texture(materials[index].texture, materials[index].sampled_view,
                                                                materials[index].texture_desc, queried_state.value);
            if (!imported) {
                return imported.result;
            }
            materials[index].graph_texture = to_pipeline_handle(imported.value);
        }
    }

    if (asset_pipeline_ready) {
        std::vector<asset_pipeline::CpuAsset> completed;
        if (const Result result = asset_pipeline.poll(completed); !result) {
            return result;
        }
        bool asset_database_changed = false;
        for (const asset_pipeline::CpuAsset& asset : completed) {
            const EditorAssetRecord* record = asset_database.find(asset.asset_id);
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
            if (const Result result = asset_database.refresh(); !result) {
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
                ((asset.kind == EditorAssetKind::kMesh && imported_meshes.contains(asset.asset_id) &&
                  imported_meshes[asset.asset_id].source_hash == asset.source_hash &&
                  imported_meshes[asset.asset_id].from_baked_format) ||
                 (asset.kind == EditorAssetKind::kTexture && imported_textures.contains(asset.asset_id) &&
                  imported_textures[asset.asset_id].source_hash == asset.source_hash &&
                  imported_textures[asset.asset_id].from_baked_format) ||
                 (asset.kind == EditorAssetKind::kMaterial && imported_materials.contains(asset.asset_id) &&
                  imported_materials[asset.asset_id].source_hash == asset.source_hash &&
                  imported_materials[asset.asset_id].from_baked_format))) {
                continue;
            }
            if (const Result result = consume_asset(asset, context); !result) {
                diagnostic = "Background asset replacement failed: " + result.message;
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
    for (auto& [id, material] : imported_materials) {
        const auto texture = imported_textures.find(material.texture_asset_id);
        material.graph_texture =
            texture != imported_textures.end() ? texture->second.graph_texture : materials[0].graph_texture;
        (void)id;
    }
    for (uint32_t index = 0; index < kMaterialCount; ++index) {
        MaterialGpu& material = materials[index];
        resource::TextureMaterialParameters parameters{};
        parameters.tint = material.tint;
        if (const Result result = device->write_buffer(material.parameter_buffers[current_slot], 0,
                                                       std::as_bytes(std::span{&parameters, 1}));
            !result) {
            return result;
        }
        const RenderResourceId parameter_id = index == 0 ? kMaterialParameterRed : kMaterialParameterGreen;
        const auto parameter_graph = context.render_graph.import_buffer(material.parameter_buffers[current_slot],
                                                                        material.parameter_desc, host_write_state());
        if (!parameter_graph) {
            return parameter_graph.result;
        }
        material.graph_parameter = to_pipeline_handle(parameter_graph.value);
        (void)parameter_id;
    }
    for (auto& [id, material] : imported_materials) {
        resource::TextureMaterialParameters parameters{};
        parameters.tint = material.tint;
        if (const Result result = device->write_buffer(material.parameter_buffers[current_slot], 0,
                                                       std::as_bytes(std::span{&parameters, 1}));
            !result) {
            return result;
        }
        const auto parameter_graph = context.render_graph.import_buffer(material.parameter_buffers[current_slot],
                                                                        material.parameter_desc, host_write_state());
        if (!parameter_graph) {
            return parameter_graph.result;
        }
        material.graph_parameter = to_pipeline_handle(parameter_graph.value);
        (void)id;
    }
    const auto lighting_graph =
        context.render_graph.import_buffer(lighting_buffers[current_slot], lighting_desc, host_write_state());
    if (!lighting_graph) {
        return lighting_graph.result;
    }
    this->lighting_graph = to_pipeline_handle(lighting_graph.value);
    target_has_previous_frame = true;
    return Result::success();
}

foundation::Result EditorSceneRuntime::Impl::destroy_resources() {
    if (device == nullptr) {
        return Result::success();
    }
    asset_pipeline.shutdown();
    asset_pipeline_ready = false;
    if (const auto result = device->wait_idle(); !result)
        return result;
    if (frame_context) {
        if (const auto result = frame_context->drain(); !result)
            return result;
    }
    if (render_graph)
        render_graph->reset();
    renderer.clear_extractors();
    for (auto& [id, material] : imported_materials) {
        for (BindingSetHandle set : material.binding_sets) {
            if (set.is_valid()) {
                (void)device->destroy_binding_set(set);
            }
        }
        for (BufferHandle buffer : material.parameter_buffers) {
            if (buffer.is_valid()) {
                (void)device->destroy_buffer(buffer);
            }
        }
        if (material.sampler.is_valid()) {
            (void)device->destroy_sampler(material.sampler);
        }
        (void)id;
    }
    imported_materials.clear();
    for (auto& [id, texture] : imported_textures) {
        (void)texture_uploader->destroy_texture(texture.gpu);
        (void)id;
    }
    imported_textures.clear();
    for (auto& [id, mesh] : imported_meshes) {
        (void)mesh_uploader->destroy_mesh(mesh.gpu);
        (void)id;
    }
    imported_meshes.clear();
    for (auto& material : materials) {
        for (auto set : material.binding_sets) {
            if (set.is_valid()) {
                (void)device->destroy_binding_set(set);
            }
        }
        for (auto buffer : material.parameter_buffers) {
            if (buffer.is_valid()) {
                (void)device->destroy_buffer(buffer);
            }
        }
        if (material.sampler.is_valid()) {
            (void)device->destroy_sampler(material.sampler);
        }
        if (material.sampled_view.is_valid()) {
            (void)device->destroy_texture_view(material.sampled_view);
        }
        if (material.texture.is_valid()) {
            (void)device->destroy_texture(material.texture);
        }
        material = {};
    }
    for (auto& frame : object_resources) {
        for (auto& object : frame) {
            if (object.binding_set.is_valid()) {
                (void)device->destroy_binding_set(object.binding_set);
            }
            if (object.transform_buffer.is_valid())
                (void)device->destroy_buffer(object.transform_buffer);
        }
        frame.clear();
    }
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        if (lighting_sets[frame].is_valid()) {
            (void)device->destroy_binding_set(lighting_sets[frame]);
        }
        if (lighting_buffers[frame].is_valid()) {
            (void)device->destroy_buffer(lighting_buffers[frame]);
        }
    }
    for (auto buffer : mesh_vertex_buffers) {
        if (buffer.is_valid()) {
            (void)device->destroy_buffer(buffer);
        }
    }
    if (index_buffer.is_valid()) {
        (void)device->destroy_buffer(index_buffer);
    }
    if (pipeline.is_valid()) {
        (void)device->destroy_pipeline(pipeline);
    }
    if (gbuffer_pipeline.is_valid()) {
        (void)device->destroy_pipeline(gbuffer_pipeline);
    }
    if (depth_pipeline.is_valid()) {
        (void)device->destroy_pipeline(depth_pipeline);
    }
    deferred_lighting.reset();
    shadows.reset();
    light_culling.reset();
    post_process.reset();
    if (clustered_pipeline.is_valid())
        (void)device->destroy_pipeline(clustered_pipeline);
    if (unshadowed_clustered.pipeline.is_valid())
        (void)device->destroy_pipeline(unshadowed_clustered.pipeline);
    if (shadow_pipeline.is_valid())
        (void)device->destroy_pipeline(shadow_pipeline);
    if (color_view.is_valid()) {
        (void)device->destroy_texture_view(color_view);
        (void)device->destroy_texture_view(color_sampled_view);
        (void)device->destroy_texture(color_texture);
    }
    if (depth_view.is_valid()) {
        (void)device->destroy_texture_view(depth_view);
        (void)device->destroy_texture(depth_texture);
    }
    if (viewport_sampler.is_valid()) {
        (void)device->destroy_sampler(viewport_sampler);
    }
    shader_manager_owner.reset();
    shader_manager = nullptr;
    return Result::success();
}

foundation::Result EditorSceneRuntime::shutdown() {
    if (impl_ == nullptr) return Result::success();
    if (const auto result = impl_->close_features(); !result) return result;
    impl_->initialized = false;
    impl_->orchestrator.reset();
    if (impl_->edit_scope != nullptr) {
        if (const auto result = impl_->edit_scope->shutdown(); !result) return result;
    }
    if (impl_->runtime_scope != nullptr) {
        if (const auto result = impl_->runtime_scope->shutdown(); !result) return result;
    }
    // Preserve the World owners and GPU context until the existing drain succeeds.
    if (const auto result = impl_->destroy_resources(); !result) return result;
    impl_.reset();
    return Result::success();
}

foundation::Result EditorSceneRuntime::request_viewport_extent(uint32_t width, uint32_t height) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (width == 0 || height == 0) {
        return foundation::invalid_argument("Editor viewport extent must be positive");
    }
    if (impl_->requested_width != width || impl_->requested_height != height) {
        impl_->requested_width = width;
        impl_->requested_height = height;
        impl_->target_resize_pending = true;
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::request_viewport_readback() {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    impl_->viewport_readback.requested = true;
    impl_->viewport_readback.pass_built = false;
    impl_->viewport_readback.pass_executed = false;
    return Result::success();
}

foundation::ResultValue<std::vector<std::byte>> EditorSceneRuntime::read_viewport_pixels() const {
    if (impl_ == nullptr || !impl_->initialized) {
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    }
    if (impl_->viewport_readback.pixels.empty()) {
        return {foundation::not_found("No viewport readback is available"), {}};
    }
    return {Result::success(), impl_->viewport_readback.pixels};
}

editor::ImGuiTextureBinding EditorSceneRuntime::viewport_binding() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return {impl_->color_sampled_view, impl_->viewport_sampler, impl_->color_texture, impl_->color_desc,
            impl_->viewport_export_state};
}

bool EditorSceneRuntime::viewport_ready() const noexcept {
    return impl_ != nullptr && impl_->initialized && impl_->color_texture.is_valid() &&
           impl_->color_sampled_view.is_valid() && impl_->viewport_sampler.is_valid();
}

bool EditorSceneRuntime::viewport_resize_pending() const noexcept {
    return impl_ != nullptr && impl_->initialized && impl_->target_resize_pending;
}

foundation::ResultValue<ViewportPickResult> EditorSceneRuntime::pick_viewport(ViewportPoint pixel, uint32_t width,
                                                                              uint32_t height) const {
    if (impl_ == nullptr || !impl_->initialized) {
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    }
    const auto ray = impl_->make_viewport_ray(pixel, width, height);
    if (!ray) {
        return {ray.result, {}};
    }
    const auto snapshot = impl_->edit_scope->latest_snapshot();
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
        if (transform == nullptr || !math::is_finite(transform->translation) || !math::is_finite(transform->scale)) {
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
            if (!math::is_finite(discriminant) || discriminant < 0.0f) {
                return;
            }
            distance = -b - std::sqrt(discriminant);
            if (distance < 0.0f) {
                distance = -b + std::sqrt(discriminant);
            }
        }
        if (!math::is_finite(distance) || distance < 0.0f || distance >= nearest) {
            return;
        }
        const auto scene_entity =
            std::find_if(impl_->edit_instance.entities().begin(), impl_->edit_instance.entities().end(),
                         [runtime_entity](const auto& binding) { return binding.runtime_entity == runtime_entity; });
        if (scene_entity == impl_->edit_instance.entities().end()) {
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
    if (impl_ == nullptr || !impl_->initialized) {
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    }
    if (impl_->mode != Mode::kEdit) {
        return {Result::success(), {}};
    }
    const auto view = impl_->make_edit_view(width, height);
    if (!view) {
        return {view.result, {}};
    }
    const auto snapshot = impl_->edit_scope->latest_snapshot();
    if (!snapshot) {
        return {snapshot.result, {}};
    }
    const auto runtime = impl_->edit_instance.runtime_entity(impl_->selected_entity);
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
    const float length = std::max(0.4f, math::is_finite(distance) ? distance * 0.18f : 0.75f);
    const math::Mat4 view_projection = view.value.camera.view_projection();
    const auto project = [width, height, &view_projection](math::Vec3 point, ViewportPoint& result) {
        const math::Vec4 clip = view_projection * math::Vec4{point.x, point.y, point.z, 1.0f};
        if (!math::is_finite(clip.x) || !math::is_finite(clip.y) || !math::is_finite(clip.z) ||
            !math::is_finite(clip.w) || clip.w <= math::kEpsilon) {
            return false;
        }
        const float ndc_x = clip.x / clip.w;
        const float ndc_y = clip.y / clip.w;
        result = {((ndc_x + 1.0f) * 0.5f) * static_cast<float>(width),
                  ((1.0f - ndc_y) * 0.5f) * static_cast<float>(height)};
        return math::is_finite(result.x) && math::is_finite(result.y);
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
    if (impl_ == nullptr || !impl_->initialized || impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Transform editing requires an initialized Edit World");
    }
    if (impl_->transform_edit.active) {
        return foundation::invalid_state("A Transform edit transaction is already active");
    }
    if (!valid_gizmo_axis(axis)) {
        return foundation::invalid_argument("Transform Gizmo axis is invalid");
    }
    const auto runtime = impl_->edit_instance.runtime_entity(impl_->selected_entity);
    if (!runtime) {
        return runtime.result;
    }
    const auto snapshot = impl_->edit_scope->latest_snapshot();
    if (!snapshot) {
        return snapshot.result;
    }
    const Transform* transform = snapshot.value->get<Transform>(runtime.value);
    if (transform == nullptr) {
        return foundation::not_found("Selected entity has no Transform component");
    }
    ViewportPoint scale_screen_axis{};
    if (mode == ViewportGizmoMode::kScale) {
        const auto projection = selected_gizmo_projection(width, height);
        if (!projection) {
            return projection.result;
        }
        if (!projection.value.valid) {
            return foundation::invalid_state("Transform scale handle is not visible");
        }
        const std::array<ViewportPoint, 3> tips{projection.value.x_axis, projection.value.y_axis,
                                                projection.value.z_axis};
        const ViewportPoint tip = tips[static_cast<std::size_t>(axis)];
        const float dx = tip.x - projection.value.center.x;
        const float dy = tip.y - projection.value.center.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!math::is_finite(length) || length <= math::kEpsilon) {
            return foundation::invalid_state("Transform scale axis has no screen direction");
        }
        // Freeze the visible positive-axis direction for the whole drag transaction.
        scale_screen_axis = {dx / length, dy / length};
    }
    const auto ray = impl_->make_viewport_ray(pixel, width, height);
    if (!ray) {
        return ray.result;
    }
    const Transform* camera_transform = snapshot.value->get<Transform>(impl_->edit_camera_runtime);
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
        Impl::EditTransaction transaction{
            true,        mode,         axis,        impl_->selected_entity, pixel,      *transform,
            initial_hit, plane_normal, axis_vector, scale_screen_axis,      *transform, {}};
        transaction.command.before.push_back(
            Impl::EditorComponentValue{impl_->selected_entity, transform_type, bytes_of(*transform)});
        impl_->history.reserve(impl_->history.size() + 1u);
        impl_->transform_edit = std::move(transaction);
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
    transform_edit = {};
    selected_entity = {};
    selected_entities.clear();
    mode = EditorSceneRuntime::Mode::kEdit;
    runtime_scope.reset();
    runtime_state.reset();
    runtime_camera_runtime = {};
    active_scope = nullptr;
    scene_path = project_manager.loaded() ? project_manager.default_scene_file()
                                          : std::filesystem::path{"editor_scene.aegscene"};
    const Result result = rebuild_edit_world();
    if (result) {
        reset_history(true);
    }
    return result;
}

foundation::Result EditorSceneRuntime::update_transform_edit(ViewportPoint pixel, uint32_t width, uint32_t height) {
    if (impl_ == nullptr || !impl_->initialized || !impl_->transform_edit.active) {
        return foundation::invalid_state("No Transform edit transaction is active");
    }
    const auto ray = impl_->make_viewport_ray(pixel, width, height);
    if (!ray) {
        return ray.result;
    }
    Transform next = impl_->transform_edit.initial;
    if (impl_->transform_edit.mode == ViewportGizmoMode::kTranslate) {
        math::Vec3 current_hit{};
        if (!intersect_ray_plane(ray.value.origin, ray.value.direction, impl_->transform_edit.initial.translation,
                                 impl_->transform_edit.plane_normal, current_hit)) {
            return foundation::invalid_state("Viewport ray cannot update the Transform translation");
        }
        const float delta =
            math::dot(current_hit - impl_->transform_edit.initial_plane_hit, impl_->transform_edit.axis_vector);
        next.translation += impl_->transform_edit.axis_vector * delta;
    } else if (impl_->transform_edit.mode == ViewportGizmoMode::kRotate) {
        const float delta_pixels =
            (pixel.x - impl_->transform_edit.start_pixel.x) + (impl_->transform_edit.start_pixel.y - pixel.y);
        const math::Quat delta_rotation =
            math::quat_from_axis_angle(impl_->transform_edit.axis_vector, delta_pixels * 0.01f);
        next.rotation = math::normalize(delta_rotation * impl_->transform_edit.initial.rotation);
    } else {
        const float delta_pixels =
            (pixel.x - impl_->transform_edit.start_pixel.x) * impl_->transform_edit.scale_screen_axis.x +
            (pixel.y - impl_->transform_edit.start_pixel.y) * impl_->transform_edit.scale_screen_axis.y;
        const float factor = std::max(0.01f, 1.0f + delta_pixels * 0.01f);
        const std::size_t axis_index = static_cast<std::size_t>(impl_->transform_edit.axis);
        next.scale[axis_index] = impl_->transform_edit.initial.scale[axis_index] * factor;
    }
    const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
    std::vector<Impl::EditorComponentValue> values;
    values.push_back(Impl::EditorComponentValue{impl_->transform_edit.entity, transform_type, bytes_of(next)});
    const Result queued = impl_->enqueue_values(values);
    if (!queued) {
        return queued;
    }
    impl_->transform_edit.current = next;
    impl_->transform_edit.command.after = values;
    return Result::success();
}

foundation::Result EditorSceneRuntime::commit_transform_edit() {
    if (impl_ == nullptr || !impl_->transform_edit.active) {
        return foundation::invalid_state("No Transform edit transaction is active");
    }
    if (impl_->transform_edit.command.after.empty()) {
        const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
        impl_->transform_edit.command.after.push_back(Impl::EditorComponentValue{
            impl_->transform_edit.entity, transform_type, bytes_of(impl_->transform_edit.current)});
    }
    const Result result = impl_->commit_history(std::move(impl_->transform_edit.command));
    if (!result) {
        return result;
    }
    impl_->transform_edit.active = false;
    impl_->refresh_dirty();
    return Result::success();
}

foundation::Result EditorSceneRuntime::cancel_transform_edit() {
    if (impl_ == nullptr || !impl_->transform_edit.active) {
        return foundation::invalid_state("No Transform edit transaction is active");
    }
    const ecs::ComponentTypeId transform_type(aeg::engine::components::kTransformName);
    std::vector<Impl::EditorComponentValue> values;
    values.push_back(Impl::EditorComponentValue{impl_->transform_edit.entity, transform_type,
                                                bytes_of(impl_->transform_edit.initial)});
    const Result queued = impl_->enqueue_values(values);
    if (!queued) {
        return queued;
    }
    impl_->transform_edit.active = false;
    impl_->refresh_dirty();
    return Result::success();
}

bool EditorSceneRuntime::transform_edit_active() const noexcept {
    return impl_ != nullptr && impl_->transform_edit.active;
}

foundation::Result EditorSceneRuntime::select_feature_module(const std::filesystem::path& manifest,
                                                                     std::string_view configuration,
                                                                     FeatureGraphLoadPolicy policy) {
    if (impl_ == nullptr || !impl_->initialized || !impl_->project_manager.loaded() || impl_->mode != Mode::kEdit)
        return foundation::invalid_state("Executable module selection requires an open project in Edit mode");
    if (policy != FeatureGraphLoadPolicy::kPreserveActive && policy != FeatureGraphLoadPolicy::kSavedStartup)
        return foundation::invalid_argument("Executable module graph load policy is invalid");
    if (impl_->pending_feature_session != nullptr ||
        (impl_->feature_session != nullptr &&
         (impl_->feature_session->host == nullptr || impl_->feature_session->host->has_pending())))
        return foundation::invalid_state("A Feature publication is already queued");
    const auto project_file = impl_->project_manager.project_file();
    const auto selected = tooling::ProjectOperationService::select_module(project_file, manifest, configuration);
    if (!selected) return selected.result;
    const auto project = project::ProjectFileCodec{}.load(project_file);
    if (!project) return project.result;
    const auto paths = project::ProjectPaths::resolve(project_file, project.value.config.paths);
    if (!paths) return paths.result;
    if (policy == FeatureGraphLoadPolicy::kSavedStartup && project.value.config.startup_feature_graphs.empty())
        return foundation::invalid_argument("Saved startup module selection requires at least one saved FeatureGraph root");
    std::vector<feature::FeatureGraphDocument> graphs;
    if (policy == FeatureGraphLoadPolicy::kPreserveActive &&
        impl_->feature_session != nullptr && impl_->feature_session->host->active() != nullptr) {
        graphs = impl_->feature_session->host->active_documents();
    } else {
        for (const auto& relative : project.value.config.startup_feature_graphs) {
            const auto path = paths.value.resolve_under(project::ProjectDirectory::kRoot, relative);
            if (!path) return path.result;
            const auto graph = authoring::load_feature_graph_document(path.value);
            if (!graph) return graph.result;
            graphs.push_back(graph.value);
        }
    }
    try {
        if (!impl_->clock_host) {
            const auto clock_host = feature::make_clock_host();
            if (!clock_host) return clock_host.result;
            impl_->clock_host = clock_host.value;
        }
        auto candidate = std::make_unique<Impl::FeatureSession>(*impl_->device, project_file, impl_->clock_host,
            impl_->feature_session ? impl_->feature_session->execution.get() : nullptr);
        if (const auto result = candidate->loader.load(selected.value, candidate->handle); !result) return result;
        auto definition = candidate->loader.make_feature_module(candidate->handle);
        if (!definition) return definition.result;
        if (const auto result = module::register_static_module(candidate->modules, std::move(definition.value)); !result)
            return result;
        if (const auto result = candidate->execution->enable_async_execution(); !result) return result;
        for (const auto& graph : graphs) {
            if (const auto result = candidate->host->stage(graph); !result) return result;
        }
        impl_->pending_feature_session = std::move(candidate);
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Scene Feature session allocation failed");
    }
}

foundation::Result EditorSceneRuntime::queue_feature_graph(const feature::FeatureGraphDocument& graph) {
    if (impl_ == nullptr || !impl_->initialized || impl_->feature_session == nullptr || impl_->feature_session->host == nullptr)
        return foundation::not_ready("Select an executable Feature module before Apply");
    if (impl_->pending_feature_session != nullptr || impl_->feature_session->host->has_pending())
        return foundation::invalid_state("A Feature publication is already queued");
    auto& host = *impl_->feature_session->host;
    auto graphs = host.active_documents();
    const auto previous = std::find_if(graphs.begin(), graphs.end(), [&](const auto& value) {
        return value.asset_uuid == graph.asset_uuid;
    });
    if (previous == graphs.end()) graphs.push_back(graph);
    else *previous = graph;
    if (!impl_->feature_world_bindings.empty()) {
        if (impl_->edit_scope == nullptr)
            return foundation::invalid_state("Editor Edit World is unavailable while validating a bound Apply candidate");
        for (const auto& binding : impl_->feature_world_bindings) {
            if (const auto result = validate_editor_world_binding_candidate(binding, graphs,
                    impl_->feature_session->registry, impl_->scene_asset, impl_->edit_instance,
                    *impl_->edit_scope); !result)
                return result;
        }
    }
    for (const auto& value : graphs) {
        if (const auto result = host.stage(value); !result) return result;
    }
    return Result::success();
}

foundation::ResultValue<runtime::FeatureRuntimeNodeSnapshot> EditorSceneRuntime::inspect_feature_node(
    std::string_view asset_uuid, feature::FeatureNodeId node) const {
    if (impl_ == nullptr || impl_->feature_session == nullptr || impl_->feature_session->host == nullptr)
        return {foundation::not_ready("Scene has no executable Feature graph"), {}};
    auto result = impl_->feature_session->host->inspect_node(asset_uuid, node);
    if (!result)
        return result;
    const auto authored = std::find_if(
        impl_->project_feature_authoring.feature_graphs.begin(),
        impl_->project_feature_authoring.feature_graphs.end(),
        [&](const auto& document) { return document.graph.asset_uuid == result.value.asset_uuid; });
    if (authored != impl_->project_feature_authoring.feature_graphs.end())
        result.value.source_path = authored->path;
    return result;
}

foundation::ResultValue<runtime::FeatureRuntimeExecutionSnapshot> EditorSceneRuntime::inspect_feature_execution(
    std::string_view asset_uuid) const {
    if (impl_ == nullptr || impl_->feature_session == nullptr || impl_->feature_session->host == nullptr)
        return {foundation::not_ready("Scene has no executable Feature graph"), {}};
    auto result = impl_->feature_session->host->inspect_execution(asset_uuid);
    if (!result) return result;
    const auto authored = std::find_if(impl_->project_feature_authoring.feature_graphs.begin(),
        impl_->project_feature_authoring.feature_graphs.end(),
        [&](const auto& document) { return document.graph.asset_uuid == result.value.asset_uuid; });
    if (authored != impl_->project_feature_authoring.feature_graphs.end()) result.value.source_path = authored->path;
    return result;
}

foundation::ResultValue<std::shared_ptr<const EditorRenderGraphSnapshot>> EditorSceneRuntime::inspect_render_graph() const {
    if (impl_ == nullptr || !impl_->initialized || !impl_->rendered_inspection)
        return {foundation::not_ready("Scene has no captured successful RenderGraph frame"), {}};
    return {foundation::Result::success(), impl_->rendered_inspection};
}

foundation::Result EditorSceneRuntime::publish_renderer_feature_graph(const feature::FeatureGraphDocument& graph) {
    if (impl_ == nullptr || !impl_->initialized || impl_->preview_renderer == nullptr)
        return foundation::invalid_state("Editor Renderer Feature preview is unavailable");
    if (impl_->mode != Mode::kEdit || impl_->transform_edit.active || impl_->component_edit_active ||
        impl_->frame_context->is_frame_active())
        return foundation::invalid_state("Renderer Feature publication requires an Edit frame boundary");
    return impl_->preview_renderer->publish(graph);
}

foundation::ResultValue<runtime::FeatureRuntimeExecutionSnapshot> EditorSceneRuntime::inspect_renderer_feature_execution() const {
    if (impl_ == nullptr || !impl_->initialized || impl_->preview_renderer == nullptr)
        return {foundation::invalid_state("Editor Renderer Feature preview is unavailable"), {}};
    return impl_->preview_renderer->inspect();
}

foundation::Result EditorSceneRuntime::control_feature_process(const runtime::RuntimeProcessCommand& command) {
    if (impl_ == nullptr || impl_->feature_session == nullptr || impl_->feature_session->host == nullptr)
        return foundation::not_ready("Scene has no executable Feature graph");
    return impl_->feature_session->host->control_process(command);
}

bool EditorSceneRuntime::feature_publication_pending() const noexcept {
    return impl_ != nullptr && (impl_->pending_feature_session != nullptr ||
        (impl_->feature_session != nullptr && impl_->feature_session->host != nullptr &&
         impl_->feature_session->host->has_pending()));
}

foundation::Result EditorSceneRuntime::render_frame(uint64_t frame_number, double elapsed_seconds) {
    if (impl_ == nullptr || !impl_->initialized || impl_->active_scope == nullptr || impl_->orchestrator == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not ready to render");
    }
    if (!std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0)
        return foundation::invalid_argument("Scene Feature delta must be finite and nonnegative");
    bool defer_world_bound_publication = false;
    if (impl_->pending_feature_session != nullptr) {
        auto& candidate = *impl_->pending_feature_session;
        if (candidate.host->has_pending()) {
            if (const auto result = candidate.host->apply_pending(); !result) return result;
        }
        if (impl_->feature_session != nullptr) {
            if (const auto result = impl_->feature_session->close(); !result) return result;
        }
        impl_->feature_session = std::move(impl_->pending_feature_session);
    }
    if (impl_->feature_session != nullptr) {
        auto& session = *impl_->feature_session;
        defer_world_bound_publication = impl_->world_command_consumer != nullptr &&
            impl_->mode != Mode::kEdit && session.host->has_pending();
        if (session.host->has_pending() && !defer_world_bound_publication) {
            if (const auto result = session.host->apply_pending(); !result) return result;
        }
        if (impl_->world_command_consumer != nullptr && impl_->mode != Mode::kEdit) {
            if (const auto result = impl_->world_command_consumer->set_frame_delta(elapsed_seconds); !result)
                return result;
        }
        // Pause and World-bound Apply retire issued work only; they cannot issue
        // deferred clocks, queued backlog, or zero-backoff retries.
        const auto result = impl_->mode == Mode::kPlay && !defer_world_bound_publication
            ? session.execution->advance(elapsed_seconds)
            : impl_->mode == Mode::kEdit ? session.execution->pump_completions()
                                         : feature::detail::FeatureExecutionControl::drain_issued_completions(
                                               *session.execution);
        if (!result) return result;
    }
    {
        if (impl_->preview_renderer == nullptr || impl_->preview_history == nullptr)
            return foundation::invalid_state("Editor Renderer Feature preview is unavailable");
        if (const auto result = impl_->preview_renderer->tick(); !result) return result;
    }
    if (const Result result = impl_->resize_targets(); !result) {
        return result;
    }
    impl_->current_slot = impl_->frame_context->frame_slot_index();
    engine::EngineFrameDesc desc{};
    desc.fixed_tick = frame_number;
    desc.render_output_extent = {impl_->requested_width, impl_->requested_height};
    desc.renderer = &impl_->renderer;
    desc.render_view_resolver = [impl = impl_.get()](const engine::EngineFrameRenderViewContext& context) {
        const ecs::EntityId camera =
            impl->mode == Mode::kEdit ? impl->edit_camera_runtime : impl->runtime_camera_runtime;
        auto view = aeg::engine::components::make_render_view(
            context.world_snapshot,
            aeg::engine::components::RuntimeRenderViewDesc{// Positive-height Vulkan viewports map negative NDC Y to the
                                                           // top pixel row. Picking/gizmo math retains its Y-up view.
                                                           camera,
                                                           0,
                                                           context.output_extent,
                                                           0xffffffffu,
                                                           kSceneColor,
                                                           kSceneDepth,
                                                           false,
                                                           {.y_down = true}});
        if (view) {
            view.value.color_target_final_usage = ResourceUsage::kSampledRead;
        }
        return view;
    };
    desc.render_resource_resolver = &impl_->resolver;
    // Asset uploads happen in RenderBuild preparation. Extraction therefore
    // accepts stable IDs first and the Resolver resolves fast-path/baked GPU
    // resources after preparation has had a chance to publish them.
    desc.require_resolved_render_resources = false;
    desc.user_data = &impl_->viewport_readback;
    desc.render_feature_preparation = [impl = impl_.get()](const engine::EngineFramePreparationContext& context) {
        return impl->prepare_frame(context);
    };
    std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> outputs;
    std::vector<feature::RenderResourceOwner> owners;
    std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> preview_outputs;
    std::vector<feature::RenderResourceOwner> preview_owners;
    {
        auto captured = impl_->preview_renderer->capture();
        if (!captured) return captured.result;
        auto captured_owners = impl_->preview_renderer->capture_owners();
        if (!captured_owners) return captured_owners.result;
        preview_outputs = std::move(captured.value);
        preview_owners = std::move(captured_owners.value);
    }
    auto* session = impl_->feature_session.get();
    if (session != nullptr && session->host->active()) {
        auto captured = session->execution->capture_render_outputs(*session->host->active());
        if (!captured) return captured.result;
        auto captured_owners = session->execution->capture_render_resource_owners(*session->host->active());
        if (!captured_owners) return captured_owners.result;
        outputs = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(captured.value));
        owners = std::move(captured_owners.value);
    }
    desc.render_feature_post_build = [impl = impl_.get(), session, outputs = std::move(outputs), owners = std::move(owners),
        preview_outputs = std::move(preview_outputs), preview_owners = std::move(preview_owners)]
        (const engine::EngineFramePostBuildContext& context) {
        const render_graph::RenderPassExecutionHost pass_host{&impl->scene_passes,
            pipeline::detail::RenderPassRegistryExecution::execution_epoch(impl->scene_passes)};
        if (preview_outputs) {
            if (const auto result = build_scene_feature_outputs(context, preview_outputs, preview_owners,
                                                               *impl->preview_history, pass_host); !result) return result;
        }
        if (outputs) {
            if (const auto result = build_scene_feature_outputs(context, outputs, owners, session->history, pass_host); !result)
                return result;
        }
        const auto color = std::find_if(context.feature_textures.begin(), context.feature_textures.end(),
                                       [](const auto& binding) { return binding.id == kSceneColor; });
        if (color == context.feature_textures.end()) return foundation::invalid_state("SceneColor frame binding is unavailable");
        return build_viewport_readback(context.render_graph,
                                      {color->texture.index, color->texture.generation, color->texture.graph_id},
                                      impl->viewport_readback);
    };
    const Result frame_result = impl_->orchestrator->run_frame(desc);
    if (!frame_result) {
        impl_->viewport_readback.requested = false;
        impl_->viewport_readback.allocation = {};
        if (frame_result.code == ResultCode::kNotReady && impl_->world_command_consumer != nullptr &&
            impl_->world_command_consumer->observation_waiting())
            return frame_result;
        return frame_result;
    }
    const auto viewport_state = query_texture_initial_state(*impl_->device, impl_->color_texture);
    if (!viewport_state) {
        return viewport_state.result;
    }
    impl_->viewport_export_state = viewport_state.value;
    if (impl_->viewport_readback.requested) {
        if (!impl_->viewport_readback.allocation.is_valid()) {
            impl_->viewport_readback.requested = false;
            return foundation::invalid_state("Viewport readback was requested but not prepared");
        }
        if (!impl_->viewport_readback.pass_built || !impl_->viewport_readback.pass_executed) {
            impl_->diagnostic = "Viewport readback pass was not built or executed";
        }
        if (const Result wait_result = impl_->device->wait_idle(); !wait_result) {
            impl_->viewport_readback.requested = false;
            return wait_result;
        }
        std::vector<std::byte> pixels(static_cast<std::size_t>(impl_->viewport_readback.allocation.size));
        if (const Result read_result = impl_->frame_context->read_readback(impl_->viewport_readback.allocation, pixels);
            !read_result) {
            impl_->viewport_readback.requested = false;
            return read_result;
        }
        impl_->viewport_readback.pixels = std::move(pixels);
        impl_->viewport_readback.requested = false;
        impl_->viewport_readback.allocation = {};
    }
    std::vector<runtime::FeatureRuntimeExecutionSnapshot> sources;
    bool sources_valid = true;
    {
        auto source = impl_->preview_renderer->inspect();
        if (!source) sources_valid = false;
        else sources.push_back(std::move(source.value));
    }
    if (impl_->feature_session && impl_->feature_session->host) {
        for (const auto& document : impl_->feature_session->host->active_documents()) {
            auto source = inspect_feature_execution(document.asset_uuid);
            if (!source) { sources_valid = false; break; }
            sources.push_back(std::move(source.value));
        }
    }
    if (sources_valid) {
        auto captured = capture_editor_render_graph(*impl_->render_graph, impl_->frame_context->frame_number(), std::move(sources));
        if (captured) impl_->rendered_inspection = std::move(captured.value);
    }
    if (defer_world_bound_publication && impl_->feature_session != nullptr) {
        auto& session = *impl_->feature_session;
        for (const auto& binding : impl_->feature_world_bindings) {
            const auto issued = editor_world_binding_has_issued_work(binding, *session.host,
                impl_->scene_asset, impl_->runtime_instance, *impl_->runtime_scope);
            if (!issued) return issued.result;
            if (issued.value) return frame_result;
        }
        if (const auto result = session.host->apply_pending(); !result) return result;
        auto created = make_editor_world_command_consumer(*session.execution, impl_->reflection_registry,
            *session.host, impl_->feature_world_bindings, impl_->scene_asset, impl_->runtime_instance,
            *impl_->runtime_scope);
        if (!created) {
            impl_->orchestrator.reset();
            impl_->world_command_consumer.reset();
            return created.result;
        }
        if (const auto result = created.value->set_frame_delta(elapsed_seconds); !result) {
            impl_->orchestrator.reset();
            impl_->world_command_consumer.reset();
            return result;
        }
        auto retired_consumer = std::move(impl_->world_command_consumer);
        if (const auto result = engine::detail::EngineFrameWorldCommands::attach(*impl_->orchestrator,
                *created.value); !result) {
            impl_->orchestrator.reset();
            retired_consumer.reset();
            return result;
        }
        impl_->world_command_consumer = std::move(created.value);
        retired_consumer.reset();
    }
    return frame_result;
}

std::span<const scene::SceneEntityRecord> EditorSceneRuntime::entities() const noexcept {
    return impl_ == nullptr ? std::span<const scene::SceneEntityRecord>{} : impl_->scene_asset.entities();
}

foundation::Result EditorSceneRuntime::select(scene::SceneEntityId entity, SelectionOperation operation) {
    if (impl_ == nullptr || !entity.is_valid()) {
        return foundation::invalid_argument("Editor selection identity is invalid");
    }
    const auto found = std::find_if(impl_->scene_asset.entities().begin(), impl_->scene_asset.entities().end(),
                                    [entity](const auto& candidate) { return candidate.id == entity; });
    if (found == impl_->scene_asset.entities().end()) {
        return foundation::not_found("Editor selection is not present in the Scene Asset");
    }
    const auto selected = std::find(impl_->selected_entities.begin(), impl_->selected_entities.end(), entity);
    if (operation == SelectionOperation::kReplace) {
        impl_->selected_entities.clear();
        impl_->selected_entities.push_back(entity);
    } else if (operation == SelectionOperation::kAdd) {
        if (selected == impl_->selected_entities.end()) {
            impl_->selected_entities.push_back(entity);
        }
    } else if (selected == impl_->selected_entities.end()) {
        impl_->selected_entities.push_back(entity);
    } else {
        impl_->selected_entities.erase(selected);
    }
    impl_->selected_entity =
        impl_->selected_entities.empty() ? scene::SceneEntityId{} : impl_->selected_entities.front();
    return Result::success();
}

foundation::ResultValue<std::vector<scene::SceneEntityId>> EditorSceneRuntime::children(
    scene::SceneEntityId parent) const {
    if (impl_ == nullptr) {
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    }
    return impl_->scene_asset.children(parent);
}

foundation::ResultValue<std::string> EditorSceneRuntime::hierarchy_path(scene::SceneEntityId entity) const {
    if (impl_ == nullptr || !entity.is_valid()) {
        return {foundation::invalid_argument("Editor hierarchy entity identity is invalid"), {}};
    }
    std::vector<std::string_view> names;
    scene::SceneEntityId current = entity;
    try {
        while (current.is_valid()) {
            const auto found = std::find_if(impl_->scene_asset.entities().begin(), impl_->scene_asset.entities().end(),
                                            [current](const auto& value) { return value.id == current; });
            if (found == impl_->scene_asset.entities().end()) {
                return {foundation::not_found("Editor hierarchy entity is not present in the Scene Asset"), {}};
            }
            names.push_back(found->name);
            current = found->parent;
            if (names.size() > impl_->scene_asset.entities().size()) {
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
    if (mode != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Hierarchy editing is only available in Edit mode");
    }
    if (transform_edit.active) {
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
        command.before_scene = scene_asset;
        scene::SceneAsset after_scene = scene_asset;
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
                    const auto record = std::find_if(scene_asset.entities().begin(), scene_asset.entities().end(),
                                                     [current](const auto& value) { return value.id == current; });
                    if (record == scene_asset.entities().end()) {
                        return {foundation::not_found("Editor hierarchy transform entity is not present"), {}};
                    }
                    stack.push_back(current);
                    Transform local{};
                    if (const void* value =
                            component_value_in(edit_instance, edit_state.get(), current, transform_type);
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
                const void* current_value = component_value_in(edit_instance, edit_state.get(), entity, transform_type);
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
                if (!math::try_decompose_affine_transform(inverse_parent * old_world.value, next)) {
                    return foundation::invalid_state(
                        "Hierarchy reparent would produce a non-representable or degenerate Transform");
                }
                command.before.push_back(EditorComponentValue{entity, transform_type,
                                                              bytes_of(*static_cast<const Transform*>(current_value))});
                command.after.push_back(EditorComponentValue{entity, transform_type, bytes_of(next)});
            }
        }
        history.reserve(history.size() + 1u);
        if (!command.after.empty()) {
            if (const Result result = enqueue_values(command.after); !result) {
                return result;
            }
        }
        scene_asset = std::move(after_scene);
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Hierarchy reparent transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::reparent_entities(std::span<const scene::SceneEntityId> entities,
                                                         scene::SceneEntityId parent, std::size_t sibling_index,
                                                         HierarchyTransformPolicy transform_policy) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->reparent_entities(entities, parent, sibling_index, transform_policy);
}

foundation::Result EditorSceneRuntime::Impl::create_scene_entity(scene::SceneEntityId parent, std::string_view name) {
    if (mode != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Scene entity creation is only available in Edit mode");
    }
    if (parent.is_valid() &&
        std::find_if(scene_asset.entities().begin(), scene_asset.entities().end(),
                     [parent](const auto& entity) { return entity.id == parent; }) == scene_asset.entities().end()) {
        return foundation::not_found("Scene entity creation parent is not present");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset;
        scene::SceneAsset after_scene = scene_asset;
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
                id, ecs::ComponentTypeId(aeg::engine::components::kTransformName), reflection_registry,
                reflection::TypeId(aeg::engine::components::kTransformName), &transform);
            !result) {
            return result;
        }
        command.after_scene = after_scene;
        command.rebuild_edit_world = true;
        history.reserve(history.size() + 1u);
        scene_asset = std::move(after_scene);
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset = *command.before_scene;
            static_cast<void>(rebuild_edit_world());
            return result;
        }
        selected_entities.clear();
        selected_entities.push_back(id);
        selected_entity = id;
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Scene entity creation transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::create_scene_entity(scene::SceneEntityId parent, std::string_view name) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->create_scene_entity(parent, name);
}

foundation::Result EditorSceneRuntime::Impl::rename_entity(scene::SceneEntityId entity, std::string_view name) {
    if (mode != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Entity name editing is only available in Edit mode");
    }
    if (component_edit_active || transform_edit.active) {
        return foundation::invalid_state("Entity metadata editing cannot overlap another editor transaction");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset;
        scene::SceneAsset after_scene = scene_asset;
        if (const Result result = after_scene.set_entity_name(entity, std::string(name)); !result) {
            return result;
        }
        command.after_scene = std::move(after_scene);
        history.reserve(history.size() + 1u);
        scene_asset = *command.after_scene;
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Entity name edit transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::rename_entity(scene::SceneEntityId entity, std::string_view name) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->rename_entity(entity, name);
}

foundation::Result EditorSceneRuntime::Impl::set_entity_tag(scene::SceneEntityId entity, std::string_view tag) {
    if (mode != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Entity tag editing is only available in Edit mode");
    }
    if (component_edit_active || transform_edit.active) {
        return foundation::invalid_state("Entity metadata editing cannot overlap another editor transaction");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset;
        scene::SceneAsset after_scene = scene_asset;
        if (const Result result = after_scene.set_entity_tag(entity, std::string(tag)); !result) {
            return result;
        }
        command.after_scene = std::move(after_scene);
        history.reserve(history.size() + 1u);
        scene_asset = *command.after_scene;
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Entity tag edit transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::set_entity_tag(scene::SceneEntityId entity, std::string_view tag) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->set_entity_tag(entity, tag);
}

foundation::Result EditorSceneRuntime::Impl::add_component(std::span<const scene::SceneEntityId> entities,
                                                           ecs::ComponentTypeId component) {
    if (mode != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Component addition is only available in Edit mode");
    }
    if (!component.is_valid() || entities.empty()) {
        return foundation::invalid_argument("Component addition input is invalid");
    }
    if (component_edit_active || transform_edit.active) {
        return foundation::invalid_state("Component addition overlaps another editor transaction");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset;
        scene::SceneAsset after_scene = scene_asset;
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
                        entity, component, reflection_registry,
                        reflection::TypeId(aeg::engine::components::kTransformName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kCameraName)) {
                Camera value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry,
                        reflection::TypeId(aeg::engine::components::kCameraName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName)) {
                MeshRenderer value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry,
                        reflection::TypeId(aeg::engine::components::kMeshRendererName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kTextRendererName)) {
                TextRenderer value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry,
                        reflection::TypeId(aeg::engine::components::kTextRendererName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kDirectionalLightName)) {
                DirectionalLight value{};
                if (const Result result = after_scene.add_reflected_component(
                        entity, component, reflection_registry,
                        reflection::TypeId(aeg::engine::components::kDirectionalLightName), &value);
                    !result) {
                    return result;
                }
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kPointLightName)) {
                PointLight value{};
                if (const auto result = after_scene.add_reflected_component(
                        entity, component, reflection_registry,
                        reflection::TypeId(aeg::engine::components::kPointLightName), &value);
                    !result)
                    return result;
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kSpotLightName)) {
                SpotLight value{};
                if (const auto result = after_scene.add_reflected_component(
                        entity, component, reflection_registry,
                        reflection::TypeId(aeg::engine::components::kSpotLightName), &value);
                    !result)
                    return result;
            } else if (component == ecs::ComponentTypeId(aeg::engine::components::kSceneEnvironmentName)) {
                SceneEnvironment value{};
                if (const auto result = after_scene.add_reflected_component(
                        entity, component, reflection_registry,
                        reflection::TypeId(aeg::engine::components::kSceneEnvironmentName), &value);
                    !result)
                    return result;
            } else {
                return foundation::unsupported("Component schema is not supported by the Inspector");
            }
        }
        command.after_scene = std::move(after_scene);
        command.rebuild_edit_world = true;
        history.reserve(history.size() + 1u);
        scene_asset = *command.after_scene;
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset = *command.before_scene;
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
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->add_component(entities, component);
}

foundation::Result EditorSceneRuntime::Impl::remove_component(std::span<const scene::SceneEntityId> entities,
                                                              ecs::ComponentTypeId component) {
    if (mode != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Component removal is only available in Edit mode");
    }
    if (!component.is_valid() || entities.empty()) {
        return foundation::invalid_argument("Component removal input is invalid");
    }
    if (component_edit_active || transform_edit.active) {
        return foundation::invalid_state("Component removal overlaps another editor transaction");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset;
        scene::SceneAsset after_scene = scene_asset;
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
        history.reserve(history.size() + 1u);
        scene_asset = *command.after_scene;
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset = *command.before_scene;
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
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->remove_component(entities, component);
}

foundation::Result EditorSceneRuntime::Impl::duplicate_entities(std::span<const scene::SceneEntityId> entities) {
    if (mode != EditorSceneRuntime::Mode::kEdit) {
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
        command.before_scene = scene_asset;
        scene::SceneAsset after_scene = scene_asset;
        std::unordered_set<scene::SceneEntityId, scene::Guid128Hash<scene::SceneEntityIdTag>> source_set;
        source_set.reserve(entities.size());
        for (const scene::SceneEntityId entity : entities) {
            if (!entity.is_valid() || std::find_if(scene_asset.entities().begin(), scene_asset.entities().end(),
                                                   [entity](const auto& value) { return value.id == entity; }) ==
                                          scene_asset.entities().end()) {
                return foundation::not_found("Scene duplicate source is not present in the asset");
            }
            if (!source_set.emplace(entity).second) {
                return foundation::invalid_argument("Scene duplicate sources contain a duplicate entity");
            }
        }
        for (const scene::SceneEntityId source : entities) {
            scene::SceneEntityId current = source;
            while (current.is_valid()) {
                const auto found = std::find_if(scene_asset.entities().begin(), scene_asset.entities().end(),
                                                [current](const auto& value) { return value.id == current; });
                if (found == scene_asset.entities().end()) {
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
        for (const auto& entity : scene_asset.entities()) {
            bool in_subtree = source_set.contains(entity.id);
            if (!in_subtree) {
                scene::SceneEntityId current = entity.parent;
                while (current.is_valid()) {
                    if (source_set.contains(current)) {
                        in_subtree = true;
                        break;
                    }
                    const auto parent = std::find_if(scene_asset.entities().begin(), scene_asset.entities().end(),
                                                     [current](const auto& value) { return value.id == current; });
                    if (parent == scene_asset.entities().end()) {
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
        for (const auto& component : scene_asset.components()) {
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
        history.reserve(history.size() + 1u);
        scene_asset = std::move(after_scene);
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset = *command.before_scene;
            static_cast<void>(rebuild_edit_world());
            return result;
        }
        selected_entities = std::move(duplicated);
        selected_entity = selected_entities.empty() ? scene::SceneEntityId{} : selected_entities.front();
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Scene duplicate transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::duplicate_entities(std::span<const scene::SceneEntityId> entities) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->duplicate_entities(entities);
}

foundation::Result EditorSceneRuntime::Impl::remove_entities(std::span<const scene::SceneEntityId> entities,
                                                             scene::SceneEntityRemovalPolicy policy) {
    if (mode != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Scene entity removal is only available in Edit mode");
    }
    if (std::find(entities.begin(), entities.end(), active_camera_scene) != entities.end()) {
        return foundation::invalid_state("The active editor camera cannot be removed");
    }
    if (const Result result = commit_edit_commands(); !result) {
        return result;
    }
    try {
        EditorCommand command{};
        command.before_scene = scene_asset;
        scene::SceneAsset after_scene = scene_asset;
        if (const Result result = after_scene.remove_entities(entities, policy); !result) {
            return result;
        }
        command.after_scene = after_scene;
        command.rebuild_edit_world = true;
        history.reserve(history.size() + 1u);
        scene_asset = std::move(after_scene);
        if (const Result result = rebuild_edit_world(); !result) {
            scene_asset = *command.before_scene;
            static_cast<void>(rebuild_edit_world());
            return result;
        }
        selected_entities.erase(
            std::remove_if(selected_entities.begin(), selected_entities.end(),
                           [this](scene::SceneEntityId id) {
                               return std::find_if(scene_asset.entities().begin(), scene_asset.entities().end(),
                                                   [id](const auto& entity) { return entity.id == id; }) ==
                                      scene_asset.entities().end();
                           }),
            selected_entities.end());
        selected_entity = selected_entities.empty() ? scene::SceneEntityId{} : selected_entities.front();
        return commit_history(std::move(command));
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Scene entity removal transaction allocation failed");
    }
}

foundation::Result EditorSceneRuntime::remove_entities(std::span<const scene::SceneEntityId> entities,
                                                       scene::SceneEntityRemovalPolicy policy) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->remove_entities(entities, policy);
}

foundation::Result EditorSceneRuntime::Impl::drop_resource(EditorResourceKind kind, uint64_t id) {
    if (mode != EditorSceneRuntime::Mode::kEdit) {
        return foundation::invalid_state("Resource binding is only available in Edit mode");
    }
    if (kind != EditorResourceKind::kMesh && kind != EditorResourceKind::kMaterial) {
        return foundation::invalid_argument("Only Mesh and Material assets can be dropped into the Viewport");
    }
    const EditorAssetKind asset_kind =
        kind == EditorResourceKind::kMesh ? EditorAssetKind::kMesh : EditorAssetKind::kMaterial;
    const bool built_in = kind == EditorResourceKind::kMesh ? id == kMeshAssetA || id == kMeshAssetB
                                                            : id == kMaterialAssetRed || id == kMaterialAssetGreen;
    if (!built_in && !asset_database.is_valid(id, asset_kind)) {
        return foundation::not_found("Dropped Asset is missing or has failed import");
    }
    if (transform_edit.active || component_edit_active) {
        return foundation::invalid_state("Resource binding overlaps another editor transaction");
    }

    std::vector<scene::SceneEntityId> targets(selected_entities.begin(), selected_entities.end());
    if (targets.empty()) {
        std::string name = kind == EditorResourceKind::kMesh ? "Mesh Entity" : "Material Entity";
        if (const auto* asset = asset_database.find(id); asset != nullptr) {
            name = asset->display_name;
        }
        if (const Result result = create_scene_entity({}, name); !result) {
            return result;
        }
        targets.push_back(selected_entity);
    }

    const ecs::ComponentTypeId mesh_type(aeg::engine::components::kMeshRendererName);
    for (const scene::SceneEntityId entity : targets) {
        if (component_value_in(edit_instance, edit_state.get(), entity, mesh_type) == nullptr) {
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
                component_value_in(edit_instance, edit_state.get(), entity, mesh_type));
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
        history.reserve(history.size() + 1u);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Resource binding history allocation failed");
    }
    return commit_history(std::move(command));
}

foundation::Result EditorSceneRuntime::drop_resource(EditorResourceKind kind, uint64_t id) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    return impl_->drop_resource(kind, id);
}

foundation::ResultValue<ecs::EntityId> EditorSceneRuntime::Impl::runtime_entity_in(const scene::SceneInstance& instance,
                                                                                   scene::SceneEntityId entity) const {
    return instance.runtime_entity(entity);
}

foundation::ResultValue<ecs::EntityId> EditorSceneRuntime::runtime_entity(scene::SceneEntityId entity) const {
    if (impl_ == nullptr || impl_->edit_scope == nullptr) {
        return {foundation::invalid_state("Editor edit World is not initialized"), {}};
    }
    return impl_->runtime_entity_in(impl_->edit_instance, entity);
}

const ecs::World* EditorSceneRuntime::edit_world() const noexcept {
    return impl_ == nullptr || impl_->edit_state == nullptr ? nullptr : impl_->edit_state.get();
}

foundation::ResultValue<ecs::ComponentDescriptor> EditorSceneRuntime::component_descriptor(
    ecs::ComponentTypeId type) const {
    if (impl_ == nullptr || impl_->edit_state == nullptr) {
        return {foundation::invalid_state("Editor edit World is not initialized"), {}};
    }
    return impl_->edit_state->find_component_descriptor(type);
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
                            : impl_->component_value_in(impl_->edit_instance, impl_->edit_state.get(), entity, type);
}

std::vector<ecs::ComponentTypeId> EditorSceneRuntime::selected_component_types() const {
    std::vector<ecs::ComponentTypeId> result;
    if (impl_ == nullptr || impl_->edit_state == nullptr || impl_->selected_entities.empty()) {
        return result;
    }
    const auto runtime = impl_->edit_instance.runtime_entity(impl_->selected_entities.front());
    if (!runtime) {
        return result;
    }
    const auto snapshot = impl_->edit_scope->latest_snapshot();
    if (!snapshot) {
        return result;
    }
    for (std::size_t index = 0; index < snapshot.value->component_type_count(); ++index) {
        const ecs::ComponentTypeId type = snapshot.value->component_type_at(index);
        bool common = true;
        for (const scene::SceneEntityId entity : impl_->selected_entities) {
            const auto selected_runtime = impl_->edit_instance.runtime_entity(entity);
            if (!selected_runtime || !impl_->edit_state->get_component_data(selected_runtime.value, type)) {
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

std::span<const EditorAssetRecord> EditorSceneRuntime::asset_records() const noexcept {
    return impl_ == nullptr ? std::span<const EditorAssetRecord>{} : impl_->asset_database.assets();
}

std::vector<EditorResourceDescriptor> EditorSceneRuntime::resource_descriptors() const {
    std::vector<EditorResourceDescriptor> result{};
    if (impl_ == nullptr || !impl_->project_manager.loaded()) {
        result = {
            {EditorResourceKind::kMesh, kMeshAssetA, "Mesh A", true, EditorAssetStatus::kImported, {}},
            {EditorResourceKind::kMesh, kMeshAssetB, "Mesh B", true, EditorAssetStatus::kImported, {}},
            {EditorResourceKind::kMaterial, kMaterialAssetRed, "Material Red", true, EditorAssetStatus::kImported, {}},
            {EditorResourceKind::kMaterial,
             kMaterialAssetGreen,
             "Material Green",
             true,
             EditorAssetStatus::kImported,
             {}},
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
    for (const auto& asset : impl_->asset_database.assets()) {
        result.push_back({map_kind(asset.kind), asset.id, asset.display_name,
                          asset.status == EditorAssetStatus::kImported, asset.status, asset.relative_path});
    }
    return result;
}

bool EditorSceneRuntime::is_resource_valid(EditorResourceKind kind, uint64_t id) const noexcept {
    if (kind == EditorResourceKind::kMesh) {
        return id == kMeshAssetA || id == kMeshAssetB ||
               (impl_ != nullptr && impl_->asset_database.is_valid(id, EditorAssetKind::kMesh));
    }
    if (kind == EditorResourceKind::kMaterial) {
        return id == kMaterialAssetRed || id == kMaterialAssetGreen ||
               (impl_ != nullptr && impl_->asset_database.is_valid(id, EditorAssetKind::kMaterial));
    }
    if (kind == EditorResourceKind::kTexture) {
        return impl_ != nullptr && impl_->asset_database.is_valid(id, EditorAssetKind::kTexture);
    }
    if (kind == EditorResourceKind::kScene) {
        return impl_ != nullptr && impl_->asset_database.is_valid(id, EditorAssetKind::kScene);
    }
    if (kind == EditorResourceKind::kFont) {
        return impl_ != nullptr && impl_->asset_database.is_valid(id, EditorAssetKind::kFont);
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
    if (!begin || !impl_->component_edit_active) {
        return begin;
    }
    return commit_component_edit();
}

foundation::Result EditorSceneRuntime::begin_component_edit(std::span<const ComponentUpdateView> updates) {
    if (impl_ == nullptr || impl_->edit_scope == nullptr || impl_->edit_state == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    if (impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Component editing is only available in Edit mode");
    }
    if (impl_->transform_edit.active || impl_->component_edit_active) {
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
            const auto runtime = impl_->edit_instance.runtime_entity(update.entity);
            if (!runtime) {
                return runtime.result;
            }
            const auto descriptor = impl_->edit_state->find_component_descriptor(update.component);
            if (!descriptor) {
                return descriptor.result;
            }
            if (update.bytes.size() != descriptor.value.size) {
                return foundation::invalid_argument("Editor component payload size does not match ECS storage");
            }
            if (const Result result = validate_component_payload(update.component, update.bytes); !result) {
                return result;
            }
            const auto current = impl_->edit_state->get_component_data(runtime.value, update.component);
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
        impl_->history.reserve(impl_->history.size() + 1u);
        if (const Result result = impl_->enqueue_values(command.after); !result) {
            return result;
        }
        impl_->component_edit_command = std::move(command);
        impl_->component_edit_active = true;
        impl_->refresh_dirty();
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor component edit allocation failed");
    }
}

foundation::Result EditorSceneRuntime::update_component_edit(std::span<const ComponentUpdateView> updates) {
    if (impl_ == nullptr || !impl_->component_edit_active) {
        return foundation::invalid_state("No Inspector component edit transaction is active");
    }
    if (updates.size() != impl_->component_edit_command.after.size()) {
        return foundation::invalid_argument("Inspector component edit field count changed during a transaction");
    }
    try {
        std::vector<Impl::EditorComponentValue> next;
        next.reserve(updates.size());
        for (std::size_t index = 0; index < updates.size(); ++index) {
            const ComponentUpdateView& update = updates[index];
            const Impl::EditorComponentValue& original = impl_->component_edit_command.after[index];
            if (update.entity != original.entity || update.component != original.component) {
                return foundation::invalid_argument("Inspector component edit fields changed during a transaction");
            }
            const auto descriptor = impl_->edit_state->find_component_descriptor(update.component);
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
            if (next[index].bytes != impl_->component_edit_command.after[index].bytes) {
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
        impl_->component_edit_command.after = std::move(next);
        impl_->refresh_dirty();
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor component edit update allocation failed");
    }
}

foundation::Result EditorSceneRuntime::commit_component_edit() {
    if (impl_ == nullptr || !impl_->component_edit_active) {
        return foundation::invalid_state("No Inspector component edit transaction is active");
    }
    Impl::EditorCommand command = std::move(impl_->component_edit_command);
    impl_->component_edit_command = {};
    impl_->component_edit_active = false;
    const Result result = impl_->commit_history(std::move(command));
    impl_->refresh_dirty();
    return result;
}

foundation::Result EditorSceneRuntime::cancel_component_edit() {
    if (impl_ == nullptr || !impl_->component_edit_active) {
        return foundation::invalid_state("No Inspector component edit transaction is active");
    }
    const Result queued = impl_->enqueue_values(impl_->component_edit_command.before);
    if (!queued) {
        return queued;
    }
    impl_->component_edit_command = {};
    impl_->component_edit_active = false;
    impl_->refresh_dirty();
    return Result::success();
}

bool EditorSceneRuntime::component_edit_active() const noexcept {
    return impl_ != nullptr && impl_->component_edit_active;
}

foundation::Result EditorSceneRuntime::new_project(const std::filesystem::path& root, std::string_view name) {
    return new_project(ProjectCreateOptions{root, std::string(name)});
}

foundation::Result EditorSceneRuntime::new_project(const ProjectCreateOptions& options) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit || impl_->transform_edit.active || impl_->component_edit_active) {
        return foundation::invalid_state("New Project requires Edit mode without an active edit transaction");
    }
    if (const auto result = reject_retired_scene_authoring(options.root / project::ProjectConfig{}.startup_scene);
        !result) return result;
    EditorProjectManager candidate;
    if (const auto result = candidate.new_project(options); !result) return result;
    if (const auto result = impl_->close_features(); !result) return result;
    impl_->project_manager = std::move(candidate);
    impl_->rendered_inspection.reset();
    if (const Result result = impl_->asset_database.initialize(impl_->project_manager.descriptor().root); !result) {
        static_cast<void>(impl_->project_manager.close_project());
        return result;
    }
    if (const Result result = impl_->reload_project_feature_authoring(); !result) {
        static_cast<void>(impl_->project_manager.close_project());
        return result;
    }
    if (const Result result = impl_->synchronize_asset_pipeline(); !result) {
        static_cast<void>(impl_->project_manager.close_project());
        return result;
    }
    if (const Result result = impl_->reset_scene_for_project(); !result) {
        return result;
    }
    if (const auto result = save_scene(); !result)
        return result;
    return restore_default_renderer_feature();
}

foundation::Result EditorSceneRuntime::update_project_settings(const project::ProjectConfig& config) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit || impl_->transform_edit.active || impl_->component_edit_active) {
        return foundation::invalid_state("Project settings require Edit mode without an active edit transaction");
    }
    if (const auto result = impl_->project_manager.update_project_settings(config); !result)
        return result;
    // Project settings can change startup FeatureGraph roots while the Editor
    // remains open. Refresh the source catalog immediately so Runtime Inspect
    // can continue to map live owners back to their authored .aegfeature file.
    return impl_->reload_project_feature_authoring();
}

foundation::Result EditorSceneRuntime::import_project(const std::filesystem::path& root_or_project_file) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit || impl_->transform_edit.active || impl_->component_edit_active) {
        return foundation::invalid_state("Import Project requires Edit mode without an active edit transaction");
    }
    EditorProjectManager candidate;
    if (const auto result = candidate.import_project(root_or_project_file); !result) return result;
    for (const auto& path : {candidate.recent_scene(), candidate.default_scene_file()}) {
        if (const auto result = reject_retired_scene_authoring(path); !result) return result;
    }
    if (const auto result = impl_->close_features(); !result) return result;
    impl_->project_manager = std::move(candidate);
    impl_->rendered_inspection.reset();
    if (const Result result = impl_->asset_database.initialize(impl_->project_manager.descriptor().root); !result) {
        static_cast<void>(impl_->project_manager.close_project());
        return result;
    }
    if (const Result result = impl_->reload_project_feature_authoring(); !result) {
        static_cast<void>(impl_->project_manager.close_project());
        return result;
    }
    if (const Result result = impl_->synchronize_asset_pipeline(); !result) {
        static_cast<void>(impl_->project_manager.close_project());
        return result;
    }
    const std::filesystem::path remembered_scene = impl_->project_manager.recent_scene();
    const std::filesystem::path default_scene = impl_->project_manager.default_scene_file();
    const auto open_with_renderer = [&](const std::filesystem::path& path) {
        if (const auto result = open_scene(path); !result)
            return result;
        return restore_default_renderer_feature();
    };
    if (!remembered_scene.empty()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(remembered_scene, error);
        if (error) {
            return foundation::invalid_state("Could not inspect the recent project scene: " + error.message());
        }
        if (exists) {
            return open_with_renderer(remembered_scene);
        }
    }
    {
        std::error_code error;
        const bool exists = std::filesystem::exists(default_scene, error);
        if (error) {
            return foundation::invalid_state("Could not inspect the default project scene: " + error.message());
        }
        if (exists) {
            return open_with_renderer(default_scene);
        }
    }
    if (const Result result = impl_->reset_scene_for_project(); !result) {
        return result;
    }
    if (const auto result = save_scene(); !result)
        return result;
    return restore_default_renderer_feature();
}

foundation::Result EditorSceneRuntime::close_project() {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (!impl_->project_manager.loaded()) {
        return Result::success();
    }
    if (impl_->mode != Mode::kEdit || impl_->transform_edit.active || impl_->component_edit_active) {
        return foundation::invalid_state("Close Project requires Edit mode without an active edit transaction");
    }
    if (const auto result = impl_->close_features(); !result) return result;
    if (const Result result = impl_->project_manager.close_project(); !result) {
        return result;
    }
    impl_->asset_database = EditorAssetDatabase{};
    impl_->project_feature_authoring = {};
    impl_->rendered_inspection.reset();
    if (const Result result = impl_->synchronize_asset_pipeline(); !result) {
        return result;
    }
    if (const Result result = impl_->reset_scene_for_project(); !result) {
        return result;
    }
    if (const Result result = save_scene(); !result) {
        return result;
    }
    return restore_default_renderer_feature();
}

foundation::Result EditorSceneRuntime::new_scene() {
    if (impl_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->component_edit_active) {
        if (const Result result = cancel_component_edit(); !result) {
            return result;
        }
    }
    if (impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("New Scene requires Edit mode");
    }
    const Result scene_result =
        impl_->project_manager.loaded() ? impl_->make_empty_project_scene() : impl_->make_default_scene();
    if (!scene_result) {
        return scene_result;
    }
    if (!impl_->project_manager.loaded()) {
        impl_->active_camera_scene = kCameraEntity;
    }
    impl_->transform_edit = {};
    impl_->selected_entity = impl_->project_manager.loaded() ? scene::SceneEntityId{} : kLeftMeshEntity;
    impl_->selected_entities.clear();
    if (impl_->selected_entity.is_valid()) {
        impl_->selected_entities.push_back(impl_->selected_entity);
    }
    impl_->mode = Mode::kEdit;
    impl_->runtime_scope.reset();
    impl_->runtime_state.reset();
    impl_->active_scope = nullptr;
    const Result result = impl_->rebuild_edit_world();
    if (result) {
        impl_->reset_history(true);
        impl_->feature_world_bindings.clear();
        impl_->scene_path = impl_->project_manager.loaded() ? impl_->project_manager.default_scene_file()
                                                            : std::filesystem::path{"editor_scene.aegscene"};
    }
    return result;
}

foundation::Result EditorSceneRuntime::open_scene(const std::filesystem::path& path) {
    if (impl_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Open Scene requires Edit mode");
    }
    std::filesystem::path resolved_path = path;
    if (impl_->project_manager.loaded()) {
        const auto resolved = impl_->project_manager.resolve_scene_path(path);
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
    if (const auto result = reject_retired_scene_authoring(resolved_path); !result) return result;
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
    auto loaded_bindings = read_editor_world_bindings(resolved_path, asset.value.id());
    if (!loaded_bindings) return loaded_bindings.result;
    if (const auto result = validate_editor_world_binding_targets(loaded_bindings.value, asset.value); !result)
        return result;
    const scene::SceneAsset previous_asset = impl_->scene_asset;
    const std::filesystem::path previous_path = impl_->scene_path;
    const std::vector<EditorFeatureWorldBinding> previous_bindings = impl_->feature_world_bindings;
    const std::vector<scene::SceneEntityId> previous_selection = impl_->selected_entities;
    const scene::SceneEntityId previous_selected_entity = impl_->selected_entity;
    const scene::SceneEntityId previous_camera = impl_->active_camera_scene;
    if (impl_->component_edit_active) {
        if (const Result result = cancel_component_edit(); !result) {
            return result;
        }
    }
    impl_->transform_edit = {};
    impl_->scene_asset = asset.value;
    impl_->scene_path = resolved_path;
    impl_->feature_world_bindings = std::move(loaded_bindings.value);
    impl_->active_camera_scene = {};
    const ecs::ComponentTypeId camera_type(aeg::engine::components::kCameraName);
    for (const scene::SceneComponentRecord& component : impl_->scene_asset.components()) {
        if (component.component_type == camera_type) {
            impl_->active_camera_scene = component.entity;
            break;
        }
    }
    impl_->selected_entity =
        impl_->scene_asset.entities().empty() ? scene::SceneEntityId{} : impl_->scene_asset.entities().front().id;
    impl_->selected_entities.clear();
    if (impl_->selected_entity.is_valid()) {
        impl_->selected_entities.push_back(impl_->selected_entity);
    }
    impl_->mode = Mode::kEdit;
    impl_->runtime_scope.reset();
    impl_->runtime_state.reset();
    impl_->active_scope = nullptr;
    Result result = impl_->rebuild_edit_world();
    if (result && !impl_->feature_world_bindings.empty() && impl_->feature_session != nullptr &&
        impl_->feature_session->host != nullptr && impl_->feature_session->host->active() != nullptr) {
        const auto graphs = impl_->feature_session->host->active_documents();
        for (const auto& binding : impl_->feature_world_bindings) {
            result = validate_editor_world_binding_candidate(binding, graphs, impl_->feature_session->registry,
                impl_->scene_asset, impl_->edit_instance, *impl_->edit_scope);
            if (!result) break;
        }
    }
    if (result) {
        impl_->reset_history(false);
        if (impl_->project_manager.loaded()) {
            static_cast<void>(impl_->project_manager.remember_scene(impl_->scene_path));
        }
    } else {
        impl_->scene_asset = previous_asset;
        impl_->scene_path = previous_path;
        impl_->feature_world_bindings = previous_bindings;
        impl_->selected_entities = previous_selection;
        impl_->selected_entity = previous_selected_entity;
        impl_->active_camera_scene = previous_camera;
        impl_->mode = Mode::kEdit;
        impl_->runtime_scope.reset();
        impl_->runtime_state.reset();
        impl_->active_scope = nullptr;
        static_cast<void>(impl_->rebuild_edit_world());
    }
    return result;
}

foundation::Result EditorSceneRuntime::save_scene() {
    if (impl_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->transform_edit.active) {
        return foundation::invalid_state("Save requires the active Transform edit to be committed or cancelled");
    }
    if (impl_->component_edit_active) {
        if (const Result result = commit_component_edit(); !result) {
            return result;
        }
    }
    if (const Result result = impl_->commit_edit_commands(); !result) {
        return result;
    }
    if (const Result result = validate_editor_world_binding_targets(impl_->feature_world_bindings,
                                                                    impl_->scene_asset); !result) {
        return result;
    }
    serialization::BinaryWriter writer;
    if (const Result result = scene::SceneSerializer::serialize(impl_->scene_asset, writer); !result) {
        return result;
    }
    if (impl_->project_manager.loaded()) {
        const auto resolved = impl_->project_manager.resolve_scene_path(impl_->scene_path);
        if (!resolved) {
            return resolved.result;
        }
        impl_->scene_path = resolved.value;
    }
    if (const Result result = write_bytes_atomically(impl_->scene_path, writer.data()); !result) {
        return result;
    }
    if (const Result result = write_editor_world_bindings(impl_->scene_path, impl_->feature_world_bindings); !result) {
        return result;
    }
    impl_->clean_history_cursor = impl_->history_cursor;
    impl_->scene_dirty_base = false;
    impl_->refresh_dirty();
    if (impl_->project_manager.loaded()) {
        static_cast<void>(impl_->project_manager.remember_scene(impl_->scene_path));
    }
    return Result::success();
}

foundation::Result EditorSceneRuntime::save_scene_as(const std::filesystem::path& path) {
    if (impl_ == nullptr || !impl_->initialized) {
        return aeg::foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (path.empty()) {
        return foundation::invalid_argument("Scene Save As path is empty");
    }
    std::filesystem::path resolved_path = path;
    if (impl_->project_manager.loaded()) {
        const auto resolved = impl_->project_manager.resolve_scene_path(path);
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
    if (const auto result = reject_retired_scene_authoring(resolved_path); !result) return result;
    const std::filesystem::path previous_path = impl_->scene_path;
    impl_->scene_path = resolved_path;
    const Result result = save_scene();
    if (!result) {
        impl_->scene_path = previous_path;
    }
    return result;
}

foundation::Result EditorSceneRuntime::refresh_assets() {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Asset database changes are only available in Edit mode");
    }
    if (!impl_->project_manager.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    const Result result = impl_->asset_database.refresh();
    if (!result) {
        return result;
    }
    if (const Result authoring = impl_->reload_project_feature_authoring(); !authoring) {
        return authoring;
    }
    return impl_->synchronize_asset_pipeline();
}

foundation::Result EditorSceneRuntime::import_external_asset(const EditorAssetImportRequest& request) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Asset import is only available in Edit mode");
    }
    if (!impl_->project_manager.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    const Result imported = impl_->asset_database.import_external(request);
    if (!imported) {
        return imported;
    }
    if (const Result authoring = impl_->reload_project_feature_authoring(); !authoring) {
        return authoring;
    }
    return impl_->synchronize_asset_pipeline();
}

foundation::Result EditorSceneRuntime::reimport_asset(uint64_t id) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Asset database changes are only available in Edit mode");
    }
    if (!impl_->project_manager.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    const Result result = impl_->asset_database.reimport(id);
    if (!result) {
        return result;
    }
    if (const Result sync = impl_->synchronize_asset_pipeline(); !sync) {
        return sync;
    }
    return impl_->asset_pipeline.request_reimport(id);
}

foundation::Result EditorSceneRuntime::rename_asset(uint64_t id, std::string_view new_name) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Asset database changes are only available in Edit mode");
    }
    if (!impl_->project_manager.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    return impl_->asset_database.rename(id, new_name);
}

foundation::Result EditorSceneRuntime::remove_asset(uint64_t id) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Asset database changes are only available in Edit mode");
    }
    if (!impl_->project_manager.loaded()) {
        return foundation::invalid_state("No game project is open");
    }
    return impl_->asset_database.remove(id);
}

foundation::Result EditorSceneRuntime::set_feature_world_bindings(
    std::span<const EditorFeatureWorldBinding> bindings) {
    if (impl_ == nullptr || !impl_->initialized)
        return foundation::invalid_state("Editor scene runtime is not initialized");
    if (impl_->mode != Mode::kEdit)
        return foundation::invalid_state("Feature World bindings can only change in Edit mode");
    try {
        std::vector<EditorFeatureWorldBinding> candidate(bindings.begin(), bindings.end());
        if (!candidate.empty()) {
            if (impl_->feature_session == nullptr || impl_->feature_session->host == nullptr ||
                impl_->feature_session->host->active() == nullptr)
                return foundation::not_ready("Publish an executable Feature graph before binding its World commands");
            if (impl_->edit_scope == nullptr || impl_->active_scope != impl_->edit_scope.get())
                return foundation::invalid_state("Editor Edit World is not the active World");
            std::vector<feature::FeatureInstanceId> owners;
            owners.reserve(candidate.size());
            for (const auto& binding : candidate) {
                const auto resolved = resolve_editor_world_binding(binding, *impl_->feature_session->host,
                    impl_->scene_asset, impl_->edit_instance, *impl_->edit_scope);
                if (!resolved) return resolved.result;
                if (std::find(owners.begin(), owners.end(), resolved.value.instance) != owners.end())
                    return foundation::invalid_argument("Editor World command owner has more than one explicit route");
                owners.push_back(resolved.value.instance);
            }
        }
        if (const auto result = write_editor_world_bindings(impl_->scene_path, candidate); !result) return result;
        impl_->feature_world_bindings = std::move(candidate);
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor World binding copy allocation failed");
    }
}

std::span<const EditorFeatureWorldBinding> EditorSceneRuntime::feature_world_bindings() const noexcept {
    return impl_ == nullptr ? std::span<const EditorFeatureWorldBinding>{}
                            : std::span<const EditorFeatureWorldBinding>{impl_->feature_world_bindings};
}

foundation::Result EditorSceneRuntime::bind_feature_world_node(
    std::string_view graph_asset_uuid, feature::FeatureNodeId node, scene::SceneEntityId scene_entity) {
    if (impl_ == nullptr || !impl_->initialized)
        return foundation::invalid_state("Editor scene runtime is not initialized");
    if (graph_asset_uuid.empty() || !node.is_valid() || !scene_entity.is_valid())
        return foundation::invalid_argument("Feature World route selection has an invalid authored identity");
    if (impl_->feature_session == nullptr || impl_->feature_session->host == nullptr ||
        impl_->feature_session->host->active() == nullptr)
        return foundation::not_ready("Publish the selected FeatureGraph before creating a World route");
    const auto execution = impl_->feature_session->host->inspect_execution(graph_asset_uuid);
    if (!execution) return execution.result;
    const runtime::FeatureRuntimeExecutionProvenance* attribution = nullptr;
    for (const auto& candidate : execution.value.provenance) {
        if (candidate.authored_node_id != node) continue;
        if (attribution != nullptr)
            return foundation::invalid_argument("Selected Feature node has multiple expanded owners; choose one exact owner route");
        attribution = &candidate;
    }
    if (attribution == nullptr)
        return foundation::not_found("Selected Feature node has no published execution owner");
    const auto observed = impl_->feature_session->host->inspect_node(graph_asset_uuid, node);
    if (!observed) return observed.result;
    const auto owner = std::find_if(observed.value.owners.begin(), observed.value.owners.end(), [&](const auto& candidate) {
        return candidate.owner.instance_id == attribution->runtime.instance_id;
    });
    if (owner == observed.value.owners.end())
        return foundation::invalid_state("Selected Feature execution owner is not present in its node snapshot");
    const auto effect = std::find_if(owner->contract.side_effects.begin(), owner->contract.side_effects.end(),
        [](const feature::SideEffectContract& candidate) {
            return candidate.id == feature::kWorldTransformCommandEffect &&
                   candidate.version == feature::kWorldTransformCommandVersion &&
                   candidate.kind == feature::SideEffectKind::kCommand &&
                   candidate.commit == feature::CommitPolicy::kOutbox;
        });
    if (effect == owner->contract.side_effects.end())
        return foundation::invalid_argument("Selected Feature contract does not declare the World.Transform outbox");
    feature::PortId observation_port{};
    for (const auto& port : owner->contract.ports) {
        if (port.direction != feature::PortDirection::kInput ||
            port.schema != feature::kWorldTransformInputSchema ||
            port.version != feature::kWorldTransformInputVersion) continue;
        if (observation_port.is_valid())
            return foundation::invalid_argument("Selected Feature contract has multiple World.Transform input ports");
        observation_port = port.id;
    }

    try {
        std::vector<EditorFeatureWorldBinding> candidate(impl_->feature_world_bindings);
        candidate.erase(std::remove_if(candidate.begin(), candidate.end(), [&](const auto& binding) {
            return binding.graph_asset_uuid == graph_asset_uuid && binding.node_id == node;
        }), candidate.end());
        candidate.push_back({std::string(graph_asset_uuid), node, attribution->authored_instance_path,
                             impl_->scene_asset.id(), scene_entity, effect->id, effect->version, observation_port});
        return set_feature_world_bindings(candidate);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor World route candidate allocation failed");
    }
}

foundation::Result EditorSceneRuntime::clear_feature_world_node_binding(
    std::string_view graph_asset_uuid, feature::FeatureNodeId node) {
    if (impl_ == nullptr || !impl_->initialized)
        return foundation::invalid_state("Editor scene runtime is not initialized");
    if (graph_asset_uuid.empty() || !node.is_valid())
        return foundation::invalid_argument("Feature World route removal has an invalid authored identity");
    try {
        std::vector<EditorFeatureWorldBinding> candidate(impl_->feature_world_bindings);
        candidate.erase(std::remove_if(candidate.begin(), candidate.end(), [&](const auto& binding) {
            return binding.graph_asset_uuid == graph_asset_uuid && binding.node_id == node;
        }), candidate.end());
        if (candidate.size() == impl_->feature_world_bindings.size()) return Result::success();
        return set_feature_world_bindings(candidate);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor World route removal allocation failed");
    }
}

foundation::ResultValue<EditorWorldSnapshot> EditorSceneRuntime::inspect_active_world_snapshot() const {
    if (impl_ == nullptr || !impl_->initialized)
        return {foundation::invalid_state("Editor scene runtime is not initialized"), {}};
    const bool edit = impl_->mode == Mode::kEdit;
    world::WorldScope* scope = edit ? impl_->edit_scope.get() : impl_->runtime_scope.get();
    const scene::SceneInstance& instance = edit ? impl_->edit_instance : impl_->runtime_instance;
    if (scope == nullptr || impl_->active_scope != scope || !scope->is_initialized())
        return {foundation::invalid_state("Editor active World lifecycle does not match its current mode"), {}};
    const auto snapshot = scope->latest_snapshot();
    if (!snapshot) return {snapshot.result, {}};
    try {
        EditorWorldSnapshot result{};
        result.snapshot = snapshot.value;
        result.entities.assign(instance.entities().begin(), instance.entities().end());
        return {Result::success(), std::move(result)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Editor active World snapshot diagnostics allocation failed"), {}};
    }
}

foundation::Result EditorSceneRuntime::play() {
    if (impl_ == nullptr || impl_->edit_scope == nullptr) {
        return foundation::invalid_state("Editor edit World is not initialized");
    }
    if (impl_->mode == Mode::kPlay) {
        return Result::success();
    }
    if (impl_->mode == Mode::kPause) {
        impl_->mode = Mode::kPlay;
        return Result::success();
    }
    if (impl_->transform_edit.active) {
        return foundation::invalid_state("Play requires the active Transform edit to be committed or cancelled");
    }
    if (impl_->component_edit_active) {
        if (const Result result = commit_component_edit(); !result) {
            return result;
        }
    }
    if (const Result result = impl_->commit_edit_commands(); !result) {
        return result;
    }
    impl_->runtime_scope.reset();
    impl_->runtime_state.reset();
    impl_->runtime_camera_runtime = {};
    if (const Result result =
            impl_->instantiate_world(impl_->scene_asset, impl_->runtime_state, impl_->runtime_scope,
                                     impl_->runtime_instance, impl_->runtime_camera_runtime, "Editor.RuntimeWorld");
        !result) {
        return result;
    }
    const auto initial_snapshot = impl_->runtime_scope->commit(impl_->frame_context->frame_number());
    if (!initial_snapshot) {
        impl_->runtime_scope.reset();
        impl_->runtime_state.reset();
        return initial_snapshot.result;
    }
    std::unique_ptr<engine::detail::FeatureWorldCommandConsumer> candidate_consumer;
    if (!impl_->feature_world_bindings.empty()) {
        if (impl_->feature_session == nullptr || impl_->feature_session->host == nullptr) {
            impl_->runtime_scope.reset();
            impl_->runtime_state.reset();
            return foundation::not_ready("Feature World bindings require an active executable Feature session");
        }
        auto created = make_editor_world_command_consumer(*impl_->feature_session->execution,
            impl_->reflection_registry, *impl_->feature_session->host, impl_->feature_world_bindings,
            impl_->scene_asset, impl_->runtime_instance, *impl_->runtime_scope);
        if (!created) {
            impl_->runtime_scope.reset();
            impl_->runtime_state.reset();
            return created.result;
        }
        candidate_consumer = std::move(created.value);
    }
    impl_->mode = Mode::kPlay;
    impl_->active_scope = impl_->runtime_scope.get();
    impl_->world_command_consumer = std::move(candidate_consumer);
    return impl_->create_orchestrator_for(*impl_->active_scope);
}

foundation::Result EditorSceneRuntime::pause() {
    if (impl_ == nullptr || impl_->mode != Mode::kPlay) {
        return foundation::invalid_state("Editor runtime is not playing");
    }
    impl_->mode = Mode::kPause;
    return Result::success();
}

foundation::Result EditorSceneRuntime::stop() {
    if (impl_ == nullptr) {
        return foundation::invalid_state("Editor scene runtime is not initialized");
    }
    if (impl_->mode == Mode::kEdit) {
        if (impl_->transform_edit.active) {
            return cancel_transform_edit();
        }
        if (impl_->component_edit_active) {
            return cancel_component_edit();
        }
        return Result::success();
    }
    if (impl_->pending_feature_session != nullptr) {
        if (const auto result = impl_->pending_feature_session->close(); !result) return result;
        impl_->pending_feature_session.reset();
    }
    // The consumer borrows the current FeatureRuntime, and the orchestrator borrows the consumer.
    impl_->orchestrator.reset();
    impl_->world_command_consumer.reset();
    if (impl_->feature_session != nullptr) {
        try {
            if (const auto result = impl_->feature_session->reset_execution(); !result) return result;
        } catch (const std::bad_alloc&) {
            return foundation::out_of_memory("Scene Feature restart allocation failed");
        }
    }
    impl_->transform_edit = {};
    impl_->runtime_scope.reset();
    impl_->runtime_state.reset();
    impl_->runtime_camera_runtime = {};
    impl_->mode = Mode::kEdit;
    impl_->active_scope = impl_->edit_scope.get();
    return impl_->create_orchestrator_for(*impl_->active_scope);
}

bool EditorSceneRuntime::can_undo() const noexcept {
    return impl_ != nullptr && impl_->mode == Mode::kEdit && !impl_->transform_edit.active && impl_->history_cursor > 0;
}

bool EditorSceneRuntime::can_redo() const noexcept {
    return impl_ != nullptr && impl_->mode == Mode::kEdit && !impl_->transform_edit.active &&
           impl_->history_cursor < impl_->history.size();
}

foundation::Result EditorSceneRuntime::undo() {
    if (impl_ == nullptr || impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Undo is only available in Edit mode");
    }
    if (impl_->transform_edit.active) {
        return foundation::invalid_state("Undo cannot run during an active Transform edit");
    }
    if (impl_->history_cursor == 0) {
        return foundation::not_found("Editor Undo history is empty");
    }
    const Impl::EditorCommand& command = impl_->history[impl_->history_cursor - 1];
    if (const Result result = impl_->apply_history_command(command, true); !result) {
        return result;
    }
    --impl_->history_cursor;
    impl_->refresh_dirty();
    return Result::success();
}

foundation::Result EditorSceneRuntime::redo() {
    if (impl_ == nullptr || impl_->mode != Mode::kEdit) {
        return foundation::invalid_state("Redo is only available in Edit mode");
    }
    if (impl_->transform_edit.active) {
        return foundation::invalid_state("Redo cannot run during an active Transform edit");
    }
    if (impl_->history_cursor >= impl_->history.size()) {
        return foundation::not_found("Editor Redo history is empty");
    }
    const Impl::EditorCommand& command = impl_->history[impl_->history_cursor];
    if (const Result result = impl_->apply_history_command(command, false); !result) {
        return result;
    }
    ++impl_->history_cursor;
    impl_->refresh_dirty();
    return Result::success();
}
}  // namespace aeg::application
