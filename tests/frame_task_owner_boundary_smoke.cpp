#include <iostream>
#include <memory>
#include <utility>

#include <engine_frame_orchestrator.hpp>
#include <rhi.hpp>
#include <world_scope.hpp>
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include "engine_frame_inspection.hpp"

namespace {
struct Transform { int x{5}; };
struct AttackState {
    aeg::world::WorldScope* world{nullptr};
    aeg::ecs::EntityId entity{};
    bool callback_ran{false};
};

template<class Builder>
constexpr bool has_raw_task_api = requires(Builder& builder) {
    builder.add_task({});
    builder.bind_task({}, {});
};

template<class Node>
constexpr bool has_raw_callback = requires(Node node) { node.execute; } ||
    requires(Node node) { node.user_data; };

template<class Method> struct TaskParameter;
template<class Result, class Owner, class Argument>
struct TaskParameter<Result (Owner::*)(const Argument&)> { using type = Argument; };

template<class Builder>
aeg::foundation::Result declare_attack(Builder& builder, AttackState& state) {
    const auto stage = builder.add_stage({aeg::pipeline::StageId{"RawFrame.BusinessStage"},
        aeg::pipeline::EnginePhase::kPreUpdate, aeg::pipeline::ExecutionDomain::kMainThread});
    if (!stage) return builder.status();
    if constexpr (has_raw_task_api<Builder>) {
        using Descriptor = typename TaskParameter<decltype(&Builder::add_task)>::type;
        Descriptor declaration{};
        declaration.id = decltype(declaration.id){"RawFrame.BusinessWrite"};
        declaration.execute = [](auto& context) {
            auto* attack = static_cast<AttackState*>(context.user_data());
            auto* transform = attack->world->state().template get<Transform>(attack->entity);
            if (!transform) return aeg::foundation::invalid_state("raw Frame target is missing");
            transform->x = 7;
            attack->callback_ran = true;
            return aeg::foundation::Result::success();
        };
        declaration.user_data = &state;
        const auto task = builder.add_task(declaration);
        return task ? builder.bind_task(task, stage) : builder.status();
    }
    return aeg::foundation::Result::success();
}

[[nodiscard]] aeg::foundation::ResultValue<aeg::pipeline::PipelineDefinition> frame_attack_definition(AttackState& state) {
    aeg::pipeline::PipelineDescriptor descriptor{};
    descriptor.id = aeg::pipeline::PipelineId{"RawFrame.Business"};
    descriptor.name = "Raw Frame business injection";
    descriptor.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame);
    aeg::pipeline::PipelineBuilder builder(descriptor);
    if (const auto result = declare_attack(builder, state); !result) return {result, {}};
    return builder.finish();
}
}

int main() {
    auto device = aeg::rhi::create_device({});
    if (!device) return 1;
    aeg::ecs::WorldManager worlds;
    const auto handle = worlds.create_world({"RawFrameTarget", true});
    if (!handle) return 2;
    const auto state = worlds.find(handle.value);
    if (!state || !state.value->register_component<Transform>(aeg::ecs::ComponentTypeId{"Transform"}, "Transform")) return 3;
    const auto entity = state.value->create_entity();
    if (!entity || !state.value->emplace<Transform>(entity.value, Transform{5})) return 4;
    aeg::world::WorldScope world(state.value);
    if (!world.initialize()) return 5;
    AttackState attack{&world, entity.value};
    aeg::pipeline::PipelineRegistry registry;
    auto definition = frame_attack_definition(attack);
    if (!definition || !registry.register_pipeline(std::move(definition.value))) return 6;
    aeg::pipeline::PipelineScopeHost host(aeg::pipeline::PipelineScope::kFrame,
        aeg::pipeline::PipelineInstanceId{"RawFrame.Instance"});
    if (!host.bind_pipeline(aeg::pipeline::PipelineId{"RawFrame.Business"}) || !host.rebuild(registry)) return 7;
    aeg::pipeline::PipelineScheduler scheduler;
    aeg::render_graph::FrameContext frame(*device.value);
    aeg::render_graph::RenderGraph graph(*device.value);
    aeg::engine::EngineFramePipelineHost services(registry, scheduler);
    aeg::engine::EngineFrameOrchestrator orchestrator(services, frame, graph);
    if (!orchestrator.register_world(world) ||
        !orchestrator.register_pipeline(aeg::engine::EngineFramePipelineRegistration(host)) ||
        !orchestrator.initialize() || !orchestrator.run_frame({.fixed_tick = 23})) return 8;
    const auto* current = world.state().get<Transform>(entity.value);
    const auto snapshot = world.latest_snapshot();
    if (!snapshot) return 9;
    const auto* published = snapshot.value->get<Transform>(entity.value);
    if (!current || !published || snapshot.value->revision() != 1) return 9;
    if (attack.callback_ran || current->x != 5 || published->x != 5) {
        std::cerr << "Raw Frame Task executed ECS business without a Feature invocation owner: "
                  << current->x << ", published " << published->x << '\n';
        return 42;
    }
    const auto ordinary_trace = aeg::engine::detail::EngineFrameInspection::capture(orchestrator);
    if (!ordinary_trace || !ordinary_trace.value.stages.empty()) return 44;
    if constexpr (has_raw_task_api<aeg::pipeline::PipelineBuilder> || has_raw_callback<aeg::pipeline::PipelineNode>) {
        std::cerr << "Raw callback injection remains available behind the private boundary\n";
        return 43;
    }
    return orchestrator.shutdown() ? 0 : 10;
}
