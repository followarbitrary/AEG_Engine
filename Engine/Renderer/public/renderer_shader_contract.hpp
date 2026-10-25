/**
 * @file renderer_shader_contract.hpp
 * @brief Backend-neutral fixed shader value contracts owned by Renderer.
 */
#pragma once

#include <cstdint>

#include "math.hpp"
#include "rhi_struct.hpp"

namespace aeg::renderer {

/** @brief Directional shadow constants shared by the public resolver contract. */
struct RenderShadowConstants final {
    math::Mat4 clip_to_shadow{math::identity()};
    /** @brief Inverse viewport extent, depth comparison bias, reserved. */
    math::Vec4 parameters{};
};

static_assert(sizeof(RenderShadowConstants) == 80);

/** @brief Interleaved vertex attribute offsets and formats consumed by Opaque. */
struct RenderMeshVertexLayout final {
    /** @brief Byte offset of the float4 position within one interleaved vertex. */
    uint32_t position_offset{0};
    /** @brief Shader-facing format of the position attribute. */
    rhi::Format position_format{rhi::Format::kRgba32Float};
    /** @brief Byte offset of the float2 UV within one interleaved vertex. */
    uint32_t uv_offset{sizeof(float) * 4u};
    /** @brief Shader-facing format of the UV attribute. */
    rhi::Format uv_format{rhi::Format::kRg32Float};
    /** @brief Byte offset of the float3 normal within one interleaved vertex. */
    uint32_t normal_offset{sizeof(float) * 6u};
    /** @brief Shader-facing format of the normal attribute. */
    rhi::Format normal_format{rhi::Format::kRgb32Float};
};

/** @brief Object constants shared by the standard opaque vertex shader contract. */
struct RenderObjectConstants final {
    /** @brief Matrix that transforms local positions into clip space. */
    math::Mat4 model_view_projection{math::identity()};
    /** @brief Inverse-transpose matrix that transforms local normals into view space. */
    math::Mat4 normal_matrix{math::identity()};
};

static_assert(sizeof(RenderObjectConstants) == sizeof(math::Mat4) * 2u);

/** @brief Frame/view-local constants consumed by the minimal Lambert shader contract. */
struct RenderLightingConstants final {
    /** @brief View-space light direction and directional intensity in w. */
    math::Vec4 directional_direction_intensity{0.0f, 0.0f, -1.0f, 0.0f};
    /** @brief Linear directional light color; alpha is reserved and must remain one. */
    math::Vec4 directional_color{0.0f, 0.0f, 0.0f, 1.0f};
    /** @brief Linear environment contribution; alpha is reserved and must remain one. */
    math::Vec4 ambient_color{0.05f, 0.05f, 0.05f, 1.0f};

    [[nodiscard]] bool is_valid() const noexcept;
};

static_assert(sizeof(RenderLightingConstants) == sizeof(math::Vec4) * 3u);

}  // namespace aeg::renderer
