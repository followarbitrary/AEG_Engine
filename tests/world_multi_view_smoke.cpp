/**
 * @file world_multi_view_smoke.cpp
 * @brief Exercises isolated World snapshots and multiple Frame views.
 */
#include <array>
#include <cstdint>
#include <engine_frame_orchestrator.hpp>
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include <memory>
#include <rhi.hpp>
#include <utility>
#include "../Engine/Pipeline/private/pipeline_scope_host_execution.hpp"
#include "world_snapshot_execution.hpp"
#include "engine_frame_inspection.hpp"

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

[[nodiscard]] aeg::foundation::Result read_world_snapshot(
    const aeg::engine::EngineFrameOrchestrator& owner, MultiViewState& state, uint64_t frame_number) {
    const auto trace = aeg::engine::detail::EngineFrameInspection::capture(owner);
    if (!trace) return trace.result;
    uint32_t matches = 0;
    for (const auto& event : trace.value.stages) {
        if (event.stage != aeg::pipeline::StageId{"WorldMultiViewExtract"} || event.frame_number != frame_number) continue;
        ++matches;
        if (!event.world_frame || !event.world_snapshot || !event.bridged_snapshot || event.scope_identity == 0 ||
            event.phase != aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kRenderExtract))
            return invalid_state("Frame Stage lost current World ownership");
        const auto& key = *event.world_frame;
        auto* observation = state.find(key.world, key.view_id);
        if (!observation || observation->call_count >= 2) return invalid_state("Repeated or unregistered World view");
        const auto& snapshot = *event.world_snapshot;
        const auto* bridged = event.bridged_snapshot->as<aeg::ecs::WorldSnapshot>(aeg::world::detail::kWorldSnapshotArtifactTypeId);
        const auto* transform = snapshot.get<Transform>(observation->entity);
        if (!bridged || !transform || snapshot.world() != observation->world || snapshot.world() != key.world ||
            event.bridged_snapshot->owner_scope() != aeg::pipeline::PipelineScope::kWorld ||
            snapshot.world() != bridged->world() || snapshot.revision() != bridged->revision() ||
            snapshot.frame_number() != key.frame_number || snapshot.fixed_tick() != event.fixed_tick)
            return invalid_state("Frame Stage snapshot and actual bridge disagree");
        const uint32_t call_index = observation->call_count++;
        observation->scope_addresses[call_index] = event.scope_identity;
        observation->frame_numbers[call_index] = key.frame_number;
        observation->revisions[call_index] = snapshot.revision();
        observation->values[call_index] = transform->value;
        observation->user_data_matches[call_index] = event.user_data_identity == reinterpret_cast<uintptr_t>(state.expected_user_data);
    }
    return matches == 3 ? aeg::foundation::Result::success() : invalid_state("Unexpected multi-View Stage visit count");
}
[[nodiscard]] aeg::foundation::ResultValue<aeg::pipeline::PipelineDefinition> multi_view_pipeline_definition() {
    aeg::pipeline::PipelineDescriptor descriptor{};
    descriptor.id = aeg::pipeline::PipelineId("WorldMultiViewFrame");
    descriptor.name = "WorldMultiViewFrame";
    descriptor.category = aeg::pipeline::PipelineCategory::kRendering;
    descriptor.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame);
    aeg::pipeline::PipelineBuilder builder(descriptor);
    const auto stage = builder.add_stage({
        aeg::pipeline::StageId("WorldMultiViewExtract"),
        aeg::pipeline::EnginePhase::kRenderExtract,
        aeg::pipeline::ExecutionDomain::kMainThread,
        aeg::pipeline::TimePolicy::kFrame,
        false,
    });
    const auto artifact = builder.declare_artifact({
        aeg::world::detail::kWorldSnapshotArtifactId,
        aeg::world::detail::kWorldSnapshotArtifactTypeId,
        aeg::pipeline::PipelineScope::kWorld,
        aeg::pipeline::ArtifactStorage::kImmutableSnapshot,
        aeg::pipeline::ArtifactLifetime::kWorld,
        aeg::pipeline::OwnershipPolicy::kSharedImmutable,
        aeg::world::detail::kWorldSnapshotArtifactVersion,
        0,
        true,
    });
    if (!stage || !artifact) {
        return {builder.status(), {}};
    }
    if (const auto result =
            builder.expose_artifact(aeg::world::detail::kWorldSnapshotArtifactId, aeg::pipeline::PipelineScope::kFrame);
        !result) {
        return {result, {}};
    }
    if (const auto result = builder.reads(stage, {
                                    aeg::world::detail::kWorldSnapshotArtifactId,
                                    aeg::pipeline::ArtifactAccess::kRead,
                                    aeg::world::detail::kWorldSnapshotArtifactVersion,
                                    aeg::pipeline::AccessFlags::kNone,
                                }); !result) return {result, {}};
    return builder.finish();
}

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
    auto definition = multi_view_pipeline_definition();
    if (!definition || !registry.register_pipeline(std::move(definition.value))) {
        return 5;
    }

    aeg::world::WorldScope first_scope(first_world.value);
    aeg::world::WorldScope second_scope(second_world.value);
    if (!first_scope.initialize() || !second_scope.initialize()) {
        return 6;
    }

    auto sentinel_result = aeg::world::detail::WorldSnapshotExecution::create_frame_bridge(first_scope);
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
    aeg::pipeline::detail::PipelineScopeHostExecution::set_scope_bridge(first_view_host, sentinel_bridge.get());
    if (!first_view_host.bind_pipeline(aeg::pipeline::PipelineId("WorldMultiViewFrame")) ||
        !second_view_host.bind_pipeline(aeg::pipeline::PipelineId("WorldMultiViewFrame")) ||
        !preview_view_host.bind_pipeline(aeg::pipeline::PipelineId("WorldMultiViewFrame")) ||
        !first_view_host.rebuild(registry) || !second_view_host.rebuild(registry) ||
        !preview_view_host.rebuild(registry)) {
        return 8;
    }

    aeg::pipeline::PipelineScheduler scheduler;
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFramePipelineHost pipeline_host(registry, scheduler);
    aeg::engine::EngineFrameOrchestrator orchestrator(pipeline_host, frame_context, render_graph);
    if (!orchestrator.register_world_frame(
            first_scope, aeg::engine::EngineFramePipelineRegistration{first_view_host}, 10) ||
        !orchestrator.register_world_frame(
            first_scope, aeg::engine::EngineFramePipelineRegistration{second_view_host}, 20) ||
        !orchestrator.register_world_frame(
            second_scope, aeg::engine::EngineFramePipelineRegistration{preview_view_host}, 30) ||
        orchestrator.world_count() != 2 ||
        orchestrator.world_frame_count() != 3 || !orchestrator.initialize()) {
        return 9;
    }

    if (!aeg::engine::detail::EngineFrameInspection::enable(orchestrator)) return 14;
    aeg::engine::EngineFrameDesc first_frame{};
    first_frame.fixed_tick = 4;
    first_frame.user_data = &frame_user_data;
    if (!orchestrator.run_frame(first_frame) || !read_world_snapshot(orchestrator, state, 1) || !check_frame(state, 0, 1, 1, 10, 20) ||
        orchestrator.is_frame_active() ||
        aeg::pipeline::detail::PipelineScopeHostExecution::scope_bridge(first_view_host) != sentinel_bridge.get() ||
        aeg::pipeline::detail::PipelineScopeHostExecution::scope_bridge(second_view_host) != nullptr ||
        aeg::pipeline::detail::PipelineScopeHostExecution::scope_bridge(preview_view_host) != nullptr) {
        return 10;
    }

    first_world.value->get<Transform>(first_entity.value)->value = 11;
    second_world.value->get<Transform>(second_entity.value)->value = 21;
    aeg::engine::EngineFrameDesc second_frame{};
    second_frame.fixed_tick = 5;
    second_frame.user_data = &frame_user_data;
    if (!orchestrator.run_frame(second_frame) || !read_world_snapshot(orchestrator, state, 2) || !check_frame(state, 1, 2, 2, 11, 21)) {
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

    const auto retained_trace = aeg::engine::detail::EngineFrameInspection::capture(orchestrator);
    if (!retained_trace) return 15;
    if (!aeg::engine::detail::EngineFrameInspection::enable(orchestrator, false) ||
        !aeg::engine::detail::EngineFrameInspection::capture(orchestrator).value.stages.empty()) return 16;
    uint32_t retained_views = 0;
    for (const auto& event : retained_trace.value.stages) {
        if (!event.world_snapshot || event.frame_number != 1) continue;
        const auto* observation = state.find(event.world_frame->world, event.world_frame->view_id);
        const auto* original = observation ? event.world_snapshot->get<Transform>(observation->entity) : nullptr;
        if (!original || original->value != observation->expected_values[0] || event.world_snapshot->revision() != 1) return 17;
        ++retained_views;
    }
    if (retained_views != 3) return 18;
    aeg::pipeline::detail::PipelineScopeHostExecution::set_scope_bridge(first_view_host, nullptr);
    sentinel_bridge.reset();
    if (!first_scope.shutdown() || !second_scope.shutdown()) {
        return 13;
    }
    return 0;
}
