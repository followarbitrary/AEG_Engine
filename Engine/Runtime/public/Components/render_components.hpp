/**
 * @file render_components.hpp
 * @brief Runtime-owned ECS data contracts used by Scene and RenderExtract.
 */
#pragma once

#include <cstdint>

#include "math.hpp"

namespace aeg::engine::components {
/** @brief Reuses the canonical Math transform as the ECS transform component. */
using Transform = math::Transform;

/** @brief Camera projection parameters consumed while creating a RenderView. */
struct Camera final {
    /** @brief Vertical field of view in radians. */
    float vertical_fov_radians{1.0471976f};
    /** @brief Positive near clipping-plane distance. */
    float near_plane{0.1f};
    /** @brief Positive far clipping-plane distance greater than near_plane. */
    float far_plane{1000.0f};
};

/** @brief Renderable mesh data attached to an entity with a Transform component. */
struct MeshRenderer final {
    /** @brief Stable renderer mesh asset identity; zero leaves the renderer unassigned. */
    uint64_t mesh_asset_id{0};
    /** @brief Stable renderer material identity; zero leaves the renderer unassigned. */
    uint64_t material_asset_id{0};
    /** @brief Local-space center of the conservative mesh bounds. */
    math::Vec3 bounds_center{};
    /** @brief Non-negative local-space radius of the conservative mesh bounds. */
    float bounds_radius{0.0f};
    /** @brief Per-view visibility mask applied during extraction. */
    uint32_t visibility_mask{0xffffffffu};
    /** @brief Controls whether a fully assigned renderer contributes an instance. */
    bool visible{true};
};
}  // namespace aeg::engine::components
