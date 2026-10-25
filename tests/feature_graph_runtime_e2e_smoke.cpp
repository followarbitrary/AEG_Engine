/**
 * @file feature_graph_runtime_e2e_smoke.cpp
 * @brief FeatureGraph asset, module contract and last-good runtime tests.
 */
#include "feature_clock_test_fixture.hpp"
#include <cassert>
#include <cstddef>
#include <iostream>
#include <algorithm>
#include <utility>
#include <vector>

#include <feature_graph_runtime_host.hpp>
#include <feature_graph_asset.hpp>
#include <feature_module_api.h>

namespace {
using namespace aeg::feature;
using namespace aeg::asset;
using namespace aeg::application::runtime;

uint8_t g_physics_input = 0;

FeatureGraphDocument graph() {
    FeatureGraphDocument document{};
    document.asset_uuid = "asset://startup";
    document.schema_version = 1;
    document.graph_version = 1;
    document.draft_revision = 3;
    document.layout_revision = 5;
    document.module_requirements = {"gameplay@1", "renderer@2"};
    const auto parameter_id = ParameterId("Initial").value();
    std::vector<uint8_t> parameter_blob{1, 0, 0, 0};
    for (unsigned byte = 0; byte < sizeof(parameter_id); ++byte)
        parameter_blob.push_back(static_cast<uint8_t>(parameter_id >> (byte * 8u)));
    parameter_blob.insert(parameter_blob.end(), {1, 0, 0, 0, 3, 0, 0, 0, 1, 2, 3});
    document.nodes.push_back({FeatureNodeId("LogicNode"), FeatureDefinitionId("Logic.Root"),
                              {1, 0, 0}, OwnerPath{{FeatureNodeId("Root")}}, std::move(parameter_blob)});
    document.nodes.push_back({FeatureNodeId("PhysicsNode"), FeatureDefinitionId("Physics.Resolve"),
                              {1, 0, 0}, OwnerPath{{FeatureNodeId("Root"), FeatureNodeId("Physics")}}, {}});
    document.edges.push_back({FeatureNodeId("LogicNode"), PortId("Out"), FeatureNodeId("PhysicsNode"),
                              PortId("In"), EdgeKind::kSnapshot, ClockId("LogicTick"),
                              ClockId("PhysicsTick"), 1, 8, 9});
    document.process_subgraphs.push_back({FeatureNodeId("LogicNode"), {"Idle", "Active"},
                                          {"Active->Idle", "Idle->Active"}, 11, "Active", {}});
    document.input_bindings = {"Player.Move", "Player.Fire"};
    document.variadic_slots = {"Effects"};
    document.extensions = {"vendor.example:debug"};
    document.contract_digest = 0x0102030405060708ull;
    return document;
}

FeatureDefinition definition(FeatureDefinitionId id, FeatureDomain domain, ClockId clock) {
    FeatureDefinition result{};
    result.id = id;
    result.version = {1, 0, 0};
    result.contract.definition_id = id;
    result.contract.version = result.version;
    result.contract.domains = to_mask(domain);
    result.contract.execution = id.value() == FeatureDefinitionId("Logic.Root").value()
                                    ? ExecutionKind::kProcess
                                    : ExecutionKind::kCall;
    result.contract.state = id.value() == FeatureDefinitionId("Logic.Root").value()
                                ? StatePolicy::kProcessState
                                : StatePolicy::kStateless;
    result.contract.scope = ScopeContract{ScopeId("World")};
    result.contract.clock = aeg::feature::make_periodic_clock_contract(clock, 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, result.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    result.contract.permissions.version = 1;
    result.contract.has_continuation = result.contract.execution == ExecutionKind::kProcess;
    if (id == FeatureDefinitionId("Logic.Root"))
        result.contract.parameters = {{ParameterId("Initial"), 1, 3, true}};
    result.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Int.v1"), 1},
                             {PortId("In"), PortDirection::kInput, ValueSchemaId("Int.v1"), 1}};
    result.execute = [id](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input,
                          std::span<const uint8_t> parameters) {
        if (id == FeatureDefinitionId("Logic.Root")) {
            const std::vector<ParameterContract> schema{{ParameterId("Initial"), 1, 3, true}};
            const auto decoded = decode_feature_parameters(parameters, schema);
            assert(decoded && decoded.value.front().payload == std::vector<uint8_t>({1, 2, 3}));
        }
        if (id == FeatureDefinitionId("Physics.Resolve")) {
            if (input.empty())
                return aeg::foundation::ResultValue<StateDelta>{
                    aeg::foundation::invalid_argument("Physics.Resolve did not receive its graph input"), {}};
            // Input contains only graph-delivered messages. Parameters use
            // the separate, complete envelope passed to the callback.
            g_physics_input = input.back();
        }
        StateDelta delta{token.state_version, 1};
        if (id == FeatureDefinitionId("Logic.Root"))
            delta.output_payload = {7};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    return result;
}

void test_feature_graph_asset_rejects_old_magic_and_roundtrips() {
    const auto encoded = encode_feature_graph_asset(graph());
    assert(encoded);
    const auto decoded = decode_feature_graph_asset(encoded.value);
    assert(decoded);
    assert(decoded.value.asset_uuid == "asset://startup");
    assert(decoded.value.schema_version == 1 && decoded.value.graph_version == 1);
    assert(decoded.value.draft_revision == 3 && decoded.value.layout_revision == 5);
    assert(decoded.value.module_requirements == graph().module_requirements);
    assert(decoded.value.nodes.size() == 2);
    assert(decoded.value.nodes[1].owner_path.nodes.size() == 2);
    assert(decoded.value.nodes[0].parameter_blob.size() == 23);
    assert(decoded.value.edges.size() == 1);
    assert(decoded.value.edges[0].kind == EdgeKind::kSnapshot && decoded.value.edges[0].version == 1 &&
           decoded.value.edges[0].capacity == 8);
    assert(decoded.value.process_subgraphs.size() == 1);
    assert(decoded.value.process_subgraphs[0].states == std::vector<std::string>({"Idle", "Active"}));
    assert(decoded.value.process_subgraphs[0].initial_state == "Active");
    assert(decoded.value.input_bindings == graph().input_bindings);
    assert(decoded.value.variadic_slots == graph().variadic_slots);
    assert(decoded.value.extensions == graph().extensions);
    assert(decoded.value.contract_digest == 0x0102030405060708ull);
    assert(decode_feature_graph_asset(std::vector<std::byte>{std::byte{'A'}, std::byte{'E'},
                                                              std::byte{'G'}, std::byte{'P'}})
               .result.code == aeg::foundation::ResultCode::kUnsupported);
}

void test_typed_parameters_survive_asset_decode_and_execution() {
    auto executable = definition(FeatureDefinitionId("Logic.Typed"), FeatureDomain::kLogic, ClockId("LogicTick"));
    executable.contract.state = StatePolicy::kInstanceState;
    ParameterContract amount{ParameterId("Amount"), 1, 4, true};
    amount.metadata = ParameterMetadata{};
    amount.metadata->type = ParameterType::kInt32;
    amount.metadata->default_value = int64_t{7};
    executable.contract.parameters = {amount};
    std::vector<int64_t> observed;
    bool second_round = false;
    executable.execute = [&](const InvocationToken& token, const StateSnapshot& state, std::span<const uint8_t>, std::span<const uint8_t> parameters) {
        const auto records = decode_feature_parameters(parameters, executable.contract.parameters);
        assert(!parameters.empty() && records && records.value.size() == 1);
        const auto typed = deserialize_parameter_value(records.value[0].payload, ParameterType::kInt32);
        assert(typed);
        assert(second_round ? state.payload == records.value[0].payload : state.payload.empty());
        observed.push_back(std::get<int64_t>(typed.value));
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = records.value[0].payload;
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    FeatureGraphDocument source{};
    source.asset_uuid = "asset://typed-parameters";
    source.schema_version = 1;
    source.graph_version = 1;
    source.contract_digest = registry.contract_digest();
    const auto explicit_bytes = encode_feature_parameters(std::vector<FeatureParameterRecord>{{amount.id,1,{4,0,0,0}}}, executable.contract.parameters);
    assert(explicit_bytes);
    source.nodes = {{FeatureNodeId("Explicit"), executable.id, executable.version, {}, explicit_bytes.value},
                    {FeatureNodeId("Default"), executable.id, executable.version, {}, {}}};
    const auto asset = encode_feature_graph_asset(source);
    assert(asset);
    FeatureRuntime runtime(registry);
    FeatureGraphRuntimeHost host(runtime, registry);
    assert(host.stage_asset(asset.value));
    assert(host.apply_pending());
    for (const auto& provenance : host.active()->provenance)
        assert(runtime.dispatch_call(provenance.instance_id));
    std::sort(observed.begin(), observed.end());
    assert(observed == std::vector<int64_t>({4,7}));
    second_round = true;
    for (const auto& provenance : host.active()->provenance)
        assert(runtime.dispatch_call(provenance.instance_id));
    // Publication namespaces node IDs by asset. The second-round callback
    // already checks each owner's bytes against its own decoded parameters.
    std::vector<std::vector<uint8_t>> stored;
    for (const auto& provenance : host.active()->provenance)
        stored.push_back(runtime.find_instance(provenance.instance_id)->state.payload);
    std::sort(stored.begin(), stored.end());
    assert(stored == std::vector<std::vector<uint8_t>>({{4, 0, 0, 0}, {7, 0, 0, 0}}));
}

void test_runtime_host_keeps_last_good_generation() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry,
        definition(FeatureDefinitionId("Logic.Root"), FeatureDomain::kLogic, ClockId("LogicTick"))));
    assert(aeg::feature::test::register_clock_test_definition(registry, definition(FeatureDefinitionId("Physics.Resolve"),
                                                    FeatureDomain::kPhysics, ClockId("PhysicsTick"))));
    FeatureRuntime runtime(registry);
    FeatureGraphRuntimeHost host(runtime, registry);
    auto staged_graph = graph();
    staged_graph.contract_digest = registry.contract_digest();
    const auto encoded = encode_feature_graph_asset(staged_graph);
    assert(encoded);
    const auto first = host.stage_asset(encoded.value);
    if (!first) {
        std::cerr << "stage failed: " << first.message << "\n";
    }
    assert(first);
    auto second_graph = graph();
    second_graph.asset_uuid = "asset://startup-2";
    second_graph.contract_digest = registry.contract_digest();
    const auto second_encoded = encode_feature_graph_asset(second_graph);
    assert(second_encoded);
    assert(host.stage_asset(second_encoded.value));
    assert(host.apply_pending());
    assert(host.generation() == runtime.generation());
    assert(host.active_documents().size() == 2);
    assert(host.active_documents()[0].asset_uuid == "asset://startup");
    assert(host.active_documents()[1].asset_uuid == "asset://startup-2");
    assert(host.active()->provenance.size() == 4);
    const auto instance = host.active()->provenance.front().instance_id;
    assert(runtime.find_instance(instance) != nullptr);
    assert(runtime.tick(ClockId("LogicTick")));
    const auto process = runtime.process(instance);
    assert(process);
    assert(runtime.find_process(process.value)->state.commit_count == 1);
    assert(runtime.find_process(process.value)->current_state == "Active");
    const auto physics = std::find_if(host.active()->provenance.begin(), host.active()->provenance.end(),
                                      [](const auto& item) {
                                          return item.definition_id == FeatureDefinitionId("Physics.Resolve");
                                      });
    assert(physics != host.active()->provenance.end());
    assert(runtime.dispatch_call(physics->instance_id));
    assert(g_physics_input == 7);
    assert(runtime.transition(process.value, "Idle"));
    assert(runtime.find_process(process.value)->current_state == "Idle");
    const auto generation = host.generation();
    FeatureGraphDocument invalid = graph();
    invalid.schema_version = 0;
    assert(!host.stage(invalid));
    assert(host.generation() == generation);
    FeatureGraphDocument stale = staged_graph;
    stale.contract_digest ^= 1u;
    assert(!host.stage(stale));
    assert(host.generation() == generation);
}

