/**
 * @file feature_runtime_smoke.cpp
 * @brief Exercises Feature definition expansion and process lifecycle.
 */
#include "feature_clock_test_fixture.hpp"
#include <cassert>
#include <iostream>
#include <span>

#include <feature_registry.hpp>
#include <feature_runtime.hpp>

namespace {
using namespace aeg::feature;

FeatureDefinition make_definition(std::string_view id, FeatureDomain domain,
                                  ExecutionKind execution = ExecutionKind::kProcess,
                                  StatePolicy state = StatePolicy::kProcessState) {
    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId(id);
    definition.version = {1, 0, 0};
    definition.contract.definition_id = definition.id;
    definition.contract.version = definition.version;
    definition.contract.structure = FeatureStructure::kAtomic;
    definition.contract.domains = to_mask(domain);
    definition.contract.execution = execution;
    definition.contract.state = state;
    definition.contract.scope = ScopeContract{ScopeId("World")};
    definition.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("LogicTick"), 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    definition.contract.permissions.version = 1;
    definition.contract.has_continuation = execution == ExecutionKind::kProcess;
    definition.contract.ports.push_back({PortId("Input"), PortDirection::kInput,
                                         ValueSchemaId("Input.v1"), 1});
    definition.contract.side_effects.push_back({SideEffectId("State"), 1,
                                                SideEffectKind::kStateDelta,
                                                CommitPolicy::kOwnerBoundary});
    return definition;
}

void test_process_lifecycle_rejects_stale_completions() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, make_definition("Logic.Skill", FeatureDomain::kLogic)));
    const auto expanded = registry.expand(FeatureDefinitionId("Logic.Skill"), OwnerPath{});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto instance = runtime.activate(expanded.value, ScopeId("World.Instance"));
    assert(instance);
    const auto process = runtime.process(instance.value);
    assert(process);
    const auto first = runtime.invoke(instance.value);
    assert(first);
    assert(runtime.suspend(process.value));
    assert(!runtime.invoke(instance.value));
    assert(runtime.resume(process.value));
    const auto second = runtime.retry(process.value);
    assert(second);
    assert(second.value.attempt_id != first.value.attempt_id);
    assert(!runtime.complete(first.value, StateDelta{first.value.state_version, 1}));
    assert(runtime.complete(second.value, StateDelta{second.value.state_version, 2}));
}

void test_instance_snapshots_module_provenance() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto definition = make_definition("Logic.Owned", FeatureDomain::kLogic);
    definition.owner_module = ModuleIdentity("Game.Logic");
    assert(aeg::feature::test::register_clock_test_definition(registry, definition));
    const auto expanded = registry.expand(definition.id, OwnerPath{});
    assert(expanded && expanded.value.owner_module == definition.owner_module);
    FeatureRuntime runtime(registry);
    const auto instance = runtime.activate(expanded.value, ScopeId("World.Instance"));
    assert(instance);
    const auto* value = runtime.find_instance(instance.value);
    assert(value != nullptr && value->owner_module == definition.owner_module);
    const auto process = runtime.process(instance.value);
    assert(process && runtime.find_process(process.value)->owner_module == definition.owner_module);
}

void test_call_feature_requires_explicit_dispatch() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto definition = make_definition("Logic.Damage", FeatureDomain::kLogic,
                                      ExecutionKind::kCall, StatePolicy::kInstanceState);
    definition.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>)
        -> aeg::foundation::ResultValue<StateDelta> {
        return {aeg::foundation::Result::success(), StateDelta{token.state_version, 1}};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, definition));
    const auto expanded = registry.expand(definition.id, OwnerPath{});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto instance = runtime.activate(expanded.value, ScopeId("World.Instance"));
    assert(instance);
    assert(runtime.tick(ClockId("LogicTick")));
    assert(runtime.find_instance(instance.value)->state.commit_count == 0);
    assert(runtime.dispatch_call(instance.value));
    assert(runtime.find_instance(instance.value)->state.commit_count == 1);
}

