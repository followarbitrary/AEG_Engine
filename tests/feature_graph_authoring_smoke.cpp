/**
 * @file feature_graph_authoring_smoke.cpp
 * @brief FeatureGraph authoring and compilation acceptance tests.
 */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <utility>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <feature_catalog.hpp>
#include <feature_graph.hpp>
#include <feature_process.hpp>

namespace {
using namespace aeg::feature;

FeatureDefinition definition(std::string_view id, FeatureDomain domain, ClockId clock) {
    FeatureDefinition result{};
    result.id = FeatureDefinitionId(id);
    result.version = {1, 0, 0};
    result.contract.definition_id = result.id;
    result.contract.version = result.version;
    result.contract.domains = to_mask(domain);
    result.contract.execution = ExecutionKind::kCall;
    result.contract.state = StatePolicy::kStateless;
    result.contract.scope = ScopeContract{ScopeId("World")};
    result.contract.clock = aeg::feature::make_periodic_clock_contract(clock, 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, result.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    result.contract.permissions.version = 1;
    result.contract.ports = {
        {PortId("Out"), PortDirection::kOutput, ValueSchemaId("Int.v1"), 1},
        {PortId("In"), PortDirection::kInput, ValueSchemaId("Int.v1"), 1},
    };
    return result;
}

void test_feature_graph_compiles_with_provenance_and_clock_plans() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto logic = definition("Logic.Intent", FeatureDomain::kLogic, ClockId("LogicTick"));
    logic.contract.execution = ExecutionKind::kProcess;
    logic.contract.state = StatePolicy::kProcessState;
    logic.contract.has_continuation = true;
    assert(aeg::feature::test::register_clock_test_definition(registry, std::move(logic)));
    assert(aeg::feature::test::register_clock_test_definition(registry, definition("Physics.Resolve", FeatureDomain::kPhysics,
                                                    ClockId("FixedTick"))));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://combat";
    graph.schema_version = 1;
    graph.graph_version = 2;
    graph.nodes.push_back({FeatureNodeId("IntentNode"), FeatureDefinitionId("Logic.Intent"),
                           {1, 0, 0}, OwnerPath{{FeatureNodeId("Root")}}, {}});
    graph.nodes.push_back({FeatureNodeId("ResolveNode"), FeatureDefinitionId("Physics.Resolve"),
                           {1, 0, 0}, OwnerPath{{FeatureNodeId("Root")}}, {}});
    graph.edges.push_back({FeatureNodeId("IntentNode"), PortId("Out"), FeatureNodeId("ResolveNode"),
                           PortId("In"), EdgeKind::kSnapshot, ClockId("LogicTick"),
                           ClockId("FixedTick"), 1, 4, 2});
    graph.process_subgraphs.push_back({FeatureNodeId("IntentNode"), {"Idle", "Done"},
                                       {"Done->Idle", "Idle->Idle"}, 1, "Done", {"Idle"}});
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    assert(compiled.value.plans.size() == 2);
    assert(compiled.value.provenance.size() == 2);
    assert(compiled.value.provenance.front().definition_id.is_valid());
    assert(compiled.value.provenance.front().node_id.is_valid());
    assert(compiled.value.provenance.front().owner_path.nodes.size() == 1);
    assert(compiled.value.provenance.front().domains == to_mask(FeatureDomain::kLogic));
    auto invalid_process = graph;
    invalid_process.process_subgraphs.front().initial_state = "Missing";
    assert(!FeatureGraphCompiler{}.compile(invalid_process, registry));
    invalid_process = graph;
    invalid_process.process_subgraphs.front().terminal_states = {"Missing"};
    assert(!FeatureGraphCompiler{}.compile(invalid_process, registry));
}

void test_process_state_machine_entry_must_match_marked_state() {
    ProcessStateMachine machine{};
    machine.initial_state = "Start";
    machine.states = {{"Start", {}, {}, false, false}, {"Done", {}, {}, true, true}};
    machine.transitions = {{"Start", "Done", {}, {}, 0}};
    assert(!validate_process_state_machine(machine));
}

void test_same_clock_unbuffered_cycle_is_rejected() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, definition("Logic.A", FeatureDomain::kLogic,
                                                    ClockId("LogicTick"))));
    assert(aeg::feature::test::register_clock_test_definition(registry, definition("Logic.B", FeatureDomain::kLogic,
                                                    ClockId("LogicTick"))));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://cycle";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes.push_back({FeatureNodeId("A"), FeatureDefinitionId("Logic.A"), {1, 0, 0}, {}, {}});
    graph.nodes.push_back({FeatureNodeId("B"), FeatureDefinitionId("Logic.B"), {1, 0, 0}, {}, {}});
    graph.edges.push_back({FeatureNodeId("A"), PortId("Out"), FeatureNodeId("B"), PortId("In"),
                           EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 0, 0});
    graph.edges.push_back({FeatureNodeId("B"), PortId("Out"), FeatureNodeId("A"), PortId("In"),
                           EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 0, 0});
    assert(!FeatureGraphCompiler{}.compile(graph, registry));
}

