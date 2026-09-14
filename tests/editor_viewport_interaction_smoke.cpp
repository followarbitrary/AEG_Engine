/**
 * @file editor_viewport_interaction_smoke.cpp
 * @brief Verifies CPU viewport picking and editor-owned Transform transactions.
 */
#include <cmath>
#include <cstddef>
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

const aeg::scene::SceneEntityRecord* find_entity(const aeg::application::EditorSceneRuntime& runtime,
                                                 const char* name) {
    for (const auto& entity : runtime.entities()) {
        if (entity.name == name) {
            return &entity;
        }
    }
    return nullptr;
}

bool same_transform(const aeg::engine::components::Transform& left, const aeg::engine::components::Transform& right) {
    return aeg::math::nearly_equal(left.translation, right.translation, 1.0e-4f) &&
           aeg::math::nearly_equal(left.rotation, right.rotation, 1.0e-4f) &&
           aeg::math::nearly_equal(left.scale, right.scale, 1.0e-4f);
}
}  // namespace

int main() {
    const aeg::rhi::DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan,
        "AEG Editor Viewport Interaction Smoke",
        std::numeric_limits<uint32_t>::max(),
        true,
        true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "viewport interaction smoke device creation failed")) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);

    aeg::application::EditorSceneRuntime runtime;
    if (!require(runtime.initialize(*device), "viewport interaction smoke initialization failed") ||
        !require(runtime.request_viewport_extent(96, 64), "viewport interaction smoke extent request failed") ||
        !require(runtime.render_frame(0), "viewport interaction smoke initial frame failed")) {
        return 2;
    }

    const auto* left = find_entity(runtime, "Left Mesh");
    const auto* right = find_entity(runtime, "Right Mesh");
    if (!require(left != nullptr && right != nullptr, "viewport interaction smoke mesh entities are missing") ||
        !require(runtime.select(left->id), "viewport interaction smoke left selection failed")) {
        return 3;
    }

    const auto projection = runtime.selected_gizmo_projection(96, 64);
    if (!require(projection && projection.value.valid, "viewport interaction smoke gizmo projection failed")) {
        return 4;
    }
    const auto picked_left = runtime.pick_viewport(projection.value.center, 96, 64);
    if (!require(picked_left && picked_left.value.hit && picked_left.value.entity == left->id,
                 "viewport interaction smoke left pick failed") ||
        !require(runtime.select(right->id), "viewport interaction smoke right selection failed")) {
        return 5;
    }
    const auto right_projection = runtime.selected_gizmo_projection(96, 64);
    const auto picked_right = right_projection ? runtime.pick_viewport(right_projection.value.center, 96, 64)
                                               : aeg::foundation::ResultValue<aeg::application::ViewportPickResult>{};
    const auto empty_pick = runtime.pick_viewport({0.0f, 0.0f}, 96, 64);
    if (!require(right_projection && right_projection.value.valid,
                 "viewport interaction smoke right projection failed") ||
        !require(picked_right && picked_right.value.hit && picked_right.value.entity == right->id,
                 "viewport interaction smoke right pick failed") ||
        !require(empty_pick && !empty_pick.value.hit,
                 "viewport interaction smoke reported a hit in an empty viewport region") ||
        !require(!runtime.pick_viewport({0.0f, 0.0f}, 0, 64),
                 "viewport interaction smoke accepted a zero-width viewport")) {
        return 6;
    }

    const auto* camera = find_entity(runtime, "Camera");
    const auto camera_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kCameraName);
    const auto* source_camera =
        camera == nullptr
            ? nullptr
            : static_cast<const aeg::engine::components::Camera*>(runtime.component_value(camera->id, camera_type));
    if (!require(source_camera != nullptr, "viewport interaction smoke camera component is missing")) {
        return 7;
    }
    aeg::engine::components::Camera invalid_camera = *source_camera;
    invalid_camera.vertical_fov_radians = 444.0f;
    if (!require(!runtime.queue_component_update(camera->id, camera_type, std::as_bytes(std::span{&invalid_camera, 1})),
                 "viewport interaction smoke accepted an invalid camera projection") ||
        !require(runtime.pick_viewport({48.0f, 32.0f}, 96, 64),
                 "viewport interaction smoke rejected a valid camera after invalid edit")) {
        return 8;
    }

    const auto transform_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName);
    const auto* source_right_transform =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(right->id, transform_type));
    if (!require(source_right_transform != nullptr, "viewport interaction smoke right Transform is missing")) {
        return 9;
    }
    aeg::engine::components::Transform zoomed_right = *source_right_transform;
    zoomed_right.translation.z = 1.9f;
    if (!require(runtime.queue_component_update(right->id, transform_type, std::as_bytes(std::span{&zoomed_right, 1})),
                 "viewport interaction smoke could not move right mesh toward camera") ||
        !require(runtime.render_frame(1), "viewport interaction smoke zoomed frame failed") ||
        !require(runtime.select(left->id), "viewport interaction smoke could not select left mesh after zoom")) {
        return 10;
    }
    const auto zoomed_left_projection = runtime.selected_gizmo_projection(96, 64);
    const auto zoomed_left_pick = zoomed_left_projection
                                      ? runtime.pick_viewport(zoomed_left_projection.value.center, 96, 64)
                                      : aeg::foundation::ResultValue<aeg::application::ViewportPickResult>{};
    if (!require(zoomed_left_projection && zoomed_left_projection.value.valid,
                 "viewport interaction smoke zoomed left projection failed") ||
        !require(zoomed_left_pick && zoomed_left_pick.value.hit && zoomed_left_pick.value.entity == left->id,
                 "viewport interaction smoke picked the enlarged right fallback bounds over the left Quad")) {
        return 11;
    }

    if (!require(runtime.select(left->id), "viewport interaction smoke reselect left failed")) {
        return 12;
    }
    const auto* initial =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left->id, transform_type));
    if (!require(initial != nullptr, "viewport interaction smoke initial Transform is missing")) {
        return 13;
    }
    const aeg::engine::components::Transform initial_copy = *initial;
    const auto left_projection = runtime.selected_gizmo_projection(96, 64);
    if (!require(left_projection && left_projection.value.valid,
                 "viewport interaction smoke left reprojection failed") ||
        !require(
            runtime.begin_transform_edit(aeg::application::ViewportGizmoMode::kTranslate,
                                         aeg::application::ViewportGizmoAxis::kX, left_projection.value.x_axis, 96, 64),
            "viewport interaction smoke begin translate failed") ||
        !require(runtime.update_transform_edit({left_projection.value.x_axis.x + 8.0f, left_projection.value.x_axis.y},
                                               96, 64),
                 "viewport interaction smoke update translate failed") ||
        !require(runtime.commit_transform_edit(), "viewport interaction smoke commit translate failed") ||
        !require(runtime.render_frame(2), "viewport interaction smoke committed frame failed")) {
        return 14;
    }
    const auto* translated =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left->id, transform_type));
    if (!require(translated != nullptr && !aeg::math::nearly_equal(translated->translation, initial_copy.translation),
                 "viewport interaction smoke translate did not change the edit World")) {
        return 15;
    }
    const aeg::engine::components::Transform committed_copy = *translated;

    const auto after_translate_projection = runtime.selected_gizmo_projection(96, 64);
    if (!require(after_translate_projection && after_translate_projection.value.valid,
                 "viewport interaction smoke post-translate projection failed") ||
        !require(runtime.begin_transform_edit(aeg::application::ViewportGizmoMode::kRotate,
                                              aeg::application::ViewportGizmoAxis::kZ,
                                              after_translate_projection.value.z_axis, 96, 64),
                 "viewport interaction smoke begin rotate failed") ||
        !require(
            runtime.update_transform_edit(
                {after_translate_projection.value.z_axis.x + 10.0f, after_translate_projection.value.z_axis.y}, 96, 64),
            "viewport interaction smoke update rotate failed") ||
        !require(runtime.cancel_transform_edit(), "viewport interaction smoke cancel rotate failed") ||
        !require(runtime.render_frame(3), "viewport interaction smoke cancelled frame failed")) {
        return 16;
    }
    const auto* cancelled =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left->id, transform_type));
    if (!require(cancelled != nullptr && same_transform(*cancelled, committed_copy),
                 "viewport interaction smoke cancel did not restore the initial Transform")) {
        return 17;
    }

    const std::filesystem::path scene_path =
        std::filesystem::temp_directory_path() / "aeg_editor_viewport_interaction_smoke.aegscene";
    std::error_code remove_error;
    std::filesystem::remove(scene_path, remove_error);
    if (!require(runtime.save_scene_as(scene_path), "viewport interaction smoke Save As failed") ||
        !require(runtime.open_scene(scene_path), "viewport interaction smoke reload failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 13;
    }
    const auto* reopened_left = find_entity(runtime, "Left Mesh");
    const aeg::scene::SceneEntityId reopened_left_id =
        reopened_left == nullptr ? aeg::scene::SceneEntityId{} : reopened_left->id;
    const auto* reopened_transform = reopened_left == nullptr
                                         ? nullptr
                                         : static_cast<const aeg::engine::components::Transform*>(
                                               runtime.component_value(reopened_left_id, transform_type));
    if (!require(reopened_transform != nullptr && same_transform(*reopened_transform, committed_copy),
                 "viewport interaction smoke Transform did not round-trip through Save/Load") ||
        !require(runtime.play(), "viewport interaction smoke Play failed") ||
        !require(runtime.render_frame(3), "viewport interaction smoke Play frame failed") ||
        !require(runtime.stop(), "viewport interaction smoke Stop failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 14;
    }
    const auto* after_stop = static_cast<const aeg::engine::components::Transform*>(
        runtime.component_value(reopened_left_id, transform_type));
    const bool preserved = after_stop != nullptr && same_transform(*after_stop, committed_copy);
    std::filesystem::remove(scene_path, remove_error);
    if (!require(preserved, "viewport interaction smoke Play/Stop polluted the edit Transform") ||
        !require(runtime.shutdown(), "viewport interaction smoke shutdown failed")) {
        return 15;
    }
    std::cout << "Editor viewport interaction smoke passed\n";
    return 0;
}