void test_process_feature_dispatch_executes_once() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto definition = make_definition("Logic.ProcessDispatch", FeatureDomain::kLogic);
    definition.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>)
        -> aeg::foundation::ResultValue<StateDelta> {
        return {aeg::foundation::Result::success(), StateDelta{token.state_version, 7, {}, true}};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, definition));
    const auto expanded = registry.expand(definition.id, OwnerPath{});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto instance = runtime.activate(expanded.value, ScopeId("World.Instance"));
    assert(instance);
    assert(runtime.dispatch_process(instance.value));
    const auto process = runtime.process(instance.value);
    assert(process && runtime.find_process(process.value)->status == ProcessStatus::kCompleted);
    assert(runtime.find_process(process.value)->state.payload_revision == 7);
}

void test_recursive_composite_expansion_preserves_owner_paths() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto logic = make_definition("Logic.Child", FeatureDomain::kLogic,
                                 ExecutionKind::kCall, StatePolicy::kInstanceState);
    auto audio = make_definition("Audio.Child", FeatureDomain::kAudio,
                                 ExecutionKind::kCall, StatePolicy::kInstanceState);
    assert(aeg::feature::test::register_clock_test_definition(registry, logic));
    assert(aeg::feature::test::register_clock_test_definition(registry, audio));
    auto composite = make_definition("Gameplay.Composite", FeatureDomain::kLogic,
                                     ExecutionKind::kCall, StatePolicy::kStateless);
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.domains = to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kAudio);
    composite.contract.children = {logic.id, audio.id};
    assert(aeg::feature::test::register_clock_test_definition(registry, composite));
    const auto root = registry.expand(composite.id, OwnerPath{{FeatureNodeId("Root")}});
    assert(root);
    assert(root.value.children.size() == 2);
    assert(root.value.domains == composite.contract.domains);
    assert(root.value.children[0].owner_path.nodes.size() == 2);
    assert(root.value.children[1].owner_path.nodes.size() == 2);

    const auto first = registry.expand(logic.id, OwnerPath{{FeatureNodeId("A")}});
    const auto second = registry.expand(logic.id, OwnerPath{{FeatureNodeId("B")}});
    assert(first && second);
    assert(first.value.owner_path.nodes != second.value.owner_path.nodes);
}

void test_structural_recursion_is_rejected() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto a = make_definition("Composite.A", FeatureDomain::kLogic);
    auto b = make_definition("Composite.B", FeatureDomain::kLogic);
    a.contract.execution = ExecutionKind::kCall;
    a.contract.state = StatePolicy::kStateless;
    a.contract.has_continuation = false;
    b.contract.execution = ExecutionKind::kCall;
    b.contract.state = StatePolicy::kStateless;
    b.contract.has_continuation = false;
    a.contract.structure = FeatureStructure::kComposite; a.contract.clock.work_budget_ns = 0;
    b.contract.structure = FeatureStructure::kComposite; b.contract.clock.work_budget_ns = 0;
    a.contract.domains = to_mask(FeatureDomain::kLogic);
    b.contract.domains = to_mask(FeatureDomain::kLogic);
    a.contract.children = {b.id};
    b.contract.children = {a.id};
    assert(aeg::feature::test::register_clock_test_definition(registry, a));
    assert(aeg::feature::test::register_clock_test_definition(registry, b));
    assert(!registry.expand(a.id, OwnerPath{}));
}

void test_definition_replacement_requires_current_migration_origin() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto current = make_definition("Logic.HotSwap", FeatureDomain::kLogic,
                                   ExecutionKind::kProcess, StatePolicy::kProcessState);
    assert(aeg::feature::test::register_clock_test_definition(registry, current));
    FeatureRuntime runtime(registry);
    auto replacement = current;
    replacement.version = {2, 0, 0};
    replacement.contract.version = replacement.version;
    const auto generation = runtime.generation();
    const StateMigration wrong_origin{{9, 0, 0}, replacement.version, {1, 0, 0}, {2, 0, 0}};
    assert(!runtime.replace_definition(replacement, wrong_origin));
    assert(runtime.generation() == generation);
    replacement.contract.state_payload.schema_version = {2, 0, 0};
    const StateMigration migration{current.version, replacement.version, {1, 0, 0}, {2, 0, 0}};
    assert(runtime.replace_definition(replacement, migration));
    assert(runtime.generation() != generation);
}

