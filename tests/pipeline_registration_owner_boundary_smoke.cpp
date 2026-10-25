#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <engine_frame_orchestrator.hpp>
#include <rhi.hpp>
#include <world_scope.hpp>
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include "pipeline_compiler_execution.hpp"
#include "pipeline_scope_host_execution.hpp"

namespace aeg::pipeline {
class PipelineFeature;
class RenderFeature;
struct PipelineFeatureDeclaration;
struct FeatureDescriptor;
struct FeaturePointDescriptor;
struct FeatureBindingDescriptor;
struct FeatureInstanceConfiguration;
struct PipelineInstanceConfiguration;
}

namespace {
using namespace aeg;
struct Transform { int x{5}; };
struct AttackState {
    world::WorldScope* world{};
    ecs::EntityId entity{};
    bool ran{};
    void write() { world->state().get<Transform>(entity)->x = 7; ran = true; }
};
struct ArbitraryPipelineFactory {
    AttackState* state{};
    pipeline::PipelineDefinition operator()() const { state->write(); return {}; }
};
struct ArbitraryCollector {
    AttackState* state{};
    pipeline::PipelineDefinition operator()() const { state->write(); return {}; }
};
template<class Registry, class Input>
constexpr bool accepts_pipeline = requires(Registry& registry, Input input) { registry.register_pipeline(input); };
static_assert(!accepts_pipeline<pipeline::PipelineRegistry, ArbitraryPipelineFactory>);
static_assert(!accepts_pipeline<pipeline::PipelineRegistry, ArbitraryCollector>);
static_assert(accepts_pipeline<pipeline::PipelineRegistry, pipeline::PipelineDefinition>);

template<class Registry>
concept HasLegacyFeatureRegistry = requires { &Registry::register_feature; } ||
    requires { &Registry::unregister_feature; } || requires { &Registry::bind_feature; } ||
    requires { &Registry::unbind_feature; } || requires { &Registry::find_feature; } ||
    requires { &Registry::find_feature_descriptor; } || requires { &Registry::find_feature_declaration; };
template<class Builder>
concept HasLegacyFeatureBuilder = requires { &Builder::add_feature_point; } || requires { &Builder::bind_feature; };
template<class Definition>
concept HasLegacyFeatureRecords = requires(Definition value) { value.feature_points; } ||
    requires(Definition value) { value.feature_bindings; };
template<class Host>
concept HasLegacyConfiguration = requires { &Host::set_configuration; } || requires { &Host::configuration; };
template<class Input>
concept HasConfigurationInput = requires(Input value) { value.configuration; };
template<class Descriptor>
concept HasLegacyDomains = requires(Descriptor value) { value.domains; };
template<class Plan>
concept HasLegacyContributions = requires(Plan plan) { plan.feature_implementations; } ||
    requires(Plan plan) { plan.feature_contributions; } || requires(Plan plan) { plan.feature_owners; };
template<class T>
concept Complete = requires { sizeof(T); };
template<class Builder>
concept HasLegacyState = requires(const Builder& value) { value.state_input(); } ||
    requires(const Builder& value) { value.state_inputs(); } ||
    requires(const Builder& value) { value.state_output(); } ||
    requires(const Builder& value) { value.state_layer_count(); };
static_assert(!Complete<pipeline::PipelineFeature> && !Complete<pipeline::RenderFeature>);
static_assert(!Complete<pipeline::PipelineFeatureDeclaration> && !Complete<pipeline::FeatureDescriptor> &&
    !Complete<pipeline::FeaturePointDescriptor> && !Complete<pipeline::FeatureBindingDescriptor> &&
    !Complete<pipeline::FeatureInstanceConfiguration> && !Complete<pipeline::PipelineInstanceConfiguration>);
static_assert(!HasLegacyFeatureRegistry<pipeline::PipelineRegistry>);
static_assert(!HasLegacyFeatureBuilder<pipeline::PipelineBuilder>);
static_assert(!HasLegacyFeatureRecords<pipeline::PipelineDefinition>);
static_assert(!HasLegacyContributions<pipeline::PipelinePlan>);
static_assert(!HasLegacyConfiguration<pipeline::PipelineScopeHost>);
static_assert(!HasConfigurationInput<pipeline::PipelineCompileInput>);
static_assert(!HasLegacyDomains<pipeline::PipelineDescriptor>);
static_assert(!HasLegacyState<pipeline::RenderGraphBuilder>);

foundation::ResultValue<pipeline::PipelineDefinition> definition() {
    std::string name = "Owned definition";
    pipeline::PipelineBuilder builder({.id = pipeline::PipelineId{"Registration.Value"}, .name = name,
        .supported_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame)});
    if (!builder.add_stage({.id = pipeline::StageId{"Registration.Anchor"}, .phase = pipeline::EnginePhase::kPreUpdate}))
        return {builder.status(), {}};
    const auto marker = builder.add_stage({.id = pipeline::StageId{"Registration.Marker"},
        .phase = pipeline::EnginePhase::kPreUpdate});
    if (!marker) return {builder.status(), {}};
    if (const auto ordered = builder.runs_before(marker, pipeline::StageId{"Registration.Anchor"}); !ordered)
        return {ordered, {}};
    if (!builder.declare_artifact({.id = pipeline::ArtifactId{"Registration.ValueArtifact"},
            .type = pipeline::ArtifactTypeId{"Registration.Int"}, .owner_scope = pipeline::PipelineScope::kFrame,
            .storage = pipeline::ArtifactStorage::kMutableState,
            .lifetime = pipeline::ArtifactLifetime::kFrame})) return {builder.status(), {}};
    name[0] = 'X';
    return builder.finish();
}

