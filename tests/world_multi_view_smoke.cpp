/**
 * @file world_multi_view_smoke.cpp
 * @brief Exercises isolated World snapshots and multiple Frame views.
 */
#include <array>
#include <cstdint>
#include <engine_frame_orchestrator.hpp>
#include <memory>
#include <rhi.hpp>

namespace {
struct Transform final {
    int value{0};
};

struct ViewObservation final {
    aeg::ecs::WorldHandle world{};
    aeg::ecs::EntityId entity{};
    uint32_t view_id{0};
    int expected_values[2]{0, 0};
    uint32_t call_count{0};
    uintptr_t scope_addresses[2]{0, 0};
    uint64_t frame_numbers[2]{0, 0};
    uint64_t revisions[2]{0, 0};
    int values[2]{0, 0};
    bool user_data_matches[2]{false, false};
};

struct MultiViewState final {
    std::array<ViewObservation, 3> observations{};
    void* expected_user_data{nullptr};

    [[nodiscard]] ViewObservation* find(aeg::ecs::WorldHandle world, uint32_t view_id) noexcept {
        for (ViewObservation& observation : observations) {
            if (observation.world == world && observation.view_id == view_id) {
                return &observation;
            }
        }
        return nullptr;
    }
};

[[nodiscard]] aeg::foundation::Result invalid_state(const char* message) {
    return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState, message);
}

[[nodiscard]] aeg::foundation::Result read_world_snapshot(aeg::pipeline::PipelineTaskContext& context) {
    auto* state = static_cast<MultiViewState*>(context.user_data());
    auto* frame_scope = static_cast<aeg::world::WorldFrameScope*>(context.scope_data());
    if (state == nullptr || frame_scope == nullptr || !frame_scope->is_valid()) {
        return invalid_state("Frame task did not receive a valid WorldFrameScope");
    }

    const aeg::world::WorldFrameKey& key = frame_scope->key();
    ViewObservation* observation = state->find(key.world, key.view_id);
    if (observation == nullptr || observation->call_count >= 2) {
        return invalid_state("Frame task received an unregistered or repeated World view");
    }

    const auto view =
        context.read_artifact(aeg::world::kWorldSnapshotArtifactId, aeg::world::kWorldSnapshotArtifactTypeId,
                              aeg::world::kWorldSnapshotArtifactVersion);
    if (!view) {
        return view.result;
    }
    const auto* snapshot = view.value.as<aeg::ecs::WorldSnapshot>(aeg::world::kWorldSnapshotArtifactTypeId);
    if (snapshot == nullptr || snapshot->world() != observation->world ||
        view.value.owner_scope() != aeg::pipeline::PipelineScope::kWorld) {
        return invalid_state("Frame task received an invalid World snapshot");
    }

    const Transform* transform = snapshot->get<Transform>(observation->entity);
    if (transform == nullptr) {
        return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kNotFound,
                                                "Frame task snapshot entity is missing");
    }

    const uint32_t call_index = observation->call_count++;
    observation->scope_addresses[call_index] = reinterpret_cast<uintptr_t>(frame_scope);
    observation->frame_numbers[call_index] = key.frame_number;
    observation->revisions[call_index] = snapshot->revision();
    observation->values[call_index] = transform->value;
    observation->user_data_matches[call_index] = context.execution_context().user_data() == state->expected_user_data;
    return aeg::foundation::Result::success();
}

