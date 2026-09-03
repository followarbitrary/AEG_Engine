/**
 * @file math.hpp
 * @brief Public SIMD-independent vector, quaternion, matrix, and transform API.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace aeg::math {
/**
 * AEG Math semantic contract
 *
 * Coordinate system:
 * - Right-handed world and view coordinates.
 * - +X is right, +Y is up, and the camera looks along -Z.
 * - Angles are expressed in radians. Positive rotations follow the
 *   right-hand rule.
 *
 * Matrix system:
 * - Mat4 is a column-major matrix made of four column vectors.
 * - Vectors are column vectors and are multiplied on the right of a matrix.
 * - The transform chain is clip = projection * view * model * local.
 * - trs() applies scale first, rotation second, and translation last.
 * - Mat4{} is a zero matrix; use identity() when an identity matrix is needed.
 *
 * Camera and clip space:
 * - perspective() and orthographic() use a 0..1 NDC depth range.
 * - near_plane and far_plane are positive distances; visible camera-space
 *   positions have Z in the range [-far_plane, -near_plane].
 * - ClipSpaceConvention::y_down makes positive NDC Y point down.
 * - ClipSpaceConvention::reversed_z maps the near plane to 1 and the far
 *   plane to 0. The default maps near to 0 and far to 1.
 *
 * Failure and comparison rules:
 * - try_* functions return false and leave their output argument unchanged
 *   when the input is invalid or the operation cannot be completed.
 * - Value-returning failure wrappers return a zero vector or zero matrix.
 *   normalize(), inverse(), and axis-angle construction use the identity
 *   quaternion (0, 0, 0, 1) as their quaternion fallback.
 * - operator== performs exact component comparison. Use nearly_equal() for
 *   finite-value comparisons with an absolute tolerance.
 * - kEpsilon is the default numerical threshold; it is not a unit conversion.
 *
 * ABI and SIMD rules:
 * - Vec3 is 16-byte aligned and occupies 16 bytes. Its fourth float is
 *   padding, not a semantic component; do not serialize it as a packed 3-float
 *   structure.
 * - Mat4 is suitable for direct upload only when the shader uses the same
 *   column-major convention. No implicit transpose is performed.
 * - SIMD is an implementation detail. The public API does not expose SIMD
 *   types and must preserve the same semantics with SSE2 or scalar fallback;
 *   minor floating-point rounding differences are possible.
 */

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kEpsilon = 1.0e-6f;

/**
 * @brief Two-dimensional floating-point value type.
 *
 * Components are ordered x, y. Arithmetic operators are component-wise unless
 * a scalar operand is explicitly present.
 */
struct Vec2 {
    float x{0.0f};
    float y{0.0f};

    constexpr Vec2() = default;
    constexpr Vec2(float x_value, float y_value) : x(x_value), y(y_value) {}

    [[nodiscard]] float& operator[](std::size_t index) noexcept;
    [[nodiscard]] const float& operator[](std::size_t index) const noexcept;
};

/**
 * @brief 16-byte-aligned three-dimensional floating-point value type.
 *
 * The private fourth float exists only for alignment and is not a semantic
 * component. Do not serialize Vec3 as a packed three-float object.
 */
struct alignas(16) Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    constexpr Vec3() = default;
    constexpr Vec3(float x_value, float y_value, float z_value) : x(x_value), y(y_value), z(z_value) {}

    [[nodiscard]] float& operator[](std::size_t index) noexcept;
    [[nodiscard]] const float& operator[](std::size_t index) const noexcept;

private:
    float padding_{0.0f};
};

/**
 * @brief Four-dimensional floating-point value type.
 *
 * Components are ordered x, y, z, w. It is suitable for homogeneous vectors
 * and direct upload to a matching four-float shader value.
 */
struct alignas(16) Vec4 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{0.0f};

    constexpr Vec4() = default;
    constexpr Vec4(float x_value, float y_value, float z_value, float w_value)
        : x(x_value), y(y_value), z(z_value), w(w_value) {}

    [[nodiscard]] float& operator[](std::size_t index) noexcept;
    [[nodiscard]] const float& operator[](std::size_t index) const noexcept;
};

/**
 * @brief Unit-quaternion value type used for 3D rotation.
 *
 * Components are stored in x, y, z, w order. The default value is the identity
 * rotation (0, 0, 0, 1). Rotation APIs expect radians and normalized input is
 * produced by normalize() when required.
 */
struct alignas(16) Quat {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};

    constexpr Quat() = default;
    constexpr Quat(float x_value, float y_value, float z_value, float w_value)
        : x(x_value), y(y_value), z(z_value), w(w_value) {}
};

/**
 * @brief Column-major 4x4 matrix.
 *
 * The matrix stores four column vectors. at(row, column) is the canonical
 * element accessor; operator[] selects a column.
 */
struct alignas(16) Mat4 {
    std::array<Vec4, 4> columns{};

    [[nodiscard]] Vec4& operator[](std::size_t column) noexcept;
    [[nodiscard]] const Vec4& operator[](std::size_t column) const noexcept;