bool rejected_values(pipeline::PipelineRegistry& registry, const pipeline::PipelineDefinition& original) {
    const auto before = registry.find_definition(original.descriptor.id);
    if (!before) return false;
    for (int attack = 0; attack < 12; ++attack) {
        auto malformed = original;
        switch (attack) {
            case 0: malformed.stages.push_back(malformed.stages.front()); break;
            case 1: malformed.stages[0].descriptor.phase = static_cast<pipeline::EnginePhase>(255); break;
            case 2: malformed.stages[0].descriptor.execution_domain = static_cast<pipeline::ExecutionDomain>(255); break;
            case 3: malformed.artifacts[0].descriptor.version = 0; break;
            case 4: malformed.artifacts[0].descriptor.storage = static_cast<pipeline::ArtifactStorage>(255); break;
            case 5: malformed.bindings.push_back({9, {malformed.artifacts[0].descriptor.id, pipeline::ArtifactAccess::kRead, 1}}); break;
            case 6: malformed.artifacts[0].descriptor.owner_scope = static_cast<pipeline::PipelineScope>(255); break;
            case 7: malformed.orderings.push_back({0, malformed.stages[0].descriptor.id, true}); break;
            case 8: malformed.exposures.push_back({malformed.artifacts[0].descriptor.id, pipeline::PipelineScope::kEngine}); break;
            case 9: malformed.bindings.push_back({0, {malformed.artifacts[0].descriptor.id, pipeline::ArtifactAccess::kRead,
                1, static_cast<pipeline::AccessFlags>(0x80000000u)}}); break;
            case 10: malformed.bindings.push_back({0, {malformed.artifacts[0].descriptor.id, pipeline::ArtifactAccess::kRead, 2}}); break;
            case 11: malformed.orderings.push_back({9, malformed.stages[0].descriptor.id, true}); break;
        }
        const auto rejected = registry.register_pipeline(std::move(malformed));
        const auto after = registry.find_definition(original.descriptor.id);
        if (rejected || rejected.code == foundation::ResultCode::kInvalidState || !after || after.value != before.value) {
            std::cerr << "Malformed aggregate reached registration or changed its immutable owner: " << attack << '\n';
            return false;
        }
    }
    return true;
}
}

