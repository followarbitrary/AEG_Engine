/**
 * @file scene_asset_smoke.cpp
 * @brief Exercises Scene Asset validation, deterministic archives, and World instantiation.
 */
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <utility>

#include "scene_asset.hpp"

namespace {
struct SceneComponent final {
    int32_t health{0};
    float position{0.0F};
    std::string label{};
};

struct MismatchedComponent final {
    int32_t value{0};
};

template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

bool register_scene_schema(aeg::reflection::TypeRegistry& registry, aeg::reflection::TypeId type_id, const char* name) {
    aeg::reflection::TypeBuilder builder(aeg::reflection::TypeDescriptor{
        type_id, name, 1, aeg::reflection::TypeKind::kComponent, sizeof(SceneComponent), alignof(SceneComponent)});
    if (!builder.add_field("health", aeg::reflection::type_id<int32_t>(),
                           aeg::reflection::make_member_accessor(&SceneComponent::health)) ||
        !builder.add_field("position", aeg::reflection::type_id<float>(),
                           aeg::reflection::make_member_accessor(&SceneComponent::position)) ||
        !builder.add_field("label", aeg::reflection::type_id<std::string>(),
                           aeg::reflection::make_member_accessor(&SceneComponent::label))) {
        return false;
    }
    auto descriptor = std::move(builder).finish();
    return descriptor && registry.register_type(std::move(descriptor.value));
}

bool register_mismatched_schema(aeg::reflection::TypeRegistry& registry, aeg::reflection::TypeId type_id,
                                const char* name) {
    aeg::reflection::TypeBuilder builder(
        aeg::reflection::TypeDescriptor{type_id, name, 1, aeg::reflection::TypeKind::kComponent,
                                        sizeof(MismatchedComponent), alignof(MismatchedComponent)});
    if (!builder.add_field("value", aeg::reflection::type_id<int32_t>(),
                           aeg::reflection::make_member_accessor(&MismatchedComponent::value))) {
        return false;
    }
    auto descriptor = std::move(builder).finish();
    return descriptor && registry.register_type(std::move(descriptor.value));
}

bool add_entity(aeg::scene::SceneAsset& asset, aeg::scene::SceneEntityRecord entity) {
    return static_cast<bool>(asset.add_entity(std::move(entity)));
}
}  // namespace

