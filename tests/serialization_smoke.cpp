/**
 * @file serialization_smoke.cpp
 * @brief Exercises deterministic reflection-driven object serialization.
 */
#include <cstdint>
#include <iostream>
#include <string>

#include "serialization.hpp"

namespace {
struct Vec2 final {
    float x{0.0F};
    float y{0.0F};
};

struct Actor final {
    int32_t health{0};
    std::string name{};
    Vec2 position{};
};

template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

bool register_vec2(aeg::reflection::TypeRegistry& registry) {
    aeg::reflection::TypeBuilder builder(
        aeg::reflection::TypeDescriptor{aeg::reflection::TypeId("Game.Vec2"), "Game.Vec2", 1,
                                        aeg::reflection::TypeKind::kStruct, sizeof(Vec2), alignof(Vec2)});
    if (!builder.add_field("x", aeg::reflection::type_id<float>(), aeg::reflection::make_member_accessor(&Vec2::x)) ||
        !builder.add_field("y", aeg::reflection::type_id<float>(), aeg::reflection::make_member_accessor(&Vec2::y))) {
        return false;
    }
    auto descriptor = std::move(builder).finish();
    return descriptor && registry.register_type(std::move(descriptor.value));
}

bool register_actor(aeg::reflection::TypeRegistry& registry) {
    aeg::reflection::TypeBuilder builder(
        aeg::reflection::TypeDescriptor{aeg::reflection::TypeId("Game.Actor"), "Game.Actor", 1,
                                        aeg::reflection::TypeKind::kStruct, sizeof(Actor), alignof(Actor)});
    if (!builder.add_field("health", aeg::reflection::type_id<int32_t>(),
                           aeg::reflection::make_member_accessor(&Actor::health)) ||
        !builder.add_field("name", aeg::reflection::type_id<std::string>(),
                           aeg::reflection::make_member_accessor(&Actor::name)) ||
        !builder.add_field("position", aeg::reflection::TypeId("Game.Vec2"),
                           aeg::reflection::make_member_accessor(&Actor::position))) {
        return false;
    }
    auto descriptor = std::move(builder).finish();
    return descriptor && registry.register_type(std::move(descriptor.value));
}
}  // namespace

int main() {
    aeg::reflection::TypeRegistry registry;
    if (!require(register_vec2(registry), "failed to register Vec2") ||
        !require(register_actor(registry), "failed to register Actor")) {
        return 1;
    }

    const Actor source{125, "Player", Vec2{12.5F, -3.0F}};
    aeg::serialization::BinaryWriter writer;
    const aeg::foundation::Result write_result =
        aeg::serialization::serialize_object(registry, aeg::reflection::TypeId("Game.Actor"), &source, writer);
    if (!require(write_result, "object serialization failed") ||
        !require(!writer.data().empty(), "serialization produced no bytes")) {
        return 1;
    }

    Actor restored{0, "", Vec2{0.0F, 0.0F}};
    aeg::serialization::BinaryReader reader(writer.data());
    const aeg::foundation::Result read_result =
        aeg::serialization::deserialize_object(registry, aeg::reflection::TypeId("Game.Actor"), reader, &restored);
    if (!require(read_result, "object deserialization failed") ||
        !require(restored.health == source.health, "deserialized integer field is incorrect") ||
        !require(restored.name == source.name, "deserialized string field is incorrect") ||
        !require(restored.position.x == source.position.x && restored.position.y == source.position.y,
                 "deserialized nested field is incorrect") ||
        !require(reader.remaining() == 0, "deserializer left unread bytes")) {
        return 1;
    }

    const auto& bytes = writer.data();
    aeg::serialization::BinaryReader truncated_reader(std::span<const uint8_t>(bytes.data(), bytes.size() - 1));
    Actor truncated_target{};
    const auto truncated_result = aeg::serialization::deserialize_object(
        registry, aeg::reflection::TypeId("Game.Actor"), truncated_reader, &truncated_target);
    if (!require(!truncated_result, "truncated input was accepted")) {
        return 1;
    }

    return 0;
}
