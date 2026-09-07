/**
 * @file world_frame_orchestrator_smoke.cpp
 * @brief Exercises automatic WorldCommit publication and Frame snapshot consumption.
 */
#include <engine_frame_orchestrator.hpp>
#include <memory>
#include <rhi.hpp>

namespace {
struct Transform final {
    float x{0.0f};
};

struct FrameUserData final {
    uint32_t marker{0xAE60u};
};

struct WorldTaskState final {
    aeg::ecs::EntityId entity{};
    void* expected_user_data{nullptr};
    bool received_world{false};
    bool received_user_data{false};
    bool command_applied{false};
};

struct FrameTaskState final {
    void* expected_user_data{nullptr};
    float observed_x{0.0f};
    uint64_t observed_revision{0};
    uint32_t observed_view_id{0};
    bool received_user_data{false};
};

[[nodiscard]] aeg::foundation::Result update_world(aeg::pipeline::PipelineTaskContext& context) {
    auto* world_scope = static_cast<aeg::world::WorldScope*>(context.scope_data());
    auto* state = static_cast<WorldTaskState*>(context.user_data());
    if (world_scope == nullptr || state == nullptr) {
        return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                "World pipeline did not receive its World scope");
    }
    auto* world = &world_scope->state();
    state->received_world = true;
    state->received_user_data = context.execution_context().user_data() == state->expected_user_data;
    Transform* transform = world->get<Transform>(state->entity);
    if (transform == nullptr) {
        return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kNotFound,
                                                "World pipeline entity is missing");
    }
    state->command_applied = transform->x == 5.0f;
    transform->x = 7.0f;
    return aeg::foundation::Result::success();
}

[[nodiscard]] aeg::foundation::Result read_world_snapshot(aeg::pipeline::PipelineTaskContext& context) {
    auto view = context.read_artifact(aeg::world::kWorldSnapshotArtifactId, aeg::world::kWorldSnapshotArtifactTypeId);
    if (!view) {
        return view.result;
    }
    const auto* snapshot = view.value.as<aeg::ecs::WorldSnapshot>(aeg::world::kWorldSnapshotArtifactTypeId);
    auto* state = static_cast<FrameTaskState*>(context.user_data());
    auto* frame_scope = static_cast<aeg::world::WorldFrameScope*>(context.scope_data());
    if (snapshot == nullptr || state == nullptr || frame_scope == nullptr) {
        return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                "Frame pipeline received an invalid World snapshot");
    }
    state->received_user_data = context.execution_context().user_data() == state->expected_user_data;
    const Transform* transform = snapshot->get<Transform>(aeg::ecs::EntityId{0, 1});
    if (transform == nullptr) {
        return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kNotFound,
                                                "Frame pipeline snapshot entity is missing");
    }
    state->observed_x = transform->x;
    state->observed_revision = snapshot->revision();
    state->observed_view_id = frame_scope->key().view_id;
    return aeg::foundation::Result::success();
}

class WorldPipeline final : public aeg::pipeline::Pipeline {
public:
    explicit WorldPipeline(WorldTaskState& state) : state_(state) {
        descriptor_.id = aeg::pipeline::PipelineId("WorldFrameOrchestratorWorld");
        descriptor_.name = "WorldFrameOrchestratorWorld";
        descriptor_.category = aeg::pipeline::PipelineCategory::kSimulation;
        descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kWorld);
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
            aeg::pipeline::StageId("WorldFrameOrchestratorUpdate"),
            aeg::pipeline::EnginePhase::kPreUpdate,
            aeg::pipeline::ExecutionDomain::kMainThread,
            aeg::pipeline::TimePolicy::kFrame,
            false,
        });
        const auto task = builder.add_task({
            aeg::pipeline::TaskId("WorldFrameOrchestratorUpdateTask"),
            aeg::pipeline::TaskFlags::kMustRun,
            &update_world,
            &state_,
        });
        if (!stage || !task) {
            return builder.status();
        }
        return builder.bind_task(task, stage);
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
    WorldTaskState& state_;
};