int main() {
    constexpr aeg::scene::SceneAssetId kAssetId{0x1111222233334444ull, 0x5555666677778888ull};
    constexpr aeg::scene::SceneEntityId kRootId{0xaaaabbbbcccc0001ull, 100ull};
    constexpr aeg::scene::SceneEntityId kChildId{0xaaaabbbbcccc0002ull, 200ull};
    constexpr aeg::ecs::ComponentTypeId kComponentId{"Game.SceneComponent"};
    const aeg::reflection::TypeId schema_id("Game.SceneComponent");

    aeg::reflection::TypeRegistry registry;
    if (!require(register_scene_schema(registry, schema_id, "Game.SceneComponent"),
                 "failed to register Scene component schema") ||
        !require(register_mismatched_schema(registry, aeg::reflection::TypeId("Game.MismatchedComponent"),
                                            "Game.MismatchedComponent"),
                 "failed to register mismatched component schema")) {
        return 1;
    }

    aeg::scene::SceneAsset asset(kAssetId, "TestScene", 7);
    if (!require(add_entity(asset, {kRootId, {}, "Root", aeg::scene::SceneEntityFlags::kNone}),
                 "failed to add Scene root") ||
        !require(add_entity(asset, {kChildId, kRootId, "Child",
                                    aeg::scene::SceneEntityFlags::kHidden | aeg::scene::SceneEntityFlags::kLocked}),
                 "failed to add Scene child")) {
        return 2;
    }

    SceneComponent source_component{125, 12.5F, "Player"};
    if (!require(asset.add_reflected_component(kChildId, kComponentId, registry, schema_id, &source_component),
                 "failed to add reflected Scene component") ||
        !require(asset.validate(), "valid Scene Asset was rejected")) {
        return 3;
    }

    aeg::scene::SceneAsset reordered_asset(kAssetId, "TestScene", 7);
    if (!require(add_entity(reordered_asset,
                            {kChildId, kRootId, "Child",
                             aeg::scene::SceneEntityFlags::kHidden | aeg::scene::SceneEntityFlags::kLocked}),
                 "failed to add reordered Scene child") ||
        !require(add_entity(reordered_asset, {kRootId, {}, "Root", aeg::scene::SceneEntityFlags::kNone}),
                 "failed to add reordered Scene root") ||
        !require(
            reordered_asset.add_reflected_component(kChildId, kComponentId, registry, schema_id, &source_component),
            "failed to add reordered reflected component")) {
        return 4;
    }

    aeg::serialization::BinaryWriter archive_writer;
    aeg::serialization::BinaryWriter reordered_writer;
    if (!require(aeg::scene::SceneSerializer::serialize(asset, archive_writer), "Scene Asset serialization failed") ||
        !require(aeg::scene::SceneSerializer::serialize(reordered_asset, reordered_writer),
                 "reordered Scene Asset serialization failed") ||
        !require(archive_writer.size() != 0, "Scene Asset archive is empty") ||
        !require(
            archive_writer.size() == reordered_writer.size() &&
                std::equal(archive_writer.data().begin(), archive_writer.data().end(), reordered_writer.data().begin()),
            "Scene Asset archive is not deterministic")) {
        return 5;
    }

    aeg::serialization::BinaryReader archive_reader(archive_writer.data());
    const auto decoded = aeg::scene::SceneSerializer::deserialize(archive_reader);
    if (!require(decoded, "Scene Asset deserialization failed") ||
        !require(archive_reader.remaining() == 0, "Scene Asset archive left unread bytes") ||
        !require(decoded.value.id() == kAssetId && decoded.value.name() == "TestScene" && decoded.value.version() == 7,
                 "decoded Scene Asset metadata is incorrect") ||
        !require(decoded.value.entities().size() == 2 && decoded.value.components().size() == 1,
                 "decoded Scene Asset record counts are incorrect")) {
        return 6;
    }

    const auto decoded_root =
        std::find_if(decoded.value.entities().begin(), decoded.value.entities().end(),
                     [kRootId](const aeg::scene::SceneEntityRecord& entity) { return entity.id == kRootId; });
    const auto decoded_child =
        std::find_if(decoded.value.entities().begin(), decoded.value.entities().end(),
                     [kChildId](const aeg::scene::SceneEntityRecord& entity) { return entity.id == kChildId; });
    if (!require(decoded_root != decoded.value.entities().end() && decoded_child != decoded.value.entities().end(),
                 "decoded Scene hierarchy is incomplete") ||
        !require(decoded_root->parent == aeg::scene::SceneEntityId{} && decoded_root->name == "Root" &&
                     decoded_child->parent == kRootId && decoded_child->name == "Child" &&
                     aeg::scene::has_flag(decoded_child->flags, aeg::scene::SceneEntityFlags::kHidden) &&
                     aeg::scene::has_flag(decoded_child->flags, aeg::scene::SceneEntityFlags::kLocked),
                 "decoded Scene hierarchy metadata is incorrect") ||
        !require(decoded.value.components()[0].entity == kChildId &&
                     decoded.value.components()[0].component_type == kComponentId &&
                     decoded.value.components()[0].schema_type == schema_id &&
                     decoded.value.components()[0].schema_version == 1 &&
                     decoded.value.components()[0].payload == asset.components()[0].payload,
                 "decoded Scene component metadata is incorrect")) {
        return 7;
    }

    const std::span<const uint8_t> archive_bytes = archive_writer.data();
    aeg::serialization::BinaryReader truncated_reader(archive_bytes.first(archive_bytes.size() - 1));
    if (!require(!aeg::scene::SceneSerializer::deserialize(truncated_reader),
                 "truncated Scene Asset archive was accepted")) {
        return 8;
    }

    aeg::scene::SceneAsset cyclic_asset(kAssetId, "CyclicScene", 1);
    if (!require(add_entity(cyclic_asset, {kRootId, kChildId, "Root", aeg::scene::SceneEntityFlags::kNone}),
                 "failed to add cyclic Scene root") ||
        !require(add_entity(cyclic_asset, {kChildId, kRootId, "Child", aeg::scene::SceneEntityFlags::kNone}),
                 "failed to add cyclic Scene child") ||
        !require(!cyclic_asset.validate(), "cyclic Scene hierarchy was accepted")) {
        return 9;
    }

    aeg::ecs::WorldManager world_manager;
    const auto world_handle = world_manager.create_world({"SceneWorld", true});
    const auto world_result = world_handle ? world_manager.find(world_handle.value)
                                           : aeg::foundation::ResultValue<std::shared_ptr<aeg::ecs::World>>{};
    const auto schema = registry.find_type(schema_id);
    if (!require(world_handle && world_result, "failed to create Scene World") ||
        !require(schema, "Scene component schema cannot be found") ||
        !require(world_result.value->register_reflected_component<SceneComponent>(*schema.value, kComponentId,
                                                                                  "SceneComponent"),
                 "failed to register Scene component in World")) {
        return 10;
    }

    const auto placeholder = world_result.value->create_entity();
    if (!require(placeholder, "failed to create Scene World placeholder") ||
        !require(decoded.value.instantiate(*world_result.value, registry), "Scene Asset instantiation failed") ||
        !require(world_result.value->entity_count() == 2, "Scene instantiation entity count is incorrect")) {
        return 11;
    }

    const auto first_instance = decoded.value.instantiate(*world_result.value, registry);
    if (!require(first_instance, "second Scene Asset instantiation failed") ||
        !require(first_instance.value.entity_count() == 2, "Scene instance binding count is incorrect")) {
        return 12;
    }

    const auto first_root = first_instance.value.runtime_entity(kRootId);
    const auto first_child = first_instance.value.runtime_entity(kChildId);
    if (!require(first_root && first_child, "Scene instance binding lookup failed") ||
        !require(first_root.value != first_child.value, "Scene entities share a runtime identity") ||
        !require(first_root.value.index != static_cast<uint32_t>(kRootId.low()) ||
                     static_cast<uint64_t>(first_root.value.generation) != kRootId.high(),
                 "runtime EntityId unexpectedly matches SceneEntityId fields")) {
        return 13;
    }

    const SceneComponent* restored_component = world_result.value->get<SceneComponent>(first_child.value);
    if (!require(restored_component != nullptr && restored_component->health == source_component.health &&
                     restored_component->position == source_component.position &&
                     restored_component->label == source_component.label,
                 "instantiated Scene component value is incorrect")) {
        return 14;
    }

    const auto second_instance = decoded.value.instantiate(*world_result.value, registry);
    const auto second_root = second_instance ? second_instance.value.runtime_entity(kRootId)
                                             : aeg::foundation::ResultValue<aeg::ecs::EntityId>{};
    if (!require(second_instance, "repeated Scene Asset instantiation failed") ||
        !require(second_root && second_root.value != first_root.value,
                 "repeated Scene instantiation did not produce new runtime identities")) {
        return 15;
    }

    aeg::ecs::WorldManager mismatch_manager;
    const auto mismatch_handle = mismatch_manager.create_world({"MismatchWorld", true});
    const auto mismatch_world = mismatch_handle ? mismatch_manager.find(mismatch_handle.value)
                                                : aeg::foundation::ResultValue<std::shared_ptr<aeg::ecs::World>>{};
    const auto mismatched_schema = registry.find_type(aeg::reflection::TypeId("Game.MismatchedComponent"));
    if (!require(mismatch_handle && mismatch_world, "failed to create mismatch World") ||
        !require(mismatched_schema, "mismatched component schema cannot be found") ||
        !require(mismatch_world.value->register_reflected_component<MismatchedComponent>(
                     *mismatched_schema.value, kComponentId, "MismatchedComponent"),
                 "failed to register mismatched component in World") ||
        !require(mismatch_world.value->create_entity(), "failed to create mismatch World placeholder")) {
        return 16;
    }
    const auto mismatch_instance = decoded.value.instantiate(*mismatch_world.value, registry);
    if (!require(!mismatch_instance, "schema-mismatched Scene Asset was accepted") ||
        !require(mismatch_world.value->entity_count() == 0,
                 "failed Scene instantiation did not clear mismatch World")) {
        return 17;
    }

    return 0;
}