void test_edge_clock_provenance_must_match_definition_contracts() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, definition("Logic.Source", FeatureDomain::kLogic,
                                                    ClockId("LogicTick"))));
    assert(aeg::feature::test::register_clock_test_definition(registry, definition("Physics.Target", FeatureDomain::kPhysics,
                                                    ClockId("PhysicsTick"))));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://clock-mismatch";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), FeatureDefinitionId("Logic.Source"), {1, 0, 0}, {}, {}},
                   {FeatureNodeId("Target"), FeatureDefinitionId("Physics.Target"), {1, 0, 0}, {}, {}}};
    graph.edges.push_back({FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"),
                           PortId("In"), EdgeKind::kSnapshot, ClockId("WrongTick"),
                           ClockId("PhysicsTick"), 1, 1, 1});
    assert(!FeatureGraphCompiler{}.compile(graph, registry));
}

void test_compiler_and_editor_require_output_to_input_edges() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const auto source = definition("Logic.Source", FeatureDomain::kLogic, ClockId("LogicTick"));
    const auto target = definition("Logic.Target", FeatureDomain::kLogic, ClockId("LogicTick"));
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));
    FeatureCatalog catalog;
    assert(catalog.export_definition(source));
    assert(catalog.export_definition(target));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://port-directions";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    const OwnerPath source_owner{{FeatureNodeId("Root"), FeatureNodeId("SourceOwner")}};
    const OwnerPath target_owner{{FeatureNodeId("Root"), FeatureNodeId("TargetOwner")}};
    // Put the consumer first: the executable plan still follows the actual valid edge.
    graph.nodes = {{FeatureNodeId("Target"), target.id, target.version, target_owner, {}},
                   {FeatureNodeId("Source"), source.id, source.version, source_owner, {}}};
    const FeatureEdge valid{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                            EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 0, 0};
    assert(validate_feature_edge(graph, valid, catalog));
    auto accepted = graph;
    accepted.edges = {valid};
    const auto compiled = FeatureGraphCompiler{}.compile(accepted, registry);
    assert(compiled && compiled.value.plans.size() == 1);
    assert((compiled.value.plans.front().instances == std::vector<FeatureInstanceId>{
        FeatureInstanceId(valid.source_node.value()), FeatureInstanceId(valid.target_node.value())}));
    const auto provenance = [&](FeatureNodeId node) -> const FeatureNodeProvenance& {
        const auto found = std::find_if(compiled.value.provenance.begin(), compiled.value.provenance.end(),
                                        [node](const auto& value) { return value.node_id == node; });
        assert(found != compiled.value.provenance.end());
        return *found;
    };
    assert(provenance(valid.source_node).definition_id == source.id);
    assert(provenance(valid.source_node).owner_path.nodes == source_owner.nodes);
    assert(provenance(valid.target_node).definition_id == target.id);
    assert(provenance(valid.target_node).owner_path.nodes == target_owner.nodes);
    for (const auto& ports : std::array{std::pair{PortId("In"), PortId("Out")},
                                      std::pair{PortId("In"), PortId("In")},
                                      std::pair{PortId("Out"), PortId("Out")}}) {
        auto edge = valid;
        edge.source_port = ports.first;
        edge.target_port = ports.second;
        const auto editor = validate_feature_edge(graph, edge, catalog);
        auto invalid = graph;
        invalid.edges = {edge};
        const auto rejected = FeatureGraphCompiler{}.compile(invalid, registry);
        assert(!editor && editor.code == aeg::foundation::ResultCode::kInvalidArgument && !editor.message.empty());
        assert(!rejected && rejected.result.code == aeg::foundation::ResultCode::kInvalidArgument &&
               !rejected.result.message.empty());
        const auto& author = graph.nodes[edge.source_port == PortId("In") ? 1 : 0];
        const auto port = edge.source_port == PortId("In") ? edge.source_port : edge.target_port;
        assert(rejected.value.plans.empty() && rejected.value.provenance.empty());
        assert(rejected.value.diagnostics.size() == 1);
        const auto& diagnostic = rejected.value.diagnostics.front();
        assert(diagnostic.message == rejected.result.message && diagnostic.node_id == author.node_id &&
               diagnostic.port_id == port && diagnostic.owner_path.nodes == author.owner_path.nodes);
    }
    // Validation is a query: a rejected candidate does not affect this legal graph/catalog.
    assert(graph.edges.empty() && catalog.entries().size() == 2);
    assert(validate_feature_edge(graph, valid, catalog));
    assert(FeatureGraphCompiler{}.compile(accepted, registry));
}

