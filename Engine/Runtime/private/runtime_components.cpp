/**
 * @file runtime_components.cpp
 * @brief Implements Runtime component registration, reflection, and render extraction.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

#include "Components/component_bundle.hpp"
#include "Components/component_ids.hpp"
#include "Components/component_render_extraction.hpp"
#include "post_process.hpp"

namespace aeg::engine::components {
const std::string_view kTransformName{"Game.Transform"};
const std::string_view kCameraName{"Game.Camera"};
const std::string_view kMeshRendererName{"Game.MeshRenderer"};
const std::string_view kTextRendererName{"Game.TextRenderer"};
const std::string_view kDirectionalLightName{"Game.DirectionalLight"};
const std::string_view kPointLightName{"Game.PointLight"};
const std::string_view kSpotLightName{"Game.SpotLight"};
const std::string_view kSceneEnvironmentName{"Game.SceneEnvironment"};
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
    if (const foundation::Result result =
            builder.add_field("tint", reflection::TypeId(kVec3TypeName), make_vec3_accessor(&MeshRenderer::tint));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field(
            "tint_alpha", reflection::type_id<float>(), reflection::make_member_accessor(&MeshRenderer::tint_alpha));
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

foundation::ResultValue<reflection::TypeDescriptor> text_renderer_reflection_descriptor() {
    reflection::TypeBuilder builder(reflection::TypeDescriptor{
        reflection::TypeId(kTextRendererName), std::string(kTextRendererName), kRenderComponentSchemaVersion,
        reflection::TypeKind::kComponent, sizeof(TextRenderer), alignof(TextRenderer)});
    if (const foundation::Result result =
            builder.add_field("font_asset_id", reflection::type_id<uint64_t>(),
                              reflection::make_member_accessor(&TextRenderer::font_asset_id));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field("text", reflection::type_id<std::string>(),
                                                            reflection::make_member_accessor(&TextRenderer::text));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field(
            "world_size", reflection::type_id<float>(), reflection::make_member_accessor(&TextRenderer::world_size));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("color", reflection::TypeId(kVec3TypeName), make_vec3_accessor(&TextRenderer::color));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field(
            "color_alpha", reflection::type_id<float>(), reflection::make_member_accessor(&TextRenderer::color_alpha));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("visibility_mask", reflection::type_id<uint32_t>(),
                              reflection::make_member_accessor(&TextRenderer::visibility_mask));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field("visible", reflection::type_id<bool>(),
                                                            reflection::make_member_accessor(&TextRenderer::visible));
        !result) {
        return {result, {}};
    }
    return std::move(builder).finish();
}

foundation::ResultValue<reflection::TypeDescriptor> directional_light_reflection_descriptor() {
    reflection::TypeBuilder builder(reflection::TypeDescriptor{
        reflection::TypeId(kDirectionalLightName), std::string(kDirectionalLightName), kRenderComponentSchemaVersion,
        reflection::TypeKind::kComponent, sizeof(DirectionalLight), alignof(DirectionalLight)});
    if (const foundation::Result result = builder.add_field("direction", reflection::TypeId(kVec3TypeName),
                                                            make_vec3_accessor(&DirectionalLight::direction));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("color", reflection::TypeId(kVec3TypeName), make_vec3_accessor(&DirectionalLight::color));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field(
            "intensity", reflection::type_id<float>(), reflection::make_member_accessor(&DirectionalLight::intensity));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result = builder.add_field(
            "enabled", reflection::type_id<bool>(), reflection::make_member_accessor(&DirectionalLight::enabled));
        !result) {
        return {result, {}};
    }
    if (const auto result = builder.add_field("casts_shadow", reflection::type_id<bool>(),
                                              reflection::make_member_accessor(&DirectionalLight::casts_shadow));
        !result)
        return {result, {}};
    return std::move(builder).finish();
}

namespace {
template <typename Light>
foundation::ResultValue<reflection::TypeDescriptor> local_light_descriptor(std::string_view name) {
    reflection::TypeBuilder builder({reflection::TypeId(name), std::string(name), kRenderComponentSchemaVersion,
                                     reflection::TypeKind::kComponent, sizeof(Light), alignof(Light)});
    if (const auto r = builder.add_field("color", reflection::TypeId(kVec3TypeName), make_vec3_accessor(&Light::color));
        !r)
        return {r, {}};
    if (const auto r = builder.add_field("intensity", reflection::type_id<float>(),
                                         reflection::make_member_accessor(&Light::intensity));
        !r)
        return {r, {}};
    if (const auto r =
            builder.add_field("range", reflection::type_id<float>(), reflection::make_member_accessor(&Light::range));
        !r)
        return {r, {}};
    if (const auto r = builder.add_field("visibility_mask", reflection::type_id<uint32_t>(),
                                         reflection::make_member_accessor(&Light::visibility_mask));
        !r)
        return {r, {}};
    if (const auto r = builder.add_field("enabled", reflection::type_id<bool>(),
                                         reflection::make_member_accessor(&Light::enabled));
        !r)
        return {r, {}};
    if constexpr (std::is_same_v<Light, SpotLight>) {
        if (const auto r = builder.add_field("direction", reflection::TypeId(kVec3TypeName),
                                             make_vec3_accessor(&Light::direction));
            !r)
            return {r, {}};
        if (const auto r = builder.add_field("inner_cone_cosine", reflection::type_id<float>(),
                                             reflection::make_member_accessor(&Light::inner_cone_cosine));
            !r)
            return {r, {}};
        if (const auto r = builder.add_field("outer_cone_cosine", reflection::type_id<float>(),
                                             reflection::make_member_accessor(&Light::outer_cone_cosine));
            !r)
            return {r, {}};
    }
    return std::move(builder).finish();
}
}  // namespace

foundation::ResultValue<reflection::TypeDescriptor> point_light_reflection_descriptor() {
    return local_light_descriptor<PointLight>(kPointLightName);
}
foundation::ResultValue<reflection::TypeDescriptor> spot_light_reflection_descriptor() {
    return local_light_descriptor<SpotLight>(kSpotLightName);
}

foundation::ResultValue<reflection::TypeDescriptor> scene_environment_reflection_descriptor() {
    reflection::TypeBuilder builder({reflection::TypeId(kSceneEnvironmentName), std::string(kSceneEnvironmentName),
                                     kRenderComponentSchemaVersion, reflection::TypeKind::kComponent,
                                     sizeof(SceneEnvironment), alignof(SceneEnvironment)});
    for (auto [name, member] : std::array{std::pair{"sky_zenith", &SceneEnvironment::sky_zenith},
                                          std::pair{"sky_horizon", &SceneEnvironment::sky_horizon},
                                          std::pair{"sky_ground", &SceneEnvironment::sky_ground}})
        if (const auto r = builder.add_field(name, reflection::TypeId(kVec3TypeName), make_vec3_accessor(member)); !r)
            return {r, {}};
    for (auto [name, member] :
         std::array{std::pair{"exposure", &SceneEnvironment::exposure}, std::pair{"gamma", &SceneEnvironment::gamma}})
        if (const auto r =
                builder.add_field(name, reflection::type_id<float>(), reflection::make_member_accessor(member));
            !r)
            return {r, {}};
    for (auto [name, member] : std::array{std::pair{"sky_enabled", &SceneEnvironment::sky_enabled},
                                          std::pair{"tone_mapping", &SceneEnvironment::tone_mapping},
                                          std::pair{"enabled", &SceneEnvironment::enabled}})
        if (const auto r =
                builder.add_field(name, reflection::type_id<bool>(), reflection::make_member_accessor(member));
            !r)
            return {r, {}};
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
    auto text_renderer = text_renderer_reflection_descriptor();
    if (!text_renderer) {
        return text_renderer.result;
    }
    auto directional_light = directional_light_reflection_descriptor();
    if (!directional_light) {
        return directional_light.result;
    }

    auto point_light = point_light_reflection_descriptor();
    auto spot_light = spot_light_reflection_descriptor();
    auto environment = scene_environment_reflection_descriptor();
    if (!environment)
        return environment.result;
    if (!point_light || !spot_light)
        return !point_light ? point_light.result : spot_light.result;
    const std::array<reflection::TypeDescriptor, 8> descriptors{
        std::move(transform.value),         std::move(camera.value),
        std::move(mesh_renderer.value),     std::move(text_renderer.value),
        std::move(directional_light.value), std::move(point_light.value),
        std::move(spot_light.value),        std::move(environment.value),
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
    if (const foundation::Result result =
            ensure_raw_component_storage<MeshRenderer>(world, component_type_id(kMeshRendererName), kMeshRendererName);
        !result) {
        return result;
    }
    if (const foundation::Result result =
            ensure_raw_component_storage<TextRenderer>(world, component_type_id(kTextRendererName), kTextRendererName);
        !result) {
        return result;
    }
    if (const auto r = ensure_raw_component_storage<DirectionalLight>(world, component_type_id(kDirectionalLightName),
                                                                      kDirectionalLightName);
        !r)
        return r;
    if (const auto r =
            ensure_raw_component_storage<PointLight>(world, component_type_id(kPointLightName), kPointLightName);
        !r)
        return r;
    if (const auto r =
            ensure_raw_component_storage<SpotLight>(world, component_type_id(kSpotLightName), kSpotLightName);
        !r)
        return r;
    return ensure_raw_component_storage<SceneEnvironment>(world, component_type_id(kSceneEnvironmentName),
                                                          kSceneEnvironmentName);
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
    if (const foundation::Result result =
            register_component_from_schema<MeshRenderer>(world, registry, mesh_renderer_reflection_descriptor(),
                                                         component_type_id(kMeshRendererName), kMeshRendererName);
        !result) {
        return result;
    }
    if (const foundation::Result result =
            register_component_from_schema<TextRenderer>(world, registry, text_renderer_reflection_descriptor(),
                                                         component_type_id(kTextRendererName), kTextRendererName);
        !result) {
        return result;
    }
    if (const auto r = register_component_from_schema<DirectionalLight>(
            world, registry, directional_light_reflection_descriptor(), component_type_id(kDirectionalLightName),
            kDirectionalLightName);
        !r)
        return r;
    if (const auto r = register_component_from_schema<PointLight>(world, registry, point_light_reflection_descriptor(),
                                                                  component_type_id(kPointLightName), kPointLightName);
        !r)
        return r;
    if (const auto r = register_component_from_schema<SpotLight>(world, registry, spot_light_reflection_descriptor(),
                                                                 component_type_id(kSpotLightName), kSpotLightName);
        !r)
        return r;
    return register_component_from_schema<SceneEnvironment>(world, registry, scene_environment_reflection_descriptor(),
                                                            component_type_id(kSceneEnvironmentName),
                                                            kSceneEnvironmentName);
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
    foundation::Result environment_result = foundation::Result::success();
    uint32_t environment_count = 0;
    world_snapshot.for_each<SceneEnvironment>([&](ecs::EntityId, const SceneEnvironment& environment) {
        if (!environment_result || !environment.enabled)
            return;
        if (++environment_count > 1) {
            environment_result =
                foundation::invalid_state("A render view requires at most one enabled SceneEnvironment");
            return;
        }
        const auto settings = make_post_process_settings(environment);
        if (!settings) {
            environment_result = settings.result;
            return;
        }
        view.post_process = settings.value;
    });
    if (!environment_result)
        return {environment_result, {}};
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

foundation::Result extract_text_renderers(const renderer::RenderExtractContext& context) {
    foundation::Result result = foundation::Result::success();
    context.world_snapshot.for_each<TextRenderer>([&context, &result](ecs::EntityId entity, const TextRenderer& text) {
        if (!result || !text.visible || text.text.empty() || !std::isfinite(text.world_size) ||
            text.world_size <= 0.0f || !std::isfinite(text.color.x) || !std::isfinite(text.color.y) ||
            !std::isfinite(text.color.z) || text.color.x < 0.0f || text.color.x > 1.0f || text.color.y < 0.0f ||
            text.color.y > 1.0f || text.color.z < 0.0f || text.color.z > 1.0f || !std::isfinite(text.color_alpha) ||
            text.color_alpha < 0.0f || text.color_alpha > 1.0f) {
            if (!text.visible || text.text.empty()) {
                return;
            }
            result = foundation::invalid_argument("TextRenderer contains invalid text, size, or color values");
            return;
        }
        const Transform* transform = context.world_snapshot.get<Transform>(entity);
        if (transform == nullptr) {
            return;
        }
        const uint64_t text_hash = text_asset_hash(text.font_asset_id, text.text, text.world_size);
        const renderer::MeshAssetId mesh(text_mesh_asset_id(text.font_asset_id, text.text, text.world_size));
        const renderer::MaterialInstanceId material(
            text_material_asset_id(text.font_asset_id, text.text, text.world_size));
        const renderer::RenderObjectId object(renderer::make_render_object_id(entity).value() ^ text_hash);
        const math::Mat4 world_transform = transform->to_matrix();
        result = context.builder.add_instance({object,
                                               mesh,
                                               material,
                                               world_transform,
                                               world_transform,
                                               {},
                                               std::max(0.01f, text.world_size * static_cast<float>(text.text.size())),
                                               text.visibility_mask});
    });
    return result;
}

foundation::ResultValue<renderer::RenderExtractorId> register_text_renderer_extractor(renderer::Renderer& renderer) {
    return renderer.register_extractor(&extract_text_renderers);
}

foundation::Result extract_directional_lights(const renderer::RenderExtractContext& context) {
    foundation::Result result = foundation::Result::success();
    context.world_snapshot.for_each<DirectionalLight>(
        [&context, &result](ecs::EntityId entity, const DirectionalLight& light) {
            if (!result || !light.enabled) {
                return;
            }
            if (!std::isfinite(light.intensity) || light.intensity < 0.0f || !std::isfinite(light.direction.x) ||
                !std::isfinite(light.direction.y) || !std::isfinite(light.direction.z) ||
                !std::isfinite(light.color.x) || !std::isfinite(light.color.y) || !std::isfinite(light.color.z) ||
                light.color.x < 0.0f || light.color.x > 1.0f || light.color.y < 0.0f || light.color.y > 1.0f ||
                light.color.z < 0.0f || light.color.z > 1.0f) {
                result = foundation::invalid_argument("Directional Light contains invalid editor values");
                return;
            }
            math::Vec3 direction{};
            if (light.intensity > 0.0f && !math::try_normalize(light.direction, direction)) {
                result = foundation::invalid_argument("Directional Light direction must be non-zero");
                return;
            }
            result = context.builder.add_light({renderer::make_render_light_id(entity),
                                                renderer::RenderLightType::kDirectional,
                                                {},
                                                direction,
                                                light.color,
                                                light.intensity,
                                                0.0f,
                                                1.0f,
                                                0.0f,
                                                0xffffffffu,
                                                light.casts_shadow});
        });
    return result;
}

foundation::ResultValue<renderer::RenderExtractorId> register_directional_light_extractor(
    renderer::Renderer& renderer) {
    return renderer.register_extractor(&extract_directional_lights);
}

foundation::ResultValue<renderer::RenderPostProcessSettings> make_post_process_settings(
    const SceneEnvironment& environment) {
    renderer::RenderPostProcessSettings settings{
        environment.sky_zenith, environment.sky_horizon, environment.sky_ground,  environment.exposure,
        environment.gamma,      environment.sky_enabled, environment.tone_mapping};
    const auto result = renderer::validate_post_process_settings(settings);
    return {result, settings};
}

foundation::Result validate_point_light(const PointLight& light) {
    const auto unit = [](float value) { return math::is_finite(value) && value >= 0 && value <= 1; };
    if (!unit(light.color.x) || !unit(light.color.y) || !unit(light.color.z) || !math::is_finite(light.intensity) ||
        light.intensity < 0 || !math::is_finite(light.range) || light.range < 0)
        return foundation::invalid_argument(
            "Local light requires finite color [0,1], non-negative intensity and range");
    return foundation::Result::success();
}

foundation::Result validate_spot_light(const SpotLight& light) {
    if (const auto r =
            validate_point_light({light.color, light.intensity, light.range, light.visibility_mask, light.enabled});
        !r)
        return r;
    math::Vec3 normalized{};
    if (!math::try_normalize(light.direction, normalized) || !math::is_finite(light.inner_cone_cosine) ||
        !math::is_finite(light.outer_cone_cosine) || light.outer_cone_cosine < -1 || light.inner_cone_cosine > 1 ||
        light.outer_cone_cosine > light.inner_cone_cosine)
        return foundation::invalid_argument(
            "Spot light requires a direction and -1 <= outer <= inner <= 1 cone cosines");
    return foundation::Result::success();
}

foundation::Result extract_local_lights(const renderer::RenderExtractContext& context) {
    auto result = foundation::Result::success();
    const auto extract = [&]<typename Light>() {
        context.world_snapshot.for_each<Light>([&](ecs::EntityId entity, const Light& light) {
            if (!result || !light.enabled)
                return;
            constexpr bool kSpot = std::is_same_v<Light, SpotLight>;
            if constexpr (kSpot)
                result = validate_spot_light(light);
            else
                result = validate_point_light(light);
            if (!result)
                return;
            const auto* transform = context.world_snapshot.get<Transform>(entity);
            if (!transform)
                return;
            // Include the component identity so multiple light types on one entity remain distinct.
            const auto type = ecs::ComponentTypeId(kSpot ? kSpotLightName : kPointLightName);
            renderer::RenderLight output{};
            output.light = renderer::RenderLightId(renderer::make_render_light_id(entity).value() ^ type.value());
            output.type = kSpot ? renderer::RenderLightType::kSpot : renderer::RenderLightType::kPoint;
            output.position = transform->translation;
            output.color = light.color;
            output.intensity = light.intensity;
            output.range = light.range;
            output.visibility_mask = light.visibility_mask;
            if constexpr (kSpot) {
                if (!math::try_normalize(math::rotate(transform->rotation, light.direction), output.direction)) {
                    result = foundation::invalid_argument("Spot light world direction is invalid");
                    return;
                }
                output.inner_cone_cosine = light.inner_cone_cosine;
                output.outer_cone_cosine = light.outer_cone_cosine;
            }
            result = context.builder.add_light(output);
        });
    };
    extract.operator()<PointLight>();
    extract.operator()<SpotLight>();
    return result;
}

foundation::ResultValue<renderer::RenderExtractorId> register_local_light_extractor(renderer::Renderer& renderer) {
    return renderer.register_extractor(&extract_local_lights);
}
}  // namespace aeg::engine::components