class FramePipeline final : public aeg::pipeline::Pipeline {
public:
    explicit FramePipeline(FrameTaskState& state) : state_(state) {
        descriptor_.id = aeg::pipeline::PipelineId("WorldFrameOrchestratorFrame");
        descriptor_.name = "WorldFrameOrchestratorFrame";
        descriptor_.category = aeg::pipeline::PipelineCategory::kRendering;
        descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame);
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
            aeg::pipeline::StageId("WorldFrameOrchestratorExtract"),
            aeg::pipeline::EnginePhase::kRenderExtract,
            aeg::pipeline::ExecutionDomain::kMainThread,
            aeg::pipeline::TimePolicy::kFrame,
            false,
        });
        const auto task = builder.add_task({
            aeg::pipeline::TaskId("WorldFrameOrchestratorExtractTask"),
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
    FrameTaskState& state_;
};
}  // namespace

int main() {
    auto device_result = aeg::rhi::create_device({});
    if (!device_result) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);

    aeg::ecs::WorldManager world_manager;
    const auto world_handle = world_manager.create_world({"OrchestratedWorld", true});
    if (!world_handle) {
        return 2;
    }
    const auto world_result = world_manager.find(world_handle.value);
    if (!world_result ||
        !world_result.value->register_component<Transform>(aeg::ecs::ComponentTypeId("Transform"), "Transform")) {
        return 3;
    }
    const auto entity = world_result.value->create_entity();
    if (!entity || !world_result.value->emplace<Transform>(entity.value, Transform{1.0f})) {
        return 4;
    }

    FrameUserData frame_user_data;
    WorldTaskState world_state{};
    world_state.entity = entity.value;
    world_state.expected_user_data = &frame_user_data;
    FrameTaskState frame_state{};
    frame_state.expected_user_data = &frame_user_data;
    aeg::pipeline::PipelineRegistry registry;
    if (!registry.register_pipeline([&world_state] { return std::make_unique<WorldPipeline>(world_state); }) ||
        !registry.register_pipeline([&frame_state] { return std::make_unique<FramePipeline>(frame_state); })) {
        return 5;
    }

    aeg::world::WorldScope world_scope(world_result.value, registry,
                                       aeg::world::WorldScopeDesc{
                                           .pipeline = aeg::pipeline::PipelineId("WorldFrameOrchestratorWorld"),
                                       });
    if (!world_scope.initialize()) {
        return 6;
    }
    aeg::pipeline::PipelineScopeHost frame_host(aeg::pipeline::PipelineScope::kFrame,
                                                aeg::pipeline::PipelineInstanceId("WorldFrameOrchestratorFrameHost"));
    if (!frame_host.bind_pipeline(aeg::pipeline::PipelineId("WorldFrameOrchestratorFrame")) ||
        !frame_host.rebuild(registry)) {
        return 7;
    }

    aeg::pipeline::PipelineScheduler scheduler({1, 1, 1, 1});
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFrameOrchestrator orchestrator(registry, scheduler, frame_context, render_graph);
    if (!orchestrator.register_world_frame(world_scope, frame_host) || orchestrator.world_count() != 1 ||
        orchestrator.world_frame_count() != 1 || !orchestrator.initialize()) {
        return 8;
    }

    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.fixed_tick = 13;
    frame_desc.user_data = &frame_user_data;
    if (!world_scope.command_buffer().enqueue([entity = entity.value](aeg::ecs::World& world) {
            Transform* transform = world.get<Transform>(entity);
            if (transform == nullptr) {
                return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kNotFound,
                                                        "queued World command entity is missing");
            }
            transform->x = 5.0f;
            return aeg::foundation::Result::success();
        })) {
        return 9;
    }
    const auto frame_result = orchestrator.run_frame(frame_desc);
    if (!frame_result || !world_state.received_world || !world_state.received_user_data ||
        !world_state.command_applied || !frame_state.received_user_data || frame_state.observed_x != 7.0f ||
        frame_state.observed_revision != 1 || frame_state.observed_view_id != 0) {
        return 9;
    }
    if (!orchestrator.shutdown() || !world_scope.shutdown()) {
        return 10;
    }
    return 0;
}
