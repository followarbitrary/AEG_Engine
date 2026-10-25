/**
 * @file feature_runtime_regression_smoke.cpp
 * @brief Regression coverage for Runtime P1 routing, composition, hot swap and retry.
 */
#include "feature_clock_test_fixture.hpp"
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <feature_graph.hpp>
#include <feature_registry.hpp>
#include <feature_runtime.hpp>
#include <feature_parameters.hpp>

namespace {
using namespace aeg::feature;

FeatureDefinition definition(std::string_view id, FeatureDomain domain, ClockId clock,
                             ExecutionKind execution = ExecutionKind::kCall,
                             StatePolicy state = StatePolicy::kInstanceState) {
    FeatureDefinition result{};
    result.id = FeatureDefinitionId(id);
    result.version = {1, 0, 0};
    result.contract.definition_id = result.id;
    result.contract.version = result.version;
    result.contract.domains = to_mask(domain);
    result.contract.execution = execution;
    result.contract.state = state;
    result.contract.scope = ScopeContract{ScopeId("World")};
    result.contract.clock = aeg::feature::make_periodic_clock_contract(clock, 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, result.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    result.contract.permissions.version = 1;
    result.contract.has_continuation = execution == ExecutionKind::kProcess;
    return result;
}

PortContract port(std::string_view id, PortDirection direction, std::string_view schema) {
    return {PortId(id), direction, ValueSchemaId(schema), 1};
}

FeatureGraphDocument graph_header(std::string_view asset) {
    FeatureGraphDocument graph{};
    graph.asset_uuid = std::string(asset);
    graph.schema_version = 1;
    graph.graph_version = 1;
    return graph;
}

FeatureInstanceId published_instance(const CompiledFeatureGraph& compiled, FeatureDefinitionId id) {
    for (const auto& provenance : compiled.provenance)
        if (provenance.definition_id == id) return provenance.instance_id;
    return {};
}

void test_state_payload_is_independent_preserved_and_clearable() {
    auto executable = definition("Logic.OwnedState", FeatureDomain::kLogic, ClockId("LogicTick"));
    executable.contract.state_payload.max_size = 4;
    executable.execute = [](const InvocationToken& token, const StateSnapshot& state,
                            std::span<const uint8_t> input, std::span<const uint8_t>) {
        assert(state.version == token.state_version);
        assert(state.schema_version == Version(1, 0, 0));
        StateDelta delta{};
        delta.expected_version = token.state_version;
        if (!input.empty() && input[0] == 1) {
            delta.state_payload = std::vector<uint8_t>{};
        } else if (input.empty()) {
            delta.state_payload = std::vector<uint8_t>{
                static_cast<uint8_t>(state.payload.empty() ? 1 : state.payload[0] + 1)};
        }
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    const auto expanded = registry.expand(executable.id, {});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto first = runtime.activate(expanded.value, ScopeId("World"));
    const auto second = runtime.activate(expanded.value, ScopeId("World"));
    assert(first && second);
    assert(runtime.dispatch_call(first.value));
    assert(runtime.dispatch_call(first.value));
    assert(runtime.dispatch_call(second.value));
    assert(runtime.find_instance(first.value)->state.payload == std::vector<uint8_t>{2});
    assert(runtime.find_instance(second.value)->state.payload == std::vector<uint8_t>{1});
    assert(runtime.dispatch_call(first.value, std::vector<uint8_t>{2}));
    assert(runtime.find_instance(first.value)->state.payload == std::vector<uint8_t>{2});
    assert(runtime.dispatch_call(first.value, std::vector<uint8_t>{1}));
    assert(runtime.find_instance(first.value)->state.payload.empty());
    assert(runtime.find_instance(second.value)->state.payload == std::vector<uint8_t>{1});
}

void test_process_state_payload_uses_declared_owner() {
    for (const auto policy : {StatePolicy::kInstanceState, StatePolicy::kProcessState}) {
        auto executable = definition("Logic.ProcessStateBytes", FeatureDomain::kLogic, ClockId("LogicTick"),
                                     ExecutionKind::kProcess, policy);
        size_t calls = 0;
        executable.execute = [&](const InvocationToken& token, const StateSnapshot& state,
                                  std::span<const uint8_t>, std::span<const uint8_t>) {
            assert(state.version == token.state_version);
            assert(state.current_state == (calls == 0 ? "Idle" : "Busy"));
            assert(state.payload == (calls == 0 ? std::vector<uint8_t>{} : std::vector<uint8_t>{5}));
            StateDelta delta{};
            delta.expected_version = token.state_version;
            delta.state_payload = std::vector<uint8_t>{static_cast<uint8_t>(5 + calls)};
            delta.next_state = calls++ == 0 ? "Busy" : "Done";
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
        };
        FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        assert(aeg::feature::test::register_clock_test_definition(registry, executable));
        auto graph = graph_header("regression://state-owner");
        graph.nodes = {{FeatureNodeId("Process"), executable.id, executable.version, {}, {}}};
        graph.process_subgraphs.push_back({FeatureNodeId("Process"), {"Idle", "Busy", "Done"},
                                           {"Idle->Busy", "Busy->Done"}, 2});
        auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
        assert(compiled);
        FeatureRuntime runtime(registry);
        assert(runtime.publish_graph(graph, compiled.value));
        const auto instance = published_instance(compiled.value, executable.id);
        const auto process = runtime.process(instance);
        assert(process && runtime.dispatch_process(instance) && runtime.dispatch_process(instance));
        const auto* active_instance = runtime.find_instance(instance);
        const auto* active_process = runtime.find_process(process.value);
        const auto& owner = policy == StatePolicy::kInstanceState ? active_instance->state : active_process->state;
        const auto& other = policy == StatePolicy::kInstanceState ? active_process->state : active_instance->state;
        assert(owner.payload == std::vector<uint8_t>{6} && owner.commit_count == 2);
        assert(owner.schema_version == Version(1, 0, 0));
        assert(other.payload.empty() && other.commit_count == 0);
        assert(active_process->state_schema_version == 2);
        assert(active_process->current_state == "Done" && active_process->status == ProcessStatus::kCompleted);
    }
}

void test_nonowners_reject_even_empty_state_updates() {
    for (const auto policy : {StatePolicy::kStateless, StatePolicy::kExternalSnapshot}) {
        auto executable = definition("Logic.NoLocalState", FeatureDomain::kLogic, ClockId("LogicTick"),
                                     ExecutionKind::kCall, policy);
        executable.execute = [](const InvocationToken& token, const StateSnapshot& state,
                                std::span<const uint8_t>, std::span<const uint8_t>) {
            assert(state.payload.empty() && state.schema_version == Version{});
            StateDelta delta{};
            delta.expected_version = token.state_version;
            delta.state_payload = std::vector<uint8_t>{};
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
        };
        FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        assert(aeg::feature::test::register_clock_test_definition(registry, executable));
        const auto expanded = registry.expand(executable.id, {});
        assert(expanded);
        FeatureRuntime runtime(registry);
        const auto instance = runtime.activate(expanded.value, ScopeId("World"));
        assert(instance && !runtime.dispatch_call(instance.value));
        assert(runtime.find_instance(instance.value)->state.payload.empty());
        assert(runtime.find_instance(instance.value)->state.commit_count == 0);
    }
}

void test_state_rejection_preserves_payload_input_and_output_queues() {
    auto source = definition("Logic.StateTransaction", FeatureDomain::kLogic, ClockId("LogicTick"));
    source.contract.state_payload.max_size = 1;
    source.contract.ports = {port("Out", PortDirection::kOutput, "Byte"), port("In", PortDirection::kInput, "Byte")};
    source.validate_state_payload = [](std::span<const uint8_t> payload) {
        return payload.empty() || payload[0] < 10 ? aeg::foundation::Result::success()
                                                 : aeg::foundation::invalid_argument("illegal business state");
    };
    auto target = definition("Logic.StateTarget", FeatureDomain::kLogic, ClockId("LogicTick"));
    target.contract.ports = source.contract.ports;
    size_t scenario = 0;
    FeatureRuntime* active_runtime = nullptr;
    FeatureInstanceId source_id;
    source.execute = [&](const InvocationToken& token, const StateSnapshot& state,
                          std::span<const uint8_t> input, std::span<const uint8_t>) {
        assert(state.payload == std::vector<uint8_t>{1});
        assert(input.size() == 1 && input[0] == 45);
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{2};
        if (scenario == 1) delta.state_payload = std::vector<uint8_t>{2, 3};
        if (scenario == 2) delta.state_payload = std::vector<uint8_t>{255};
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId(scenario == 0 ? "WrongSchema" : "Byte");
        message.schema_version = 1;
        message.payload = {77};
        delta.output_messages.push_back(message);
        if (scenario == 4) {
            const auto newer = active_runtime->invoke(source_id);
            assert(newer);
            StateDelta accepted{};
            accepted.state_payload = std::vector<uint8_t>{7};
            assert(active_runtime->complete(newer.value, accepted));
            assert(state.payload == std::vector<uint8_t>{1}); // owned snapshot, despite store replacement
        }
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, source) && aeg::feature::test::register_clock_test_definition(registry, target));
    auto graph = graph_header("regression://state-transaction");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kQueue, ClockId("LogicTick"), ClockId("LogicTick"), 1, 1, 0},
                   {FeatureNodeId("Target"), PortId("Out"), FeatureNodeId("Source"), PortId("In"),
                    EdgeKind::kQueue, ClockId("LogicTick"), ClockId("LogicTick"), 1, 1, 0}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    active_runtime = &runtime;
    assert(runtime.publish_graph(graph, compiled.value));
    source_id = published_instance(compiled.value, source.id);
    const auto target_id = published_instance(compiled.value, target.id);
    StateDelta seed{};
    seed.state_payload = std::vector<uint8_t>{1};
    seed.output_payload = {42};
    assert(runtime.complete(runtime.invoke(source_id).value, seed));
    StateDelta incoming{};
    incoming.output_payload = {45};
    assert(runtime.complete(runtime.invoke(target_id).value, incoming));
    const auto original_version = runtime.find_instance(source_id)->state.version;
    const auto original_delivery = runtime.find_instance(source_id)->pending_messages.front().delivery_id;
    for (scenario = 0; scenario < 5; ++scenario) {
        const auto result = runtime.dispatch_call(source_id);
        assert(!result);
        const auto* instance = runtime.find_instance(source_id);
        assert(instance->state.payload == std::vector<uint8_t>{static_cast<uint8_t>(scenario == 4 ? 7 : 1)});
        assert(instance->state.version.value() == original_version.value() + (scenario == 4 ? 1 : 0));
        assert(instance->state.commit_count == (scenario == 4 ? 2 : 1));
        assert(instance->pending_input == std::vector<uint8_t>{45});
        assert(instance->pending_messages.front().delivery_id == original_delivery);
        assert(runtime.find_instance(target_id)->pending_input == std::vector<uint8_t>{42});
    }
}

void test_retry_rejects_a_changed_state_snapshot(uint32_t backoff_ticks) {
    auto executable = definition("Logic.StateRetry", FeatureDomain::kLogic, ClockId("LogicTick"));
    executable.contract.failure = FailurePolicy::kRetry;
    executable.contract.retry = {2, backoff_ticks};
    FeatureRuntime* active_runtime = nullptr;
    FeatureInstanceId instance_id;
    size_t calls = 0;
    executable.execute = [&](const InvocationToken&, const StateSnapshot& state,
                              std::span<const uint8_t>, std::span<const uint8_t>) {
        ++calls;
        assert(state.payload == std::vector<uint8_t>{4});
        if (backoff_ticks == 0) {
            const auto newer = active_runtime->invoke(instance_id);
            assert(newer);
            StateDelta update{};
            update.state_payload = std::vector<uint8_t>{9};
            assert(active_runtime->complete(newer.value, update));
            assert(state.payload == std::vector<uint8_t>{4});
        }
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_argument("retry me"), {}};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    const auto expanded = registry.expand(executable.id, {});
    assert(expanded);
    FeatureRuntime runtime(registry);
    active_runtime = &runtime;
    const auto instance = runtime.activate(expanded.value, ScopeId("World"));
    assert(instance);
    instance_id = instance.value;
    StateDelta seed{};
    seed.state_payload = std::vector<uint8_t>{4};
    assert(runtime.complete(runtime.invoke(instance_id).value, seed));
    const auto earlier = runtime.invoke(instance_id);
    assert(earlier);
    const auto dispatched = runtime.dispatch_call(instance_id, std::vector<uint8_t>{3});
    if (backoff_ticks != 0) {
        assert(dispatched && runtime.find_instance(instance_id)->retry_pending);
        StateDelta update{};
        update.state_payload = std::vector<uint8_t>{9};
        assert(runtime.complete(earlier.value, update));
        assert(!runtime.tick(executable.contract.clock.id));
    } else assert(!dispatched);
    assert(calls == 1 && runtime.find_instance(instance_id)->state.payload == std::vector<uint8_t>{9});
}

bool test_state_retry_survives_pause_without_replaying_a_late_result() {
    auto executable = definition("Logic.PausedStateRetry", FeatureDomain::kLogic, ClockId("LogicTick"),
                                 ExecutionKind::kProcess, StatePolicy::kProcessState);
    executable.contract.failure = FailurePolicy::kRetry;
    executable.contract.retry = {3, 1};
    size_t calls = 0;
    executable.execute = [&](const InvocationToken& token, const StateSnapshot& state,
                              std::span<const uint8_t>, std::span<const uint8_t>) {
        assert(state.payload.empty());
        if (++calls <= 2)
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_argument("retry me"), {}};
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{3};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    const auto expanded = registry.expand(executable.id, {});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto instance = runtime.activate(expanded.value, ScopeId("World"));
    assert(instance && runtime.dispatch_process(instance.value));
    const auto process = runtime.process(instance.value);
    assert(process && runtime.suspend(process.value));
    assert(runtime.tick(executable.contract.clock.id) && calls == 1);
    if (!runtime.resume(process.value) || !runtime.tick(executable.contract.clock.id)) return false;
    if (calls != 2 || !runtime.find_instance(instance.value)->retry_pending) return false;
    return runtime.tick(executable.contract.clock.id) && calls == 3 &&
           runtime.find_process(process.value)->state.payload == std::vector<uint8_t>{3};
}

void test_state_payload_migration_is_all_owner_atomic() {
    auto current = definition("Logic.MigratedState", FeatureDomain::kLogic, ClockId("ClockA"));
    current.contract.state_payload.max_size = 1;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, current));
    const auto expanded = registry.expand(current.id, {});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto first = runtime.activate(expanded.value, ScopeId("World"));
    const auto second = runtime.activate(expanded.value, ScopeId("World"));
    assert(first && second);
    StateDelta seed{};
    seed.state_payload = std::vector<uint8_t>{1};
    assert(runtime.complete(runtime.invoke(first.value).value, seed));
    seed.state_payload = std::vector<uint8_t>{2};
    assert(runtime.complete(runtime.invoke(second.value).value, seed));
    const auto old_token = runtime.invoke(first.value);
    assert(old_token);
    const auto generation = runtime.generation();
    auto replacement = current;
    replacement.version = replacement.contract.version = {2, 0, 0};
    replacement.contract.state_payload.schema_version = {2, 0, 0};
    replacement.contract.clock.id = ClockId("ClockB");
    StateMigration migration{current.version, replacement.version, {1, 0, 0}, {2, 0, 0}};
    const auto registry_digest = registry.contract_digest();
    const auto unapproved = runtime.replace_definition(replacement, migration);
    assert(!unapproved && unapproved.code == aeg::foundation::ResultCode::kUnsupported);
    assert(runtime.generation() == generation && registry.contract_digest() == registry_digest &&
           registry.find_definition(current.id)->version == current.version);
    assert(runtime.find_instance(first.value)->state.payload == std::vector<uint8_t>{1});
    assert(runtime.find_instance(second.value)->state.payload == std::vector<uint8_t>{2});
    assert(runtime.clocks() == std::vector<ClockId>{ClockId("ClockA")});
    // Host approval precedes the original payload-migration rejection/recovery oracles.
    assert(aeg::feature::detail::admit_clock_request(registry, replacement.contract));
    assert(!runtime.replace_definition(replacement, migration)); // bytes cannot merely be relabeled
    size_t migrated = 0;
    migration.migrate_payload = [&](const StateSnapshot& state) {
        assert(state.schema_version == Version(1, 0, 0));
        if (++migrated == 2)
            return aeg::foundation::ResultValue<std::vector<uint8_t>>{aeg::foundation::invalid_argument("second owner failed"), {}};
        return aeg::foundation::ResultValue<std::vector<uint8_t>>{aeg::foundation::Result::success(), {8}};
    };
    assert(!runtime.replace_definition(replacement, migration) && migrated == 2);
    assert(runtime.generation() == generation && registry.find_definition(current.id)->version == current.version);
    assert(runtime.find_instance(first.value)->state.payload == std::vector<uint8_t>{1});
    assert(runtime.find_instance(second.value)->state.payload == std::vector<uint8_t>{2});
    assert(runtime.clocks() == std::vector<ClockId>{ClockId("ClockA")});
    migration.migrate_payload = [](const StateSnapshot& state) {
        return aeg::foundation::ResultValue<std::vector<uint8_t>>{aeg::foundation::Result::success(),
                                                                 {static_cast<uint8_t>(state.payload[0] + 3)}};
    };
    assert(runtime.replace_definition(replacement, migration));
    assert(runtime.generation() != generation);
    assert(runtime.find_instance(first.value)->state.payload == std::vector<uint8_t>{4});
    assert(runtime.find_instance(second.value)->state.payload == std::vector<uint8_t>{5});
    assert(runtime.find_instance(first.value)->state.schema_version == Version(2, 0, 0));
    assert(runtime.clocks() == std::vector<ClockId>{ClockId("ClockB")});
    StateDelta late{};
    late.state_payload = std::vector<uint8_t>{99};
    assert(!runtime.complete(old_token.value, late));
    auto same_schema = replacement;
    same_schema.version = same_schema.contract.version = {3, 0, 0};
    assert(runtime.replace_definition(same_schema, {replacement.version, same_schema.version, {2, 0, 0}, {2, 0, 0}}));
    assert(runtime.find_instance(first.value)->state.payload == std::vector<uint8_t>{4});
    auto changed_owner = same_schema;
    changed_owner.version = changed_owner.contract.version = {4, 0, 0};
    changed_owner.contract.state = StatePolicy::kProcessState;
    changed_owner.contract.execution = ExecutionKind::kProcess;
    changed_owner.contract.has_continuation = true;
    assert(!runtime.replace_definition(changed_owner, {same_schema.version, changed_owner.version, {2, 0, 0}, {2, 0, 0}}));
}

