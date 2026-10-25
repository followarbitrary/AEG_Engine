/**
 * @file renderer.hpp
 * @brief Backend-neutral render scene extraction and resource value contracts.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_set>
#include <vector>

#include "ecs.hpp"
#include "feature_render_intent.hpp"
#include "feature_types.hpp"
#include "math.hpp"
#include "render_graph_types.hpp"
#include "renderer_material_contract.hpp"
#include "renderer_material_resource_contract.hpp"
#include "renderer_profile_resource_contract.hpp"
#include "renderer_shader_contract.hpp"
#include "result.hpp"
#include "rhi_struct.hpp"
#include "stable_id.hpp"

namespace aeg::world {
class WorldFrameScope;
}

namespace aeg::rhi { class Device; }
namespace aeg::renderer {
struct RenderViewIdTag;
struct RenderObjectIdTag;
struct RenderLightIdTag;
struct RenderExtractorIdTag;
struct MeshAssetIdTag;
struct MaterialInstanceIdTag;
struct RenderSceneSnapshot;

using RenderViewId = foundation::StableId<RenderViewIdTag>;
using RenderObjectId = foundation::StableId<RenderObjectIdTag>;
using RenderLightId = foundation::StableId<RenderLightIdTag>;
using MeshAssetId = foundation::StableId<MeshAssetIdTag>;
using MaterialInstanceId = foundation::StableId<MaterialInstanceIdTag>;
using RenderExtractorId = foundation::StableId<RenderExtractorIdTag>;

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

/** @brief Returns a stable external resource identity scoped to one RenderView. */
[[nodiscard]] constexpr feature::RenderResourceId make_view_scoped_render_resource_id(
    feature::RenderResourceId base, RenderViewId view) noexcept {
    uint64_t value = base.value();
    value ^= view.value() + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    return feature::RenderResourceId(value == 0 ? 1 : value);
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
    /** Optional independent directional shadow-map extent; zero means use output_extent. */
    RenderExtent shadow_extent{0, 0};
    RenderCamera camera{};
    uint32_t visibility_mask{0xffffffffu};
    feature::RenderResourceId color_target{};
    feature::RenderResourceId depth_target{};
    bool frustum_culling{false};
    /** @brief Resource usage required after the opaque color pass completes. */
    rhi::ResourceUsage color_target_final_usage{rhi::ResourceUsage::kColorAttachmentWrite};
    feature::RenderPostProcessSettings post_process{};

    [[nodiscard]] bool is_valid() const noexcept;
};

/** @brief Stable shadow view/value contract consumed by frame resource resolvers. */
struct RenderShadowView final {
    RenderView view{};
    RenderShadowConstants constants{};
    bool enabled{false};
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
    /** @brief Optional graph-local handles supplied by a frame preparation upload. */
    render_graph::RenderGraphBuffer vertex_resource{};
    render_graph::RenderGraphBuffer index_resource{};

    /** @brief Reports whether this resource can be consumed by an indexed graphics draw. */
    [[nodiscard]] bool is_valid() const noexcept;
};

/** @brief GPU resources resolved for one frame/view-local lighting binding. */
struct RenderLightingResource final {
    /** @brief Descriptor set exposing RenderLightingConstants. */
    rhi::BindingSetHandle binding_set{};
    /** @brief Pipeline layout group used when binding binding_set. */
    uint32_t binding_group{2};
    /** @brief Uniform buffer containing the frame/view-local lighting constants. */
    rhi::BufferHandle lighting_buffer{};
    /** @brief Immutable creation contract for lighting_buffer. */
    rhi::BufferDesc lighting_buffer_desc{};
    /** @brief Graph buffer used to declare the fragment uniform read. */
    render_graph::RenderGraphBuffer lighting_resource{};
    /** @brief Byte offset of the lighting constant range. */
    uint64_t lighting_offset{0};
    /** @brief Byte size of the lighting constant range. */
    uint64_t lighting_size{sizeof(RenderLightingConstants)};

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
    /** @brief Resolves the frame/view-local lighting binding required by Opaque. */
    [[nodiscard]] virtual foundation::ResultValue<RenderLightingResource> resolve_lighting(
        const RenderSceneSnapshot& snapshot, const RenderView& view) const;
    /** @brief Resolves fullscreen deferred shading; not needed for an empty scene. */
    [[nodiscard]] virtual foundation::ResultValue<RenderDeferredLightingResource> resolve_deferred_lighting(
        const RenderView& view) const;
    [[nodiscard]] virtual foundation::ResultValue<RenderShadowResource> resolve_shadow(const RenderShadowView&) const {
        return {foundation::not_found("shadow resources are unavailable"), {}};
    }
    [[nodiscard]] virtual foundation::ResultValue<RenderLightCullingResource> resolve_light_culling(
        const RenderSceneSnapshot&, const RenderShadowView&) const {
        return {foundation::not_found("light culling resources are unavailable"), {}};
    }
    [[nodiscard]] virtual foundation::ResultValue<RenderPostProcessResource> resolve_post_process(
        const RenderView&) const {
        return {foundation::not_found("skybox/postprocess resources are unavailable"), {}};
    }
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
    /** @brief Mask-visible casters retained before camera frustum culling. */
    std::vector<RenderInstance> shadow_casters{};

    [[nodiscard]] bool is_valid() const noexcept {
        return world_revision != 0 && world.is_valid() && view.is_valid() && view.world == world;
    }
};

/** @brief Selects the first supported Directional Light and builds ambient-only fallback data when absent. */
[[nodiscard]] foundation::ResultValue<RenderLightingConstants> make_render_lighting_constants(
    std::span<const RenderLight> lights, math::Vec3 ambient_color = math::Vec3{0.05f, 0.05f, 0.05f});

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
    std::unordered_set<RenderObjectId, foundation::StableIdHash<RenderObjectIdTag>> instance_ids_{};
    std::unordered_set<RenderLightId, foundation::StableIdHash<RenderLightIdTag>> light_ids_{};
    const RenderResourceResolver* resource_resolver_{nullptr};
    bool require_resolved_resources_{false};
    math::Frustum frustum_{};
    bool frustum_valid_{true};
    bool finished_{false};

    friend class Renderer;
};

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
                                                  feature::RenderResourceId default_color_target = {}) const;

    /** @brief Extracts an immutable scene snapshot from a committed World snapshot. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> extract(
        const ecs::WorldSnapshot& world_snapshot, const RenderView& view, const RenderExtractionDesc& desc = {}) const;
    /** @brief Extracts an immutable scene snapshot through a World-to-Frame bridge. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> extract(
        const world::WorldFrameScope& frame_scope, const RenderView& view, void* user_data = nullptr) const;
    /** @brief Extracts through a World-to-Frame bridge with explicit resource policy. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> extract(
        const world::WorldFrameScope& frame_scope, const RenderView& view, const RenderExtractionDesc& desc) const;

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

}  // namespace aeg::renderer
