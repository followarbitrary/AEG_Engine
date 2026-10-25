/**
 * @file feature_execution_inspection_smoke.cpp
 * @brief Published execution inspection retains root attribution and owning values.
 */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>

#include <editor_scene_runtime.hpp>
#include <feature_graph_runtime_host.hpp>

namespace {
using namespace aeg::feature;
using namespace aeg::application::runtime;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::foundation::ResultValue;

static_assert(std::same_as<decltype(std::declval<const aeg::application::EditorSceneRuntime&>()
    .inspect_feature_execution(std::string_view{})), ResultValue<FeatureRuntimeExecutionSnapshot>>);

unsigned callbacks = 0;

FeatureDefinition atomic(const char* name, FeatureDomain domain, const char* clock,
                         ExecutionKind execution = ExecutionKind::kCall) {
    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId(name);
    definition.version = {1, 0, 0};
    definition.owner_module = ModuleIdentity(std::string(name) + ".Module");
    definition.contract.definition_id = definition.id;
    definition.contract.version = definition.version;
    definition.contract.domains = to_mask(domain);
    definition.contract.execution = execution;
    definition.contract.state = execution == ExecutionKind::kProcess
        ? StatePolicy::kProcessState : StatePolicy::kStateless;
    definition.contract.has_continuation = execution == ExecutionKind::kProcess;
    definition.contract.scope = {ScopeId("World")};
    definition.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId(clock), 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    definition.contract.permissions.version = 1;
    definition.execute = [](const InvocationToken& token, const StateSnapshot&,
                            std::span<const uint8_t>, std::span<const uint8_t>) {
        ++callbacks;
        return ResultValue<StateDelta>{Result::success(), StateDelta{token.state_version}};
    };
    return definition;
}

FeatureDefinition composite(const char* name, std::vector<FeatureDefinitionId> children,
                            FeatureDomainMask domains) {
    auto definition = atomic(name, FeatureDomain::kLogic, "LogicTick");
    definition.contract.structure = FeatureStructure::kComposite; definition.contract.clock.work_budget_ns = 0;
    definition.contract.domains = domains;
    definition.contract.children = std::move(children);
    definition.execute = {};
    return definition;
}

struct Fixture final {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry};
    FeatureGraphRuntimeHost host{runtime, registry};
    FeatureDefinition logic = atomic("Inspect.Process", FeatureDomain::kLogic, "LogicTick", ExecutionKind::kProcess);
    FeatureDefinition audio = atomic("Inspect.Audio", FeatureDomain::kAudio, "AudioTick");
    FeatureDefinition physics = atomic("Inspect.Physics", FeatureDomain::kPhysics, "PhysicsTick");
    FeatureDefinition source = atomic("Inspect.Source", FeatureDomain::kLogic, "LogicTick");
    FeatureDefinition sink = atomic("Inspect.Sink", FeatureDomain::kLogic, "LogicTick");
    FeatureDefinition nested = composite("Inspect.Nested", {logic.id}, to_mask(FeatureDomain::kLogic));
    FeatureDefinition outer = composite("Inspect.Outer", {nested.id, audio.id},
        to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kAudio));

    Fixture() {
        source.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Value.v1"), 1}};
        sink.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Value.v1"), 1}};
        for (const auto* definition : {&logic, &audio, &physics, &source, &sink, &nested, &outer})
            assert(aeg::feature::test::register_clock_test_definition(registry, *definition));
    }

    FeatureGraphDocument graph(std::string asset, uint64_t version, uint64_t revision) const {
        FeatureGraphDocument graph{};
        graph.asset_uuid = std::move(asset);
        graph.schema_version = 1;
        graph.graph_version = version;
        graph.draft_revision = revision;
        graph.contract_digest = registry.contract_digest();
        const OwnerPath authored{{FeatureNodeId("AuthoredOwner"), FeatureNodeId("SameComposite")}};
        // The edge must move Source before Sink despite this authored order.
        graph.nodes = {{FeatureNodeId("Sink"), sink.id, sink.version, authored},
                       {FeatureNodeId("Source"), source.id, source.version, authored},
                       {FeatureNodeId("SameComposite"), outer.id, outer.version, authored},
                       {FeatureNodeId("Physics"), physics.id, physics.version, authored}};
        graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Sink"), PortId("In"),
                        EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1}};
        return graph;
    }
};

