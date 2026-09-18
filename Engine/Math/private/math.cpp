/**
 * @file math.cpp
 * @brief Implements the scalar and optional SSE2 math operations.
 */
#include "../public/math.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>

#include "simd/simd.hpp"

namespace aeg::math {
namespace {
constexpr float kHalf = 0.5f;
constexpr float kPlaneNormalizationTolerance = 1.0e-3f;

[[nodiscard]] bool is_valid_scalar(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool is_valid_vector(Vec2 value) noexcept {
    return is_valid_scalar(value.x) && is_valid_scalar(value.y);
}

[[nodiscard]] bool is_valid_vector(Vec3 value) noexcept {
    return is_valid_scalar(value.x) && is_valid_scalar(value.y) && is_valid_scalar(value.z);
}

[[nodiscard]] bool is_valid_vector(Vec4 value) noexcept {
    return is_valid_scalar(value.x) && is_valid_scalar(value.y) && is_valid_scalar(value.z) && is_valid_scalar(value.w);
}

[[nodiscard]] Vec4 matrix_row(const Mat4& matrix, std::size_t row) noexcept {
    return Vec4{matrix.at(row, 0), matrix.at(row, 1), matrix.at(row, 2), matrix.at(row, 3)};
}

[[nodiscard]] bool try_normalize_plane(Vec4 equation, Plane& result) noexcept {
    if (!is_valid_vector(equation)) {
        return false;
    }

    const float normal_length = std::sqrt(length_squared(Vec3{equation.x, equation.y, equation.z}));
    if (!is_valid_scalar(normal_length) || normal_length <= kEpsilon) {
        return false;
    }

    const float inverse_length = 1.0f / normal_length;
    const Vec4 normalized{equation.x * inverse_length, equation.y * inverse_length, equation.z * inverse_length,
                          equation.w * inverse_length};
    if (!is_valid_vector(normalized)) {
        return false;
    }

    result = Plane{normalized};
    return true;
}

[[nodiscard]] bool is_valid_quat(Quat value) noexcept {
    return is_valid_scalar(value.x) && is_valid_scalar(value.y) && is_valid_scalar(value.z) && is_valid_scalar(value.w);
}

[[nodiscard]] float safe_sqrt(float value) noexcept {
    return std::sqrt(std::max(value, 0.0f));
}
}  // namespace

float& Vec2::operator[](std::size_t index) noexcept {
    assert(index < 2);
    return index == 0 ? x : y;
}

const float& Vec2::operator[](std::size_t index) const noexcept {
    assert(index < 2);
    return index == 0 ? x : y;
}

float& Vec3::operator[](std::size_t index) noexcept {
    assert(index < 3);
    switch (index) {
        case 0:
            return x;
        case 1:
            return y;
        case 2:
            return z;
        default:
            return x;
    }
}

const float& Vec3::operator[](std::size_t index) const noexcept {
    assert(index < 3);
    switch (index) {
        case 0:
            return x;
        case 1:
            return y;
        case 2:
            return z;
        default:
            return x;
    }
}

float& Vec4::operator[](std::size_t index) noexcept {
    assert(index < 4);
    switch (index) {
        case 0:
            return x;
        case 1:
            return y;
        case 2:
            return z;
        case 3:
            return w;
        default:
            return x;
    }
}

const float& Vec4::operator[](std::size_t index) const noexcept {
    assert(index < 4);
    switch (index) {
        case 0:
            return x;
        case 1:
            return y;
        case 2:
            return z;
        case 3:
            return w;
        default:
            return x;
    }
}

Vec4& Mat4::operator[](std::size_t column) noexcept {
    assert(column < 4);
    return columns[column];
}

const Vec4& Mat4::operator[](std::size_t column) const noexcept {
    assert(column < 4);
    return columns[column];
}

float& Mat4::at(std::size_t row, std::size_t column) noexcept {
    assert(row < 4 && column < 4);
    return columns[column][row];
}

const float& Mat4::at(std::size_t row, std::size_t column) const noexcept {
    assert(row < 4 && column < 4);
    return columns[column][row];
}

Vec3 Plane::normal() const noexcept {
    return Vec3{equation.x, equation.y, equation.z};
}

float Plane::offset() const noexcept {
    return equation.w;
}

float Plane::signed_distance(Vec3 point) const noexcept {
    return equation.x * point.x + equation.y * point.y + equation.z * point.z + equation.w;
}

bool Plane::is_valid() const noexcept {
    if (!is_valid_vector(equation)) {
        return false;
    }

    const float normal_length = std::sqrt(length_squared(Vec3{equation.x, equation.y, equation.z}));
    return is_valid_scalar(normal_length) && normal_length > kEpsilon;
}

Plane& Frustum::operator[](FrustumPlane plane) noexcept {
    const auto index = static_cast<std::size_t>(plane);
    assert(index < kFrustumPlaneCount);
    return planes[index];
}

const Plane& Frustum::operator[](FrustumPlane plane) const noexcept {
    const auto index = static_cast<std::size_t>(plane);
    assert(index < kFrustumPlaneCount);
    return planes[index];
}

bool Frustum::is_valid() const noexcept {
    for (const Plane& plane : planes) {
        if (!plane.is_valid()) {
            return false;
        }

        const float normal_length = length(plane.normal());
        if (!is_valid_scalar(normal_length) || std::abs(normal_length - 1.0f) > kPlaneNormalizationTolerance) {
            return false;
        }
    }
    return true;
}

bool Frustum::contains_point(Vec3 point) const noexcept {
    assert(is_valid());
    if (!is_valid_vector(point)) {
        return false;
    }

    for (const Plane& plane : planes) {
        if (plane.signed_distance(point) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersects_sphere(Vec3 center, float radius) const noexcept {
    assert(is_valid());
    if (!is_valid_vector(center) || !is_valid_scalar(radius) || radius < 0.0f) {
        return false;
    }

    for (const Plane& plane : planes) {
        if (plane.signed_distance(center) < -radius) {
            return false;
        }
    }
    return true;
}

Vec2 operator+(Vec2 lhs, Vec2 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec2(detail::add(detail::load(lhs), detail::load(rhs)));
#else
    return Vec2{lhs.x + rhs.x, lhs.y + rhs.y};
#endif
}

Vec2 operator-(Vec2 lhs, Vec2 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec2(detail::subtract(detail::load(lhs), detail::load(rhs)));
#else
    return Vec2{lhs.x - rhs.x, lhs.y - rhs.y};
#endif
}

Vec2 operator-(Vec2 value) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec2(detail::negate(detail::load(value)));
#else
    return Vec2{-value.x, -value.y};
#endif
}

Vec2 operator*(Vec2 value, float scalar) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec2(detail::multiply(detail::load(value), detail::splat(scalar)));
#else
    return Vec2{value.x * scalar, value.y * scalar};
#endif
}

Vec2 operator*(float scalar, Vec2 value) noexcept {
    return value * scalar;
}

Vec2 operator/(Vec2 value, float scalar) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec2(detail::divide(detail::load(value), detail::splat(scalar)));
#else
    return Vec2{value.x / scalar, value.y / scalar};
#endif
}

Vec2& operator+=(Vec2& lhs, Vec2 rhs) noexcept {
    lhs = lhs + rhs;
    return lhs;
}

Vec2& operator-=(Vec2& lhs, Vec2 rhs) noexcept {
    lhs = lhs - rhs;
    return lhs;
}

Vec2& operator*=(Vec2& value, float scalar) noexcept {
    value = value * scalar;
    return value;
}

Vec2& operator/=(Vec2& value, float scalar) noexcept {
    value = value / scalar;
    return value;
}

bool operator==(Vec2 lhs, Vec2 rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

Vec3 operator+(Vec3 lhs, Vec3 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec3(detail::add(detail::load(lhs), detail::load(rhs)));
#else
    return Vec3{lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
#endif
}

Vec3 operator-(Vec3 lhs, Vec3 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec3(detail::subtract(detail::load(lhs), detail::load(rhs)));
#else
    return Vec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
#endif
}

Vec3 operator-(Vec3 value) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec3(detail::negate(detail::load(value)));
#else
    return Vec3{-value.x, -value.y, -value.z};
#endif
}

Vec3 operator*(Vec3 value, float scalar) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec3(detail::multiply(detail::load(value), detail::splat(scalar)));
#else
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
#endif
}

Vec3 operator*(float scalar, Vec3 value) noexcept {
    return value * scalar;
}

Vec3 operator/(Vec3 value, float scalar) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec3(detail::divide(detail::load(value), detail::splat(scalar)));
#else
    return Vec3{value.x / scalar, value.y / scalar, value.z / scalar};
#endif
}

Vec3& operator+=(Vec3& lhs, Vec3 rhs) noexcept {
    lhs = lhs + rhs;
    return lhs;
}

Vec3& operator-=(Vec3& lhs, Vec3 rhs) noexcept {
    lhs = lhs - rhs;
    return lhs;
}

Vec3& operator*=(Vec3& value, float scalar) noexcept {
    value = value * scalar;
    return value;
}

Vec3& operator/=(Vec3& value, float scalar) noexcept {
    value = value / scalar;
    return value;
}

bool operator==(Vec3 lhs, Vec3 rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

Vec4 operator+(Vec4 lhs, Vec4 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec4(detail::add(detail::load(lhs), detail::load(rhs)));
#else
    return Vec4{lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, lhs.w + rhs.w};
#endif
}

Vec4 operator-(Vec4 lhs, Vec4 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec4(detail::subtract(detail::load(lhs), detail::load(rhs)));
#else
    return Vec4{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z, lhs.w - rhs.w};
#endif
}

Vec4 operator-(Vec4 value) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec4(detail::negate(detail::load(value)));
#else
    return Vec4{-value.x, -value.y, -value.z, -value.w};
#endif
}

Vec4 operator*(Vec4 value, float scalar) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec4(detail::multiply(detail::load(value), detail::splat(scalar)));
#else
    return Vec4{value.x * scalar, value.y * scalar, value.z * scalar, value.w * scalar};
#endif
}

