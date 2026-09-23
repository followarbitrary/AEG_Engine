/**
 * @file renderer.cpp
 * @brief Implements render scene extraction and the standard RenderPipeline.
 */
#include "../public/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "world_scope.hpp"

namespace aeg::renderer {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::out_of_memory;
using foundation::unknown_exception;

[[nodiscard]] bool is_unit_range_color(const math::Vec3& value) noexcept {
    return math::is_finite(value) && value.x >= 0.0f && value.x <= 1.0f && value.y >= 0.0f && value.y <= 1.0f &&
           value.z >= 0.0f && value.z <= 1.0f;
}

[[nodiscard]] float maximum_scale(const math::Mat4& matrix) noexcept {
    const math::Vec3 x{matrix[0].x, matrix[0].y, matrix[0].z};
    const math::Vec3 y{matrix[1].x, matrix[1].y, matrix[1].z};
    const math::Vec3 z{matrix[2].x, matrix[2].y, matrix[2].z};
    return std::max({math::length(x), math::length(y), math::length(z)});
}

[[nodiscard]] bool instance_inside_view(const RenderInstance& instance, const RenderView& view,
                                        const math::Frustum& frustum) noexcept {
    if ((instance.visibility_mask & view.visibility_mask) == 0) {
        return false;
    }
    if (!view.frustum_culling) {
        return true;
    }
    math::Vec3 center{};
    if (!math::try_transform_point(instance.world_transform, instance.bounds_center, center)) {
        return false;
    }
    const float radius = instance.bounds_radius * maximum_scale(instance.world_transform);
    return math::is_finite(radius) && frustum.intersects_sphere(center, radius);
}

[[nodiscard]] bool light_inside_view(const RenderLight& light, const RenderView& view,
                                     const math::Frustum& frustum) noexcept {
    if ((light.visibility_mask & view.visibility_mask) == 0) {
        return false;
    }
    if (!view.frustum_culling || light.type == RenderLightType::kDirectional) {
        return true;
    }
    return frustum.intersects_sphere(light.position, light.range);
}

void increment(uint32_t& value) noexcept {
    if (value != std::numeric_limits<uint32_t>::max()) {
        ++value;
    }
}

[[nodiscard]] RenderExtractorId make_extractor_id(uint64_t value) noexcept {
    return RenderExtractorId(value == 0 ? 1 : value);
}

[[nodiscard]] bool is_valid_light_component(const RenderLightComponent& component) noexcept {
    return math::is_finite(component.intensity) && math::is_finite(component.range) && component.intensity >= 0.0f &&
           component.range >= 0.0f && is_unit_range_color(component.color) &&
           math::is_finite(component.inner_cone_cosine) && math::is_finite(component.outer_cone_cosine);
}

[[nodiscard]] foundation::Result append_default_meshes(const RenderExtractContext& context) {
    foundation::Result result = foundation::Result::success();
    context.world_snapshot.for_each<RenderMeshComponent>(
        [&result, &context](ecs::EntityId entity, const RenderMeshComponent& component) {
            if (!result || !component.enabled || !component.mesh.is_valid() || !component.material.is_valid()) {
                return;
            }
            const RenderInstance instance{
                make_render_object_id(entity),
                component.mesh,
                component.material,
                component.transform.to_matrix(),
                component.transform.to_matrix(),
                component.bounds_center,
                component.bounds_radius,
                component.visibility_mask,
            };
            result = context.builder.add_instance(instance);
        });
    return result;
}

[[nodiscard]] foundation::Result append_default_lights(const RenderExtractContext& context) {
    foundation::Result result = foundation::Result::success();
    context.world_snapshot.for_each<RenderLightComponent>(
        [&result, &context](ecs::EntityId entity, const RenderLightComponent& component) {
            if (!result || !component.enabled) {
                return;
            }
            if (!is_valid_light_component(component)) {
                result = invalid_argument("render light component contains a non-finite or negative value");
                return;
            }
            const math::Vec3 direction =
                math::normalize(component.transform.transform_vector(math::Vec3{0.0f, 0.0f, -1.0f}));
            const RenderLight light{
                make_render_light_id(entity),
                component.type,
                component.transform.translation,
                direction,
                component.color,
                component.intensity,
                component.range,
                component.inner_cone_cosine,
                component.outer_cone_cosine,
                component.visibility_mask,
                component.casts_shadow,
            };
            result = context.builder.add_light(light);
        });
    return result;
}

[[nodiscard]] pipeline::StageDescriptor make_stage(pipeline::StageId id, pipeline::EnginePhase phase,
                                                   pipeline::ExecutionDomain execution_domain) {
    return pipeline::StageDescriptor{
        .id = id,
        .phase = phase,
        .execution_domain = execution_domain,
        .time_policy = pipeline::TimePolicy::kFrame,
        .parallelizable = true,
        .phase_id = {},
    };
}

[[nodiscard]] pipeline::FeaturePointDescriptor make_feature_point(pipeline::FeaturePointId id, std::string_view name,
                                                                  pipeline::StageId anchor_stage,
                                                                  pipeline::FeaturePointFlags flags) {
    return pipeline::FeaturePointDescriptor{
        .id = id,
        .name = name,
        .pipeline = {},
        .anchor_stage = anchor_stage,
        .position = pipeline::FeaturePointPosition::kBefore,
        .allowed_domains = pipeline::to_mask(pipeline::FeatureDomain::kRender),
        .allowed_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame),
        .flags = flags,
    };
}

