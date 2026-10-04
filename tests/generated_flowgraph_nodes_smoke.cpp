#include <iostream>

#include "EditorPipelineNode.flowgraph.hpp"
#include "EditorFeatureNode.flowgraph.hpp"
#include "EditorPassNode.flowgraph.hpp"
#include "editor_composition_test_support.hpp"
#include "module_bundle.hpp"

int main() {
    using namespace aeg;
    tests::EditorCompositionFixture expected;
    module::ModuleDefinition definition;
    definition.descriptor = {pipeline::ModuleId("Game.Editor"), "FlowGraph.NodeContract", {1, 0, 0}, {}};
    if (!game::graph::register_EditorPipelineNode(definition) ||
        !game::graph::register_EditorFeatureNode(definition) || !game::graph::register_EditorPassNode(definition))
        return 1;
    if (definition.pipelines.size() != 1 || definition.features.size() != 1 || definition.passes.size() != 1)
        return 2;
    // A compiled factory and its exported contract share every field, including
    // fixed/default topology and nested input/variadic contracts.
    module::ModuleMetadata actual_metadata;
    actual_metadata.module = definition.descriptor;
    actual_metadata.catalog = definition.catalog;
    module::ModuleMetadata expected_metadata;
    expected_metadata.module = definition.descriptor;
    expected_metadata.catalog = expected.catalog;
    const auto actual = module::module_contract_sha256(actual_metadata);
    const auto wanted = module::module_contract_sha256(expected_metadata);
    if (!actual || !wanted || actual.value != wanted.value)
        return 3;
    const auto pass = definition.passes.front()();
    if (pass->descriptor().type != pipeline::RenderPassTypeId("Game.EditorPass"))
        return 4;
    std::cout << "Generated FlowGraph nodes compile and share the canonical runtime contract\n";
    return 0;
}