void test_queue_self_route_consumes_old_message_and_preserves_new_message() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto loop = definition(FeatureDefinitionId("Logic.QueueLoop"), FeatureDomain::kLogic,
                           ClockId("LogicTick"));
    loop.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Int.v1"), 1},
                           {PortId("In"), PortDirection::kInput, ValueSchemaId("Int.v1"), 1}};
    loop.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t>) {
        if (input.empty())
            return aeg::foundation::ResultValue<StateDelta>{
                aeg::foundation::invalid_argument("QueueLoop requires an input"), {}};
        StateDelta delta{token.state_version, 1};
        delta.output_payload = {static_cast<uint8_t>(input.back() + 1)};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, loop));
    FeatureRuntime runtime(registry);
    FeatureGraphRuntimeHost host(runtime, registry);
    FeatureGraphDocument queue{};
    queue.asset_uuid = "asset://queue-loop";
    queue.schema_version = 1;
    queue.graph_version = 1;
    queue.contract_digest = registry.contract_digest();
    queue.nodes.push_back({FeatureNodeId("QueueLoop"), loop.id, loop.version, {}, {}});
    queue.edges.push_back({FeatureNodeId("QueueLoop"), PortId("Out"), FeatureNodeId("QueueLoop"),
                           PortId("In"), EdgeKind::kQueue, ClockId("LogicTick"), ClockId("LogicTick"),
                           1, 1, 0});
    assert(host.stage(queue));
    assert(host.apply_pending());
    const auto instance = host.active()->provenance.front().instance_id;
    assert(runtime.dispatch_call(instance, std::vector<uint8_t>{1}));
    assert(runtime.find_instance(instance)->pending_input_messages == 1);
    assert(runtime.dispatch_call(instance));
    assert(runtime.find_instance(instance)->pending_input_messages == 1);
    assert(runtime.find_instance(instance)->pending_input.back() == 3);
}

