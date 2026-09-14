/**
 * @file editor_history_smoke.cpp
 * @brief Verifies Editor-owned Undo/Redo history, multi-selection edits, and Edit/Play isolation.
 */
#include <array>
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

bool same_transform(const aeg::engine::components::Transform& left, const aeg::engine::components::Transform& right) {
    return aeg::math::nearly_equal(left.translation, right.translation, 1.0e-4f) &&
           aeg::math::nearly_equal(left.rotation, right.rotation, 1.0e-4f) &&
           aeg::math::nearly_equal(left.scale, right.scale, 1.0e-4f);
}
}  // namespace

int main() {
    const aeg::rhi::DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan, "AEG Editor History Smoke", std::numeric_limits<uint32_t>::max(), true, true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "editor history smoke device creation failed")) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);

    aeg::application::EditorSceneRuntime runtime;
    if (!require(runtime.initialize(*device), "editor history smoke initialization failed") ||
        !require(runtime.request_viewport_extent(96, 64), "editor history smoke extent request failed") ||
        !require(runtime.render_frame(0), "editor history smoke initial frame failed")) {
        return 2;
    }

    const auto* left = find_entity(runtime, "Left Mesh");
    const auto* right = find_entity(runtime, "Right Mesh");
    if (!require(left != nullptr && right != nullptr, "editor history smoke mesh entities are missing")) {
        return 3;
    }
    const aeg::scene::SceneEntityId left_id = left->id;
    const aeg::scene::SceneEntityId right_id = right->id;
    const auto transform_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName);
    const auto scene_path = std::filesystem::temp_directory_path() / "aeg_editor_history_smoke.aegscene";
    std::error_code remove_error;
    std::filesystem::remove(scene_path, remove_error);

    if (!require(runtime.save_scene_as(scene_path), "editor history smoke initial checkpoint failed") ||
        !require(!runtime.dirty(), "editor history smoke initial checkpoint remained dirty") ||
        !require(!runtime.can_undo(), "editor history smoke initial checkpoint has unexpected history")) {
        std::filesystem::remove(scene_path, remove_error);
        return 4;
    }

    const auto* initial_value =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(initial_value != nullptr, "editor history smoke initial Transform is missing")) {
        std::filesystem::remove(scene_path, remove_error);
        return 5;
    }
    const auto initial = *initial_value;
    auto first_edit = initial;
    first_edit.translation.x += 0.25f;
    if (!require(runtime.queue_component_update(left_id, transform_type, bytes_of(first_edit)),
                 "editor history smoke first edit queue failed") ||
        !require(runtime.dirty() && runtime.can_undo() && !runtime.can_redo(),
                 "editor history smoke first edit history state is invalid") ||
        !require(runtime.render_frame(1), "editor history smoke first edit commit failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 6;
    }
    const auto* first_committed =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(first_committed != nullptr && same_transform(*first_committed, first_edit),
                 "editor history smoke first edit did not commit")) {
        std::filesystem::remove(scene_path, remove_error);
        return 7;
    }
    if (!require(runtime.undo(), "editor history smoke first undo failed") ||
        !require(runtime.can_redo() && !runtime.dirty(), "editor history smoke undo checkpoint state is invalid") ||
        !require(runtime.render_frame(2), "editor history smoke undo commit failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 8;
    }
    const auto* after_undo =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(after_undo != nullptr && same_transform(*after_undo, initial),
                 "editor history smoke undo did not restore the initial Transform") ||
        !require(runtime.redo(), "editor history smoke first redo failed") ||
        !require(runtime.dirty(), "editor history smoke redo did not restore dirty state") ||
        !require(runtime.render_frame(3), "editor history smoke redo commit failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 9;
    }

    auto second_edit = first_edit;
    second_edit.translation.x += 0.20f;
    auto third_edit = second_edit;
    third_edit.translation.y += 0.15f;
    if (!require(runtime.queue_component_update(left_id, transform_type, bytes_of(second_edit)),
                 "editor history smoke second edit queue failed") ||
        !require(runtime.render_frame(4), "editor history smoke second edit commit failed") ||
        !require(runtime.queue_component_update(left_id, transform_type, bytes_of(third_edit)),
                 "editor history smoke third edit queue failed") ||
        !require(runtime.render_frame(5), "editor history smoke third edit commit failed") ||
        !require(runtime.undo(), "editor history smoke consecutive undo failed") ||
        !require(runtime.render_frame(6), "editor history smoke consecutive undo commit failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 10;
    }
    const auto* after_consecutive_undo =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(after_consecutive_undo != nullptr && same_transform(*after_consecutive_undo, second_edit),
                 "editor history smoke consecutive undo restored the wrong value")) {
        std::filesystem::remove(scene_path, remove_error);
        return 11;
    }
    auto branched_edit = second_edit;
    branched_edit.translation.z += 0.10f;
    if (!require(runtime.queue_component_update(left_id, transform_type, bytes_of(branched_edit)),
                 "editor history smoke branch edit queue failed") ||
        !require(!runtime.can_redo(), "editor history smoke redo branch was not truncated") ||
        !require(runtime.render_frame(7), "editor history smoke branch edit commit failed") ||
        !require(runtime.save_scene(), "editor history smoke branch checkpoint failed") ||
        !require(!runtime.dirty(), "editor history smoke branch checkpoint remained dirty")) {
        std::filesystem::remove(scene_path, remove_error);
        return 12;
    }

    if (!require(runtime.select(left_id), "editor history smoke Gizmo selection failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 13;
    }
    const auto gizmo_projection = runtime.selected_gizmo_projection(96, 64);
    if (!require(gizmo_projection && gizmo_projection.value.valid, "editor history smoke Gizmo projection failed") ||
        !require(runtime.begin_transform_edit(aeg::application::ViewportGizmoMode::kTranslate,
                                              aeg::application::ViewportGizmoAxis::kX, gizmo_projection.value.x_axis,
                                              96, 64),
                 "editor history smoke Gizmo transaction begin failed") ||
        !require(runtime.update_transform_edit(
                     {gizmo_projection.value.x_axis.x + 8.0f, gizmo_projection.value.x_axis.y}, 96, 64),
                 "editor history smoke first Gizmo update failed") ||
        !require(runtime.update_transform_edit(
                     {gizmo_projection.value.x_axis.x + 16.0f, gizmo_projection.value.x_axis.y}, 96, 64),
                 "editor history smoke second Gizmo update failed") ||
        !require(runtime.commit_transform_edit(), "editor history smoke Gizmo transaction commit failed") ||
        !require(runtime.render_frame(8), "editor history smoke Gizmo commit frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 14;
    }
    const auto* after_gizmo =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(after_gizmo != nullptr && !same_transform(*after_gizmo, branched_edit),
                 "editor history smoke Gizmo did not change the Transform") ||
        !require(runtime.undo(), "editor history smoke Gizmo undo failed") ||
        !require(runtime.render_frame(9), "editor history smoke Gizmo undo frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 15;
    }
    const auto* after_gizmo_undo =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(after_gizmo_undo != nullptr && same_transform(*after_gizmo_undo, branched_edit),
                 "editor history smoke Gizmo undo exposed an intermediate frame") ||
        !require(!runtime.dirty(), "editor history smoke Gizmo undo did not return to the clean checkpoint")) {
        std::filesystem::remove(scene_path, remove_error);
        return 16;
    }

    if (!require(runtime.select(left_id, aeg::application::SelectionOperation::kReplace),
                 "editor history smoke multi-selection replace failed") ||
        !require(runtime.select(right_id, aeg::application::SelectionOperation::kAdd),
                 "editor history smoke multi-selection add failed") ||
        !require(
            runtime.selected_entities().size() == 2 && runtime.is_selected(left_id) && runtime.is_selected(right_id),
            "editor history smoke multi-selection set is invalid")) {
        std::filesystem::remove(scene_path, remove_error);
        return 17;
    }
    const auto* left_before_batch_value =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    const auto* right_before_batch_value =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(right_id, transform_type));
    if (!require(left_before_batch_value != nullptr && right_before_batch_value != nullptr,
                 "editor history smoke batch Transform values are missing")) {
        std::filesystem::remove(scene_path, remove_error);
        return 18;
    }
    const auto left_before_batch = *left_before_batch_value;
    const auto right_before_batch = *right_before_batch_value;
    auto left_batch = left_before_batch;
    auto right_batch = right_before_batch;
    left_batch.translation.y += 0.12f;
    right_batch.translation.y -= 0.12f;
    const std::array<aeg::application::ComponentUpdateView, 2> batch_updates{{
        {left_id, transform_type, bytes_of(left_batch)},
        {right_id, transform_type, bytes_of(right_batch)},
    }};
    if (!require(runtime.queue_component_updates(batch_updates), "editor history smoke batch edit queue failed") ||
        !require(runtime.render_frame(10), "editor history smoke batch edit commit failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 19;
    }
    const auto* left_after_batch =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    const auto* right_after_batch =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(right_id, transform_type));
    if (!require(left_after_batch != nullptr && right_after_batch != nullptr &&
                     same_transform(*left_after_batch, left_batch) && same_transform(*right_after_batch, right_batch),
                 "editor history smoke batch edit did not update both entities") ||
        !require(runtime.undo(), "editor history smoke batch undo failed") ||
        !require(runtime.render_frame(11), "editor history smoke batch undo commit failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 20;
    }
    const auto* left_after_batch_undo =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    const auto* right_after_batch_undo =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(right_id, transform_type));
    if (!require(left_after_batch_undo != nullptr && right_after_batch_undo != nullptr &&
                     same_transform(*left_after_batch_undo, left_before_batch) &&
                     same_transform(*right_after_batch_undo, right_before_batch),
                 "editor history smoke batch undo was not atomic") ||
        !require(runtime.redo(), "editor history smoke batch redo failed") ||
        !require(runtime.render_frame(12), "editor history smoke batch redo commit failed") ||
        !require(runtime.save_scene(), "editor history smoke batch checkpoint failed") ||
        !require(!runtime.dirty(), "editor history smoke saved batch remained dirty")) {
        std::filesystem::remove(scene_path, remove_error);
        return 21;
    }

    const bool can_undo_before_play = runtime.can_undo();
    const auto* before_play_value =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(before_play_value != nullptr, "editor history smoke pre-Play Transform is missing") ||
        !require(runtime.play(), "editor history smoke Play failed") ||
        !require(!runtime.can_undo() && !runtime.can_redo(), "editor history smoke Play exposed Edit history") ||
        !require(!runtime.undo(), "editor history smoke Undo unexpectedly succeeded in Play mode") ||
        !require(runtime.render_frame(13), "editor history smoke Play frame failed") ||
        !require(runtime.stop(), "editor history smoke Stop failed") ||
        !require(runtime.can_undo() == can_undo_before_play,
                 "editor history smoke Stop changed the Edit history availability")) {
        std::filesystem::remove(scene_path, remove_error);
        return 22;
    }
    const auto* after_stop_value =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    const bool edit_world_preserved =
        after_stop_value != nullptr && same_transform(*after_stop_value, *before_play_value);
    std::filesystem::remove(scene_path, remove_error);
    if (!require(edit_world_preserved, "editor history smoke Play/Stop polluted the Edit Transform") ||
        !require(runtime.shutdown(), "editor history smoke shutdown failed")) {
        return 23;
    }
    std::cout << "Editor history smoke passed\n";
    return 0;
}
