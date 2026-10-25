#pragma once
#include "feature_catalog.hpp"
#include "feature_graph.hpp"
#include "feature_contract_metadata.hpp"

namespace aeg::tests {
inline feature::FeatureDefinition feature_definition() {
    feature::FeatureDefinition entry{};
    entry.id = feature::FeatureDefinitionId{"Game.CustomFeature"}; entry.version = {1, 0, 0};
    entry.owner_module = feature::ModuleIdentity{"com.aeg.editor.fixture"};
    auto& c = entry.contract;
    c.definition_id = entry.id; c.version = entry.version;
    c.domains = feature::to_mask(feature::FeatureDomain::kLogic);
    c.scope.id = feature::ScopeId{"World"};
    c.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId{"Simulation"}, 60, 8, feature::LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, c.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    c.permissions.version = 1;
    c.ports = {{feature::PortId{11}, feature::PortDirection::kInput, feature::ValueSchemaId{"Fixture.Scalar"}, 1},
               {feature::PortId{12}, feature::PortDirection::kOutput, feature::ValueSchemaId{"Fixture.Scalar"}, 1}};
    feature::ParameterMetadata metadata{};
    metadata.display_name = "Gain";
    metadata.type = feature::ParameterType::kDouble; metadata.default_value = 1.0;
    metadata.constraint = feature::NumericRange{0.0, 2.0};
    c.parameters.push_back({feature::ParameterId{21}, 1, 8, false, metadata});
    return entry;
}
inline feature::FeatureCatalog feature_catalog(const feature::FeatureDefinition& definition) {
    feature::FeatureCatalog result;
    (void)result.export_definition(definition);
    return result;
}
inline feature::FeatureGraphDocument feature_graph(const feature::FeatureDefinition& definition) {
    feature::FeatureGraphDocument graph{};
    graph.asset_uuid = "feature-definition-fixture"; graph.schema_version = 1; graph.graph_version = 1;
    graph.nodes = {{feature::FeatureNodeId{101}, definition.id, definition.version, {}, {}},
                   {feature::FeatureNodeId{102}, definition.id, definition.version, {}, {}}};
    graph.edges = {{feature::FeatureNodeId{101}, feature::PortId{12}, feature::FeatureNodeId{102}, feature::PortId{11},
                   feature::EdgeKind::kData, definition.contract.clock.id, definition.contract.clock.id, 1}};
    graph.contract_digest = feature_catalog(definition).contract_digest();
    return graph;
}
}  // namespace aeg::tests