[[nodiscard]] foundation::Result add_stage_and_point(pipeline::PipelineBuilder& builder, pipeline::StageId stage_id,
                                                     pipeline::FeaturePointId point_id, std::string_view point_name,
                                                     pipeline::EnginePhase phase,
                                                     pipeline::ExecutionDomain execution_domain,
                                                     pipeline::FeaturePointFlags point_flags) {
    const pipeline::PipelineStageHandle stage = builder.add_stage(make_stage(stage_id, phase, execution_domain));
    if (!stage) {
        return builder.status();
    }
    const pipeline::PipelineFeaturePointHandle point =
        builder.add_feature_point(make_feature_point(point_id, point_name, stage_id, point_flags));
    return point ? foundation::Result::success() : builder.status();
}
}  // namespace

bool RenderMeshResource::is_valid() const noexcept {
    if (!vertex_buffer.is_valid() || !index_buffer.is_valid() || vertex_stride == 0 || vertex_count == 0 ||
        index_count == 0 || vertex_buffer_desc.size == 0 || index_buffer_desc.size == 0 ||
        !rhi::has_flag(vertex_buffer_desc.capabilities, rhi::BufferCapability::kVertex) ||
        !rhi::has_flag(index_buffer_desc.capabilities, rhi::BufferCapability::kIndex)) {
        return false;
    }
    const uint64_t vertex_bytes = static_cast<uint64_t>(vertex_stride) * vertex_count;
    const uint64_t index_stride = index_format == rhi::IndexFormat::kUint16 ? sizeof(uint16_t) : sizeof(uint32_t);
    const uint64_t index_bytes = index_stride * index_count;
    return vertex_bytes <= vertex_buffer_desc.size && index_bytes <= index_buffer_desc.size;
}

bool RenderMaterialResource::is_valid() const noexcept {
    return pipeline.is_valid();
}

bool RenderLightingConstants::is_valid() const noexcept {
    const math::Vec3 direction{directional_direction_intensity.x, directional_direction_intensity.y,
                               directional_direction_intensity.z};
    const math::Vec3 directional_color_value{directional_color.x, directional_color.y, directional_color.z};
    const math::Vec3 ambient_color_value{ambient_color.x, ambient_color.y, ambient_color.z};
    const float intensity = directional_direction_intensity.w;
    return math::is_finite(directional_direction_intensity) && math::is_finite(directional_color) &&
           math::is_finite(ambient_color) && math::is_finite(intensity) && intensity >= 0.0f &&
           is_unit_range_color(directional_color_value) && is_unit_range_color(ambient_color_value) &&
           directional_color.w == 1.0f && ambient_color.w == 1.0f &&
           (intensity == 0.0f || math::length_squared(direction) > math::kEpsilon * math::kEpsilon);
}