bool test_state_schema_callbacks_cannot_mutate_runtime() {
    auto executable = definition("Logic.PureStateSchema", FeatureDomain::kLogic, ClockId("LogicTick"));
    FeatureRuntime* active_runtime = nullptr;
    FeatureInstanceId instance_id;
    executable.validate_state_payload = [&](std::span<const uint8_t>) {
        return active_runtime->invoke(instance_id)
                   ? aeg::foundation::invalid_argument("schema validator created a mutable invocation")
                   : aeg::foundation::Result::success();
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(registry, executable)) return false;
    const auto expanded = registry.expand(executable.id, {});
    if (!expanded) return false;
    FeatureRuntime runtime(registry);
    active_runtime = &runtime;
    const auto instance = runtime.activate(expanded.value, ScopeId("World"));
    if (!instance) return false;
    instance_id = instance.value;
    const auto token = runtime.invoke(instance_id);
    if (!token) return false;
    StateDelta delta{};
    delta.state_payload = std::vector<uint8_t>{1};
    if (!runtime.complete(token.value, delta)) return false;
    auto replacement = executable;
    replacement.version = replacement.contract.version = {2, 0, 0};
    StateMigration migration{executable.version, replacement.version, {1, 0, 0}, {1, 0, 0}};
    migration.migrate_payload = [&](const StateSnapshot&) {
        if (runtime.invoke(instance_id))
            return aeg::foundation::ResultValue<std::vector<uint8_t>>{
                aeg::foundation::invalid_argument("state migration created a mutable invocation"), {}};
        return aeg::foundation::ResultValue<std::vector<uint8_t>>{aeg::foundation::Result::success(), {2}};
    };
    return runtime.replace_definition(replacement, migration) &&
           runtime.find_instance(instance_id)->state.payload == std::vector<uint8_t>{2};
}

