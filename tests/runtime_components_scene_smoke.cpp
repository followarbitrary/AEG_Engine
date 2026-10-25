/**
 * @file runtime_components_scene_smoke.cpp
 * @brief Exercises Runtime component registration, Scene persistence, and Renderer extraction.
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

#include "Components/component_bundle.hpp"
#include "Components/component_ids.hpp"
#include "Components/component_render_extraction.hpp"
#include "scene_asset.hpp"

namespace {
/** @brief Reports one failed contract assertion and keeps smoke-test exits concise. */
template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

/** @brief Verifies the fixed four-float encodings required for Transform's Math fields. */
bool verify_transform_encoding(const aeg::reflection::TypeRegistry& registry,
                               const aeg::engine::components::Transform& source) {
    aeg::serialization::BinaryWriter writer;
    if (!require(aeg::serialization::serialize_object(
                     registry, aeg::reflection::TypeId(aeg::engine::components::kTransformName), &source, writer),
                 "Transform serialization failed")) {
        return false;
    }

    aeg::serialization::BinaryReader reader(writer.data());
    const auto type = reader.read_u64();
    const auto version = reader.read_u32();
    const auto field_count = reader.read_u32();
    if (!require(type && version && field_count, "Transform archive header is truncated") ||
        !require(type.value == aeg::reflection::TypeId(aeg::engine::components::kTransformName).value() &&
                     version.value == aeg::engine::components::kRenderComponentSchemaVersion && field_count.value == 3,
                 "Transform archive header is invalid")) {
        return false;
    }

    uint32_t vec3_field_count = 0;
    uint32_t quat_field_count = 0;
    for (uint32_t index = 0; index < field_count.value; ++index) {
        const auto field = reader.read_u64();
        const auto payload_size = reader.read_u32();
        if (!require(field && payload_size, "Transform archive field header is truncated")) {
            return false;
        }
        const auto payload = reader.read_bytes(payload_size.value);
        if (!require(payload, "Transform archive field payload is truncated")) {
            return false;
        }
        if (field.value == aeg::reflection::FieldId("translation").value() ||
            field.value == aeg::reflection::FieldId("scale").value()) {
            if (!require(payload_size.value == sizeof(aeg::math::Vec3),
                         "Vec3 was serialized as a packed three-float payload")) {
                return false;
            }
            ++vec3_field_count;
        } else if (field.value == aeg::reflection::FieldId("rotation").value()) {
            if (!require(payload_size.value == sizeof(aeg::math::Quat), "Quat payload size is invalid")) {
                return false;
            }
            ++quat_field_count;
        }
    }
    if (!require(reader.remaining() == 0 && vec3_field_count == 2 && quat_field_count == 1,
                 "Transform archive field layout is invalid")) {
        return false;
    }

    aeg::engine::components::Transform restored{};
    aeg::serialization::BinaryReader restore_reader(writer.data());
    if (!require(
            aeg::serialization::deserialize_object(
                registry, aeg::reflection::TypeId(aeg::engine::components::kTransformName), restore_reader, &restored),
            "Transform deserialization failed") ||
        !require(restore_reader.remaining() == 0, "Transform deserializer left unread bytes") ||
        !require(aeg::math::nearly_equal(restored.translation, source.translation) &&
                     aeg::math::nearly_equal(restored.rotation, source.rotation) &&
                     aeg::math::nearly_equal(restored.scale, source.scale),
                 "Transform semantic values did not round-trip")) {
        return false;
    }

    std::array<float, 4> translation_storage{};
    std::memcpy(translation_storage.data(), &restored.translation, sizeof(translation_storage));
    return require(translation_storage[3] == 0.0f, "Vec3 padding was not canonicalized during deserialization");
}
}  // namespace

