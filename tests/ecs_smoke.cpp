/**
 * @file ecs_smoke.cpp
 * @brief Exercises World entity generations, component pools, commands, and snapshots.
 */
#include <concurrency.hpp>
#include <ecs.hpp>

namespace {
struct Position final {
    float x{0.0f};
    float y{0.0f};
};

struct Velocity final {
    float x{0.0f};
};

struct Acceleration final {
    float x{0.0f};
};
}  // namespace

int main() {
    aeg::ecs::WorldManager manager;
    const auto first_handle = manager.create_world({"GameWorld", true});
    const auto second_handle = manager.create_world({"PreviewWorld", true});
    if (!first_handle || !second_handle || first_handle.value == second_handle.value || manager.world_count() != 2) {
        return 1;
    }

    const auto first_result = manager.find(first_handle.value);
    const auto second_result = manager.find(second_handle.value);
    if (!first_result || !second_result) {
        return 2;
    }
    const std::shared_ptr<aeg::ecs::World>& first = first_result.value;
    const std::shared_ptr<aeg::ecs::World>& second = second_result.value;
    if (!first->register_component<Position>(aeg::ecs::ComponentTypeId("Position"), "Position") ||
        !first->register_component<Velocity>(aeg::ecs::ComponentTypeId("Velocity"), "Velocity") ||
        !second->register_component<Position>(aeg::ecs::ComponentTypeId("Position"), "Position")) {
        return 3;
    }

    const auto first_entity = first->create_entity();
    const auto second_entity = second->create_entity();
    if (!first_entity || !second_entity || !first->emplace<Position>(first_entity.value, Position{1.0f, 2.0f}) ||
        !second->emplace<Position>(second_entity.value, Position{8.0f, 9.0f})) {
        return 4;
    }
    Position* first_position = first->get<Position>(first_entity.value);
    const Position* second_position = second->get<Position>(second_entity.value);
    if (first_position == nullptr || second_position == nullptr || second_position->x != 8.0f) {
        return 5;
    }
    std::size_t mutable_count = 0;
    if (!first->for_each<Position>([&mutable_count](aeg::ecs::EntityId entity, Position& position) {
            if (entity.index == 0 && position.x == 1.0f) {
                ++mutable_count;
            }
        }) ||
        mutable_count != 1) {
        return 5;
    }
    const aeg::ecs::World& first_const = *first;
    std::size_t const_count = 0;
    if (!first_const.for_each<Position>([&const_count](aeg::ecs::EntityId, const Position&) { ++const_count; }) ||
        const_count != 1) {
        return 5;
    }
    bool structural_mutation_rejected = false;
    bool structural_mutation_checks_passed = false;
    if (!first->for_each<Position>([&](aeg::ecs::EntityId entity, Position&) {
            const auto created = first->create_entity();
            const auto destroyed = first->destroy_entity(entity);
            const auto added = first->emplace<Velocity>(entity, Velocity{2.0f});
            const auto removed = first->remove<Position>(entity);
            const auto registered =
                first->register_component<Acceleration>(aeg::ecs::ComponentTypeId("Acceleration"), "Acceleration");
            aeg::ecs::WorldCommandBuffer nested_commands;
            const auto applied = first->apply_commands(nested_commands);
            structural_mutation_rejected = created.result.code == aeg::foundation::ResultCode::kInvalidState &&
                                           destroyed.code == aeg::foundation::ResultCode::kInvalidState &&
                                           added.result.code == aeg::foundation::ResultCode::kInvalidState &&
                                           removed.code == aeg::foundation::ResultCode::kInvalidState &&
                                           registered.code == aeg::foundation::ResultCode::kInvalidState &&
                                           applied.code == aeg::foundation::ResultCode::kInvalidState;
            structural_mutation_checks_passed = true;
        }) ||
        !structural_mutation_checks_passed || !structural_mutation_rejected) {
        return 5;
    }
    first_position->x = 20.0f;
    if (second->get<Position>(second_entity.value)->x != 8.0f) {
        return 5;
    }

    aeg::ecs::WorldCommandBuffer commands;
    if (!commands.enqueue([entity = first_entity.value](aeg::ecs::World& world) {
            Position* position = world.get<Position>(entity);
            if (position == nullptr) {
                return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kNotFound,
                                                        "command entity is missing");
            }
            position->x = 4.0f;
            return aeg::foundation::Result::success();
        }) ||
        commands.size() != 1) {
        return 6;
    }
    aeg::concurrency::WorkerExecutor producer_executor(
        {aeg::concurrency::ExecutorDomain::kCpuWorker, 1, true});
    aeg::concurrency::TaskDesc producer_task{};
    producer_task.execute = [&commands, entity = first_entity.value](const aeg::concurrency::TaskContext&) {
        return commands.enqueue_remove<Position>(entity);
    };
    const auto producer_submission = producer_executor.submit(std::move(producer_task));
    if (!producer_submission || !producer_submission.value.wait(5'000'000'000ull)) {
        return 7;
    }
    if (commands.size() != 2 || !first->apply_commands(commands) || !commands.empty() ||
        first->has<Position>(first_entity.value)) {
        return 7;
    }

    if (!first->emplace<Position>(first_entity.value, Position{3.0f, 5.0f})) {
        return 8;
    }
    const auto snapshot = first->commit(11, 7);
    if (!snapshot || snapshot.value.revision() != 1 || snapshot.value.frame_number() != 11 ||
        snapshot.value.fixed_tick() != 7 || !snapshot.value.contains(first_entity.value)) {
        return 9;
    }
    const Position* snapshot_position = snapshot.value.get<Position>(first_entity.value);
    if (snapshot_position == nullptr || snapshot_position->x != 3.0f || snapshot_position->y != 5.0f) {
        return 10;
    }
    if (snapshot.value.entity_at(0) != first_entity.value || snapshot.value.entity_at(1).is_valid()) {
        return 10;
    }

    if (!first->destroy_entity(first_entity.value)) {
        return 11;
    }
    const auto replacement_entity = first->create_entity();
    if (!replacement_entity || replacement_entity.value.index != first_entity.value.index ||
        replacement_entity.value.generation == first_entity.value.generation ||
        snapshot.value.get<Position>(first_entity.value) == nullptr) {
        return 12;
    }

    if (!manager.destroy_world(first_handle.value) || manager.contains(first_handle.value) ||
        manager.world_count() != 1 ||
        manager.find(first_handle.value).result.code != aeg::foundation::ResultCode::kNotFound) {
        return 13;
    }
    return 0;
}
