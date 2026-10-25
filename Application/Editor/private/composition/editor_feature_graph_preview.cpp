#include "editor_feature_graph_preview.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aeg::application {
namespace {
using namespace feature;

const FeatureCatalogEntry* find_entry(const FeatureCatalog& catalog, FeatureDefinitionId id) {
    const auto it = std::find_if(catalog.entries().begin(), catalog.entries().end(),
                                 [id](const auto& entry) { return entry.definition_id == id; });
    return it == catalog.entries().end() ? nullptr : &*it;
}
const FeatureNode* find_node(const FeatureGraphDocument& graph, FeatureNodeId id) {
    const auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                 [id](const auto& node) { return node.node_id == id; });
    return it == graph.nodes.end() ? nullptr : &*it;
}

void add_error(EditorFeatureGraphPreview& preview, std::string message,
               const FeatureNode* node = nullptr, std::optional<PortId> port = std::nullopt,
               std::optional<std::size_t> edge = std::nullopt, OwnerPath owner = {});

std::string definition_path(const std::vector<FeatureDefinitionId>& path,
                            std::optional<FeatureDefinitionId> tail = std::nullopt) {
    std::string result;
    for (const auto id : path) {
        if (!result.empty()) result += " -> ";
        result += std::to_string(id.value());
    }
    if (tail) {
        if (!result.empty()) result += " -> ";
        result += std::to_string(tail->value());
    }
    return result;
}

FeatureDomainMask projected_port_domains(
    const FeatureCatalog& catalog, FeatureDefinitionId definition_id, PortId port_id,
    PortDirection direction,
    std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>>& visiting) {
    if (!visiting.insert(definition_id).second)
        return 0;
    const auto* entry = find_entry(catalog, definition_id);
    if (entry == nullptr) {
        visiting.erase(definition_id);
        return 0;
    }
    const auto& contract = entry->contract;
    if (contract.structure == FeatureStructure::kAtomic || !contract.definition_id.is_valid()) {
        visiting.erase(definition_id);
        return entry->domains;
    }
    FeatureDomainMask domains = 0;
    for (const auto& mapping : contract.child_port_mappings) {
        if (mapping.parent_port != port_id || mapping.direction != direction)
            continue;
        domains = static_cast<FeatureDomainMask>(
            domains | projected_port_domains(catalog, mapping.child_definition, mapping.child_port,
                                             direction, visiting));
    }
    visiting.erase(definition_id);
    // Composite boundary ports are projected through their declared mappings.
    // Do not substitute the composite's aggregate domain mask when a port has
    // no mapping: that would make edge masks appear cross-domain merely because
    // the node itself is multi-domain.
    return domains;
}

FeatureDomainMask projected_port_domains(const FeatureCatalog& catalog, FeatureDefinitionId definition_id,
                                         PortId port_id, PortDirection direction) {
    std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>> visiting;
    return projected_port_domains(catalog, definition_id, port_id, direction, visiting);
}

FeatureDomainMask composite_domains(
    const FeatureCatalog& catalog, FeatureDefinitionId definition_id,
    std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>>& visiting) {
    const auto* entry = find_entry(catalog, definition_id);
    if (entry == nullptr) return 0;
    if (!entry->contract.definition_id.is_valid() ||
        entry->contract.structure == FeatureStructure::kAtomic)
        return entry->domains;
    if (!visiting.insert(definition_id).second)
        return 0;
    // Keep the authored declaration in addition to any transitive child
    // domains. A multi-domain Composite must remain present in every lane it
    // explicitly declares, even when its children only cover a subset.
    FeatureDomainMask domains = entry->domains;
    for (const auto child : entry->contract.children)
        domains = static_cast<FeatureDomainMask>(domains | composite_domains(catalog, child, visiting));
    visiting.erase(definition_id);
    return domains;
}

FeatureDomainMask composite_domains(const FeatureCatalog& catalog, FeatureDefinitionId definition_id) {
    std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>> visiting;
    return composite_domains(catalog, definition_id, visiting);
}

