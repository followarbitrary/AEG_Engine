/**
 * @file editor_scene_runtime_smoke.cpp
 * @brief Verifies the minimum editable Scene document and offscreen preview loop.
 */
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
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

bool same_tint(const aeg::engine::components::MeshRenderer& left, const aeg::engine::components::MeshRenderer& right) {
    return left.tint.x == right.tint.x && left.tint.y == right.tint.y && left.tint.z == right.tint.z &&
           left.tint_alpha == right.tint_alpha;
}
}  // namespace

int main() {
    const aeg::rhi::DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan,
        "AEG Editor Scene Runtime Smoke",
        std::numeric_limits<uint32_t>::max(),
        true,
        true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "editor scene smoke device creation failed")) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);

    aeg::application::EditorSceneRuntime runtime;
    if (!require(runtime.initialize(*device), "editor scene smoke runtime initialization failed") ||
        !require(runtime.request_viewport_extent(96, 64), "editor scene smoke viewport request failed") ||
        !require(runtime.entities().size() == 4, "editor scene smoke default scene entity count is invalid")) {
        return 2;
    }

    const auto light_entity =
        std::find_if(runtime.entities().begin(), runtime.entities().end(),
                     [](const aeg::scene::SceneEntityRecord& entity) { return entity.name == "Directional Light"; });
    const aeg::ecs::ComponentTypeId light_type(aeg::engine::components::kDirectionalLightName);
    const auto* source_light = light_entity == runtime.entities().end()
                                   ? nullptr
                                   : static_cast<const aeg::engine::components::DirectionalLight*>(
                                         runtime.component_value(light_entity->id, light_type));
    if (!require(light_entity != runtime.entities().end(), "editor scene smoke directional light is missing") ||
        !require(source_light != nullptr, "editor scene smoke directional light value is missing")) {
        return 3;
    }
    aeg::engine::components::DirectionalLight invalid_light = *source_light;
    invalid_light.color = {444.0f, 444.0f, 444.0f};
    if (!require(
            !runtime.queue_component_update(light_entity->id, light_type, std::as_bytes(std::span{&invalid_light, 1})),
            "editor scene smoke accepted an out-of-range light color") ||
        !require(runtime.render_frame(0), "editor scene smoke rejected-light frame failed")) {
        return 4;
    }
    const auto* preserved_light = static_cast<const aeg::engine::components::DirectionalLight*>(
        runtime.component_value(light_entity->id, light_type));
    if (!require(preserved_light != nullptr && preserved_light->color.x <= 1.0f && preserved_light->color.y <= 1.0f &&
                     preserved_light->color.z <= 1.0f,
                 "editor scene smoke invalid light edit polluted the World")) {
        return 5;
    }

    const auto left_entity =
        std::find_if(runtime.entities().begin(), runtime.entities().end(),
                     [](const aeg::scene::SceneEntityRecord& entity) { return entity.name == "Left Mesh"; });
    if (!require(left_entity != runtime.entities().end(), "editor scene smoke left mesh is missing") ||
        !require(runtime.select(left_entity->id), "editor scene smoke entity selection failed")) {
        return 6;
    }

    const aeg::ecs::ComponentTypeId mesh_type(aeg::engine::components::kMeshRendererName);
    const auto descriptor = runtime.component_descriptor(mesh_type);
    const auto* source =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_entity->id, mesh_type));
    if (!require(descriptor, "editor scene smoke MeshRenderer descriptor is missing") ||
        !require(source != nullptr, "editor scene smoke MeshRenderer value is missing") ||
        !require(descriptor.value.size == sizeof(aeg::engine::components::MeshRenderer),
                 "editor scene smoke MeshRenderer storage size is invalid")) {
        return 7;
    }

    aeg::engine::components::MeshRenderer edited = *source;
    edited.tint = {0.2f, 0.7f, 0.95f};
    edited.tint_alpha = 0.8f;
    const auto edited_bytes = std::as_bytes(std::span{&edited, 1});
    if (!require(runtime.queue_component_update(left_entity->id, mesh_type, edited_bytes),
                 "editor scene smoke MeshRenderer edit queue failed") ||
        !require(runtime.render_frame(0), "editor scene smoke first RenderGraph frame failed") ||
        !require(runtime.render_frame(1), "editor scene smoke second RenderGraph frame failed") ||
        !require(runtime.viewport_ready(), "editor scene smoke viewport target is not ready")) {
        return 8;
    }

    const auto* committed =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_entity->id, mesh_type));
    if (!require(committed != nullptr && same_tint(*committed, edited),
                 "editor scene smoke CommandBuffer edit did not commit")) {
        return 9;
    }

    const std::filesystem::path scene_path =
        std::filesystem::temp_directory_path() / "aeg_editor_scene_runtime_smoke.aegscene";
    std::error_code remove_error;
    std::filesystem::remove(scene_path, remove_error);
    if (!require(runtime.save_scene_as(scene_path), "editor scene smoke Save As failed") ||
        !require(!runtime.dirty(), "editor scene smoke scene remained dirty after save") ||
        !require(runtime.open_scene(scene_path), "editor scene smoke reopen failed") ||
        !require(runtime.entities().size() == 4, "editor scene smoke reopened entity count is invalid")) {
        std::filesystem::remove(scene_path, remove_error);
        return 10;
    }

    const auto reopened_left =
        std::find_if(runtime.entities().begin(), runtime.entities().end(),
                     [](const aeg::scene::SceneEntityRecord& entity) { return entity.name == "Left Mesh"; });
    const aeg::scene::SceneEntityId reopened_left_id =
        reopened_left == runtime.entities().end() ? aeg::scene::SceneEntityId{} : reopened_left->id;
    const auto* reopened_value = reopened_left == runtime.entities().end()
                                     ? nullptr
                                     : static_cast<const aeg::engine::components::MeshRenderer*>(
                                           runtime.component_value(reopened_left_id, mesh_type));
    if (!require(reopened_left != runtime.entities().end(), "editor scene smoke reopened left mesh is missing") ||
        !require(reopened_value != nullptr && same_tint(*reopened_value, edited),
                 "editor scene smoke MeshRenderer values did not round-trip")) {
        std::filesystem::remove(scene_path, remove_error);
        return 11;
    }

    if (!require(runtime.play(), "editor scene smoke Play failed") ||
        !require(runtime.render_frame(2), "editor scene smoke Play frame failed") ||
        !require(runtime.pause(), "editor scene smoke Pause failed") ||
        !require(runtime.render_frame(3), "editor scene smoke Pause frame failed") ||
        !require(runtime.stop(), "editor scene smoke Stop failed") ||
        !require(runtime.mode() == aeg::application::EditorSceneRuntime::Mode::kEdit,
                 "editor scene smoke Stop did not return to Edit mode") ||
        !require(runtime.request_viewport_extent(72, 48), "editor scene smoke resize request failed") ||
        !require(runtime.render_frame(4), "editor scene smoke resized frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 12;
    }

    const auto* after_stop =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(reopened_left_id, mesh_type));
    const bool preserved = after_stop != nullptr && same_tint(*after_stop, edited);
    std::filesystem::remove(scene_path, remove_error);
    if (!require(preserved, "editor scene smoke Play/Stop polluted edit World")) {
        return 13;
    }
    aeg::engine::components::MeshRenderer failed_save_edit = edited;
    failed_save_edit.tint.x = 0.15f;
    if (!require(
            runtime.queue_component_update(reopened_left_id, mesh_type, std::as_bytes(std::span{&failed_save_edit, 1})),
            "editor scene smoke dirty setup failed") ||
        !require(runtime.dirty(), "editor scene smoke dirty setup did not mark scene dirty")) {
        std::filesystem::remove(scene_path, remove_error);
        return 14;
    }
    const auto failed_save_directory = std::filesystem::temp_directory_path() / "aeg_editor_save_failure_directory";
    std::filesystem::create_directories(failed_save_directory, remove_error);
    if (!require(!runtime.save_scene_as(failed_save_directory),
                 "editor scene smoke directory Save As unexpectedly passed") ||
        !require(runtime.dirty(), "editor scene smoke failed save cleared dirty state")) {
        std::filesystem::remove_all(failed_save_directory, remove_error);
        std::filesystem::remove(scene_path, remove_error);
        return 15;
    }
    std::filesystem::remove_all(failed_save_directory, remove_error);
    const auto corrupt_scene = std::filesystem::temp_directory_path() / "aeg_editor_corrupt_scene.aegscene";
    std::ofstream(corrupt_scene, std::ios::binary) << "corrupt";
    const auto scene_before_failed_open = runtime.scene_path();
    const auto entity_count_before_failed_open = runtime.entities().size();
    if (!require(!runtime.open_scene(corrupt_scene), "editor scene smoke corrupt scene unexpectedly loaded") ||
        !require(runtime.scene_path() == scene_before_failed_open,
                 "editor scene smoke failed open changed the current scene path") ||
        !require(runtime.entities().size() == entity_count_before_failed_open,
                 "editor scene smoke failed open changed the current scene")) {
        std::filesystem::remove(corrupt_scene, remove_error);
        std::filesystem::remove(scene_path, remove_error);
        return 16;
    }
    std::filesystem::remove(corrupt_scene, remove_error);
    const auto project_root = std::filesystem::temp_directory_path() / "aeg_editor_scene_runtime_project";
    std::filesystem::remove_all(project_root, remove_error);
    const auto new_project_result = runtime.new_project(project_root, "Runtime Smoke Game");
    if (!new_project_result) {
        std::cerr << "editor scene smoke new game project diagnostic: " << new_project_result.message << '\n';
    }
    if (!require(new_project_result, "editor scene smoke new game project failed") ||
        !require(runtime.project_loaded(), "editor scene smoke new game project is not active") ||
        !require(runtime.project_root() == std::filesystem::weakly_canonical(project_root),
                 "editor scene smoke active project root is incorrect") ||
        !require(runtime.save_scene(), "editor scene smoke project scene save failed") ||
        !require(runtime.import_project(project_root), "editor scene smoke project reimport failed") ||
        !require(runtime.entities().size() == 4, "editor scene smoke imported project scene is invalid")) {
        std::filesystem::remove(project_root / "Scenes" / "Main.aegscene", remove_error);
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    std::filesystem::remove_all(project_root, remove_error);
    if (!require(runtime.shutdown(), "editor scene smoke shutdown failed")) {
        return 18;
    }
    std::cout << "Editor scene runtime smoke passed\n";
    return 0;
}