bool test_stale_expansion_cannot_activate_old_state_schema() {
    auto child = definition("Logic.StaleStateChild", FeatureDomain::kLogic, ClockId("LogicTick"));
    auto parent = definition("Logic.StaleStateParent", FeatureDomain::kLogic, ClockId("LogicTick"),
                             ExecutionKind::kCall, StatePolicy::kStateless);
    parent.contract.structure = FeatureStructure::kComposite; parent.contract.clock.work_budget_ns = 0;
    parent.contract.children = {child.id};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(registry, child) || !aeg::feature::test::register_clock_test_definition(registry, parent)) return false;
    const auto stale = registry.expand(parent.id, {});
    if (!stale) return false;
    FeatureRuntime runtime(registry);
    auto replacement = child;
    replacement.version = replacement.contract.version = {2, 0, 0};
    replacement.contract.state_payload.schema_version = {2, 0, 0};
    if (!runtime.replace_definition(replacement, {child.version, replacement.version, {1, 0, 0}, {2, 0, 0}})) return false;
    if (runtime.activate(stale.value, ScopeId("World"))) return false;
    if (!runtime.clocks().empty()) return false; // no parent or earlier child was partially activated
    const auto live = registry.expand(parent.id, {});
    if (!live) return false;
    const auto activated = runtime.activate(live.value, ScopeId("World"));
    if (!activated || activated.value != FeatureInstanceId(1)) return false;
    const auto* instance = runtime.find_instance(activated.value);
    if (instance->children.size() != 1) return false;
    return runtime.find_instance(instance->children[0])->state.schema_version == Version(2, 0, 0);
}

bool test_registry_replacement_cannot_bypass_live_state_migration() {
    auto current = definition("Logic.RegistryStateBinding", FeatureDomain::kLogic, ClockId("LogicTick"));
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(registry, current)) return false;
    const auto expanded = registry.expand(current.id, {});
    if (!expanded) return false;
    FeatureRuntime runtime(registry);
    const auto instance = runtime.activate(expanded.value, ScopeId("World"));
    if (!instance) return false;
    StateDelta seed{};
    seed.state_payload = std::vector<uint8_t>{1};
    if (!runtime.complete(runtime.invoke(instance.value).value, seed)) return false;
    const auto earlier = runtime.invoke(instance.value);
    if (!earlier) return false;
    auto replacement = current;
    replacement.version = replacement.contract.version = {2, 0, 0};
    replacement.contract.state_payload.schema_version = {2, 0, 0};
    size_t calls = 0;
    replacement.execute = [&](const InvocationToken& token, const StateSnapshot&,
                               std::span<const uint8_t>, std::span<const uint8_t>) {
        ++calls;
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{2};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    if (!aeg::feature::test::replace_clock_test_definition(registry, replacement)) return false;
    StateDelta late{};
    late.state_payload = std::vector<uint8_t>{2};
    const auto completed = runtime.complete(earlier.value, late);
    const auto dispatched = runtime.dispatch_call(instance.value);
    return !completed && !dispatched && calls == 0 &&
           runtime.find_instance(instance.value)->state.payload == std::vector<uint8_t>{1} &&
           runtime.find_instance(instance.value)->state.schema_version == Version(1, 0, 0);
}

void test_typed_defaults_reach_the_callback() {
    auto executable = definition("Logic.TypedDefaults", FeatureDomain::kLogic, ClockId("LogicTick"));
    ParameterContract amount{ParameterId("Amount"), 1, 4, true};
    amount.metadata = ParameterMetadata{};
    amount.metadata->type = ParameterType::kInt32;
    amount.metadata->default_value = int64_t{7};
    executable.contract.parameters = {amount};
    size_t calls = 0;
    executable.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input,
                              std::span<const uint8_t> parameters) {
        if (calls == 0) assert(input.empty());
        else assert(input.size() == 1 && input[0] == 9);
        const auto values = decode_feature_parameters(parameters, executable.contract.parameters);
        assert(!parameters.empty() && values && values.value[0].payload == std::vector<uint8_t>({7,0,0,0}));
        ++calls;
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), {token.state_version, 1}};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    auto graph = graph_header("asset://typed-defaults");
    graph.contract_digest = registry.contract_digest();
    graph.nodes = {{FeatureNodeId("Typed"), executable.id, executable.version, {}, {}}};
    FeatureGraphCompiler compiler;
    auto compiled = compiler.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    assert(runtime.publish_graph(graph, compiled.value));
    const auto instance = published_instance(compiled.value, executable.id);
    assert(instance.is_valid());
    assert(runtime.dispatch_call(instance));
    assert(runtime.dispatch_call(instance, std::vector<uint8_t>{9}));
    assert(calls == 2);
}

void test_port_route_does_not_broadcast_ambiguous_payload() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.Source", FeatureDomain::kLogic, ClockId("LogicTick"));
    source.contract.ports = {port("OutA", PortDirection::kOutput, "Schema.A"),
                             port("OutB", PortDirection::kOutput, "Schema.B")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.output_payload = {9};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto target_a = definition("Logic.TargetA", FeatureDomain::kLogic, ClockId("LogicTick"));
    target_a.contract.ports = {port("InA", PortDirection::kInput, "Schema.A")};
    auto target_b = definition("Logic.TargetB", FeatureDomain::kLogic, ClockId("LogicTick"));
    target_b.contract.ports = {port("InB", PortDirection::kInput, "Schema.B")};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target_a));
    assert(aeg::feature::test::register_clock_test_definition(registry, target_b));

    auto graph = graph_header("regression://ports");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("TargetA"), target_a.id, target_a.version, {}, {}},
                   {FeatureNodeId("TargetB"), target_b.id, target_b.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("OutA"), FeatureNodeId("TargetA"), PortId("InA"),
                    EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 1, 0},
                   {FeatureNodeId("Source"), PortId("OutB"), FeatureNodeId("TargetB"), PortId("InB"),
                    EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 1, 0}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto target_a_id = published_instance(plan, target_a.id);
    const auto target_b_id = published_instance(plan, target_b.id);

    // A legacy opaque payload cannot identify either output port. It must not
    // be broadcast to both differently-schema routes.
    assert(!runtime.dispatch_call(source_id));
    assert(runtime.find_instance(target_a_id)->pending_input_messages == 0);
    assert(runtime.find_instance(target_b_id)->pending_input_messages == 0);
}

