/**
 * @file renderer.hpp
 * @brief Backend-neutral render scene extraction and RenderPipeline contracts.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#include "ecs.hpp"
#include "math.hpp"
#include "pipeline.hpp"
#include "rhi_struct.hpp"

namespace aeg::world {
class WorldFrameScope;
}

namespace aeg::renderer {
struct RenderViewIdTag;
struct RenderObjectIdTag;
struct RenderLightIdTag;
struct RenderExtractorIdTag;
struct MeshAssetIdTag;
struct MaterialInstanceIdTag;

using RenderViewId = pipeline::StableId<RenderViewIdTag>;
using RenderObjectId = pipeline::StableId<RenderObjectIdTag>;
using RenderLightId = pipeline::StableId<RenderLightIdTag>;
using MeshAssetId = pipeline::StableId<MeshAssetIdTag>;
using MaterialInstanceId = pipeline::StableId<MaterialInstanceIdTag>;
using RenderExtractorId = pipeline::StableId<RenderExtractorIdTag>;

inline constexpr pipeline::ArtifactId kRenderSceneSnapshotArtifactId{"RenderSceneSnapshot"};
inline constexpr pipeline::ArtifactTypeId kRenderSceneSnapshotArtifactTypeId{"RenderSceneSnapshot"};
inline constexpr uint32_t kRenderSceneSnapshotArtifactVersion = 1;

/** @brief Creates a stable view identity for a numeric World view slot. */
[[nodiscard]] constexpr RenderViewId make_render_view_id(uint32_t view_id) noexcept {
    return RenderViewId(static_cast<uint64_t>(view_id) + 1u);
}

/** @brief Creates a view identity unique within a World and view slot pair. */
[[nodiscard]] constexpr RenderViewId make_render_view_id(ecs::WorldHandle world, uint32_t view_id) noexcept {
    uint64_t value = 14695981039346656037ull;
    const auto mix = [&value](uint64_t component) constexpr {
        value ^= component;
        value *= 1099511628211ull;
    };
    mix(world.index);
    mix(world.generation);
    mix(view_id);
    return RenderViewId(value == 0 ? 1 : value);
}

/** @brief Creates a stable render-object identity from an ECS entity identity. */
[[nodiscard]] constexpr RenderObjectId make_render_object_id(ecs::EntityId entity) noexcept {
    const uint64_t value = (static_cast<uint64_t>(entity.generation) << 32u) | entity.index;
    return RenderObjectId(value == 0 ? 1 : value);
}

/** @brief Creates a stable light identity from an ECS entity identity. */
[[nodiscard]] constexpr RenderLightId make_render_light_id(ecs::EntityId entity) noexcept {
    const uint64_t value = (static_cast<uint64_t>(entity.generation) << 32u) | entity.index;
    return RenderLightId(value == 0 ? 1 : value);
}

/** @brief Pixel extent associated with one render view. */
struct RenderExtent final {
    uint32_t width{1};
    uint32_t height{1};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return width != 0 && height != 0; }
    friend constexpr bool operator==(RenderExtent, RenderExtent) = default;
};

/** @brief Camera matrices and clip-space parameters consumed by rendering. */
struct RenderCamera final {
    math::Mat4 view{math::identity()};
    math::Mat4 projection{math::identity()};
    math::Vec3 position{};
    float near_plane{0.1f};
    float far_plane{1000.0f};
    math::ClipSpaceConvention clip_space_convention{};

    [[nodiscard]] math::Mat4 view_projection() const noexcept { return projection * view; }
};

/** @brief Immutable input describing one World/View rendering context. */
struct RenderView final {
    RenderViewId id{};
    ecs::WorldHandle world{};
    uint32_t view_id{0};
    RenderExtent output_extent{};
    RenderCamera camera{};
    uint32_t visibility_mask{0xffffffffu};
    pipeline::RenderResourceId color_target{};
    pipeline::RenderResourceId depth_target{};
    bool frustum_culling{false};

    [[nodiscard]] bool is_valid() const noexcept;
};

/** @brief Reports view-local visibility filtering performed during extraction. */
struct RenderVisibilityStats final {
    uint32_t candidate_instance_count{0};
    uint32_t visible_instance_count{0};
    uint32_t visibility_masked_instance_count{0};
    uint32_t frustum_culled_instance_count{0};
    uint32_t candidate_light_count{0};
    uint32_t visible_light_count{0};
    uint32_t visibility_masked_light_count{0};
    uint32_t frustum_culled_light_count{0};
};