void test_feature_edges_reject_missing_endpoints_and_stale_port_contracts() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const auto source = definition("Logic.EndpointSource", FeatureDomain::kLogic, ClockId("LogicTick"));
    const auto target = definition("Logic.EndpointTarget", FeatureDomain::kLogic, ClockId("LogicTick"));
    assert(aeg::feature::test::register_clock_test_definition(registry, source));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));
    FeatureCatalog catalog;
    assert(catalog.export_definition(source));
    assert(catalog.export_definition(target));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://edge-contracts";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, OwnerPath{{FeatureNodeId("SourceOwner")}}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, OwnerPath{{FeatureNodeId("TargetOwner")}}, {}}};
    const FeatureEdge valid{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                            EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 0, 0};
    for (unsigned failure = 0; failure != 8; ++failure) {
        auto invalid = graph;
        auto edge = valid;
        switch (failure) {
        case 0: edge.source_node = FeatureNodeId("Absent"); break;
        case 1: edge.target_node = FeatureNodeId("Absent"); break;
        case 2: edge.source_port = PortId("Absent"); break;
        case 3: edge.target_port = PortId("Absent"); break;
        case 4: edge.version = 0; break;
        case 5: edge.version = 2; break;
        case 6: invalid.nodes.front().definition_id = FeatureDefinitionId("Absent"); break;
        case 7: ++invalid.nodes.back().definition_version.major; break;
        }
        assert(!validate_feature_edge(invalid, edge, catalog));
        invalid.edges = {edge};
        const auto rejected = FeatureGraphCompiler{}.compile(invalid, registry);
        assert(!rejected);
        if (failure == 2 || failure == 3 || failure == 6) {
            const auto& author = invalid.nodes[failure == 3 ? 1 : 0];
            const auto port = failure == 6 ? PortId{} : failure == 3 ? edge.target_port : edge.source_port;
            assert(rejected.value.plans.empty() && rejected.value.provenance.empty());
            assert(rejected.value.diagnostics.size() == 1);
            const auto& diagnostic = rejected.value.diagnostics.front();
            assert(diagnostic.message == rejected.result.message && diagnostic.node_id == author.node_id &&
                   diagnostic.port_id == port && diagnostic.owner_path.nodes == author.owner_path.nodes);
            const auto retained_owner = diagnostic.owner_path.nodes;
            invalid.nodes.clear();
            invalid.edges.clear();
            assert(diagnostic.owner_path.nodes == retained_owner && diagnostic.node_id == graph.nodes[failure == 3 ? 1 : 0].node_id);
        }
    }
    for (unsigned mismatch = 0; mismatch != 2; ++mismatch) {
        auto incompatible = target;
        auto& input = incompatible.contract.ports.back();
        if (mismatch == 0) input.schema = ValueSchemaId("Other.v1");
        else input.version = 2;
        assert(aeg::feature::test::replace_clock_test_definition(registry, incompatible));
        FeatureCatalog incompatible_catalog;
        assert(incompatible_catalog.export_definition(source));
        assert(incompatible_catalog.export_definition(incompatible));
        assert(!validate_feature_edge(graph, valid, incompatible_catalog));
        auto invalid = graph;
        invalid.edges = {valid};
        assert(!FeatureGraphCompiler{}.compile(invalid, registry));
    }
    assert(aeg::feature::test::replace_clock_test_definition(registry, target));
    graph.edges = {valid};
    assert(FeatureGraphCompiler{}.compile(graph, registry));
}

