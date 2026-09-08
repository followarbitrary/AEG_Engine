/**
 * @file world_serialization_smoke.cpp
 * @brief Exercises reflected ECS World archive serialization and restore semantics.
 */
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "world_serializer.hpp"

namespace {
struct SerializableComponent final {
    int32_t health{0};
    float position{0.0F};
    std::string name{};
};

struct MismatchedComponent final {
    int32_t value{0};
};

struct UnreflectedComponent final {
    int32_t value{0};
};

template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

bool register_serializable_schema(aeg::reflection::TypeRegistry& registry) {
    aeg::reflection::TypeBuilder builder(aeg::reflection::TypeDescriptor{
        aeg::reflection::TypeId("Game.SerializableComponent"), "Game.SerializableComponent", 1,
        aeg::reflection::TypeKind::kComponent, sizeof(SerializableComponent), alignof(SerializableComponent)});
    if (!builder.add_field("health", aeg::reflection::type_id<int32_t>(),
                           aeg::reflection::make_member_accessor(&SerializableComponent::health)) ||
        !builder.add_field("position", aeg::reflection::type_id<float>(),
                           aeg::reflection::make_member_accessor(&SerializableComponent::position)) ||
        !builder.add_field("name", aeg::reflection::type_id<std::string>(),
                           aeg::reflection::make_member_accessor(&SerializableComponent::name))) {
        return false;
    }
    auto descriptor = std::move(builder).finish();
    return descriptor && registry.register_type(std::move(descriptor.value));
}

bool register_mismatched_schema(aeg::reflection::TypeRegistry& registry) {
    aeg::reflection::TypeBuilder builder(aeg::reflection::TypeDescriptor{
        aeg::reflection::TypeId("Game.MismatchedComponent"), "Game.MismatchedComponent", 1,
        aeg::reflection::TypeKind::kComponent, sizeof(MismatchedComponent), alignof(MismatchedComponent)});
    if (!builder.add_field("value", aeg::reflection::type_id<int32_t>(),
                           aeg::reflection::make_member_accessor(&MismatchedComponent::value))) {
        return false;
    }
    auto descriptor = std::move(builder).finish();
    return descriptor && registry.register_type(std::move(descriptor.value));
}

aeg::foundation::ResultValue<aeg::ecs::WorldHandle> create_world(aeg::ecs::WorldManager& manager,
                                                                 aeg::ecs::WorldDesc desc) {
    return manager.create_world(desc);
}
}  // namespace

