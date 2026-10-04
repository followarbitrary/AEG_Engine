#include <algorithm>
#include <iostream>

#include "editor_composition_catalog.hpp"
#include "editor_composition_test_support.hpp"

int main() {
    aeg::tests::EditorCompositionFixture fixture;
    using namespace aeg::application;
    const auto top = build_composition_palette(fixture.document, fixture.catalog, {});
    if (!top || std::none_of(top.value.begin(), top.value.end(), [](const auto& entry) {
            return entry.type.kind == aeg::pipeline::AuthoringTypeKind::kPipeline;
        }) ||
        std::any_of(top.value.begin(), top.value.end(), [](const auto& entry) {
            return entry.type.kind == aeg::pipeline::AuthoringTypeKind::kFeature;
        }))
        return 1;
    const auto features =
        build_composition_palette(fixture.document, fixture.catalog, {EditorGraphKind::kFeature, "pipeline", {}, {}});
    if (!features || features.value.size() != 1U ||
        features.value.front().type.kind != aeg::pipeline::AuthoringTypeKind::kFeature)
        return 2;
    const auto passes = build_composition_palette(fixture.document, fixture.catalog,
                                                  {EditorGraphKind::kPass, "pipeline", "feature", "render"});
    if (!passes || passes.value.size() != 1U ||
        passes.value.front().type.kind != aeg::pipeline::AuthoringTypeKind::kRenderPass)
        return 3;
    auto invalid = fixture.document;
    invalid.feature_nodes.front().type = aeg::pipeline::FeatureId("Missing.EditorFeature");
    const auto diagnostics = aeg::pipeline::validate_pipeline_document(invalid, fixture.catalog);
    bool located = false;
    for (const auto& diagnostic : diagnostics) {
        const auto target = resolve_composition_diagnostic(invalid, diagnostic);
        located |= target && target.value.node_id == "feature";
    }
    if (!located)
        return 4;
    {
        auto resource_document = fixture.document;
        resource_document.resources.push_back({.node_id = "feature/SceneTexture",
                                                .resource_id = aeg::pipeline::RenderResourceId("Game.SceneTexture"),
                                                .type = aeg::pipeline::ArtifactTypeId("Pipeline.DefaultTexture")});
        resource_document.edges.push_back({.edge_id = "scene-texture",
                                           .kind = aeg::pipeline::DocumentEdgeKind::kRenderResource,
                                           .source = {"pass", 11},
                                           .destination = {"feature/SceneTexture", 77}});
        const auto target = resolve_composition_diagnostic(
            resource_document,
            {.code = "resource.binding", .message = "resource binding", .node_id = "feature/SceneTexture"});
        if (!target || target.value.node_id != "pass" || target.value.graph.kind != EditorGraphKind::kPass)
            return 5;
    }
    std::cout << "Editor composition palette smoke passed\n";
    return 0;
}
