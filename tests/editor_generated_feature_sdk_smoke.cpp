#include "feature_definition_fixture.hpp"
#include "feature_contract_json.hpp"
#include "EditorCompositeNode.flowgraph.hpp"
#include "EditorFeatureNode0.flowgraph.hpp"
#include "EditorFeatureNode1.flowgraph.hpp"
#include "EditorFeatureNode2.flowgraph.hpp"
#include "EditorFeatureNode3.flowgraph.hpp"
#include "EditorFeatureNode4.flowgraph.hpp"
#include <iostream>

namespace {
template<class Generated, class Register>
bool check(aeg::feature::FeatureDefinition expected, Register register_feature) {
    using namespace aeg;
    const auto& actual = Generated::definition();
    const auto wanted = feature::encode_feature_contract_json(expected.contract);
    const auto encoded = feature::encode_feature_contract_json(actual.contract);
    if (!wanted || !encoded || wanted.value != encoded.value || actual.id != expected.id ||
        actual.version != expected.version || actual.owner_module != expected.owner_module || !actual.execute) return false;
    module::FeatureModuleDefinition owner{};
    owner.descriptor = {expected.owner_module, "Generated Feature SDK fixture", {1, 0, 0}, {}};
    if (!register_feature(owner) || owner.feature_definitions.size() != 1 || owner.feature_catalog.entries().size() != 1 ||
        owner.feature_catalog.entries().front().contract.parameters != expected.contract.parameters ||
        owner.feature_catalog.entries().front().contract.state_payload != expected.contract.state_payload) return false;
    if (register_feature(owner) || owner.feature_definitions.size() != 1 || owner.feature_catalog.entries().size() != 1) return false;
    const auto result = owner.feature_definitions.front().execute({}, {}, {}, {});
    return result.result.code == foundation::ResultCode::kUnsupported;
}
}
int main() {
    using namespace aeg;
    auto composite = tests::feature_definition();
    composite.contract.structure = feature::FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.children = {feature::FeatureDefinitionId{"Game.Child"}};
    if (!check<game::graph::EditorCompositeNode>(composite, game::graph::register_EditorCompositeNode)) return 1;
    auto leaf = tests::feature_definition();
    leaf.contract.execution = feature::ExecutionKind::kProcess; leaf.contract.state = feature::StatePolicy::kProcessState;
    leaf.contract.has_continuation = true; leaf.contract.state_payload = {{7, 2, 1}, 65};
    if (!check<game::graph::EditorFeatureNode0>(leaf, game::graph::register_EditorFeatureNode0)) return 2;
    leaf = tests::feature_definition(); leaf.contract.domains = feature::to_mask(feature::FeatureDomain::kPhysics);
    if (!check<game::graph::EditorFeatureNode1>(leaf, game::graph::register_EditorFeatureNode1)) return 3;
    leaf.contract.domains = feature::to_mask(feature::FeatureDomain::kRender);
    if (!check<game::graph::EditorFeatureNode2>(leaf, game::graph::register_EditorFeatureNode2)) return 4;
    leaf.contract.domains = feature::to_mask(feature::FeatureDomain::kAudio);
    if (!check<game::graph::EditorFeatureNode3>(leaf, game::graph::register_EditorFeatureNode3)) return 5;
    leaf.contract.domains = feature::to_mask(feature::FeatureDomain::kStream);
    if (!check<game::graph::EditorFeatureNode4>(leaf, game::graph::register_EditorFeatureNode4)) return 6;
    std::cout << "Generated five-domain Features compile, register and preserve contracts through GameSDK\n";
}
