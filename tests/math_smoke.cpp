/**
 * @file math_smoke.cpp
 * @brief Exercises the public vector, quaternion, matrix, and transform API.
 */
#include <cmath>
#include <iostream>
#include <math.hpp>

namespace {
using aeg::math::ClipSpaceConvention;
using aeg::math::Frustum;
using aeg::math::FrustumPlane;
using aeg::math::Mat4;
using aeg::math::Quat;
using aeg::math::Transform;
using aeg::math::Vec3;
using aeg::math::Vec4;

constexpr float kTolerance = 1.0e-4f;

[[nodiscard]] bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const Mat4 identity = aeg::math::identity();
    if (!check(
            aeg::math::nearly_equal(identity * Vec4{1.0f, 2.0f, 3.0f, 1.0f}, Vec4{1.0f, 2.0f, 3.0f, 1.0f}, kTolerance),
            "identity transform failed")) {
        return 1;
    }

    const Vec3 axis{0.0f, 0.0f, 1.0f};
    const Quat quarter_turn = aeg::math::quat_from_axis_angle(axis, aeg::math::kPi * 0.5f);
    if (!check(aeg::math::nearly_equal(aeg::math::rotate(quarter_turn, Vec3{1.0f, 0.0f, 0.0f}), Vec3{0.0f, 1.0f, 0.0f},
                                       kTolerance),
               "quaternion rotation failed")) {
        return 2;
    }

    const Transform transform{Vec3{2.0f, 3.0f, 4.0f}, quarter_turn, Vec3{2.0f, 3.0f, 4.0f}};
    if (!check(aeg::math::nearly_equal(transform.transform_point(Vec3{1.0f, 0.0f, 0.0f}), Vec3{2.0f, 5.0f, 4.0f},
                                       kTolerance),
               "TRS point transform failed") ||
        !check(aeg::math::nearly_equal(transform.transform_vector(Vec3{1.0f, 0.0f, 0.0f}), Vec3{0.0f, 2.0f, 0.0f},
                                       kTolerance),
               "TRS vector transform failed")) {
        return 3;
    }

    const Mat4 transform_matrix = transform.to_matrix();
    Mat4 inverse_matrix{};
    if (!check(aeg::math::try_inverse(transform_matrix, inverse_matrix), "matrix inverse failed") ||
        !check(aeg::math::nearly_equal(transform_matrix * inverse_matrix, identity, kTolerance),
               "matrix inverse result failed")) {
        return 4;
    }

    const Mat4 view = aeg::math::look_at(Vec3{0.0f, 0.0f, 3.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 projection =
        aeg::math::perspective(aeg::math::kPi / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f, ClipSpaceConvention{false, false});
    Vec3 projected_point{};
    if (!check(aeg::math::try_transform_point(projection * view, Vec3{0.0f, 0.0f, 0.0f}, projected_point),
               "camera projection failed") ||
        !check(projected_point.z >= 0.0f && projected_point.z <= 1.0f, "depth range is invalid")) {
        return 5;
    }

    const Mat4 orthographic =
        aeg::math::orthographic(-2.0f, 6.0f, -1.0f, 3.0f, 0.1f, 10.0f, ClipSpaceConvention{true, false});
    Vec3 orthographic_point{};
    if (!check(aeg::math::try_transform_point(orthographic, Vec3{2.0f, 1.0f, -0.1f}, orthographic_point),
               "orthographic projection failed") ||
        !check(aeg::math::nearly_equal(orthographic_point, Vec3{0.0f, 0.0f, 0.0f}, kTolerance),
               "orthographic center mapping failed")) {
        return 6;
    }

    if (!check(aeg::math::normalize(Vec3{}) == Vec3{}, "zero vector normalization policy failed")) {
        return 7;
    }

    Frustum frustum{};
    if (!check(aeg::math::try_extract_frustum(projection * view, frustum), "frustum extraction failed") ||
        !check(frustum.is_valid(), "frustum validity failed") ||
        !check(frustum.contains_point(Vec3{0.0f, 0.0f, -1.0f}), "frustum point inclusion failed") ||
        !check(!frustum.contains_point(Vec3{0.0f, 0.0f, 2.95f}), "frustum near rejection failed") ||
        !check(!frustum.contains_point(Vec3{100.0f, 0.0f, -1.0f}), "frustum side rejection failed") ||
        !check(frustum.intersects_sphere(Vec3{0.0f, 0.0f, 2.95f}, 0.1f), "frustum sphere intersection failed") ||
        !check(frustum[FrustumPlane::kNear].is_valid(), "frustum plane indexing failed")) {
        return 8;
    }

    const ClipSpaceConvention reversed_z{false, true};
    const Mat4 reversed_projection =
        aeg::math::perspective(aeg::math::kPi / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f, reversed_z);
    Frustum reversed_frustum{};
    if (!check(aeg::math::try_extract_frustum(reversed_projection * view, reversed_frustum, reversed_z),
               "reversed-z frustum extraction failed") ||
        !check(reversed_frustum.contains_point(Vec3{0.0f, 0.0f, -1.0f}), "reversed-z frustum inclusion failed") ||
        !check(!reversed_frustum.contains_point(Vec3{0.0f, 0.0f, 2.95f}), "reversed-z frustum near rejection failed")) {
        return 9;
    }

    const ClipSpaceConvention y_down{true, false};
    const Mat4 y_down_projection = aeg::math::perspective(aeg::math::kPi / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f, y_down);
    Frustum y_down_frustum{};
    if (!check(aeg::math::try_extract_frustum(y_down_projection * view, y_down_frustum, y_down),
               "y-down frustum extraction failed") ||
        !check(y_down_frustum.contains_point(Vec3{0.0f, 0.0f, -1.0f}), "y-down frustum inclusion failed") ||
        !check(y_down_frustum[FrustumPlane::kTop].normal().y < 0.0f, "y-down top plane semantic failed")) {
        return 10;
    }

    std::cout << "AEG math smoke passed\n";
    return 0;
}