Vec4 operator*(float scalar, Vec4 value) noexcept {
    return value * scalar;
}

Vec4 operator/(Vec4 value, float scalar) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec4(detail::divide(detail::load(value), detail::splat(scalar)));
#else
    return Vec4{value.x / scalar, value.y / scalar, value.z / scalar, value.w / scalar};
#endif
}

Vec4& operator+=(Vec4& lhs, Vec4 rhs) noexcept {
    lhs = lhs + rhs;
    return lhs;
}

Vec4& operator-=(Vec4& lhs, Vec4 rhs) noexcept {
    lhs = lhs - rhs;
    return lhs;
}

Vec4& operator*=(Vec4& value, float scalar) noexcept {
    value = value * scalar;
    return value;
}

Vec4& operator/=(Vec4& value, float scalar) noexcept {
    value = value / scalar;
    return value;
}

bool operator==(Vec4 lhs, Vec4 rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

float dot(Vec2 lhs, Vec2 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::horizontal_sum(detail::multiply(detail::load(lhs), detail::load(rhs)));
#else
    return lhs.x * rhs.x + lhs.y * rhs.y;
#endif
}

float dot(Vec3 lhs, Vec3 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::horizontal_sum(detail::multiply(detail::load(lhs), detail::load(rhs)));
#else
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
#endif
}

float dot(Vec4 lhs, Vec4 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::horizontal_sum(detail::multiply(detail::load(lhs), detail::load(rhs)));
#else
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
#endif
}

Vec3 cross(Vec3 lhs, Vec3 rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::store_vec3(detail::cross3(detail::load(lhs), detail::load(rhs)));
#else
    return Vec3{lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x};
#endif
}

float length_squared(Vec2 value) noexcept {
    return dot(value, value);
}

float length_squared(Vec3 value) noexcept {
    return dot(value, value);
}

float length_squared(Vec4 value) noexcept {
    return dot(value, value);
}

float length(Vec2 value) noexcept {
    return safe_sqrt(length_squared(value));
}

float length(Vec3 value) noexcept {
    return safe_sqrt(length_squared(value));
}

float length(Vec4 value) noexcept {
    return safe_sqrt(length_squared(value));
}

bool try_normalize(Vec2 value, Vec2& result) noexcept {
    const float squared_length = length_squared(value);
    if (!is_valid_vector(value) || !is_valid_scalar(squared_length) || squared_length <= kEpsilon * kEpsilon) {
        return false;
    }
    result = value / std::sqrt(squared_length);
    return true;
}

bool try_normalize(Vec3 value, Vec3& result) noexcept {
    const float squared_length = length_squared(value);
    if (!is_valid_vector(value) || !is_valid_scalar(squared_length) || squared_length <= kEpsilon * kEpsilon) {
        return false;
    }
    result = value / std::sqrt(squared_length);
    return true;
}

bool try_normalize(Vec4 value, Vec4& result) noexcept {
    const float squared_length = length_squared(value);
    if (!is_valid_vector(value) || !is_valid_scalar(squared_length) || squared_length <= kEpsilon * kEpsilon) {
        return false;
    }
    result = value / std::sqrt(squared_length);
    return true;
}

Vec2 normalize(Vec2 value) noexcept {
    Vec2 result{};
    return try_normalize(value, result) ? result : Vec2{};
}

Vec3 normalize(Vec3 value) noexcept {
    Vec3 result{};
    return try_normalize(value, result) ? result : Vec3{};
}

Vec4 normalize(Vec4 value) noexcept {
    Vec4 result{};
    return try_normalize(value, result) ? result : Vec4{};
}

Quat operator*(Quat lhs, Quat rhs) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::multiply_quat(lhs, rhs);
#else
    return Quat{lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
                lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
                lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
                lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z};
#endif
}