class MultiViewPipeline final : public aeg::pipeline::Pipeline {
public:
    explicit MultiViewPipeline(MultiViewState& state) : state_(state) {
        descriptor_.id = aeg::pipeline::PipelineId("WorldMultiViewFrame");
        descriptor_.name = "WorldMultiViewFrame";
        descriptor_.category = aeg::pipeline::PipelineCategory::kRendering;
        descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame);
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
            aeg::pipeline::StageId("WorldMultiViewExtract"),
            aeg::pipeline::EnginePhase::kRenderExtract,
            aeg::pipeline::ExecutionDomain::kMainThread,
            aeg::pipeline::TimePolicy::kFrame,
            false,
        });
        const auto task = builder.add_task({
            aeg::pipeline::TaskId("WorldMultiViewExtractTask"),
            aeg::pipeline::TaskFlags::kMustRun,
            &read_world_snapshot,
            &state_,
        });
        const auto artifact = builder.declare_artifact({
            aeg::world::kWorldSnapshotArtifactId,
            aeg::world::kWorldSnapshotArtifactTypeId,
            aeg::pipeline::PipelineScope::kWorld,
            aeg::pipeline::ArtifactStorage::kImmutableSnapshot,
            aeg::pipeline::ArtifactLifetime::kWorld,
            aeg::pipeline::OwnershipPolicy::kSharedImmutable,
            aeg::world::kWorldSnapshotArtifactVersion,
            0,
            true,
        });
        if (!stage || !task || !artifact) {
            return builder.status();
        }
        if (const auto result = builder.bind_task(task, stage); !result) {
            return result;
        }
        if (const auto result =
                builder.expose_artifact(aeg::world::kWorldSnapshotArtifactId, aeg::pipeline::PipelineScope::kFrame);
            !result) {
            return result;
        }
        return builder.reads(stage, {
                                        aeg::world::kWorldSnapshotArtifactId,
                                        aeg::pipeline::ArtifactAccess::kRead,
                                        aeg::world::kWorldSnapshotArtifactVersion,
                                        aeg::pipeline::AccessFlags::kNone,
                                    });
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
    MultiViewState& state_;
};

[[nodiscard]] bool check_frame(const MultiViewState& state, uint32_t call_index, uint64_t frame_number,
                               uint64_t revision, int first_world_value, int second_world_value) {
    for (const ViewObservation& observation : state.observations) {
        const int expected_value =
            observation.world == state.observations[2].world ? second_world_value : first_world_value;
        if (observation.call_count <= call_index || observation.revisions[call_index] != revision ||
            observation.frame_numbers[call_index] != frame_number || observation.values[call_index] != expected_value ||
            !observation.user_data_matches[call_index] || observation.scope_addresses[call_index] == 0) {
            return false;
        }
    }
    return state.observations[0].scope_addresses[call_index] != state.observations[1].scope_addresses[call_index] &&
           state.observations[0].scope_addresses[call_index] != state.observations[2].scope_addresses[call_index] &&
           state.observations[1].scope_addresses[call_index] != state.observations[2].scope_addresses[call_index];
}
}  // namespace