int main() {
    using aeg::engine::components::Camera;
    using aeg::engine::components::MeshRenderer;
    using aeg::engine::components::Transform;

    constexpr aeg::scene::SceneAssetId kAssetId{0x91a0b0c0d0e0f001ull, 0x1112131415161718ull};
    constexpr aeg::scene::SceneEntityId kRootId{0x1000000000000001ull, 0x2000000000000001ull};
    constexpr aeg::scene::SceneEntityId kCameraEntityId{0x1000000000000002ull, 0x2000000000000002ull};
    constexpr aeg::scene::SceneEntityId kMeshEntityId{0x1000000000000003ull, 0x2000000000000003ull};

    aeg::reflection::TypeRegistry registry;
    if (!require(aeg::engine::components::register_render_component_reflection(registry),
                 "Runtime component Reflection registration failed") ||
        !require(aeg::engine::components::register_render_component_reflection(registry),
                 "Runtime component Reflection registration is not idempotent")) {
        return 1;
    }

    const auto transform_schema = registry.find_type(aeg::reflection::TypeId(aeg::engine::components::kTransformName));
    const auto camera_schema = registry.find_type(aeg::reflection::TypeId(aeg::engine::components::kCameraName));
    const auto mesh_schema = registry.find_type(aeg::reflection::TypeId(aeg::engine::components::kMeshRendererName));
    const auto translation_field = registry.find_field(aeg::reflection::TypeId(aeg::engine::components::kTransformName),
                                                       aeg::reflection::FieldId("translation"));
    const auto rotation_field = registry.find_field(aeg::reflection::TypeId(aeg::engine::components::kTransformName),
                                                    aeg::reflection::FieldId("rotation"));
    if (!require(transform_schema && camera_schema && mesh_schema && translation_field && rotation_field,
                 "Runtime component schemas are incomplete") ||
        !require(transform_schema.value->kind == aeg::reflection::TypeKind::kComponent &&
                     transform_schema.value->version == aeg::engine::components::kRenderComponentSchemaVersion &&
                     transform_schema.value->size == sizeof(Transform) && transform_schema.value->fields.size() == 3 &&
                     translation_field.value.value_kind == aeg::reflection::FieldValueKind::kVec3 &&
                     translation_field.value.accessor.value_size == sizeof(aeg::math::Vec3) &&
                     translation_field.value.accessor.value_alignment == alignof(aeg::math::Vec3) &&
                     rotation_field.value.value_kind == aeg::reflection::FieldValueKind::kQuat &&
                     rotation_field.value.accessor.value_size == sizeof(aeg::math::Quat),
                 "Runtime component Reflection metadata violates the Math layout contract")) {
        return 2;
    }

    aeg::ecs::WorldManager raw_manager;
    const auto raw_handle = raw_manager.create_world({"RuntimeRawComponents", true});
    if (!require(raw_handle, "failed to create raw component World")) {
        return 3;
    }
    const auto raw_world = raw_manager.find(raw_handle.value);
    if (!require(raw_world, "failed to resolve raw component World") ||
        !require(aeg::engine::components::register_render_components(*raw_world.value),
                 "raw Runtime component registration failed") ||
        !require(aeg::engine::components::register_render_components(*raw_world.value),
                 "raw Runtime component registration is not idempotent")) {
        return 4;
    }
    const auto raw_transform =
        raw_world.value->find_component_descriptor(aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName));
    if (!require(
            raw_transform && !raw_transform.value.schema_type.is_valid() && raw_transform.value.schema_version == 0,
            "raw Runtime component unexpectedly has a Reflection schema")) {
        return 5;
    }

    Transform camera_transform{};
    camera_transform.translation = {0.0f, 2.0f, 6.0f};
    camera_transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    camera_transform.scale = {1.0f, 1.0f, 1.0f};
    std::array<float, 4> source_translation_storage{};
    std::memcpy(source_translation_storage.data(), &camera_transform.translation, sizeof(source_translation_storage));
    source_translation_storage[3] = 17.0f;
    std::memcpy(&camera_transform.translation, source_translation_storage.data(), sizeof(source_translation_storage));
    if (!verify_transform_encoding(registry, camera_transform)) {
        return 6;
    }

    Camera camera{};
    camera.vertical_fov_radians = 1.0f;
    camera.near_plane = 0.25f;
    camera.far_plane = 750.0f;

    Transform mesh_transform{};
    mesh_transform.translation = {3.0f, 0.0f, -5.0f};
    mesh_transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    mesh_transform.scale = {2.0f, 1.0f, 1.5f};

    MeshRenderer mesh_renderer{};
    mesh_renderer.mesh_asset_id = 0x1501ull;
    mesh_renderer.material_asset_id = 0x2602ull;
    mesh_renderer.bounds_center = {0.5f, 0.0f, -0.25f};
    mesh_renderer.bounds_radius = 1.5f;
    mesh_renderer.visibility_mask = 0x00000003u;
    mesh_renderer.visible = true;

    aeg::scene::SceneAsset asset(kAssetId, "RuntimeComponentScene", 1);
    if (!require(asset.add_entity({kRootId, {}, "Root", aeg::scene::SceneEntityFlags::kNone}),
                 "failed to add Runtime Scene root") ||
        !require(asset.add_entity({kCameraEntityId, kRootId, "MainCamera", aeg::scene::SceneEntityFlags::kNone}),
                 "failed to add Runtime Scene camera") ||
        !require(asset.add_entity({kMeshEntityId, kRootId, "Mesh", aeg::scene::SceneEntityFlags::kNone}),
                 "failed to add Runtime Scene mesh") ||
        !require(asset.add_reflected_component(
                     kCameraEntityId, aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName), registry,
                     aeg::reflection::TypeId(aeg::engine::components::kTransformName), &camera_transform),
                 "failed to add camera Transform to Runtime Scene") ||
        !require(asset.add_reflected_component(
                     kCameraEntityId, aeg::ecs::ComponentTypeId(aeg::engine::components::kCameraName), registry,
                     aeg::reflection::TypeId(aeg::engine::components::kCameraName), &camera),
                 "failed to add Camera to Runtime Scene") ||
        !require(asset.add_reflected_component(
                     kMeshEntityId, aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName), registry,
                     aeg::reflection::TypeId(aeg::engine::components::kTransformName), &mesh_transform),
                 "failed to add mesh Transform to Runtime Scene") ||
        !require(asset.add_reflected_component(
                     kMeshEntityId, aeg::ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName), registry,
                     aeg::reflection::TypeId(aeg::engine::components::kMeshRendererName), &mesh_renderer),
                 "failed to add MeshRenderer to Runtime Scene") ||
        !require(asset.validate(), "Runtime Scene validation failed")) {
        return 7;
    }

    aeg::serialization::BinaryWriter archive;
    if (!require(aeg::scene::SceneSerializer::serialize(asset, archive), "Runtime Scene serialization failed") ||
        !require(!archive.data().empty(), "Runtime Scene archive is empty")) {
        return 8;
    }
    aeg::serialization::BinaryReader archive_reader(archive.data());
    const auto decoded = aeg::scene::SceneSerializer::deserialize(archive_reader);
    if (!require(decoded, "Runtime Scene deserialization failed") ||
        !require(archive_reader.remaining() == 0, "Runtime Scene archive left unread bytes") ||
        !require(decoded.value.entities().size() == 3 && decoded.value.components().size() == 4,
                 "Runtime Scene record counts are incorrect")) {
        return 9;
    }
    aeg::serialization::BinaryWriter reencoded_archive;
    if (!require(aeg::scene::SceneSerializer::serialize(decoded.value, reencoded_archive),
                 "re-encoded Runtime Scene serialization failed") ||
        !require(archive.size() == reencoded_archive.size() &&
                     std::equal(archive.data().begin(), archive.data().end(), reencoded_archive.data().begin()),
                 "Runtime Scene archive is not deterministic")) {
        return 10;
    }
    const std::span<const uint8_t> archive_bytes = archive.data();
    aeg::serialization::BinaryReader truncated_reader(archive_bytes.first(archive_bytes.size() - 1));
    if (!require(!aeg::scene::SceneSerializer::deserialize(truncated_reader),
                 "truncated Runtime Scene archive was accepted")) {
        return 11;
    }

    aeg::ecs::WorldManager target_manager;
    const auto target_handle = target_manager.create_world({"RuntimeSceneTarget", true});
    if (!require(target_handle, "failed to create Runtime Scene target World")) {
        return 12;
    }
    const auto target_world = target_manager.find(target_handle.value);
    if (!require(target_world, "failed to resolve Runtime Scene target World") ||
        !require(aeg::engine::components::register_render_component_bundle(*target_world.value, registry),
                 "Runtime component bundle registration failed") ||
        !require(aeg::engine::components::register_reflected_render_components(*target_world.value, registry),
                 "reflected Runtime component registration is not idempotent")) {
        return 13;
    }
    const auto placeholder = target_world.value->create_entity();
    if (!require(placeholder, "failed to create target World placeholder")) {
        return 14;
    }
    const auto instance = decoded.value.instantiate(*target_world.value, registry);
    if (!require(instance, "Runtime Scene instantiation failed") ||
        !require(instance.value.entity_count() == 3 && target_world.value->entity_count() == 3,
                 "Runtime Scene instantiation did not replace target World state")) {
        return 15;
    }

    const auto camera_entity = instance.value.runtime_entity(kCameraEntityId);
    const auto mesh_entity = instance.value.runtime_entity(kMeshEntityId);
    if (!require(camera_entity && mesh_entity, "Runtime Scene entity bindings are incomplete")) {
        return 16;
    }
    const Transform* restored_camera_transform = target_world.value->get<Transform>(camera_entity.value);
    const Camera* restored_camera = target_world.value->get<Camera>(camera_entity.value);
    const Transform* restored_mesh_transform = target_world.value->get<Transform>(mesh_entity.value);
    const MeshRenderer* restored_mesh = target_world.value->get<MeshRenderer>(mesh_entity.value);
    if (!require(restored_camera_transform != nullptr && restored_camera != nullptr &&
                     restored_mesh_transform != nullptr && restored_mesh != nullptr,
                 "Runtime Scene components were not instantiated") ||
        !require(aeg::math::nearly_equal(restored_camera_transform->translation, camera_transform.translation) &&
                     aeg::math::nearly_equal(restored_camera_transform->rotation, camera_transform.rotation) &&
                     aeg::math::nearly_equal(restored_mesh_transform->translation, mesh_transform.translation) &&
                     restored_camera->vertical_fov_radians == camera.vertical_fov_radians &&
                     restored_camera->near_plane == camera.near_plane &&
                     restored_camera->far_plane == camera.far_plane &&
                     restored_mesh->mesh_asset_id == mesh_renderer.mesh_asset_id &&
                     restored_mesh->material_asset_id == mesh_renderer.material_asset_id &&
                     aeg::math::nearly_equal(restored_mesh->bounds_center, mesh_renderer.bounds_center) &&
                     restored_mesh->bounds_radius == mesh_renderer.bounds_radius &&
                     restored_mesh->visibility_mask == mesh_renderer.visibility_mask &&
                     restored_mesh->visible == mesh_renderer.visible,
                 "Runtime Scene component values did not round-trip")) {
        return 17;
    }

    const auto render_camera =
        aeg::engine::components::make_render_camera(*restored_camera_transform, *restored_camera, 16.0f / 9.0f);
    if (!require(render_camera, "Runtime Camera could not create a RenderCamera")) {
        return 18;
    }
    const auto snapshot = target_world.value->commit(37, 9);
    if (!require(snapshot, "Runtime Scene target World commit failed")) {
        return 19;
    }

    aeg::engine::components::RuntimeRenderViewDesc view_desc{};
    view_desc.camera_entity = camera_entity.value;
    view_desc.view_id = 1;
    view_desc.output_extent = {1280, 720};
    view_desc.shadow_extent = {320, 180};
    view_desc.visibility_mask = 0x00000003u;
    const auto view = aeg::engine::components::make_render_view(snapshot.value, view_desc);
    if (!require(view, "Runtime camera entity could not create a RenderView") ||
        !require(view.value.id == aeg::renderer::make_render_view_id(target_handle.value, 1) &&
                     view.value.world == target_handle.value && view.value.view_id == 1 &&
                     view.value.output_extent == aeg::renderer::RenderExtent{1280, 720} &&
                     view.value.shadow_extent == aeg::renderer::RenderExtent{320, 180} &&
                     aeg::math::nearly_equal(view.value.camera.position, render_camera.value.position) &&
                     view.value.visibility_mask == view_desc.visibility_mask,
                 "Runtime component RenderView does not preserve its stable contract")) {
        return 20;
    }
    aeg::engine::components::RuntimeRenderViewDesc missing_camera_desc = view_desc;
    missing_camera_desc.camera_entity = mesh_entity.value;
    const auto missing_camera_view = aeg::engine::components::make_render_view(snapshot.value, missing_camera_desc);
    if (!require(!missing_camera_view && missing_camera_view.result.code == aeg::foundation::ResultCode::kNotFound,
                 "Runtime RenderView accepted an entity without Camera")) {
        return 21;
    }
    aeg::engine::components::RuntimeRenderViewDesc invalid_extent_desc = view_desc;
    invalid_extent_desc.output_extent = {0, 720};
    const auto invalid_extent_view = aeg::engine::components::make_render_view(snapshot.value, invalid_extent_desc);
    if (!require(
            !invalid_extent_view && invalid_extent_view.result.code == aeg::foundation::ResultCode::kInvalidArgument,
            "Runtime RenderView accepted an invalid output extent")) {
        return 22;
    }

    aeg::renderer::Renderer renderer;
    const auto extractor = aeg::engine::components::register_mesh_renderer_extractor(renderer);
    if (!require(extractor, "Runtime mesh extractor registration failed")) {
        return 23;
    }
    const auto extracted = renderer.extract(snapshot.value, view.value, aeg::renderer::RenderExtractionDesc{37});
    if (!require(extracted, "Runtime mesh extraction failed") ||
        !require(extracted.value->instances.size() == 1 && extracted.value->visibility.candidate_instance_count == 1 &&
                     extracted.value->visibility.visible_instance_count == 1,
                 "Runtime mesh extraction produced incorrect instance counts")) {
        return 24;
    }
    const aeg::renderer::RenderInstance& extracted_instance = extracted.value->instances.front();
    if (!require(
            extracted_instance.object == aeg::renderer::make_render_object_id(mesh_entity.value) &&
                extracted_instance.mesh == aeg::renderer::MeshAssetId(mesh_renderer.mesh_asset_id) &&
                extracted_instance.material == aeg::renderer::MaterialInstanceId(mesh_renderer.material_asset_id) &&
                aeg::math::nearly_equal(extracted_instance.world_transform, mesh_transform.to_matrix()) &&
                aeg::math::nearly_equal(extracted_instance.previous_world_transform, mesh_transform.to_matrix()) &&
                aeg::math::nearly_equal(extracted_instance.bounds_center, mesh_renderer.bounds_center) &&
                extracted_instance.bounds_radius == mesh_renderer.bounds_radius &&
                extracted_instance.visibility_mask == mesh_renderer.visibility_mask,
            "Runtime mesh extraction produced an incorrect RenderInstance") ||
        !require(renderer.unregister_extractor(extractor.value), "Runtime mesh extractor unregistration failed")) {
        return 25;
    }

    return 0;
}
