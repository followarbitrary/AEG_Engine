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
        feature_declaration.parts = {{.part_index = 0, .domain = FeatureDomain::kRender, .name = "render"}};
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
        invalid.pass_nodes.front().part_id = "physics";
        diagnostics = validate_pipeline_document(invalid, fixture.catalog);
        if (!has_code(diagnostics, "part.missing", "outline_pass")) {
            return 4;
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
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 9;
    }
}