/** @brief One drawable instance extracted from immutable World state. */
struct RenderInstance final {
    RenderObjectId object{};
    MeshAssetId mesh{};
    MaterialInstanceId material{};
    math::Mat4 world_transform{math::identity()};
    math::Mat4 previous_world_transform{math::identity()};
    math::Vec3 bounds_center{};
    float bounds_radius{0.0f};
    uint32_t visibility_mask{0xffffffffu};
};

/** @brief Semantic light class used by render features. */
enum class RenderLightType : uint8_t {
    kDirectional,
    kPoint,
    kSpot,
};

/** @brief One light description extracted for the current view. */
struct RenderLight final {
    RenderLightId light{};
    RenderLightType type{RenderLightType::kPoint};
    math::Vec3 position{};
    math::Vec3 direction{0.0f, 0.0f, -1.0f};
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity{1.0f};
    float range{1.0f};
    float inner_cone_cosine{1.0f};
    float outer_cone_cosine{0.0f};
    uint32_t visibility_mask{0xffffffffu};
    bool casts_shadow{false};
};

/** @brief Standard ECS component contract for an extracted drawable. */
struct RenderMeshComponent final {
    math::Transform transform{};
    MeshAssetId mesh{};
    MaterialInstanceId material{};
    math::Vec3 bounds_center{};
    float bounds_radius{0.0f};
    uint32_t visibility_mask{0xffffffffu};
    bool enabled{true};
};

/** @brief Standard ECS component contract for an extracted light. */
struct RenderLightComponent final {
    RenderLightType type{RenderLightType::kPoint};
    math::Transform transform{};
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity{1.0f};
    float range{1.0f};
    float inner_cone_cosine{1.0f};
    float outer_cone_cosine{0.0f};
    uint32_t visibility_mask{0xffffffffu};
    bool casts_shadow{false};
    bool enabled{true};
};

/** @brief Interleaved vertex attribute offsets and formats consumed by Opaque. */
struct RenderMeshVertexLayout final {
    /** @brief Byte offset of the float4 position within one interleaved vertex. */
    uint32_t position_offset{0};
    /** @brief Shader-facing format of the position attribute. */
    rhi::Format position_format{rhi::Format::kRgba32Float};
    /** @brief Byte offset of the float2 UV within one interleaved vertex. */
    uint32_t uv_offset{sizeof(float) * 4u};
    /** @brief Shader-facing format of the UV attribute. */
    rhi::Format uv_format{rhi::Format::kRg32Float};
};

/** @brief GPU resources resolved for one mesh asset without exposing backend types. */
struct RenderMeshResource final {
    /** @brief Vertex buffer containing the mesh's interleaved vertex records. */
    rhi::BufferHandle vertex_buffer{};
    /** @brief Immutable creation contract for vertex_buffer. */
    rhi::BufferDesc vertex_buffer_desc{};
    /** @brief Index buffer containing the mesh's primitive indices. */
    rhi::BufferHandle index_buffer{};
    /** @brief Immutable creation contract for index_buffer. */
    rhi::BufferDesc index_buffer_desc{};
    /** @brief Byte stride of each vertex record. */
    uint32_t vertex_stride{0};
    /** @brief Number of vertices available in vertex_buffer. */
    uint32_t vertex_count{0};
    /** @brief Number of indices consumed by an indexed draw. */
    uint32_t index_count{0};
    /** @brief Element width used by index_buffer. */
    rhi::IndexFormat index_format{rhi::IndexFormat::kUint16};
    /** @brief Interleaved vertex attribute offsets and formats consumed by Opaque. */
    RenderMeshVertexLayout vertex_layout{};

    /** @brief Reports whether this resource can be consumed by an indexed graphics draw. */
    [[nodiscard]] bool is_valid() const noexcept;
};

/** @brief GPU resources resolved for one material instance. */
struct RenderMaterialResource final {
    /** @brief Graphics pipeline selected by this material instance. */
    rhi::PipelineHandle pipeline{};
    /** @brief Descriptor set exposing material textures and samplers. */
    rhi::BindingSetHandle binding_set{};
    /** @brief Pipeline layout group used when binding binding_set. */
    uint32_t binding_group{1};
    /** @brief Graph texture sampled by the material binding set. */
    pipeline::RenderGraphTexture material_texture{};
    /** @brief Uniform buffer containing material parameters. */
    rhi::BufferHandle material_parameter_buffer{};
    /** @brief Immutable creation contract for material_parameter_buffer. */
    rhi::BufferDesc material_parameter_buffer_desc{};
    /** @brief Graph buffer used to declare material parameter reads. */
    pipeline::RenderGraphBuffer material_parameter_resource{};
    /** @brief Byte offset of the material parameter range. */
    uint64_t material_parameter_offset{0};
    /** @brief Byte size of the material parameter range. */
    uint64_t material_parameter_size{0};