void test_port_message_preserves_schema_and_selects_target_port() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.MessageSource", FeatureDomain::kLogic, ClockId("LogicTick"));
    source.contract.ports = {port("OutA", PortDirection::kOutput, "Schema.A"),
                             port("OutB", PortDirection::kOutput, "Schema.B")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        PortMessage message{};
        message.source_port = PortId("OutA");
        message.schema = ValueSchemaId("Schema.A");
        message.schema_version = 1;
        message.payload = {7, 8};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto target_a = definition("Logic.MessageTargetA", FeatureDomain::kLogic, ClockId("LogicTick"));
    target_a.contract.ports = {port("InA", PortDirection::kInput, "Schema.A")};
    auto target_b = definition("Logic.MessageTargetB", FeatureDomain::kLogic, ClockId("LogicTick"));
    target_b.contract.ports = {port("InB", PortDirection::kInput, "Schema.B")};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target_a));
    assert(aeg::feature::test::register_clock_test_definition(registry, target_b));
    auto graph = graph_header("regression://port-message");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("TargetA"), target_a.id, target_a.version, {}, {}},
                   {FeatureNodeId("TargetB"), target_b.id, target_b.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("OutA"), FeatureNodeId("TargetA"), PortId("InA"),
                    EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 1, 0},
                   {FeatureNodeId("Source"), PortId("OutB"), FeatureNodeId("TargetB"), PortId("InB"),
                    EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 1, 0}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto target_a_id = published_instance(plan, target_a.id);
    const auto target_b_id = published_instance(plan, target_b.id);
    assert(runtime.dispatch_call(source_id));
    const auto* received = runtime.find_instance(target_a_id);
    const auto* rejected = runtime.find_instance(target_b_id);
    assert(received != nullptr && rejected != nullptr);
    assert(received->pending_input_messages == 1);
    assert(received->pending_input == std::vector<uint8_t>({7, 8}));
    assert(received->pending_messages.size() == 1);
    assert(received->pending_messages.front().source_port == PortId("OutA"));
    assert(received->pending_messages.front().target_port == PortId("InA"));
    assert(received->pending_messages.front().schema == ValueSchemaId("Schema.A"));
    assert(rejected->pending_input_messages == 0);
}

void test_render_capture_records_only_consumed_runtime_stamped_deliveries() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const ClockId clock("RenderReceiptTick");
    auto source = definition("Render.ReceiptSource", FeatureDomain::kRender, clock);
    source.contract.ports = {port("Out", PortDirection::kOutput, "LightList.v1")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{token.state_version};
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("LightList.v1");
        message.schema_version = 1;
        message.payload = {7, 8};
        message.delivery_id = MessageDeliveryId(999);
        message.source = {FeatureInstanceId(999), {InvocationId(999), AttemptId(999), RuntimeGeneration(999)},
                          999, 999, ClockId("Forged")};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    auto target = definition("Render.ReceiptTarget", FeatureDomain::kRender, clock);
    target.contract.ports = {port("In", PortDirection::kInput, "LightList.v1")};
    target.contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    target.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{token.state_version};
        delta.render_intent = RenderIntent{};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, source) && aeg::feature::test::register_clock_test_definition(registry, target));
    auto graph = graph_header("regression://render-receipt");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, clock, clock, 1, 1, 0}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    assert(runtime.publish_graph(graph, compiled.value));
    const auto source_id = published_instance(compiled.value, source.id);
    const auto target_id = published_instance(compiled.value, target.id);
    const auto captured_target = [&]() {
        auto capture = runtime.capture_render_outputs(compiled.value);
        assert(capture && capture.value.size() == 1);
        return capture.value.front();
    };
    assert(runtime.dispatch_call(source_id));
    const auto delivered = runtime.find_instance(target_id)->pending_messages.front();
    assert(delivered.source.instance == source_id);
    assert(delivered.source.invocation.generation == runtime.generation());
    assert(delivered.source.invocation.invocation_id != InvocationId(999));
    assert(delivered.source.invocation.attempt_id != AttemptId(999));
    assert(delivered.source.definition_digest == feature_contract_digest(source));
    assert(delivered.source.resource_epoch == runtime.find_instance(source_id)->render_resource_epoch);
    assert(delivered.source.clock == clock && delivered.delivery_id != MessageDeliveryId(999));

    // Explicit caller bytes do not confer authorization from a queued DataEdge.
    assert(runtime.dispatch_call(target_id, std::vector<uint8_t>{9}));
    assert(captured_target().input_messages.empty());
    assert(runtime.find_instance(target_id)->pending_messages.size() == 1);
    assert(runtime.dispatch_call(target_id));
    const auto captured = captured_target();
    assert(captured.input_messages.size() == 1);
    assert(captured.input_messages.front().source == delivered.source);
    assert(captured.input_messages.front().delivery_id == delivered.delivery_id);
    assert(captured.input_messages.front().source_port == PortId("Out"));
    assert(captured.input_messages.front().target_port == PortId("In"));
    assert(captured.input_messages.front().payload == std::vector<uint8_t>({7, 8}));
    assert(runtime.find_instance(target_id)->pending_messages.empty());

    // A rejected commit cannot replace the accepted intent's delivery evidence.
    assert(runtime.dispatch_call(source_id));
    const auto token = runtime.invoke(target_id);
    assert(token);
    StateDelta invalid{token.value.state_version};
    invalid.render_intent = RenderIntent{};
    invalid.render_intent->schema_version = 99;
    assert(!runtime.complete(token.value, invalid));
    assert(captured_target().input_messages.front().delivery_id == delivered.delivery_id);
    assert(runtime.find_instance(target_id)->pending_messages.size() == 1);
    // Host-issued invoke/complete does not claim that its callback consumed inputs.
    invalid.render_intent->schema_version = 1;
    assert(runtime.complete(token.value, invalid));
    assert(captured_target().input_messages.empty());
    assert(runtime.find_instance(target_id)->pending_messages.size() == 1);
    assert(runtime.dispatch_call(target_id));
    assert(captured_target().input_messages.front().delivery_id != delivered.delivery_id);
    // The owning capture survives retirement of the originating Runtime.
    assert(runtime.shutdown_execution());
    assert(captured.input_messages.front().source == delivered.source);
}

void test_resource_bus_projects_one_route_without_fanout_duplicates() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.FanoutSource", FeatureDomain::kLogic, ClockId("FanoutTick"));
    source.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("Payload.v1");
        message.schema_version = 1;
        message.payload = {11};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto target_a = definition("Logic.FanoutTargetA", FeatureDomain::kLogic, ClockId("FanoutTick"));
    target_a.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    auto target_b = definition("Logic.FanoutTargetB", FeatureDomain::kLogic, ClockId("FanoutTick"));
    target_b.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target_a));
    assert(aeg::feature::test::register_clock_test_definition(registry, target_b));

    auto graph = graph_header("regression://resource-bus-fanout");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("TargetA"), target_a.id, target_a.version, {}, {}},
                   {FeatureNodeId("TargetB"), target_b.id, target_b.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("TargetA"), PortId("In"),
                    EdgeKind::kData, ClockId("FanoutTick"), ClockId("FanoutTick"), 1, 0, 0},
                   {FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("TargetB"), PortId("In"),
                    EdgeKind::kData, ClockId("FanoutTick"), ClockId("FanoutTick"), 1, 0, 0}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto target_a_id = published_instance(plan, target_a.id);
    const auto target_b_id = published_instance(plan, target_b.id);
    assert(runtime.dispatch_call(source_id));
    assert(runtime.find_instance(target_a_id)->pending_messages.size() == 1);
    assert(runtime.find_instance(target_b_id)->pending_messages.size() == 1);
}

void test_snapshot_commit_replaces_latest_value() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.SnapshotSource", FeatureDomain::kLogic, ClockId("SnapshotTick"));
    source.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    uint8_t value = 0;
    source.execute = [&value](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("Payload.v1");
        message.schema_version = 1;
        message.payload = {++value};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto target = definition("Logic.SnapshotTarget", FeatureDomain::kLogic, ClockId("SnapshotTick"));
    target.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));
    auto graph = graph_header("regression://resource-bus-snapshot");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kSnapshot, ClockId("SnapshotTick"), ClockId("SnapshotTick"), 1, 0, 10}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto target_id = published_instance(plan, target.id);
    assert(runtime.dispatch_call(source_id));
    assert(runtime.dispatch_call(source_id));
    const auto* target_instance = runtime.find_instance(target_id);
    assert(target_instance->pending_messages.size() == 1);
    assert(target_instance->pending_messages.front().payload == std::vector<uint8_t>{2});
    assert(target_instance->pending_input == std::vector<uint8_t>{2});
}

