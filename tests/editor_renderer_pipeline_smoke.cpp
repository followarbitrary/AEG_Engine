/** @brief Verifies real Renderer Feature publication, recovery and viewport pixels. */
#include <algorithm>
#include <array>
#include <chrono>
#include <concurrency.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "editor_scene_runtime.hpp"
#include "composition/editor_render_graph_inspection.hpp"
#include "project_context.hpp"
#include "renderer_feature_profiles.hpp"

namespace {
using namespace aeg;
bool check(const foundation::Result& result) {
    if (!result)
        std::cerr << result.message << '\n';
    return bool(result);
}
bool draw(application::EditorSceneRuntime& runtime, uint64_t frame) {
    std::cerr << "Rendering frame " << frame << '\n';
    if (!check(runtime.request_viewport_readback()) ||
        !check(runtime.render_frame(frame, 1.0 / 60.0)))
        return false;
    const auto pixels = runtime.read_viewport_pixels();
    if (!pixels || pixels.value.empty())
        return false;
    size_t colored = 0;
    for (size_t i = 0; i + 3 < pixels.value.size(); i += 4) {
        if (std::to_integer<unsigned>(pixels.value[i]) > 12 || std::to_integer<unsigned>(pixels.value[i + 1]) > 12)
            ++colored;
    }
    if (colored < 10)
        std::cerr << "Selected renderer did not draw scene pixels\n";
    return colored >= 10;
}
std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
}
bool active(application::EditorSceneRuntime& runtime, renderer::SceneRenderProfile profile) {
    const auto definition = renderer::scene_render_profile_definition(profile);
    const auto source = runtime.inspect_renderer_feature_execution();
    return definition && source && source.value.generation.is_valid() && source.value.plans.size() == 1 &&
        source.value.provenance.size() == 1 &&
        source.value.provenance.front().runtime.definition_id == definition.value.id;
}
bool actual_feature_work(application::EditorSceneRuntime& runtime, renderer::SceneRenderProfile profile) {
    const auto source = runtime.inspect_renderer_feature_execution();
    const auto captured = runtime.inspect_render_graph();
    const auto definition = renderer::scene_render_profile_definition(profile);
    if (!source || !captured || !definition || !active(runtime, profile)) return false;
    const size_t count = profile == renderer::SceneRenderProfile::kOpaque ? 1 :
                         profile == renderer::SceneRenderProfile::kForward ? 5 : 4;
    std::vector<bool> operations(count, false);
    const auto& origin = source.value.provenance.front().runtime;
    for (const auto& pass : captured.value->plan.pass_plans) {
        if (!pass.live || !pass.feature_work) continue;
        const auto& work = *pass.feature_work;
        if (work.feature_definition_id != definition.value.id || work.node_id != origin.node_id ||
            work.instance_id != origin.instance_id || !work.id.is_valid() ||
            work.owner_module != feature::ModuleIdentity("com.aeg.renderer") ||
            work.invocation.generation != source.value.generation || !work.invocation.invocation_id.is_valid() ||
            work.frame_index != captured.value->frame_index || work.operation_index >= count)
            return false;
        operations[work.operation_index] = true;
    }
    return std::ranges::all_of(operations, [](bool value) { return value; }) &&
        std::ranges::any_of(captured.value->sources, [&](const auto& value) {
            return value.asset_uuid == source.value.asset_uuid && value.generation == source.value.generation &&
                value.contract_digest == source.value.contract_digest && !value.plans.empty();
        });
}
bool default_feature_preview(application::EditorSceneRuntime& runtime, uint64_t frame) {
    return active(runtime, renderer::SceneRenderProfile::kOpaque) && draw(runtime, frame) &&
        actual_feature_work(runtime, renderer::SceneRenderProfile::kOpaque);
}
bool retired_authoring_is_rejected(application::EditorSceneRuntime& runtime,
                                   const std::filesystem::path& fixture, uint64_t& frame) {
    const auto source = runtime.inspect_renderer_feature_execution();
    const auto scene_path = runtime.scene_path();
    const auto entity_count = runtime.entities().size();
    const auto project_file = runtime.project_root() / "Project.aegproj";
    const auto project_bytes = read_file(project_file);
    const auto scene_bytes = read_file(scene_path);
    const auto unchanged = [&] {
        const auto after = runtime.inspect_renderer_feature_execution();
        return source && after && after.value.generation == source.value.generation &&
            after.value.contract_digest == source.value.contract_digest && runtime.scene_path() == scene_path &&
            runtime.entities().size() == entity_count && read_file(project_file) == project_bytes &&
            read_file(scene_path) == scene_bytes;
    };
    for (const auto& extension : {".aegframe", ".aegpipe", ".aegpipeline", ".pipeline", ".AEGFRAME", ".AEGPIPE"}) {
        const auto path = runtime.project_root() / "Assets" / (std::string("Retired") + extension);
        std::ofstream(path) << "retired author schema must never be decoded";
        const auto bytes = read_file(path);
        const auto result = runtime.open_scene(path);
        if (result || result.code != foundation::ResultCode::kUnsupported || !unchanged() || read_file(path) != bytes) {
            std::cerr << "Retired author document changed the active Feature source or was not explicitly rejected\n";
            return false;
        }
        const bool dirty_before = runtime.dirty();
        const auto saved = runtime.save_scene_as(path);
        if (saved || saved.code != foundation::ResultCode::kUnsupported || !unchanged() ||
            runtime.dirty() != dirty_before || read_file(path) != bytes) {
            std::cerr << "Save As overwrote retired author bytes or changed the active Scene/Feature source\n";
            return false;
        }
    }
    const auto sidecar = std::filesystem::path(scene_path.string() + ".pipeline");
    for (const auto& bytes : {std::string("AEG_PIPELINE_EDITS 3\n"), std::string("malformed bytes\n")}) {
        std::ofstream(sidecar, std::ios::binary | std::ios::trunc) << bytes;
        for (bool import : {false, true}) {
            const auto result = import ? runtime.import_project(runtime.project_root()) : runtime.open_scene(scene_path);
            if (result || result.code != foundation::ResultCode::kUnsupported || !unchanged() || read_file(sidecar) != bytes)
                return false;
        }
        if (!draw(runtime, frame++) || !unchanged()) return false;
    }
    std::filesystem::remove(sidecar);
    const auto blocked_root = fixture / "BlockedProject";
    const auto blocked_scene = blocked_root / application::project::ProjectConfig{}.startup_scene;
    std::filesystem::create_directories(blocked_scene.parent_path());
    const auto blocked_sidecar = std::filesystem::path(blocked_scene.string() + ".pipeline");
    std::ofstream(blocked_sidecar) << "do not import or mutate";
    const auto blocked = runtime.new_project(blocked_root, "Blocked author project");
    if (blocked || blocked.code != foundation::ResultCode::kUnsupported || !unchanged() ||
        std::filesystem::exists(blocked_root / "Project.aegproj")) return false;
    const auto count = runtime.entities().size();
    return check(runtime.create_scene_entity({}, "Still editable")) && runtime.entities().size() == count + 1 &&
        check(runtime.undo()) && runtime.entities().size() == count && draw(runtime, frame++);
}
bool vertical_orientation_pixels(application::EditorSceneRuntime& runtime, uint64_t& frame) {
    using namespace engine::components;
    scene::SceneEntityId red{}, green{};
    for (const auto& item : runtime.entities()) {
        if (item.name == "Left Mesh")
            red = item.id;
        if (item.name == "Right Mesh")
            green = item.id;
    }
    const ecs::ComponentTypeId type(kTransformName);
    const auto old_red = *static_cast<const Transform*>(runtime.component_value(red, type));
    const auto old_green = *static_cast<const Transform*>(runtime.component_value(green, type));
    Transform lower{}, upper{};
    lower.translation = {0, -.6f, 0};
    upper.translation = {0, .6f, 0};
    lower.scale = upper.scale = {.35f, .35f, 1};
    const std::array<application::ComponentUpdateView, 2> updates{
        {{red, type, std::as_bytes(std::span{&lower, 1})}, {green, type, std::as_bytes(std::span{&upper, 1})}}};
    if (!check(runtime.queue_component_updates(updates)) || !draw(runtime, frame++))
        return false;
    const auto pixels = runtime.read_viewport_pixels();
    if (!pixels || pixels.value.size() != 64 * 64 * 4)
        return false;
    uint64_t top_red = 0, top_green = 0, bottom_red = 0, bottom_green = 0;
    for (size_t y = 4; y < 60; ++y)
        for (size_t x = 20; x < 44; ++x) {
            const auto offset = (y * 64 + x) * 4;
            auto& r = y < 32 ? top_red : bottom_red;
            auto& g = y < 32 ? top_green : bottom_green;
            r += std::to_integer<unsigned>(pixels.value[offset]);
            g += std::to_integer<unsigned>(pixels.value[offset + 1]);
        }
    std::cerr << "Vertical orientation: top red/green=" << top_red << '/' << top_green << ", bottom=" << bottom_red
              << '/' << bottom_green << '\n';
    const std::array<application::ComponentUpdateView, 2> restore{
        {{red, type, std::as_bytes(std::span{&old_red, 1})}, {green, type, std::as_bytes(std::span{&old_green, 1})}}};
    return top_green > top_red * 2 && bottom_red > bottom_green * 2 &&
           check(runtime.queue_component_updates(restore)) && draw(runtime, frame++);
}
bool previously_uploaded_mesh_pixels(application::EditorSceneRuntime& runtime, uint64_t& frame,
                                     const std::filesystem::path& path) {
    using namespace engine::components;
    {
        std::ofstream mesh(path);
        mesh << "v -1 -1 0\nv 1 -1 0\nv 0 1 0\n"
                "vt 0 0\nvt 1 0\nvt 0.5 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\n";
        if (!mesh)
            return false;
    }
    if (!check(runtime.refresh_assets()))
        return false;
    // Upload while no entity references this mesh. Its first draw is in a
    // later frame, so the resident GPU buffers still belong to the copy queue.
    for (int i = 0; i < 10; ++i) {
        if (!check(runtime.render_frame(frame++, 1.0 / 60.0)))
            return false;
        static_cast<void>(aeg::concurrency::sleep_for(50'000'000ull));
    }
    uint64_t mesh_id = 0;
    const auto relative = path.lexically_relative(runtime.project_root()).generic_string();
    for (const auto& resource : runtime.resource_descriptors())
        if (resource.path == relative && resource.valid)
            mesh_id = resource.id;
    if (mesh_id == 0)
        return false;
    scene::SceneEntityId receiver{};
    for (const auto& item : runtime.entities())
        if (item.name == "Right Mesh")
            receiver = item.id;
    const ecs::ComponentTypeId type(kMeshRendererName);
    const auto* original = static_cast<const MeshRenderer*>(runtime.component_value(receiver, type));
    if (!original || !draw(runtime, frame++))
        return false;
    const MeshRenderer saved = *original;
    const auto before = runtime.read_viewport_pixels();
    auto imported = saved;
    imported.mesh_asset_id = mesh_id;
    if (!check(runtime.queue_component_update(receiver, type, std::as_bytes(std::span{&imported, 1}))) ||
        !draw(runtime, frame++))
        return false;
    const auto after = runtime.read_viewport_pixels();
    if (!before || !after || before.value == after.value)
        return false;
    return check(runtime.queue_component_update(receiver, type, std::as_bytes(std::span{&saved, 1}))) &&
           draw(runtime, frame++);
}
bool shadow_pixels(application::EditorSceneRuntime& runtime, uint64_t& frame) {
    using namespace engine::components;
    const auto find = [&](std::string_view name) {
        for (const auto& entity : runtime.entities())
            if (entity.name == name)
                return entity.id;
        return scene::SceneEntityId{};
    };
    const auto left = find("Left Mesh"), right = find("Right Mesh"), light_id = find("Directional Light");
    const ecs::ComponentTypeId transform_type(kTransformName), light_type(kDirectionalLightName);
    const auto* source = static_cast<const DirectionalLight*>(runtime.component_value(light_id, light_type));
    if (!source)
        return false;
    DirectionalLight light = *source;
    light.direction = {0.5f, 0, -1};
    light.casts_shadow = false;
    Transform caster{}, receiver{};
    caster.translation = {-0.45f, 0, 0.6f};
    caster.scale = {0.15f, 0.15f, 1};
    receiver.scale = {0.9f, 0.9f, 1};
    const std::array<application::ComponentUpdateView, 3> updates{
        {{left, transform_type, std::as_bytes(std::span{&caster, 1})},
         {right, transform_type, std::as_bytes(std::span{&receiver, 1})},
         {light_id, light_type, std::as_bytes(std::span{&light, 1})}}};
    if (!check(runtime.queue_component_updates(updates)) || !draw(runtime, frame++))
        return false;
    const auto unshadowed = runtime.read_viewport_pixels();
    light.casts_shadow = true;
    if (!check(runtime.queue_component_update(light_id, light_type, std::as_bytes(std::span{&light, 1}))) ||
        !draw(runtime, frame++))
        return false;
    const auto shadowed = runtime.read_viewport_pixels();
    if (!unshadowed || !shadowed || unshadowed.value.size() != 64 * 64 * 4 || shadowed.value.size() != 64 * 64 * 4)
        return false;
    const size_t sample = (32 * 64 + 29) * 4 + 1;
    const int before = std::to_integer<unsigned>(unshadowed.value[sample]);
    const int after = std::to_integer<unsigned>(shadowed.value[sample]);
    std::cerr << "Editor shadow receiver: " << before << " -> " << after << '\n';
    return before > after + 20 && check(runtime.save_scene());
}
bool transformed_lighting_pixels(application::EditorSceneRuntime& runtime, uint64_t& frame) {
    using namespace engine::components;
    const auto find = [&](std::string_view name) {
        for (const auto& entity : runtime.entities())
            if (entity.name == name)
                return entity.id;
        return scene::SceneEntityId{};
    };
    const auto receiver = find("Right Mesh"), sun = find("Directional Light"), camera_entity = find("Camera");
    const ecs::ComponentTypeId transform_type(kTransformName), light_type(kDirectionalLightName);
    const auto old_transform = *static_cast<const Transform*>(runtime.component_value(receiver, transform_type));
    const auto old_light = *static_cast<const DirectionalLight*>(runtime.component_value(sun, light_type));
    const auto old_camera = *static_cast<const Transform*>(runtime.component_value(camera_entity, transform_type));
    auto camera = old_camera;
    Transform transform{};
    transform.scale = {0.9f, 0.7f, 1.4f};
    DirectionalLight light{};
    std::array<unsigned, 4> values{};
    for (size_t step = 0; step < values.size(); ++step) {
        if (step == 1)
            transform.rotation = math::quat_from_axis_angle({0, 1, 0}, math::kPi / 3);
        if (step == 2)
            light.direction = math::rotate(transform.rotation, {0, 0, -1});
        if (step == 3) {
            const auto orbit = math::quat_from_axis_angle({0, 1, 0}, math::kPi / 4);
            camera.translation = math::rotate(orbit, old_camera.translation);
            camera.rotation = orbit * old_camera.rotation;
        }
        const std::array<application::ComponentUpdateView, 3> updates{
            {{receiver, transform_type, std::as_bytes(std::span{&transform, 1})},
             {sun, light_type, std::as_bytes(std::span{&light, 1})},
             {camera_entity, transform_type, std::as_bytes(std::span{&camera, 1})}}};
        if (!check(runtime.queue_component_updates(updates)) || !draw(runtime, frame++))
            return false;
        const auto pixels = runtime.read_viewport_pixels();
        if (!pixels)
            return false;
        values[step] = std::to_integer<unsigned>(pixels.value[(32 * 64 + 32) * 4 + 1]);
    }
    std::cerr << "Editor transformed lighting: " << values[0] << ',' << values[1] << ',' << values[2] << ','
              << values[3] << '\n';
    const std::array<application::ComponentUpdateView, 3> restore{
        {{receiver, transform_type, std::as_bytes(std::span{&old_transform, 1})},
         {sun, light_type, std::as_bytes(std::span{&old_light, 1})},
         {camera_entity, transform_type, std::as_bytes(std::span{&old_camera, 1})}}};
    return values[0] > values[1] + 30 && std::abs(int(values[0]) - int(values[2])) <= 3 &&
           std::abs(int(values[2]) - int(values[3])) <= 3 && check(runtime.queue_component_updates(restore)) &&
           draw(runtime, frame++);
}
bool environment_pixels(application::EditorSceneRuntime& runtime, uint64_t& frame) {
    using namespace engine::components;
    if (!check(runtime.create_scene_entity({}, "Environment Test")))
        return false;
    scene::SceneEntityId entity{}, camera_entity{};
    for (const auto& item : runtime.entities()) {
        if (item.name == "Environment Test")
            entity = item.id;
        if (item.name == "Camera")
            camera_entity = item.id;
    }
    const ecs::ComponentTypeId type(kSceneEnvironmentName), transform_type(kTransformName);
    if (!check(runtime.add_component(std::span{&entity, 1}, type)))
        return false;
    SceneEnvironment environment{};
    environment.sky_zenith = environment.sky_horizon = environment.sky_ground = {0.25f, 0.5f, 1};
    environment.gamma = 1;
    const auto render = [&]() {
        return check(runtime.queue_component_update(entity, type, std::as_bytes(std::span{&environment, 1}))) &&
               check(runtime.request_viewport_readback()) && check(runtime.render_frame(frame++, 1.0 / 60.0));
    };
    std::array<unsigned, 3> values{};
    for (size_t step = 0; step < values.size(); ++step) {
        environment.exposure = step == 1 ? 0.5f : 1;
        environment.gamma = step == 2 ? 2 : 1;
        if (!render())
            return false;
        const auto pixels = runtime.read_viewport_pixels();
        if (!pixels)
            return false;
        values[step] = std::to_integer<unsigned>(pixels.value[0]);
        // Sky must not replace the center receiver.
        if (pixels.value[(32 * 64 + 32) * 4] == pixels.value[0])
            return false;
    }
    std::cerr << "Editor sky/postprocess: " << values[0] << ',' << values[1] << ',' << values[2] << '\n';
    if (std::abs(int(values[0]) - 51) > 2 || std::abs(int(values[1]) - 28) > 2 || std::abs(int(values[2]) - 114) > 2)
        return false;
    const auto path = runtime.scene_path();
    if (!check(runtime.save_scene()) || !check(runtime.open_scene(path)))
        return false;
    const auto* restored = static_cast<const SceneEnvironment*>(runtime.component_value(entity, type));
    if (!restored || restored->gamma != 2 || restored->sky_horizon[1] != 0.5f)
        return false;
    environment.gamma = 0;
    if (runtime.queue_component_update(entity, type, std::as_bytes(std::span{&environment, 1})))
        return false;
    environment.gamma = 1;
    environment.tone_mapping = false;
    environment.sky_zenith = {1, 0, 0};
    environment.sky_horizon = {0, 1, 0};
    environment.sky_ground = {0, 0, 1};
    const auto old_camera = *static_cast<const Transform*>(runtime.component_value(camera_entity, transform_type));
    for (int direction : {1, -1}) {
        auto camera = old_camera;
        camera.rotation = math::quat_from_axis_angle({1, 0, 0}, direction * math::kPi / 2);
        if (!check(
                runtime.queue_component_update(camera_entity, transform_type, std::as_bytes(std::span{&camera, 1}))) ||
            !render())
            return false;
        const auto pixels = runtime.read_viewport_pixels();
        if (!pixels || std::to_integer<unsigned>(pixels.value[(32 * 64 + 32) * 4 + (direction == 1 ? 0 : 2)]) < 248)
            return false;
    }
    environment.enabled = false;
    if (!check(
            runtime.queue_component_update(camera_entity, transform_type, std::as_bytes(std::span{&old_camera, 1}))) ||
        !render())
        return false;
    const auto disabled = runtime.read_viewport_pixels();
    if (!disabled || disabled.value[0] != std::byte{0})
        return false;
    return check(runtime.remove_entities(std::span{&entity, 1})) && draw(runtime, frame++) &&
           check(runtime.save_scene());
}
bool local_light_pixels(application::EditorSceneRuntime& runtime, uint64_t& frame) {
    using namespace engine::components;
    const auto find = [&](std::string_view name) {
        for (const auto& entity : runtime.entities())
            if (entity.name == name)
                return entity.id;
        return scene::SceneEntityId{};
    };
    const ecs::ComponentTypeId transform_type(kTransformName), sun_type(kDirectionalLightName),
        point_type(kPointLightName), spot_type(kSpotLightName);
    const auto sun = find("Directional Light");
    const auto old_sun = *static_cast<const DirectionalLight*>(runtime.component_value(sun, sun_type));
    auto sunlight = old_sun;
    sunlight.intensity = 0;
    if (!check(runtime.create_scene_entity({}, "Point Test")))
        return false;
    const auto point_entity = find("Point Test");
    if (!check(runtime.add_component(std::span{&point_entity, 1}, point_type)) ||
        !check(runtime.create_scene_entity({}, "Spot Test")))
        return false;
    const auto spot_entity = find("Spot Test");
    if (!check(runtime.add_component(std::span{&spot_entity, 1}, spot_type)))
        return false;
    PointLight point{};
    point.range = 1.5f;
    point.intensity = 0;
    SpotLight spot{};
    spot.range = 1.5f;
    spot.intensity = 0;
    spot.inner_cone_cosine = 0.99f;
    spot.outer_cone_cosine = 0.90f;
    Transform point_transform{}, spot_transform{};
    point_transform.translation = {-0.1f, 0, 0.6f};
    spot_transform.translation = {0.1f, 0, 0.7f};
    std::array<unsigned, 5> values{};
    for (size_t step = 0; step < values.size(); ++step) {
        if (step == 1)
            point.intensity = 1;
        if (step == 2) {
            point_transform.translation.x = 100;
            spot.intensity = 1;
        }
        if (step == 3)
            spot_transform.rotation = math::quat_from_axis_angle({0, 1, 0}, math::kPi);
        if (step == 4) {
            spot_transform.rotation = {};
            spot.visibility_mask = 0;
        }
        const std::array<application::ComponentUpdateView, 5> updates{
            {{sun, sun_type, std::as_bytes(std::span{&sunlight, 1})},
             {point_entity, transform_type, std::as_bytes(std::span{&point_transform, 1})},
             {spot_entity, transform_type, std::as_bytes(std::span{&spot_transform, 1})},
             {point_entity, point_type, std::as_bytes(std::span{&point, 1})},
             {spot_entity, spot_type, std::as_bytes(std::span{&spot, 1})}}};
        if (!check(runtime.queue_component_updates(updates)) || !check(runtime.request_viewport_readback()) ||
            !check(runtime.render_frame(frame++, 1.0 / 60.0)))
            return false;
        const auto pixels = runtime.read_viewport_pixels();
        if (!pixels || pixels.value.size() != 64 * 64 * 4)
            return false;
        values[step] = std::to_integer<unsigned>(pixels.value[(32 * 64 + 32) * 4 + 1]);
    }
    std::cerr << "Editor local lights: ";
    for (auto value : values)
        std::cerr << value << ',';
    std::cerr << '\n';
    if (values[1] <= values[0] + 20 || values[2] <= values[0] + 20 || values[3] != values[0] || values[4] != values[0])
        return false;
    // Scene serialization keeps both schemas and their non-default parameters.
    const auto path = runtime.scene_path();
    if (!check(runtime.save_scene()) || !check(runtime.open_scene(path)))
        return false;
    const auto* saved_point = static_cast<const PointLight*>(runtime.component_value(point_entity, point_type));
    const auto* saved_spot = static_cast<const SpotLight*>(runtime.component_value(spot_entity, spot_type));
    if (!saved_point || !saved_spot || saved_point->range != 1.5f || saved_spot->outer_cone_cosine != 0.90f ||
        saved_spot->visibility_mask != 0)
        return false;
    point.range = -1;
    if (runtime.queue_component_update(point_entity, point_type, std::as_bytes(std::span{&point, 1})))
        return false;
    spot.outer_cone_cosine = 1.1f;
    if (runtime.queue_component_update(spot_entity, spot_type, std::as_bytes(std::span{&spot, 1})))
        return false;
    const std::array<scene::SceneEntityId, 2> remove{point_entity, spot_entity};
    return check(runtime.remove_entities(remove)) &&
           check(runtime.queue_component_update(sun, sun_type, std::as_bytes(std::span{&old_sun, 1}))) &&
           draw(runtime, frame++) && check(runtime.save_scene());
}
}  // namespace