void test_catalog_is_feature_only() {
    const auto source = definition("Render.Post", FeatureDomain::kRender, ClockId("Frame"));
    FeatureCatalog catalog;
    assert(catalog.export_definition(source));
    assert(catalog.entries().size() == 1);
    assert(catalog.entries().front().definition_id == source.id);
    assert(catalog.entries().front().execution == ExecutionKind::kCall);
    assert(catalog.entries().front().state == StatePolicy::kStateless);
    assert(catalog.entries().front().ports.size() == source.contract.ports.size());
}

void test_cross_domain_data_requires_explicit_transport_even_on_one_clock() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, definition("Logic.Source", FeatureDomain::kLogic, ClockId("SharedTick"))));
    assert(aeg::feature::test::register_clock_test_definition(registry, definition("Physics.Target", FeatureDomain::kPhysics, ClockId("SharedTick"))));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://domain-boundary";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("A"), FeatureDefinitionId("Logic.Source"), {1, 0, 0}, {}, {}},
                   {FeatureNodeId("B"), FeatureDefinitionId("Physics.Target"), {1, 0, 0}, {}, {}}};
    graph.edges = {{FeatureNodeId("A"), PortId("Out"), FeatureNodeId("B"), PortId("In"),
                    EdgeKind::kData, ClockId("SharedTick"), ClockId("SharedTick"), 1, 0, 0}};
    assert(!FeatureGraphCompiler{}.compile(graph, registry));
    graph.edges.front().kind = EdgeKind::kQueue;
    graph.edges.front().capacity = 4;
    assert(FeatureGraphCompiler{}.compile(graph, registry));
}

void test_composite_nodes_are_expanded_before_compilation() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto child = definition("Logic.Child", FeatureDomain::kLogic, ClockId("LogicTick"));
    assert(aeg::feature::test::register_clock_test_definition(registry, child));
    auto composite = definition("Logic.Composite", FeatureDomain::kLogic, ClockId("LogicTick"));
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.children = {child.id};
    assert(aeg::feature::test::register_clock_test_definition(registry, composite));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://composite";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Root"), composite.id, composite.version, {}, {}}};
    assert(FeatureGraphCompiler{}.compile(graph, registry));
    composite.contract.children = {FeatureDefinitionId("Logic.Missing")};
    assert(aeg::feature::test::replace_clock_test_definition(registry, composite));
    assert(!FeatureGraphCompiler{}.compile(graph, registry));
}

void test_composite_port_domains_require_explicit_transport() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto logic_child = definition("Logic.CompositeSource", FeatureDomain::kLogic, ClockId("SharedTick"));
    auto physics_child = definition("Physics.CompositeTarget", FeatureDomain::kPhysics, ClockId("SharedTick"));
    assert(aeg::feature::test::register_clock_test_definition(registry, logic_child));
    assert(aeg::feature::test::register_clock_test_definition(registry, physics_child));

    auto source = definition("Composite.Source", FeatureDomain::kLogic, ClockId("SharedTick"));
    source.contract.structure = FeatureStructure::kComposite; source.contract.clock.work_budget_ns = 0;
    source.contract.domains = to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kPhysics);
    source.contract.children = {logic_child.id, physics_child.id};
    source.contract.child_port_mappings = {
        {PortId("Out"), logic_child.id, PortId("Out"), PortDirection::kOutput},
        {PortId("In"), logic_child.id, PortId("In"), PortDirection::kInput},
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, source));

    auto target = definition("Composite.Target", FeatureDomain::kLogic, ClockId("SharedTick"));
    target.contract.structure = FeatureStructure::kComposite; target.contract.clock.work_budget_ns = 0;
    target.contract.domains = source.contract.domains;
    target.contract.children = {logic_child.id, physics_child.id};
    target.contract.child_port_mappings = {
        {PortId("Out"), logic_child.id, PortId("Out"), PortDirection::kOutput},
        {PortId("In"), physics_child.id, PortId("In"), PortDirection::kInput},
    };
    assert(aeg::feature::test::register_clock_test_definition(registry, target));

    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://composite-domain-boundary";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, ClockId("SharedTick"), ClockId("SharedTick"), 1, 0, 0}};
    assert(!FeatureGraphCompiler{}.compile(graph, registry));
    graph.edges.front().kind = EdgeKind::kQueue;
    graph.edges.front().capacity = 2;
    assert(FeatureGraphCompiler{}.compile(graph, registry));
}

