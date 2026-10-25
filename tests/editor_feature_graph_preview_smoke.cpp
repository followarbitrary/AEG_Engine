#include <cassert>
#include "editor_feature_graph_preview.hpp"

using namespace aeg::feature;
using namespace aeg::application;

static FeatureDefinition make_definition(std::string_view name, FeatureDomainMask domains,
                                         FeatureStructure structure = FeatureStructure::kAtomic) {
    FeatureDefinition d{};
    d.id = FeatureDefinitionId(name);
    d.version = {1, 0, 0};
    d.contract.definition_id = d.id;
    d.contract.version = d.version;
    d.contract.domains = domains;
    d.contract.structure = structure;
    d.contract.scope = ScopeContract{ScopeId("World")};
    d.contract.clock = make_periodic_clock_contract(ClockId("Tick"), 60, 4, LateDataPolicy::kReject,
                                                     DeadlinePolicy::kObserve, 1000, 0);
    d.contract.permissions.version = 1;
    d.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Value"), 1},
                        {PortId("In"), PortDirection::kInput, ValueSchemaId("Value"), 1}};
    return d;
}

static void test_duplicate_process_owner_is_rejected() {
    auto process = make_definition("Process", to_mask(FeatureDomain::kLogic));
    process.contract.execution = ExecutionKind::kProcess;
    process.contract.state = StatePolicy::kProcessState;
    process.contract.has_continuation = true;
    FeatureCatalog catalog;
    assert(catalog.export_definition(process));
    FeatureGraphDocument graph;
    graph.asset_uuid = "process-preview";
    graph.schema_version = 1;
    graph.graph_version = graph.draft_revision = 1;
    const FeatureNodeId owner{uint64_t{41}};
    graph.nodes = {{owner, process.id, process.version, {}, {}}};
    graph.process_subgraphs = {{owner, {"Ready", "Done"}, {"Ready->Done"}, 1, "Ready", {"Done"}}};
    const auto valid = EditorFeatureGraphPreviewCompiler{}.compile(graph, catalog);
    assert(valid && !valid.value.has_errors());

    graph.process_subgraphs.push_back(graph.process_subgraphs.front());
    const auto duplicate = EditorFeatureGraphPreviewCompiler{}.compile(graph, catalog);
    assert(duplicate && duplicate.value.has_errors());
    assert(duplicate.value.diagnostics.size() == 1);
    const auto& diagnostic = duplicate.value.diagnostics.front();
    assert(diagnostic.message == "each Feature node may declare only one Process subgraph");
    assert(diagnostic.node_id == owner);
    assert(graph.process_subgraphs.size() == 2);
}