bool operator==(Quat lhs, Quat rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

float length_squared(Quat value) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::horizontal_sum(detail::multiply(detail::load(value), detail::load(value)));
#else
    return value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
#endif
}

Quat normalize(Quat value) noexcept {
    const float squared_length = length_squared(value);
    if (!is_valid_quat(value) || !is_valid_scalar(squared_length) || squared_length <= kEpsilon * kEpsilon) {
        return Quat{};
    }
    const float scale_value = 1.0f / std::sqrt(squared_length);
#if AEG_MATH_HAS_SSE2
    return detail::scale_quat(value, scale_value);
#else
    return Quat{value.x * scale_value, value.y * scale_value, value.z * scale_value, value.w * scale_value};
#endif
}

Quat conjugate(Quat value) noexcept {
#if AEG_MATH_HAS_SSE2
    return detail::conjugate_quat(value);
#else
    return Quat{-value.x, -value.y, -value.z, value.w};
#endif
}

Quat inverse(Quat value) noexcept {
    const float squared_length = length_squared(value);
    if (!is_valid_quat(value) || !is_valid_scalar(squared_length) || squared_length <= kEpsilon * kEpsilon) {
        return Quat{};
    }
    const Quat result = conjugate(value);
    const float scale_value = 1.0f / squared_length;
#if AEG_MATH_HAS_SSE2
    return detail::scale_quat(result, scale_value);
#else
    return Quat{result.x * scale_value, result.y * scale_value, result.z * scale_value, result.w * scale_value};
#endif
}