    /** @brief Reports whether this material can issue a graphics draw. */
    [[nodiscard]] bool is_valid() const noexcept;
};

/** @brief View-local object bindings consumed by the standard opaque shader contract. */
struct RenderObjectResource final {
    /** @brief Uniform buffer containing the object's transform constants. */
    rhi::BufferHandle transform_buffer{};
    /** @brief Immutable creation contract for transform_buffer. */
    rhi::BufferDesc transform_buffer_desc{};
    /** @brief Layout used to allocate binding_set. */
    rhi::BindingLayoutHandle binding_layout{};
    /** @brief Descriptor set exposing transform_buffer to the material pipeline. */
    rhi::BindingSetHandle binding_set{};
    /** @brief Pipeline layout group used when binding binding_set. */
    uint32_t binding_group{0};
    /** @brief Byte offset of the transform range declared to RenderGraph. */
    uint64_t transform_offset{0};
    /** @brief Byte size of the transform range declared to RenderGraph. */
    uint64_t transform_size{0};

    /** @brief Reports whether this object exposes a complete transform uniform binding. */
    [[nodiscard]] bool is_valid() const noexcept;
};

/** @brief Resolves renderer asset identities to backend-neutral RHI resources. */
class RenderResourceResolver {
public:
    virtual ~RenderResourceResolver() = default;

    [[nodiscard]] virtual foundation::ResultValue<RenderMeshResource> resolve_mesh(MeshAssetId mesh) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<RenderMaterialResource> resolve_material(
        MaterialInstanceId material) const = 0;
    /** @brief Resolves the view-local transform binding required for one opaque render object. */
    [[nodiscard]] virtual foundation::ResultValue<RenderObjectResource> resolve_object(
        const RenderInstance& instance, const RenderView& view) const = 0;
};

/** @brief Immutable frame-local rendering input produced by RenderExtract. */
struct RenderSceneSnapshot final {
    uint64_t frame_number{0};
    uint64_t world_revision{0};
    ecs::WorldHandle world{};
    RenderView view{};
    RenderVisibilityStats visibility{};
    std::vector<RenderInstance> instances{};
    std::vector<RenderLight> lights{};

    [[nodiscard]] bool is_valid() const noexcept {
        return world_revision != 0 && world.is_valid() && view.is_valid() && view.world == world;
    }
};

class RenderSceneSnapshotBuilder;

/** @brief Per-extraction inputs not stored in the immutable scene snapshot. */
struct RenderExtractionDesc final {
    uint64_t frame_number{0};
    void* user_data{nullptr};
    const RenderResourceResolver* resource_resolver{nullptr};
    bool require_resolved_resources{false};
};

/** @brief Context passed to one World-to-render extraction callback. */
struct RenderExtractContext final {
    const ecs::WorldSnapshot& world_snapshot;
    const RenderView& view;
    RenderSceneSnapshotBuilder& builder;
    void* user_data{nullptr};
    const RenderResourceResolver* resource_resolver{nullptr};
    bool require_resolved_resources{false};
};

using RenderExtractFunction = std::function<foundation::Result(const RenderExtractContext& context)>;

/** @brief Mutable construction facade for one immutable RenderSceneSnapshot. */
class RenderSceneSnapshotBuilder final {
public:
    RenderSceneSnapshotBuilder(const RenderSceneSnapshotBuilder&) = delete;
    RenderSceneSnapshotBuilder& operator=(const RenderSceneSnapshotBuilder&) = delete;
    RenderSceneSnapshotBuilder(RenderSceneSnapshotBuilder&&) = delete;
    RenderSceneSnapshotBuilder& operator=(RenderSceneSnapshotBuilder&&) = delete;

