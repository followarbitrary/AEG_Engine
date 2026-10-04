/** @file renderer_authoring_smoke.cpp */
#include <algorithm>
#include <iostream>
#include <renderer_variants.hpp>
#include <variant>

#include "renderer_variant_document_fixture.hpp"

int main() {
    aeg::pipeline::TypeCatalog catalog;
    if (!aeg::renderer::register_renderer_authoring_types(catalog)) {
        return 1;
    }
    if (catalog.entries().size() != 11u) {
        return 2;
    }
    const auto* deferred =
        catalog.find({aeg::pipeline::AuthoringTypeKind::kPipeline, aeg::renderer::kDeferredRenderPipelineId.value()});
    const auto* forward_feature =
        catalog.find({aeg::pipeline::AuthoringTypeKind::kFeature, aeg::renderer::kForwardRenderFeatureId.value()});
    if (deferred == nullptr || forward_feature == nullptr) {
        return 3;
    }
    const auto* declaration = std::get_if<aeg::pipeline::FeatureAuthoringDeclaration>(&forward_feature->declaration);
    if (declaration == nullptr || declaration->allowed_pass_types.size() != 5u ||
        declaration->default_resources.size() != 5u || !declaration->allow_add_passes) {
        return 4;
    }
    aeg::pipeline::TypeCatalogEntry collision = *deferred;
    collision.name = "Renderer.ConflictingPipeline";
    if (catalog.add(collision)) {
        return 5;
    }
    aeg::pipeline::PipelineRegistry pipelines;
    aeg::pipeline::RenderPassRegistry passes;
    if (!aeg::renderer::register_builtin_rendering(pipelines, passes))
        return 6;
    for (bool forward : {false, true}) {
        auto document = aeg::test::renderer_document(catalog, forward);
        if (document.feature_nodes.size() != 1 || document.pass_nodes.size() != (forward ? 5u : 4u) ||
            document.edges.empty())
            return 7;
        const auto diagnostics = aeg::pipeline::validate_pipeline_document(document, catalog);
        for (const auto& diagnostic : diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        if (!diagnostics.empty())
            return 8;
        auto prepared = aeg::pipeline::prepare_pipeline_document(document, catalog, pipelines, passes);
        if (!prepared || prepared.value.data_dependencies.empty())
            return 9;
        auto reversed = document;
        std::reverse(reversed.pass_nodes.begin(), reversed.pass_nodes.end());
        if (aeg::pipeline::document_semantic_digest(reversed) ==
            aeg::pipeline::document_semantic_digest(document))
            return 23;
        auto ordered = aeg::pipeline::prepare_pipeline_document(reversed, catalog, pipelines, passes);
        if (!ordered ||
            ordered.value.configurations.front()->features.front().passes.front().source !=
                document.pass_nodes[1].node_id ||
            !std::any_of(ordered.value.configurations.front()->features.front().passes.begin() + 1,
                         ordered.value.configurations.front()->features.front().passes.end(),
                         [](const auto& pass) { return !pass.ordered_after.empty(); })) {
            return 20;
        }
        auto legacy_ordered = reversed;
        legacy_ordered.edges.push_back({"legacy-pass-order", aeg::pipeline::DocumentEdgeKind::kOrdering,
                                        {document.pass_nodes[0].node_id, 0, 0},
                                        {document.pass_nodes[1].node_id, 0, 0}, 0});
        auto migrated_order = aeg::pipeline::prepare_pipeline_document(legacy_ordered, catalog, pipelines, passes);
        if (!migrated_order ||
            migrated_order.value.configurations.front()->features.front().passes.front().source !=
                document.pass_nodes[1].node_id)
            return 22;
        auto explicitly_empty = document;
        explicitly_empty.pass_nodes.clear();
        explicitly_empty.edges.clear();
        explicitly_empty.input_bindings.clear();
        explicitly_empty.present_connections = {document.feature_nodes.front().node_id};
        auto empty_materialized =
            aeg::pipeline::prepare_pipeline_document(explicitly_empty, catalog, pipelines, passes);
        if (!empty_materialized ||
            !empty_materialized.value.configurations.front()->features.front().passes.empty())
            return 21;
        auto configuration = prepared.value.configurations.front();
        aeg::pipeline::PipelineScopeHost host(aeg::pipeline::PipelineScope::kFrame, configuration->instance);
        if (!host.bind_pipeline(configuration->pipeline) || !host.set_configuration(configuration) ||
            !host.rebuild(pipelines) || host.active_plan()->feature_contributions.size() != 1)
            return 10;
        auto rebound = std::make_shared<aeg::pipeline::PipelineInstanceConfiguration>(*configuration);
        rebound->features.front().passes.front().resource_bindings.emplace(1, 2);
        if (!host.set_configuration(rebound) ||
            host.rebuild(pipelines).code != aeg::foundation::ResultCode::kUnsupported)
            return 16;
        const auto definition = pipelines.find_definition(configuration->pipeline);
        const auto* pipeline_entry =
            catalog.find({aeg::pipeline::AuthoringTypeKind::kPipeline, configuration->pipeline.value()});
        if (!definition || pipeline_entry == nullptr)
            return 17;
        const auto* pipeline_declaration =
            std::get_if<aeg::pipeline::PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
        if (pipeline_declaration == nullptr ||
            pipeline_declaration->feature_points.size() != definition.value->feature_points.size())
            return 18;
        for (const auto& runtime_point : definition.value->feature_points) {
            const auto authored =
                std::find_if(pipeline_declaration->feature_points.begin(), pipeline_declaration->feature_points.end(),
                             [&](const auto& point) { return point.id == runtime_point.descriptor.id; });
            if (authored == pipeline_declaration->feature_points.end() ||
                authored->anchor_stage != runtime_point.descriptor.anchor_stage ||
                authored->domains != runtime_point.descriptor.allowed_domains ||
                authored->scopes != runtime_point.descriptor.allowed_scopes ||
                authored->flags != runtime_point.descriptor.flags)
                return 19;
        }
        auto invalid = document;
        invalid.present_connections.clear();
        if (aeg::pipeline::validate_pipeline_document(invalid, catalog).empty())
            return 11;
        invalid = document;
        invalid.pass_nodes.front().enabled = false;
        if (aeg::pipeline::validate_pipeline_document(invalid, catalog).empty())
            return 15;
        invalid = document;
        invalid.pass_nodes.front().type = aeg::pipeline::RenderPassTypeId("Renderer.MissingPass");
        if (aeg::pipeline::validate_pipeline_document(invalid, catalog).empty())
            return 12;
        invalid = document;
        invalid.resources.front().type = {};
        if (aeg::pipeline::validate_pipeline_document(invalid, catalog).empty())
            return 13;
    }
    if (aeg::renderer::register_renderer_authoring_types(catalog))
        return 14;
    return 0;
}