bool same_provenance(const FeatureNodeProvenance& left, const FeatureNodeProvenance& right) {
    return left.definition_id == right.definition_id && left.node_id == right.node_id &&
        left.instance_id == right.instance_id && left.process_id == right.process_id &&
        left.invocation_id == right.invocation_id && left.owner_path.nodes == right.owner_path.nodes &&
        left.clock == right.clock && left.domains == right.domains && left.owner_module == right.owner_module;
}

bool same_snapshot(const FeatureRuntimeExecutionSnapshot& left, const FeatureRuntimeExecutionSnapshot& right) {
    if (left.generation != right.generation || left.asset_uuid != right.asset_uuid ||
        left.source_path != right.source_path || left.graph_version != right.graph_version ||
        left.draft_revision != right.draft_revision || left.contract_digest != right.contract_digest ||
        left.plans.size() != right.plans.size() || left.provenance.size() != right.provenance.size()) return false;
    for (size_t index = 0; index != left.plans.size(); ++index)
        if (left.plans[index].clock != right.plans[index].clock ||
            left.plans[index].instances != right.plans[index].instances) return false;
    for (size_t index = 0; index != left.provenance.size(); ++index)
        if (left.provenance[index].authored_node_id != right.provenance[index].authored_node_id ||
            left.provenance[index].authored_owner_path.nodes != right.provenance[index].authored_owner_path.nodes ||
            left.provenance[index].authored_instance_path.nodes != right.provenance[index].authored_instance_path.nodes ||
            !same_provenance(left.provenance[index].runtime, right.provenance[index].runtime)) return false;
    return true;
}

const FeatureRuntimeExecutionProvenance& row(const FeatureRuntimeExecutionSnapshot& snapshot,
                                           FeatureDefinitionId definition) {
    const auto found = std::find_if(snapshot.provenance.begin(), snapshot.provenance.end(),
        [&](const auto& item) { return item.runtime.definition_id == definition; });
    assert(found != snapshot.provenance.end());
    return *found;
}

