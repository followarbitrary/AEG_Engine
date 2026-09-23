/** @brief Verifies light-space fitting and camera-to-shadow reconstruction. */
#include <cmath>
#include <iostream>

#include "shadow_rendering.hpp"

using namespace aeg;

int main() {
    renderer::RenderSceneSnapshot scene{};
    scene.world = {0, 1};
    scene.world_revision = 1;
    scene.view.world = scene.world;
    scene.view.id = renderer::RenderViewId("ShadowView.Camera");
    scene.view.output_extent = {128, 64};
    scene.view.camera.view = math::translation({-3, 2, -4});
    scene.instances.push_back({.object = renderer::RenderObjectId(1),
                               .mesh = renderer::MeshAssetId(1),
                               .material = renderer::MaterialInstanceId(1),
                               .world_transform = math::translation({10, 2, -3}) * math::scale({2, 3, 4}),
                               .bounds_radius = 1});
    scene.lights.push_back({.light = renderer::RenderLightId(1),
                            .type = renderer::RenderLightType::kDirectional,
                            .direction = {0, -1, 0},
                            .casts_shadow = false});
    const auto disabled = renderer::make_directional_shadow_view(scene);
    if (!disabled || disabled.value.enabled)
        return 1;
    scene.lights.front().casts_shadow = true;
    const auto shadow = renderer::make_directional_shadow_view(scene);
    if (!shadow || !shadow.value.enabled || !shadow.value.view.is_valid() || shadow.value.view.id == scene.view.id ||
        shadow.value.view.output_extent != scene.view.output_extent)
        return 2;
    // All six extrema of this non-uniformly scaled bound must remain inside the map.
    for (const auto point : {math::Vec3{8, 2, -3}, {12, 2, -3}, {10, -1, -3}, {10, 5, -3}, {10, 2, -7}, {10, 2, 1}}) {
        math::Vec3 projected{};
        if (!math::try_transform_point(shadow.value.view.camera.view_projection(), point, projected) ||
            std::abs(projected.x) > 1 || std::abs(projected.y) > 1 || projected.z <= 0 || projected.z >= 1) {
            std::cerr << "Shadow bound outside map: " << projected.x << ',' << projected.y << ',' << projected.z
                      << '\n';
            return 3;
        }
        const auto camera_clip = scene.view.camera.view_projection() * math::Vec4{point.x, point.y, point.z, 1};
        const auto light_clip = shadow.value.constants.clip_to_shadow * camera_clip;
        if (std::abs(light_clip.x / light_clip.w - projected.x) > 0.0001f ||
            std::abs(light_clip.y / light_clip.w - projected.y) > 0.0001f ||
            std::abs(light_clip.z / light_clip.w - projected.z) > 0.0001f)
            return 4;
    }
    scene.view.camera.projection = {};
    if (renderer::make_directional_shadow_view(scene))
        return 5;
    scene.view.camera.projection = math::identity();
    scene.lights.front().direction = {};
    if (renderer::make_directional_shadow_view(scene))
        return 6;
    scene.lights.front().direction = {0, 0, -1};
    scene.lights.front().intensity = 0;
    const auto dark = renderer::make_directional_shadow_view(scene);
    if (!dark || dark.value.enabled)
        return 7;
    std::cout << "Directional shadow fitting and reconstruction passed\n";
}
