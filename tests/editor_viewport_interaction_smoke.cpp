/**
 * @file editor_viewport_interaction_smoke.cpp
 * @brief Verifies CPU viewport picking and editor-owned Transform transactions.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "editor_scene_runtime.hpp"
#include "composition/editor_render_graph_inspection.hpp"
#include "renderer_feature_profiles.hpp"
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

bool preview_has_execution_source(const aeg::application::EditorSceneRuntime& runtime,
                                  aeg::feature::RuntimeGeneration expected = {}) {
    using namespace aeg;
    const auto execution = runtime.inspect_renderer_feature_execution();
    const auto captured = runtime.inspect_render_graph();
    if (!require(execution && captured && execution.value.generation.is_valid() &&
                 (!expected.is_valid() || execution.value.generation == expected) &&
                 execution.value.provenance.size() == 1 && execution.value.plans.size() == 1 &&
                 captured.value->plan.revision != 0,
                 "viewport Preview has no real Feature ExecutionPlan/RenderGraph source")) return false;
    const auto& origin = execution.value.provenance.front();
    if (!require(origin.runtime.definition_id == feature::FeatureDefinitionId{"Renderer.Scene.Opaque"} &&
                 origin.runtime.owner_module == feature::ModuleIdentity{"com.aeg.renderer"} &&
                 origin.runtime.node_id.is_valid() && origin.runtime.instance_id.is_valid() &&
                 origin.authored_node_id == feature::FeatureNodeId{"Renderer.Scene.Root"} &&
                 std::ranges::find(execution.value.plans.front().instances, origin.runtime.instance_id) !=
                     execution.value.plans.front().instances.end(),
                 "viewport Preview ExecutionPlan lost actual Definition/Node/Instance/module attribution")) return false;
    const auto source = std::ranges::find_if(captured.value->sources, [&](const auto& value) {
        return value.asset_uuid == execution.value.asset_uuid && value.generation == execution.value.generation;
    });
    if (!require(source != captured.value->sources.end() && source->provenance.size() == 1 &&
                 source->provenance.front().runtime.instance_id == origin.runtime.instance_id,
                 "viewport RenderGraph does not retain its actual Preview publication")) return false;
    bool drew = false;
    for (const auto& pass : captured.value->plan.pass_plans) {
        if (!pass.live || !pass.feature_work) continue;
        const auto& work = *pass.feature_work;
        if (!require(work.feature_definition_id == origin.runtime.definition_id &&
                     work.owner_module == origin.runtime.owner_module && work.node_id == origin.runtime.node_id &&
                     work.instance_id == origin.runtime.instance_id && work.frame_index == captured.value->frame_index &&
                     work.operation_index == 0 && work.invocation.invocation_id.is_valid() &&
                     work.invocation.generation == execution.value.generation,
                     "viewport business Scene draw lost real invocation/frame/Feature provenance")) return false;
        drew = true;
    }
    return require(drew && runtime.draw_call_count() != 0, "viewport Preview has no actual Feature Scene draw");
}

bool scale_drag_follows_visible_axis(aeg::application::EditorSceneRuntime& runtime) {
    using aeg::application::ViewportGizmoAxis;
    using aeg::application::ViewportGizmoMode;
    using aeg::application::ViewportPoint;
    using aeg::engine::components::Transform;
    const auto transform_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName);
    const auto* camera = find_entity(runtime, "Camera");
    const auto* mesh = find_entity(runtime, "Right Mesh");
    if (!require(camera && mesh, "scale direction regression entities are missing"))
        return false;
    const auto camera_id = camera->id;
    const auto mesh_id = mesh->id;
    const Transform original_camera =
        *static_cast<const Transform*>(runtime.component_value(camera_id, transform_type));
    const Transform initial = *static_cast<const Transform*>(runtime.component_value(mesh_id, transform_type));
    if (!require(runtime.select(mesh_id), "scale direction selection failed") ||
        !require(runtime.request_viewport_extent(320, 240), "scale direction extent failed"))
        return false;
    uint64_t frame = 21;
    // View the same object from opposite sides: +Z projects left/down, then right/down.
    for (float yaw : {0.65f, -0.65f}) {
        Transform camera_pose = original_camera;
        camera_pose.rotation = aeg::math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) *
                               aeg::math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.35f);
        camera_pose.translation = initial.translation + aeg::math::rotate(camera_pose.rotation, {0.0f, 0.0f, 6.0f});
        if (!require(
                runtime.queue_component_update(camera_id, transform_type, std::as_bytes(std::span{&camera_pose, 1})),
                "scale direction camera update failed") ||
            !require(runtime.render_frame(frame++, 1.0 / 60.0), "scale direction camera frame failed"))
            return false;
        for (const auto axis : {ViewportGizmoAxis::kZ, ViewportGizmoAxis::kX, ViewportGizmoAxis::kY}) {
            const auto projection = runtime.selected_gizmo_projection(320, 240);
            if (!require(projection && projection.value.valid, "scale direction projection failed"))
                return false;
            const std::array<ViewportPoint, 3> tips{projection.value.x_axis, projection.value.y_axis,
                                                    projection.value.z_axis};
            const auto index = static_cast<std::size_t>(axis);
            const ViewportPoint tip = tips[index];
            const float dx = tip.x - projection.value.center.x;
            const float dy = tip.y - projection.value.center.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (!require(length > 1.0f, "scale direction fixture has a collapsed handle"))
                return false;
            // Outward must grow, inward must shrink, perpendicular motion must not scale.
            for (const int direction : {1, -1, 0}) {
                const ViewportPoint end = direction == 0
                                              ? ViewportPoint{tip.x - dy / length * 12.0f, tip.y + dx / length * 12.0f}
                                              : ViewportPoint{tip.x + dx / length * 12.0f * direction,
                                                              tip.y + dy / length * 12.0f * direction};
                if (!require(runtime.begin_transform_edit(ViewportGizmoMode::kScale, axis, tip, 320, 240),
                             "scale direction begin failed") ||
                    !require(runtime.update_transform_edit(end, 320, 240), "scale direction update failed") ||
                    !require(runtime.render_frame(frame++, 1.0 / 60.0), "scale direction preview frame failed"))
                    return false;
                const auto* edited = static_cast<const Transform*>(runtime.component_value(mesh_id, transform_type));
                if (!require(edited != nullptr, "scale direction edited Transform is missing"))
                    return false;
                const float before = initial.scale[index];
                const float after = edited->scale[index];
                const bool correct = direction > 0   ? after > before
                                     : direction < 0 ? after > 0.0f && after < before
                                                     : aeg::math::nearly_equal(after, before, 1.0e-4f);
                if (!correct) {
                    std::cerr << "Scale drag disagrees with visible axis: axis=" << index << ", yaw=" << yaw
                              << ", direction=" << direction << ", before=" << before << ", after=" << after << '\n';
                    return false;
                }
                Transform unchanged = *edited;
                unchanged.scale[index] = initial.scale[index];
                if (!require(same_transform(unchanged, initial), "axis scale changed another Transform component") ||
                    !require(runtime.cancel_transform_edit(), "scale direction cancel failed") ||
                    !require(runtime.render_frame(frame++, 1.0 / 60.0), "scale direction cancel frame failed"))
                    return false;
                const auto* cancelled = static_cast<const Transform*>(runtime.component_value(mesh_id, transform_type));
                if (!require(cancelled && same_transform(*cancelled, initial),
                             "scale direction cancel did not restore Transform"))
                    return false;
            }
        }
    }
    return require(
               runtime.queue_component_update(camera_id, transform_type, std::as_bytes(std::span{&original_camera, 1})),
               "scale direction camera restore failed") &&
           require(runtime.render_frame(frame, 1.0 / 60.0), "scale direction restore frame failed");
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
        !require(runtime.render_frame(0, 1.0 / 60.0), "viewport interaction smoke initial frame failed")) {
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
        !require(runtime.render_frame(1, 1.0 / 60.0), "viewport interaction smoke zoomed frame failed") ||
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
        !require(runtime.render_frame(2, 1.0 / 60.0), "viewport interaction smoke committed frame failed")) {
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
        !require(runtime.render_frame(3, 1.0 / 60.0), "viewport interaction smoke cancelled frame failed")) {
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
        !require(runtime.render_frame(3, 1.0 / 60.0), "viewport interaction smoke Play frame failed") ||
        !require(runtime.stop(), "viewport interaction smoke Stop failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 14;
    }
    const auto* after_stop = static_cast<const aeg::engine::components::Transform*>(
        runtime.component_value(reopened_left_id, transform_type));
    const bool preserved = after_stop != nullptr && same_transform(*after_stop, committed_copy);
    std::filesystem::remove(scene_path, remove_error);
    const auto mesh_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName);
    for (const char* name : {"Left Mesh", "Right Mesh"}) {
        const auto* entity = find_entity(runtime, name);
        if (!require(entity, "visibility regression entity missing"))
            return 19;
        auto mesh =
            *static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(entity->id, mesh_type));
        mesh.visible = false;
        if (!require(runtime.select(entity->id), "visibility selection failed") ||
            !require(runtime.queue_component_update(entity->id, mesh_type, std::as_bytes(std::span{&mesh, 1})),
                     "visibility update failed") ||
            !require(runtime.render_frame(10, 1.0 / 60.0), "visibility frame failed"))
            return 20;
        const auto hidden_gizmo = runtime.selected_gizmo_projection(96, 64);
        if (!require(hidden_gizmo && !hidden_gizmo.value.valid, "hidden entity still exposes a gizmo"))
            return 21;
    }
    if (!require(runtime.request_viewport_readback(), "empty viewport readback request failed") ||
        !require(runtime.render_frame(11, 1.0 / 60.0), "empty viewport frame failed"))
        return 22;
    const auto pixels = runtime.read_viewport_pixels();
    if (!require(pixels, "empty viewport readback failed"))
        return 23;
    for (std::size_t i = 0; i + 3 < pixels.value.size(); i += 4) {
        if (!require(std::to_integer<int>(pixels.value[i]) < 10 && std::to_integer<int>(pixels.value[i + 1]) < 10 &&
                         std::to_integer<int>(pixels.value[i + 2]) < 10,
                     "all hidden viewport retained previous colored geometry"))
            return 24;
    }
    const auto* restore_entity = find_entity(runtime, "Right Mesh");
    auto restored_mesh = *static_cast<const aeg::engine::components::MeshRenderer*>(
        runtime.component_value(restore_entity->id, mesh_type));
    restored_mesh.visible = true;
    if (!require(
            runtime.queue_component_update(restore_entity->id, mesh_type, std::as_bytes(std::span{&restored_mesh, 1})),
            "restore visibility failed") ||
        !require(runtime.request_viewport_readback(), "restore readback request failed") ||
        !require(runtime.render_frame(12, 1.0 / 60.0), "restore frame failed"))
        return 25;
    const auto restored_gizmo = runtime.selected_gizmo_projection(96, 64);
    const auto restored_pixels = runtime.read_viewport_pixels();
    bool has_geometry = false;
    if (!require(restored_pixels && restored_gizmo && restored_gizmo.value.valid,
                 "restoring visibility did not restore gizmo/readback"))
        return 26;
    for (std::size_t i = 0; i + 3 < restored_pixels.value.size(); i += 4)
        has_geometry |= std::to_integer<int>(restored_pixels.value[i + 1]) > 40;
    if (!require(has_geometry, "restoring visibility did not restore rendered geometry"))
        return 27;

    if (!preview_has_execution_source(runtime)) return 28;
    const auto source = runtime.inspect_renderer_feature_execution();
    const auto preset = aeg::renderer::scene_render_profile_graph(aeg::renderer::SceneRenderProfile::kOpaque);
    if (!require(source && preset, "actual Renderer Feature preset is unavailable")) return 29;
    const auto initial_generation = source.value.generation;
    std::vector<aeg::feature::FeatureGraphDocument> invalid_graphs(4, preset.value);
    invalid_graphs[0].nodes.front().definition_id = aeg::feature::FeatureDefinitionId{"Game.Logic.Foreign"};
    invalid_graphs[1].nodes.front().definition_version.major += 1;
    invalid_graphs[2].contract_digest ^= 1;
    const auto node = preset.value.nodes.front().node_id;
    invalid_graphs[3].edges.push_back({node, aeg::feature::PortId{"Out"}, node, aeg::feature::PortId{"In"},
        aeg::feature::EdgeKind::kData, aeg::renderer::kSceneRenderProfileClock,
        aeg::renderer::kSceneRenderProfileClock, 1, 1, 0});
    uint64_t feature_frame = 13;
    for (const auto& invalid : invalid_graphs) {
        const auto rejected = runtime.publish_renderer_feature_graph(invalid);
        const auto retained = runtime.inspect_renderer_feature_execution();
        if (!require(!rejected && rejected.code == aeg::foundation::ResultCode::kInvalidArgument &&
                     retained && retained.value.generation == initial_generation &&
                     retained.value.asset_uuid == source.value.asset_uuid &&
                     retained.value.graph_version == source.value.graph_version,
                     "invalid Preview Feature publication changed the last-good source") ||
            !require(runtime.request_viewport_readback(), "rejected Feature readback request failed") ||
            !require(runtime.render_frame(feature_frame++, 1.0 / 60.0), "rejected Feature continuation frame failed") ||
            !preview_has_execution_source(runtime, initial_generation)) return 31;
        const auto retained_pixels = runtime.read_viewport_pixels();
        if (!require(retained_pixels && retained_pixels.value == restored_pixels.value,
                     "rejected Preview Feature publication changed actual last-good pixels")) return 32;
    }
    auto empty = preset.value;
    empty.graph_version += 1;
    empty.nodes.clear();
    if (!require(runtime.publish_renderer_feature_graph(empty), "empty Preview Feature publication failed") ||
        !require(runtime.request_viewport_readback(), "empty Feature readback request failed") ||
        !require(runtime.render_frame(feature_frame++, 1.0 / 60.0), "empty Feature frame failed")) return 33;
    const auto empty_source = runtime.inspect_renderer_feature_execution();
    const auto empty_graph = runtime.inspect_render_graph();
    const auto empty_pixels = runtime.read_viewport_pixels();
    if (!require(empty_source && empty_source.value.generation != initial_generation &&
                 empty_source.value.plans.empty() && empty_source.value.provenance.empty() &&
                 empty_graph && std::ranges::none_of(empty_graph.value->plan.pass_plans,
                     [](const auto& pass) { return pass.feature_work.has_value(); }) &&
                 runtime.draw_call_count() == 0 &&
                 empty_pixels && empty_pixels.value == restored_pixels.value,
                 "empty Feature Preview performed hidden rendering or replayed retired business work")) return 34;
    if (!require(runtime.restore_default_renderer_feature(), "actual default Feature Preview restore failed") ||
        !require(runtime.request_viewport_readback(), "restored Feature readback request failed") ||
        !require(runtime.render_frame(feature_frame++, 1.0 / 60.0), "restored Feature frame failed") ||
        !preview_has_execution_source(runtime)) return 35;
    const auto restored_source = runtime.inspect_renderer_feature_execution();
    const auto reconnected_pixels = runtime.read_viewport_pixels();
    bool reconnected_geometry = false;
    if (!require(restored_source && restored_source.value.generation != empty_source.value.generation &&
                 restored_source.value.generation != initial_generation && !restored_source.value.plans.empty() &&
                 reconnected_pixels && reconnected_pixels.value == restored_pixels.value,
                 "default restore did not publish a new actual Opaque Feature source/pixels")) return 36;
    for (std::size_t i = 0; i + 3 < reconnected_pixels.value.size(); i += 4)
        reconnected_geometry |= std::to_integer<int>(reconnected_pixels.value[i + 1]) > 40;
    if (!require(reconnected_geometry, "restored Opaque Feature did not restore rendered geometry")) return 37;
    // Continue past the frames-in-flight rotation with fresh Scene bindings and actual Feature provenance.
    for (uint64_t frame = 15; frame < 21; ++frame) {
        const auto continued_frame = runtime.render_frame(frame, 1.0 / 60.0);
        if (!require(continued_frame, continued_frame.message.empty() ? "Feature Preview continuation frame failed"
                                                                      : continued_frame.message.c_str()) ||
            !preview_has_execution_source(runtime, restored_source.value.generation)) {
            return 38;
        }
    }
    if (!scale_drag_follows_visible_axis(runtime))
        return 39;
    if (!require(preserved, "viewport interaction smoke Play/Stop polluted the edit Transform") ||
        !require(runtime.shutdown(), "viewport interaction smoke shutdown failed")) {
        return 15;
    }
    std::cout << "Editor viewport interaction smoke passed\n";
    return 0;
}