    [[nodiscard]] float& at(std::size_t row, std::size_t column) noexcept;
    [[nodiscard]] const float& at(std::size_t row, std::size_t column) const noexcept;
};

struct ClipSpaceConvention {
    bool y_down{false};

    bool reversed_z{false};
};

/**
 * @brief Affine transform represented by translation, rotation, and scale.
 *
 * Its matrix form is translation * rotation * scale. The default scale is one
 * on every axis and the default rotation is identity.
 */
struct Transform {
    Vec3 translation{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] Mat4 to_matrix() const noexcept;
    [[nodiscard]] Vec3 transform_point(Vec3 point) const noexcept;
    [[nodiscard]] Vec3 transform_vector(Vec3 vector) const noexcept;
};

static_assert(sizeof(Vec2) == sizeof(float) * 2);
static_assert(sizeof(Vec3) == sizeof(float) * 4);
static_assert(sizeof(Vec4) == sizeof(float) * 4);
static_assert(sizeof(Quat) == sizeof(float) * 4);
static_assert(sizeof(Mat4) == sizeof(float) * 16);

// Vector operations use component-wise arithmetic. Division does not apply a
// special zero-divisor policy and follows the platform floating-point rules.
[[nodiscard]] Vec2 operator+(Vec2 lhs, Vec2 rhs) noexcept;
[[nodiscard]] Vec2 operator-(Vec2 lhs, Vec2 rhs) noexcept;
[[nodiscard]] Vec2 operator-(Vec2 value) noexcept;
[[nodiscard]] Vec2 operator*(Vec2 value, float scalar) noexcept;
[[nodiscard]] Vec2 operator*(float scalar, Vec2 value) noexcept;
[[nodiscard]] Vec2 operator/(Vec2 value, float scalar) noexcept;
Vec2& operator+=(Vec2& lhs, Vec2 rhs) noexcept;
Vec2& operator-=(Vec2& lhs, Vec2 rhs) noexcept;
Vec2& operator*=(Vec2& value, float scalar) noexcept;
Vec2& operator/=(Vec2& value, float scalar) noexcept;
[[nodiscard]] bool operator==(Vec2 lhs, Vec2 rhs) noexcept;

[[nodiscard]] Vec3 operator+(Vec3 lhs, Vec3 rhs) noexcept;
[[nodiscard]] Vec3 operator-(Vec3 lhs, Vec3 rhs) noexcept;
[[nodiscard]] Vec3 operator-(Vec3 value) noexcept;
[[nodiscard]] Vec3 operator*(Vec3 value, float scalar) noexcept;
[[nodiscard]] Vec3 operator*(float scalar, Vec3 value) noexcept;
[[nodiscard]] Vec3 operator/(Vec3 value, float scalar) noexcept;
Vec3& operator+=(Vec3& lhs, Vec3 rhs) noexcept;
Vec3& operator-=(Vec3& lhs, Vec3 rhs) noexcept;
Vec3& operator*=(Vec3& value, float scalar) noexcept;
Vec3& operator/=(Vec3& value, float scalar) noexcept;
[[nodiscard]] bool operator==(Vec3 lhs, Vec3 rhs) noexcept;

[[nodiscard]] Vec4 operator+(Vec4 lhs, Vec4 rhs) noexcept;
[[nodiscard]] Vec4 operator-(Vec4 lhs, Vec4 rhs) noexcept;
[[nodiscard]] Vec4 operator-(Vec4 value) noexcept;
[[nodiscard]] Vec4 operator*(Vec4 value, float scalar) noexcept;
[[nodiscard]] Vec4 operator*(float scalar, Vec4 value) noexcept;
[[nodiscard]] Vec4 operator/(Vec4 value, float scalar) noexcept;
Vec4& operator+=(Vec4& lhs, Vec4 rhs) noexcept;
Vec4& operator-=(Vec4& lhs, Vec4 rhs) noexcept;
Vec4& operator*=(Vec4& value, float scalar) noexcept;
Vec4& operator/=(Vec4& value, float scalar) noexcept;
[[nodiscard]] bool operator==(Vec4 lhs, Vec4 rhs) noexcept;

// dot(), cross(), and length functions use the semantic components only.
// normalize() returns a zero vector for invalid or near-zero vector input.
// The try_normalize() output is unchanged when it returns false.
[[nodiscard]] float dot(Vec2 lhs, Vec2 rhs) noexcept;
[[nodiscard]] float dot(Vec3 lhs, Vec3 rhs) noexcept;
[[nodiscard]] float dot(Vec4 lhs, Vec4 rhs) noexcept;
[[nodiscard]] Vec3 cross(Vec3 lhs, Vec3 rhs) noexcept;
[[nodiscard]] float length_squared(Vec2 value) noexcept;
[[nodiscard]] float length_squared(Vec3 value) noexcept;
[[nodiscard]] float length_squared(Vec4 value) noexcept;
[[nodiscard]] float length(Vec2 value) noexcept;
[[nodiscard]] float length(Vec3 value) noexcept;
[[nodiscard]] float length(Vec4 value) noexcept;
[[nodiscard]] Vec2 normalize(Vec2 value) noexcept;
[[nodiscard]] Vec3 normalize(Vec3 value) noexcept;
[[nodiscard]] Vec4 normalize(Vec4 value) noexcept;
[[nodiscard]] bool try_normalize(Vec2 value, Vec2& result) noexcept;
[[nodiscard]] bool try_normalize(Vec3 value, Vec3& result) noexcept;
[[nodiscard]] bool try_normalize(Vec4 value, Vec4& result) noexcept;