void test_invalid_output_envelope_does_not_commit_feature_or_bus_state() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.InvalidEnvelopeSource", FeatureDomain::kLogic,
                             ClockId("InvalidEnvelopeTick"));
    source.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("Payload.v2");
        message.schema_version = 99;
        message.timestamp_ticks = 7;
        message.payload = {3};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto target = definition("Logic.InvalidEnvelopeTarget", FeatureDomain::kLogic,
                             ClockId("InvalidEnvelopeTick"));
    target.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));

    auto graph = graph_header("regression://invalid-output-envelope");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, ClockId("InvalidEnvelopeTick"),
                    ClockId("InvalidEnvelopeTick"), 1, 1, 0}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto target_id = published_instance(plan, target.id);
    const auto before = runtime.find_instance(source_id)->state;

    assert(!runtime.dispatch_call(source_id));
    assert(runtime.find_instance(source_id)->state.version == before.version);
    assert(runtime.find_instance(source_id)->state.commit_count == before.commit_count);
    assert(runtime.find_instance(target_id)->pending_messages.empty());
}

void test_snapshot_rejects_timestamp_regression_without_replacing_latest() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.TimestampSource", FeatureDomain::kLogic,
                             ClockId("TimestampTick"));
    source.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    uint64_t sequence = 0;
    source.execute = [&sequence](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("Payload.v1");
        message.schema_version = 1;
        message.timestamp_ticks = sequence++ == 0 ? 10 : 9;
        message.payload = {static_cast<uint8_t>(message.timestamp_ticks)};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto target = definition("Logic.TimestampTarget", FeatureDomain::kLogic,
                             ClockId("TimestampTick"));
    target.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));

    auto graph = graph_header("regression://snapshot-timestamp-regression");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kSnapshot, ClockId("TimestampTick"), ClockId("TimestampTick"),
                    1, 0, 100}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    std::atomic<uint64_t> host_now{10};
    FeatureRuntimeOptions options{};
    options.now_ns = [&host_now] { return host_now.load(); };
    FeatureRuntime runtime(registry, {}, options);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto target_id = published_instance(plan, target.id);
    assert(runtime.dispatch_call(source_id));
    assert(runtime.find_instance(target_id)->pending_messages.front().timestamp_ticks == 10);
    const auto before = runtime.find_instance(source_id)->state;

    // A host source regression cannot refresh or replace the last committed delivery.
    host_now.store(9);
    assert(!runtime.dispatch_call(source_id));
    const auto* target_instance = runtime.find_instance(target_id);
    assert(target_instance->pending_messages.size() == 1);
    assert(target_instance->pending_messages.front().timestamp_ticks == 10);
    assert(target_instance->pending_messages.front().payload == std::vector<uint8_t>{10});
    assert(runtime.find_instance(source_id)->state.version == before.version);
    assert(runtime.find_instance(source_id)->state.commit_count == before.commit_count);
}

void test_failed_state_commit_does_not_publish_bus_output() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.AtomicSource", FeatureDomain::kLogic, ClockId("AtomicTick"),
                             ExecutionKind::kProcess, StatePolicy::kProcessState);
    source.contract.has_continuation = true;
    source.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.next_state = "Missing";
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("Payload.v1");
        message.schema_version = 1;
        message.payload = {9};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto target = definition("Logic.AtomicTarget", FeatureDomain::kLogic, ClockId("AtomicTick"));
    target.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));
    auto graph = graph_header("regression://resource-bus-atomic");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, ClockId("AtomicTick"), ClockId("AtomicTick"), 1, 0, 0}};
    ProcessSubgraph process{};
    process.owner_node = FeatureNodeId("Source");
    process.states = {"Ready", "Done"};
    process.transitions = {"Ready->Done"};
    process.state_schema_version = 1;
    graph.process_subgraphs.push_back(process);
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto target_id = published_instance(plan, target.id);
    assert(!runtime.dispatch_process(source_id));
    assert(runtime.find_instance(target_id)->pending_messages.empty());
    const auto process_id = runtime.process(source_id);
    assert(process_id && runtime.find_process(process_id.value)->current_state == "Ready");
}

void test_self_route_consumes_old_input_and_keeps_new_input() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto feature = definition("Logic.SelfRoute", FeatureDomain::kLogic, ClockId("SelfTick"));
    feature.contract.ports = {port("In", PortDirection::kInput, "Payload.v1"),
                              port("Out", PortDirection::kOutput, "Payload.v1")};
    feature.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("Payload.v1");
        message.schema_version = 1;
        message.payload = {input.empty() ? uint8_t{1} : static_cast<uint8_t>(input.back() + 1)};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, feature));
    auto graph = graph_header("regression://resource-bus-self-route");
    graph.nodes = {{FeatureNodeId("Self"), feature.id, feature.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Self"), PortId("Out"), FeatureNodeId("Self"), PortId("In"),
                    EdgeKind::kQueue, ClockId("SelfTick"), ClockId("SelfTick"), 1, 8, 0}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto id = published_instance(plan, feature.id);
    assert(runtime.dispatch_call(id));
    assert(runtime.dispatch_call(id));
    const auto* instance = runtime.find_instance(id);
    assert(instance->pending_messages.size() == 1);
    assert(instance->pending_messages.front().payload == std::vector<uint8_t>{2});
    assert(instance->pending_input == std::vector<uint8_t>{2});
}

void test_composite_parent_output_reaches_child_boundary_target() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto child = definition("Logic.Child", FeatureDomain::kLogic, ClockId("LogicTick"),
                            ExecutionKind::kContinuous);
    child.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    child.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.output_payload = {42};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto composite = definition("Logic.Composite", FeatureDomain::kLogic, ClockId("LogicTick"));
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.state = StatePolicy::kStateless;
    composite.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    composite.contract.children = {child.id};
    composite.contract.child_port_mappings = {
        {PortId("Out"), child.id, PortId("Out"), PortDirection::kOutput}};
    auto target = definition("Logic.Target", FeatureDomain::kLogic, ClockId("LogicTick"));
    target.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    assert(aeg::feature::test::register_clock_test_definition(registry, child));
    assert(aeg::feature::test::register_clock_test_definition(registry, composite));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));

    auto graph = graph_header("regression://composite");
    graph.nodes = {{FeatureNodeId("Composite"), composite.id, composite.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Composite"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 1, 0}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto composite_id = published_instance(plan, composite.id);
    const auto target_id = published_instance(plan, target.id);
    const auto* composite_instance = runtime.find_instance(composite_id);
    assert(composite_instance != nullptr && composite_instance->children.size() == 1);

    assert(runtime.tick(ClockId("LogicTick")));
    assert(runtime.find_instance(target_id)->pending_input_messages == 1);
    assert(runtime.find_instance(target_id)->pending_input == std::vector<uint8_t>{42});
}

void test_composite_parent_input_reaches_mapped_child() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.ParentInputSource", FeatureDomain::kLogic, ClockId("LogicTick"));
    source.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("Payload.v1");
        message.schema_version = 1;
        message.payload = {31};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto child = definition("Logic.InputChild", FeatureDomain::kLogic, ClockId("LogicTick"),
                            ExecutionKind::kContinuous);
    child.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    uint8_t observed = 0;
    child.execute = [&observed](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t>) {
        if (!input.empty()) observed = input.back();
        StateDelta delta{};
        delta.expected_version = token.state_version;
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto composite = definition("Logic.InputComposite", FeatureDomain::kLogic, ClockId("LogicTick"));
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.state = StatePolicy::kStateless;
    composite.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    composite.contract.children = {child.id};
    composite.contract.child_port_mappings = {
        {PortId("In"), child.id, PortId("In"), PortDirection::kInput}};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, child));
    assert(aeg::feature::test::register_clock_test_definition(registry, composite));
    auto graph = graph_header("regression://composite-input");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Composite"), composite.id, composite.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Composite"), PortId("In"),
                    EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 1, 0}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto composite_id = published_instance(plan, composite.id);
    assert(runtime.dispatch_call(source_id));
    assert(runtime.find_instance(composite_id)->children.size() == 1);
    const auto child_id = runtime.find_instance(composite_id)->children.front();
    assert(runtime.find_instance(child_id)->pending_input_messages == 1);
    assert(runtime.tick(ClockId("LogicTick")));
    assert(observed == 31);
}

