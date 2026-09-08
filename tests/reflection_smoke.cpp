/**
 * @file reflection_smoke.cpp
 * @brief Exercises reflected type registration and field access.
 */
#include <cstdint>
#include <iostream>
#include <string>

#include "reflection.hpp"

namespace {
struct Transform final {
    float x{0.0F};
    float y{0.0F};
    std::string name{};
};

template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}
}  // namespace

int main() {
    aeg::reflection::TypeRegistry registry;
    aeg::reflection::TypeBuilder builder(
        aeg::reflection::TypeDescriptor{aeg::reflection::TypeId("Game.Transform"), "Game.Transform", 1,
                                        aeg::reflection::TypeKind::kStruct, sizeof(Transform), alignof(Transform)});

    if (!require(builder.add_field("x", aeg::reflection::type_id<float>(),
                                   aeg::reflection::make_member_accessor(&Transform::x)),
                 "failed to add x field") ||
        !require(builder.add_field("y", aeg::reflection::type_id<float>(),
                                   aeg::reflection::make_member_accessor(&Transform::y)),
                 "failed to add y field") ||
        !require(builder.add_field("name", aeg::reflection::type_id<std::string>(),
                                   aeg::reflection::make_member_accessor(&Transform::name)),
                 "failed to add name field")) {
        return 1;
    }

    const auto descriptor = std::move(builder).finish();
    if (!require(descriptor, "failed to finish reflected descriptor") ||
        !require(registry.register_type(std::move(descriptor.value)), "failed to register reflected type")) {
        return 1;
    }

    const aeg::reflection::TypeId type_id("Game.Transform");
    const auto type = registry.find_type(type_id);
    if (!require(type, "registered type cannot be found") ||
        !require(type.value->fields.size() == 3, "reflected field count is incorrect")) {
        return 1;
    }

    const auto field = registry.find_field(type_id, aeg::reflection::FieldId("x"));
    if (!require(field, "registered field cannot be found")) {
        return 1;
    }

    Transform value{};
    void* mutable_value = field.value.accessor.mutable_access(&value);
    if (!require(mutable_value != nullptr, "mutable field accessor is invalid")) {
        return 1;
    }
    *static_cast<float*>(mutable_value) = 42.0F;
    if (!require(value.x == 42.0F, "mutable field accessor did not update the object")) {
        return 1;
    }

    const auto duplicate = registry.register_type(*type.value);
    if (!require(!duplicate && duplicate.code == aeg::foundation::ResultCode::kInvalidState,
                 "duplicate type registration was accepted")) {
        return 1;
    }

    if (!require(registry.unregister_type(type_id), "registered type could not be removed") ||
        !require(!registry.contains(type_id), "removed type is still present")) {
        return 1;
    }

    return 0;
}