void test_staged_graph_is_revalidated_when_registry_changes() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, definition(FeatureDefinitionId("Logic.Root"), FeatureDomain::kLogic,
                                                    ClockId("LogicTick"))));
    assert(aeg::feature::test::register_clock_test_definition(registry, definition(FeatureDefinitionId("Physics.Resolve"), FeatureDomain::kPhysics,
                                                    ClockId("PhysicsTick"))));
    FeatureRuntime runtime(registry);
    FeatureGraphRuntimeHost host(runtime, registry);
    auto staged = graph();
    staged.contract_digest = registry.contract_digest();
    assert(host.stage(staged));
    assert(aeg::feature::test::register_clock_test_definition(registry, definition(FeatureDefinitionId("Audio.New"), FeatureDomain::kAudio,
                                                    ClockId("AudioTick"))));
    const auto generation = host.generation();
    assert(!host.apply_pending());
    assert(host.generation() == generation);
}

void test_multiple_routes_to_one_target_share_declared_capacity() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry,
        definition(FeatureDefinitionId("Logic.Root"), FeatureDomain::kLogic, ClockId("LogicTick"))));
    assert(aeg::feature::test::register_clock_test_definition(registry, definition(FeatureDefinitionId("Physics.Resolve"), FeatureDomain::kPhysics,
                                                    ClockId("PhysicsTick"))));
    FeatureRuntime runtime(registry);
    FeatureGraphRuntimeHost host(runtime, registry);
    auto fanout = graph();
    fanout.asset_uuid = "asset://fanout-capacity";
    fanout.contract_digest = registry.contract_digest();
    fanout.edges.push_back({FeatureNodeId("LogicNode"), PortId("Out"), FeatureNodeId("PhysicsNode"),
                            PortId("In"), EdgeKind::kQueue, ClockId("LogicTick"), ClockId("PhysicsTick"),
                            1, 1, 0});
    assert(host.stage(fanout));
    assert(host.apply_pending());
    const auto logic = std::find_if(host.active()->provenance.begin(), host.active()->provenance.end(),
                                    [](const auto& item) {
                                        return item.definition_id == FeatureDefinitionId("Logic.Root");
                                    });
    assert(logic != host.active()->provenance.end());
    assert(!runtime.tick(ClockId("LogicTick")));
    const auto process = runtime.process(logic->instance_id);
    assert(process && runtime.find_process(process.value)->state.commit_count == 0);
}