bool RenderLightingResource::is_valid() const noexcept {
    if (!binding_set.is_valid() || binding_group != 2 || !lighting_buffer.is_valid() || !lighting_resource.is_valid() ||
        lighting_buffer_desc.size == 0 || lighting_size < sizeof(RenderLightingConstants) ||
        !rhi::has_flag(lighting_buffer_desc.capabilities, rhi::BufferCapability::kUniform) ||
        lighting_offset > lighting_buffer_desc.size || lighting_size > lighting_buffer_desc.size - lighting_offset) {
        return false;
    }
    return true;
}

bool RenderObjectResource::is_valid() const noexcept {
    if (!transform_buffer.is_valid() || !binding_layout.is_valid() || !binding_set.is_valid() ||
        transform_buffer_desc.size == 0 || transform_size < sizeof(RenderObjectConstants) ||
        !rhi::has_flag(transform_buffer_desc.capabilities, rhi::BufferCapability::kUniform)) {
        return false;
    }
    return transform_offset <= transform_buffer_desc.size &&
           transform_size <= transform_buffer_desc.size - transform_offset;
}

RenderSceneSnapshotBuilder::RenderSceneSnapshotBuilder(const ecs::WorldSnapshot& world_snapshot, const RenderView& view,
                                                       const RenderExtractionDesc& desc)
    : snapshot_{
          .frame_number = desc.frame_number != 0 ? desc.frame_number : world_snapshot.frame_number(),
          .world_revision = world_snapshot.revision(),
          .world = world_snapshot.world(),
          .view = view,
      },
      resource_resolver_(desc.resource_resolver),
      require_resolved_resources_(desc.require_resolved_resources),
      frustum_valid_(!view.frustum_culling || math::try_extract_frustum(view.camera.view_projection(), frustum_,
                                                                        view.camera.clip_space_convention)) {}

foundation::Result RenderSceneSnapshotBuilder::validate_instance_resources(const RenderInstance& instance) const {
    if (!require_resolved_resources_) {
        return foundation::Result::success();
    }
    if (resource_resolver_ == nullptr) {
        return invalid_state("render extraction requires a resource resolver");
    }

    const foundation::ResultValue<RenderMeshResource> mesh = resource_resolver_->resolve_mesh(instance.mesh);
    if (!mesh) {
        return mesh.result;
    }
    if (!mesh.value.is_valid()) {
        return invalid_state("render mesh resolver returned an invalid resource");
    }

    const foundation::ResultValue<RenderMaterialResource> material =
        resource_resolver_->resolve_material(instance.material);
    if (!material) {
        return material.result;
    }
    if (!material.value.is_valid()) {
        return invalid_state("render material resolver returned an invalid resource");
    }
    return foundation::Result::success();
}