Quat quat_from_axis_angle(Vec3 axis, float radians) noexcept {
    const Vec3 normalized_axis = normalize(axis);
    if (length_squared(normalized_axis) <= kEpsilon * kEpsilon || !is_valid_scalar(radians)) {
        return Quat{};
    }
    const float half_angle = radians * kHalf;
    const float sine = std::sin(half_angle);
    return normalize(
        Quat{normalized_axis.x * sine, normalized_axis.y * sine, normalized_axis.z * sine, std::cos(half_angle)});
}

Vec3 rotate(Quat rotation, Vec3 value) noexcept {
    const Quat normalized_rotation = normalize(rotation);
    const Vec3 quaternion_vector{normalized_rotation.x, normalized_rotation.y, normalized_rotation.z};
    const Vec3 first_cross = cross(quaternion_vector, value);
    const Vec3 second_cross = cross(quaternion_vector, first_cross);
    return value + (2.0f * normalized_rotation.w) * first_cross + 2.0f * second_cross;
}

Vec4 operator*(const Mat4& matrix, Vec4 value) noexcept {
#if AEG_MATH_HAS_SSE2
    detail::Float4 result = detail::zero();
    for (std::size_t column = 0; column < 4; ++column) {
        result = detail::add(result, detail::multiply(detail::load(matrix[column]), detail::splat(value[column])));
    }
    return detail::store_vec4(result);
#else
    return Vec4{
        matrix.at(0, 0) * value.x + matrix.at(0, 1) * value.y + matrix.at(0, 2) * value.z + matrix.at(0, 3) * value.w,
        matrix.at(1, 0) * value.x + matrix.at(1, 1) * value.y + matrix.at(1, 2) * value.z + matrix.at(1, 3) * value.w,
        matrix.at(2, 0) * value.x + matrix.at(2, 1) * value.y + matrix.at(2, 2) * value.z + matrix.at(2, 3) * value.w,
        matrix.at(3, 0) * value.x + matrix.at(3, 1) * value.y + matrix.at(3, 2) * value.z + matrix.at(3, 3) * value.w};
#endif
}

Mat4 operator*(const Mat4& lhs, const Mat4& rhs) noexcept {
    Mat4 result{};
#if AEG_MATH_HAS_SSE2
    for (std::size_t column = 0; column < 4; ++column) {
        detail::Float4 result_column = detail::zero();
        for (std::size_t source_column = 0; source_column < 4; ++source_column) {
            result_column = detail::add(result_column, detail::multiply(detail::load(lhs[source_column]),
                                                                        detail::splat(rhs[column][source_column])));
        }
        result[column] = detail::store_vec4(result_column);
    }
#else
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            float value = 0.0f;
            for (std::size_t source_column = 0; source_column < 4; ++source_column) {
                value += lhs.at(row, source_column) * rhs.at(source_column, column);
            }
            result.at(row, column) = value;
        }
    }