void test_host_inspects_real_composite_owners_and_checks_process_commands() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    FeatureGraphRuntimeHost host(runtime, registry);
    auto logic = definition(FeatureDefinitionId("Inspect.Logic"), FeatureDomain::kLogic, ClockId("LogicTick"));
    logic.contract.execution = ExecutionKind::kProcess;
    logic.contract.state = StatePolicy::kProcessState;
    logic.contract.has_continuation = true;
    logic.contract.ports.clear();
    logic.owner_module = ModuleIdentity("Inspect.Module");
    std::vector<InvocationToken> entered;
    logic.execute = [&](const InvocationToken& token, const StateSnapshot& state,
                        std::span<const uint8_t>, std::span<const uint8_t>) {
        entered.push_back(token);
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{static_cast<uint8_t>(state.payload.empty() ? 1 : state.payload[0] + 1)};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    auto audio = logic;
    audio.id = FeatureDefinitionId("Inspect.Audio");
    audio.contract.definition_id = audio.id;
    audio.contract.domains = to_mask(FeatureDomain::kAudio);
    audio.contract.execution = ExecutionKind::kContinuous;
    audio.contract.state = StatePolicy::kInstanceState;
    audio.contract.has_continuation = false;
    audio.contract.clock.id = ClockId("AudioTick");
    auto nested = logic;
    nested.id = FeatureDefinitionId("Inspect.Nested");
    nested.contract.definition_id = nested.id;
    nested.contract.structure = FeatureStructure::kComposite; nested.contract.clock.work_budget_ns = 0;
    nested.contract.execution = ExecutionKind::kCall;
    nested.contract.state = StatePolicy::kStateless;
    nested.contract.has_continuation = false;
    nested.contract.children = {logic.id};
    nested.execute = {};
    auto outer = nested;
    outer.id = FeatureDefinitionId("Inspect.Outer");
    outer.contract.definition_id = outer.id;
    outer.contract.domains |= to_mask(FeatureDomain::kAudio);
    outer.contract.children = {nested.id, audio.id};
    assert(aeg::feature::test::register_clock_test_definition(registry, logic) && aeg::feature::test::register_clock_test_definition(registry, audio));
    assert(aeg::feature::test::register_clock_test_definition(registry, nested) && aeg::feature::test::register_clock_test_definition(registry, outer));
    FeatureGraphDocument a{};
    a.asset_uuid = "asset://inspect-a";
    a.schema_version = a.graph_version = 1;
    a.contract_digest = registry.contract_digest();
    a.nodes = {{FeatureNodeId("SameNode"), outer.id, outer.version},
               {FeatureNodeId("OtherNode"), outer.id, outer.version}};
    auto b = a;
    b.asset_uuid = "asset://inspect-b";
    b.nodes.resize(1);
    assert(!host.inspect_node(a.asset_uuid, a.nodes[0].node_id));
    assert(host.stage(a) && host.stage(b) && host.apply_pending());
    const auto inspect = [&](const auto& asset, FeatureNodeId node) {
        auto result = host.inspect_node(asset, node);
        assert(result && result.value.generation == runtime.generation() && result.value.owners.size() == 4);
        return result.value;
    };
    const auto selected = [](const auto& snapshot, FeatureDefinitionId id) {
        const auto item = std::find_if(snapshot.owners.begin(), snapshot.owners.end(),
                                      [&](const auto& owner) { return owner.definition_id == id; });
        assert(item != snapshot.owners.end());
        return *item;
    };
    auto first = inspect(a.asset_uuid, a.nodes[0].node_id);
    auto second = inspect(b.asset_uuid, b.nodes[0].node_id);
    const auto root = selected(first, outer.id);
    const auto leaf = selected(first, logic.id);
    const auto other = selected(second, logic.id);
    assert(root.processes.empty() && root.children.size() == 2 && leaf.processes.size() == 1);
    assert(leaf.owner.instance_id != root.owner.instance_id && leaf.owner != other.owner);
    assert(leaf.owner_path.nodes != other.owner_path.nodes && leaf.owner_module == logic.owner_module);
    assert(leaf.contract.domains == to_mask(FeatureDomain::kLogic) && leaf.contract.clock.id == ClockId("LogicTick"));
    assert(selected(first, audio.id).contract.domains == to_mask(FeatureDomain::kAudio));
    assert(!leaf.render_output && leaf.processes[0].state.commit_count == 0);
    assert(!leaf.invocations.last_issued && !leaf.invocations.last_failed && !leaf.invocations.last_committed &&
           leaf.invocations.open_attempts.empty() && !leaf.invocations.retry);
    assert(!root.invocations.last_issued && !root.invocations.last_committed);
    const auto provenance = std::find_if(host.active()->provenance.begin(), host.active()->provenance.end(),
        [&](const auto& row) { return row.instance_id == root.owner.instance_id; });
    assert(provenance != host.active()->provenance.end() && leaf.owner.node_id == provenance->node_id);
    const auto command = [&](const auto& snapshot, const auto& owner, RuntimeProcessAction action) {
        return RuntimeProcessCommand{snapshot.generation, snapshot.asset_uuid, snapshot.node_id,
            owner.owner, owner.processes[0].id, owner.processes[0].revision, action};
    };
    auto suspend = command(first, leaf, RuntimeProcessAction::kSuspend);
    auto foreign = command(first, other, RuntimeProcessAction::kSuspend);
    assert(!host.control_process(foreign));
    foreign = command(first, selected(inspect(a.asset_uuid, a.nodes[1].node_id), logic.id), RuntimeProcessAction::kSuspend);
    assert(!host.control_process(foreign));
    auto wrong = suspend;
    wrong.owner.resource_epoch++;
    assert(!host.control_process(wrong));
    wrong = suspend;
    wrong.process = other.processes[0].id;
    assert(!host.control_process(wrong));
    wrong = suspend;
    wrong.action = static_cast<RuntimeProcessAction>(255);
    assert(!host.control_process(wrong));
    assert(runtime.tick(ClockId("LogicTick")) && runtime.tick(ClockId("AudioTick")));
    const auto logic_commit = selected(inspect(a.asset_uuid, a.nodes[0].node_id), logic.id);
    const auto other_commit = selected(inspect(b.asset_uuid, b.nodes[0].node_id), logic.id);
    const auto audio_commit = selected(inspect(a.asset_uuid, a.nodes[0].node_id), audio.id);
    for (const auto* value : {&logic_commit, &other_commit, &audio_commit}) {
        assert(!value->render_output && value->invocations.last_committed &&
               value->invocations.last_issued->token == value->invocations.last_committed->attempt.token &&
               value->invocations.open_attempts.empty());
        assert(std::find(entered.begin(), entered.end(), value->invocations.last_committed->attempt.token) != entered.end());
    }
    assert(logic_commit.invocations.last_committed->attempt.process == leaf.processes[0].id &&
           logic_commit.invocations.last_committed->state_version == StateVersion(2));
    assert(logic_commit.invocations.last_committed->attempt.token != other_commit.invocations.last_committed->attempt.token);
    assert(!selected(inspect(a.asset_uuid, a.nodes[0].node_id), outer.id).invocations.last_issued &&
           !selected(inspect(a.asset_uuid, a.nodes[0].node_id), nested.id).invocations.last_committed);
    // Ordinary state commits do not invalidate a revision-based UI command.
    assert(host.control_process(suspend));
    auto suspended = selected(inspect(a.asset_uuid, a.nodes[0].node_id), logic.id);
    assert(suspended.processes[0].status == ProcessStatus::kSuspended && suspended.processes[0].state.payload == std::vector<uint8_t>{1});
    assert(suspended.owner == leaf.owner && suspended.processes[0].revision != leaf.processes[0].revision);
    assert(!host.control_process(suspend));
    assert(runtime.tick(ClockId("LogicTick")) && runtime.tick(ClockId("AudioTick")));
    assert(runtime.find_process(leaf.processes[0].id)->state.commit_count == 1);
    assert(runtime.find_process(other.processes[0].id)->state.commit_count == 2);
    assert(selected(inspect(a.asset_uuid, a.nodes[0].node_id), audio.id).state.commit_count == 2);
    assert(host.control_process(command(first, suspended, RuntimeProcessAction::kResume)));
    assert(runtime.tick(ClockId("LogicTick")));
    auto resumed = selected(inspect(a.asset_uuid, a.nodes[0].node_id), logic.id);
    assert(resumed.processes[0].state.commit_count == 2 && resumed.processes[0].state.payload == std::vector<uint8_t>{2});
    assert(host.control_process(command(first, resumed, RuntimeProcessAction::kCancel)));
    const auto cancelled = selected(inspect(a.asset_uuid, a.nodes[0].node_id), logic.id);
    assert(cancelled.processes[0].status == ProcessStatus::kCancelled && cancelled.owner.resource_epoch > resumed.owner.resource_epoch);
    assert(!host.control_process(command(first, cancelled, RuntimeProcessAction::kResume)));
    auto invalid = a;
    invalid.contract_digest++;
    assert(!host.stage(invalid));
    assert(inspect(a.asset_uuid, a.nodes[0].node_id).owners[0].owner == first.owners[0].owner);
    assert(selected(inspect(a.asset_uuid, a.nodes[0].node_id), logic.id).invocations.last_committed->attempt.token ==
           resumed.invocations.last_committed->attempt.token);
    assert(host.stage(a) && host.stage(b) && host.apply_pending());
    assert(!host.control_process(suspend));
    const auto fresh = inspect(a.asset_uuid, a.nodes[0].node_id);
    assert(fresh.generation != first.generation && fresh.owners[0].owner.instance_id != first.owners[0].owner.instance_id);
    assert(leaf.processes[0].state.commit_count == 0 && suspended.processes[0].state.commit_count == 1);
    for (const auto& owner : fresh.owners)
        assert(!owner.invocations.last_issued && !owner.invocations.last_failed && !owner.invocations.last_committed);
    assert(suspended.invocations.last_committed->attempt.token == logic_commit.invocations.last_committed->attempt.token);
    assert(!host.inspect_node("asset://missing", a.nodes[0].node_id));
    assert(!host.inspect_node(a.asset_uuid, FeatureNodeId("Missing")));
}