void test_composite_parent_input_fans_out_to_all_mapped_children() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto source = definition("Logic.FanoutCompositeSource", FeatureDomain::kLogic,
                             ClockId("CompositeFanoutTick"));
    source.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{};
        delta.expected_version = token.state_version;
        PortMessage message{};
        message.source_port = PortId("Out");
        message.schema = ValueSchemaId("Payload.v1");
        message.schema_version = 1;
        message.payload = {47};
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         std::move(delta)};
    };
    auto child_a = definition("Logic.FanoutChildA", FeatureDomain::kLogic,
                              ClockId("CompositeFanoutTick"));
    child_a.contract.ports = {port("InA", PortDirection::kInput, "Payload.v1")};
    auto child_b = definition("Logic.FanoutChildB", FeatureDomain::kLogic,
                              ClockId("CompositeFanoutTick"));
    child_b.contract.ports = {port("InB", PortDirection::kInput, "Payload.v1")};
    auto composite = definition("Logic.FanoutInputComposite", FeatureDomain::kLogic,
                                ClockId("CompositeFanoutTick"));
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.state = StatePolicy::kStateless;
    composite.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    composite.contract.children = {child_a.id, child_b.id};
    composite.contract.child_port_mappings = {
        {PortId("In"), child_a.id, PortId("InA"), PortDirection::kInput},
        {PortId("In"), child_b.id, PortId("InB"), PortDirection::kInput}};
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, child_a));
    assert(aeg::feature::test::register_clock_test_definition(registry, child_b));
    assert(aeg::feature::test::register_clock_test_definition(registry, composite));

    auto graph = graph_header("regression://composite-input-fanout");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Composite"), composite.id, composite.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Composite"), PortId("In"),
                    EdgeKind::kData, ClockId("CompositeFanoutTick"),
                    ClockId("CompositeFanoutTick"), 1, 1, 0}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto source_id = published_instance(plan, source.id);
    const auto composite_id = published_instance(plan, composite.id);
    assert(runtime.dispatch_call(source_id));
    const auto* composite_instance = runtime.find_instance(composite_id);
    assert(composite_instance != nullptr && composite_instance->children.size() == 2);

    const FeatureInstance* child_a_instance = nullptr;
    const FeatureInstance* child_b_instance = nullptr;
    for (const auto child_id : composite_instance->children) {
        const auto* child_instance = runtime.find_instance(child_id);
        assert(child_instance != nullptr);
        if (child_instance->definition_id == child_a.id) child_a_instance = child_instance;
        if (child_instance->definition_id == child_b.id) child_b_instance = child_instance;
    }
    assert(child_a_instance != nullptr && child_b_instance != nullptr);
    assert(child_a_instance->pending_input_messages == 1);
    assert(child_a_instance->pending_input == std::vector<uint8_t>({47}));
    assert(child_a_instance->pending_messages.front().target_port == PortId("InA"));
    assert(child_b_instance->pending_input_messages == 1);
    assert(child_b_instance->pending_input == std::vector<uint8_t>({47}));
    assert(child_b_instance->pending_messages.front().target_port == PortId("InB"));
}

void test_hot_replace_migrates_active_process_and_clock_plan() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto current = definition("Logic.HotSwap", FeatureDomain::kLogic, ClockId("ClockA"),
                              ExecutionKind::kProcess, StatePolicy::kProcessState);
    current.contract.has_continuation = true;
    assert(aeg::feature::test::register_clock_test_definition(registry, current));
    auto graph = graph_header("regression://hotswap");
    graph.nodes = {{FeatureNodeId("HotSwap"), current.id, current.version, {}, {}}};
    graph.process_subgraphs.push_back({FeatureNodeId("HotSwap"), {"Idle", "Done"},
                                       {"Idle->Done"}, 1});
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto instance_id = published_instance(plan, current.id);
    const auto process_id = runtime.process(instance_id);
    assert(process_id && runtime.find_process(process_id.value)->state_schema_version == 1);

    auto replacement = current;
    replacement.version = {2, 0, 0};
    replacement.contract.version = replacement.version;
    replacement.contract.clock.id = ClockId("ClockB");
    replacement.contract.state_payload.schema_version = {2, 0, 0};
    const StateMigration migration{current.version, replacement.version, {1, 0, 0}, {2, 0, 0}};
    assert(aeg::feature::detail::admit_clock_request(registry, replacement.contract));
    assert(runtime.replace_definition(replacement, migration));
    assert(runtime.find_process(process_id.value)->state_schema_version == 1);
    assert(runtime.find_process(process_id.value)->state.schema_version == Version(2, 0, 0));
    const auto clocks = runtime.clocks();
    assert(std::find(clocks.begin(), clocks.end(), ClockId("ClockB")) != clocks.end());
    assert(std::find(clocks.begin(), clocks.end(), ClockId("ClockA")) == clocks.end());
}

void test_retry_policy_retries_until_max_attempts() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto retrying = definition("Logic.Retry", FeatureDomain::kLogic, ClockId("LogicTick"),
                               ExecutionKind::kProcess, StatePolicy::kProcessState);
    retrying.contract.has_continuation = true;
    retrying.contract.failure = FailurePolicy::kRetry;
    retrying.contract.retry = RetryPolicy{3, 0};
    uint32_t attempts = 0;
    retrying.execute = [&attempts](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        ++attempts;
        if (attempts < 3) {
            return aeg::foundation::ResultValue<StateDelta>{
                aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                 "transient failure"),
                {}};
        }
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         StateDelta{token.state_version, 1}};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, retrying));
    const auto expanded = registry.expand(retrying.id, OwnerPath{});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto instance = runtime.activate(expanded.value, ScopeId("World.Instance"));
    assert(instance);
    assert(runtime.dispatch_process(instance.value));
    assert(attempts == 3);
    const auto process = runtime.process(instance.value);
    assert(process && runtime.find_process(process.value)->state.commit_count == 1);
}

void test_retry_policy_honors_backoff_ticks() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto retrying = definition("Logic.RetryBackoff", FeatureDomain::kLogic, ClockId("RetryTick"),
                               ExecutionKind::kProcess, StatePolicy::kProcessState);
    retrying.contract.has_continuation = true;
    retrying.contract.failure = FailurePolicy::kRetry;
    retrying.contract.retry = RetryPolicy{3, 1};
    uint32_t attempts = 0;
    retrying.execute = [&attempts](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        ++attempts;
        if (attempts < 3) {
            return aeg::foundation::ResultValue<StateDelta>{
                aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                 "transient failure"),
                {}};
        }
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         StateDelta{token.state_version, 1}};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, retrying));
    FeatureGraphDocument graph = graph_header("regression://retry-backoff");
    graph.nodes = {{FeatureNodeId("Retry"), retrying.id, retrying.version, {}, {}}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    auto plan = compiled.value;
    assert(runtime.publish_graph(graph, plan));
    const auto instance_id = published_instance(plan, retrying.id);
    assert(runtime.tick(ClockId("RetryTick")));
    assert(attempts == 1);
    assert(runtime.find_instance(instance_id)->retry_pending);
    const auto pending_generation = runtime.generation();
    assert(!runtime.dispatch_process(instance_id, std::vector<uint8_t>{9}));
    assert(!runtime.invoke(instance_id));
    assert(!runtime.retry(runtime.process(instance_id).value));
    auto replacement = retrying;
    replacement.version = {2, 0, 0};
    replacement.contract.version = replacement.version;
    assert(!runtime.replace_definition(replacement, {retrying.version, replacement.version, {1, 0, 0}, {1, 0, 0}}));
    assert(runtime.generation() == pending_generation && attempts == 1);
    assert(runtime.tick(ClockId("RetryTick")));
    assert(attempts == 2);
    assert(runtime.tick(ClockId("RetryTick")));
    assert(attempts == 3);
    const auto process = runtime.process(instance_id);
    assert(process && runtime.find_process(process.value)->state.commit_count == 1);
}

