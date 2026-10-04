#include <algorithm>
#include <iostream>

#include "editor_composition_test_support.hpp"
#include "editor_flowgraph_geometry.hpp"
#include "editor_composition_view.hpp"

int main() {
    {
        const auto top_left = aeg::application::flowgraph::drag_node_top_left({320.0f, 180.0f});
        if (top_left.x != 320.0f || top_left.y != 180.0f) {
            std::cerr << "drag preview must use the pointer as the node top-left\n";
            return 60;
        }
        const auto graph_position = aeg::application::flowgraph::graph_position_from_mouse(
            {420.0f, 280.0f}, {20.0f, 40.0f}, 2.0f);
        const auto panned_graph_position = aeg::application::flowgraph::graph_position_from_mouse(
            {20.0f, 30.0f}, {40.0f, 50.0f}, 1.0f);
        if (graph_position.x != 200.0f || graph_position.y != 120.0f ||
            panned_graph_position.x != -20.0f || panned_graph_position.y != -20.0f) {
            std::cerr << "graph drop position must preserve the pointer anchor\n";
            return 61;
        }
        const auto columns = aeg::application::flowgraph::distribute_columns(2400.0f, 260.0f, 320.0f);
        if (columns.palette != 260.0f || columns.inspector != 320.0f || columns.canvas != 1820.0f) {
            std::cerr << "maximized graph layout must assign remaining width to the canvas\n";
            return 62;
        }
        const auto layout = aeg::application::flowgraph::node_layout(3, 2);
        if (layout.height() < 110.0f || layout.input_anchor(2).y >= layout.height() ||
            layout.output_anchor(1).x <= layout.input_anchor(0).x ||
            layout.output_anchor(1).y >= layout.height()) {
            std::cerr << "node card must reserve distinct left and right port rows\n";
            return 63;
        }
        const auto empty_layout = aeg::application::flowgraph::node_layout(0, 0);
        if (empty_layout.height() <= empty_layout.header_height + empty_layout.padding * 2.0f ||
            aeg::application::flowgraph::compact_label("Renderer.GBuffer.Albedo") != "Albedo" ||
            aeg::application::flowgraph::compact_label("Renderer.SceneLighting") != "Scene Lighting" ||
            aeg::application::flowgraph::compact_label("Custom_Pipeline") != "Custom Pipeline" ||
            aeg::application::flowgraph::compact_label("Renderer_DeferredRenderFeature") !=
                "Deferred Render Feature") {
            std::cerr << "empty cards and compact semantic labels must have stable geometry\n";
            return 67;
        }
        if (aeg::application::flowgraph::port_anchor_key("resource", true, 42, 0) ==
            aeg::application::flowgraph::port_anchor_key("resource", false, 42, 0)) {
            std::cerr << "resource input and output must have distinct anchor identities\n";
            return 65;
        }
        if (aeg::application::flowgraph::first_grid_line_offset(0.0f, 24.0f) != 0.0f ||
            aeg::application::flowgraph::first_grid_line_offset(48.0f, 24.0f) != 0.0f ||
            aeg::application::flowgraph::first_grid_line_offset(10.0f, 24.0f) != 14.0f ||
            aeg::application::flowgraph::first_grid_line_offset(-10.0f, 24.0f) != 10.0f) {
            std::cerr << "grid phase must remain stable for large positive and negative scroll offsets\n";
            return 66;
        }
    }
    aeg::tests::EditorCompositionFixture fixture;
    using namespace aeg::application;
    {
        using namespace aeg::pipeline;
        PipelinePlan plan;
        plan.pipeline = fixture.document.pipeline_nodes.front().type;
        plan.instance = PipelineInstanceId("pipeline");
        plan.revision = 7;
        plan.nodes = {{.task = TaskId("first"), .stage = StageId("first-stage"),
                       .execution_domain = ExecutionDomain::kRenderThread, .definition_stage_index = 3},
                      {.task = TaskId("second"), .stage = StageId("second-stage"), .synthetic = true},
                      {.task = TaskId("third"), .stage = StageId("third-stage"), .definition_stage_index = 0}};
        plan.feature_contributions = {{.feature_instance = FeatureInstanceId("feature"),
                                        .feature = fixture.document.feature_nodes.front().type,
                                        .part = FeaturePartId("render"), .first_stage_index = 3, .stage_count = 1}};
        plan.dependencies = {{0, 1, PipelineDependencyKind::kArtifact, ArtifactId("color")},
                             {1, 2, PipelineDependencyKind::kOrdering, {}}};
        const auto inspected = project_runtime_plan(plan, "Game / Renderer", &fixture.document);
        if (!inspected || inspected.value.nodes.size() != 3 || inspected.value.dependencies.size() != 2 ||
            inspected.value.revision != 7 || inspected.value.nodes[0].author_node != "feature" ||
            inspected.value.nodes[2].author_node != "pipeline" ||
            !inspected.value.nodes[1].author_node.empty() || !inspected.value.nodes[1].synthetic ||
            inspected.value.nodes[0].outgoing_dependencies != 1 ||
            inspected.value.nodes[1].incoming_dependencies != 1 ||
            inspected.value.nodes[1].outgoing_dependencies != 1 ||
            inspected.value.nodes[2].incoming_dependencies != 1 ||
            inspected.value.graph_asset != fixture.document.asset_uuid ||
            inspected.value.semantic_digest != document_semantic_digest(fixture.document)) {
            std::cerr << "runtime inspection must retain active revision, dependency kinds and exact author provenance\n";
            return 50;
        }
        plan.dependencies.push_back({9, 0});
        if (project_runtime_plan(plan, "bad-plan", &fixture.document)) {
            std::cerr << "runtime inspection must reject invalid dependency endpoints\n";
            return 51;
        }
    }
    const auto top = project_composition_graph(fixture.document, fixture.catalog, {});
    if (!top || top.value.nodes.size() != 1U ||
        std::any_of(top.value.nodes.begin(), top.value.nodes.end(),
                    [](const auto& node) { return node.kind != EditorGraphNodeKind::kPipeline; }) ||
        !top.value.edges.empty()) {
        return 1;
    }
    if (!top || std::any_of(top.value.nodes.begin(), top.value.nodes.end(), [](const auto& node) {
            return node.kind == EditorGraphNodeKind::kFeature;
        })) {
        std::cerr << "Pipeline graph must project Pipeline nodes only\n";
        return 72;
    }
    const auto feature =
        project_composition_graph(fixture.document, fixture.catalog, {EditorGraphKind::kFeature, "pipeline", {}, {}});
    if (!feature || feature.value.nodes.size() != 1U ||
        std::none_of(feature.value.nodes.begin(), feature.value.nodes.end(), [](const auto& node) {
            return node.kind == EditorGraphNodeKind::kFeature && node.node_id == "feature";
        }) || !feature.value.edges.empty())
        return 2;
    if (!feature || std::any_of(feature.value.nodes.begin(), feature.value.nodes.end(), [](const auto& node) {
            return node.kind != EditorGraphNodeKind::kFeature;
        })) {
        std::cerr << "Feature graph must project Feature nodes only\n";
        return 70;
    }
    const auto pass = project_composition_graph(fixture.document, fixture.catalog,
                                                {EditorGraphKind::kPass, "pipeline", "feature", "render"});
    if (!pass ||
        pass.value.nodes.size() != 1U ||
        std::none_of(pass.value.nodes.begin(), pass.value.nodes.end(),
                     [](const auto& node) {
                         return node.kind == EditorGraphNodeKind::kPass && node.node_id == "pass";
                     }) ||
        !pass.value.edges.empty() ||
        std::any_of(pass.value.nodes.begin(), pass.value.nodes.end(),
                    [](const auto& node) { return node.kind != EditorGraphNodeKind::kPass; }))
        return 4;
    {
        using namespace aeg::pipeline;
        auto unpartitioned_document = fixture.document;
        unpartitioned_document.pass_nodes.front().part_id.clear();
        TypeCatalog unpartitioned_catalog;
        for (auto entry : fixture.catalog.entries()) {
            if (entry.key.kind == AuthoringTypeKind::kFeature) {
                auto* declaration = std::get_if<FeatureAuthoringDeclaration>(&entry.declaration);
                if (declaration != nullptr) {
                    declaration->parts.clear();
                    for (auto& default_pass : declaration->default_passes)
                        default_pass.part_id.clear();
                }
            }
            if (!unpartitioned_catalog.add(std::move(entry)))
                return 68;
        }
        const auto unpartitioned = project_composition_graph(
            unpartitioned_document, unpartitioned_catalog, {EditorGraphKind::kPass, "pipeline", "feature", {}});
        if (!unpartitioned ||
            std::none_of(unpartitioned.value.nodes.begin(), unpartitioned.value.nodes.end(),
                         [](const auto& node) {
                             return node.kind == EditorGraphNodeKind::kPass && node.node_id == "pass";
                         })) {
            std::cerr << "an authored unpartitioned Pass graph must project its real Pass nodes\n";
            return 69;
        }
    }
    {
        auto legacy = fixture.document;
        legacy.pass_nodes.push_back(legacy.pass_nodes.front());
        legacy.pass_nodes.back().node_id = "pass2";
        legacy.edges.push_back({.edge_id = "legacy-order", .kind = aeg::pipeline::DocumentEdgeKind::kOrdering,
                                .source = {"pass", 0}, .destination = {"pass2", 0}});
        legacy.edges.push_back({.edge_id = "direct-pass", .kind = aeg::pipeline::DocumentEdgeKind::kArtifact,
                                .source = {"pass", 12}, .destination = {"pass2", 13}});
        const auto projected = project_composition_graph(
            legacy, fixture.catalog, {EditorGraphKind::kPass, "pipeline", "feature", "render"});
        if (!projected || projected.value.edges.size() != 1U ||
            projected.value.edges.front().edge_id != "direct-pass") {
            std::cerr << "strict Pass canvas must keep direct Pass edges and hide resource/feature endpoints\n";
            return 64;
        }
    }
    {
        using namespace aeg::pipeline;
        auto relay_document = fixture.document;
        relay_document.pass_nodes.push_back(relay_document.pass_nodes.front());
        relay_document.pass_nodes.back().node_id = "pass2";
        relay_document.resources.push_back({.node_id = "feature/SceneColor",
                                            .resource_id = RenderResourceId("Game.SceneColor"),
                                            .type = ArtifactTypeId("Pipeline.DefaultTexture")});
        relay_document.edges.clear();
        relay_document.edges.push_back({.edge_id = "produce-scene-color",
                                        .kind = DocumentEdgeKind::kRenderResource,
                                        .source = {"pass", 11},
                                        .destination = {"feature/SceneColor", 77}});
        relay_document.edges.push_back({.edge_id = "consume-scene-color",
                                        .kind = DocumentEdgeKind::kRenderResource,
                                        .source = {"feature/SceneColor", 77},
                                        .destination = {"pass2", 12}});
        const auto relay = project_composition_graph(
            relay_document, fixture.catalog, {EditorGraphKind::kPass, "pipeline", "feature", "render"});
        if (!relay || relay.value.edges.size() != 1U || relay.value.edges.front().source_node != "pass" ||
            relay.value.edges.front().destination_node != "pass2" ||
            relay.value.edges.front().backing_edge_ids !=
                std::vector<std::string>{"produce-scene-color", "consume-scene-color"}) {
            std::cerr << "resource relays must collapse to editable Pass-to-Pass flow edges\n";
            return 73;
        }
    }
    {
        using namespace aeg::pipeline;
        auto with_resource = fixture.document;
        with_resource.resources.push_back({.node_id = "feature/SceneTexture",
                                           .resource_id = RenderResourceId("Game.SceneTexture"),
                                           .type = ArtifactTypeId("Pipeline.DefaultTexture"),
                                           .version = 1,
                                           .owner_scope = PipelineScope::kFrame,
                                           .visible_to = to_mask(PipelineScope::kFrame),
                                           .external_input = true,
                                           .storage = ArtifactStorage::kGpuLogicalResource,
                                           .lifetime = ArtifactLifetime::kFrame,
                                           .ownership = OwnershipPolicy::kGraphOwned,
                                           .resource_constraints = {.resource_class = ResourceClass::kTexture}});
        const auto resource_view = project_composition_graph(
            with_resource, fixture.catalog, {EditorGraphKind::kPass, "pipeline", "feature", "render"});
        if (!resource_view || std::any_of(resource_view.value.nodes.begin(), resource_view.value.nodes.end(),
                                          [](const auto& node) {
                                              return node.kind == EditorGraphNodeKind::kResource;
                                          }))
            return 41;
    }
    auto missing = fixture.document;
    missing.feature_nodes.front().type = aeg::pipeline::FeatureId("Missing.EditorFeature");
    const auto missing_view =
        project_composition_graph(missing, fixture.catalog, {EditorGraphKind::kFeature, "pipeline", {}, {}});
    if (!missing_view || std::none_of(missing_view.value.nodes.begin(), missing_view.value.nodes.end(),
                                      [](const auto& node) { return node.unknown; }))
        return 5;
    {
        using namespace aeg::pipeline;
        PipelineDocument explicit_document = fixture.document;
        explicit_document.variadic_slots = {{"feature", 100, 301, 0, {}}};
        TypeCatalog explicit_catalog;
        for (auto entry : fixture.catalog.entries()) {
            if (entry.key.kind == AuthoringTypeKind::kFeature) {
                auto layer = entry.ports.front();
                layer.id = 23;
                layer.name = "Layers";
                layer.input = InputPortContract{.allowed_sources = input_source_mask(InputSourceKind::kEdge),
                                                 .presence = InputPresenceRequirement::kOptional,
                                                 .min_edges = 0,
                                                 .max_edges = 1,
                                                 .variadic_group_id = 100,
                                                 };
                entry.ports.push_back(layer);
                entry.variadic_inputs = {{100, "Layers", 23, 1, 3, true, false}};
            }
            if (!explicit_catalog.add(std::move(entry)))
                return 6;
        }
        const auto variadic = project_composition_graph(
            explicit_document, explicit_catalog, {EditorGraphKind::kFeature, "pipeline", {}, {}});
        if (!variadic)
            return 7;
        const auto feature_node = std::find_if(variadic.value.nodes.begin(), variadic.value.nodes.end(),
                                               [](const auto& node) { return node.node_id == "feature"; });
        if (feature_node == variadic.value.nodes.end() ||
            std::none_of(feature_node->ports.begin(), feature_node->ports.end(),
                         [](const auto& port) { return port.port_id == 23 && port.slot_id == 301; }))
            return 7;
    }
    std::cout << "Editor composition view smoke passed\n";
    return 0;
}