#endif
    return result;
}

bool operator==(const Mat4& lhs, const Mat4& rhs) noexcept {
    return lhs.columns == rhs.columns;
}

Mat4 identity() noexcept {
    Mat4 result{};
    result.at(0, 0) = 1.0f;
    result.at(1, 1) = 1.0f;
    result.at(2, 2) = 1.0f;
    result.at(3, 3) = 1.0f;
    return result;
}

Mat4 translation(Vec3 value) noexcept {
    Mat4 result = identity();
    result.at(0, 3) = value.x;
    result.at(1, 3) = value.y;
    result.at(2, 3) = value.z;
    return result;
}

Mat4 scale(Vec3 value) noexcept {
    Mat4 result{};
    result.at(0, 0) = value.x;
    result.at(1, 1) = value.y;
    result.at(2, 2) = value.z;
    result.at(3, 3) = 1.0f;
    return result;
}

Mat4 rotation_x(float radians) noexcept {
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    Mat4 result = identity();
    result.at(1, 1) = cosine;
    result.at(2, 1) = sine;
    result.at(1, 2) = -sine;
    result.at(2, 2) = cosine;
    return result;
}

Mat4 rotation_y(float radians) noexcept {
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    Mat4 result = identity();
    result.at(0, 0) = cosine;
    result.at(2, 0) = -sine;
    result.at(0, 2) = sine;
    result.at(2, 2) = cosine;
    return result;
}

Mat4 rotation_z(float radians) noexcept {
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    Mat4 result = identity();
    result.at(0, 0) = cosine;
    result.at(1, 0) = sine;
    result.at(0, 1) = -sine;
    result.at(1, 1) = cosine;
    return result;
}

Mat4 from_quat(Quat rotation) noexcept {
    const Quat normalized_rotation = normalize(rotation);
    const float xx = normalized_rotation.x * normalized_rotation.x;
    const float yy = normalized_rotation.y * normalized_rotation.y;
    const float zz = normalized_rotation.z * normalized_rotation.z;
    const float xy = normalized_rotation.x * normalized_rotation.y;
    const float xz = normalized_rotation.x * normalized_rotation.z;
    const float yz = normalized_rotation.y * normalized_rotation.z;
    const float wx = normalized_rotation.w * normalized_rotation.x;
    const float wy = normalized_rotation.w * normalized_rotation.y;
    const float wz = normalized_rotation.w * normalized_rotation.z;

    Mat4 result = identity();
    result.at(0, 0) = 1.0f - 2.0f * (yy + zz);
    result.at(1, 0) = 2.0f * (xy + wz);
    result.at(2, 0) = 2.0f * (xz - wy);
    result.at(0, 1) = 2.0f * (xy - wz);
    result.at(1, 1) = 1.0f - 2.0f * (xx + zz);
    result.at(2, 1) = 2.0f * (yz + wx);
    result.at(0, 2) = 2.0f * (xz + wy);
    result.at(1, 2) = 2.0f * (yz - wx);
    result.at(2, 2) = 1.0f - 2.0f * (xx + yy);
    return result;
}

Mat4 trs(Vec3 translation_value, Quat rotation, Vec3 scale_value) noexcept {
    return translation(translation_value) * from_quat(rotation) * scale(scale_value);
}

bool try_perspective(float vertical_fov_radians, float aspect_ratio, float near_plane, float far_plane, Mat4& result,
                     ClipSpaceConvention convention) noexcept {
    if (!is_valid_scalar(vertical_fov_radians) || !is_valid_scalar(aspect_ratio) || !is_valid_scalar(near_plane) ||
        !is_valid_scalar(far_plane) || vertical_fov_radians <= 0.0f || vertical_fov_radians >= kPi ||
        aspect_ratio <= 0.0f || near_plane <= 0.0f || far_plane <= near_plane) {
        return false;
    }

    const float tangent = std::tan(vertical_fov_radians * kHalf);
    if (!is_valid_scalar(tangent) || std::abs(tangent) <= kEpsilon) {
        return false;
    }

    const float focal_length = 1.0f / tangent;
    const float depth_denominator = far_plane - near_plane;
    Mat4 projection{};
    projection.at(0, 0) = focal_length / aspect_ratio;
    projection.at(1, 1) = convention.y_down ? -focal_length : focal_length;
    projection.at(3, 2) = -1.0f;
    if (convention.reversed_z) {
        projection.at(2, 2) = near_plane / depth_denominator;
        projection.at(2, 3) = (far_plane * near_plane) / depth_denominator;
    } else {
        projection.at(2, 2) = -far_plane / depth_denominator;
        projection.at(2, 3) = -(far_plane * near_plane) / depth_denominator;
    }
    result = projection;
    return true;
}