void test_composite_port_schema_is_validated_before_publication() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto child = definition("Logic.MismatchedChild", FeatureDomain::kLogic, ClockId("LogicTick"));
    child.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Other.v1"), 2},
                            {PortId("In"), PortDirection::kInput, ValueSchemaId("Int.v1"), 1}};
    auto composite = definition("Logic.MismatchedComposite", FeatureDomain::kLogic, ClockId("LogicTick"));
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.children = {child.id};
    composite.contract.child_port_mappings = {
        {PortId("Out"), child.id, PortId("Out"), PortDirection::kOutput}};
    auto target = definition("Logic.MismatchedTarget", FeatureDomain::kLogic, ClockId("LogicTick"));
    assert(aeg::feature::test::register_clock_test_definition(registry, child));
    assert(aeg::feature::test::register_clock_test_definition(registry, composite));
    assert(aeg::feature::test::register_clock_test_definition(registry, target));

    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://composite-schema-mismatch";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Composite"), composite.id, composite.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    graph.edges = {{FeatureNodeId("Composite"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 0, 0}};
    assert(!FeatureGraphCompiler{}.compile(graph, registry));
}

void test_compiler_rejects_stale_contract_digest() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, definition("Logic.Digest", FeatureDomain::kLogic,
                                                    ClockId("LogicTick"))));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://stale-digest";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest() ^ 1u;
    graph.nodes = {{FeatureNodeId("Digest"), FeatureDefinitionId("Logic.Digest"), {1, 0, 0}, {}, {}}};
    const auto rejected = FeatureGraphCompiler{}.compile(graph, registry);
    assert(!rejected && rejected.value.plans.empty() && rejected.value.provenance.empty());
    assert(rejected.value.diagnostics.size() == 1);
    const auto& diagnostic = rejected.value.diagnostics.front();
    assert(diagnostic.message == rejected.result.message && !diagnostic.node_id.is_valid() &&
           !diagnostic.port_id.is_valid() && diagnostic.owner_path.nodes.empty());
}

void test_parameter_schema_is_validated_before_graph_compilation() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto parameterized = definition("Logic.Parameterized", FeatureDomain::kLogic, ClockId("LogicTick"));
    parameterized.contract.parameters = {{ParameterId("Damage"), 3, 2, true}};
    assert(aeg::feature::test::register_clock_test_definition(registry, parameterized));

    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://parameter-schema";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Parameterized"), parameterized.id, parameterized.version,
                    OwnerPath{{FeatureNodeId("ParameterOwner")}}, {1, 0, 0, 0}}}; // count=1, truncated record
    assert(!FeatureGraphCompiler{}.compile(graph, registry));

    const auto id = ParameterId("Damage").value();
    graph.nodes.front().parameter_blob = {1, 0, 0, 0}; // record count
    for (unsigned byte = 0; byte < sizeof(id); ++byte)
        graph.nodes.front().parameter_blob.push_back(static_cast<uint8_t>(id >> (byte * 8u)));
    graph.nodes.front().parameter_blob.insert(graph.nodes.front().parameter_blob.end(), {3, 0, 0, 0,
                                                                                           2, 0, 0, 0,
                                                                                           9, 8});
    assert(FeatureGraphCompiler{}.compile(graph, registry));

    const auto valid_blob = graph.nodes.front().parameter_blob;
    graph.nodes.front().parameter_blob[12] = 2; // schema version no longer matches
    assert(!FeatureGraphCompiler{}.compile(graph, registry));
    graph.nodes.front().parameter_blob = valid_blob;
    const auto unknown = ParameterId("Unknown").value();
    for (unsigned byte = 0; byte < sizeof(unknown); ++byte)
        graph.nodes.front().parameter_blob[4 + byte] = static_cast<uint8_t>(unknown >> (byte * 8u));
    const auto rejected = FeatureGraphCompiler{}.compile(graph, registry);
    assert(!rejected && rejected.value.plans.empty() && rejected.value.provenance.empty());
    assert(rejected.value.diagnostics.size() == 1);
    const auto& diagnostic = rejected.value.diagnostics.front();
    assert(diagnostic.node_id == graph.nodes.front().node_id && !diagnostic.port_id.is_valid() &&
           diagnostic.owner_path.nodes == graph.nodes.front().owner_path.nodes && diagnostic.message == rejected.result.message);
    graph.nodes.front().parameter_blob = valid_blob;
    assert(FeatureGraphCompiler{}.compile(graph, registry));
}