void validate_composite_children(const FeatureCatalog& catalog, FeatureDefinitionId definition_id,
                                 const FeatureNode& node,
                                 std::vector<FeatureDefinitionId>& visiting,
                                 std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>>& complete,
                                 EditorFeatureGraphPreview& preview) {
    if (complete.contains(definition_id))
        return;
    if (std::find(visiting.begin(), visiting.end(), definition_id) != visiting.end()) {
        add_error(preview, "Composite definition recursively contains a child cycle: " +
                             definition_path(visiting, definition_id), &node);
        return;
    }
    const auto* entry = find_entry(catalog, definition_id);
    if (entry == nullptr) {
        add_error(preview, "Composite child definition " + std::to_string(definition_id.value()) +
                             " is missing from the catalog (path " + definition_path(visiting, definition_id) + ")",
                  &node);
        return;
    }
    if (!entry->contract.definition_id.is_valid() || entry->contract.structure != FeatureStructure::kComposite) {
        complete.insert(definition_id);
        return;
    }
    visiting.push_back(definition_id);
    for (const auto child : entry->contract.children) {
        if (find_entry(catalog, child) == nullptr) {
            add_error(preview, "Composite child definition " + std::to_string(child.value()) +
                                 " is missing from the catalog (path " + definition_path(visiting, child) + ")",
                      &node);
            continue;
        }
        validate_composite_children(catalog, child, node, visiting, complete, preview);
    }
    visiting.pop_back();
    complete.insert(definition_id);
}

void add_error(EditorFeatureGraphPreview& preview, std::string message,
               const FeatureNode* node, std::optional<PortId> port,
               std::optional<std::size_t> edge, OwnerPath owner) {
    EditorFeatureGraphPreviewDiagnostic diagnostic{};
    diagnostic.message = std::move(message);
    if (node != nullptr) {
        diagnostic.node_id = node->node_id;
        diagnostic.owner_path = node->owner_path;
    } else {
        diagnostic.owner_path = std::move(owner);
    }
    diagnostic.port_id = port;
    diagnostic.edge_index = edge;
    preview.diagnostics.push_back(std::move(diagnostic));
}
}  // namespace