int main(int argc, char** argv) {
    // The RED fixture recorded factory/build/collect executing actual ECS writes.
    // Every mode now reaches only the value API; callable inputs are ill-formed.
    const std::string_view mode = argc > 1 ? argv[1] : "collect";
    auto declared = definition();
    if (!declared || declared.value.descriptor.name != "Owned definition" || declared.value.stages.size() != 2 ||
        declared.value.stages[1].descriptor.id != pipeline::StageId{"Registration.Marker"}) return 42;
    auto copied = declared.value;
    auto moved = std::move(copied);
    pipeline::PipelineRegistry registry;
    if (!registry.register_pipeline(std::move(moved)) || !rejected_values(registry, declared.value)) return 13;
    const auto registered = registry.find_definition(declared.value.descriptor.id);
    if (!registered || registered.value->descriptor.name != "Owned definition" || registered.value->stages.size() != 2 ||
        registered.value->stages[1].descriptor.id != pipeline::StageId{"Registration.Marker"}) return 14;
    if (mode == "name") return 0;
    auto device = rhi::create_device({});
    if (!device) return 1;
    ecs::WorldManager worlds;
    const auto handle = worlds.create_world({"RegistrationTarget", true});
    if (!handle) return 2;
    const auto state = worlds.find(handle.value);
    if (!state || !state.value->register_component<Transform>(ecs::ComponentTypeId{"Transform"}, "Transform")) return 3;
    const auto entity = state.value->create_entity();
    if (!entity || !state.value->emplace<Transform>(entity.value, Transform{5})) return 4;
    world::WorldScope world(state.value);
    if (!world.initialize()) return 5;
    AttackState attack{&world, entity.value};
    ArbitraryPipelineFactory forbidden_factory{&attack};
    ArbitraryCollector forbidden_collector{&attack};
    (void)forbidden_factory; (void)forbidden_collector;
    pipeline::PipelineScopeHost host(pipeline::PipelineScope::kFrame, pipeline::PipelineInstanceId{"Registration.Instance"});
    if (!host.bind_pipeline(declared.value.descriptor.id) || !host.rebuild(registry) || !host.rebuild(registry)) return 7;
    const auto plan = pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host);
    if (!plan || plan->nodes.size() != 2 || plan->definition_owner != registered.value ||
        plan->topological_order.size() != 2 ||
        plan->nodes[plan->topological_order[0]].stage != pipeline::StageId{"Registration.Marker"} ||
        plan->nodes[plan->topological_order[1]].stage != pipeline::StageId{"Registration.Anchor"}) return 16;
    pipeline::PipelineScheduler scheduler;
    render_graph::FrameContext frame(*device.value);
    render_graph::RenderGraph graph(*device.value);
    engine::EngineFramePipelineHost services(registry, scheduler);
    engine::EngineFrameOrchestrator orchestrator(services, frame, graph);
    if (!orchestrator.register_world(world) ||
        !orchestrator.register_pipeline(engine::EngineFramePipelineRegistration(host)) ||
        !orchestrator.initialize() || !orchestrator.run_frame({.fixed_tick = 31})) return 8;
    const auto* current = world.state().get<Transform>(entity.value);
    const auto snapshot = world.latest_snapshot();
    if (!snapshot) return 9;
    const auto* published = snapshot.value->get<Transform>(entity.value);
    if (!current || !published || snapshot.value->revision() != 1) return 10;
    if (attack.ran || current->x != 5 || published->x != 5) {
        std::cerr << "Registration " << mode << " still executed ECS without a Feature invocation: "
            << current->x << ", published " << published->x << '\n';
        return 42;
    }
    if (!orchestrator.shutdown()) return 11;
    std::cout << "retired structural APIs absent; actual owned technical stages=" << plan->nodes.size() << '\n';
    return 0;
}