Mat4 perspective(float vertical_fov_radians, float aspect_ratio, float near_plane, float far_plane,
                 ClipSpaceConvention convention) noexcept {
    Mat4 result{};
    (void)try_perspective(vertical_fov_radians, aspect_ratio, near_plane, far_plane, result, convention);
    return result;
}

bool try_orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane, Mat4& result,
                      ClipSpaceConvention convention) noexcept {
    if (!is_valid_scalar(left) || !is_valid_scalar(right) || !is_valid_scalar(bottom) || !is_valid_scalar(top) ||
        !is_valid_scalar(near_plane) || !is_valid_scalar(far_plane) || right == left || top == bottom ||
        near_plane <= 0.0f || far_plane <= near_plane) {
        return false;
    }

    const float width = right - left;
    const float height = top - bottom;
    const float depth = far_plane - near_plane;
    Mat4 projection = identity();
    projection.at(0, 0) = 2.0f / width;
    projection.at(1, 1) = convention.y_down ? -2.0f / height : 2.0f / height;
    projection.at(2, 2) = convention.reversed_z ? 1.0f / depth : -1.0f / depth;
    projection.at(0, 3) = -(right + left) / width;
    projection.at(1, 3) = convention.y_down ? (top + bottom) / height : -(top + bottom) / height;
    projection.at(2, 3) = convention.reversed_z ? far_plane / depth : -near_plane / depth;
    result = projection;
    return true;
}

Mat4 orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane,
                  ClipSpaceConvention convention) noexcept {
    Mat4 result{};
    (void)try_orthographic(left, right, bottom, top, near_plane, far_plane, result, convention);
    return result;
}

bool try_extract_frustum(const Mat4& view_projection, Frustum& result, ClipSpaceConvention convention) noexcept {
    const Vec4 row0 = matrix_row(view_projection, 0);
    const Vec4 row1 = matrix_row(view_projection, 1);
    const Vec4 row2 = matrix_row(view_projection, 2);
    const Vec4 row3 = matrix_row(view_projection, 3);

    std::array<Vec4, kFrustumPlaneCount> raw{};
    raw[static_cast<std::size_t>(FrustumPlane::kLeft)] = row3 + row0;
    raw[static_cast<std::size_t>(FrustumPlane::kRight)] = row3 - row0;
    raw[static_cast<std::size_t>(FrustumPlane::kBottom)] = convention.y_down ? row3 - row1 : row3 + row1;
    raw[static_cast<std::size_t>(FrustumPlane::kTop)] = convention.y_down ? row3 + row1 : row3 - row1;
    if (convention.reversed_z) {
        raw[static_cast<std::size_t>(FrustumPlane::kNear)] = row3 - row2;
        raw[static_cast<std::size_t>(FrustumPlane::kFar)] = row2;
    } else {
        raw[static_cast<std::size_t>(FrustumPlane::kNear)] = row2;
        raw[static_cast<std::size_t>(FrustumPlane::kFar)] = row3 - row2;
    }

    Frustum extracted{};
    for (std::size_t index = 0; index < kFrustumPlaneCount; ++index) {
        if (!try_normalize_plane(raw[index], extracted.planes[index])) {
            return false;
        }
    }

    result = extracted;
    return true;
}

bool try_look_at(Vec3 eye, Vec3 target, Vec3 up, Mat4& result) noexcept {
    Vec3 forward{};
    if (!try_normalize(target - eye, forward)) {
        return false;
    }

    Vec3 right{};
    if (!try_normalize(cross(forward, up), right)) {
        return false;
    }
    const Vec3 corrected_up = cross(right, forward);

    Mat4 view = identity();
    view.at(0, 0) = right.x;
    view.at(1, 0) = right.y;
    view.at(2, 0) = right.z;
    view.at(0, 1) = corrected_up.x;
    view.at(1, 1) = corrected_up.y;
    view.at(2, 1) = corrected_up.z;
    view.at(0, 2) = -forward.x;
    view.at(1, 2) = -forward.y;
    view.at(2, 2) = -forward.z;
    view.at(0, 3) = -dot(right, eye);
    view.at(1, 3) = -dot(corrected_up, eye);
    view.at(2, 3) = dot(forward, eye);
    result = view;
    return true;
}

Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    Mat4 result{};
    (void)try_look_at(eye, target, up, result);
    return result;
}

