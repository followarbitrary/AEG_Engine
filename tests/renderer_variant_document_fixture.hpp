#pragma once
#include <renderer_variants.hpp>

namespace aeg::test {
inline pipeline::PipelineDocument renderer_document(const pipeline::TypeCatalog& catalog, bool forward) {
    using namespace pipeline;
    const auto type = forward ? renderer::kForwardRenderPipelineId : renderer::kDeferredRenderPipelineId;
    PipelineDocument document;
    document.asset_uuid = "00000000-0000-4000-8000-000000000102";
    document.name = "Renderer variant";
    document.pipeline_nodes.push_back({"pipeline", type, {1, 0, 0}, PipelineScope::kFrame, "Renderer"});
    const auto* entry = catalog.find({AuthoringTypeKind::kPipeline, type.value()});
    const auto& declaration = std::get<PipelineAuthoringDeclaration>(entry->declaration);
    for (const auto& feature : declaration.default_features) {
        document.feature_nodes.push_back({feature.node_id, feature.type, feature.version, "pipeline", feature.point});
        const auto* feature_entry = catalog.find({AuthoringTypeKind::kFeature, feature.type.value()});
        const auto& defaults = std::get<FeatureAuthoringDeclaration>(feature_entry->declaration);
        for (const auto& pass : defaults.default_passes)
            document.pass_nodes.push_back(
                {pass.node_id, pass.type, pass.version, feature.node_id, pass.part_id, pass.parameters, pass.enabled});
        for (const auto& resource : defaults.default_resources)
            document.resources.push_back(
                {.node_id = resource.node_id,
                 .resource_id = resource.resource_id,
                 .type =
                     ArtifactTypeId(resource.constraints.resource_class == ResourceClass::kBuffer ? "Renderer.Buffer"
                                                                                                  : "Renderer.Texture"),
                 .owner_scope = PipelineScope::kFrame,
                 .visible_to = to_mask(PipelineScope::kFrame),
                 .external_input = resource.resource_id == RenderResourceId("Renderer.SceneColor"),
                 .storage = ArtifactStorage::kGpuLogicalResource,
                 .lifetime = ArtifactLifetime::kFrame,
                 .ownership = OwnershipPolicy::kGraphOwned,
                 .resource_constraints = resource.constraints});
        document.edges.insert(document.edges.end(), defaults.default_edges.begin(), defaults.default_edges.end());
    }
    document.present_connections = {"SkyboxPostProcess"};
    return document;
}
}  // namespace aeg::test
