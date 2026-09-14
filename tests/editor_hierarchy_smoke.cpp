/**
 * @file editor_hierarchy_smoke.cpp
 * @brief Verifies hierarchy editing, transform preservation, ordering, persistence, and isolation.
 */
#include <array>
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
    return aeg::math::nearly_equal(left.translation, right.translation, 2.0e-4f) &&
           aeg::math::nearly_equal(left.rotation, right.rotation, 2.0e-4f) &&
           aeg::math::nearly_equal(left.scale, right.scale, 2.0e-4f);
}

bool has_parent(const aeg::application::EditorSceneRuntime& runtime, aeg::scene::SceneEntityId entity,
                aeg::scene::SceneEntityId parent) {
    for (const auto& record : runtime.entities()) {
        if (record.id == entity) {
            return record.parent == parent;
        }
    }
    return false;
}

bool has_entity(const aeg::application::EditorSceneRuntime& runtime, aeg::scene::SceneEntityId entity) {
    for (const auto& record : runtime.entities()) {
        if (record.id == entity) {
            return true;
        }
    }
    return false;
}
}  // namespace

int main() {
    const aeg::rhi::DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan, "AEG Editor Hierarchy Smoke", std::numeric_limits<uint32_t>::max(), true, true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "editor hierarchy smoke device creation failed")) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);
    aeg::application::EditorSceneRuntime runtime;
    if (!require(runtime.initialize(*device), "editor hierarchy smoke initialization failed") ||
        !require(runtime.request_viewport_extent(96, 64), "editor hierarchy smoke extent request failed") ||
        !require(runtime.render_frame(0), "editor hierarchy smoke initial frame failed")) {
        return 2;
    }

    const auto* left = find_entity(runtime, "Left Mesh");
    const auto* right = find_entity(runtime, "Right Mesh");
    const auto* camera = find_entity(runtime, "Camera");
    if (!require(left != nullptr && right != nullptr && camera != nullptr,
                 "editor hierarchy smoke default entities are missing")) {
        return 3;
    }
    const aeg::scene::SceneEntityId left_id = left->id;
    const aeg::scene::SceneEntityId right_id = right->id;
    const aeg::scene::SceneEntityId camera_id = camera->id;
    const auto transform_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName);
    const auto scene_path = std::filesystem::temp_directory_path() / "aeg_editor_hierarchy_smoke.aegscene";
    std::error_code remove_error;
    std::filesystem::remove(scene_path, remove_error);
    if (!require(runtime.save_scene_as(scene_path), "editor hierarchy smoke initial checkpoint failed") ||
        !require(!runtime.dirty(), "editor hierarchy smoke initial checkpoint remained dirty")) {
        std::filesystem::remove(scene_path, remove_error);
        return 4;
    }

    const auto* initial_value =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(initial_value != nullptr, "editor hierarchy smoke initial Transform is missing")) {
        std::filesystem::remove(scene_path, remove_error);
        return 5;
    }
    const auto initial_transform = *initial_value;
    const std::array<aeg::scene::SceneEntityId, 1> left_selection{{left_id}};
    if (!require(runtime.reparent_entities(left_selection, right_id, static_cast<std::size_t>(-1),
                                           aeg::application::HierarchyTransformPolicy::kKeepWorld),
                 "editor hierarchy smoke reparent failed") ||
        !require(runtime.render_frame(1), "editor hierarchy smoke reparent frame failed") ||
        !require(has_parent(runtime, left_id, right_id), "editor hierarchy smoke parent relation was not committed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 6;
    }
    const auto* preserved_value =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(preserved_value != nullptr && !same_transform(*preserved_value, initial_transform),
                 "editor hierarchy smoke Keep World did not update local Transform") ||
        !require(runtime.undo(), "editor hierarchy smoke hierarchy undo failed") ||
        !require(runtime.render_frame(2), "editor hierarchy smoke hierarchy undo frame failed") ||
        !require(has_parent(runtime, left_id, {}), "editor hierarchy smoke undo did not restore the root relation")) {
        std::filesystem::remove(scene_path, remove_error);
        return 7;
    }
    const auto* undo_value =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(left_id, transform_type));
    if (!require(undo_value != nullptr && same_transform(*undo_value, initial_transform),
                 "editor hierarchy smoke hierarchy undo did not restore Transform") ||
        !require(runtime.redo(), "editor hierarchy smoke hierarchy redo failed") ||
        !require(runtime.render_frame(3), "editor hierarchy smoke hierarchy redo frame failed") ||
        !require(has_parent(runtime, left_id, right_id),
                 "editor hierarchy smoke redo did not restore parent relation")) {
        std::filesystem::remove(scene_path, remove_error);
        return 8;
    }

    const std::array<aeg::scene::SceneEntityId, 1> right_selection{{right_id}};
    if (!require(!runtime.reparent_entities(right_selection, left_id),
                 "editor hierarchy smoke accepted a cycle-producing reparent") ||
        !require(runtime.can_undo(), "editor hierarchy smoke failed cycle changed history state")) {
        std::filesystem::remove(scene_path, remove_error);
        return 9;
    }
    const std::array<aeg::scene::SceneEntityId, 2> nested_sources{{right_id, left_id}};
    if (!require(!runtime.reparent_entities(nested_sources, camera_id),
                 "editor hierarchy smoke accepted nested reparent sources")) {
        std::filesystem::remove(scene_path, remove_error);
        return 10;
    }

    if (!require(
            runtime.reparent_entities(left_selection, {}, 0, aeg::application::HierarchyTransformPolicy::kKeepWorld),
            "editor hierarchy smoke root reorder failed") ||
        !require(runtime.render_frame(4), "editor hierarchy smoke root reorder frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 11;
    }
    const auto roots = runtime.children({});
    if (!require(roots && !roots.value.empty() && roots.value.front() == left_id,
                 "editor hierarchy smoke sibling order was not changed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 12;
    }
    if (!require(runtime.save_scene(), "editor hierarchy smoke reordered save failed") ||
        !require(runtime.open_scene(scene_path), "editor hierarchy smoke reordered reload failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 13;
    }
    const auto reloaded_roots = runtime.children({});
    if (!require(reloaded_roots && !reloaded_roots.value.empty() && reloaded_roots.value.front() == left_id,
                 "editor hierarchy smoke sibling order did not persist")) {
        std::filesystem::remove(scene_path, remove_error);
        return 14;
    }

    const auto* right_before_local_reparent =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(right_id, transform_type));
    if (!require(right_before_local_reparent != nullptr,
                 "editor hierarchy smoke Keep Local source Transform is missing")) {
        std::filesystem::remove(scene_path, remove_error);
        return 14;
    }
    const auto right_local_transform = *right_before_local_reparent;
    if (!require(runtime.reparent_entities(right_selection, camera_id, static_cast<std::size_t>(-1),
                                           aeg::application::HierarchyTransformPolicy::kKeepLocal),
                 "editor hierarchy smoke Keep Local reparent failed") ||
        !require(runtime.render_frame(5), "editor hierarchy smoke Keep Local frame failed") ||
        !require(has_parent(runtime, right_id, camera_id),
                 "editor hierarchy smoke Keep Local parent was not committed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 15;
    }
    const auto* right_after_local_reparent =
        static_cast<const aeg::engine::components::Transform*>(runtime.component_value(right_id, transform_type));
    if (!require(
            right_after_local_reparent != nullptr && same_transform(*right_after_local_reparent, right_local_transform),
            "editor hierarchy smoke Keep Local changed the local Transform") ||
        !require(runtime.undo(), "editor hierarchy smoke Keep Local undo failed") ||
        !require(runtime.render_frame(6), "editor hierarchy smoke Keep Local undo frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 16;
    }

    const std::array<aeg::scene::SceneEntityId, 2> multi_selection{{left_id, right_id}};
    if (!require(runtime.reparent_entities(multi_selection, camera_id, static_cast<std::size_t>(-1),
                                           aeg::application::HierarchyTransformPolicy::kKeepWorld),
                 "editor hierarchy smoke multi-selection reparent failed") ||
        !require(runtime.render_frame(7), "editor hierarchy smoke multi-selection frame failed") ||
        !require(has_parent(runtime, left_id, camera_id) && has_parent(runtime, right_id, camera_id),
                 "editor hierarchy smoke multi-selection relation was not committed") ||
        !require(runtime.undo(), "editor hierarchy smoke multi-selection undo failed") ||
        !require(runtime.render_frame(8), "editor hierarchy smoke multi-selection undo frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 17;
    }
    if (!require(!has_parent(runtime, left_id, camera_id) && !has_parent(runtime, right_id, camera_id),
                 "editor hierarchy smoke multi-selection undo was not atomic")) {
        std::filesystem::remove(scene_path, remove_error);
        return 18;
    }

    if (!require(runtime.play(), "editor hierarchy smoke Play failed") ||
        !require(!runtime.reparent_entities(left_selection, camera_id),
                 "editor hierarchy smoke allowed hierarchy editing in Play mode") ||
        !require(runtime.stop(), "editor hierarchy smoke Stop failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 19;
    }

    if (!require(runtime.create_scene_entity(camera_id, "Hierarchy Child"),
                 "editor hierarchy smoke child creation failed") ||
        !require(runtime.render_frame(9), "editor hierarchy smoke child creation frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 20;
    }
    const aeg::scene::SceneEntityId created_id = runtime.selected_entity();
    if (!require(created_id.is_valid() && has_parent(runtime, created_id, camera_id),
                 "editor hierarchy smoke created child relation is invalid")) {
        std::filesystem::remove(scene_path, remove_error);
        return 21;
    }
    if (!require(runtime.create_scene_entity(created_id, "Nested Child"),
                 "editor hierarchy smoke nested child creation failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 22;
    }
    const aeg::scene::SceneEntityId nested_id = runtime.selected_entity();
    const std::array<aeg::scene::SceneEntityId, 1> nested_selection{{nested_id}};
    const std::array<aeg::scene::SceneEntityId, 1> created_selection{{created_id}};
    if (!require(runtime.remove_entities(created_selection, aeg::scene::SceneEntityRemovalPolicy::kPromoteChildren),
                 "editor hierarchy smoke promote-children delete failed") ||
        !require(runtime.render_frame(10), "editor hierarchy smoke promote-children frame failed") ||
        !require(!has_entity(runtime, created_id) && has_parent(runtime, nested_id, camera_id),
                 "editor hierarchy smoke promoted child relation is invalid") ||
        !require(runtime.undo(), "editor hierarchy smoke promote-children undo failed") ||
        !require(runtime.render_frame(11), "editor hierarchy smoke promote-children undo frame failed") ||
        !require(has_entity(runtime, created_id) && has_parent(runtime, nested_id, created_id),
                 "editor hierarchy smoke promote-children undo did not restore hierarchy") ||
        !require(runtime.redo(), "editor hierarchy smoke promote-children redo failed") ||
        !require(runtime.render_frame(12), "editor hierarchy smoke promote-children redo frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 23;
    }
    if (!require(runtime.create_scene_entity(camera_id, "Cascade Source"),
                 "editor hierarchy smoke cascade source creation failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 24;
    }
    const aeg::scene::SceneEntityId cascade_source_id = runtime.selected_entity();
    if (!require(runtime.create_scene_entity(cascade_source_id, "Cascade Child"),
                 "editor hierarchy smoke cascade child creation failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 25;
    }
    const aeg::scene::SceneEntityId cascade_child_id = runtime.selected_entity();
    const std::array<aeg::scene::SceneEntityId, 1> cascade_selection{{cascade_source_id}};
    if (!require(runtime.remove_entities(cascade_selection, aeg::scene::SceneEntityRemovalPolicy::kCascade),
                 "editor hierarchy smoke cascade delete failed") ||
        !require(runtime.render_frame(13), "editor hierarchy smoke cascade delete frame failed") ||
        !require(!has_entity(runtime, cascade_source_id) && !has_entity(runtime, cascade_child_id),
                 "editor hierarchy smoke cascade delete left descendants") ||
        !require(runtime.duplicate_entities(nested_selection), "editor hierarchy smoke duplicate failed") ||
        !require(runtime.render_frame(14), "editor hierarchy smoke duplicate frame failed") ||
        !require(runtime.selected_entities().size() == 1 && runtime.selected_entity() != nested_id,
                 "editor hierarchy smoke duplicate selection is invalid")) {
        std::filesystem::remove(scene_path, remove_error);
        return 26;
    }
    if (!require(runtime.remove_entities(nested_selection), "editor hierarchy smoke delete failed") ||
        !require(runtime.render_frame(15), "editor hierarchy smoke delete frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 27;
    }

    std::filesystem::remove(scene_path, remove_error);
    if (!require(runtime.shutdown(), "editor hierarchy smoke shutdown failed")) {
        return 28;
    }
    std::cout << "Editor hierarchy smoke passed\n";
    return 0;
}
