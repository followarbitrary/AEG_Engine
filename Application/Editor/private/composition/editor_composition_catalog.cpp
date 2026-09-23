#include "editor_composition_catalog.hpp"

#include <algorithm>
#include <variant>

namespace aeg::application {
namespace {
using namespace pipeline;

bool compatible(FeatureDomainMask domains, PipelineScopeMask scopes, const AuthoringFeaturePoint& point) {
    return (domains & point.domains) != 0 && (scopes & point.scopes) != 0;
}

const PipelineNodeDocument* find_pipeline(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.pipeline_nodes.end() ? nullptr : &*found;
}

const FeatureNodeDocument* find_feature(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.feature_nodes.begin(), document.feature_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.feature_nodes.end() ? nullptr : &*found;
}

void sort_palette(std::vector<EditorPaletteEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        if (left.label != right.label)
            return left.label < right.label;
        if (left.type.kind != right.type.kind)
            return left.type.kind < right.type.kind;
        return left.type.id < right.type.id;
    });
}
}  // namespace

foundation::ResultValue<std::vector<EditorPaletteEntry>> build_composition_palette(
    const pipeline::PipelineDocument& document, const pipeline::TypeCatalog& catalog,
    const EditorGraphAddress& address) {
    std::vector<EditorPaletteEntry> result;
    if (address.kind == EditorGraphKind::kPipeline) {
        for (const auto& entry : catalog.entries()) {
            if (entry.key.kind == pipeline::AuthoringTypeKind::kPipeline)
                result.push_back({entry.key, entry.display_name, true, {}});
        }
    } else if (address.kind == EditorGraphKind::kFeature) {
        const auto* pipeline_node = find_pipeline(document, address.pipeline_node);
        if (!pipeline_node)
            return {foundation::not_found("Pipeline graph address does not exist"), {}};
        const auto* pipeline_entry =
            catalog.find({pipeline::AuthoringTypeKind::kPipeline, pipeline_node->type.value()});
        if (!pipeline_entry)
            return {foundation::not_found("Pipeline type is unavailable"), {}};
        const auto* declaration = std::get_if<pipeline::PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
        if (!declaration)
            return {foundation::invalid_argument("Pipeline catalog declaration is invalid"), {}};
        for (const auto& entry : catalog.entries()) {
            if (entry.key.kind != pipeline::AuthoringTypeKind::kFeature)
                continue;
            bool allowed = false;
            for (const auto& point : declaration->feature_points)
                allowed |= compatible(entry.domains, entry.scopes, point);
            if (allowed)
                result.push_back({entry.key, entry.display_name, true, {}});
        }
    } else {
        const auto* feature_node = find_feature(document, address.feature_node);
        if (!feature_node)
            return {foundation::not_found("Feature graph address does not exist"), {}};
        const auto* feature_entry = catalog.find({pipeline::AuthoringTypeKind::kFeature, feature_node->type.value()});
        if (!feature_entry)
            return {foundation::not_found("Feature type is unavailable"), {}};
        const auto* declaration = std::get_if<pipeline::FeatureAuthoringDeclaration>(&feature_entry->declaration);
        if (!declaration)
            return {foundation::invalid_argument("Feature catalog declaration is invalid"), {}};
        if (!declaration->allow_add_passes)
            return {foundation::Result::success(), std::move(result)};
        for (const auto type : declaration->allowed_pass_types) {
            const auto* pass = catalog.find({pipeline::AuthoringTypeKind::kRenderPass, type.value()});
            if (pass)
                result.push_back({pass->key, pass->display_name, true, {}});
        }
    }
    sort_palette(result);
    return {foundation::Result::success(), std::move(result)};
}

foundation::ResultValue<EditorDiagnosticTarget> resolve_composition_diagnostic(
    const pipeline::PipelineDocument& document, const pipeline::DocumentDiagnostic& diagnostic) {
    EditorDiagnosticTarget target;
    target.node_id = diagnostic.node_id;
    target.edge_id = diagnostic.edge_id;
    target.port_id = diagnostic.port_id;
    target.parameter_id = diagnostic.parameter_id;
    if (!diagnostic.node_id.empty()) {
        for (const auto& node : document.pipeline_nodes)
            if (node.node_id == diagnostic.node_id)
                target.graph = {EditorGraphKind::kPipeline, node.node_id, {}, {}};
        for (const auto& node : document.feature_nodes)
            if (node.node_id == diagnostic.node_id)
                target.graph = {EditorGraphKind::kFeature, node.pipeline_node, node.node_id, {}};
        for (const auto& node : document.pass_nodes)
            if (node.node_id == diagnostic.node_id) {
                const auto* feature = find_feature(document, node.feature_node);
                target.graph = {EditorGraphKind::kPass, feature ? feature->pipeline_node : std::string{},
                                node.feature_node, node.part_id};
            }
        for (const auto& node : document.resources)
            if (node.node_id == diagnostic.node_id)
                target.graph = {EditorGraphKind::kPass, {}, {}, {}};
    }
    if (target.node_id.empty() && target.edge_id.empty())
        return {foundation::Result::success(), std::move(target)};
    if (!target.node_id.empty() && target.graph.pipeline_node.empty() && target.graph.feature_node.empty())
        return {foundation::not_found("Authored diagnostic source node does not exist"), {}};
    return {foundation::Result::success(), std::move(target)};
}

}  // namespace aeg::application
