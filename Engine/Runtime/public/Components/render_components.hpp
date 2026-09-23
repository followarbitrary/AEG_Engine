/**
 * @file render_components.hpp
 * @brief Runtime-owned ECS data contracts used by Scene and RenderExtract.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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
    /** @brief Linear RGB material tint applied by the standard opaque shader. */
    math::Vec3 tint{1.0f, 1.0f, 1.0f};
    /** @brief Alpha multiplier retained by the material contract. */
    float tint_alpha{1.0f};
    /** @brief Local-space center of the conservative mesh bounds. */
    math::Vec3 bounds_center{};
    /** @brief Non-negative local-space radius of the conservative mesh bounds. */
    float bounds_radius{0.0f};
    /** @brief Per-view visibility mask applied during extraction. */
    uint32_t visibility_mask{0xffffffffu};
    /** @brief Controls whether a fully assigned renderer contributes an instance. */
    bool visible{true};
};

/** @brief Runtime world-space text rendered through the same frame resource path as meshes. */
struct TextRenderer final {
    /** @brief Stable imported font Asset ID; zero selects the bundled fallback font. */
    uint64_t font_asset_id{0};
    /** @brief UTF-8 text; the runtime atlas currently guarantees ASCII and substitutes '?' for other codepoints. */
    std::string text{"AEG"};
    /** @brief World-space pixel height of the glyphs. */
    float world_size{0.5f};
    /** @brief Linear RGBA text color. */
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    float color_alpha{1.0f};
    /** @brief Per-view visibility mask. */
    uint32_t visibility_mask{0xffffffffu};
    /** @brief Controls whether the text contributes an instance. */
    bool visible{true};
};

[[nodiscard]] inline uint64_t text_asset_hash(uint64_t font_asset_id, std::string_view text,
                                              float world_size) noexcept {
    uint64_t hash = 14695981039346656037ull ^ font_asset_id;
    const auto mix = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    for (const unsigned char value : text) {
        mix(value);
    }
    const auto* size_bytes = reinterpret_cast<const uint8_t*>(&world_size);
    for (std::size_t index = 0; index < sizeof(world_size); ++index) {
        mix(size_bytes[index]);
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] inline uint64_t text_mesh_asset_id(uint64_t font_asset_id, std::string_view text,
                                                 float world_size) noexcept {
    return 0x4000000000000000ull | (text_asset_hash(font_asset_id, text, world_size) & 0x3fffffffffffffffull);
}

[[nodiscard]] inline uint64_t text_material_asset_id(uint64_t font_asset_id, std::string_view text,
                                                     float world_size) noexcept {
    return 0x6000000000000000ull | (text_asset_hash(font_asset_id, text, world_size) & 0x1fffffffffffffffull);
}

/** @brief Minimal directional light parameters consumed by the opaque Lambert pass. */
struct DirectionalLight final {
    /** @brief World-space direction from the light toward the scene. */
    math::Vec3 direction{0.0f, 0.0f, -1.0f};
    /** @brief Linear RGB light color in the normalized range. */
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    /** @brief Non-negative directional intensity. */
    float intensity{1.0f};
    /** @brief Controls whether this light contributes during extraction. */
    bool enabled{true};
    /** @brief Enables directional shadow casting in supported renderer variants. */
    bool casts_shadow{false};
};

/** @brief Local light at the entity Transform position; range is measured in world units. */
struct PointLight final {
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity{1.0f};
    float range{5.0f};
    uint32_t visibility_mask{0xffffffffu};
    bool enabled{true};
};

/** @brief Local cone light; Transform rotation maps its local direction into world space. */
struct SpotLight final {
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity{1.0f};
    float range{5.0f};
    uint32_t visibility_mask{0xffffffffu};
    bool enabled{true};
    math::Vec3 direction{0.0f, 0.0f, -1.0f};
    float inner_cone_cosine{0.95f};
    float outer_cone_cosine{0.85f};
};

/** @brief One enabled environment supplies the view's procedural sky and postprocessing. */
struct SceneEnvironment final {
    math::Vec3 sky_zenith{0.08f, 0.2f, 0.5f};
    math::Vec3 sky_horizon{0.5f, 0.65f, 0.9f};
    math::Vec3 sky_ground{0.025f, 0.025f, 0.025f};
    float exposure{1.0f};
    float gamma{2.2f};
    bool sky_enabled{true};
    bool tone_mapping{true};
    bool enabled{true};
};
}  // namespace aeg::engine::components