int main() {
    const aeg::ecs::ComponentTypeId component_id("Game.SerializableComponent");

    aeg::reflection::TypeRegistry source_registry;
    if (!require(register_serializable_schema(source_registry), "failed to register source component schema")) {
        return 1;
    }

    aeg::ecs::WorldManager source_manager;
    const auto source_handle = create_world(source_manager, {"SourceWorld", true});
    if (!require(source_handle, "failed to create source World")) {
        return 2;
    }
    const auto source_world = source_manager.find(source_handle.value);
    if (!require(source_world, "failed to find source World") ||
        !require(source_world.value->register_reflected_component<SerializableComponent>(
                     *source_registry.find_type(aeg::reflection::TypeId("Game.SerializableComponent")).value,
                     component_id, "SerializableComponent"),
                 "failed to register source reflected component")) {
        return 3;
    }

    const auto source_entity = source_world.value->create_entity();
    if (!require(source_entity, "failed to create source entity") ||
        !require(source_world.value->emplace<SerializableComponent>(source_entity.value,
                                                                    SerializableComponent{125, 12.5F, "Player"}),
                 "failed to add source component")) {
        return 4;
    }
    const auto source_snapshot = source_world.value->commit(42, 7);
    if (!require(source_snapshot, "failed to commit source World") ||
        !require(source_snapshot.value.component_type_count() == 1, "source snapshot component count is incorrect")) {
        return 5;
    }

    aeg::serialization::BinaryWriter writer;
    if (!require(aeg::world::WorldSerializer::serialize(source_snapshot.value, source_registry, writer),
                 "World archive serialization failed") ||
        !require(!writer.data().empty(), "World archive is empty")) {
        return 6;
    }

    aeg::reflection::TypeRegistry restore_registry;
    if (!require(register_serializable_schema(restore_registry), "failed to register restore schema")) {
        return 7;
    }
    aeg::ecs::WorldManager restore_manager;
    const auto restore_handle = create_world(restore_manager, {"RestoreWorld", true});
    const auto restore_world = restore_manager.find(restore_handle.value);
    if (!require(restore_handle && restore_world, "failed to create restore World") ||
        !require(restore_world.value->register_reflected_component<SerializableComponent>(
                     *restore_registry.find_type(aeg::reflection::TypeId("Game.SerializableComponent")).value,
                     component_id, "SerializableComponent"),
                 "failed to register restore reflected component")) {
        return 8;
    }

    const auto occupied_restore_entity = restore_world.value->create_entity();
    if (!require(occupied_restore_entity, "failed to create restore placeholder entity")) {
        return 9;
    }
    aeg::serialization::BinaryReader reader(writer.data());
    if (!require(aeg::world::WorldSerializer::deserialize(*restore_world.value, restore_registry, reader),
                 "World archive restore failed") ||
        !require(reader.remaining() == 0, "World archive restore left unread bytes") ||
        !require(restore_world.value->entity_count() == 1, "restored entity count is incorrect")) {
        return 10;
    }

    const auto restored_snapshot = restore_world.value->commit(42, 7);
    if (!require(restored_snapshot, "failed to commit restored World")) {
        return 11;
    }
    const aeg::ecs::EntityId restored_entity = restored_snapshot.value.entity_at(0);
    const SerializableComponent* restored_component = restore_world.value->get<SerializableComponent>(restored_entity);
    if (!require(restored_entity.is_valid() && restored_entity != source_entity.value,
                 "restored entity identity was not remapped") ||
        !require(restored_component != nullptr && restored_component->health == 125 &&
                     restored_component->position == 12.5F && restored_component->name == "Player",
                 "restored component value is incorrect")) {
        return 12;
    }

    const auto& archive_bytes = writer.data();
    aeg::serialization::BinaryReader truncated_reader(
        std::span<const uint8_t>(archive_bytes.data(), archive_bytes.size() - 1));
    const auto truncated_result =
        aeg::world::WorldSerializer::deserialize(*restore_world.value, restore_registry, truncated_reader);
    if (!require(!truncated_result, "truncated World archive was accepted") ||
        !require(restore_world.value->entity_count() == 1, "truncated World archive modified the target World")) {
        return 13;
    }

    aeg::reflection::TypeRegistry mismatch_registry;
    if (!require(register_serializable_schema(mismatch_registry), "failed to register archive schema for mismatch") ||
        !require(register_mismatched_schema(mismatch_registry), "failed to register mismatched schema")) {
        return 14;
    }
    aeg::ecs::WorldManager mismatch_manager;
    const auto mismatch_handle = create_world(mismatch_manager, {"MismatchWorld", true});
    const auto mismatch_world = mismatch_manager.find(mismatch_handle.value);
    if (!require(mismatch_handle && mismatch_world, "failed to create mismatch World") ||
        !require(mismatch_world.value->register_reflected_component<MismatchedComponent>(
                     *mismatch_registry.find_type(aeg::reflection::TypeId("Game.MismatchedComponent")).value,
                     component_id, "MismatchedComponent"),
                 "failed to register mismatched component")) {
        return 15;
    }
    if (!require(mismatch_world.value->create_entity(), "failed to create mismatch placeholder entity")) {
        return 16;
    }
    aeg::serialization::BinaryReader mismatch_reader(writer.data());
    const auto mismatch_result =
        aeg::world::WorldSerializer::deserialize(*mismatch_world.value, mismatch_registry, mismatch_reader);
    if (!require(!mismatch_result, "schema-mismatched World archive was accepted") ||
        !require(mismatch_world.value->entity_count() == 0, "failed World restore did not clear the target World")) {
        return 17;
    }

    aeg::reflection::TypeRegistry unreflected_registry;
    aeg::ecs::WorldManager unreflected_manager;
    const auto unreflected_handle = create_world(unreflected_manager, {"UnreflectedWorld", true});
    const auto unreflected_world = unreflected_manager.find(unreflected_handle.value);
    if (!require(unreflected_handle && unreflected_world, "failed to create unreflected World") ||
        !require(unreflected_world.value->register_component<UnreflectedComponent>(
                     aeg::ecs::ComponentTypeId("Game.UnreflectedComponent"), "UnreflectedComponent"),
                 "failed to register unreflected component")) {
        return 18;
    }
    const auto unreflected_entity = unreflected_world.value->create_entity();
    if (!require(unreflected_entity, "failed to create unreflected entity") ||
        !require(
            unreflected_world.value->emplace<UnreflectedComponent>(unreflected_entity.value, UnreflectedComponent{1}),
            "failed to add unreflected component")) {
        return 19;
    }
    const auto unreflected_snapshot = unreflected_world.value->commit();
    aeg::serialization::BinaryWriter unreflected_writer;
    if (!require(unreflected_writer.write_u32(0xAABBCCDDu), "failed to seed output writer")) {
        return 20;
    }
    const std::vector<uint8_t> original_writer_data(unreflected_writer.data().begin(), unreflected_writer.data().end());
    const auto unreflected_result =
        aeg::world::WorldSerializer::serialize(unreflected_snapshot.value, unreflected_registry, unreflected_writer);
    if (!require(!unreflected_result, "unreflected component was serialized") ||
        !require(unreflected_writer.data().size() == original_writer_data.size() &&
                     std::equal(unreflected_writer.data().begin(), unreflected_writer.data().end(),
                                original_writer_data.begin()),
                 "failed archive polluted the output writer")) {
        return 20;
    }

    return 0;
}