int main() {
    test_duplicate_process_owner_is_rejected();
    FeatureCatalog catalog{};
    const auto logic = make_definition("Logic", to_mask(FeatureDomain::kLogic));
    const auto child = make_definition("Composite.Child", to_mask(FeatureDomain::kRender));
    const auto render = make_definition("Render", to_mask(FeatureDomain::kRender));
    auto composite = make_definition("Composite", to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kRender), FeatureStructure::kComposite);
    composite.contract.children = {child.id};
    composite.contract.child_port_mappings = {{PortId("Out"), child.id, PortId("Out"), PortDirection::kOutput},
                                              {PortId("In"), child.id, PortId("In"), PortDirection::kInput}};
    composite.contract.clock.work_budget_ns = 0;
    auto nested_missing = make_definition("Composite.NestedMissing", to_mask(FeatureDomain::kLogic), FeatureStructure::kComposite);
    auto nested_missing_child = make_definition("Composite.NestedMissing.Child", to_mask(FeatureDomain::kLogic), FeatureStructure::kComposite);
    nested_missing.contract.clock.work_budget_ns = 0;
    nested_missing_child.contract.clock.work_budget_ns = 0;
    // Reference an unexported nested child so the compiler reports the
    // transitive missing-definition path without requiring the catalog to
    // accept an invalid child contract at registration time.
    nested_missing.contract.children = {nested_missing_child.id};
    auto cycle_a = make_definition("Composite.CycleA", to_mask(FeatureDomain::kLogic), FeatureStructure::kComposite);
    auto cycle_b = make_definition("Composite.CycleB", to_mask(FeatureDomain::kLogic), FeatureStructure::kComposite);
    auto cycle_c = make_definition("Composite.CycleC", to_mask(FeatureDomain::kLogic), FeatureStructure::kComposite);
    cycle_a.contract.clock.work_budget_ns = 0;
    cycle_b.contract.clock.work_budget_ns = 0;
    cycle_c.contract.clock.work_budget_ns = 0;
    cycle_a.contract.children = {cycle_b.id};
    cycle_b.contract.children = {cycle_c.id};
    cycle_c.contract.children = {cycle_a.id};
    assert(catalog.export_definition(logic));
    assert(catalog.export_definition(child));
    assert(catalog.export_definition(render));
    assert(catalog.export_definition(composite));
    assert(catalog.export_definition(nested_missing));
    assert(catalog.export_definition(cycle_a));
    assert(catalog.export_definition(cycle_b));
    assert(catalog.export_definition(cycle_c));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "preview";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.draft_revision = 7;
    graph.contract_digest = catalog.contract_digest();
    graph.nodes = {{FeatureNodeId(1), logic.id, logic.version, {}, {}},
                   {FeatureNodeId(2), composite.id, composite.version, {}, {}},
                   {FeatureNodeId(3), render.id, render.version, {}, {}},
                   {FeatureNodeId(4), nested_missing.id, nested_missing.version, {}, {}},
                   {FeatureNodeId(5), cycle_a.id, cycle_a.version, {}, {}}};
    graph.edges.push_back({FeatureNodeId(1), PortId("Out"), FeatureNodeId(2), PortId("In"), EdgeKind::kData,
                           logic.contract.clock.id, composite.contract.clock.id, 1, 0, 0});
    graph.edges.push_back({FeatureNodeId(3), PortId("Out"), FeatureNodeId(2), PortId("In"), EdgeKind::kData,
                           render.contract.clock.id, composite.contract.clock.id, 1, 0, 0});
    const auto before = graph;
    const auto preview = EditorFeatureGraphPreviewCompiler{}.compile(graph, catalog);
    assert(preview);
    assert(preview.value.graph_revision == 7);
    assert(preview.value.lanes.size() == 5);
    assert(preview.value.lanes[0].node_ids.size() == 4);
    assert(preview.value.lanes[2].node_ids.size() == 2);
    assert(preview.value.edges.size() == 2);
    assert(preview.value.edges[0].cross_domain);
    assert(preview.value.edges[0].source_domains == to_mask(FeatureDomain::kLogic));
    assert(preview.value.edges[0].target_domains == to_mask(FeatureDomain::kRender));
    assert(!preview.value.edges[1].cross_domain);
    assert(preview.value.edges[1].source_domains == to_mask(FeatureDomain::kRender));
    assert(preview.value.edges[1].target_domains == to_mask(FeatureDomain::kRender));
    assert(preview.value.edges[1].validation);
    assert(preview.value.has_errors());
    bool saw_nested_missing = false;
    bool saw_cycle = false;
    for (const auto& diagnostic : preview.value.diagnostics) {
        saw_nested_missing |= diagnostic.node_id == FeatureNodeId(4) &&
                              diagnostic.message.find("missing from the catalog") != std::string::npos &&
                              diagnostic.message.find(std::to_string(nested_missing_child.id.value())) != std::string::npos;
        saw_cycle |= diagnostic.node_id == FeatureNodeId(5) &&
                     diagnostic.message.find("recursively") != std::string::npos;
    }
    assert(saw_nested_missing && saw_cycle);
    auto stale_graph = graph;
    stale_graph.contract_digest ^= 1;
    const auto stale_preview = EditorFeatureGraphPreviewCompiler{}.compile(stale_graph, catalog);
    assert(stale_preview);
    bool saw_located_stale = false;
    for (const auto& diagnostic : stale_preview.value.diagnostics)
        saw_located_stale |= diagnostic.message.find("contract digest is stale") != std::string::npos &&
                             diagnostic.node_id == FeatureNodeId(1);
    assert(saw_located_stale);
    assert(graph.graph_version == before.graph_version && graph.nodes.size() == before.nodes.size());
    return 0;
}