bool test_graph_semantic_digest_is_canonical_and_complete() {
    const auto require = [](bool condition, const char* label) {
        if (!condition) std::cerr << "GRAPH_SEMANTIC_DIGEST failed: " << label << '\n';
        return condition;
    };
    auto first = definition("Digest.First", FeatureDomain::kLogic, ClockId("LogicTick"));
    auto second = definition("Digest.Second", FeatureDomain::kLogic, ClockId("LogicTick"));
    auto target = definition("Digest.Target", FeatureDomain::kLogic, ClockId("LogicTick"));
    first.contract.parameters = {{ParameterId("Amount"), 1, 1, false}};
    for (auto* item : {&second, &target}) {
        item->contract.execution = ExecutionKind::kProcess;
        item->contract.state = StatePolicy::kProcessState;
        item->contract.has_continuation = true;
    }
    const auto parameters = encode_feature_parameters(
        std::vector<FeatureParameterRecord>{{ParameterId("Amount"), 1, {7}}}, first.contract.parameters);
    if (!require(static_cast<bool>(parameters), "fixture parameter envelope")) return false;

    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://semantic-digest";
    graph.schema_version = 1;
    graph.graph_version = 17;
    graph.draft_revision = 23;
    graph.layout_revision = 31;
    graph.module_requirements = {"game@1", "renderer@1"};
    graph.nodes = {
        {FeatureNodeId(0xf000000000000003ull), first.id, first.version,
         OwnerPath{{FeatureNodeId("Root"), FeatureNodeId("Group")}}, parameters.value},
        {FeatureNodeId(0x1000000000000002ull), second.id, second.version, {}, {}},
        {FeatureNodeId(0x8000000000000001ull), target.id, target.version, {}, {}}};
    graph.edges = {
        {graph.nodes[0].node_id, PortId("Out"), graph.nodes[2].node_id, PortId("In"),
         EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 2, 3},
        {graph.nodes[1].node_id, PortId("Out"), graph.nodes[2].node_id, PortId("In"),
         EdgeKind::kQueue, ClockId("LogicTick"), ClockId("LogicTick"), 1, 4, 5}};
    graph.process_subgraphs = {
        {graph.nodes[1].node_id, {"Ready", "Done"}, {"Ready->Done"}, 1, "Ready", {"Done"}},
        {graph.nodes[2].node_id, {"Idle", "Complete"}, {"Idle->Complete"}, 2, "Idle", {"Complete"}}};
    graph.input_bindings = {"Input.Fire", "Input.Cancel"};
    graph.variadic_slots = {"Layer.Third", "Layer.First", "Layer.Second"};
    graph.extensions = {"vendor:runtime-one", "vendor:runtime-two", "aeg.editor.canvas.v1:keep",
        "aeg.editor.process-layout.v1:{old}", "aeg.editor.feature-layout.v1:{old}"};

    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    for (auto* item : {&first, &second, &target})
        if (!require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, *item)),
                     "fixture definition registration")) return false;
    graph.contract_digest = registry.contract_digest();
    if (!require(static_cast<bool>(FeatureGraphCompiler{}.compile(graph, registry)), "fixture compiles")) return false;

    const uint64_t baseline = graph_semantic_digest(graph);
    bool passed = require(baseline != 0, "nonzero digest");
    passed = require(graph_semantic_digest(graph) == baseline, "repeat stability") && passed;
    auto reordered = graph;
    std::reverse(reordered.nodes.begin(), reordered.nodes.end());
    std::reverse(reordered.edges.begin(), reordered.edges.end());
    std::reverse(reordered.process_subgraphs.begin(), reordered.process_subgraphs.end());
    std::reverse(reordered.module_requirements.begin(), reordered.module_requirements.end());
    std::reverse(reordered.input_bindings.begin(), reordered.input_bindings.end());
    std::reverse(reordered.extensions.begin(), reordered.extensions.end());
    passed = require(graph_semantic_digest(reordered) == baseline, "semantic collection reorder") && passed;
    auto layout_only = graph;
    ++layout_only.draft_revision;
    ++layout_only.layout_revision;
    layout_only.extensions[3] = "aeg.editor.process-layout.v2:{moved}";
    layout_only.extensions[4] = "aeg.editor.feature-layout.v1:{moved}";
    passed = require(graph_semantic_digest(layout_only) == baseline, "source and layout revisions excluded") && passed;
    layout_only.extensions.erase(layout_only.extensions.begin() + 3, layout_only.extensions.end());
    passed = require(graph_semantic_digest(layout_only) == baseline, "layout record presence excluded") && passed;

    const auto changed = [&](const char* label, const auto& mutation) {
        auto candidate = graph;
        mutation(candidate);
        return require(graph_semantic_digest(candidate) != baseline, label);
    };
    passed = changed("asset identity", [](auto& value) { value.asset_uuid += "-other"; }) && passed;
    passed = changed("asset schema", [](auto& value) { ++value.schema_version; }) && passed;
    passed = changed("published graph version", [](auto& value) { ++value.graph_version; }) && passed;
    passed = changed("catalog contract digest", [](auto& value) { value.contract_digest ^= 1; }) && passed;
    passed = changed("module requirement", [](auto& value) { value.module_requirements[0] += "-other"; }) && passed;
    passed = changed("node identity full uint64", [](auto& value) {
        value.nodes[0].node_id = FeatureNodeId(value.nodes[0].node_id.value() ^ (uint64_t{1} << 63));
    }) && passed;
    passed = changed("definition identity", [](auto& value) { value.nodes[0].definition_id = FeatureDefinitionId("Other"); }) && passed;
    passed = changed("definition major", [](auto& value) { ++value.nodes[0].definition_version.major; }) && passed;
    passed = changed("definition minor", [](auto& value) { ++value.nodes[0].definition_version.minor; }) && passed;
    passed = changed("definition patch", [](auto& value) { ++value.nodes[0].definition_version.patch; }) && passed;
    passed = changed("owner path value", [](auto& value) { value.nodes[0].owner_path.nodes[0] = FeatureNodeId("OtherOwner"); }) && passed;
    passed = changed("owner path order", [](auto& value) { std::reverse(value.nodes[0].owner_path.nodes.begin(), value.nodes[0].owner_path.nodes.end()); }) && passed;
    passed = changed("parameter envelope", [](auto& value) { value.nodes[0].parameter_blob.back() ^= 1; }) && passed;
    passed = changed("edge source node", [](auto& value) { value.edges[0].source_node = value.nodes[1].node_id; }) && passed;
    passed = changed("edge source port", [](auto& value) { value.edges[0].source_port = PortId("OtherOut"); }) && passed;
    passed = changed("edge target node", [](auto& value) { value.edges[0].target_node = value.nodes[1].node_id; }) && passed;
    passed = changed("edge target port", [](auto& value) { value.edges[0].target_port = PortId("OtherIn"); }) && passed;
    passed = changed("edge kind", [](auto& value) { value.edges[0].kind = EdgeKind::kEvent; }) && passed;
    passed = changed("edge source clock", [](auto& value) { value.edges[0].source_clock = ClockId("OtherSource"); }) && passed;
    passed = changed("edge target clock", [](auto& value) { value.edges[0].target_clock = ClockId("OtherTarget"); }) && passed;
    passed = changed("edge version", [](auto& value) { ++value.edges[0].version; }) && passed;
    passed = changed("edge capacity", [](auto& value) { ++value.edges[0].capacity; }) && passed;
    passed = changed("edge deadline", [](auto& value) { ++value.edges[0].deadline_ticks; }) && passed;
    passed = changed("process owner", [](auto& value) { value.process_subgraphs[0].owner_node = value.nodes[0].node_id; }) && passed;
    passed = changed("process states", [](auto& value) { value.process_subgraphs[0].states[0] += "-other"; }) && passed;
    passed = changed("process transitions", [](auto& value) { value.process_subgraphs[0].transitions.push_back("Ready->Ready"); }) && passed;
    passed = changed("process schema", [](auto& value) { ++value.process_subgraphs[0].state_schema_version; }) && passed;
    passed = changed("process entry", [](auto& value) { value.process_subgraphs[0].initial_state = "Done"; }) && passed;
    passed = changed("process terminals", [](auto& value) { value.process_subgraphs[0].terminal_states = {"Ready"}; }) && passed;
    passed = changed("input binding", [](auto& value) { value.input_bindings[0] += "-other"; }) && passed;
    passed = changed("slot value", [](auto& value) { value.variadic_slots[0] += "-other"; }) && passed;
    passed = changed("slot order", [](auto& value) { std::reverse(value.variadic_slots.begin(), value.variadic_slots.end()); }) && passed;
    passed = changed("business extension", [](auto& value) { value.extensions[0] += "-other"; }) && passed;
    passed = changed("canvas remains business data", [](auto& value) { value.extensions[2] += "-other"; }) && passed;
    passed = changed("vendor layout lookalike remains business data", [](auto& value) { value.extensions.push_back("vendor:aeg.editor.process-layout.v1:{data}"); }) && passed;
    passed = changed("non-reserved editor extension remains business data", [](auto& value) { value.extensions.push_back("aeg.editor.runtime-data.v1:{data}"); }) && passed;
    passed = changed("duplicate module preserved", [](auto& value) { value.module_requirements.push_back(value.module_requirements[0]); }) && passed;
    passed = changed("duplicate binding preserved", [](auto& value) { value.input_bindings.push_back(value.input_bindings[0]); }) && passed;
    passed = changed("duplicate business extension preserved", [](auto& value) { value.extensions.push_back(value.extensions[0]); }) && passed;
    passed = changed("duplicate edge preserved", [](auto& value) { value.edges.push_back(value.edges[0]); }) && passed;
    passed = changed("duplicate node preserved", [](auto& value) { value.nodes.push_back(value.nodes[0]); }) && passed;
    auto duplicate_identity = graph;
    duplicate_identity.nodes.push_back(duplicate_identity.nodes[0]);
    passed = require(!FeatureGraphCompiler{}.compile(duplicate_identity, registry), "duplicate node still rejected by compiler") && passed;
    auto strings_left = graph;
    auto strings_right = graph;
    strings_left.input_bindings = {"ab", "c"};
    strings_right.input_bindings = {"a", "bc"};
    passed = require(graph_semantic_digest(strings_left) != graph_semantic_digest(strings_right),
                     "string collection boundaries") && passed;
    std::cout << "Graph semantic digest canonical and complete=" << passed << '\n';
    return passed;
}
}  // namespace

