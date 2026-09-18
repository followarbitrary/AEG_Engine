#include "editor_composition_view.hpp"

#include <algorithm>
#include <variant>

namespace aeg::application {
namespace {
using namespace pipeline;

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

const TypeCatalogEntry* find_type(const TypeCatalog& catalog, AuthoringTypeKind kind, uint64_t id) {
    return catalog.find({kind, id});
}

void sort_nodes(EditorCompositionGraphView& view) {
    std::stable_sort(view.nodes.begin(), view.nodes.end(), [](const auto& left, const auto& right) {
        if (left.kind != right.kind)
            return left.kind < right.kind;
        return left.node_id < right.node_id;
    });
    std::stable_sort(view.edges.begin(), view.edges.end(),
                     [](const auto& left, const auto& right) { return left.edge_id < right.edge_id; });
}

void append_ports(EditorGraphNodeView& node, const TypeCatalogEntry* type) {
    if (!type)
        return;
    for (const auto& port : type->ports) {
        const auto kind = port.value_kind == PortValueKind::kArtifact      ? DocumentEdgeKind::kArtifact
                          : port.value_kind == PortValueKind::kRenderState ? DocumentEdgeKind::kRenderState
                                                                           : DocumentEdgeKind::kRenderResource;
        node.ports.push_back({port.id, port.name, port.direction == PortDirection::kInput, port.required, kind});
    }
}

}  // namespace

foundation::ResultValue<EditorCompositionGraphView> project_composition_graph(
    const pipeline::PipelineDocument& document, const pipeline::TypeCatalog& catalog,
    const EditorGraphAddress& address) {
    EditorCompositionGraphView view;
    view.address = address;
    if (address.kind == EditorGraphKind::kPipeline) {
        for (const auto& pipeline : document.pipeline_nodes) {
            const auto* type = find_type(catalog, AuthoringTypeKind::kPipeline, pipeline.type.value());
            view.nodes.push_back({pipeline.node_id, EditorGraphNodeKind::kPipeline, false, type == nullptr, {}});
            append_ports(view.nodes.back(), type);
        }
    } else if (address.kind == EditorGraphKind::kFeature) {
        const auto* pipeline = find_pipeline(document, address.pipeline_node);
        if (!pipeline)
            return {foundation::not_found("Pipeline graph address does not exist"), {}};
        for (const auto& feature : document.feature_nodes) {
            if (feature.pipeline_node != pipeline->node_id)
                continue;
            const auto* type = find_type(catalog, AuthoringTypeKind::kFeature, feature.type.value());
            view.nodes.push_back({feature.node_id, EditorGraphNodeKind::kFeature, false, type == nullptr, {}});
            append_ports(view.nodes.back(), type);
            if (type) {
                if (const auto* declaration = std::get_if<FeatureAuthoringDeclaration>(&type->declaration)) {
                    for (const auto& part : declaration->parts)
                        view.nodes.push_back(
                            {feature.node_id + "::part::" + part.name, EditorGraphNodeKind::kPart, true, false, {}});
                }
            }
        }
    } else {
        const auto* feature = find_feature(document, address.feature_node);
        if (!feature || (address.pipeline_node.size() && feature->pipeline_node != address.pipeline_node))
            return {foundation::not_found("Feature graph address does not exist"), {}};
        std::vector<std::string> node_ids;
        const auto* feature_type = find_type(catalog, AuthoringTypeKind::kFeature, feature->type.value());
        view.nodes.push_back({feature->node_id, EditorGraphNodeKind::kFeature, true, feature_type == nullptr, {}});
        append_ports(view.nodes.back(), feature_type);
        node_ids.push_back(feature->node_id);
        for (const auto& pass : document.pass_nodes) {
            if (pass.feature_node != feature->node_id || (!address.part_id.empty() && pass.part_id != address.part_id))
                continue;
            const auto* type = find_type(catalog, AuthoringTypeKind::kRenderPass, pass.type.value());
            view.nodes.push_back({pass.node_id, EditorGraphNodeKind::kPass, false, type == nullptr, {}});
            append_ports(view.nodes.back(), type);
            node_ids.push_back(pass.node_id);
        }
        for (const auto& resource : document.resources) {
            bool referenced = false;
            for (const auto& edge : document.edges)
                referenced |= edge.source.node_id == resource.node_id || edge.destination.node_id == resource.node_id;
            if (referenced) {
                view.nodes.push_back({resource.node_id, EditorGraphNodeKind::kResource, false, false, {}});
                node_ids.push_back(resource.node_id);
            }
        }
        std::sort(node_ids.begin(), node_ids.end());
        for (const auto& edge : document.edges) {
            const bool source = std::binary_search(node_ids.begin(), node_ids.end(), edge.source.node_id);
            const bool destination = std::binary_search(node_ids.begin(), node_ids.end(), edge.destination.node_id);
            if (source && destination)
                view.edges.push_back({edge.edge_id, false, edge.kind, edge.source.node_id, edge.source.port_id,
                                      edge.destination.node_id, edge.destination.port_id});
        }
    }
    sort_nodes(view);
    return {foundation::Result::success(), std::move(view)};
}

}  // namespace aeg::application