    /** @brief Appends one drawable instance and rejects duplicate object identities. */
    [[nodiscard]] foundation::Result add_instance(const RenderInstance& instance);
    /** @brief Appends one light and rejects duplicate light identities. */
    [[nodiscard]] foundation::Result add_light(const RenderLight& light);
    /** @brief Finalizes the builder and transfers immutable snapshot ownership. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> finish();

private:
    RenderSceneSnapshotBuilder(const ecs::WorldSnapshot& world_snapshot, const RenderView& view,
                               const RenderExtractionDesc& desc);

    [[nodiscard]] foundation::Result validate_instance_resources(const RenderInstance& instance) const;

    RenderSceneSnapshot snapshot_{};
    std::unordered_set<RenderObjectId, pipeline::StableIdHash<RenderObjectIdTag>> instance_ids_{};
    std::unordered_set<RenderLightId, pipeline::StableIdHash<RenderLightIdTag>> light_ids_{};
    const RenderResourceResolver* resource_resolver_{nullptr};
    bool require_resolved_resources_{false};
    math::Frustum frustum_{};
    bool frustum_valid_{true};
    bool finished_{false};

    friend class Renderer;
};

/** @brief Runtime data made available to a RenderFeature while building its graph. */
struct RenderFeatureContext final {
    const RenderSceneSnapshot* scene_snapshot{nullptr};
    const RenderResourceResolver* resource_resolver{nullptr};
    void* user_data{nullptr};

    [[nodiscard]] bool is_valid() const noexcept { return scene_snapshot != nullptr && scene_snapshot->is_valid(); }
};

/** @brief Returns the typed renderer context attached to a runtime RenderFeature builder. */
[[nodiscard]] const RenderFeatureContext* render_feature_context(const pipeline::RenderGraphBuilder& builder) noexcept;

/** @brief Owns registered extraction callbacks and produces frame snapshots. */
class Renderer final {
public:
    Renderer() = default;
    ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    /** @brief Registers one callback used during RenderExtract. */
    [[nodiscard]] foundation::ResultValue<RenderExtractorId> register_extractor(RenderExtractFunction function);
    /** @brief Registers extraction callbacks for the standard render components. */
    [[nodiscard]] foundation::Result register_builtin_extractors();
    /** @brief Removes a previously registered extraction callback. */
    [[nodiscard]] foundation::Result unregister_extractor(RenderExtractorId extractor);
    /** @brief Removes all registered extraction callbacks. */
    void clear_extractors() noexcept;

    /** @brief Validates a view and applies a default color output when needed. */
    [[nodiscard]] foundation::Result prepare_view(RenderView& view,
                                                  pipeline::RenderResourceId default_color_target = {}) const;

    /** @brief Extracts an immutable scene snapshot from a committed World snapshot. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> extract(
        const ecs::WorldSnapshot& world_snapshot, const RenderView& view, const RenderExtractionDesc& desc = {}) const;
    /** @brief Extracts an immutable scene snapshot through a World-to-Frame bridge. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> extract(
        const world::WorldFrameScope& frame_scope, const RenderView& view, void* user_data = nullptr) const;
    /** @brief Extracts through a World-to-Frame bridge with explicit resource policy. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> extract(
        const world::WorldFrameScope& frame_scope, const RenderView& view, const RenderExtractionDesc& desc) const;

    /** @brief Installs the standard RenderSceneSnapshot contract on a Frame host. */
    [[nodiscard]] foundation::Result install_frame_contract(pipeline::PipelineScopeHost& frame_host) const;
    /** @brief Publishes one extracted snapshot into a Frame host artifact registry. */
    [[nodiscard]] foundation::Result publish_frame_snapshot(pipeline::PipelineScopeHost& frame_host,
                                                            std::shared_ptr<const RenderSceneSnapshot> snapshot) const;

private:
    struct ExtractorRegistration final {
        RenderExtractorId id{0};
        RenderExtractFunction function{};
    };

    mutable std::shared_mutex mutex_{};
    std::vector<ExtractorRegistration> extractors_{};
    RenderExtractorId builtin_mesh_extractor_id_{};
    RenderExtractorId builtin_light_extractor_id_{};
    uint64_t next_extractor_id_{1};
};

/** @brief Standard frame-scoped RenderPipeline with public render FeaturePoints. */
class RenderPipeline final : public pipeline::Pipeline {
public:
    RenderPipeline();

    [[nodiscard]] const pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }
    [[nodiscard]] foundation::Result build(pipeline::PipelineBuilder& builder) const override;

private:
    pipeline::PipelineDescriptor descriptor_{};
};

/** @brief Registers the standard RenderPipeline definition in a PipelineRegistry. */
[[nodiscard]] foundation::Result register_render_pipeline(pipeline::PipelineRegistry& registry);
}  // namespace aeg::renderer
