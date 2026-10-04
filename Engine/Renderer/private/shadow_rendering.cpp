/** @file shadow_rendering.cpp @brief Fits and draws directional shadow casters. */
#include "shadow_rendering.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "builtin_render_passes.hpp"
#include "opaque_render_feature.hpp"

namespace aeg::renderer {
foundation::ResultValue<RenderShadowView> make_directional_shadow_view(const RenderSceneSnapshot& snapshot) {
    using namespace foundation;
    if (!snapshot.is_valid())
        return {invalid_argument("shadow requires a valid scene and view"), {}};
    const auto lighting = make_render_lighting_constants(snapshot.lights);
    if (!lighting)
        return {lighting.result, {}};
    const RenderLight* light = nullptr;
    for (const auto& candidate : snapshot.lights) {
        if (candidate.type == RenderLightType::kDirectional && candidate.casts_shadow && candidate.intensity > 0 &&
            (candidate.visibility_mask & snapshot.view.visibility_mask) != 0)
            light = &candidate;
    }
    const auto& casters = snapshot.shadow_casters.empty() ? snapshot.instances : snapshot.shadow_casters;
    if (!light || casters.empty())
        return {Result::success(), {}};
    math::Vec3 direction{};
    math::Mat4 inverse_camera{};
    if (!math::try_normalize(light->direction, direction) ||
        !math::try_inverse(snapshot.view.camera.view_projection(), inverse_camera))
        return {invalid_argument("shadow requires an invertible camera and a nonzero light direction"), {}};
    constexpr float kLimit = std::numeric_limits<float>::max();
    math::Vec3 lower{kLimit, kLimit, kLimit}, upper{-kLimit, -kLimit, -kLimit};
    for (const auto& instance : casters) {
        math::Vec3 center{};
        if (!math::is_finite(instance.bounds_radius) || instance.bounds_radius < 0 ||
            !math::try_transform_point(instance.world_transform, instance.bounds_center, center))
            return {invalid_argument("shadow caster has invalid world bounds"), {}};
        // Row lengths bound a transformed sphere, including nonuniform scale and shear.
        for (size_t axis = 0; axis != 3; ++axis) {
            const auto& transform = instance.world_transform;
            const float radius =
                instance.bounds_radius *
                math::length(math::Vec3{transform.at(axis, 0), transform.at(axis, 1), transform.at(axis, 2)});
            if (!math::is_finite(radius))
                return {invalid_argument("shadow caster bound overflow"), {}};
            lower[axis] = std::min(lower[axis], center[axis] - radius);
            upper[axis] = std::max(upper[axis], center[axis] + radius);
        }
    }
    const math::Vec3 center = (lower + upper) * 0.5f;
    const float radius = std::max(0.01f, math::length((upper - lower) * 0.5f)) * 1.05f;
    RenderShadowView shadow{};
    shadow.view = snapshot.view;
    const uint64_t shadow_id = snapshot.view.id.value() ^ 0x534841444f575649ull;
    shadow.view.id = RenderViewId(shadow_id == 0 ? 1 : shadow_id);
    shadow.view.frustum_culling = false;
    shadow.view.camera.position = center - direction * (radius + 0.1f);
    shadow.view.camera.near_plane = 0.1f;
    shadow.view.camera.far_plane = 2 * radius + 0.1f;
    shadow.view.camera.clip_space_convention = {};
    const math::Vec3 up = std::abs(direction.y) > 0.95f ? math::Vec3{1, 0, 0} : math::Vec3{0, 1, 0};
    if (!math::try_look_at(shadow.view.camera.position, center, up, shadow.view.camera.view) ||
        !math::try_orthographic(-radius, radius, -radius, radius, 0.1f, 2 * radius + 0.1f,
                                shadow.view.camera.projection))
        return {invalid_argument("directional shadow projection is invalid"), {}};
    shadow.constants.clip_to_shadow = shadow.view.camera.view_projection() * inverse_camera;
    shadow.constants.parameters = {1.0f / snapshot.view.output_extent.width, 1.0f / snapshot.view.output_extent.height,
                                   0.001f, 0};
    shadow.enabled = true;
    return {Result::success(), shadow};
}

foundation::Result build_shadow_render_graph(pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context,
                                             const RenderShadowView& shadow, pipeline::RenderGraphTexture target,
                                             pipeline::RenderPassInstanceId authored_instance) {
    if (!context.is_valid() || render_feature_context(builder) != &context || !target.is_valid())
        return foundation::invalid_argument("shadow graph context or target is invalid");
    if (shadow.enabled) {
        const auto& snapshot = *context.scene_snapshot;
        const auto& casters = snapshot.shadow_casters.empty() ? snapshot.instances : snapshot.shadow_casters;
        return build_depth_render_graph(builder, context, shadow.view, target, "Renderer.ShadowPass", casters,
                                        authored_instance);
    }
    const auto pass = detail::add_builtin_render_pass(
        builder, context, kShadowPassTypeId,
        [target](pipeline::RenderPassBuilder& pass) {
            return pass.use_depth_stencil_attachment(target, rhi::LoadOp::kClear, rhi::StoreOp::kStore, 1, 0,
                                                     rhi::ResourceUsage::kSampledRead);
        },
         [](pipeline::RenderPassExecutionContext& execution) {
            if (const auto result = execution.begin_rendering(); !result)
                return result;
            return execution.end_rendering();
        },
        authored_instance);
    return pass ? foundation::Result::success() : pass.result;
}
}  // namespace aeg::renderer