foundation::Result RenderSceneSnapshotBuilder::add_instance(const RenderInstance& instance) {
    if (finished_) {
        return invalid_state("render scene snapshot builder has already been finalized");
    }
    if (!instance.object.is_valid() || !instance.mesh.is_valid() || !instance.material.is_valid()) {
        return invalid_argument("render instance identity or asset identity is invalid");
    }
    if (!math::is_finite(instance.bounds_radius) || instance.bounds_radius < 0.0f) {
        return invalid_argument("render instance bounds radius is invalid");
    }
    if (!math::is_finite(instance.world_transform) || !math::is_finite(instance.previous_world_transform) ||
        !math::is_finite(instance.bounds_center)) {
        return invalid_argument("render instance contains a non-finite transform or bounds value");
    }
    if (instance_ids_.find(instance.object) != instance_ids_.end()) {
        return invalid_state("render scene snapshot contains a duplicate object identity");
    }
    try {
        instance_ids_.emplace(instance.object);
    } catch (const std::bad_alloc&) {
        return out_of_memory("render instance identity allocation failed");
    }
    increment(snapshot_.visibility.candidate_instance_count);

    if ((instance.visibility_mask & snapshot_.view.visibility_mask) == 0) {
        increment(snapshot_.visibility.visibility_masked_instance_count);
        return foundation::Result::success();
    }
    try {
        snapshot_.shadow_casters.push_back(instance);
    } catch (const std::bad_alloc&) {
        return out_of_memory("shadow caster snapshot allocation failed");
    }
    if (snapshot_.view.frustum_culling) {
        if (!frustum_valid_) {
            return invalid_argument("render view frustum is invalid");
        }
        if (!instance_inside_view(instance, snapshot_.view, frustum_)) {
            increment(snapshot_.visibility.frustum_culled_instance_count);
            return foundation::Result::success();
        }
    }
    if (const foundation::Result result = validate_instance_resources(instance); !result) {
        return result;
    }
    try {
        snapshot_.instances.push_back(instance);
        increment(snapshot_.visibility.visible_instance_count);
    } catch (const std::bad_alloc&) {
        return out_of_memory("render instance snapshot allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result RenderSceneSnapshotBuilder::add_light(const RenderLight& light) {
    if (finished_) {
        return invalid_state("render scene snapshot builder has already been finalized");
    }
    if (!light.light.is_valid() || !math::is_finite(light.intensity) || !math::is_finite(light.range) ||
        light.intensity < 0.0f || light.range < 0.0f || !math::is_finite(light.position) ||
        !math::is_finite(light.direction) || !is_unit_range_color(light.color) ||
        !math::is_finite(light.inner_cone_cosine) || !math::is_finite(light.outer_cone_cosine)) {
        return invalid_argument("render light identity or scalar values are invalid");
    }
    if (light_ids_.find(light.light) != light_ids_.end()) {
        return invalid_state("render scene snapshot contains a duplicate light identity");
    }
    try {
        light_ids_.emplace(light.light);
    } catch (const std::bad_alloc&) {
        return out_of_memory("render light identity allocation failed");
    }
    increment(snapshot_.visibility.candidate_light_count);
    if ((light.visibility_mask & snapshot_.view.visibility_mask) == 0) {
        increment(snapshot_.visibility.visibility_masked_light_count);
        return foundation::Result::success();
    }
    if (snapshot_.view.frustum_culling) {
        if (!frustum_valid_) {
            return invalid_argument("render view frustum is invalid");
        }
        if (!light_inside_view(light, snapshot_.view, frustum_)) {
            increment(snapshot_.visibility.frustum_culled_light_count);
            return foundation::Result::success();
        }
    }
    try {
        snapshot_.lights.push_back(light);
        increment(snapshot_.visibility.visible_light_count);
    } catch (const std::bad_alloc&) {
        return out_of_memory("render light snapshot allocation failed");
    }
    return foundation::Result::success();
}

foundation::ResultValue<RenderLightingResource> RenderResourceResolver::resolve_lighting(const RenderSceneSnapshot&,
                                                                                         const RenderView&) const {
    return {not_found("render resource resolver does not provide a frame lighting resource"), {}};
}

foundation::ResultValue<RenderDeferredLightingResource> RenderResourceResolver::resolve_deferred_lighting(
    const RenderView&) const {
    return {not_found("render resource resolver does not provide deferred lighting resources"), {}};
}

foundation::ResultValue<RenderLightingConstants> make_render_lighting_constants(std::span<const RenderLight> lights,
                                                                                math::Vec3 ambient_color) {
    if (!is_unit_range_color(ambient_color)) {
        return {invalid_argument("render lighting ambient color must be finite and within [0, 1]"), {}};
    }

    RenderLightingConstants constants{};
    constants.ambient_color = math::Vec4{ambient_color.x, ambient_color.y, ambient_color.z, 1.0f};
    uint32_t directional_count = 0;
    for (const RenderLight& light : lights) {
        if (light.type != RenderLightType::kDirectional) {
            continue;
        }
        if (++directional_count > 1) {
            return {invalid_state("minimal opaque lighting accepts at most one Directional Light"), {}};
        }
        if (!math::is_finite(light.intensity) || light.intensity < 0.0f || !math::is_finite(light.direction) ||
            !is_unit_range_color(light.color)) {
            return {invalid_argument("Directional Light contains an invalid direction, color, or intensity"), {}};
        }

        math::Vec3 normalized_direction{};
        if (light.intensity > 0.0f && !math::try_normalize(light.direction, normalized_direction)) {
            return {invalid_argument("Directional Light direction must be non-zero"), {}};
        }
        if (light.intensity == 0.0f) {
            normalized_direction = {0.0f, 0.0f, -1.0f};
        }
        constants.directional_direction_intensity =
            math::Vec4{normalized_direction.x, normalized_direction.y, normalized_direction.z, light.intensity};
        constants.directional_color = math::Vec4{light.color.x, light.color.y, light.color.z, 1.0f};
    }
    return {constants.is_valid() ? foundation::Result::success()
                                 : invalid_state("render lighting constants violate the shader contract"),
            constants};
}

foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> RenderSceneSnapshotBuilder::finish() {
    if (finished_) {
        return {invalid_state("render scene snapshot builder has already been finalized"), {}};
    }
    finished_ = true;
    if (!snapshot_.is_valid()) {
        return {invalid_argument("render scene snapshot input is invalid"), {}};
    }

    try {
        std::sort(snapshot_.instances.begin(), snapshot_.instances.end(),
                  [](const RenderInstance& left, const RenderInstance& right) { return left.object < right.object; });
        std::sort(snapshot_.lights.begin(), snapshot_.lights.end(),
                  [](const RenderLight& left, const RenderLight& right) { return left.light < right.light; });
        return {foundation::Result::success(), std::make_shared<const RenderSceneSnapshot>(std::move(snapshot_))};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("render scene snapshot ownership allocation failed"), {}};
    }
}

foundation::ResultValue<RenderExtractorId> Renderer::register_extractor(RenderExtractFunction function) {
    if (!function) {
        return {invalid_argument("render extractor callback is empty"), {}};
    }
    std::unique_lock lock(mutex_);
    const RenderExtractorId id = make_extractor_id(next_extractor_id_++);
    try {
        extractors_.push_back(ExtractorRegistration{id, std::move(function)});
    } catch (const std::bad_alloc&) {
        return {out_of_memory("render extractor registration allocation failed"), {}};
    }
    return {foundation::Result::success(), id};
}

foundation::Result Renderer::register_builtin_extractors() {
    std::unique_lock lock(mutex_);
    const std::size_t initial_size = extractors_.size();
    const RenderExtractorId initial_mesh_extractor = builtin_mesh_extractor_id_;
    const RenderExtractorId initial_light_extractor = builtin_light_extractor_id_;
    const uint64_t initial_next_extractor_id = next_extractor_id_;
    try {
        extractors_.reserve(initial_size + (builtin_mesh_extractor_id_.is_valid() ? 0u : 1u) +
                            (builtin_light_extractor_id_.is_valid() ? 0u : 1u));
        if (!builtin_mesh_extractor_id_.is_valid()) {
            builtin_mesh_extractor_id_ = make_extractor_id(next_extractor_id_++);
            extractors_.push_back(ExtractorRegistration{builtin_mesh_extractor_id_, &append_default_meshes});
        }
        if (!builtin_light_extractor_id_.is_valid()) {
            builtin_light_extractor_id_ = make_extractor_id(next_extractor_id_++);
            extractors_.push_back(ExtractorRegistration{builtin_light_extractor_id_, &append_default_lights});
        }
    } catch (const std::bad_alloc&) {
        extractors_.resize(initial_size);
        builtin_mesh_extractor_id_ = initial_mesh_extractor;
        builtin_light_extractor_id_ = initial_light_extractor;
        next_extractor_id_ = initial_next_extractor_id;
        return out_of_memory("builtin render extractor registration allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result Renderer::unregister_extractor(RenderExtractorId extractor) {
    if (!extractor.is_valid()) {
        return invalid_argument("render extractor identity is invalid");
    }
    std::unique_lock lock(mutex_);
    const auto iterator = std::find_if(extractors_.begin(), extractors_.end(),
                                       [extractor](const auto& entry) { return entry.id == extractor; });
    if (iterator == extractors_.end()) {
        return not_found("render extractor is not registered");
    }
    extractors_.erase(iterator);
    if (extractor == builtin_mesh_extractor_id_) {
        builtin_mesh_extractor_id_ = {};
    }
    if (extractor == builtin_light_extractor_id_) {
        builtin_light_extractor_id_ = {};
    }
    return foundation::Result::success();
}

void Renderer::clear_extractors() noexcept {
    std::unique_lock lock(mutex_);
    extractors_.clear();
    builtin_mesh_extractor_id_ = {};
    builtin_light_extractor_id_ = {};
}

foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> Renderer::extract(
    const ecs::WorldSnapshot& world_snapshot, const RenderView& view, const RenderExtractionDesc& desc) const {
    if (!world_snapshot.is_valid()) {
        return {invalid_argument("render extraction requires a valid WorldSnapshot"), {}};
    }
    if (!view.is_valid() || view.world != world_snapshot.world()) {
        return {invalid_argument("render view does not belong to the WorldSnapshot"), {}};
    }
    const uint64_t frame_number = desc.frame_number != 0 ? desc.frame_number : world_snapshot.frame_number();

    std::vector<ExtractorRegistration> extractors;
    try {
        {
            std::shared_lock lock(mutex_);
            extractors = extractors_;
        }
        RenderSceneSnapshotBuilder builder(world_snapshot, view,
                                           RenderExtractionDesc{frame_number, desc.user_data, desc.resource_resolver,
                                                                desc.require_resolved_resources});
        const RenderExtractContext context{
            world_snapshot, view, builder, desc.user_data, desc.resource_resolver, desc.require_resolved_resources};
        for (const ExtractorRegistration& extractor : extractors) {
            if (const foundation::Result result = extractor.function(context); !result) {
                return {result, {}};
            }
        }
        return builder.finish();
    } catch (const std::bad_alloc&) {
        return {out_of_memory("render extraction allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {unknown_exception(exception), {}};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "render extraction threw an unknown exception"),
                {}};
    }
}

foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> Renderer::extract(
    const world::WorldFrameScope& frame_scope, const RenderView& view, void* user_data) const {
    return extract(frame_scope, view, RenderExtractionDesc{frame_scope.key().frame_number, user_data, nullptr, false});
}

foundation::ResultValue<std::shared_ptr<const RenderSceneSnapshot>> Renderer::extract(
    const world::WorldFrameScope& frame_scope, const RenderView& view, const RenderExtractionDesc& desc) const {
    if (!frame_scope.is_valid()) {
        return {invalid_state("render extraction requires a valid WorldFrameScope"), {}};
    }
    const foundation::ResultValue<pipeline::ArtifactView> artifact = frame_scope.read_snapshot();
    if (!artifact) {
        return {artifact.result, {}};
    }
    const ecs::WorldSnapshot* snapshot = artifact.value.as<ecs::WorldSnapshot>(world::kWorldSnapshotArtifactTypeId);
    if (snapshot == nullptr) {
        return {invalid_state("WorldFrameScope does not contain a WorldSnapshot"), {}};
    }
    RenderExtractionDesc extraction_desc = desc;
    if (extraction_desc.frame_number == 0) {
        extraction_desc.frame_number = frame_scope.key().frame_number;
    }
    return extract(*snapshot, view, extraction_desc);
}

bool RenderView::is_valid() const noexcept {
    return id.is_valid() && world.is_valid() && output_extent.is_valid() && math::is_finite(camera.view) &&
           math::is_finite(camera.projection) && math::is_finite(camera.position) &&
           math::is_finite(camera.near_plane) && math::is_finite(camera.far_plane) && camera.near_plane > 0.0f &&
           camera.far_plane > camera.near_plane;
}

foundation::Result Renderer::prepare_view(RenderView& view, pipeline::RenderResourceId default_color_target) const {
    if (!view.id.is_valid() && view.world.is_valid()) {
        view.id = make_render_view_id(view.world, view.view_id);
    }
    if (!view.color_target.is_valid() && default_color_target.is_valid()) {
        view.color_target = default_color_target;
    }
    if (!view.is_valid()) {
        return invalid_argument("render view contains an invalid identity, extent, camera, or world");
    }
    return foundation::Result::success();
}

const RenderFeatureContext* render_feature_context(const pipeline::RenderGraphBuilder& builder) noexcept {
    if (!builder.is_runtime() || builder.feature_data() == nullptr) {
        return nullptr;
    }
    return static_cast<const RenderFeatureContext*>(builder.feature_data());
}

foundation::Result Renderer::install_frame_contract(pipeline::PipelineScopeHost& frame_host) const {
    if (frame_host.scope() != pipeline::PipelineScope::kFrame) {
        return invalid_argument("RenderSceneSnapshot contract requires a Frame pipeline host");
    }
    return frame_host.artifacts().install_contract({
        .id = kRenderSceneSnapshotArtifactId,
        .type = kRenderSceneSnapshotArtifactTypeId,
        .owner_scope = pipeline::PipelineScope::kFrame,
        .storage = pipeline::ArtifactStorage::kImmutableSnapshot,
        .lifetime = pipeline::ArtifactLifetime::kFrame,
        .ownership = pipeline::OwnershipPolicy::kSharedImmutable,
        .version = kRenderSceneSnapshotArtifactVersion,
        .visible_to = 0,
        .external_input = true,
    });
}

foundation::Result Renderer::publish_frame_snapshot(pipeline::PipelineScopeHost& frame_host,
                                                    std::shared_ptr<const RenderSceneSnapshot> snapshot) const {
    if (frame_host.scope() != pipeline::PipelineScope::kFrame) {
        return invalid_argument("RenderSceneSnapshot publication requires a Frame pipeline host");
    }
    if (snapshot == nullptr || !snapshot->is_valid()) {
        return invalid_argument("render scene snapshot is invalid");
    }
    if (const foundation::Result result = install_frame_contract(frame_host); !result) {
        return result;
    }
    return frame_host.artifacts().publish(
        kRenderSceneSnapshotArtifactId,
        pipeline::ArtifactPayload::owned(kRenderSceneSnapshotArtifactTypeId, std::move(snapshot), false));
}

RenderPipeline::RenderPipeline() {
    descriptor_.id = pipeline::PipelineId("RenderPipeline");
    descriptor_.name = "RenderPipeline";
    descriptor_.category = pipeline::PipelineCategory::kRendering;
    descriptor_.supported_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame);
    descriptor_.domains = pipeline::to_mask(pipeline::FeatureDomain::kRender);
    descriptor_.default_execution_domain = pipeline::ExecutionDomain::kRenderThread;
    descriptor_.default_time_policy = pipeline::TimePolicy::kFrame;
}

foundation::Result RenderPipeline::build(pipeline::PipelineBuilder& builder) const {
    const pipeline::PipelineStageHandle visibility_stage =
        builder.add_stage(make_stage(pipeline::StageId("RenderPipeline.Visibility"),
                                     pipeline::EnginePhase::kRenderExtract, pipeline::ExecutionDomain::kCpuWorker));
    if (!visibility_stage) {
        return builder.status();
    }
    const pipeline::PipelineArtifactHandle scene_artifact = builder.declare_artifact({
        .id = kRenderSceneSnapshotArtifactId,
        .type = kRenderSceneSnapshotArtifactTypeId,
        .owner_scope = pipeline::PipelineScope::kFrame,
        .storage = pipeline::ArtifactStorage::kImmutableSnapshot,
        .lifetime = pipeline::ArtifactLifetime::kFrame,
        .ownership = pipeline::OwnershipPolicy::kSharedImmutable,
        .version = kRenderSceneSnapshotArtifactVersion,
        .visible_to = 0,
        .external_input = true,
    });
    if (!scene_artifact) {
        return builder.status();
    }
    if (const foundation::Result result =
            builder.reads(visibility_stage, {kRenderSceneSnapshotArtifactId, pipeline::ArtifactAccess::kRead,
                                             kRenderSceneSnapshotArtifactVersion, pipeline::AccessFlags::kNone});
        !result) {
        return result;
    }

    struct RenderStage final {
        pipeline::StageId stage;
        pipeline::FeaturePointId point;
        std::string_view name;
        pipeline::EnginePhase phase;
        pipeline::ExecutionDomain execution_domain;
        pipeline::FeaturePointFlags flags;
    };
    const RenderStage stages[] = {
        {pipeline::StageId("RenderPipeline.Shadow"), pipeline::FeaturePointId("RenderPipeline.Shadow"),
         "RenderPipeline.Shadow", pipeline::EnginePhase::kRenderBuild, pipeline::ExecutionDomain::kRenderThread,
         pipeline::FeaturePointFlags::kMayCreateArtifacts | pipeline::FeaturePointFlags::kMayCreateGraphicsPass |
             pipeline::FeaturePointFlags::kMayCreateComputePass | pipeline::FeaturePointFlags::kMayUseDedicatedQueue},
        {pipeline::StageId("RenderPipeline.Depth"), pipeline::FeaturePointId("RenderPipeline.Depth"),
         "RenderPipeline.Depth", pipeline::EnginePhase::kRenderBuild, pipeline::ExecutionDomain::kRenderThread,
         pipeline::FeaturePointFlags::kMayCreateArtifacts | pipeline::FeaturePointFlags::kMayCreateGraphicsPass |
             pipeline::FeaturePointFlags::kMayCreateComputePass},
        {pipeline::StageId("RenderPipeline.Opaque"), pipeline::FeaturePointId("RenderPipeline.Opaque"),
         "RenderPipeline.Opaque", pipeline::EnginePhase::kRenderBuild, pipeline::ExecutionDomain::kRenderThread,
         pipeline::FeaturePointFlags::kMayCreateArtifacts | pipeline::FeaturePointFlags::kMayCreateGraphicsPass |
             pipeline::FeaturePointFlags::kMayCreateCopyPass | pipeline::FeaturePointFlags::kMayUseDedicatedQueue},
        {pipeline::StageId("RenderPipeline.Lighting"), pipeline::FeaturePointId("RenderPipeline.Lighting"),
         "RenderPipeline.Lighting", pipeline::EnginePhase::kRenderBuild, pipeline::ExecutionDomain::kRenderThread,
         pipeline::FeaturePointFlags::kMayCreateArtifacts | pipeline::FeaturePointFlags::kMayCreateComputePass |
             pipeline::FeaturePointFlags::kMayUseDedicatedQueue},
        {pipeline::StageId("RenderPipeline.Transparent"), pipeline::FeaturePointId("RenderPipeline.Transparent"),
         "RenderPipeline.Transparent", pipeline::EnginePhase::kRenderBuild, pipeline::ExecutionDomain::kRenderThread,
         pipeline::FeaturePointFlags::kMayCreateArtifacts | pipeline::FeaturePointFlags::kMayCreateGraphicsPass},
        {pipeline::StageId("RenderPipeline.PostProcess"), pipeline::FeaturePointId("RenderPipeline.PostProcess"),
         "RenderPipeline.PostProcess", pipeline::EnginePhase::kRenderBuild, pipeline::ExecutionDomain::kRenderThread,
         pipeline::FeaturePointFlags::kMayCreateArtifacts | pipeline::FeaturePointFlags::kMayCreateGraphicsPass |
             pipeline::FeaturePointFlags::kMayCreateComputePass},
        {pipeline::StageId("RenderPipeline.BeforePresent"), pipeline::FeaturePointId("RenderPipeline.BeforePresent"),
         "RenderPipeline.BeforePresent", pipeline::EnginePhase::kPresent, pipeline::ExecutionDomain::kRenderThread,
         pipeline::FeaturePointFlags::kMayCreateArtifacts | pipeline::FeaturePointFlags::kMayCreateGraphicsPass |
             pipeline::FeaturePointFlags::kMayCreateComputePass | pipeline::FeaturePointFlags::kMayWriteFinalOutput},
    };
    const pipeline::PipelineFeaturePointHandle visibility_point = builder.add_feature_point(make_feature_point(
        pipeline::FeaturePointId("RenderPipeline.Visibility"), "RenderPipeline.Visibility",
        pipeline::StageId("RenderPipeline.Visibility"),
        pipeline::FeaturePointFlags::kMayCreateArtifacts | pipeline::FeaturePointFlags::kMayCreateComputePass));
    if (!visibility_point) {
        return builder.status();
    }
    for (const RenderStage& stage : stages) {
        if (const foundation::Result result = add_stage_and_point(builder, stage.stage, stage.point, stage.name,
                                                                  stage.phase, stage.execution_domain, stage.flags);
            !result) {
            return result;
        }
    }
    return foundation::Result::success();
}

foundation::Result register_render_pipeline(pipeline::PipelineRegistry& registry) {
    return registry.register_pipeline([] { return std::make_unique<RenderPipeline>(); });
}
}  // namespace aeg::renderer