int main(int argc, char** argv) try {
    using namespace aeg;
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    const bool recovery_fixture = argc == 3 && std::string_view(argv[1]) == "--startup-recovery-fixture";
    if (argc != 1 && !recovery_fixture) return 25;
    const auto fixture = recovery_fixture ? std::filesystem::absolute(argv[2]) :
        std::filesystem::temp_directory_path() / ("aeg_renderer_feature_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(fixture);
    // Closing a project saves the detached editor_scene.aegscene relative to
    // the process working directory. Keep that source and its atomic-save
    // files inside this invocation's fixture too.
    const auto previous_working_directory = std::filesystem::current_path();
    std::filesystem::current_path(fixture);
#if defined(_WIN32)
    _putenv_s("APPDATA", fixture.string().c_str());
#endif
    const auto root = fixture / "Project";
    auto device = rhi::create_device({rhi::BackendType::kVulkan, "Editor Renderer Feature smoke"});
    if (!device) return 1;
    application::EditorSceneRuntime runtime;
    if (!check(runtime.initialize(*device.value)) || !check(runtime.save_scene_as(fixture / "source.aegscene")) ||
        !check(runtime.new_project(root, "Renderer Feature publication"))) return 2;
    const auto project_settings = application::project::ProjectFileCodec{}.load(root / "Project.aegproj");
    if (!project_settings) return 2;
    const auto mesh_scene = root / project_settings.value.config.startup_scene.parent_path() / "Mesh.aegscene";
    std::filesystem::copy_file(fixture / "source.aegscene", mesh_scene);
    if (!check(runtime.open_scene(mesh_scene)) || !check(runtime.request_viewport_extent(64, 64))) return 3;
    if (recovery_fixture) {
        // Main must explicitly reject this retired sidecar before mutating a Scene, while its default Preview remains usable.
        std::ofstream(runtime.scene_path().string() + ".pipeline") << "AEG_PIPELINE_EDITS 3\n";
        return check(runtime.shutdown()) ? 0 : 25;
    }
    const auto settings_bytes = read_file(root / "Project.aegproj");
    auto forward = renderer::scene_render_profile_graph(renderer::SceneRenderProfile::kForward);
    auto deferred = renderer::scene_render_profile_graph(renderer::SceneRenderProfile::kDeferred);
    if (!forward || !deferred) return 4;
    uint64_t frame = 1;
    for (const auto profile : {renderer::SceneRenderProfile::kForward, renderer::SceneRenderProfile::kDeferred,
                               renderer::SceneRenderProfile::kForward}) {
        const auto graph = renderer::scene_render_profile_graph(profile);
        const auto before = runtime.inspect_renderer_feature_execution();
        if (!graph || !before || !check(runtime.publish_renderer_feature_graph(graph.value))) return 5;
        const auto after = runtime.inspect_renderer_feature_execution();
        if (!after || after.value.generation == before.value.generation || !draw(runtime, frame++) ||
            !actual_feature_work(runtime, profile)) return 6;
        if (!check(runtime.request_viewport_extent(96, 48)) || !draw(runtime, frame++) ||
            !check(runtime.request_viewport_extent(64, 64)) || !draw(runtime, frame++)) return 6;
        if (!vertical_orientation_pixels(runtime, frame)) return 23;
        if (!shadow_pixels(runtime, frame)) return 18;
        if (!transformed_lighting_pixels(runtime, frame)) return 19;
        if (!environment_pixels(runtime, frame)) return 21;
    }
    if (!local_light_pixels(runtime, frame)) return 20;
    const auto valid_source = runtime.inspect_renderer_feature_execution();
    const auto valid_pixels = runtime.read_viewport_pixels();
    if (!valid_source || !valid_pixels) return 7;
    for (int failure = 0; failure < 6; ++failure) {
        auto invalid = deferred.value;
        if (failure == 0) invalid.nodes.front().definition_id = feature::FeatureDefinitionId("Unknown.Scene");
        if (failure == 1) ++invalid.nodes.front().definition_version.major;
        if (failure == 2) ++invalid.contract_digest;
        if (failure == 3) invalid.edges.push_back({invalid.nodes.front().node_id, feature::PortId("Missing"),
            invalid.nodes.front().node_id, feature::PortId("Missing"), feature::EdgeKind::kData});
        if (failure == 4) { auto duplicate = invalid.nodes.front(); duplicate.node_id = feature::FeatureNodeId("Second.Root"); invalid.nodes.push_back(duplicate); }
        if (failure == 5) invalid.process_subgraphs.push_back({invalid.nodes.front().node_id});
        const auto rejected = runtime.publish_renderer_feature_graph(invalid);
        const auto after = runtime.inspect_renderer_feature_execution();
        if (rejected || !after || after.value.generation != valid_source.value.generation ||
            !draw(runtime, frame++) || !actual_feature_work(runtime, renderer::SceneRenderProfile::kForward) ||
            runtime.read_viewport_pixels().value != valid_pixels.value) return 8;
    }
    if (!retired_authoring_is_rejected(runtime, fixture, frame)) return 24;
    const auto before_boundary = runtime.inspect_renderer_feature_execution();
    if (!check(runtime.play()) || runtime.publish_renderer_feature_graph(deferred.value) ||
        runtime.restore_default_renderer_feature() || !draw(runtime, frame++) || !check(runtime.pause()) ||
        runtime.publish_renderer_feature_graph(deferred.value) || runtime.restore_default_renderer_feature() ||
        !check(runtime.stop()) || !draw(runtime, frame++)) return 15;
    const auto after_boundary = runtime.inspect_renderer_feature_execution();
    if (!before_boundary || !after_boundary || after_boundary.value.generation != before_boundary.value.generation ||
        !actual_feature_work(runtime, renderer::SceneRenderProfile::kForward)) return 15;
    if (!check(runtime.restore_default_renderer_feature()) || !default_feature_preview(runtime, frame++) ||
        read_file(root / "Project.aegproj") != settings_bytes) return 11;
    if (!check(runtime.publish_renderer_feature_graph(deferred.value)) || !draw(runtime, frame++) ||
        !check(runtime.new_project(fixture / "OtherProject", "Other renderer project")) ||
        !active(runtime, renderer::SceneRenderProfile::kOpaque)) return 17;
    if (!check(runtime.import_project(root)) || !default_feature_preview(runtime, frame++)) return 10;
    for (const auto profile : {renderer::SceneRenderProfile::kForward, renderer::SceneRenderProfile::kDeferred}) {
        const auto graph = renderer::scene_render_profile_graph(profile);
        if (!graph || !check(runtime.publish_renderer_feature_graph(graph.value)) ||
            !previously_uploaded_mesh_pixels(runtime, frame, root / "Assets" /
                (profile == renderer::SceneRenderProfile::kForward ? "Forward.obj" : "Deferred.obj")) ||
            !actual_feature_work(runtime, profile)) return 22;
    }
    const auto last_pixels = runtime.read_viewport_pixels();
    auto empty = deferred.value; ++empty.graph_version; empty.nodes.clear();
    const auto last_source = runtime.inspect_renderer_feature_execution();
    if (!last_pixels || !last_source || !check(runtime.publish_renderer_feature_graph(empty))) return 26;
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        if (!check(runtime.request_viewport_readback()) || !check(runtime.render_frame(frame++, 0))) return 26;
        const auto pixels = runtime.read_viewport_pixels();
        const auto captured = runtime.inspect_render_graph();
        const auto source = runtime.inspect_renderer_feature_execution();
        if (!pixels || pixels.value != last_pixels.value || runtime.draw_call_count() != 0 || !source ||
            source.value.generation == last_source.value.generation || !source.value.plans.empty() || !captured ||
            std::ranges::any_of(captured.value->plan.pass_plans, [](const auto& pass) { return pass.feature_work.has_value(); })) return 26;
    }
    if (!check(runtime.restore_default_renderer_feature()) || !default_feature_preview(runtime, frame++) ||
        !check(runtime.close_project()) || !active(runtime, renderer::SceneRenderProfile::kOpaque) ||
        !runtime.entities().empty() || !check(runtime.request_viewport_readback()) ||
        !check(runtime.render_frame(frame++, 0)) || !actual_feature_work(runtime, renderer::SceneRenderProfile::kOpaque)) return 16;
    const auto closed_pixels = runtime.read_viewport_pixels();
    if (!closed_pixels || closed_pixels.value.empty() || runtime.draw_call_count() != 0) return 16;
    for (size_t offset = 0; offset < closed_pixels.value.size(); offset += 4)
        for (size_t channel = 0; channel < 3; ++channel)
            if (std::to_integer<unsigned>(closed_pixels.value[offset + channel]) > 9) return 16;
    if (read_file(root / "Project.aegproj") != settings_bytes || !check(runtime.shutdown())) return 12;
    std::filesystem::current_path(previous_working_directory);
    std::error_code error;
    std::filesystem::remove_all(fixture, error);
    std::cout << "Editor Renderer Feature publication and pixels passed\n";
} catch (const std::exception& error) {
    std::cerr << "Renderer Feature test failed: " << error.what() << '\n';
    return 20;
}
