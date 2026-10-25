/** @file world_transform_schema.cpp
 *  @brief The single Transform reflection schema shared by Runtime and the value protocol.
 */
#include <string>
#include <utility>

#include "Components/component_ids.hpp"
#include "Components/component_reflection.hpp"
#include "Components/render_components.hpp"
#include "feature_world_transform.hpp"

namespace aeg::engine::components {
const std::string_view kTransformName{feature::kWorldTransformComponentName};
const std::string_view kVec3TypeName{"Math.Vec3"};
const std::string_view kQuatTypeName{"Math.Quat"};
const uint32_t kRenderComponentSchemaVersion = 1;

namespace {
template <typename Owner>
[[nodiscard]] reflection::FieldAccessor make_vec3_accessor(math::Vec3 Owner::* member) {
    reflection::FieldAccessor accessor = reflection::make_member_accessor(member);
    accessor.value_kind = reflection::FieldValueKind::kVec3;
    return accessor;
}

template <typename Owner>
[[nodiscard]] reflection::FieldAccessor make_quat_accessor(math::Quat Owner::* member) {
    reflection::FieldAccessor accessor = reflection::make_member_accessor(member);
    accessor.value_kind = reflection::FieldValueKind::kQuat;
    return accessor;
}
}  // namespace

foundation::ResultValue<reflection::TypeDescriptor> transform_reflection_descriptor() {
    reflection::TypeBuilder builder(reflection::TypeDescriptor{
        reflection::TypeId(kTransformName), std::string(kTransformName), kRenderComponentSchemaVersion,
        reflection::TypeKind::kComponent, sizeof(Transform), alignof(Transform)});
    if (const foundation::Result result = builder.add_field("translation", reflection::TypeId(kVec3TypeName),
                                                            make_vec3_accessor(&Transform::translation));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("rotation", reflection::TypeId(kQuatTypeName), make_quat_accessor(&Transform::rotation));
        !result) {
        return {result, {}};
    }
    if (const foundation::Result result =
            builder.add_field("scale", reflection::TypeId(kVec3TypeName), make_vec3_accessor(&Transform::scale));
        !result) {
        return {result, {}};
    }
    return std::move(builder).finish();
}
}  // namespace aeg::engine::components
