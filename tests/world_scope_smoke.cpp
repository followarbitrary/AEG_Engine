/**
 * @file world_scope_smoke.cpp
 * @brief Exercises World Scope snapshot publication and World-to-Frame data flow.
 */
#include <ecs.hpp>
#include <memory>
#include <world_scope.hpp>

namespace {
struct Transform final {
    float x{0.0f};
    float y{0.0f};
};
}  // namespace

int main() {
    aeg::ecs::WorldManager manager;
    const auto handle_result = manager.create_world({"ScopeWorld", true});
    if (!handle_result) {
        return 1;
    }
    const auto world_result = manager.find(handle_result.value);
    if (!world_result) {
        return 2;
    }

    aeg::world::WorldScope scope(world_result.value);
    if (!scope.initialize() ||
        !scope.state().register_component<Transform>(aeg::ecs::ComponentTypeId("Transform"), "Transform")) {
        return 3;
    }

    const auto entity = scope.state().create_entity();
    if (!entity || !scope.state().emplace<Transform>(entity.value, Transform{12.0f, 15.0f})) {
        return 4;
    }
    const auto first_snapshot = scope.commit(21, 4);
    if (!first_snapshot || first_snapshot.value->revision() != 1 || first_snapshot.value->world() != scope.handle()) {
        return 5;
    }
    const auto latest_after_first_commit = scope.latest_snapshot();
    if (!latest_after_first_commit || latest_after_first_commit.value->revision() != 1) {
        return 5;
    }

    auto frame_result = scope.create_frame_scope(21, 3);
    if (!frame_result || !frame_result.value.capture()) {
        return 6;
    }
    const auto first_view = frame_result.value.read_snapshot();
    if (!first_view) {
        return 7;
    }
    const Transform* first_transform = first_view.value.get<Transform>(entity.value);
    if (first_transform == nullptr || first_transform->x != 12.0f || first_transform->y != 15.0f ||
        first_view.value.world() != scope.handle() || first_view.value.revision() != 1 ||
        first_view.value.frame_number() != 21 || first_view.value.fixed_tick() != 4 ||
        frame_result.value.key().view_id != 3) {
        return 8;
    }

    Transform* mutable_transform = scope.state().get<Transform>(entity.value);
    if (mutable_transform == nullptr) {
        return 9;
    }
    mutable_transform->x = 99.0f;
    const auto second_snapshot = scope.commit(22, 5);
    if (!second_snapshot || second_snapshot.value->revision() != 2) {
        return 10;
    }
    const auto latest_after_second_commit = scope.latest_snapshot();
    if (!latest_after_second_commit || latest_after_second_commit.value->revision() != 2 ||
        scope.publish_snapshot(*first_snapshot.value).code != aeg::foundation::ResultCode::kInvalidState) {
        return 10;
    }

    const auto retained_first_view = frame_result.value.read_snapshot();
    if (!retained_first_view) {
        return 11;
    }
    const Transform* retained_transform = retained_first_view.value.get<Transform>(entity.value);
    if (retained_transform == nullptr || retained_transform->x != 12.0f ||
        retained_first_view.value.world() != scope.handle() || retained_first_view.value.revision() != 1) {
        return 12;
    }

    auto second_frame_result = scope.create_frame_scope(22, 3);
    if (!second_frame_result || !second_frame_result.value.capture()) {
        return 13;
    }
    const auto second_view = second_frame_result.value.read_snapshot();
    const Transform* second_transform = second_view ? second_view.value.get<Transform>(entity.value) : nullptr;
    if (!second_view || second_transform == nullptr || second_transform->x != 99.0f ||
        second_view.value.revision() != 2 || second_view.value.world() != scope.handle() ||
        second_view.value.frame_number() != 22 || second_view.value.fixed_tick() != 5) {
        return 14;
    }

    if (!scope.command_buffer().enqueue([](aeg::ecs::World&) { return aeg::foundation::Result::success(); }) ||
        !scope.shutdown()) {
        return 15;
    }
    if (frame_result.value.capture().code != aeg::foundation::ResultCode::kInvalidState) {
        return 16;
    }
    const auto retained_after_shutdown = frame_result.value.read_snapshot();
    if (!retained_after_shutdown) {
        return 17;
    }
    const auto* retained_after_shutdown_transform = retained_after_shutdown.value.get<Transform>(entity.value);
    if (retained_after_shutdown.value.revision() != 1 || retained_after_shutdown.value.world() != scope.handle() ||
        retained_after_shutdown_transform == nullptr || retained_after_shutdown_transform->x != 12.0f) {
        return 18;
    }
    if (!scope.initialize() || !scope.command_buffer().empty()) {
        return 19;
    }
    if (frame_result.value.capture().code != aeg::foundation::ResultCode::kInvalidState) {
        return 20;
    }
    const auto third_snapshot = scope.commit(23, 6);
    if (!third_snapshot || third_snapshot.value->revision() != 3) {
        return 21;
    }
    return 0;
}
