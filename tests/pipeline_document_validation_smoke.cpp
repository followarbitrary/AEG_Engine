#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

#include "pipeline_authoring.hpp"

namespace {
using namespace aeg::pipeline;

bool has_code(const std::vector<DocumentDiagnostic>& diagnostics, std::string_view code, std::string_view node = {},
              std::string_view edge = {}) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const DocumentDiagnostic& diagnostic) {
        return diagnostic.code == code && (node.empty() || diagnostic.node_id == node) &&
               (edge.empty() || diagnostic.edge_id == edge);
    });
}

struct Fixture final {
    TypeCatalog catalog{};
    PipelineDocument document{};

    Fixture() {
        const auto pipeline_id = PipelineId("Game.CustomForward");
        const auto feature_id = FeatureId("Game.CustomOutline");
        const auto pass_id = RenderPassTypeId("Game.CustomOutlinePass");
        PipelineAuthoringDeclaration pipeline_declaration;
        pipeline_declaration.stages = {{.id = StageId("RenderBuild"),
                                        .name = "RenderBuild",
                                        .phase = PhaseId("RenderBuild"),
                                        .execution_domain = ExecutionDomain::kRenderThread,
                                        .time_policy = TimePolicy::kFrame}};
        pipeline_declaration.feature_points = {{.id = FeaturePointId("PostProcess"),
                                                .name = "PostProcess",
                                                .anchor_stage = StageId("RenderBuild"),
                                                .position = FeaturePointPosition::kAfter,
                                                .domains = to_mask(FeatureDomain::kRender),
                                                .scopes = to_mask(PipelineScope::kFrame),
                                                .flags = FeaturePointFlags::kMayCreateGraphicsPass}};

        TypeCatalogEntry pipeline_entry;
        pipeline_entry.key = {AuthoringTypeKind::kPipeline, pipeline_id.value()};
        pipeline_entry.version = {1, 0, 0};
        pipeline_entry.owner_module = ModuleId("Game");
        pipeline_entry.name = "Game.CustomForward";
        pipeline_entry.display_name = "Custom Forward";
        pipeline_entry.scopes = to_mask(PipelineScope::kFrame);
        pipeline_entry.domains = to_mask(FeatureDomain::kRender);
        pipeline_entry.declaration = pipeline_declaration;

        FeatureAuthoringDeclaration feature_declaration;
        feature_declaration.allowed_pass_types = {pass_id};
        feature_declaration.allow_add_passes = true;
        feature_declaration.parts = {{.part_index = 0, .domain = FeatureDomain::kRender, .name = "render"},
                                     {.part_index = 1, .domain = FeatureDomain::kPhysics, .name = "physics"}};
        feature_declaration.default_passes = {
            {.node_id = "OutlinePass", .type = pass_id, .version = {1, 0, 0}, .part_id = "render"}};

        TypeCatalogEntry feature_entry;
        feature_entry.key = {AuthoringTypeKind::kFeature, feature_id.value()};
        feature_entry.version = {1, 0, 0};
        feature_entry.owner_module = ModuleId("Game");
        feature_entry.name = "Game.CustomOutline";
        feature_entry.display_name = "Custom Outline";
        feature_entry.domains = to_mask(FeatureDomain::kRender);
        feature_entry.scopes = to_mask(PipelineScope::kFrame);
        feature_entry.ports = {{.id = 22,
                                .name = "SceneColor",
                                .direction = PortDirection::kInput,
                                .value_kind = PortValueKind::kTexture,
                                .schema_id = 1,
                                .binding_id = 77,
                                .resource_constraints = {.resource_class = ResourceClass::kTexture}}};
        feature_entry.declaration = feature_declaration;

        TypeCatalogEntry pass_entry;
        pass_entry.key = {AuthoringTypeKind::kRenderPass, pass_id.value()};
        pass_entry.version = {1, 0, 0};
        pass_entry.owner_module = ModuleId("Game");
        pass_entry.name = "Game.CustomOutlinePass";
        pass_entry.display_name = "Custom Outline Pass";
        pass_entry.domains = to_mask(FeatureDomain::kRender);
        pass_entry.scopes = to_mask(PipelineScope::kFrame);
        pass_entry.ports = {{.id = 11,
                             .name = "ColorOut",
                             .direction = PortDirection::kOutput,
                             .value_kind = PortValueKind::kTexture,
                             .schema_id = 1,
                             .binding_id = 77,
                             .resource_constraints = {.resource_class = ResourceClass::kTexture}}};
        pass_entry.declaration = PassAuthoringDeclaration{.type = pass_id, .allowed_queue_mask = 1};

        if (!catalog.add(pipeline_entry) || !catalog.add(feature_entry) || !catalog.add(pass_entry)) {
            throw std::runtime_error("fixture catalog failed validation");
        }

        document.schema_version = 1;
        document.asset_uuid = "00000000-0000-0000-0000-000000000001";
        document.name = "Custom Forward";
        document.module_requirements = {ModuleId("Game")};
        document.pipeline_nodes = {
            {.node_id = "pipeline", .type = pipeline_id, .version = {1, 0, 0}, .scope = PipelineScope::kFrame}};
        document.feature_nodes = {{.node_id = "outline",
                                   .type = feature_id,
                                   .version = {1, 0, 0},
                                   .pipeline_node = "pipeline",
                                   .point = FeaturePointId("PostProcess")}};
        document.pass_nodes = {{.node_id = "outline_pass",
                                .type = pass_id,
                                .version = {1, 0, 0},
                                .feature_node = "outline",
                                .part_id = "render"}};
        document.edges = {{.edge_id = "color_edge",
                           .kind = DocumentEdgeKind::kRenderResource,
                           .source = {"outline_pass", 11},
                           .destination = {"outline", 22},
                           .binding_id = 77}};
    }
};

}  // namespace