int main() {
    auto device_result = aeg::rhi::create_device({});
    if (!device_result) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);

    aeg::ecs::WorldManager world_manager;
    const auto first_handle = world_manager.create_world({"MultiViewGame", true});
    const auto second_handle = world_manager.create_world({"MultiViewPreview", true});
    if (!first_handle || !second_handle) {
        return 2;
    }
    const auto first_world = world_manager.find(first_handle.value);
    const auto second_world = world_manager.find(second_handle.value);
    if (!first_world || !second_world ||
        !first_world.value->register_component<Transform>(aeg::ecs::ComponentTypeId("Transform"), "Transform") ||
        !second_world.value->register_component<Transform>(aeg::ecs::ComponentTypeId("Transform"), "Transform")) {
        return 3;
    }

    const auto first_entity = first_world.value->create_entity();
    const auto second_entity = second_world.value->create_entity();
    if (!first_entity || !second_entity || !first_world.value->emplace<Transform>(first_entity.value, Transform{10}) ||
        !second_world.value->emplace<Transform>(second_entity.value, Transform{20})) {
        return 4;
    }

    MultiViewState state{};
    struct FrameUserData final {
        uint32_t marker{0xAEE1u};
    } frame_user_data;
    state.expected_user_data = &frame_user_data;
    state.observations = {
        ViewObservation{first_handle.value, first_entity.value, 10, {10, 11}},
        ViewObservation{first_handle.value, first_entity.value, 20, {10, 11}},
        ViewObservation{second_handle.value, second_entity.value, 30, {20, 21}},
    };

    aeg::pipeline::PipelineRegistry registry;
    if (!registry.register_pipeline([&state] { return std::make_unique<MultiViewPipeline>(state); })) {
        return 5;
    }

    aeg::world::WorldScope first_scope(first_world.value, registry, {});
    aeg::world::WorldScope second_scope(second_world.value, registry, {});
    if (!first_scope.initialize() || !second_scope.initialize()) {
        return 6;
    }

    auto sentinel_result = first_scope.create_frame_bridge();
    if (!sentinel_result) {
        return 7;
    }
    std::unique_ptr<aeg::pipeline::ArtifactScopeBridge> sentinel_bridge = std::move(sentinel_result.value);

    aeg::pipeline::PipelineScopeHost first_view_host(aeg::pipeline::PipelineScope::kFrame,
                                                     aeg::pipeline::PipelineInstanceId("MultiViewGameView10"));
    aeg::pipeline::PipelineScopeHost second_view_host(aeg::pipeline::PipelineScope::kFrame,
                                                      aeg::pipeline::PipelineInstanceId("MultiViewGameView20"));
    aeg::pipeline::PipelineScopeHost preview_view_host(aeg::pipeline::PipelineScope::kFrame,
                                                       aeg::pipeline::PipelineInstanceId("MultiViewPreviewView30"));
    first_view_host.set_scope_bridge(sentinel_bridge.get());
    if (!first_view_host.bind_pipeline(aeg::pipeline::PipelineId("WorldMultiViewFrame")) ||
        !second_view_host.bind_pipeline(aeg::pipeline::PipelineId("WorldMultiViewFrame")) ||
        !preview_view_host.bind_pipeline(aeg::pipeline::PipelineId("WorldMultiViewFrame")) ||
        !first_view_host.rebuild(registry) || !second_view_host.rebuild(registry) ||
        !preview_view_host.rebuild(registry)) {
        return 8;
    }

    aeg::pipeline::PipelineScheduler scheduler({1, 1, 1, 1});
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFrameOrchestrator orchestrator(registry, scheduler, frame_context, render_graph);
    if (!orchestrator.register_world_frame(first_scope, first_view_host, 10) ||
        !orchestrator.register_world_frame(first_scope, second_view_host, 20) ||
        !orchestrator.register_world_frame(second_scope, preview_view_host, 30) || orchestrator.world_count() != 2 ||
        orchestrator.world_frame_count() != 3 || !orchestrator.initialize()) {
        return 9;
    }

    aeg::engine::EngineFrameDesc first_frame{};
    first_frame.fixed_tick = 4;
    first_frame.user_data = &frame_user_data;
    if (!orchestrator.run_frame(first_frame) || !check_frame(state, 0, 1, 1, 10, 20) ||
        orchestrator.is_frame_active() || first_view_host.scope_bridge() != sentinel_bridge.get() ||
        second_view_host.scope_bridge() != nullptr || preview_view_host.scope_bridge() != nullptr) {
        return 10;
    }

    first_world.value->get<Transform>(first_entity.value)->value = 11;
    second_world.value->get<Transform>(second_entity.value)->value = 21;
    aeg::engine::EngineFrameDesc second_frame{};
    second_frame.fixed_tick = 5;
    second_frame.user_data = &frame_user_data;
    if (!orchestrator.run_frame(second_frame) || !check_frame(state, 1, 2, 2, 11, 21)) {
        return 11;
    }

    if (!orchestrator.shutdown() || orchestrator.is_initialized() ||
        !orchestrator.unregister_world_frame(first_scope.handle(), 10) ||
        !orchestrator.unregister_world_frame(first_scope.handle(), 20) ||
        !orchestrator.unregister_world_frame(second_scope.handle(), 30) ||
        !orchestrator.unregister_world(first_scope.handle()) || !orchestrator.unregister_world(second_scope.handle()) ||
        orchestrator.world_count() != 0 || orchestrator.world_frame_count() != 0) {
        return 12;
    }

    first_view_host.set_scope_bridge(nullptr);
    sentinel_bridge.reset();
    if (!first_scope.shutdown() || !second_scope.shutdown()) {
        return 13;
    }
    return 0;
}