void test_retry_preserves_original_input_and_leaves_new_delivery(EdgeKind kind) {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const ClockId clock("FrozenRetryTick");
    auto source = definition("Render.RetrySource", FeatureDomain::kRender, clock);
    source.contract.ports = {port("Out", PortDirection::kOutput, "Payload.v1")};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t>) {
        StateDelta delta{token.state_version, 1};
        PortMessage message{};
        message.source_port = PortId("Out");
        message.timestamp_ticks = 10; // repeated delivery may have the same timestamp and payload
        message.payload.assign(input.begin(), input.end());
        delta.output_messages.push_back(std::move(message));
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    auto target = definition("Render.RetryTarget", FeatureDomain::kRender, clock,
                             ExecutionKind::kContinuous);
    target.contract.ports = {port("In", PortDirection::kInput, "Payload.v1")};
    target.contract.failure = FailurePolicy::kRetry;
    target.contract.retry = {2, 1};
    target.contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    std::vector<std::vector<uint8_t>> observed;
    target.execute = [&observed](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t>) {
        observed.emplace_back(input.begin(), input.end());
        if (observed.size() == 1)
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_state("transient"), {}};
        StateDelta delta{token.state_version, 1};
        delta.render_intent = RenderIntent{};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));
    auto graph = graph_header("regression://frozen-retry");
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    kind, clock, clock, 1, kind == EdgeKind::kQueue ? 4u : 1u, 100}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    assert(runtime.publish_graph(graph, compiled.value));
    const auto source_id = published_instance(compiled.value, source.id);
    const auto target_id = published_instance(compiled.value, target.id);
    const std::vector<uint8_t> first{1};
    // Snapshot/Data may replace an identical payload at the same source tick;
    // content comparison cannot distinguish this new delivery from the old one.
    const std::vector<uint8_t> next{1};
    assert(runtime.dispatch_call(source_id, first));
    const auto original = runtime.find_instance(target_id)->pending_messages.front();
    assert(runtime.tick(clock));
    assert(observed.size() == 1 && observed.front() == first);
    assert(runtime.find_instance(target_id)->retry_pending);
    const auto original_delivery = runtime.find_instance(target_id)->pending_messages.front().delivery_id;
    assert(runtime.dispatch_call(source_id, next));
    const auto replacement = runtime.find_instance(target_id)->pending_messages.back();
    assert(runtime.find_instance(target_id)->pending_messages.back().delivery_id != original_delivery);
    assert(runtime.tick(clock));
    // A retry sees the original input. Its commit must leave the new Queue,
    // Snapshot or capacity-one Data delivery for the following invocation.
    assert(observed.size() == 2 && observed.back() == first);
    assert(runtime.find_instance(target_id)->pending_input == next);
    auto captured = runtime.capture_render_outputs(compiled.value);
    assert(captured && captured.value.size() == 1 && captured.value.front().input_messages.size() == 1);
    assert(captured.value.front().input_messages.front().source == original.source);
    assert(captured.value.front().input_messages.front().delivery_id == original.delivery_id);
    assert(runtime.tick(clock));
    assert(observed.size() == 3 && observed.back() == next);
    captured = runtime.capture_render_outputs(compiled.value);
    assert(captured && captured.value.front().input_messages.size() == 1);
    assert(captured.value.front().input_messages.front().source == replacement.source);
    assert(captured.value.front().input_messages.front().delivery_id == replacement.delivery_id);
}

void test_instance_parameters_are_separate_from_explicit_input() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto feature = definition("Logic.ParameterInput", FeatureDomain::kLogic, ClockId("ParameterTick"));
    feature.contract.parameters = {{ParameterId(7), 1, 1, true}};
    std::vector<uint8_t> observed_parameters;
    std::vector<uint8_t> observed_input;
    feature.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input,
                           std::span<const uint8_t> parameters) {
        const auto decoded = decode_feature_parameters(parameters, feature.contract.parameters);
        assert(decoded && decoded.value.size() == 1);
        observed_parameters = decoded.value.front().payload;
        observed_input.assign(input.begin(), input.end());
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         StateDelta{token.state_version, 1}};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, feature));
    auto graph = graph_header("regression://parameter-input");
    const auto first = encode_feature_parameters(
        std::vector<FeatureParameterRecord>{{ParameterId(7), 1, {11}}}, feature.contract.parameters);
    const auto second = encode_feature_parameters(
        std::vector<FeatureParameterRecord>{{ParameterId(7), 1, {22}}}, feature.contract.parameters);
    assert(first && second);
    graph.nodes = {{FeatureNodeId("First"), feature.id, feature.version, {}, first.value},
                   {FeatureNodeId("Second"), feature.id, feature.version, {}, second.value}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    FeatureRuntime runtime(registry);
    assert(runtime.publish_graph(graph, compiled.value));
    const auto instance_for = [&](FeatureNodeId node) {
        for (const auto& provenance : compiled.value.provenance)
            if (provenance.node_id == node) return provenance.instance_id;
        return FeatureInstanceId{};
    };
    const std::vector<uint8_t> explicit_input{9};
    assert(runtime.dispatch_call(instance_for(FeatureNodeId("First")), explicit_input));
    assert(observed_parameters == std::vector<uint8_t>{11} && observed_input == explicit_input);
    assert(runtime.dispatch_call(instance_for(FeatureNodeId("Second"))));
    assert(observed_parameters == std::vector<uint8_t>{22} && observed_input.empty());

    auto replacement = feature;
    replacement.version = {2, 0, 0};
    replacement.contract.version = replacement.version;
    replacement.contract.parameters.front().version = 2;
    const auto generation = runtime.generation();
    assert(!runtime.replace_definition(replacement, {feature.version, replacement.version, {1, 0, 0}, {1, 0, 0}}));
    assert(runtime.generation() == generation && registry.find_definition(feature.id)->version == feature.version);
    assert(runtime.dispatch_call(instance_for(FeatureNodeId("First")), explicit_input));
    assert(observed_parameters == std::vector<uint8_t>{11});
}

void test_retry_snapshots_explicit_caller_input(uint32_t backoff_ticks) {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto feature = definition("Logic.ExplicitRetry", FeatureDomain::kLogic, ClockId("ExplicitRetryTick"),
                              ExecutionKind::kProcess, StatePolicy::kProcessState);
    feature.contract.failure = FailurePolicy::kRetry;
    feature.contract.retry = {2, backoff_ticks};
    std::vector<uint8_t> caller_input{3};
    std::vector<std::vector<uint8_t>> observed;
    feature.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input,
                           std::span<const uint8_t>) {
        observed.emplace_back(input.begin(), input.end());
        if (observed.size() == 1) {
            caller_input[0] = 9; // producer reuses its buffer after the first attempt
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_state("transient"), {}};
        }
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(),
                                                         StateDelta{token.state_version, 1}};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, feature));
    const auto expanded = registry.expand(feature.id, {});
    assert(expanded);
    FeatureRuntime runtime(registry);
    const auto instance = runtime.activate(expanded.value, ScopeId("World"));
    assert(instance && runtime.dispatch_process(instance.value, caller_input));
    if (backoff_ticks != 0) assert(runtime.tick(feature.contract.clock.id));
    assert(observed.size() == 2 && observed.front() == std::vector<uint8_t>{3});
    assert(observed.back() == observed.front());
}

void test_invocation_inspect_keeps_real_success_failure_and_retry_separate() {
    auto executable = definition("Logic.InspectAttempts", FeatureDomain::kLogic, ClockId("LogicTick"));
    executable.contract.failure = FailurePolicy::kRetry;
    executable.contract.retry = {2, 2};
    std::vector<InvocationToken> entered;
    bool fail = false;
    executable.execute = [&](const InvocationToken& token, const StateSnapshot& state,
                             std::span<const uint8_t>, std::span<const uint8_t>) {
        entered.push_back(token);
        if (fail) return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::not_ready("real retry failure"), {}};
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{static_cast<uint8_t>(state.payload.empty() ? 1 : state.payload[0] + 1)};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    FeatureRuntime runtime(registry);
    const auto id = runtime.activate(registry.expand(executable.id, {}).value, ScopeId("World")).value;
    auto initial = runtime.inspect_invocations(id);
    assert(initial && initial.value.open_attempts.empty() && !initial.value.last_issued &&
           !initial.value.last_failed && !initial.value.last_committed && !initial.value.retry);
    assert(runtime.dispatch_call(id));
    auto success = runtime.inspect_invocations(id).value;
    assert(entered.size() == 1 && success.last_issued->token == entered[0] &&
           success.last_committed->attempt.token == entered[0] && !success.last_failed &&
           success.last_committed->state_version == StateVersion(2) && success.open_attempts.empty());
    fail = true;
    assert(runtime.dispatch_call(id)); // Success here means a retry was scheduled, not a commit.
    auto waiting = runtime.inspect_invocations(id).value;
    assert(entered.size() == 2 && waiting.open_attempts.empty() && waiting.retry &&
           waiting.retry->origin == entered[1] && waiting.retry->next_attempt == 2 && waiting.retry->due_tick == 2);
    assert(waiting.last_issued->token == entered[1] && waiting.last_failed->attempt.token == entered[1] &&
           waiting.last_failed->stage == InvocationFailureStage::kCallback &&
           waiting.last_failed->result.code == aeg::foundation::ResultCode::kNotReady &&
           waiting.last_failed->result.message == "real retry failure" &&
           waiting.last_committed->attempt.token == entered[0]);
    assert(runtime.tick(executable.contract.clock.id) && entered.size() == 2);
    fail = false;
    assert(runtime.tick(executable.contract.clock.id));
    const auto retried = runtime.inspect_invocations(id).value;
    assert(entered.size() == 3 && entered[2].invocation_id != entered[1].invocation_id &&
           entered[2].attempt_id != entered[1].attempt_id && !retried.retry && retried.open_attempts.empty());
    assert(retried.last_committed->attempt.token == entered[2] && retried.last_failed->attempt.token == entered[1]);
    assert(runtime.find_instance(id)->state.payload == std::vector<uint8_t>{2});
    // Owning snapshots must not change when the Runtime advances.
    assert(waiting.last_committed->attempt.token == entered[0] && waiting.retry->next_attempt == 2);
    assert(!runtime.inspect_invocations(FeatureInstanceId(999999)));
}