int main() {
    try {
        Fixture fixture;
        if (!validate_pipeline_document(fixture.document, fixture.catalog).empty()) {
            std::cerr << "valid document was rejected\n";
            return 1;
        }

        auto invalid = fixture.document;
        invalid.feature_nodes.front().pipeline_node = "missing_pipeline";
        auto diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "parent.missing", "outline")) {
            return 2;
        }

        invalid = fixture.document;
        invalid.feature_nodes.front().point = FeaturePointId("MissingPoint");
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "feature_point.missing", "outline")) {
            return 3;
        }

        invalid = fixture.document;
        invalid.pass_nodes.front().part_id = "unknown";
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "part.missing", "outline_pass")) {
            return 4;
        }

        invalid = fixture.document;
        invalid.pass_nodes.front().part_id.clear();
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "part.missing", "outline_pass")) {
            return 10;
        }

        invalid = fixture.document;
        invalid.pass_nodes.front().part_id = "physics";
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "part.domain", "outline_pass")) {
            return 11;
        }

        invalid = fixture.document;
        invalid.edges.front().source = {"outline", 22};
        invalid.edges.front().destination = {"outline_pass", 11};
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "port.direction", {}, "color_edge")) {
            return 5;
        }

        invalid = fixture.document;
        invalid.edges.front().binding_id = 99;
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "port.binding", {}, "color_edge")) {
            return 6;
        }

        invalid = fixture.document;
        invalid.edges.push_back({.edge_id = "cycle_edge",
                                 .kind = DocumentEdgeKind::kOrdering,
                                 .source = {"outline", 0},
                                 .destination = {"outline_pass", 0}});
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "graph.cycle")) {
            return 7;
        }

        invalid = fixture.document;
        invalid.feature_nodes.front().parameters = {{999, true}};
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "parameter.unknown", "outline") ||
            !has_code(diagnostics, "parameter.invalid", "outline")) {
            return 8;
        }

        invalid = fixture.document;
        invalid.feature_nodes.push_back({.node_id = "outline_2",
                                         .type = FeatureId("Game.CustomOutline"),
                                         .version = {1, 0, 0},
                                         .pipeline_node = "pipeline",
                                         .point = FeaturePointId("PostProcess")});
        invalid.pass_nodes.push_back({.node_id = "outline_pass_2",
                                      .type = RenderPassTypeId("Game.CustomOutlinePass"),
                                      .version = {1, 0, 0},
                                      .feature_node = "outline_2",
                                      .part_id = "render"});
        invalid.edges.push_back({.edge_id = "cross_feature_order",
                                 .kind = DocumentEdgeKind::kOrdering,
                                 .source = {"outline_pass", 0},
                                 .destination = {"outline_pass_2", 0},
                                 .binding_id = 0});
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "edge.ordering.feature", {}, "cross_feature_order"))
            return 12;

        invalid = fixture.document;
        invalid.edges.push_back({.edge_id = "feature_pass_order",
                                 .kind = DocumentEdgeKind::kOrdering,
                                 .source = {"outline", 0},
                                 .destination = {"outline_pass", 0},
                                 .binding_id = 0});
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "edge.ordering.type", {}, "feature_pass_order"))
            return 17;

        invalid = fixture.document;
        auto pass_entry =
            *fixture.catalog.find({AuthoringTypeKind::kRenderPass, RenderPassTypeId("Game.CustomOutlinePass").value()});
        pass_entry.key = {AuthoringTypeKind::kRenderPass, RenderPassTypeId("Game.SecondPass").value()};
        pass_entry.name = "Game.SecondPass";
        pass_entry.display_name = "Second Pass";
        pass_entry.declaration =
            PassAuthoringDeclaration{.type = RenderPassTypeId("Game.SecondPass"), .allowed_queue_mask = 1};
        auto feature_entry =
            *fixture.catalog.find({AuthoringTypeKind::kFeature, FeatureId("Game.CustomOutline").value()});
        auto feature_declaration = std::get<FeatureAuthoringDeclaration>(feature_entry.declaration);
        feature_declaration.parts.push_back({.part_index = 2, .domain = FeatureDomain::kRender, .name = "render_2"});
        feature_declaration.allowed_pass_types.push_back(RenderPassTypeId("Game.SecondPass"));
        feature_entry.key = {AuthoringTypeKind::kFeature, FeatureId("Game.TwoRenderParts").value()};
        feature_entry.name = "Game.TwoRenderParts";
        feature_entry.display_name = "Two Render Parts";
        feature_entry.declaration = std::move(feature_declaration);
        TypeCatalog cross_part_catalog;
        const auto* pipeline_entry =
            fixture.catalog.find({AuthoringTypeKind::kPipeline, PipelineId("Game.CustomForward").value()});
        if (pipeline_entry == nullptr || !cross_part_catalog.add(*pipeline_entry) ||
            !cross_part_catalog.add(feature_entry) || !cross_part_catalog.add(pass_entry))
            return 13;
        invalid.feature_nodes.front().type = FeatureId("Game.TwoRenderParts");
        invalid.pass_nodes.push_back({.node_id = "second_render_pass",
                                      .type = RenderPassTypeId("Game.SecondPass"),
                                      .version = {1, 0, 0},
                                      .feature_node = "outline",
                                      .part_id = "render_2"});
        invalid.edges.push_back({.edge_id = "cross_part_order",
                                 .kind = DocumentEdgeKind::kOrdering,
                                 .source = {"outline_pass", 0},
                                 .destination = {"second_render_pass", 0},
                                 .binding_id = 0});
        diagnostics = validate_pipeline_document(invalid, cross_part_catalog);
        if (!has_code(diagnostics, "edge.ordering.part", {}, "cross_part_order"))
            return 14;

        auto fixed_catalog = fixture.catalog;
        auto fixed_feature =
            *fixed_catalog.find({AuthoringTypeKind::kFeature, FeatureId("Game.CustomOutline").value()});
        auto fixed_declaration = std::get<FeatureAuthoringDeclaration>(fixed_feature.declaration);
        fixed_declaration.allow_add_passes = false;
        fixed_feature.key = {AuthoringTypeKind::kFeature, FeatureId("Game.FixedFeature").value()};
        fixed_feature.name = "Game.FixedFeature";
        fixed_feature.display_name = "Fixed Feature";
        fixed_declaration.default_passes.clear();
        fixed_feature.declaration = std::move(fixed_declaration);
        if (!fixed_catalog.add(std::move(fixed_feature)))
            return 15;
        invalid = fixture.document;
        invalid.feature_nodes.front().type = FeatureId("Game.FixedFeature");
        invalid.pass_nodes.front().feature_node = "outline";
        diagnostics = validate_pipeline_document(invalid, fixed_catalog);
        if (!has_code(diagnostics, "pass.additional", "outline_pass"))
            return 16;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 9;
    }
}