int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    {
        FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        FeatureGraphDocument future;
        future.asset_uuid = "asset://future-schema";
        future.schema_version = 999;
        future.graph_version = 1;
        if (FeatureGraphCompiler{}.compile(future, registry)) {
            std::cerr << "FeatureGraph compiler accepted an unsupported future schema\n";
            return 3;
        }
    }
    test_feature_graph_compiles_with_provenance_and_clock_plans();
    test_process_state_machine_entry_must_match_marked_state();
    test_same_clock_unbuffered_cycle_is_rejected();
    test_edge_clock_provenance_must_match_definition_contracts();
    test_compiler_and_editor_require_output_to_input_edges();
    test_feature_edges_reject_missing_endpoints_and_stale_port_contracts();
    test_catalog_is_feature_only();
    test_cross_domain_data_requires_explicit_transport_even_on_one_clock();
    test_composite_nodes_are_expanded_before_compilation();
    test_composite_port_domains_require_explicit_transport();
    test_composite_port_schema_is_validated_before_publication();
    test_compiler_rejects_stale_contract_digest();
    test_parameter_schema_is_validated_before_graph_compilation();
    if (!test_graph_semantic_digest_is_canonical_and_complete()) return 2;
    std::cout << "feature graph authoring smoke passed\n";
    return 0;
}