void test_composite_contract_rejects_duplicate_children() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto composite = make_definition("Logic.DuplicateComposite", FeatureDomain::kLogic,
                                     ExecutionKind::kCall, StatePolicy::kStateless);
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.children = {FeatureDefinitionId("Logic.Child"), FeatureDefinitionId("Logic.Child")};
    assert(!aeg::feature::test::register_clock_test_definition(registry, composite));
}

void test_composite_expansion_rejects_incorrect_domain_union() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto child = make_definition("Audio.UnionChild", FeatureDomain::kAudio,
                                 ExecutionKind::kCall, StatePolicy::kStateless);
    assert(aeg::feature::test::register_clock_test_definition(registry, child));
    auto composite = make_definition("Logic.BadUnion", FeatureDomain::kLogic,
                                     ExecutionKind::kCall, StatePolicy::kStateless);
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.children = {child.id};
    assert(aeg::feature::test::register_clock_test_definition(registry, composite));
    assert(!registry.expand(composite.id, OwnerPath{}));
}

void test_shared_clock_contract_is_consistent() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto first = make_definition("Logic.ClockOwner", FeatureDomain::kLogic,
                                 ExecutionKind::kCall, StatePolicy::kStateless);
    assert(aeg::feature::test::register_clock_test_definition(registry, first));
    auto conflicting = make_definition("Physics.ClockOwner", FeatureDomain::kPhysics,
                                       ExecutionKind::kCall, StatePolicy::kStateless);
    conflicting.contract.clock.frequency_hz = 30;
    assert(!aeg::feature::test::register_clock_test_definition(registry, conflicting));
}

