/**
 * @file simd.hpp
 * @brief Private SSE2 primitives used by the public math implementation.
 *
 * This header is an implementation detail. The public math contract remains
 * independent of the selected SIMD instruction set.
 */
#pragma once

#include "../../public/math.hpp"

#if defined(AEG_MATH_ENABLE_SIMD) && AEG_MATH_ENABLE_SIMD && \
    (defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__))
#define AEG_MATH_HAS_SSE2 1
#include <emmintrin.h>
#else
#define AEG_MATH_HAS_SSE2 0
#endif

namespace aeg::math::detail {
#if AEG_MATH_HAS_SSE2
struct Float4 {
    __m128 value;
};

[[nodiscard]] inline Float4 load(const float* values) noexcept {
    return {_mm_loadu_ps(values)};
}

[[nodiscard]] inline Float4 load(Vec2 value) noexcept {
    const float values[4]{value.x, value.y, 0.0f, 0.0f};
    return load(values);
}

[[nodiscard]] inline Float4 load(Vec3 value) noexcept {
    const float values[4]{value.x, value.y, value.z, 0.0f};
    return load(values);
}

[[nodiscard]] inline Float4 load(Vec4 value) noexcept {
    const float values[4]{value.x, value.y, value.z, value.w};
    return load(values);
}

[[nodiscard]] inline Float4 load(Quat value) noexcept {
    const float values[4]{value.x, value.y, value.z, value.w};
    return load(values);
}

[[nodiscard]] inline Vec2 store_vec2(Float4 value) noexcept {
    alignas(16) float values[4];
    _mm_store_ps(values, value.value);
    return Vec2{values[0], values[1]};
}

[[nodiscard]] inline Vec3 store_vec3(Float4 value) noexcept {
    alignas(16) float values[4];
    _mm_store_ps(values, value.value);
    return Vec3{values[0], values[1], values[2]};
}

[[nodiscard]] inline Vec4 store_vec4(Float4 value) noexcept {
    alignas(16) float values[4];
    _mm_store_ps(values, value.value);
    return Vec4{values[0], values[1], values[2], values[3]};
}

[[nodiscard]] inline Quat store_quat(Float4 value) noexcept {
    alignas(16) float values[4];
    _mm_store_ps(values, value.value);
    return Quat{values[0], values[1], values[2], values[3]};
}

[[nodiscard]] inline Float4 zero() noexcept {
    return {_mm_setzero_ps()};
}

[[nodiscard]] inline Float4 splat(float value) noexcept {
    return {_mm_set1_ps(value)};
}

[[nodiscard]] inline Float4 add(Float4 lhs, Float4 rhs) noexcept {
    return {_mm_add_ps(lhs.value, rhs.value)};
}

[[nodiscard]] inline Float4 subtract(Float4 lhs, Float4 rhs) noexcept {
    return {_mm_sub_ps(lhs.value, rhs.value)};
}

[[nodiscard]] inline Float4 multiply(Float4 lhs, Float4 rhs) noexcept {
    return {_mm_mul_ps(lhs.value, rhs.value)};
}

[[nodiscard]] inline Float4 divide(Float4 lhs, Float4 rhs) noexcept {
    return {_mm_div_ps(lhs.value, rhs.value)};
}

[[nodiscard]] inline Float4 negate(Float4 value) noexcept {
    return {_mm_sub_ps(_mm_setzero_ps(), value.value)};
}

[[nodiscard]] inline float horizontal_sum(Float4 value) noexcept {
    __m128 pairwise = _mm_add_ps(value.value, _mm_movehl_ps(value.value, value.value));
    pairwise = _mm_add_ss(pairwise, _mm_shuffle_ps(pairwise, pairwise, 0x01));
    return _mm_cvtss_f32(pairwise);
}

[[nodiscard]] inline Float4 cross3(Float4 lhs, Float4 rhs) noexcept {
    const __m128 lhs_yzx = _mm_shuffle_ps(lhs.value, lhs.value, _MM_SHUFFLE(3, 0, 2, 1));
    const __m128 rhs_yzx = _mm_shuffle_ps(rhs.value, rhs.value, _MM_SHUFFLE(3, 0, 2, 1));
    const __m128 lhs_zxy = _mm_shuffle_ps(lhs.value, lhs.value, _MM_SHUFFLE(3, 1, 0, 2));
    const __m128 rhs_zxy = _mm_shuffle_ps(rhs.value, rhs.value, _MM_SHUFFLE(3, 1, 0, 2));
    return {_mm_sub_ps(_mm_mul_ps(lhs_yzx, rhs_zxy), _mm_mul_ps(lhs_zxy, rhs_yzx))};
}

[[nodiscard]] inline Quat multiply_quat(Quat lhs, Quat rhs) noexcept {
    const __m128 lhs_value = load(lhs).value;
    const __m128 rhs_value = load(rhs).value;
    const __m128 xyz_mask = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    const __m128 lhs_xyz = _mm_and_ps(lhs_value, xyz_mask);
    const __m128 rhs_xyz = _mm_and_ps(rhs_value, xyz_mask);
    const __m128 lhs_w = _mm_shuffle_ps(lhs_value, lhs_value, _MM_SHUFFLE(3, 3, 3, 3));
    const __m128 rhs_w = _mm_shuffle_ps(rhs_value, rhs_value, _MM_SHUFFLE(3, 3, 3, 3));
    const Float4 vector_part{_mm_add_ps(_mm_add_ps(_mm_mul_ps(lhs_w, rhs_xyz), _mm_mul_ps(rhs_w, lhs_xyz)),
                                        cross3(Float4{lhs_xyz}, Float4{rhs_xyz}).value)};
    const float scalar_part =
        _mm_cvtss_f32(_mm_mul_ss(lhs_w, rhs_w)) - horizontal_sum(Float4{_mm_mul_ps(lhs_xyz, rhs_xyz)});
    const Vec3 vector = store_vec3(vector_part);
    return Quat{vector.x, vector.y, vector.z, scalar_part};
}

[[nodiscard]] inline Quat conjugate_quat(Quat value) noexcept {
    const __m128 sign_mask = _mm_set_ps(0.0f, -0.0f, -0.0f, -0.0f);
    return store_quat(Float4{_mm_xor_ps(load(value).value, sign_mask)});
}

[[nodiscard]] inline Quat scale_quat(Quat value, float scalar) noexcept {
    return store_quat(multiply(load(value), splat(scalar)));
}
#endif
}  // namespace aeg::math::detail
