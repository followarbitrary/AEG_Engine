/** @brief Verifies project-authored renderer selection changes the live viewport. */
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "editor_scene_runtime.hpp"
#include "pipeline_document_io.hpp"
#include "project_context.hpp"
#include "renderer_variant_document_fixture.hpp"

namespace {
using namespace aeg;
bool check(const foundation::Result& result) {
    if (!result)
        std::cerr << result.message << '\n';
    return bool(result);
}
bool draw(application::EditorSceneRuntime& runtime, uint64_t frame) {
    std::cerr << "Rendering frame " << frame << '\n';
    if (!runtime.viewport_present_connected() || !check(runtime.request_viewport_readback()) ||
        !check(runtime.render_frame(frame)))
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
bool active(application::EditorSceneRuntime& runtime, pipeline::PipelineId pipeline) {
    return std::ranges::any_of(runtime.pipeline_snapshots(), [&](const auto& item) {
        return item.first == "Game / Renderer" && item.second && item.second->pipeline == pipeline;
    });
}
bool invalid_startup_renderer_is_recoverable(application::EditorSceneRuntime& runtime, rhi::Device& device,
                                             const std::filesystem::path& fixture, const pipeline::TypeCatalog& catalog,
                                             uint64_t& frame) {
    const auto root = fixture / "RecoveryProject";
    if (!check(runtime.new_project(root, "Pipeline recovery")))
        return false;
    const auto scene_path = runtime.scene_path();
    std::filesystem::copy_file(fixture / "source.aegscene", scene_path,
                               std::filesystem::copy_options::overwrite_existing);
    if (!check(runtime.open_scene(scene_path)))
        return false;
    const auto forward = root / "Assets" / "Forward.aegpipe";
    const auto deferred = root / "Assets" / "Deferred.aegpipe";
    const auto read_file = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    };
    if (!check(application::project::save_pipeline_document(forward, test::renderer_document(catalog, true))) ||
        !check(application::project::save_pipeline_document(deferred, test::renderer_document(catalog, false))) ||
        !check(runtime.refresh_assets()) || !check(runtime.activate_renderer_pipeline_document(forward)))
        return false;
    for (int failure = 0; failure < 4; ++failure) {
        std::cerr << "Recovering startup renderer failure " << failure << '\n';
        if (failure == 0) {
            if (!check(runtime.set_authored_present_connection(forward, "SkyboxPostProcess", false)))
                return false;
        } else if (failure == 1) {
            auto invalid = test::renderer_document(catalog, true);
            invalid.pass_nodes.front().enabled = false;
            if (!check(application::project::save_pipeline_document(forward, invalid)))
                return false;
        } else if (failure == 2) {
            std::ofstream(forward, std::ios::trunc) << "pipeline_nodes = [invalid TOML";
        } else {
            std::filesystem::remove(forward);
        }
        const auto project_bytes = read_file(root / "Project.aegproj");
        const auto pipeline_bytes = read_file(forward);
        if (failure == 0 && (!check(runtime.shutdown()) || !check(runtime.initialize(device))))
            return false;
        // A bad project graph must not fail project opening or take down the editor viewport.
        if (!check(runtime.import_project(root)) || !runtime.project_loaded() ||
            !runtime.active_renderer_pipeline_document().empty() ||
            runtime.diagnostic().find("Forward.aegpipe") == std::string_view::npos || !draw(runtime, frame++)) {
            std::cerr << "Invalid startup pipeline blocked the editor or lacked a diagnostic\n";
            return false;
        }
        if (failure != 3 && !std::ranges::any_of(runtime.authored_pipeline_documents(),
                                                 [&](const auto& item) { return item.path == forward; }))
            return false;
        if (read_file(root / "Project.aegproj") != project_bytes || read_file(forward) != pipeline_bytes ||
            runtime.diagnostic().find("Forward.aegpipe") == std::string_view::npos)
            return false;
        const auto entity_count = runtime.entities().size();
        if (!check(runtime.create_scene_entity({}, "Still editable")) ||
            runtime.entities().size() != entity_count + 1 || !check(runtime.undo()) ||
            runtime.entities().size() != entity_count)
            return false;
        // Repair and explicitly apply the same document without restarting the editor.
        if (!check(application::project::save_pipeline_document(forward, test::renderer_document(catalog, true))) ||
            !check(runtime.refresh_assets()) || !check(runtime.activate_renderer_pipeline_document(forward)) ||
            runtime.diagnostic().find("Forward.aegpipe") != std::string_view::npos ||
            !active(runtime, renderer::kForwardRenderPipelineId) || !draw(runtime, frame++))
            return false;
    }
    return true;
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
        if (!check(runtime.render_frame(frame++)))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
               check(runtime.request_viewport_readback()) && check(runtime.render_frame(frame++));
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
    if (!restored || restored->gamma != 2 || restored->sky_horizon.y != 0.5f)
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
            !check(runtime.render_frame(frame++)))
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
    if (argc != 1 && !recovery_fixture)
        return 25;
    const auto fixture = recovery_fixture
                             ? std::filesystem::absolute(argv[2])
                             : std::filesystem::temp_directory_path() /
                                   ("aeg_renderer_selection_" +
                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(fixture);
#if defined(_WIN32)
    // Keep the editor's recent-project list inside this test's temporary fixture.
    _putenv_s("APPDATA", fixture.string().c_str());
#endif
    const auto root = fixture / "Project";
    auto device = rhi::create_device({rhi::BackendType::kVulkan, "Editor renderer pipeline smoke"});
    if (!device)
        return 1;
    application::EditorSceneRuntime runtime;
    std::cerr << "Initializing scene and project\n";
    if (!check(runtime.initialize(*device.value)))
        return 2;
    std::cerr << "Saving source scene\n";
    if (!check(runtime.save_scene_as(fixture / "source.aegscene")))
        return 2;
    std::cerr << "Creating project\n";
    if (!check(runtime.new_project(root, "Renderer selection")))
        return 2;
    std::cerr << "Opening mesh scene\n";
    const auto project_settings = application::project::ProjectFileCodec{}.load(root / "Project.aegproj");
    if (!project_settings)
        return 2;
    const auto mesh_scene = root / project_settings.value.config.startup_scene.parent_path() / "Mesh.aegscene";
    std::filesystem::copy_file(fixture / "source.aegscene", mesh_scene);
    if (!check(runtime.open_scene(mesh_scene)) || !check(runtime.request_viewport_extent(64, 64)))
        return 3;
    pipeline::TypeCatalog catalog;
    if (!check(renderer::register_renderer_authoring_types(catalog)))
        return 4;
    const auto forward = root / "Assets" / "Forward.aegpipe";
    const auto deferred = root / "Assets" / "Deferred.aegpipe";
    std::cerr << "Importing renderer documents\n";
    if (!check(application::project::save_pipeline_document(forward, test::renderer_document(catalog, true))) ||
        !check(application::project::save_pipeline_document(deferred, test::renderer_document(catalog, false))) ||
        !check(runtime.import_project(root)))
        return 5;
    uint64_t frame = 1;
    if (recovery_fixture) {
        if (!check(runtime.activate_renderer_pipeline_document(forward)) ||
            !check(runtime.set_authored_present_connection(forward, "SkyboxPostProcess", false)))
            return 25;
        // Keep the broken document selected so the application test also draws its load-error UI.
        std::filesystem::remove(deferred);
        std::ofstream(runtime.scene_path().string() + ".pipeline") << "AEG_PIPELINE_EDITS 3\n";
        return check(runtime.shutdown()) ? 0 : 25;
    }
    for (const auto& path : {forward, deferred, forward}) {
        std::cerr << "Activating " << path.filename() << '\n';
        if (!check(runtime.activate_renderer_pipeline_document(path)) ||
            !active(runtime,
                    path == deferred ? renderer::kDeferredRenderPipelineId : renderer::kForwardRenderPipelineId) ||
            runtime.active_renderer_pipeline_document() != path || !draw(runtime, frame++))
            return 6;
        if (!check(runtime.request_viewport_extent(96, 48)) || !draw(runtime, frame++) ||
            !check(runtime.request_viewport_extent(64, 64)) || !draw(runtime, frame++))
            return 6;
        if (!vertical_orientation_pixels(runtime, frame))
            return 23;
        if (!shadow_pixels(runtime, frame))
            return 18;
        if (!transformed_lighting_pixels(runtime, frame))
            return 19;
        if (!environment_pixels(runtime, frame))
            return 21;
    }
    if (!local_light_pixels(runtime, frame))
        return 20;
    auto invalid = test::renderer_document(catalog, false);
    invalid.pass_nodes.front().enabled = false;
    const auto invalid_path = root / "Assets" / "Invalid.aegpipe";
    if (!check(application::project::save_pipeline_document(invalid_path, invalid)))
        return 7;
    const auto rejected = runtime.activate_renderer_pipeline_document(invalid_path);
    if (rejected || !active(runtime, renderer::kForwardRenderPipelineId) ||
        runtime.active_renderer_pipeline_document() != forward || !draw(runtime, frame++))
        return 8;
    const auto settings = application::project::ProjectFileCodec{}.load(root / "Project.aegproj");
    auto expected_startup = project_settings.value.config.startup_pipelines;
    expected_startup.emplace_back("Assets/Forward.aegpipe");
    if (!settings || settings.value.config.startup_pipelines != expected_startup)
        return 9;
    const auto outside = fixture / "Outside.aegpipe";
    if (!check(application::project::save_pipeline_document(outside, test::renderer_document(catalog, false))))
        return 13;
    if (runtime.activate_renderer_pipeline_document(outside) || runtime.active_renderer_pipeline_document() != forward)
        return 13;
    // Force the project's atomic save to fail after the candidate host has initialized.
    const auto blocked_save = root / "Project.aegproj.tmp";
    std::filesystem::create_directory(blocked_save);
    const auto save_failed = runtime.activate_renderer_pipeline_document(deferred);
    std::filesystem::remove(blocked_save);
    if (save_failed || runtime.active_renderer_pipeline_document() != forward ||
        !active(runtime, renderer::kForwardRenderPipelineId) || !draw(runtime, frame++))
        return 14;
    const auto after_failure = application::project::ProjectFileCodec{}.load(root / "Project.aegproj");
    if (!after_failure || after_failure.value.config.startup_pipelines != expected_startup)
        return 14;
    if (!check(runtime.play()) || runtime.activate_renderer_pipeline_document(deferred) || !draw(runtime, frame++) ||
        !check(runtime.pause()) || runtime.restore_default_renderer_pipeline() || !check(runtime.stop()) ||
        !draw(runtime, frame++))
        return 15;
    std::cerr << "Restoring project selection\n";
    if (!check(runtime.import_project(root)) || !active(runtime, renderer::kForwardRenderPipelineId) ||
        !draw(runtime, frame++))
        return 10;
    std::cerr << "Restoring default renderer\n";
    const auto missing_active = root / "Assets" / "Forward.aegpipe.missing";
    std::filesystem::rename(forward, missing_active);
    const auto default_result = runtime.restore_default_renderer_pipeline();
    std::filesystem::rename(missing_active, forward);
    if (!check(default_result) || !active(runtime, pipeline::PipelineId("RenderPipeline")) || !draw(runtime, frame++))
        return 11;
    const auto restored = application::project::ProjectFileCodec{}.load(root / "Project.aegproj");
    if (!restored || restored.value.config.startup_pipelines != project_settings.value.config.startup_pipelines ||
        !runtime.active_renderer_pipeline_document().empty())
        return 16;
    if (!check(runtime.activate_renderer_pipeline_document(deferred)) || !draw(runtime, frame++) ||
        !check(runtime.new_project(fixture / "OtherProject", "Other renderer project")) ||
        !runtime.active_renderer_pipeline_document().empty() || active(runtime, renderer::kDeferredRenderPipelineId))
        return 17;
    if (!check(runtime.import_project(root)) || !check(runtime.activate_renderer_pipeline_document(deferred)) ||
        !draw(runtime, frame++))
        return 18;
    for (const auto& path : {forward, deferred}) {
        if (!check(runtime.activate_renderer_pipeline_document(path)) ||
            !previously_uploaded_mesh_pixels(runtime, frame, root / "Assets" / (path.stem().string() + ".obj")))
            return 22;
    }
    if (!invalid_startup_renderer_is_recoverable(runtime, *device.value, fixture, catalog, frame))
        return 24;
    std::cerr << "Shutting down renderer\n";
    if (!check(runtime.shutdown()))
        return 12;
    std::error_code error;
    std::filesystem::remove_all(fixture, error);
    std::cout << "Editor renderer selection and pixels passed\n";
} catch (const std::exception& error) {
    std::cerr << "Renderer selection test failed: " << error.what() << '\n';
    return 20;
}