void test_continuous_service_aggregates_parallel_child_processes() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto child_a = make_definition("Logic.ServiceChildA", FeatureDomain::kLogic,
                                   ExecutionKind::kProcess, StatePolicy::kProcessState);
    auto child_b = make_definition("Logic.ServiceChildB", FeatureDomain::kLogic,
                                   ExecutionKind::kProcess, StatePolicy::kProcessState);
    auto service = make_definition("Logic.Service", FeatureDomain::kLogic,
                                   ExecutionKind::kContinuous, StatePolicy::kProcessState);
    service.contract.allows_child_process = true;
    service.contract.children = {child_a.id, child_b.id};
    assert(aeg::feature::test::register_clock_test_definition(registry, child_a));
    assert(aeg::feature::test::register_clock_test_definition(registry, child_b));
    // This is the first red assertion: the service contract explicitly owns
    // parallel child Processes, which the runtime must model as a parent
    // Process with independently versioned children.
    assert(aeg::feature::test::register_clock_test_definition(registry, service));

    const auto expanded = registry.expand(service.id, OwnerPath{});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto service_instance = runtime.activate(expanded.value, ScopeId("World.Service"));
    assert(service_instance);
    const auto parent_id = runtime.process(service_instance.value);
    assert(parent_id);
    const auto* parent = runtime.find_process(parent_id.value);
    assert(parent != nullptr && parent->children.size() == 2);

    const auto* service_value = runtime.find_instance(service_instance.value);
    assert(service_value != nullptr && service_value->children.size() == 2);
    const auto child_a_id = runtime.process(service_value->children[0]);
    const auto child_b_id = runtime.process(service_value->children[1]);
    assert(child_a_id && child_b_id && child_a_id.value != child_b_id.value);
    assert(runtime.find_process(child_a_id.value)->parent == parent_id.value);
    assert(runtime.find_process(child_b_id.value)->parent == parent_id.value);

    const auto child_a_token = runtime.invoke(service_value->children[0]);
    const auto child_b_token = runtime.invoke(service_value->children[1]);
    assert(child_a_token && child_b_token);
    assert(child_a_token.value.attempt_id != child_b_token.value.attempt_id);
    assert(child_a_token.value.state_version == child_b_token.value.state_version);

    const auto parent_token = runtime.invoke(service_instance.value);
    assert(parent_token);
    assert(!runtime.complete(parent_token.value,
                             StateDelta{parent_token.value.state_version, 1, {}, true}));
    assert(runtime.find_process(parent_id.value)->status == ProcessStatus::kReady);

    assert(runtime.suspend(parent_id.value));
    assert(runtime.find_process(child_a_id.value)->status == ProcessStatus::kSuspended);
    assert(runtime.find_process(child_b_id.value)->status == ProcessStatus::kSuspended);
    assert(runtime.resume(parent_id.value));
    assert(runtime.find_process(child_a_id.value)->status == ProcessStatus::kReady);
    assert(runtime.find_process(child_b_id.value)->status == ProcessStatus::kReady);

    // Resume advances the parent revision, so the pre-suspend token is
    // intentionally stale.  A resumed service must issue a fresh attempt.
    assert(!runtime.complete(parent_token.value,
                             StateDelta{parent_token.value.state_version, 2, {}, true}));
    const auto resumed_parent_token = runtime.invoke(service_instance.value);
    assert(resumed_parent_token);
    assert(!runtime.complete(child_a_token.value,
                             StateDelta{child_a_token.value.state_version, 1, {}, true}));
    assert(!runtime.complete(child_b_token.value,
                             StateDelta{child_b_token.value.state_version, 1, {}, true}));
    const auto resumed_child_a_token = runtime.invoke(service_value->children[0]);
    const auto resumed_child_b_token = runtime.invoke(service_value->children[1]);
    assert(resumed_child_a_token && resumed_child_b_token);

    assert(runtime.complete(resumed_child_a_token.value,
                            StateDelta{resumed_child_a_token.value.state_version, 1, {}, true}));
    assert(runtime.complete(resumed_child_b_token.value,
                            StateDelta{resumed_child_b_token.value.state_version, 1, {}, true}));
    assert(runtime.find_process(parent_id.value)->status == ProcessStatus::kReady);
    assert(runtime.complete(resumed_parent_token.value,
                            StateDelta{resumed_parent_token.value.state_version, 2, {}, true}));
    assert(runtime.find_process(parent_id.value)->status == ProcessStatus::kCompleted);

    const auto cancelled_instance = runtime.activate(expanded.value, ScopeId("World.CancelledService"));
    assert(cancelled_instance);
    const auto cancelled_parent = runtime.process(cancelled_instance.value);
    assert(cancelled_parent);
    const auto* cancelled_value = runtime.find_instance(cancelled_instance.value);
    assert(cancelled_value != nullptr && cancelled_value->children.size() == 2);
    const auto cancelled_child_token = runtime.invoke(cancelled_value->children.front());
    assert(cancelled_child_token);
    assert(runtime.cancel(cancelled_parent.value));
    assert(runtime.find_process(cancelled_parent.value)->status == ProcessStatus::kCancelled);
    for (const auto child : cancelled_value->children)
        assert(runtime.find_process(runtime.process(child).value)->status == ProcessStatus::kCancelled);
    assert(!runtime.complete(cancelled_child_token.value,
                             StateDelta{cancelled_child_token.value.state_version, 1, {}, true}));
}
}  // namespace

int main() {
    test_process_lifecycle_rejects_stale_completions();
    test_instance_snapshots_module_provenance();
    test_call_feature_requires_explicit_dispatch();
    test_process_feature_dispatch_executes_once();
    test_recursive_composite_expansion_preserves_owner_paths();
    test_structural_recursion_is_rejected();
    test_definition_replacement_requires_current_migration_origin();
    test_composite_contract_rejects_duplicate_children();
    test_composite_expansion_rejects_incorrect_domain_union();
    test_shared_clock_contract_is_consistent();
    test_continuous_service_aggregates_parallel_child_processes();
    std::cout << "feature runtime smoke passed\n";
    return 0;
}
