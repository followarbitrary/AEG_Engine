#include <algorithm>
#include <iostream>

#include "editor_composition_test_support.hpp"
#include "editor_composition_view.hpp"

int main() {
    aeg::tests::EditorCompositionFixture fixture;
    using namespace aeg::application;
    const auto top = project_composition_graph(fixture.document, fixture.catalog, {});
    if (!top || top.value.nodes.size() != 1U || top.value.nodes.front().kind != EditorGraphNodeKind::kPipeline)
        return 1;
    const auto feature =
        project_composition_graph(fixture.document, fixture.catalog, {EditorGraphKind::kFeature, "pipeline", {}, {}});
    if (!feature || std::none_of(feature.value.nodes.begin(), feature.value.nodes.end(), [](const auto& node) {
            return node.kind == EditorGraphNodeKind::kFeature && node.node_id == "feature";
        }))
        return 2;
    if (!feature || std::none_of(feature.value.nodes.begin(), feature.value.nodes.end(), [](const auto& node) {
            return node.kind == EditorGraphNodeKind::kPart && node.fixed;
        }))
        return 3;
    const auto pass = project_composition_graph(fixture.document, fixture.catalog,
                                                {EditorGraphKind::kPass, "pipeline", "feature", "render"});
    if (!pass ||
        std::none_of(
            pass.value.nodes.begin(), pass.value.nodes.end(),
            [](const auto& node) { return node.kind == EditorGraphNodeKind::kPass && node.node_id == "pass"; }) ||
        pass.value.edges.size() != 1U ||
        pass.value.edges.front().kind != aeg::pipeline::DocumentEdgeKind::kRenderResource)
        return 4;
    auto missing = fixture.document;
    missing.feature_nodes.front().type = aeg::pipeline::FeatureId("Missing.EditorFeature");
    const auto missing_view =
        project_composition_graph(missing, fixture.catalog, {EditorGraphKind::kFeature, "pipeline", {}, {}});
    if (!missing_view || std::none_of(missing_view.value.nodes.begin(), missing_view.value.nodes.end(),
                                      [](const auto& node) { return node.unknown; }))
        return 5;
    std::cout << "Editor composition view smoke passed\n";
    return 0;
}