void assert_published_projection(const Fixture& fixture, const FeatureGraphDocument& authored,
                                 const FeatureRuntimeExecutionSnapshot& snapshot) {
    assert(snapshot.asset_uuid == authored.asset_uuid && snapshot.source_path.empty());
    assert(snapshot.generation == fixture.runtime.generation());
    const OwnerPath parent{{FeatureNodeId("AuthoredOwner"), FeatureNodeId("SameComposite")}};
    assert(row(snapshot, fixture.outer.id).authored_instance_path.nodes == parent.nodes);
    assert((row(snapshot, fixture.nested.id).authored_instance_path.nodes == std::vector<FeatureNodeId>{
        FeatureNodeId("AuthoredOwner"), FeatureNodeId("SameComposite"), FeatureNodeId("SameComposite"),
        FeatureNodeId("Inspect.Nested")}));
    assert((row(snapshot, fixture.logic.id).authored_instance_path.nodes == std::vector<FeatureNodeId>{
        FeatureNodeId("AuthoredOwner"), FeatureNodeId("SameComposite"), FeatureNodeId("SameComposite"),
        FeatureNodeId("Inspect.Nested"), FeatureNodeId("Inspect.Process")}));
    assert((row(snapshot, fixture.audio.id).authored_instance_path.nodes == std::vector<FeatureNodeId>{
        FeatureNodeId("AuthoredOwner"), FeatureNodeId("SameComposite"), FeatureNodeId("SameComposite"),
        FeatureNodeId("Inspect.Audio")}));
    assert(snapshot.graph_version == authored.graph_version && snapshot.draft_revision == authored.draft_revision &&
        snapshot.contract_digest == authored.contract_digest);
    assert(snapshot.plans.size() == 3 && snapshot.provenance.size() == 7);
    std::vector<FeatureInstanceId> owned;
    for (const auto& node : authored.nodes) {
        const auto live = fixture.host.inspect_node(authored.asset_uuid, node.node_id);
        assert(live);
        for (const auto& owner : live.value.owners) {
            owned.push_back(owner.owner.instance_id);
            const auto found = std::find_if(snapshot.provenance.begin(), snapshot.provenance.end(),
                [&](const auto& item) { return item.runtime.instance_id == owner.owner.instance_id; });
            assert(found != snapshot.provenance.end());
            assert(found->authored_node_id == node.node_id && found->authored_owner_path.nodes == node.owner_path.nodes);
            assert(found->runtime.definition_id == owner.definition_id && found->runtime.owner_module == owner.owner_module);
            assert(found->runtime.node_id == owner.owner.node_id && found->runtime.node_id != node.node_id);
            assert(found->runtime.clock == owner.contract.clock.id);
            assert(found->runtime.process_id == (owner.processes.empty() ? ProcessId{} : owner.processes.front().id));
            if (owner.owner.instance_id != live.value.owners.front().owner.instance_id)
                assert(found->runtime.owner_path.nodes == owner.owner_path.nodes);
            else {
                const auto published = std::find_if(fixture.host.active()->provenance.begin(), fixture.host.active()->provenance.end(),
                    [&](const auto& item) { return item.instance_id == owner.owner.instance_id; });
                assert(published != fixture.host.active()->provenance.end() && same_provenance(found->runtime, *published));
            }
            assert(owner.state.commit_count == 0 && !owner.invocations.last_issued);
        }
    }
    size_t selected_plan = 0;
    for (const auto& published : fixture.host.active()->plans) {
        std::vector<FeatureInstanceId> expected;
        for (const auto id : published.instances)
            if (std::find(owned.begin(), owned.end(), id) != owned.end()) expected.push_back(id);
        if (expected.empty()) continue;
        assert(selected_plan != snapshot.plans.size());
        const auto& actual = snapshot.plans[selected_plan++];
        assert(actual.clock == published.clock && actual.instances == expected);
        std::vector<FeatureDefinitionId> definitions;
        for (const auto id : actual.instances) {
            const auto item = std::find_if(snapshot.provenance.begin(), snapshot.provenance.end(),
                [&](const auto& value) { return value.runtime.instance_id == id; });
            assert(item != snapshot.provenance.end() && item->runtime.clock == actual.clock);
            definitions.push_back(item->runtime.definition_id);
        }
        if (actual.clock == ClockId("LogicTick"))
            assert(definitions == std::vector<FeatureDefinitionId>({fixture.source.id, fixture.outer.id,
                fixture.nested.id, fixture.logic.id, fixture.sink.id}));
        else if (actual.clock == ClockId("AudioTick")) assert(definitions == std::vector<FeatureDefinitionId>{fixture.audio.id});
        else assert(definitions == std::vector<FeatureDefinitionId>{fixture.physics.id});
    }
    assert(selected_plan == snapshot.plans.size());
    assert(row(snapshot, fixture.outer.id).runtime.domains ==
        (to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kAudio)));
    assert(row(snapshot, fixture.nested.id).runtime.domains == to_mask(FeatureDomain::kLogic));
    assert(row(snapshot, fixture.logic.id).runtime.domains == to_mask(FeatureDomain::kLogic));
    assert(row(snapshot, fixture.audio.id).runtime.domains == to_mask(FeatureDomain::kAudio));
    assert(row(snapshot, fixture.physics.id).runtime.domains == to_mask(FeatureDomain::kPhysics));
}

