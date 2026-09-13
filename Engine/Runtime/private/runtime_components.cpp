/**
 * @file runtime_components.cpp
 * @brief Implements Runtime component registration, reflection, and render extraction.
 */
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include "Components/component_bundle.hpp"
#include "Components/component_ids.hpp"
#include "Components/component_render_extraction.hpp"

namespace aeg::engine::components {
const std::string_view kTransformName{"Game.Transform"};
const std::string_view kCameraName{"Game.Camera"};
const std::string_view kMeshRendererName{"Game.MeshRenderer"};
const std::string_view kVec3TypeName{"Math.Vec3"};
const std::string_view kQuatTypeName{"Math.Quat"};
const uint32_t kRenderComponentSchemaVersion = 1;

namespace {
/** @brief Builds the ECS identity from one Runtime component's stable logical name. */
[[nodiscard]] ecs::ComponentTypeId component_type_id(std::string_view name) {
    return ecs::ComponentTypeId(name);
}

/** @brief Adapts a Vec3 member to the contract-specific aligned vector wire encoding. */
template <typename Owner>
[[nodiscard]] reflection::FieldAccessor make_vec3_accessor(math::Vec3 Owner::* member) {
    reflection::FieldAccessor accessor = reflection::make_member_accessor(member);
    accessor.value_kind = reflection::FieldValueKind::kVec3;
    return accessor;
}

/** @brief Adapts a quaternion member to the contract-specific XYZW wire encoding. */
template <typename Owner>
[[nodiscard]] reflection::FieldAccessor make_quat_accessor(math::Quat Owner::* member) {
    reflection::FieldAccessor accessor = reflection::make_member_accessor(member);
    accessor.value_kind = reflection::FieldValueKind::kQuat;
    return accessor;
}

/** @brief Compares stable Reflection metadata without comparing type-erased accessor callables. */
[[nodiscard]] bool has_compatible_metadata(const reflection::TypeDescriptor& existing,
                                           const reflection::TypeDescriptor& expected) noexcept {
    if (existing.id != expected.id || existing.name != expected.name || existing.version != expected.version ||
        existing.kind != expected.kind || existing.size != expected.size || existing.alignment != expected.alignment ||
        existing.fields.size() != expected.fields.size()) {
        return false;
    }
    for (std::size_t index = 0; index < existing.fields.size(); ++index) {
        const reflection::FieldDescriptor& actual = existing.fields[index];
        const reflection::FieldDescriptor& required = expected.fields[index];
        if (actual.id != required.id || actual.name != required.name || actual.type != required.type ||
            actual.value_kind != required.value_kind || actual.flags != required.flags ||
            actual.accessor.value_kind != required.accessor.value_kind ||
            actual.accessor.value_size != required.accessor.value_size ||
            actual.accessor.value_alignment != required.accessor.value_alignment) {
            return false;
        }
    }
    return true;
}

/** @brief Registers one schema once and identifies whether this call inserted it. */
[[nodiscard]] foundation::ResultValue<bool> ensure_registered(reflection::TypeRegistry& registry,
                                                              const reflection::TypeDescriptor& descriptor) {
    const auto existing = registry.find_type(descriptor.id);
    if (existing) {
        if (!has_compatible_metadata(*existing.value, descriptor)) {
            return {foundation::invalid_state("registered reflection type conflicts with Runtime component schema"),
                    false};
        }
        return {foundation::Result::success(), false};
    }
    if (existing.result.code != foundation::ResultCode::kNotFound) {
        return {existing.result, false};
    }

    const foundation::Result registration = registry.register_type(descriptor);
    if (registration) {
        return {foundation::Result::success(), true};
    }
    if (registration.code != foundation::ResultCode::kInvalidState) {
        return {registration, false};
    }

    const auto concurrent = registry.find_type(descriptor.id);
    if (concurrent && has_compatible_metadata(*concurrent.value, descriptor)) {
        return {foundation::Result::success(), false};
    }
    return {foundation::invalid_state("registered reflection type conflicts with Runtime component schema"), false};
}

/** @brief Tests whether an existing raw ECS component matches the Runtime storage contract. */
template <typename Component>
[[nodiscard]] bool has_compatible_raw_component_descriptor(const ecs::ComponentDescriptor& existing,
                                                           ecs::ComponentTypeId id, std::string_view name) noexcept {
    return existing.id == id && existing.name == name && existing.size == sizeof(Component) &&
           existing.alignment == alignof(Component) && existing.snapshotable && !existing.schema_type.is_valid() &&
           existing.schema_version == 0;
}

/** @brief Registers raw Runtime component storage while rejecting identity conflicts. */
template <typename Component>
[[nodiscard]] foundation::Result ensure_raw_component_storage(ecs::World& world, ecs::ComponentTypeId id,
                                                              std::string_view name) {
    const auto existing = world.find_component_descriptor(id);
    if (existing) {
        return has_compatible_raw_component_descriptor<Component>(existing.value, id, name)
                   ? foundation::Result::success()
                   : foundation::invalid_state("registered ECS component conflicts with Runtime component schema");
    }
    if (existing.result.code != foundation::ResultCode::kNotFound) {
        return existing.result;
    }
    if (world.component_type_id<Component>().is_valid()) {
        return foundation::invalid_state(
            "C++ component type is registered with a different Runtime component identity");
    }
    return world.register_component<Component>(id, name);
}

/** @brief Tests whether an existing ECS component matches one reflected Runtime schema. */
[[nodiscard]] bool has_compatible_reflected_component_descriptor(const ecs::ComponentDescriptor& existing,
                                                                 const reflection::TypeDescriptor& schema,
                                                                 ecs::ComponentTypeId component_id,
                                                                 std::string_view name) noexcept {
    return existing.id == component_id && existing.name == name && existing.size == schema.size &&
           existing.alignment == schema.alignment && existing.snapshotable && existing.schema_type == schema.id &&
           existing.schema_version == schema.version;
}

/** @brief Registers reflected Runtime component storage while rejecting identity conflicts. */
template <typename Component>
[[nodiscard]] foundation::Result ensure_reflected_component_storage(ecs::World& world,
                                                                    const reflection::TypeDescriptor& schema,
                                                                    ecs::ComponentTypeId component_id,
                                                                    std::string_view name) {
    const auto existing = world.find_component_descriptor(component_id);
    if (existing) {
        return has_compatible_reflected_component_descriptor(existing.value, schema, component_id, name)
                   ? foundation::Result::success()
                   : foundation::invalid_state("registered ECS component conflicts with Runtime component schema");
    }
    if (existing.result.code != foundation::ResultCode::kNotFound) {
        return existing.result;
    }
    if (world.component_type_id<Component>().is_valid()) {
        return foundation::invalid_state(
            "C++ component type is registered with a different Runtime component identity");
    }
    return world.register_reflected_component<Component>(schema, component_id, name);
}

/** @brief Resolves one registered schema before binding it to a World component pool. */
template <typename Component>
[[nodiscard]] foundation::Result register_component_from_schema(
    ecs::World& world, const reflection::TypeRegistry& registry,
    foundation::ResultValue<reflection::TypeDescriptor> expected_schema, ecs::ComponentTypeId component_id,
    std::string_view name) {
    if (!expected_schema) {
        return expected_schema.result;
    }
    const auto registered_schema = registry.find_type(expected_schema.value.id);
    if (!registered_schema) {
        return registered_schema.result;
    }
    if (!has_compatible_metadata(*registered_schema.value, expected_schema.value)) {
        return foundation::invalid_state("registered reflection type conflicts with Runtime component schema");
    }
    return ensure_reflected_component_storage<Component>(world, *registered_schema.value, component_id, name);
}
}  // namespace

foundation::ResultValue<reflection::TypeDescriptor> transform_reflection_descriptor() {
    reflection::TypeBuilder builder(reflection::TypeDescriptor{
        reflection::TypeId(kTransformName), std::string(kTransformName), kRenderComponentSchemaVersion,
        reflection::TypeKind::kComponent, sizeof(Transform), alignof(Transform)});
    if (const foundation::Result result = builder.add_field("translation", reflection::TypeId(kVec3TypeName),
                                                            make_vec3_accessor(&Transform::translation));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("rotation", reflection::TypeId(kQuatTypeName), make_quat_accessor(&Transform::rotation));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("scale", reflection::TypeId(kVec3TypeName), make_vec3_accessor(&Transform::scale));
        !result) {
        return {result, {}};
    }
    return std::move(builder).finish();
}

foundation::ResultValue<reflection::TypeDescriptor> camera_reflection_descriptor() {
    reflection::TypeBuilder builder(reflection::TypeDescriptor{
        reflection::TypeId(kCameraName), std::string(kCameraName), kRenderComponentSchemaVersion,
        reflection::TypeKind::kComponent, sizeof(Camera), alignof(Camera)});
    if (const foundation::Result result =
            builder.add_field("vertical_fov_radians", reflection::type_id<float>(),
                              reflection::make_member_accessor(&Camera::vertical_fov_radians));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field("near_plane", reflection::type_id<float>(),
                                                            reflection::make_member_accessor(&Camera::near_plane));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field("far_plane", reflection::type_id<float>(),
                                                            reflection::make_member_accessor(&Camera::far_plane));
        !result) {
        return {result, {}};
    }
    return std::move(builder).finish();
}

foundation::ResultValue<reflection::TypeDescriptor> mesh_renderer_reflection_descriptor() {
    reflection::TypeBuilder builder(reflection::TypeDescriptor{
        reflection::TypeId(kMeshRendererName), std::string(kMeshRendererName), kRenderComponentSchemaVersion,
        reflection::TypeKind::kComponent, sizeof(MeshRenderer), alignof(MeshRenderer)});
    if (const foundation::Result result =
            builder.add_field("mesh_asset_id", reflection::type_id<uint64_t>(),
                              reflection::make_member_accessor(&MeshRenderer::mesh_asset_id));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("material_asset_id", reflection::type_id<uint64_t>(),
                              reflection::make_member_accessor(&MeshRenderer::material_asset_id));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field("bounds_center", reflection::TypeId(kVec3TypeName),
                                                            make_vec3_accessor(&MeshRenderer::bounds_center));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("bounds_radius", reflection::type_id<float>(),
                              reflection::make_member_accessor(&MeshRenderer::bounds_radius));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("visibility_mask", reflection::type_id<uint32_t>(),
                              reflection::make_member_accessor(&MeshRenderer::visibility_mask));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field("visible", reflection::type_id<bool>(),
                                                            reflection::make_member_accessor(&MeshRenderer::visible));
        !result) {
        return {result, {}};
    }
    return std::move(builder).finish();
}

foundation::Result register_render_component_reflection(reflection::TypeRegistry& registry) {
    auto transform = transform_reflection_descriptor();
    if (!transform) {
        return transform.result;
    }
    auto camera = camera_reflection_descriptor();
    if (!camera) {
        return camera.result;
    }
    auto mesh_renderer = mesh_renderer_reflection_descriptor();
    if (!mesh_renderer) {
        return mesh_renderer.result;
    }

    const std::array<reflection::TypeDescriptor, 3> descriptors{
        std::move(transform.value),
        std::move(camera.value),
        std::move(mesh_renderer.value),
    };
    std::array<reflection::TypeId, descriptors.size()> inserted{};
    std::size_t inserted_count = 0;
    for (const reflection::TypeDescriptor& descriptor : descriptors) {
        const auto registered = ensure_registered(registry, descriptor);
        if (!registered) {
            while (inserted_count != 0) {
                --inserted_count;
                static_cast<void>(registry.unregister_type(inserted[inserted_count]));
            }
            return registered.result;
        }
        if (registered.value) {
            inserted[inserted_count++] = descriptor.id;
        }
    }
    return foundation::Result::success();
}

foundation::Result register_render_components(ecs::World& world) {
    if (const foundation::Result result =
            ensure_raw_component_storage<Transform>(world, component_type_id(kTransformName), kTransformName);
        !result) {
        return result;
    }
    if (const foundation::Result result =
            ensure_raw_component_storage<Camera>(world, component_type_id(kCameraName), kCameraName);
        !result) {
        return result;
    }
    return ensure_raw_component_storage<MeshRenderer>(world, component_type_id(kMeshRendererName), kMeshRendererName);
}

foundation::Result register_reflected_render_components(ecs::World& world, const reflection::TypeRegistry& registry) {
    if (const foundation::Result result = register_component_from_schema<Transform>(
            world, registry, transform_reflection_descriptor(), component_type_id(kTransformName), kTransformName);
        !result) {
        return result;
    }
    if (const foundation::Result result = register_component_from_schema<Camera>(
            world, registry, camera_reflection_descriptor(), component_type_id(kCameraName), kCameraName);
        !result) {
        return result;
    }
    return register_component_from_schema<MeshRenderer>(world, registry, mesh_renderer_reflection_descriptor(),
                                                        component_type_id(kMeshRendererName), kMeshRendererName);
}

foundation::Result register_render_component_bundle(ecs::World& world, reflection::TypeRegistry& registry) {
    if (const foundation::Result result = register_render_component_reflection(registry); !result) {
        return result;
    }
    return register_reflected_render_components(world, registry);
}

foundation::ResultValue<renderer::RenderCamera> make_render_camera(const Transform& transform, const Camera& camera,
                                                                   float aspect_ratio,
                                                                   math::ClipSpaceConvention clip_space_convention) {
    if (!std::isfinite(aspect_ratio) || aspect_ratio <= 0.0f) {
        return {foundation::invalid_argument("camera aspect ratio must be finite and positive"), {}};
    }

    math::Mat4 view{};
    if (!math::try_inverse(transform.to_matrix(), view)) {
        return {foundation::invalid_argument("camera transform cannot produce a finite inverse view matrix"), {}};
    }
    math::Mat4 projection{};
    if (!math::try_perspective(camera.vertical_fov_radians, aspect_ratio, camera.near_plane, camera.far_plane,
                               projection, clip_space_convention)) {
        return {foundation::invalid_argument("camera projection parameters are invalid"), {}};
    }
    return {foundation::Result::success(),
            renderer::RenderCamera{view, projection, transform.translation, camera.near_plane, camera.far_plane,
                                   clip_space_convention}};
}

foundation::ResultValue<renderer::RenderView> make_render_view(const ecs::WorldSnapshot& world_snapshot,
                                                               const RuntimeRenderViewDesc& desc) {
    if (!world_snapshot.is_valid()) {
        return {foundation::invalid_argument("Runtime RenderView requires a valid WorldSnapshot"), {}};
    }
    if (!desc.camera_entity.is_valid()) {
        return {foundation::invalid_argument("Runtime RenderView camera entity is invalid"), {}};
    }
    if (!world_snapshot.contains(desc.camera_entity)) {
        return {foundation::not_found("Runtime RenderView camera entity is not present in the WorldSnapshot"), {}};
    }
    if (!desc.output_extent.is_valid()) {
        return {foundation::invalid_argument("Runtime RenderView output extent is invalid"), {}};
    }

    const Transform* transform = world_snapshot.get<Transform>(desc.camera_entity);
    if (transform == nullptr) {
        return {foundation::not_found("Runtime RenderView camera entity has no Transform component"), {}};
    }
    const Camera* camera = world_snapshot.get<Camera>(desc.camera_entity);
    if (camera == nullptr) {
        return {foundation::not_found("Runtime RenderView camera entity has no Camera component"), {}};
    }

    const float aspect_ratio =
        static_cast<float>(desc.output_extent.width) / static_cast<float>(desc.output_extent.height);
    const auto render_camera = make_render_camera(*transform, *camera, aspect_ratio, desc.clip_space_convention);
    if (!render_camera) {
        return {render_camera.result, {}};
    }

    renderer::RenderView view{};
    view.id = renderer::make_render_view_id(world_snapshot.world(), desc.view_id);
    view.world = world_snapshot.world();
    view.view_id = desc.view_id;
    view.output_extent = desc.output_extent;
    view.camera = render_camera.value;
    view.visibility_mask = desc.visibility_mask;
    view.color_target = desc.color_target;
    view.depth_target = desc.depth_target;
    view.frustum_culling = desc.frustum_culling;
    if (!view.is_valid()) {
        return {foundation::invalid_state("Runtime RenderView violates the renderer view contract"), {}};
    }
    return {foundation::Result::success(), std::move(view)};
}

foundation::Result extract_mesh_renderers(const renderer::RenderExtractContext& context) {
    foundation::Result result = foundation::Result::success();
    context.world_snapshot.for_each<MeshRenderer>(
        [&context, &result](ecs::EntityId entity, const MeshRenderer& mesh_renderer) {
            if (!result || !mesh_renderer.visible) {
                return;
            }
            const Transform* transform = context.world_snapshot.get<Transform>(entity);
            if (transform == nullptr) {
                return;
            }
            const renderer::MeshAssetId mesh(mesh_renderer.mesh_asset_id);
            const renderer::MaterialInstanceId material(mesh_renderer.material_asset_id);
            if (!mesh.is_valid() || !material.is_valid()) {
                return;
            }
            const math::Mat4 world_transform = transform->to_matrix();
            result = context.builder.add_instance({renderer::make_render_object_id(entity), mesh, material,
                                                   world_transform, world_transform, mesh_renderer.bounds_center,
                                                   mesh_renderer.bounds_radius, mesh_renderer.visibility_mask});
        });
    return result;
}

foundation::ResultValue<renderer::RenderExtractorId> register_mesh_renderer_extractor(renderer::Renderer& renderer) {
    return renderer.register_extractor(&extract_mesh_renderers);
}
}  // namespace aeg::engine::components
