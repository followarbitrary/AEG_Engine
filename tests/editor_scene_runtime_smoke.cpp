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
    const bool rejected_light_update = !runtime.queue_component_update(
        light_entity->id, light_type, std::as_bytes(std::span{&invalid_light, 1}));
    const auto rejected_light_frame = runtime.render_frame(0);
    if (!require(
            rejected_light_update, "editor scene smoke accepted an out-of-range light color") ||
        !require(rejected_light_frame, rejected_light_frame.message.empty()
                                          ? "editor scene smoke rejected-light frame failed"
                                          : rejected_light_frame.message.c_str())) {
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
        !require(runtime.play(), "editor scene smoke Resume failed") ||
        !require(runtime.mode() == aeg::application::EditorSceneRuntime::Mode::kPlay,
                 "editor scene smoke Resume did not restore Play mode") ||
        !require(runtime.render_frame(4), "editor scene smoke resumed Play frame failed") ||
        !require(runtime.stop(), "editor scene smoke Stop failed") ||
        !require(runtime.mode() == aeg::application::EditorSceneRuntime::Mode::kEdit,
                 "editor scene smoke Stop did not return to Edit mode") ||
        !require(runtime.request_viewport_extent(72, 48), "editor scene smoke resize request failed") ||
        !require(runtime.render_frame(5), "editor scene smoke resized frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 12;
    }

    const auto pipelines = runtime.pipeline_snapshots();
    const auto renderer_pipeline = std::find_if(
        pipelines.begin(), pipelines.end(),
        [](const auto& snapshot) { return snapshot.first == "Game / Renderer" && snapshot.second != nullptr; });
    if (!require(renderer_pipeline != pipelines.end(), "editor scene smoke Renderer pipeline is missing")) {
        std::filesystem::remove(scene_path, remove_error);
        return 19;
    }
    std::vector<std::pair<uint32_t, uint32_t>> present_inputs;
    for (const auto& dependency : renderer_pipeline->second->dependencies) {
        if (dependency.destination_node < renderer_pipeline->second->nodes.size() &&
            renderer_pipeline->second->nodes[dependency.destination_node].phase == aeg::pipeline::EnginePhase::kPresent) {
            present_inputs.emplace_back(dependency.source_node, dependency.destination_node);
        }
    }
    if (!require(!present_inputs.empty(), "editor scene smoke Renderer pipeline has no Present input to remove")) {
        std::filesystem::remove(scene_path, remove_error);
        return 20;
    }
    for (const auto& [source_node, destination_node] : present_inputs) {
        if (!require(runtime.remove_pipeline_runtime_ordering(
                         aeg::application::EditorSceneRuntime::PipelineTarget::kRenderer, source_node, destination_node),
                     "editor scene smoke could not remove a Present input")) {
            std::filesystem::remove(scene_path, remove_error);
            return 21;
        }
    }
    if (!require(runtime.render_frame(6), "editor scene smoke disconnected Present frame failed") ||
        !require(!runtime.viewport_present_connected(),
                 "editor scene smoke viewport remained connected after Present inputs were removed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 22;
    }
    if (!require(runtime.request_viewport_readback(), "editor scene smoke disconnected Present readback request failed") ||
        !require(runtime.render_frame(7), "editor scene smoke disconnected Present readback frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 23;
    }
    const auto disconnected_pixels = runtime.read_viewport_pixels();
    if (!require(disconnected_pixels, "editor scene smoke disconnected Present readback failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 24;
    }
    for (std::size_t index = 0; index + 3 < disconnected_pixels.value.size(); index += 4) {
        if (!require(std::to_integer<uint8_t>(disconnected_pixels.value[index]) < 10 &&
                         std::to_integer<uint8_t>(disconnected_pixels.value[index + 1]) < 10 &&
                         std::to_integer<uint8_t>(disconnected_pixels.value[index + 2]) < 10,
                     "editor scene smoke disconnected Present retained rendered color")) {
            std::filesystem::remove(scene_path, remove_error);
            return 25;
        }
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
        !require(runtime.entities().empty(), "editor scene smoke new project inherited editor sample entities") ||
        !require(runtime.pipeline_snapshots().empty(),
                 "editor scene smoke new project exposed the internal renderer pipeline") ||
        !require(std::ranges::none_of(runtime.resource_descriptors(),
                                     [](const auto& resource) { return resource.path.empty(); }),
                 "editor scene smoke new project exposed editor sample resources") ||
        !require(runtime.create_project_renderer_pipeline(),
                 "editor scene smoke could not create the project renderer pipeline") ||
        !require(std::ranges::any_of(runtime.pipeline_snapshots(),
                                    [](const auto& snapshot) { return snapshot.first == "Game / Renderer"; }),
                 "editor scene smoke created project renderer pipeline is missing") ||
        !require(runtime.pipeline_snapshots().front().second->nodes.size() == 1 &&
                     runtime.pipeline_snapshots().front().second->nodes.front().phase ==
                         aeg::pipeline::EnginePhase::kPresent &&
                     runtime.pipeline_snapshots().front().second->feature_implementations.empty(),
                 "editor scene smoke new project renderer pipeline is not a Present-only output bus") ||
        !require(!runtime.viewport_present_connected(),
                 "editor scene smoke project renderer pipeline did not replace the live Engine state flow") ||
        !require(runtime.render_frame(100), "editor scene smoke empty project viewport frame failed") ||
        !require(runtime.play(), "editor scene smoke empty project Play failed") ||
        !require(runtime.render_frame(101), "editor scene smoke empty project Play frame failed") ||
        !require(runtime.stop(), "editor scene smoke empty project Stop failed") ||
        !require(runtime.save_scene(), "editor scene smoke project scene save failed") ||
        !require(runtime.import_project(project_root), "editor scene smoke project reimport failed") ||
        !require(runtime.entities().empty(), "editor scene smoke imported project restored editor sample entities") ||
        !require(std::ranges::none_of(runtime.resource_descriptors(),
                                     [](const auto& resource) { return resource.path.empty(); }),
                 "editor scene smoke imported project exposed editor sample resources") ||
        !require(runtime.create_scene_entity({}, "Project Entity"),
                 "editor scene smoke project entity creation failed") ||
        !require(runtime.save_scene(), "editor scene smoke populated project scene save failed") ||
        !require(runtime.import_project(project_root), "editor scene smoke populated project reimport failed") ||
        !require(runtime.entities().size() == 1 && runtime.entities().front().name == "Project Entity",
                 "editor scene smoke project reimport did not preserve project content")) {
        std::filesystem::remove(project_root / "Scenes" / "Main.aegscene", remove_error);
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    const auto default_project_scene = runtime.scene_path();
    const auto missing_recent_scene = project_root / "Scenes" / "MissingRecent.aegscene";
    if (!require(runtime.save_scene_as(missing_recent_scene),
                 "editor scene smoke alternate recent scene save failed")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    std::filesystem::remove(missing_recent_scene, remove_error);
    if (!require(!remove_error, "editor scene smoke alternate recent scene removal failed") ||
        !require(runtime.import_project(project_root),
                 "editor scene smoke missing recent scene fallback import failed") ||
        !require(runtime.scene_path() == default_project_scene,
                 "editor scene smoke import did not fall back to the default scene") ||
        !require(runtime.entities().size() == 1 && runtime.entities().front().name == "Project Entity",
                 "editor scene smoke default scene fallback lost project content") ||
        !require(runtime.new_scene(), "editor scene smoke project New Scene failed") ||
        !require(runtime.entities().empty(), "editor scene smoke project New Scene restored editor sample entities")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    std::filesystem::remove(default_project_scene, remove_error);
    if (!require(!remove_error, "editor scene smoke default scene removal failed") ||
        !require(runtime.import_project(project_root), "editor scene smoke scene-less project import failed") ||
        !require(runtime.entities().empty(), "editor scene smoke scene-less project import restored sample entities") ||
        !require(std::ranges::none_of(runtime.resource_descriptors(),
                                     [](const auto& resource) { return resource.path.empty(); }),
                 "editor scene smoke scene-less project import exposed sample resources")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    if (!require(runtime.shutdown(), "editor scene smoke shutdown failed")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 18;
    }
    aeg::application::EditorSceneRuntime imported_runtime;
    if (!require(imported_runtime.initialize(*device), "editor scene smoke imported runtime initialization failed") ||
        !require(imported_runtime.import_project(project_root), "editor scene smoke fresh project import failed") ||
        !require(imported_runtime.pipeline_snapshots().empty(),
                 "editor scene smoke fresh import exposed an internal renderer pipeline without a document") ||
        !require(imported_runtime.shutdown(), "editor scene smoke imported runtime shutdown failed")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 18;
    }
    std::filesystem::remove_all(project_root, remove_error);
    std::cout << "Editor scene runtime smoke passed\n";
    return 0;
}