foundation::ResultValue<EditorFeatureGraphPreview> EditorFeatureGraphPreviewCompiler::compile(
    const FeatureGraphDocument& graph, const FeatureCatalog& catalog) const {
    EditorFeatureGraphPreview preview{};
    preview.graph_revision = graph.draft_revision;
    preview.catalog_digest = catalog.contract_digest();
    preview.lanes.reserve(5);
    for (uint8_t index = 0; index < 5; ++index)
        preview.lanes.push_back({static_cast<FeatureDomain>(index), {}});

    if (graph.asset_uuid.empty() || graph.schema_version == 0 || graph.graph_version == 0)
        add_error(preview, "FeatureGraph identity and schema version are required");
    if (graph.schema_version != 1)
        add_error(preview, "Unsupported FeatureGraph schema; migrate explicitly before preview");
    if (graph.contract_digest != 0 && graph.contract_digest != preview.catalog_digest) {
        const auto anchor = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                         [](const auto& node) { return node.node_id.is_valid(); });
        add_error(preview, "FeatureGraph contract digest is stale",
                  anchor == graph.nodes.end() ? nullptr : &*anchor);
    }

    std::unordered_map<FeatureNodeId, const FeatureNode*, StableIdHash<FeatureNodeIdTag>> nodes;
    std::unordered_map<ClockId, std::size_t, StableIdHash<ClockIdTag>> clocks;
    for (const auto& node : graph.nodes) {
        if (!node.node_id.is_valid() || !nodes.emplace(node.node_id, &node).second) {
            add_error(preview, "FeatureGraph node ids must be stable and unique", &node);
            continue;
        }
        const auto* entry = find_entry(catalog, node.definition_id);
        if (entry == nullptr) {
            add_error(preview, "FeatureGraph node definition is missing from the catalog", &node);
            continue;
        }
        if (node.definition_version != entry->version)
            add_error(preview, "FeatureGraph node definition is stale", &node);
        if (const auto result = validate_feature_contract_draft(entry->contract); !result)
            add_error(preview, result.message, &node);
        if (const auto result = validate_feature_parameter_blob(node.parameter_blob, entry->contract.parameters); !result)
            add_error(preview, result.message, &node);
        std::unordered_set<FeatureNodeId, StableIdHash<FeatureNodeIdTag>> path;
        for (const auto owner : node.owner_path.nodes)
            if (!path.insert(owner).second) { add_error(preview, "FeatureGraph owner paths must not contain cycles", &node); break; }
        if (entry->contract.structure == FeatureStructure::kComposite) {
            std::vector<FeatureDefinitionId> visiting;
            std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>> complete;
            validate_composite_children(catalog, entry->definition_id, node, visiting, complete, preview);
        }

        const auto domains = composite_domains(catalog, entry->definition_id);
        for (uint8_t index = 0; index < 5; ++index)
            if ((domains & to_mask(static_cast<FeatureDomain>(index))) != 0)
                preview.lanes[index].node_ids.push_back(node.node_id);
        auto [clock_it, inserted] = clocks.emplace(entry->clock, preview.clock_groups.size());
        if (inserted) preview.clock_groups.push_back({entry->clock, {}});
        preview.clock_groups[clock_it->second].node_ids.push_back(node.node_id);
        preview.provenance.push_back({node.node_id, node.definition_id, node.owner_path, entry->owner_module});
    }

    std::unordered_set<FeatureNodeId, StableIdHash<FeatureNodeIdTag>> process_owners;
    for (std::size_t index = 0; index < graph.process_subgraphs.size(); ++index) {
        const auto& process = graph.process_subgraphs[index];
        const auto* owner = find_node(graph, process.owner_node);
        const auto* entry = owner == nullptr ? nullptr : find_entry(catalog, owner->definition_id);
        if (owner == nullptr) { add_error(preview, "Process subgraph owner node is missing", nullptr, std::nullopt, std::nullopt); continue; }
        if (!process_owners.insert(process.owner_node).second) {
            add_error(preview, "each Feature node may declare only one Process subgraph", owner);
            continue;
        }
        if (entry == nullptr) { add_error(preview, "Process subgraph owner definition is missing", owner); continue; }
        if (const auto result = canonicalize_process_subgraph(process, entry->contract); !result)
            add_error(preview, result.result.message, owner);
    }

    for (std::size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        const auto* source = find_node(graph, edge.source_node);
        const auto* target = find_node(graph, edge.target_node);
        const auto* source_entry = source == nullptr ? nullptr : find_entry(catalog, source->definition_id);
        const auto* target_entry = target == nullptr ? nullptr : find_entry(catalog, target->definition_id);
        const auto source_domains = source_entry == nullptr ? FeatureDomainMask{0} :
            projected_port_domains(catalog, source_entry->definition_id, edge.source_port, PortDirection::kOutput);
        const auto target_domains = target_entry == nullptr ? FeatureDomainMask{0} :
            projected_port_domains(catalog, target_entry->definition_id, edge.target_port, PortDirection::kInput);
        EditorFeatureGraphPreviewEdge projected{index, edge.kind, source_domains, target_domains,
                                                edge.source_clock, edge.target_clock, edge.capacity,
                                                edge.deadline_ticks, source_domains != target_domains, {}};
        FeatureGraphDocument validation_graph = graph;
        validation_graph.edges.erase(validation_graph.edges.begin() + static_cast<std::ptrdiff_t>(index));
        projected.validation = validate_feature_edge(validation_graph, edge, catalog);
        preview.edges.push_back(projected);
        if (!projected.validation)
            add_error(preview, projected.validation.message, source != nullptr ? source : target,
                      source != nullptr ? std::optional<PortId>(edge.source_port) : std::optional<PortId>(edge.target_port), index);
    }
    return {foundation::Result::success(), std::move(preview)};
}
}  // namespace aeg::application