bool try_inverse(const Mat4& matrix, Mat4& result) noexcept {
    float augmented[4][8]{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            augmented[row][column] = matrix.at(row, column);
        }
        augmented[row][row + 4] = 1.0f;
    }

    for (std::size_t pivot_column = 0; pivot_column < 4; ++pivot_column) {
        std::size_t pivot_row = pivot_column;
        float pivot_value = std::abs(augmented[pivot_row][pivot_column]);
        for (std::size_t row = pivot_column + 1; row < 4; ++row) {
            const float candidate = std::abs(augmented[row][pivot_column]);
            if (candidate > pivot_value) {
                pivot_value = candidate;
                pivot_row = row;
            }
        }
        if (!is_valid_scalar(pivot_value) || pivot_value <= kEpsilon) {
            return false;
        }
        if (pivot_row != pivot_column) {
            for (std::size_t column = 0; column < 8; ++column) {
                std::swap(augmented[pivot_row][column], augmented[pivot_column][column]);
            }
        }

        const float inverse_pivot = 1.0f / augmented[pivot_column][pivot_column];
        for (float& value : augmented[pivot_column]) {
            value *= inverse_pivot;
        }

        for (std::size_t row = 0; row < 4; ++row) {
            if (row == pivot_column) {
                continue;
            }
            const float factor = augmented[row][pivot_column];
            for (std::size_t column = 0; column < 8; ++column) {
                augmented[row][column] -= factor * augmented[pivot_column][column];
            }
        }
    }

    Mat4 inverse_matrix{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            inverse_matrix.at(row, column) = augmented[row][column + 4];
        }
    }
    result = inverse_matrix;
    return true;
}

Mat4 inverse(const Mat4& matrix) noexcept {
    Mat4 result{};
    (void)try_inverse(matrix, result);
    return result;
}

Vec3 transform_point(const Mat4& matrix, Vec3 point) noexcept {
    Vec3 result{};
    (void)try_transform_point(matrix, point, result);
    return result;
}

Vec3 transform_vector(const Mat4& matrix, Vec3 vector) noexcept {
    const Vec4 transformed = matrix * Vec4{vector.x, vector.y, vector.z, 0.0f};
    return Vec3{transformed.x, transformed.y, transformed.z};
}

bool try_transform_point(const Mat4& matrix, Vec3 point, Vec3& result) noexcept {
    const Vec4 transformed = matrix * Vec4{point.x, point.y, point.z, 1.0f};
    if (!is_valid_vector(transformed) || std::abs(transformed.w) <= kEpsilon) {
        return false;
    }
    result = Vec3{transformed.x / transformed.w, transformed.y / transformed.w, transformed.z / transformed.w};
    return is_valid_vector(result);
}

bool nearly_equal(float lhs, float rhs, float tolerance) noexcept {
    return is_valid_scalar(lhs) && is_valid_scalar(rhs) && is_valid_scalar(tolerance) && tolerance >= 0.0f &&
           std::abs(lhs - rhs) <= tolerance;
}

bool nearly_equal(Vec3 lhs, Vec3 rhs, float tolerance) noexcept {
    return nearly_equal(lhs.x, rhs.x, tolerance) && nearly_equal(lhs.y, rhs.y, tolerance) &&
           nearly_equal(lhs.z, rhs.z, tolerance);
}

bool nearly_equal(Vec4 lhs, Vec4 rhs, float tolerance) noexcept {
    return nearly_equal(lhs.x, rhs.x, tolerance) && nearly_equal(lhs.y, rhs.y, tolerance) &&
           nearly_equal(lhs.z, rhs.z, tolerance) && nearly_equal(lhs.w, rhs.w, tolerance);
}

bool nearly_equal(Quat lhs, Quat rhs, float tolerance) noexcept {
    return nearly_equal(lhs.x, rhs.x, tolerance) && nearly_equal(lhs.y, rhs.y, tolerance) &&
           nearly_equal(lhs.z, rhs.z, tolerance) && nearly_equal(lhs.w, rhs.w, tolerance);
}

bool nearly_equal(const Mat4& lhs, const Mat4& rhs, float tolerance) noexcept {
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (!nearly_equal(lhs.at(row, column), rhs.at(row, column), tolerance)) {
                return false;
            }
        }
    }
    return true;
}

bool is_finite(float value) noexcept {
    return is_valid_scalar(value);
}

bool is_finite(Vec2 value) noexcept {
    return is_valid_vector(value);
}

bool is_finite(Vec3 value) noexcept {
    return is_valid_vector(value);
}

bool is_finite(Vec4 value) noexcept {
    return is_valid_vector(value);
}

bool is_finite(Quat value) noexcept {
    return is_valid_quat(value);
}

