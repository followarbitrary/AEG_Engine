#pragma once

#include <stdexcept>

#include "pipeline_authoring.hpp"

namespace aeg::tests {

struct EditorCompositionFixture final {
    pipeline::TypeCatalog catalog{};
    pipeline::PipelineDocument document{};

    EditorCompositionFixture() {
        using namespace pipeline;
        const PipelineId pipeline_id("Game.EditorPipeline");
        const FeatureId feature_id("Game.EditorFeature");
        const RenderPassTypeId pass_id("Game.EditorPass");
        const ModuleId module("Game.Editor");

        PipelineAuthoringDeclaration pipeline_declaration;
        pipeline_declaration.stages = {{.id = StageId("RenderBuild"),
                                        .name = "Render Build",
                                        .phase = PhaseId("RenderBuild"),
                                        .execution_domain = ExecutionDomain::kRenderThread,
                                        .time_policy = TimePolicy::kFrame}};
        pipeline_declaration.feature_points = {{.id = FeaturePointId("PostProcess"),
                                                .name = "Post Process",
                                                .anchor_stage = StageId("RenderBuild"),
                                                .position = FeaturePointPosition::kAfter,
                                                .domains = to_mask(FeatureDomain::kRender),
                                                .scopes = to_mask(PipelineScope::kFrame),
                                                .flags = FeaturePointFlags::kMayCreateGraphicsPass}};
        TypeCatalogEntry pipeline_entry;
        pipeline_entry.key = {AuthoringTypeKind::kPipeline, pipeline_id.value()};
        pipeline_entry.version = {1, 0, 0};
        pipeline_entry.owner_module = module;
        pipeline_entry.name = "Game.EditorPipeline";
        pipeline_entry.display_name = "Editor Pipeline";
        pipeline_entry.domains = to_mask(FeatureDomain::kRender);
        pipeline_entry.scopes = to_mask(PipelineScope::kFrame);
        pipeline_entry.declaration = std::move(pipeline_declaration);

        FeatureAuthoringDeclaration feature_declaration;
        feature_declaration.parts = {{.part_index = 0, .domain = FeatureDomain::kRender, .name = "render"}};
        feature_declaration.allowed_pass_types = {pass_id};
        feature_declaration.allow_add_passes = true;
        TypeCatalogEntry feature_entry;
        feature_entry.key = {AuthoringTypeKind::kFeature, feature_id.value()};
        feature_entry.version = {1, 0, 0};
        feature_entry.owner_module = module;
        feature_entry.name = "Game.EditorFeature";
        feature_entry.display_name = "Editor Feature";
        feature_entry.domains = to_mask(FeatureDomain::kRender);
        feature_entry.scopes = to_mask(PipelineScope::kFrame);
        feature_entry.ports = {{.id = 22,
                                .name = "SceneColor",
                                .direction = PortDirection::kInput,
                                .value_kind = PortValueKind::kTexture,
                                .schema_id = 1,
                                .binding_id = 77,
                                .resource_constraints = {.resource_class = ResourceClass::kTexture}}};
        feature_entry.declaration = std::move(feature_declaration);

        TypeCatalogEntry pass_entry;
        pass_entry.key = {AuthoringTypeKind::kRenderPass, pass_id.value()};
        pass_entry.version = {1, 0, 0};
        pass_entry.owner_module = module;
        pass_entry.name = "Game.EditorPass";
        pass_entry.display_name = "Editor Pass";
        pass_entry.domains = to_mask(FeatureDomain::kRender);
        pass_entry.scopes = to_mask(PipelineScope::kFrame);
        pass_entry.ports = {{.id = 11,
                             .name = "ColorOut",
                             .direction = PortDirection::kOutput,
                             .value_kind = PortValueKind::kTexture,
                             .schema_id = 1,
                             .binding_id = 77,
                             .resource_constraints = {.resource_class = ResourceClass::kTexture}}};
        pass_entry.declaration = PassAuthoringDeclaration{
            .type = pass_id, .allowed_queue_mask = uint64_t{1} << static_cast<uint8_t>(rhi::QueueClass::kGraphics)};

        if (!catalog.add(pipeline_entry) || !catalog.add(feature_entry) || !catalog.add(pass_entry))
            throw std::runtime_error("editor composition fixture catalog is invalid");

        document.schema_version = 1;
        document.asset_uuid = "00000000-0000-0000-0000-000000000001";
        document.name = "Editor Composition";
        document.module_requirements = {module};
        document.pipeline_nodes = {
            {.node_id = "pipeline", .type = pipeline_id, .version = {1, 0, 0}, .scope = PipelineScope::kFrame}};
        document.feature_nodes = {{.node_id = "feature",
                                   .type = feature_id,
                                   .version = {1, 0, 0},
                                   .pipeline_node = "pipeline",
                                   .point = FeaturePointId("PostProcess")}};
        document.pass_nodes = {
            {.node_id = "pass", .type = pass_id, .version = {1, 0, 0}, .feature_node = "feature", .part_id = "render"}};
        document.edges = {{.edge_id = "color",
                           .kind = DocumentEdgeKind::kRenderResource,
                           .source = {"pass", 11},
                           .destination = {"feature", 22},
                           .binding_id = 77}};
    }
};

}  // namespace aeg::tests