void published_execution_is_root_specific_read_only_and_owning() {
    callbacks = 0;
    FeatureRuntimeExecutionSnapshot retained;
    FeatureRuntimeExecutionSnapshot retained_expected;
    {
        Fixture fixture;
        auto a = fixture.graph("asset://inspection-a", 7, 11);
        auto b = fixture.graph("asset://inspection-b", 17, 21);
        assert(fixture.host.inspect_execution(a.asset_uuid).result.code == ResultCode::kNotReady);
        assert(fixture.host.stage(a) && fixture.host.stage(b) && fixture.host.apply_pending());
        const auto active_before = *fixture.host.active();
        const auto a_snapshot = fixture.host.inspect_execution(a.asset_uuid);
        const auto b_snapshot = fixture.host.inspect_execution(b.asset_uuid);
        assert(a_snapshot && b_snapshot);
        assert_published_projection(fixture, a, a_snapshot.value);
        assert_published_projection(fixture, b, b_snapshot.value);
        assert(callbacks == 0 && !fixture.host.has_pending());
        assert(fixture.host.inspect_execution("asset://missing").result.code == ResultCode::kInvalidArgument);
        assert(row(a_snapshot.value, fixture.logic.id).runtime.instance_id != row(b_snapshot.value, fixture.logic.id).runtime.instance_id);
        assert(row(a_snapshot.value, fixture.logic.id).runtime.node_id != row(b_snapshot.value, fixture.logic.id).runtime.node_id);
        assert(row(a_snapshot.value, fixture.logic.id).runtime.owner_path.nodes != row(b_snapshot.value, fixture.logic.id).runtime.owner_path.nodes);
        assert(fixture.host.active()->plans.size() == active_before.plans.size());
        for (size_t index = 0; index != active_before.plans.size(); ++index)
            assert(fixture.host.active()->plans[index].instances == active_before.plans[index].instances);
        auto pending = a;
        pending.graph_version = 77;
        pending.draft_revision = 99;
        pending.nodes.erase(pending.nodes.begin() + 3);
        assert(fixture.host.stage(pending));
        assert(same_snapshot(fixture.host.inspect_execution(a.asset_uuid).value, a_snapshot.value));
        auto invalid = pending;
        invalid.contract_digest ^= 1;
        assert(!fixture.host.stage(invalid) && !fixture.host.has_pending());
        assert(same_snapshot(fixture.host.inspect_execution(a.asset_uuid).value, a_snapshot.value));
        assert(fixture.host.stage(pending));
        assert(aeg::feature::test::register_clock_test_definition(fixture.registry, atomic("Inspect.New", FeatureDomain::kLogic, "LogicTick")));
        assert(!fixture.host.apply_pending());
        assert(same_snapshot(fixture.host.inspect_execution(a.asset_uuid).value, a_snapshot.value));
        retained = a_snapshot.value;
        retained_expected = retained;
        a.contract_digest = b.contract_digest = fixture.registry.contract_digest();
        ++a.graph_version;
        ++a.draft_revision;
        assert(fixture.host.stage(a) && fixture.host.stage(b) && fixture.host.apply_pending());
        const auto replacement = fixture.host.inspect_execution(a.asset_uuid);
        assert(replacement && replacement.value.generation != retained.generation);
        assert(row(replacement.value, fixture.logic.id).runtime.instance_id != row(retained, fixture.logic.id).runtime.instance_id);
        assert(same_snapshot(retained, retained_expected));
        // An external publication retires this host while its copied values remain usable.
        auto compiled = FeatureGraphCompiler{}.compile(a, fixture.registry);
        assert(compiled && fixture.runtime.publish_graph(a, compiled.value));
        assert(fixture.host.inspect_execution(a.asset_uuid).result.code == ResultCode::kInvalidState);
        assert(same_snapshot(retained, retained_expected));
    }
    assert(same_snapshot(retained, retained_expected));
    assert(callbacks == 0);
}

void inspection_rejects_retired_descendant_definitions_and_accepts_empty_roots() {
    Fixture fixture;
    auto a = fixture.graph("asset://inspection-a", 1, 1);
    FeatureGraphDocument empty{};
    empty.asset_uuid = "asset://empty";
    empty.schema_version = 1;
    empty.graph_version = 2;
    empty.draft_revision = 3;
    empty.contract_digest = fixture.registry.contract_digest();
    assert(fixture.host.stage(a) && fixture.host.stage(empty) && fixture.host.apply_pending());
    const auto snapshot = fixture.host.inspect_execution(empty.asset_uuid);
    assert(snapshot && snapshot.value.asset_uuid == empty.asset_uuid && snapshot.value.plans.empty() && snapshot.value.provenance.empty());
    assert(snapshot.value.graph_version == 2 && snapshot.value.draft_revision == 3);
    assert(!fixture.registry.unregister_definition(fixture.logic.id)); // Still referenced by a Composite.
    auto replacement = fixture.logic;
    ++replacement.version.patch; replacement.contract.version = replacement.version;
    assert(aeg::feature::test::replace_clock_test_definition(fixture.registry, std::move(replacement)));
    assert(fixture.host.inspect_execution(a.asset_uuid).result.code == ResultCode::kInvalidState);
}
}  // namespace

int main() {
    published_execution_is_root_specific_read_only_and_owning();
    inspection_rejects_retired_descendant_definitions_and_accepts_empty_roots();
    {
        Fixture fixture;
        const auto graph = fixture.graph("asset://observed-invocation", 1, 1);
        assert(fixture.host.stage(graph) && fixture.host.apply_pending());
        const auto before = fixture.host.inspect_execution(graph.asset_uuid);
        assert(before);
        const auto token = fixture.runtime.invoke(row(before.value, fixture.source.id).runtime.instance_id);
        assert(token);
        const auto observed = fixture.host.inspect_execution(graph.asset_uuid);
        if (!observed || row(observed.value, fixture.source.id).runtime.invocation_id != token.value.invocation_id) {
            std::cerr << "Published plan did not observe the root's issued invocation\n";
            std::exit(1);
        }
        assert(callbacks == 0); // Inspection observes an issued token without entering its callback.
    }
    std::cout << "Feature execution inspection smoke tests passed\n";
}