bool is_finite(const Mat4& value) noexcept {
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (!is_valid_scalar(value.at(row, column)))
                return false;
        }
    }
    return true;
}

bool try_decompose_affine_transform(const Mat4& matrix, Transform& result) noexcept {
    if (!is_finite(matrix))
        return false;
    Vec3 basis_x{matrix.at(0, 0), matrix.at(1, 0), matrix.at(2, 0)};
    Vec3 basis_y{matrix.at(0, 1), matrix.at(1, 1), matrix.at(2, 1)};
    Vec3 basis_z{matrix.at(0, 2), matrix.at(1, 2), matrix.at(2, 2)};
    float scale_x = length(basis_x);
    float scale_y = length(basis_y);
    float scale_z = length(basis_z);
    if (!is_finite(scale_x) || !is_finite(scale_y) || !is_finite(scale_z) || scale_x <= kEpsilon ||
        scale_y <= kEpsilon || scale_z <= kEpsilon) {
        return false;
    }
    basis_x /= scale_x;
    basis_y /= scale_y;
    basis_z /= scale_z;
    const float orthogonality =
        std::max({std::abs(dot(basis_x, basis_y)), std::abs(dot(basis_x, basis_z)), std::abs(dot(basis_y, basis_z))});
    if (!is_finite(orthogonality) || orthogonality > 2.0e-3f)
        return false;
    if (dot(cross(basis_x, basis_y), basis_z) < 0.0f) {
        scale_x = -scale_x;
        basis_x = -basis_x;
    }
    Mat4 rotation = identity();
    rotation.at(0, 0) = basis_x.x;
    rotation.at(1, 0) = basis_x.y;
    rotation.at(2, 0) = basis_x.z;
    rotation.at(0, 1) = basis_y.x;
    rotation.at(1, 1) = basis_y.y;
    rotation.at(2, 1) = basis_y.z;
    rotation.at(0, 2) = basis_z.x;
    rotation.at(1, 2) = basis_z.y;
    rotation.at(2, 2) = basis_z.z;
    result.translation = {matrix.at(0, 3), matrix.at(1, 3), matrix.at(2, 3)};
    const float m00 = rotation.at(0, 0);
    const float m01 = rotation.at(0, 1);
    const float m02 = rotation.at(0, 2);
    const float m10 = rotation.at(1, 0);
    const float m11 = rotation.at(1, 1);
    const float m12 = rotation.at(1, 2);
    const float m20 = rotation.at(2, 0);
    const float m21 = rotation.at(2, 1);
    const float m22 = rotation.at(2, 2);
    const float trace = m00 + m11 + m22;
    Quat quaternion{};
    if (trace > 0.0f) {
        const float scale = std::sqrt(trace + 1.0f) * 2.0f;
        quaternion.w = 0.25f * scale;
        quaternion.x = (m21 - m12) / scale;
        quaternion.y = (m02 - m20) / scale;
        quaternion.z = (m10 - m01) / scale;
    } else if (m00 > m11 && m00 > m22) {
        const float scale = std::sqrt(std::max(0.0f, 1.0f + m00 - m11 - m22)) * 2.0f;
        quaternion.w = (m21 - m12) / scale;
        quaternion.x = 0.25f * scale;
        quaternion.y = (m01 + m10) / scale;
        quaternion.z = (m02 + m20) / scale;
    } else if (m11 > m22) {
        const float scale = std::sqrt(std::max(0.0f, 1.0f + m11 - m00 - m22)) * 2.0f;
        quaternion.w = (m02 - m20) / scale;
        quaternion.x = (m01 + m10) / scale;
        quaternion.y = 0.25f * scale;
        quaternion.z = (m12 + m21) / scale;
    } else {
        const float scale = std::sqrt(std::max(0.0f, 1.0f + m22 - m00 - m11)) * 2.0f;
        quaternion.w = (m10 - m01) / scale;
        quaternion.x = (m02 + m20) / scale;
        quaternion.y = (m12 + m21) / scale;
        quaternion.z = 0.25f * scale;
    }
    result.rotation = normalize(quaternion);
    result.scale = {scale_x, scale_y, scale_z};
    return is_finite(result.translation) && is_finite(result.rotation) && is_finite(result.scale);
}

Mat4 Transform::to_matrix() const noexcept {
    return trs(translation, rotation, scale);
}

Vec3 Transform::transform_point(Vec3 point) const noexcept {
    return aeg::math::transform_point(to_matrix(), point);
}

Vec3 Transform::transform_vector(Vec3 vector) const noexcept {
    return aeg::math::transform_vector(to_matrix(), vector);
}
}  // namespace aeg::math