void test_invocation_inspect_authenticates_manual_completions_and_retired_tokens() {
    auto executable = definition("Logic.InspectProcess", FeatureDomain::kLogic, ClockId("LogicTick"),
                                 ExecutionKind::kProcess, StatePolicy::kProcessState);
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    FeatureRuntime runtime(registry);
    auto graph = graph_header("inspect://manual-process");
    graph.nodes = {{FeatureNodeId("Process"), executable.id, executable.version}};
    ProcessSubgraph subgraph{};
    subgraph.owner_node = graph.nodes[0].node_id;
    subgraph.states = {"Ready", "Next"};
    subgraph.transitions = {"Ready->Next"};
    subgraph.state_schema_version = 1;
    graph.process_subgraphs.push_back(subgraph);
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry).value;
    assert(runtime.publish_graph(graph, compiled));
    const auto id = published_instance(compiled, executable.id);
    const auto process = runtime.process(id).value;
    const auto a = runtime.invoke(id).value;
    const auto b = runtime.invoke(id).value;
    auto open = runtime.inspect_invocations(id).value;
    assert(open.open_attempts.size() == 2 && open.open_attempts[0].attempt.token == a &&
           open.open_attempts[1].attempt.token == b && open.open_attempts[0].committable &&
           open.open_attempts[1].committable && open.last_issued->token == b && !open.last_committed);
    StateDelta rejected{b.state_version};
    rejected.next_state = "Undeclared";
    auto forged = b;
    forged.attempt_id = AttemptId(b.attempt_id.value() + 100);
    assert(!runtime.complete(forged, rejected) && !runtime.inspect_invocations(id).value.last_failed);
    assert(!runtime.complete(b, rejected));
    auto failure = runtime.inspect_invocations(id).value;
    assert(failure.last_failed->attempt.token == b && failure.last_failed->attempt.process == process &&
           failure.last_failed->stage == InvocationFailureStage::kCommit && failure.open_attempts.size() == 2 &&
           !failure.last_committed && runtime.find_process(process)->state.commit_count == 0);
    StateDelta accepted{b.state_version};
    accepted.state_payload = std::vector<uint8_t>{7};
    assert(runtime.complete(b, accepted));
    auto committed = runtime.inspect_invocations(id).value;
    assert(committed.last_committed->attempt.token == b && committed.last_committed->attempt.process == process &&
           committed.last_committed->state_version == StateVersion(2) &&
           committed.last_committed->process_revision == ProcessRevision(1));
    assert(committed.open_attempts.size() == 1 && committed.open_attempts[0].attempt.token == a &&
           !committed.open_attempts[0].committable);
    assert(!runtime.complete(a, accepted) && runtime.inspect_invocations(id).value.last_failed->attempt.token == b);
    const auto c = runtime.invoke(id).value;
    assert(runtime.suspend(process));
    auto suspended = runtime.inspect_invocations(id).value;
    assert(suspended.open_attempts.size() == 2 && !suspended.open_attempts[1].committable);
    assert(runtime.resume(process) && runtime.transition(process, "Next"));
    assert(runtime.find_process(process)->state.commit_count == 2 &&
           runtime.inspect_invocations(id).value.last_committed->attempt.token == b);
    auto replacement = executable;
    replacement.version = replacement.contract.version = {2, 0, 0};
    assert(runtime.replace_definition(replacement, {executable.version, replacement.version, {1, 0, 0}, {1, 0, 0}}));
    assert(!runtime.complete(c, accepted));
    auto retired = runtime.inspect_invocations(id).value;
    assert(retired.last_committed->attempt.token == b && retired.last_issued->token == c &&
           retired.last_issued->token.generation != runtime.generation());
    for (const auto& item : retired.open_attempts) assert(!item.committable);
    assert(runtime.shutdown_execution());
    assert(runtime.inspect_invocations(id).value.open_attempts.empty());
    assert(open.open_attempts.size() == 2 && open.open_attempts[0].committable && !open.last_committed);
}


void test_invocation_inspect_preserves_commit_when_render_publication_rejects() {
    auto executable = definition("Render.InspectPublication", FeatureDomain::kRender, ClockId("Frame"));
    executable.contract.permissions = {kAllRenderIntentCapabilities, 1};
    executable.contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    FeatureRuntime runtime(registry);
    auto graph = graph_header("inspect://publication");
    graph.nodes = {{FeatureNodeId("Draw"), executable.id, executable.version}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry).value;
    assert(runtime.publish_graph(graph, compiled));
    const auto id = published_instance(compiled, executable.id);
    const auto first = runtime.invoke(id).value;
    assert(runtime.complete(first, StateDelta{first.state_version}));
    const auto token = runtime.invoke(id).value;
    RenderDrawClusteredScene draw{};
    draw.scene = {RenderResourceId("Scene"), 1};
    draw.light_list = {RenderResourceId("MissingProducer"), 1, token.invocation_id, token.generation};
    draw.hdr.target = {RenderResourceId("Hdr"), 1, true};
    draw.depth = {{RenderResourceId("Depth"), 1, true}, RenderAttachmentLoad::kLoad, 1.f};
    StateDelta delta{token.state_version};
    delta.state_payload = std::vector<uint8_t>{7};
    delta.render_intent = RenderIntent{};
    delta.render_intent->operations = {draw};
    const auto result = runtime.complete(token, delta);
    assert(!result && result.code == aeg::foundation::ResultCode::kInvalidArgument);
    const auto observed = runtime.inspect_invocations(id).value;
    assert(runtime.find_instance(id)->state.commit_count == 2 && runtime.find_instance(id)->state.payload == std::vector<uint8_t>{7});
    assert(observed.last_committed->attempt.token == token && observed.last_committed->state_version == StateVersion(3) &&
           observed.last_failed->attempt.token == token && observed.last_failed->stage == InvocationFailureStage::kPublication &&
           observed.last_failed->result.message == result.message && observed.open_attempts.empty());
    assert(!runtime.complete(token, delta) && runtime.inspect_invocations(id).value.last_failed->stage == InvocationFailureStage::kPublication);
}

}  // namespace

int main() {
#if defined(_MSC_VER)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#endif
    if (!test_registry_replacement_cannot_bypass_live_state_migration()) {
        std::cerr << "Registry replacement bypassed live state migration\n";
        return 3;
    }
    const bool paused_retry_ok = test_state_retry_survives_pause_without_replaying_a_late_result();
    const bool stale_expansion_ok = test_stale_expansion_cannot_activate_old_state_schema();
    if (!paused_retry_ok || !stale_expansion_ok) {
        std::cerr << "state review regressions: paused_retry=" << paused_retry_ok
                  << ", stale_expansion=" << stale_expansion_ok << '\n';
        return 2;
    }
    if (!test_state_schema_callbacks_cannot_mutate_runtime()) {
        std::cerr << "state schema callback mutated Runtime during a transaction\n";
        return 1;
    }
    test_invocation_inspect_preserves_commit_when_render_publication_rejects();
    test_invocation_inspect_keeps_real_success_failure_and_retry_separate();
    test_invocation_inspect_authenticates_manual_completions_and_retired_tokens();
    test_state_payload_is_independent_preserved_and_clearable();
    test_process_state_payload_uses_declared_owner();
    test_nonowners_reject_even_empty_state_updates();
    test_state_rejection_preserves_payload_input_and_output_queues();
    test_retry_rejects_a_changed_state_snapshot(0);
    test_retry_rejects_a_changed_state_snapshot(1);

    test_state_payload_migration_is_all_owner_atomic();
    test_typed_defaults_reach_the_callback();
    test_port_route_does_not_broadcast_ambiguous_payload();
    test_port_message_preserves_schema_and_selects_target_port();
    test_render_capture_records_only_consumed_runtime_stamped_deliveries();
    test_resource_bus_projects_one_route_without_fanout_duplicates();
    test_snapshot_commit_replaces_latest_value();
    test_invalid_output_envelope_does_not_commit_feature_or_bus_state();
    test_snapshot_rejects_timestamp_regression_without_replacing_latest();
    test_failed_state_commit_does_not_publish_bus_output();
    test_self_route_consumes_old_input_and_keeps_new_input();
    test_composite_parent_output_reaches_child_boundary_target();
    test_composite_parent_input_reaches_mapped_child();
    test_composite_parent_input_fans_out_to_all_mapped_children();
    test_hot_replace_migrates_active_process_and_clock_plan();
    test_retry_policy_retries_until_max_attempts();
    test_retry_policy_honors_backoff_ticks();
    test_retry_preserves_original_input_and_leaves_new_delivery(EdgeKind::kQueue);
    test_retry_preserves_original_input_and_leaves_new_delivery(EdgeKind::kSnapshot);
    test_retry_preserves_original_input_and_leaves_new_delivery(EdgeKind::kData);
    test_instance_parameters_are_separate_from_explicit_input();
    test_retry_snapshots_explicit_caller_input(0);
    test_retry_snapshots_explicit_caller_input(1);
    std::cout << "feature runtime regression smoke passed\n";
    return 0;
}
