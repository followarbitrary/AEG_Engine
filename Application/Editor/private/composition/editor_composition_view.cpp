#include "editor_composition_view.hpp"

#include <algorithm>
#include <variant>

namespace aeg::application {

foundation::ResultValue<EditorRuntimePlanView> project_runtime_plan(
    const pipeline::PipelinePlan& plan, std::string source, const pipeline::PipelineDocument* author_document) {
    EditorRuntimePlanView view;
    view.source = std::move(source);
    view.revision = plan.revision;
    const pipeline::PipelineNodeDocument* pipeline_node = nullptr;
    if (author_document != nullptr) {
        const auto found = std::find_if(author_document->pipeline_nodes.begin(), author_document->pipeline_nodes.end(),
                                        [&](const auto& node) {
                                            return node.type == plan.pipeline &&
                                                   pipeline::PipelineInstanceId(node.node_id) == plan.instance;
                                        });
        if (found != author_document->pipeline_nodes.end()) {
            pipeline_node = &*found;
            view.graph_asset = author_document->asset_uuid;
            view.graph_version = author_document->graph_version;
            view.semantic_digest = pipeline::document_semantic_digest(*author_document);
        }
    }
    view.nodes.reserve(plan.nodes.size());
    for (uint32_t index = 0; index < plan.nodes.size(); ++index) {
        const auto& node = plan.nodes[index];
        EditorRuntimeNodeView projected;
        projected.index = index;
        projected.stage_id = node.stage.value();
        projected.task_id = node.task.value();
        projected.execution_domain = node.execution_domain;
        projected.synthetic = node.synthetic;
        if (!node.synthetic && pipeline_node != nullptr) {
            const auto contribution = std::find_if(
                plan.feature_contributions.begin(), plan.feature_contributions.end(), [&](const auto& item) {
                    return node.definition_stage_index >= item.first_stage_index &&
                           node.definition_stage_index - item.first_stage_index < item.stage_count;
                });
            if (contribution == plan.feature_contributions.end()) {
                projected.author_node = pipeline_node->node_id;
            } else {
                projected.feature_part = contribution->part.value();
                const auto feature = std::find_if(
                    author_document->feature_nodes.begin(), author_document->feature_nodes.end(), [&](const auto& item) {
                        return item.pipeline_node == pipeline_node->node_id && item.type == contribution->feature &&
                               pipeline::FeatureInstanceId(item.node_id) == contribution->feature_instance;
                    });
                if (feature != author_document->feature_nodes.end())
                    projected.author_node = feature->node_id;
            }
        }
        view.nodes.push_back(std::move(projected));
    }
    view.dependencies.reserve(plan.dependencies.size());
    for (const auto& dependency : plan.dependencies) {
        if (dependency.source_node >= view.nodes.size() || dependency.destination_node >= view.nodes.size())
            return {foundation::invalid_argument("Runtime plan contains an invalid dependency endpoint"), {}};
        ++view.nodes[dependency.source_node].outgoing_dependencies;
        ++view.nodes[dependency.destination_node].incoming_dependencies;
        view.dependencies.push_back({dependency.source_node, dependency.destination_node, dependency.kind,
                                     dependency.artifact.value()});
    }
    return {foundation::Result::success(), std::move(view)};
}

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

bool pass_topology_is_fixed(const PipelineDocument& document, const TypeCatalog& catalog,
                            std::string_view feature_node_id) {
    const auto* feature = find_feature(document, feature_node_id);
    if (feature == nullptr)
        return false;
    const auto* entry = find_type(catalog, AuthoringTypeKind::kFeature, feature->type.value());
    const auto* declaration = entry == nullptr ? nullptr
                                                : std::get_if<FeatureAuthoringDeclaration>(&entry->declaration);
    return declaration != nullptr && !declaration->allow_add_passes;
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

void append_visible_edges(EditorCompositionGraphView& view, const pipeline::PipelineDocument& document,
                          const std::vector<std::string>& node_ids) {
    for (const auto& edge : document.edges) {
        // The authoring canvas only draws ordinary data dependencies. Legacy
        // order-only records are migration input and must never be presented
        // as a normal user-connectable line.
        if (edge.kind == pipeline::DocumentEdgeKind::kOrdering)
            continue;
        const bool source = std::find(node_ids.begin(), node_ids.end(), edge.source.node_id) != node_ids.end();
        const bool destination = std::find(node_ids.begin(), node_ids.end(), edge.destination.node_id) != node_ids.end();
        if (source && destination)
            view.edges.push_back({edge.edge_id, edge.kind == pipeline::DocumentEdgeKind::kOrdering, edge.kind,
                                  edge.source.node_id, edge.source.port_id,
                                  edge.destination.node_id, edge.destination.port_id, edge.source.slot_id,
                                  edge.destination.slot_id});
    }
}

void append_pass_edges(EditorCompositionGraphView& view, const pipeline::PipelineDocument& document,
                       const std::vector<std::string>& node_ids) {
    const auto is_pass = [&](std::string_view node_id) {
        return std::find(node_ids.begin(), node_ids.end(), node_id) != node_ids.end();
    };
    append_visible_edges(view, document, node_ids);

    // Renderer resources are relay nodes in the authoring document. The
    // strict Pass canvas hides those Resource cards, so preserve the visible
    // data flow by collapsing Pass -> Resource -> Pass into one visual edge.
    // The two source edge IDs are retained for editing and diagnostics.
    for (const auto& producer : document.edges) {
        if (producer.kind != pipeline::DocumentEdgeKind::kRenderResource ||
            !is_pass(producer.source.node_id) || is_pass(producer.destination.node_id))
            continue;
        for (const auto& consumer : document.edges) {
            if (consumer.kind != pipeline::DocumentEdgeKind::kRenderResource ||
                consumer.source.node_id != producer.destination.node_id ||
                consumer.source.port_id != producer.destination.port_id ||
                consumer.source.slot_id != producer.destination.slot_id ||
                consumer.binding_id != producer.binding_id ||
                !is_pass(consumer.destination.node_id) ||
                producer.source.node_id == consumer.destination.node_id)
                continue;
            const std::string visual_id = "relay:" + producer.edge_id + ":" + consumer.edge_id;
            const bool already_projected = std::any_of(
                view.edges.begin(), view.edges.end(),
                [&](const auto& edge) { return edge.edge_id == visual_id; });
            if (already_projected)
                continue;
            view.edges.push_back({visual_id, false, pipeline::DocumentEdgeKind::kRenderResource,
                                  producer.source.node_id, producer.source.port_id,
                                  consumer.destination.node_id, consumer.destination.port_id,
                                  producer.source.slot_id, consumer.destination.slot_id,
                                  {producer.edge_id, consumer.edge_id}});
        }
    }
}

void append_ports(EditorGraphNodeView& node, const TypeCatalogEntry* type, const PipelineDocument& document) {
    if (!type)
        return;
    for (const auto& port : type->ports) {
        const auto kind = port.value_kind == PortValueKind::kArtifact      ? DocumentEdgeKind::kArtifact
                          : port.value_kind == PortValueKind::kRenderState ? DocumentEdgeKind::kRenderState
                                                                           : DocumentEdgeKind::kRenderResource;
        const bool input = port.direction == PortDirection::kInput;
        const uint64_t group_id = port.input ? port.input->variadic_group_id : 0;
        if (input && group_id != 0) {
            std::vector<const VariadicInputSlotDocument*> slots;
            for (const auto& slot : document.variadic_slots)
                if (slot.node_id == node.node_id && slot.group_id == group_id)
                    slots.push_back(&slot);
            std::sort(slots.begin(), slots.end(), [](const auto* left, const auto* right) {
                return left->order < right->order;
            });
            const auto group = std::find_if(type->variadic_inputs.begin(), type->variadic_inputs.end(),
                                            [group_id](const auto& candidate) { return candidate.id == group_id; });
            for (const auto* slot : slots) {
                const std::string label = port.name + " [" +
                                          (group == type->variadic_inputs.end()
                                               ? std::to_string(slot->order + 1)
                                               : group->name + " " + std::to_string(slot->order + 1)) + "]";
                node.ports.push_back({port.id, label, true,
                                      port.input->presence == InputPresenceRequirement::kRequired,
                                      port.input->max_edges, group_id, port.value_kind,
                                      port.input->allowed_sources, kind, port.binding_id, slot->slot_id});
            }
            continue;
        }
        node.ports.push_back({port.id, port.name, input,
                              port.input ? port.input->presence == InputPresenceRequirement::kRequired : port.required,
                              port.input ? port.input->max_edges : port.max_connections, group_id, port.value_kind,
                              port.input ? port.input->allowed_sources : 0, kind, port.binding_id, 0});
    }
}

}  // namespace

foundation::ResultValue<EditorCompositionGraphView> project_composition_graph(
    const pipeline::PipelineDocument& document, const pipeline::TypeCatalog& catalog,
    const EditorGraphAddress& address) {
    EditorCompositionGraphView view;
    view.address = address;
    if (address.kind == EditorGraphKind::kPipeline) {
        std::vector<std::string> node_ids;
        for (const auto& pipeline : document.pipeline_nodes) {
            const auto* type = find_type(catalog, AuthoringTypeKind::kPipeline, pipeline.type.value());
            view.nodes.push_back({pipeline.node_id, EditorGraphNodeKind::kPipeline, false, type == nullptr, {}});
            append_ports(view.nodes.back(), type, document);
            node_ids.push_back(pipeline.node_id);
        }
        append_visible_edges(view, document, node_ids);
    } else if (address.kind == EditorGraphKind::kFeature) {
        const auto* pipeline = find_pipeline(document, address.pipeline_node);
        if (!pipeline)
            return {foundation::not_found("Pipeline graph address does not exist"), {}};
        std::vector<std::string> node_ids;
        for (const auto& feature : document.feature_nodes) {
            if (feature.pipeline_node != pipeline->node_id)
                continue;
            const auto* type = find_type(catalog, AuthoringTypeKind::kFeature, feature.type.value());
            view.nodes.push_back({feature.node_id, EditorGraphNodeKind::kFeature, false, type == nullptr, {}});
            append_ports(view.nodes.back(), type, document);
            node_ids.push_back(feature.node_id);
        }
        append_visible_edges(view, document, node_ids);
    } else {
        const auto* feature = find_feature(document, address.feature_node);
        if (!feature || (address.pipeline_node.size() && feature->pipeline_node != address.pipeline_node))
            return {foundation::not_found("Feature graph address does not exist"), {}};
        std::vector<std::string> node_ids;
        const bool fixed_pass_topology = pass_topology_is_fixed(document, catalog, feature->node_id);
        for (const auto& pass : document.pass_nodes) {
            if (pass.feature_node != feature->node_id || (!address.part_id.empty() && pass.part_id != address.part_id))
                continue;
            const auto* type = find_type(catalog, AuthoringTypeKind::kRenderPass, pass.type.value());
            view.nodes.push_back({pass.node_id, EditorGraphNodeKind::kPass, fixed_pass_topology, type == nullptr, {}});
            append_ports(view.nodes.back(), type, document);
            node_ids.push_back(pass.node_id);
        }
        // In the strict Pass graph presentation only Pass cards are projected.
        // Resource relays remain in the document, while their visible flow is
        // collapsed into Pass-to-Pass edges by append_pass_edges().
        append_pass_edges(view, document, node_ids);
    }
    sort_nodes(view);
    return {foundation::Result::success(), std::move(view)};
}

}  // namespace aeg::application