bool test_reconstructed_runtime_rejects_previous_session_commands_and_tokens() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto executable = definition(FeatureDefinitionId("Session.Process"), FeatureDomain::kLogic, ClockId("LogicTick"));
    executable.contract.execution = ExecutionKind::kProcess;
    executable.contract.state = StatePolicy::kProcessState;
    executable.contract.has_continuation = true;
    executable.contract.ports.clear();
    assert(aeg::feature::test::register_clock_test_definition(registry, executable));
    FeatureGraphDocument source{};
    source.asset_uuid = "asset://session";
    source.schema_version = source.graph_version = 1;
    source.contract_digest = registry.contract_digest();
    source.nodes = {{FeatureNodeId("Process"), executable.id, executable.version}};
    InvocationToken previous_token{};
    RuntimeProcessCommand previous_command{};
    {
        FeatureRuntime runtime(registry);
        FeatureGraphRuntimeHost host(runtime, registry);
        assert(host.stage(source) && host.apply_pending());
        const auto snapshot = host.inspect_node(source.asset_uuid, source.nodes[0].node_id);
        assert(snapshot && snapshot.value.owners.size() == 1);
        const auto& owner = snapshot.value.owners[0];
        assert(owner.processes.size() == 1);
        const auto token = runtime.invoke(owner.owner.instance_id);
        assert(token);
        previous_token = token.value;
        previous_command = {snapshot.value.generation, source.asset_uuid, source.nodes[0].node_id,
            owner.owner, owner.processes[0].id, owner.processes[0].revision, RuntimeProcessAction::kSuspend};
        assert(runtime.shutdown_execution());
    }
    FeatureRuntime runtime(registry);
    FeatureGraphRuntimeHost host(runtime, registry);
    assert(host.stage(source) && host.apply_pending());
    const auto current = host.inspect_node(source.asset_uuid, source.nodes[0].node_id);
    assert(current && current.value.owners.size() == 1);
    const auto token = runtime.invoke(current.value.owners[0].owner.instance_id);
    assert(token);
    StateDelta stale_delta{previous_token.state_version};
    stale_delta.state_payload = {99};
    const auto stale_completion = runtime.complete(previous_token, stale_delta);
    const auto stale_command = host.control_process(previous_command);
    if (stale_completion || stale_command)
        std::cerr << "previous session accepted: completion=" << bool(stale_completion)
                  << ", Process command=" << bool(stale_command) << '\n';
    if (stale_completion.code != aeg::foundation::ResultCode::kInvalidState ||
        stale_command.code != aeg::foundation::ResultCode::kInvalidState) return false;
    const auto untouched = host.inspect_node(source.asset_uuid, source.nodes[0].node_id);
    assert(untouched && untouched.value.owners[0].processes[0].status == ProcessStatus::kReady &&
           untouched.value.owners[0].processes[0].state.commit_count == 0 &&
           untouched.value.owners[0].invocations.open_attempts.size() == 1 &&
           untouched.value.owners[0].invocations.open_attempts[0].committable);
    StateDelta fresh_delta{token.value.state_version};
    fresh_delta.state_payload = {7};
    assert(runtime.complete(token.value, fresh_delta));
    const auto committed = host.inspect_node(source.asset_uuid, source.nodes[0].node_id);
    assert(committed && committed.value.owners[0].processes[0].state.payload == std::vector<uint8_t>{7});

    // A session that stays alive must remain distinct while another publishes,
    // replaces its definition and closes. A constructor-only seed is insufficient.
    FeatureRuntime other(registry);
    FeatureGraphRuntimeHost other_host(other, registry);
    assert(other_host.stage(source) && other_host.apply_pending());
    const auto held = other_host.inspect_node(source.asset_uuid, source.nodes[0].node_id);
    assert(held);
    const auto held_token = other.invoke(held.value.owners[0].owner.instance_id);
    assert(held_token);
    for (unsigned publication = 0; publication < 8; ++publication) {
        assert(host.stage(source) && host.apply_pending());
        assert(runtime.generation() != held.value.generation);
        assert(runtime.complete(held_token.value, StateDelta{held_token.value.state_version}).code ==
               aeg::foundation::ResultCode::kInvalidState);
    }
    auto replacement = executable;
    replacement.version.patch++;
    replacement.contract.version = replacement.version;
    assert(other.complete(held_token.value, StateDelta{held_token.value.state_version}));
    assert(runtime.replace_definition(replacement, {executable.version, replacement.version,
        executable.contract.state_payload.schema_version, replacement.contract.state_payload.schema_version}));
    assert(runtime.generation() != held.value.generation);
    assert(runtime.shutdown_execution());
    assert(runtime.generation() != held.value.generation);
    return true;
}

void test_feature_module_abi_is_feature_only() {
    static_assert(AEG_FEATURE_MODULE_ABI_MAJOR >= 2u);
    static_assert(AEG_CAP_FEATURES == (1ull << 6));
    AegFeatureExecutionApi api{};
    api.struct_size = sizeof(api);
    api.version = AEG_FEATURE_EXECUTION_API_VERSION;
    assert(api.struct_size >= sizeof(uint32_t) * 2);
    AegAbiModuleApi module_api{};
    module_api.features = nullptr;
}
}  // namespace

int main() {
    test_typed_parameters_survive_asset_decode_and_execution();
    test_feature_graph_asset_rejects_old_magic_and_roundtrips();
    test_runtime_host_keeps_last_good_generation();
    test_queue_self_route_consumes_old_message_and_preserves_new_message();
    test_staged_graph_is_revalidated_when_registry_changes();
    test_multiple_routes_to_one_target_share_declared_capacity();
    test_host_inspects_real_composite_owners_and_checks_process_commands();
    if (!test_reconstructed_runtime_rejects_previous_session_commands_and_tokens()) return 7;
    test_feature_module_abi_is_feature_only();
    std::cout << "feature graph runtime e2e smoke passed\n";
    return 0;
}