[[nodiscard]] Quat operator*(Quat lhs, Quat rhs) noexcept;
[[nodiscard]] bool operator==(Quat lhs, Quat rhs) noexcept;
[[nodiscard]] float length_squared(Quat value) noexcept;
[[nodiscard]] Quat normalize(Quat value) noexcept;
[[nodiscard]] Quat conjugate(Quat value) noexcept;
[[nodiscard]] Quat inverse(Quat value) noexcept;
// Quaternion multiplication follows Hamilton-product order: lhs * rhs applies
// rhs first and lhs second when used to rotate a vector.
// Axis is normalized internally. Invalid axes or angles return identity.
[[nodiscard]] Quat quat_from_axis_angle(Vec3 axis, float radians) noexcept;
[[nodiscard]] Vec3 rotate(Quat rotation, Vec3 value) noexcept;

// Matrix multiplication composes transforms from right to left. For example,
// (a * b) * v applies b to v first and a second.
[[nodiscard]] Mat4 operator*(const Mat4& lhs, const Mat4& rhs) noexcept;
[[nodiscard]] Vec4 operator*(const Mat4& matrix, Vec4 value) noexcept;
[[nodiscard]] bool operator==(const Mat4& lhs, const Mat4& rhs) noexcept;

[[nodiscard]] Mat4 identity() noexcept;
[[nodiscard]] Mat4 translation(Vec3 value) noexcept;
[[nodiscard]] Mat4 scale(Vec3 value) noexcept;
[[nodiscard]] Mat4 rotation_x(float radians) noexcept;
[[nodiscard]] Mat4 rotation_y(float radians) noexcept;
[[nodiscard]] Mat4 rotation_z(float radians) noexcept;
[[nodiscard]] Mat4 from_quat(Quat rotation) noexcept;
// trs() is equivalent to translation(translation_value) * from_quat(rotation)
// * scale(scale_value).
[[nodiscard]] Mat4 trs(Vec3 translation_value, Quat rotation, Vec3 scale_value) noexcept;

// Projection matrices assume a right-handed camera looking down -Z and map
// depth to the 0..1 NDC range. try_* leaves result unchanged on failure;
// non-try wrappers return a zero matrix on failure.
[[nodiscard]] bool try_perspective(float vertical_fov_radians, float aspect_ratio, float near_plane,
                                   float far_plane, Mat4& result,
                                   ClipSpaceConvention convention = {}) noexcept;
[[nodiscard]] Mat4 perspective(float vertical_fov_radians, float aspect_ratio, float near_plane,
                               float far_plane, ClipSpaceConvention convention = {}) noexcept;
[[nodiscard]] bool try_orthographic(float left, float right, float bottom, float top, float near_plane,
                                    float far_plane, Mat4& result,
                                    ClipSpaceConvention convention = {}) noexcept;
[[nodiscard]] Mat4 orthographic(float left, float right, float bottom, float top, float near_plane,
                                float far_plane, ClipSpaceConvention convention = {}) noexcept;
[[nodiscard]] bool try_look_at(Vec3 eye, Vec3 target, Vec3 up, Mat4& result) noexcept;
[[nodiscard]] Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept;
// try_inverse() fails for non-finite or numerically singular matrices and
// leaves result unchanged in that case.
[[nodiscard]] bool try_inverse(const Mat4& matrix, Mat4& result) noexcept;
[[nodiscard]] Mat4 inverse(const Mat4& matrix) noexcept;

// Point transforms use homogeneous w=1 and perform a perspective divide.
// Vector transforms use w=0, so translation is ignored. The try_* point form
// leaves result unchanged when the homogeneous result is invalid.
[[nodiscard]] Vec3 transform_point(const Mat4& matrix, Vec3 point) noexcept;
[[nodiscard]] Vec3 transform_vector(const Mat4& matrix, Vec3 vector) noexcept;
[[nodiscard]] bool try_transform_point(const Mat4& matrix, Vec3 point, Vec3& result) noexcept;
[[nodiscard]] bool nearly_equal(float lhs, float rhs, float tolerance = kEpsilon) noexcept;
[[nodiscard]] bool nearly_equal(Vec3 lhs, Vec3 rhs, float tolerance = kEpsilon) noexcept;
[[nodiscard]] bool nearly_equal(Vec4 lhs, Vec4 rhs, float tolerance = kEpsilon) noexcept;
[[nodiscard]] bool nearly_equal(Quat lhs, Quat rhs, float tolerance = kEpsilon) noexcept;
[[nodiscard]] bool nearly_equal(const Mat4& lhs, const Mat4& rhs, float tolerance = kEpsilon) noexcept;
}  // namespace aeg::math
