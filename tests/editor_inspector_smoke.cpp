/**
 * @file editor_inspector_smoke.cpp
 * @brief Verifies Inspector component transactions, metadata, resources, persistence, and isolation.
 */
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>

#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "editor_scene_runtime.hpp"
#include "rhi.hpp"

namespace {
template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

template <typename Value>
std::span<const std::byte> bytes_of(const Value& value) {
    return std::as_bytes(std::span{&value, 1});
}

const aeg::scene::SceneEntityRecord* find_entity(const aeg::application::EditorSceneRuntime& runtime,
                                                 const char* name) {
    for (const auto& entity : runtime.entities()) {
        if (entity.name == name) {
            return &entity;
        }
    }
    return nullptr;
}

bool same_vec3(const aeg::math::Vec3& left, const aeg::math::Vec3& right) {
    return aeg::math::nearly_equal(left, right, 1.0e-4f);
}
}  // namespace

int main() {
    const aeg::rhi::DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan, "AEG Editor Inspector Smoke", std::numeric_limits<uint32_t>::max(), true, true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "editor Inspector smoke device creation failed")) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);
    aeg::application::EditorSceneRuntime runtime;
    if (!require(runtime.initialize(*device), "editor Inspector smoke initialization failed") ||
        !require(runtime.request_viewport_extent(96, 64), "editor Inspector smoke extent request failed") ||
        !require(runtime.render_frame(0, 1.0 / 60.0), "editor Inspector smoke initial frame failed")) {
        return 2;
    }

    const auto* left = find_entity(runtime, "Left Mesh");
    const auto* right = find_entity(runtime, "Right Mesh");
    const auto* camera = find_entity(runtime, "Camera");
    const auto* light = find_entity(runtime, "Directional Light");
    if (!require(left != nullptr && right != nullptr && camera != nullptr && light != nullptr,
                 "editor Inspector smoke default entities are missing")) {
        return 3;
    }
    const auto left_id = left->id;
    const auto right_id = right->id;
    const auto camera_id = camera->id;
    const auto light_id = light->id;
    const auto mesh_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName);
    const auto camera_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kCameraName);
    const auto light_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kDirectionalLightName);
    const auto scene_path = std::filesystem::temp_directory_path() / "aeg_editor_inspector_smoke.aegscene";
    std::error_code remove_error;
    std::filesystem::remove(scene_path, remove_error);

    if (!require(runtime.save_scene_as(scene_path), "editor Inspector smoke clean checkpoint failed") ||
        !require(!runtime.dirty(), "editor Inspector smoke clean checkpoint remained dirty")) {
        std::filesystem::remove(scene_path, remove_error);
        return 4;
    }

    const auto* left_mesh =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_id, mesh_type));
    const auto* right_mesh =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(right_id, mesh_type));
    if (!require(left_mesh != nullptr && right_mesh != nullptr, "editor Inspector smoke MeshRenderer is missing")) {
        std::filesystem::remove(scene_path, remove_error);
        return 5;
    }
    auto edited_left = *left_mesh;
    auto edited_right = *right_mesh;
    edited_left.tint = {0.2f, 0.4f, 0.8f};
    edited_right.tint = edited_left.tint;
    const std::array<aeg::application::ComponentUpdateView, 2> first_updates{{
        {left_id, mesh_type, bytes_of(edited_left)},
        {right_id, mesh_type, bytes_of(edited_right)},
    }};
    if (!require(runtime.begin_component_edit(first_updates), "editor Inspector smoke transaction begin failed") ||
        !require(runtime.component_edit_active(), "editor Inspector smoke transaction did not become active")) {
        std::filesystem::remove(scene_path, remove_error);
        return 6;
    }
    auto updated_left = edited_left;
    auto updated_right = edited_right;
    updated_left.tint.x = 0.3f;
    updated_right.tint.x = 0.3f;
    const std::array<aeg::application::ComponentUpdateView, 2> second_updates{{
        {left_id, mesh_type, bytes_of(updated_left)},
        {right_id, mesh_type, bytes_of(updated_right)},
    }};
    if (!require(runtime.update_component_edit(second_updates), "editor Inspector smoke transaction update failed") ||
        !require(runtime.commit_component_edit(), "editor Inspector smoke transaction commit failed") ||
        !require(runtime.render_frame(1, 1.0 / 60.0), "editor Inspector smoke edited frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 7;
    }
    const auto* committed_left =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_id, mesh_type));
    if (!require(committed_left != nullptr && same_vec3(committed_left->tint, updated_left.tint),
                 "editor Inspector smoke committed MeshRenderer value is wrong") ||
        !require(runtime.undo(), "editor Inspector smoke transaction undo failed") ||
        !require(runtime.render_frame(2, 1.0 / 60.0), "editor Inspector smoke undo frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 8;
    }
    const auto* undone_left =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_id, mesh_type));
    if (!require(undone_left != nullptr && same_vec3(undone_left->tint, left_mesh->tint),
                 "editor Inspector smoke undo did not restore the original tint") ||
        !require(runtime.redo(), "editor Inspector smoke transaction redo failed") ||
        !require(runtime.render_frame(3, 1.0 / 60.0), "editor Inspector smoke redo frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 9;
    }

    auto invalid_camera =
        *static_cast<const aeg::engine::components::Camera*>(runtime.component_value(camera_id, camera_type));
    invalid_camera.vertical_fov_radians = std::numeric_limits<float>::quiet_NaN();
    if (!require(!runtime.queue_component_update(camera_id, camera_type, bytes_of(invalid_camera)),
                 "editor Inspector smoke accepted NaN camera data")) {
        std::filesystem::remove(scene_path, remove_error);
        return 10;
    }
    auto invalid_light =
        *static_cast<const aeg::engine::components::DirectionalLight*>(runtime.component_value(light_id, light_type));
    invalid_light.intensity = std::numeric_limits<float>::infinity();
    if (!require(!runtime.queue_component_update(light_id, light_type, bytes_of(invalid_light)),
                 "editor Inspector smoke accepted infinite light data")) {
        std::filesystem::remove(scene_path, remove_error);
        return 11;
    }

    if (!require(runtime.rename_entity(left_id, "Renamed Mesh"), "editor Inspector smoke rename failed") ||
        !require(runtime.set_entity_tag(left_id, "Gameplay"), "editor Inspector smoke tag edit failed") ||
        !require(runtime.save_scene(), "editor Inspector smoke save failed") ||
        !require(runtime.open_scene(scene_path), "editor Inspector smoke reload failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 12;
    }
    const auto* renamed = find_entity(runtime, "Renamed Mesh");
    if (!require(renamed != nullptr && renamed->tag == "Gameplay", "editor Inspector smoke metadata did not persist")) {
        std::filesystem::remove(scene_path, remove_error);
        return 13;
    }

    const auto resources = runtime.resource_descriptors();
    if (!require(resources.size() == 4 && runtime.is_resource_valid(aeg::application::EditorResourceKind::kMesh, 1) &&
                     runtime.is_resource_valid(aeg::application::EditorResourceKind::kMaterial, 2) &&
                     !runtime.is_resource_valid(aeg::application::EditorResourceKind::kMesh, 999),
                 "editor Inspector smoke resource identity contract is invalid")) {
        std::filesystem::remove(scene_path, remove_error);
        return 14;
    }

    const std::array<aeg::scene::SceneEntityId, 1> one_entity{{left_id}};
    if (!require(runtime.add_component(one_entity, camera_type), "editor Inspector smoke component add failed") ||
        !require(runtime.render_frame(4, 1.0 / 60.0), "editor Inspector smoke component add frame failed") ||
        !require(runtime.component_value(left_id, camera_type) != nullptr,
                 "editor Inspector smoke added component is missing") ||
        !require(runtime.undo(), "editor Inspector smoke component add undo failed") ||
        !require(runtime.render_frame(5, 1.0 / 60.0), "editor Inspector smoke component add undo frame failed") ||
        !require(runtime.component_value(left_id, camera_type) == nullptr,
                 "editor Inspector smoke component add undo did not remove the component") ||
        !require(runtime.redo(), "editor Inspector smoke component add redo failed") ||
        !require(runtime.render_frame(6, 1.0 / 60.0), "editor Inspector smoke component add redo frame failed") ||
        !require(runtime.remove_component(one_entity, camera_type), "editor Inspector smoke component remove failed") ||
        !require(runtime.render_frame(7, 1.0 / 60.0), "editor Inspector smoke component remove frame failed") ||
        !require(runtime.component_value(left_id, camera_type) == nullptr,
                 "editor Inspector smoke removed component is still present")) {
        std::filesystem::remove(scene_path, remove_error);
        return 15;
    }

    auto cancel_value = updated_left;
    cancel_value.tint.y = 0.1f;
    const std::array<aeg::application::ComponentUpdateView, 1> cancel_update{{
        {left_id, mesh_type, bytes_of(cancel_value)},
    }};
    if (!require(runtime.begin_component_edit(cancel_update), "editor Inspector smoke cancel begin failed") ||
        !require(runtime.cancel_component_edit(), "editor Inspector smoke cancel failed") ||
        !require(runtime.render_frame(8, 1.0 / 60.0), "editor Inspector smoke cancel frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 16;
    }
    const auto* after_cancel =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_id, mesh_type));
    if (!require(after_cancel != nullptr && same_vec3(after_cancel->tint, updated_left.tint),
                 "editor Inspector smoke cancel did not restore the transaction start")) {
        std::filesystem::remove(scene_path, remove_error);
        return 17;
    }

    const auto before_play = *after_cancel;
    if (!require(runtime.play(), "editor Inspector smoke Play failed") ||
        !require(!runtime.queue_component_update(left_id, mesh_type, bytes_of(cancel_value)),
                 "editor Inspector smoke allowed edit-world mutation in Play mode") ||
        !require(runtime.stop(), "editor Inspector smoke Stop failed") ||
        !require(runtime.render_frame(5, 1.0 / 60.0), "editor Inspector smoke Stop frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 18;
    }
    const auto* after_stop =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_id, mesh_type));
    if (!require(after_stop != nullptr && same_vec3(after_stop->tint, before_play.tint),
                 "editor Inspector smoke Play/Stop polluted the edit scene")) {
        std::filesystem::remove(scene_path, remove_error);
        return 19;
    }

    std::filesystem::remove(scene_path, remove_error);
    if (!require(runtime.shutdown(), "editor Inspector smoke shutdown failed")) {
        return 20;
    }
    std::cout << "Editor Inspector smoke passed\n";
    return 0;
}
