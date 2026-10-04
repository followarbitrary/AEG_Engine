#include "editor_composition_commands.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <iomanip>
#include <iterator>
#include <new>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace aeg::application {
namespace {
using namespace pipeline;

bool has_node(const PipelineDocument& document, std::string_view id) {
    return std::any_of(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                       [&](const auto& node) { return node.node_id == id; }) ||
           std::any_of(document.feature_nodes.begin(), document.feature_nodes.end(),
                       [&](const auto& node) { return node.node_id == id; }) ||
           std::any_of(document.pass_nodes.begin(), document.pass_nodes.end(),
                       [&](const auto& node) { return node.node_id == id; }) ||
           std::any_of(document.resources.begin(), document.resources.end(),
                       [&](const auto& node) { return node.node_id == id; });
}

const TypeCatalogEntry* find_type(const TypeCatalog& catalog, AuthoringTypeKey key) {
    return catalog.find(key);
}

const TypeCatalogEntry* entry_for_node(const PipelineDocument& document, const TypeCatalog& catalog,
                                       std::string_view node_id) {
    for (const auto& node : document.pipeline_nodes)
        if (node.node_id == node_id)
            return find_type(catalog, {AuthoringTypeKind::kPipeline, node.type.value()});
    for (const auto& node : document.feature_nodes)
        if (node.node_id == node_id)
            return find_type(catalog, {AuthoringTypeKind::kFeature, node.type.value()});
    for (const auto& node : document.pass_nodes)
        if (node.node_id == node_id)
            return find_type(catalog, {AuthoringTypeKind::kRenderPass, node.type.value()});
    return nullptr;
}

const PortDescriptor* port_for(const TypeCatalogEntry& entry, uint64_t port_id, PortDirection direction) {
    const auto found = std::find_if(entry.ports.begin(), entry.ports.end(), [&](const auto& port) {
        return port.id == port_id && port.direction == direction;
    });
    return found == entry.ports.end() ? nullptr : &*found;
}

const ResourceNodeDocument* resource_for_node(const PipelineDocument& document, std::string_view node_id) {
    const auto found = std::find_if(document.resources.begin(), document.resources.end(),
                                    [&](const auto& resource) { return resource.node_id == node_id; });
    return found == document.resources.end() ? nullptr : &*found;
}

const PortDescriptor* resource_port_for(const PipelineDocument& document, std::string_view node_id,
                                        uint64_t port_id, PortDirection direction, PortDescriptor& scratch) {
    const auto* resource = resource_for_node(document, node_id);
    if (resource == nullptr || port_id != resource->resource_id.value())
        return nullptr;
    scratch.id = port_id;
    scratch.name = direction == PortDirection::kInput ? "Resource" : "Result";
    scratch.direction = direction;
    scratch.value_kind = resource->resource_constraints.resource_class == ResourceClass::kBuffer
                             ? PortValueKind::kBuffer
                             : PortValueKind::kTexture;
    scratch.schema_id = resource->type.value();
    scratch.schema_version = resource->version;
    scratch.required = direction == PortDirection::kInput && !resource->external_input;
    scratch.min_connections = direction == PortDirection::kInput && !resource->external_input ? 1u : 0u;
    scratch.max_connections = 1;
    scratch.binding_id = resource->resource_id.value();
    scratch.resource_constraints = resource->resource_constraints;
    if (direction == PortDirection::kInput)
        scratch.input = InputPortContract{.allowed_sources = input_source_mask(InputSourceKind::kEdge) |
                                                               input_source_mask(InputSourceKind::kAsset),
                                          .presence = resource->external_input ? InputPresenceRequirement::kOptional
                                                                                : InputPresenceRequirement::kRequired,
                                          .min_edges = 0,
                                          .max_edges = 1};
    else
        scratch.input.reset();
    return &scratch;
}

const VariadicInputDescriptor* group_for(const TypeCatalogEntry& entry, uint64_t group_id) {
    const auto found = std::find_if(entry.variadic_inputs.begin(), entry.variadic_inputs.end(),
                                    [group_id](const auto& group) { return group.id == group_id; });
    return found == entry.variadic_inputs.end() ? nullptr : &*found;
}

bool declared_slot(const PipelineDocument& document, std::string_view node_id, uint64_t group_id, uint64_t slot_id) {
    return slot_id != 0 && std::any_of(document.variadic_slots.begin(), document.variadic_slots.end(),
                                       [&](const auto& slot) {
                                           return slot.node_id == node_id && slot.group_id == group_id &&
                                                  slot.slot_id == slot_id;
                                       });
}

DocumentEdgeKind edge_kind_for(const PortDescriptor& port) {
    switch (port.value_kind) {
        case PortValueKind::kArtifact: return DocumentEdgeKind::kArtifact;
        case PortValueKind::kRenderState: return DocumentEdgeKind::kRenderState;
        default: return DocumentEdgeKind::kRenderResource;
    }
}

bool resource_schemas_compatible(const PortDescriptor& source, const PortDescriptor& destination) {
    if (source.resource_constraints.resource_class != destination.resource_constraints.resource_class)
        return false;
    if (source.resource_constraints.sample_count != 0 && destination.resource_constraints.sample_count != 0 &&
        source.resource_constraints.sample_count != destination.resource_constraints.sample_count)
        return false;
    if (source.resource_constraints.formats.empty() || destination.resource_constraints.formats.empty())
        return true;
    return std::any_of(source.resource_constraints.formats.begin(), source.resource_constraints.formats.end(),
                       [&](const auto& format) {
                           return std::find(destination.resource_constraints.formats.begin(),
                                             destination.resource_constraints.formats.end(), format) !=
                                  destination.resource_constraints.formats.end();
                       });
}

bool reachable(const PipelineDocument& document, std::string_view start, std::string_view target) {
    std::vector<std::string> pending{std::string{start}};
    std::unordered_set<std::string> visited;
    while (!pending.empty()) {
        std::string current = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(current).second)
            continue;
        if (current == target)
            return true;
        for (const auto& edge : document.edges)
            if (edge.kind != DocumentEdgeKind::kOrdering && edge.source.node_id == current)
                pending.push_back(edge.destination.node_id);
    }
    return false;
}

PipelineScope first_scope(PipelineScopeMask mask) {
    for (uint8_t value = 0; value <= static_cast<uint8_t>(PipelineScope::kEditor); ++value)
        if (mask & (PipelineScopeMask{1u} << value))
            return static_cast<PipelineScope>(value);
    return PipelineScope::kFrame;
}

const FeatureNodeDocument* find_feature(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.feature_nodes.begin(), document.feature_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.feature_nodes.end() ? nullptr : &*found;
}

const FeatureAuthoringDeclaration* feature_declaration_for_pass(const PipelineDocument& document,
                                                                const TypeCatalog& catalog,
                                                                std::string_view pass_node_id) {
    const auto pass = std::find_if(document.pass_nodes.begin(), document.pass_nodes.end(),
                                   [&](const auto& node) { return node.node_id == pass_node_id; });
    if (pass == document.pass_nodes.end())
        return nullptr;
    const auto* feature = find_feature(document, pass->feature_node);
    if (feature == nullptr)
        return nullptr;
    const auto* entry = find_type(catalog, {AuthoringTypeKind::kFeature, feature->type.value()});
    return entry == nullptr ? nullptr : std::get_if<FeatureAuthoringDeclaration>(&entry->declaration);
}

bool fixed_pass_topology_for_node(const PipelineDocument& document, const TypeCatalog& catalog,
                                  std::string_view node_id) {
    const auto* declaration = feature_declaration_for_pass(document, catalog, node_id);
    return declaration != nullptr && !declaration->allow_add_passes;
}

bool fixed_pass_topology_for_edge(const PipelineDocument& document, const TypeCatalog& catalog,
                                  const DocumentEdge& edge) {
    return fixed_pass_topology_for_node(document, catalog, edge.source.node_id) ||
           fixed_pass_topology_for_node(document, catalog, edge.destination.node_id);
}

const PipelineNodeDocument* find_pipeline(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.pipeline_nodes.end() ? nullptr : &*found;
}

foundation::Result materialize_feature_defaults(PipelineDocument& next, const TypeCatalog& catalog,
                                                const FeatureNodeDocument& feature, PipelineScope scope) {
    const auto* entry = catalog.find({AuthoringTypeKind::kFeature, feature.type.value()});
    const auto* declaration =
        entry == nullptr ? nullptr : std::get_if<FeatureAuthoringDeclaration>(&entry->declaration);
    if (declaration == nullptr) {
        return foundation::invalid_argument("Feature declaration is unavailable");
    }
    std::unordered_map<std::string, std::string> local_ids;
    for (const auto& pass : declaration->default_passes) {
        const std::string id = feature.node_id + "/" + pass.node_id;
        if (has_node(next, id)) {
            return foundation::invalid_state("default Pass node ID is already in use");
        }
        local_ids.emplace(pass.node_id, id);
        next.pass_nodes.push_back(
            {id, pass.type, pass.version, feature.node_id, pass.part_id, pass.parameters, pass.enabled});
    }
    for (const auto& resource : declaration->default_resources) {
        const std::string id = feature.node_id + "/" + resource.node_id;
        if (has_node(next, id)) {
            return foundation::invalid_state("default resource node ID is already in use");
        }
        local_ids.emplace(resource.node_id, id);
        const bool produced = std::any_of(
            declaration->default_edges.begin(), declaration->default_edges.end(), [&](const DocumentEdge& edge) {
                return edge.kind == DocumentEdgeKind::kRenderResource && edge.destination.node_id == resource.node_id;
            });
        next.resources.push_back({.node_id = id,
                                  .resource_id = resource.resource_id,
                                  .type = ArtifactTypeId(resource.constraints.resource_class == ResourceClass::kBuffer
                                                             ? "Pipeline.DefaultBuffer"
                                                             : "Pipeline.DefaultTexture"),
                                  .owner_scope = scope,
                                  .visible_to = to_mask(scope),
                                  .external_input = !produced,
                                  .storage = ArtifactStorage::kGpuLogicalResource,
                                  .lifetime = ArtifactLifetime::kFrame,
                                  .ownership = OwnershipPolicy::kGraphOwned,
                                  .resource_constraints = resource.constraints});
    }
    for (const auto& edge : declaration->default_edges) {
        const auto source = local_ids.find(edge.source.node_id);
        const auto destination = local_ids.find(edge.destination.node_id);
        if (source == local_ids.end() || destination == local_ids.end()) {
            return foundation::invalid_argument("default edge references an unknown node");
        }
        DocumentEdge materialized = edge;
        materialized.edge_id = feature.node_id + "/" + edge.edge_id;
        materialized.source.node_id = source->second;
        materialized.destination.node_id = destination->second;
        next.edges.push_back(std::move(materialized));
    }
    return foundation::Result::success();
}

}  // namespace

foundation::Result EditorCompositionCommands::add_type(const EditorGraphAddress& address, AuthoringTypeKey key,
                                                       std::string node_id, std::string attachment_id,
                                                       std::string empty_draft_marker) {
    pipeline::TypeCatalog catalog;
    for (const auto& entry : document_.catalog().entries())
        static_cast<void>(catalog.add(entry));
    return add_type_with_catalog(address, key, std::move(node_id), std::move(catalog),
                                 std::move(attachment_id), std::move(empty_draft_marker));
}

foundation::Result EditorCompositionCommands::add_type_with_catalog(
    const EditorGraphAddress& address, AuthoringTypeKey key, std::string node_id,
    pipeline::TypeCatalog next_catalog, std::string attachment_id, std::string empty_draft_marker) {
    if (node_id.empty())
        return foundation::invalid_argument("authored node ID is empty");
    pipeline::TypeCatalog catalog_for_mutation = next_catalog;
    const auto* entry = find_type(catalog_for_mutation, key);
    if (!entry)
        return foundation::not_found("authoring type is unavailable");
    return document_.apply_contract_transaction("Add " + entry->display_name, std::move(next_catalog),
                                                [&](PipelineDocument& next) {
        if (has_node(next, node_id))
            return foundation::invalid_state("authored node ID is already in use");
        if (address.kind == EditorGraphKind::kPipeline) {
            if (key.kind == AuthoringTypeKind::kPipeline) {
                if (!address.pipeline_node.empty())
                    return foundation::invalid_argument("Pipeline insertion address or type is invalid");
                PipelineNodeDocument node;
                node.node_id = node_id;
                node.type = PipelineId(entry->key.id);
                node.version = entry->version;
                node.scope = first_scope(entry->scopes);
                node.parameters = {};
                next.pipeline_nodes.push_back(std::move(node));
                if (const auto* pipeline_declaration = std::get_if<PipelineAuthoringDeclaration>(&entry->declaration);
                    pipeline_declaration != nullptr) {
                    for (const auto& default_feature : pipeline_declaration->default_features) {
                        const std::string feature_node_id = node_id + "/" + default_feature.node_id;
                        if (has_node(next, feature_node_id)) {
                            return foundation::invalid_state("default Feature node ID is already in use");
                        }
                        FeatureNodeDocument feature{feature_node_id,         default_feature.type,
                                                    default_feature.version, node_id,
                                                    default_feature.point,   true,
                                                    default_feature.parameters};
                        next.feature_nodes.push_back(feature);
                        if (const foundation::Result result = materialize_feature_defaults(
                                next, catalog_for_mutation, feature, next.pipeline_nodes.back().scope);
                            !result) {
                            return result;
                        }
                    }
                }
            } else {
                // A Pipeline graph is intentionally a single-layer view. A
                // Feature must be authored from its owning Feature graph so
                // the created instance is never invisible in the root canvas.
                return foundation::invalid_argument("Pipeline graph accepts Pipeline nodes only");
            }
        } else if (address.kind == EditorGraphKind::kFeature) {
            if (key.kind != AuthoringTypeKind::kFeature || !find_pipeline(next, address.pipeline_node))
                return foundation::invalid_argument("Feature insertion address or type is invalid");
            if (attachment_id.empty())
                return foundation::invalid_argument("Feature insertion requires a FeaturePoint");
            const auto* pipeline_node = find_pipeline(next, address.pipeline_node);
            const auto* pipeline_entry =
                find_type(catalog_for_mutation,
                          {AuthoringTypeKind::kPipeline, pipeline_node->type.value()});
            const auto* pipeline_decl =
                pipeline_entry ? std::get_if<PipelineAuthoringDeclaration>(&pipeline_entry->declaration) : nullptr;
            uint64_t numeric_attachment = 0;
            const auto parsed_attachment =
                std::from_chars(attachment_id.data(), attachment_id.data() + attachment_id.size(), numeric_attachment);
            const auto point =
                pipeline_decl
                    ? std::find_if(pipeline_decl->feature_points.begin(), pipeline_decl->feature_points.end(),
                                   [&](const auto& value) {
                                       return value.id == FeaturePointId(attachment_id) ||
                                              (parsed_attachment.ec == std::errc{} &&
                                               parsed_attachment.ptr == attachment_id.data() + attachment_id.size() &&
                                               value.id.value() == numeric_attachment);
                                   })
                    : std::vector<AuthoringFeaturePoint>::const_iterator{};
            if (!pipeline_decl || point == pipeline_decl->feature_points.end() ||
                (entry->domains & point->domains) == 0 || (entry->scopes & point->scopes) == 0)
                return foundation::invalid_argument("FeaturePoint is not compatible with the Feature");
            FeatureNodeDocument node;
            node.node_id = node_id;
            node.type = FeatureId(entry->key.id);
            node.version = entry->version;
            node.pipeline_node = address.pipeline_node;
            // The palette may display a numeric stable ID. Preserve the
            // catalog's actual ID rather than hashing its decimal spelling.
            node.point = point->id;
            next.feature_nodes.push_back(std::move(node));
            if (const foundation::Result result = materialize_feature_defaults(
                    next, catalog_for_mutation, next.feature_nodes.back(), pipeline_node->scope);
                !result) {
                return result;
            }
        } else {
            if (key.kind != AuthoringTypeKind::kRenderPass || address.feature_node.empty())
                return foundation::invalid_argument("Pass insertion address or type is invalid");
            const auto* owner = find_feature(next, address.feature_node);
            if (!owner)
                return foundation::invalid_argument("Pass owner or part is invalid");
            const auto* owner_entry =
                find_type(catalog_for_mutation, {AuthoringTypeKind::kFeature, owner->type.value()});
            const auto* declaration =
                owner_entry ? std::get_if<FeatureAuthoringDeclaration>(&owner_entry->declaration) : nullptr;
            if (!declaration || !declaration->allow_add_passes ||
                std::find(declaration->allowed_pass_types.begin(), declaration->allowed_pass_types.end(),
                          RenderPassTypeId(key.id)) == declaration->allowed_pass_types.end())
                return foundation::invalid_argument("Pass type is not allowed by the Feature");
            std::string assigned_part = address.part_id;
            if (!assigned_part.empty()) {
                const auto part = std::find_if(declaration->parts.begin(), declaration->parts.end(),
                                               [&](const auto& value) { return value.name == assigned_part; });
                if (part == declaration->parts.end() || part->domain != FeatureDomain::kRender)
                    return foundation::invalid_argument("Pass part is not a declared render part");
            } else if (!declaration->parts.empty()) {
                // A Feature with exactly one render part has an unambiguous
                // target even when the aggregate Pass graph was entered by
                // double-click. Multiple parts must be selected explicitly
                // in the graph selector to avoid silently assigning a pass
                // to the wrong execution lane.
                const auto render_part = std::find_if(declaration->parts.begin(), declaration->parts.end(),
                                                      [](const auto& value) {
                                                          return value.domain == FeatureDomain::kRender;
                                                      });
                if (render_part == declaration->parts.end() ||
                    std::find_if(std::next(render_part), declaration->parts.end(), [](const auto& value) {
                        return value.domain == FeatureDomain::kRender;
                    }) != declaration->parts.end())
                    return foundation::invalid_argument("Pass insertion requires an explicit render part");
                assigned_part = render_part->name;
            }
            PassNodeDocument node;
            node.node_id = node_id;
            node.type = RenderPassTypeId(entry->key.id);
            node.version = entry->version;
            node.feature_node = address.feature_node;
            node.part_id = std::move(assigned_part);
            next.pass_nodes.push_back(std::move(node));
        }
        // Keep the persisted empty-type marker in the same history entry as
        // the node instance. A single Undo must remove the complete create
        // operation, rather than leaving an orphan draft marker or node.
        if (!empty_draft_marker.empty())
            next.extensions.push_back({"@flowgraph:empty-type", OpaqueAuthoringEncoding::kToml,
                                       std::move(empty_draft_marker)});
        return foundation::Result::success();
                                                });
}

foundation::Result EditorCompositionCommands::erase_node(std::string_view node_id) {
    if (node_id.empty())
        return foundation::invalid_argument("authored node ID is empty");
    return document_.execute("Delete authored node", [&](PipelineDocument& next) {
        std::unordered_set<std::string> removed;
        const auto root_exists = has_node(next, node_id);
        if (!root_exists)
            return foundation::not_found("authored node does not exist");
        if (fixed_pass_topology_for_node(next, document_.catalog(), node_id))
            return foundation::invalid_argument("Pass topology is fixed by the Feature contract");
        removed.insert(std::string{node_id});
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& feature : next.feature_nodes)
                if (removed.contains(feature.pipeline_node) && removed.insert(feature.node_id).second)
                    changed = true;
            for (const auto& pass : next.pass_nodes)
                if (removed.contains(pass.feature_node) && removed.insert(pass.node_id).second)
                    changed = true;
            // Resources created from a node's default declaration use the
            // owning node as a stable prefix. Keep user-named external assets.
            for (const auto& resource : next.resources) {
                const auto separator = resource.node_id.find('/');
                if (separator != std::string::npos && removed.contains(resource.node_id.substr(0, separator)) &&
                    removed.insert(resource.node_id).second)
                    changed = true;
            }
        }
        next.pipeline_nodes.erase(std::remove_if(next.pipeline_nodes.begin(), next.pipeline_nodes.end(),
                                                  [&](const auto& node) { return removed.contains(node.node_id); }),
                                  next.pipeline_nodes.end());
        next.feature_nodes.erase(std::remove_if(next.feature_nodes.begin(), next.feature_nodes.end(),
                                                [&](const auto& node) { return removed.contains(node.node_id); }),
                                 next.feature_nodes.end());
        next.pass_nodes.erase(std::remove_if(next.pass_nodes.begin(), next.pass_nodes.end(),
                                              [&](const auto& node) { return removed.contains(node.node_id); }),
                              next.pass_nodes.end());
        next.resources.erase(std::remove_if(next.resources.begin(), next.resources.end(),
                                            [&](const auto& node) { return removed.contains(node.node_id); }),
                             next.resources.end());
        std::unordered_set<std::string> removed_edges;
        next.edges.erase(std::remove_if(next.edges.begin(), next.edges.end(), [&](const auto& edge) {
            const bool erase = removed.contains(edge.source.node_id) || removed.contains(edge.destination.node_id);
            if (erase)
                removed_edges.insert(edge.edge_id);
            return erase;
        }), next.edges.end());
        for (auto binding = next.input_bindings.begin(); binding != next.input_bindings.end();) {
            if (removed.contains(binding->node_id)) {
                binding = next.input_bindings.erase(binding);
                continue;
            }
            binding->edge_ids.erase(
                std::remove_if(binding->edge_ids.begin(), binding->edge_ids.end(),
                               [&](const auto& id) { return removed_edges.contains(id); }),
                binding->edge_ids.end());
            ++binding;
        }
        next.variadic_slots.erase(std::remove_if(next.variadic_slots.begin(), next.variadic_slots.end(),
                                                 [&](const auto& slot) { return removed.contains(slot.node_id); }),
                                  next.variadic_slots.end());
        next.layouts.erase(std::remove_if(next.layouts.begin(), next.layouts.end(),
                                          [&](const auto& layout) { return removed.contains(layout.node_id); }),
                           next.layouts.end());
        next.extensions.erase(std::remove_if(next.extensions.begin(), next.extensions.end(), [&](const auto& extension) {
            if (extension.owner_node != "@flowgraph:empty-type" ||
                extension.payload.rfind("AEG_FLOWGRAPH_EMPTY 1 ", 0) != 0)
                return false;
            std::istringstream input(extension.payload);
            std::string signature;
            uint32_t version = 0;
            uint32_t kind = 0;
            std::string type_name;
            uint64_t owner = 0;
            std::string marker_node;
            if (!(input >> signature >> version >> kind >> std::quoted(type_name) >> owner) ||
                !(input >> std::quoted(marker_node)))
                return false;  // Legacy markers had no node identity.
            return removed.contains(marker_node);
        }), next.extensions.end());
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::move_pass(std::string_view node_id, int direction) {
    if (node_id.empty() || (direction != -1 && direction != 1))
        return foundation::invalid_argument("Pass move request is invalid");
    return document_.execute("Reorder authored Pass", [&](PipelineDocument& next) {
        auto current = std::find_if(next.pass_nodes.begin(), next.pass_nodes.end(),
                                    [&](const auto& pass) { return pass.node_id == node_id; });
        if (current == next.pass_nodes.end())
            return foundation::not_found("Pass node does not exist");
        const auto* declaration = feature_declaration_for_pass(next, document_.catalog(), node_id);
        if (declaration == nullptr || !declaration->allow_add_passes)
            return foundation::invalid_argument("Pass order is fixed by the Feature contract");

        const auto sibling = [&](const PassNodeDocument& pass) {
            return pass.feature_node == current->feature_node && pass.part_id == current->part_id;
        };
        auto adjacent = next.pass_nodes.end();
        if (direction < 0) {
            for (auto candidate = current; candidate != next.pass_nodes.begin();) {
                --candidate;
                if (sibling(*candidate)) {
                    adjacent = candidate;
                    break;
                }
            }
        } else {
            for (auto candidate = std::next(current); candidate != next.pass_nodes.end(); ++candidate) {
                if (sibling(*candidate)) {
                    adjacent = candidate;
                    break;
                }
            }
        }
        if (adjacent == next.pass_nodes.end())
            return foundation::invalid_state("Pass has no sibling in that direction");
        const bool constrained_by_data = direction < 0
                                             ? reachable(next, adjacent->node_id, current->node_id)
                                             : reachable(next, current->node_id, adjacent->node_id);
        if (constrained_by_data)
            return foundation::invalid_argument("Disconnect or rewire the data flow before moving this Pass");

        // Order-only edges came from old templates and are invisible on the
        // data-flow canvas. A deliberate reorder replaces that legacy chain;
        // visible data edges remain and continue to constrain execution.
        const std::string owner = current->feature_node;
        next.edges.erase(std::remove_if(next.edges.begin(), next.edges.end(), [&](const DocumentEdge& edge) {
            if (edge.kind != DocumentEdgeKind::kOrdering)
                return false;
            const auto source = std::find_if(next.pass_nodes.begin(), next.pass_nodes.end(),
                                             [&](const auto& pass) { return pass.node_id == edge.source.node_id; });
            const auto destination = std::find_if(next.pass_nodes.begin(), next.pass_nodes.end(),
                                                  [&](const auto& pass) { return pass.node_id == edge.destination.node_id; });
            return source != next.pass_nodes.end() && destination != next.pass_nodes.end() &&
                   source->feature_node == owner && destination->feature_node == owner;
        }), next.edges.end());
        const auto current_layout = std::find_if(next.layouts.begin(), next.layouts.end(),
                                                 [&](const auto& layout) { return layout.node_id == current->node_id; });
        const auto adjacent_layout = std::find_if(next.layouts.begin(), next.layouts.end(),
                                                  [&](const auto& layout) { return layout.node_id == adjacent->node_id; });
        if (current_layout != next.layouts.end() && adjacent_layout != next.layouts.end()) {
            std::swap(current_layout->x, adjacent_layout->x);
            std::swap(current_layout->y, adjacent_layout->y);
        }
        // Swap the two sibling slots in the document sequence. Other
        // Features may be interleaved in that sequence and must retain their
        // relative positions.
        std::iter_swap(current, adjacent);
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::connect(const EditorConnectionRequest& request) {
    if (request.edge_id.empty() || request.source_node.empty() || request.destination_node.empty() ||
        !request.source_port || !request.destination_port || !request.binding_id)
        return foundation::invalid_argument("connection request is incomplete");
    return document_.execute("Connect authored ports", [&](PipelineDocument& next) {
        if (!has_node(next, request.source_node) || !has_node(next, request.destination_node))
            return foundation::not_found("connection endpoint does not exist");
        if (fixed_pass_topology_for_node(next, document_.catalog(), request.source_node) ||
            fixed_pass_topology_for_node(next, document_.catalog(), request.destination_node))
            return foundation::invalid_argument("Pass topology is fixed by the Feature contract");
        if (std::any_of(next.edges.begin(), next.edges.end(),
                        [&](const auto& edge) { return edge.edge_id == request.edge_id; }))
            return foundation::invalid_state("connection edge ID is already in use");
        const auto* source_entry = entry_for_node(next, document_.catalog(), request.source_node);
        const auto* destination_entry = entry_for_node(next, document_.catalog(), request.destination_node);
        PortDescriptor source_resource_port{};
        PortDescriptor destination_resource_port{};
        const auto* source_port = source_entry == nullptr
                                      ? resource_port_for(next, request.source_node, request.source_port,
                                                          PortDirection::kOutput, source_resource_port)
                                      : port_for(*source_entry, request.source_port, PortDirection::kOutput);
        const auto* destination_port = destination_entry == nullptr
                                           ? resource_port_for(next, request.destination_node, request.destination_port,
                                                               PortDirection::kInput, destination_resource_port)
                                           : port_for(*destination_entry, request.destination_port,
                                                      PortDirection::kInput);
        const bool resource_edge = request.kind == DocumentEdgeKind::kRenderResource;
        const bool value_kind_compatible = source_port != nullptr && destination_port != nullptr &&
                                           (source_port->value_kind == destination_port->value_kind ||
                                            (resource_edge &&
                                             (source_port->value_kind == PortValueKind::kRenderResource ||
                                              source_port->value_kind == PortValueKind::kTexture ||
                                              source_port->value_kind == PortValueKind::kBuffer) &&
                                             (destination_port->value_kind == PortValueKind::kRenderResource ||
                                              destination_port->value_kind == PortValueKind::kTexture ||
                                              destination_port->value_kind == PortValueKind::kBuffer)));
        const bool schema_compatible = resource_edge ||
                                       (source_port != nullptr && destination_port != nullptr &&
                                        source_port->schema_id == destination_port->schema_id &&
                                        source_port->schema_version == destination_port->schema_version);
        if (source_port == nullptr || destination_port == nullptr || source_port->binding_id == 0 ||
            source_port->binding_id != destination_port->binding_id || !value_kind_compatible ||
            !schema_compatible || (resource_edge && !resource_schemas_compatible(*source_port, *destination_port)) ||
            edge_kind_for(*source_port) != request.kind || edge_kind_for(*destination_port) != request.kind)
            return foundation::invalid_argument("source and destination ports are incompatible");
        const uint64_t destination_group = destination_port->input ? destination_port->input->variadic_group_id : 0;
        if ((destination_group == 0 && request.destination_slot != 0) ||
            (destination_group != 0 && !declared_slot(next, request.destination_node, destination_group,
                                                        request.destination_slot)))
            return foundation::invalid_argument("destination SlotId is not declared by the input group");
        if (request.source_slot != 0)
            return foundation::invalid_argument("output ports do not accept a SlotId");
        if (reachable(next, request.destination_node, request.source_node))
            return foundation::invalid_state("connection would introduce a data-flow cycle");
        const uint32_t existing_edges = static_cast<uint32_t>(std::count_if(
            next.edges.begin(), next.edges.end(), [&](const auto& edge) {
                return edge.destination.node_id == request.destination_node &&
                       edge.destination.port_id == request.destination_port &&
                       edge.destination.slot_id == request.destination_slot;
            }));
        const uint32_t max_edges = destination_port->input ? destination_port->input->max_edges : destination_port->max_connections;
        if (max_edges != 0 && existing_edges >= max_edges)
            return foundation::invalid_state("destination input already has its maximum number of data sources");
        next.edges.push_back({request.edge_id,
                              request.kind,
                              {request.source_node, request.source_port, request.source_slot},
                              {request.destination_node, request.destination_port, request.destination_slot},
                              request.binding_id});
        if (destination_port->input) {
                auto binding = std::find_if(next.input_bindings.begin(), next.input_bindings.end(),
                                            [&](const auto& candidate) {
                                                return candidate.node_id == request.destination_node &&
                                                       candidate.port_id == request.destination_port &&
                                                       candidate.slot_id == request.destination_slot;
                                            });
                if (binding == next.input_bindings.end()) {
                    next.input_bindings.push_back({.node_id = request.destination_node,
                                                   .port_id = request.destination_port,
                                                   .slot_id = request.destination_slot,
                                                   .source = InputSourceKind::kEdge,
                                                   .edge_ids = {request.edge_id},
                                                   .status = InputBindingStatus::kBound});
                } else {
                    if (binding->source != InputSourceKind::kEdge)
                        return foundation::invalid_state("connected input already uses a non-edge source");
                    binding->edge_ids.push_back(request.edge_id);
                    binding->status = InputBindingStatus::kBound;
                }
            }
        // Editing operates on recoverable drafts. Full graph validation belongs
        // to Compile/Preview; an unrelated unfinished node must not block a
        // valid local connection transaction.
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::disconnect(std::string_view edge_id) {
    return document_.execute("Disconnect authored ports", [&](PipelineDocument& next) {
        const auto found = std::find_if(next.edges.begin(), next.edges.end(),
                                        [&](const auto& edge) { return edge.edge_id == edge_id; });
        if (found == next.edges.end())
            return foundation::not_found("connection edge does not exist");
        if (fixed_pass_topology_for_edge(next, document_.catalog(), *found))
            return foundation::invalid_argument("Pass topology is fixed by the Feature contract");
        if (found->kind == DocumentEdgeKind::kOrdering)
            return foundation::invalid_argument("fixed execution ordering is owned by the node contract");
        next.edges.erase(found);
        for (auto binding = next.input_bindings.begin(); binding != next.input_bindings.end();) {
            binding->edge_ids.erase(std::remove(binding->edge_ids.begin(), binding->edge_ids.end(), edge_id),
                                    binding->edge_ids.end());
            if (binding->source == InputSourceKind::kEdge && binding->edge_ids.empty())
                binding = next.input_bindings.erase(binding);
            else
                ++binding;
        }
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::set_parameter(std::string_view node_id, uint64_t parameter_id,
                                                            ParameterValue value) {
    if (node_id.empty() || !parameter_id)
        return foundation::invalid_argument("parameter request is incomplete");
    return document_.execute("Set authored parameter", [&](PipelineDocument& next) {
        auto set = [&](auto& node, AuthoringTypeKind kind, uint64_t type_id) -> foundation::Result {
            const auto* entry = find_type(document_.catalog(), {kind, type_id});
            if (!entry)
                return foundation::not_found("parameter owner type is unavailable");
            ParameterBlock candidate = node.parameters;
            candidate[parameter_id] = value;
            if (const auto result = validate_parameter_block(candidate, entry->parameters); !result)
                return result;
            node.parameters = std::move(candidate);
            return foundation::Result::success();
        };
        for (auto& node : next.pipeline_nodes)
            if (node.node_id == node_id)
                return set(node, AuthoringTypeKind::kPipeline, node.type.value());
        for (auto& node : next.feature_nodes)
            if (node.node_id == node_id)
                return set(node, AuthoringTypeKind::kFeature, node.type.value());
        for (auto& node : next.pass_nodes)
            if (node.node_id == node_id)
                return set(node, AuthoringTypeKind::kRenderPass, node.type.value());
        return foundation::not_found("parameter owner node does not exist");
    });
}

foundation::Result EditorCompositionCommands::set_input_source(const InputBindingDocument& request) {
    if (request.node_id.empty() || request.port_id == 0)
        return foundation::invalid_argument("input source request is incomplete");
    return document_.execute("Set input source", [&](PipelineDocument& next) {
        const auto* entry = entry_for_node(next, document_.catalog(), request.node_id);
        PortDescriptor resource_input_port{};
        const auto* port = entry == nullptr
                               ? resource_port_for(next, request.node_id, request.port_id, PortDirection::kInput,
                                                   resource_input_port)
                               : port_for(*entry, request.port_id, PortDirection::kInput);
        if (port == nullptr || !port->input)
            return foundation::invalid_argument("input source targets a port without an explicit input contract");
        const uint64_t group_id = port->input->variadic_group_id;
        if ((group_id == 0 && request.slot_id != 0) ||
            (group_id != 0 && !declared_slot(next, request.node_id, group_id, request.slot_id)))
            return foundation::invalid_argument("input source targets an undeclared SlotId");
        if (request.source != InputSourceKind::kUnset && request.source != InputSourceKind::kNone &&
            (port->input->allowed_sources & input_source_mask(request.source)) == 0)
            return foundation::invalid_argument("input source is not allowed by the port contract");
        if (request.source == InputSourceKind::kLiteral) {
            if (!port->input->literal_schema ||
                !validate_parameter_block({{request.port_id, request.literal}},
                                          std::span(&*port->input->literal_schema, 1)))
                return foundation::invalid_argument("literal does not match the input schema");
        }
        if (request.source == InputSourceKind::kDefault &&
            (!port->input->default_value || request.default_id != port->input->default_value->id ||
             request.default_revision != port->input->default_value->revision))
            return foundation::invalid_argument("default source does not match the contract revision");
        if (request.source == InputSourceKind::kContext &&
            std::find(port->input->context_keys.begin(), port->input->context_keys.end(), request.context_key) ==
                port->input->context_keys.end())
            return foundation::invalid_argument("context key is not declared by the input contract");
        if (request.source == InputSourceKind::kEdge) {
            if (request.edge_ids.empty())
                return foundation::invalid_argument("Edge source requires an existing data edge");
            std::unordered_set<std::string> unique_edge_ids;
            for (const auto& edge_id : request.edge_ids)
                if (!unique_edge_ids.insert(edge_id).second)
                    return foundation::invalid_argument("input source contains duplicate edge");
            if (port->input->max_edges != 0 && request.edge_ids.size() > port->input->max_edges)
                return foundation::invalid_argument("input source exceeds max edges");
            for (const auto& edge_id : request.edge_ids) {
                const auto edge = std::find_if(next.edges.begin(), next.edges.end(),
                                               [&](const auto& value) { return value.edge_id == edge_id; });
                if (edge == next.edges.end() || edge->kind == DocumentEdgeKind::kOrdering ||
                    edge->destination.node_id != request.node_id || edge->destination.port_id != request.port_id ||
                    edge->destination.slot_id != request.slot_id)
                    return foundation::invalid_argument("Edge source contains an unrelated data edge");
            }
        }
        const auto binding = std::find_if(next.input_bindings.begin(), next.input_bindings.end(),
                                          [&](const auto& value) {
                                              return value.node_id == request.node_id && value.port_id == request.port_id &&
                                                     value.slot_id == request.slot_id;
                                          });
        // Changing to Asset/Literal/Default/Context/None explicitly removes the
        // previous incoming data edges. A source change is one undoable action.
        if (request.source != InputSourceKind::kEdge)
            next.edges.erase(std::remove_if(next.edges.begin(), next.edges.end(), [&](const auto& edge) {
                return edge.destination.node_id == request.node_id && edge.destination.port_id == request.port_id &&
                       edge.destination.slot_id == request.slot_id;
            }), next.edges.end());
        InputBindingDocument normalized{};
        normalized.node_id = request.node_id;
        normalized.port_id = request.port_id;
        normalized.slot_id = request.slot_id;
        normalized.source = request.source;
        normalized.status = request.status;
        if (normalized.source == InputSourceKind::kUnset)
            normalized.status = InputBindingStatus::kUnset;
        else if (normalized.status == InputBindingStatus::kUnset)
            normalized.status = InputBindingStatus::kPending;
        if (request.source == InputSourceKind::kEdge)
            normalized.edge_ids = request.edge_ids;
        else if (request.source == InputSourceKind::kAsset) {
            normalized.asset = request.asset;
            normalized.product_id = request.product_id;
            normalized.target_variant = request.target_variant;
            normalized.content_revision = request.content_revision;
        } else if (request.source == InputSourceKind::kDefault) {
            normalized.default_id = request.default_id;
            normalized.default_revision = request.default_revision;
        } else if (request.source == InputSourceKind::kLiteral) {
            normalized.literal = request.literal;
        } else if (request.source == InputSourceKind::kContext) {
            normalized.context_key = request.context_key;
        }
        if (binding == next.input_bindings.end()) {
            next.input_bindings.push_back(std::move(normalized));
        } else {
            *binding = std::move(normalized);
        }
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::add_variadic_slot(std::string_view node_id, uint64_t group_id) {
    if (node_id.empty() || group_id == 0)
        return foundation::invalid_argument("variadic slot request is incomplete");
    return document_.execute("Add variadic input slot", [&](PipelineDocument& next) {
        const TypeCatalogEntry* entry = nullptr;
        for (const auto& node : next.pipeline_nodes)
            if (node.node_id == node_id)
                entry = find_type(document_.catalog(), {AuthoringTypeKind::kPipeline, node.type.value()});
        for (const auto& node : next.feature_nodes)
            if (node.node_id == node_id)
                entry = find_type(document_.catalog(), {AuthoringTypeKind::kFeature, node.type.value()});
        for (const auto& node : next.pass_nodes)
            if (node.node_id == node_id)
                entry = find_type(document_.catalog(), {AuthoringTypeKind::kRenderPass, node.type.value()});
        if (entry == nullptr)
            return foundation::not_found("variadic slot owner node does not exist");
        const auto group = std::find_if(entry->variadic_inputs.begin(), entry->variadic_inputs.end(),
                                        [&](const auto& value) { return value.id == group_id; });
        if (group == entry->variadic_inputs.end())
            return foundation::not_found("variadic input group is not declared by the node type");
        const auto count = static_cast<uint32_t>(std::count_if(
            next.variadic_slots.begin(), next.variadic_slots.end(),
            [&](const auto& slot) { return slot.node_id == node_id && slot.group_id == group_id; }));
        if (count >= group->max_slots)
            return foundation::invalid_state("variadic input group has reached its maximum slot count");
        uint64_t slot_id = 1;
        for (const auto& slot : next.variadic_slots)
            slot_id = std::max(slot_id, slot.slot_id + 1);
        uint32_t order = 0;
        for (const auto& slot : next.variadic_slots)
            if (slot.node_id == node_id && slot.group_id == group_id)
                order = std::max(order, slot.order + 1);
        next.variadic_slots.push_back({std::string{node_id}, group_id, slot_id, order, {}});
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::remove_variadic_slot(std::string_view node_id, uint64_t group_id,
                                                                   uint64_t slot_id) {
    if (node_id.empty() || group_id == 0 || slot_id == 0)
        return foundation::invalid_argument("variadic slot removal request is incomplete");
    return document_.execute("Remove variadic input slot", [&](PipelineDocument& next) {
        const auto slot = std::find_if(next.variadic_slots.begin(), next.variadic_slots.end(), [&](const auto& value) {
            return value.node_id == node_id && value.group_id == group_id && value.slot_id == slot_id;
        });
        if (slot == next.variadic_slots.end())
            return foundation::not_found("variadic input slot does not exist");
        const TypeCatalogEntry* entry = nullptr;
        for (const auto& node : next.pipeline_nodes)
            if (node.node_id == node_id)
                entry = find_type(document_.catalog(), {AuthoringTypeKind::kPipeline, node.type.value()});
        for (const auto& node : next.feature_nodes)
            if (node.node_id == node_id)
                entry = find_type(document_.catalog(), {AuthoringTypeKind::kFeature, node.type.value()});
        for (const auto& node : next.pass_nodes)
            if (node.node_id == node_id)
                entry = find_type(document_.catalog(), {AuthoringTypeKind::kRenderPass, node.type.value()});
        if (entry == nullptr)
            return foundation::not_found("variadic slot owner node does not exist");
        const auto group = std::find_if(entry->variadic_inputs.begin(), entry->variadic_inputs.end(),
                                        [&](const auto& value) { return value.id == group_id; });
        if (group == entry->variadic_inputs.end())
            return foundation::not_found("variadic input group is not declared by the node type");
        const auto count = static_cast<uint32_t>(std::count_if(
            next.variadic_slots.begin(), next.variadic_slots.end(),
            [&](const auto& value) { return value.node_id == node_id && value.group_id == group_id; }));
        if (count <= group->min_slots)
            return foundation::invalid_state("variadic input group is at its minimum slot count");
        std::unordered_set<std::string> removed_edges;
        next.edges.erase(std::remove_if(next.edges.begin(), next.edges.end(), [&](const auto& edge) {
            const bool erase = edge.destination.node_id == node_id && edge.destination.slot_id == slot_id;
            if (erase)
                removed_edges.insert(edge.edge_id);
            return erase;
        }), next.edges.end());
        next.variadic_slots.erase(slot);
        next.input_bindings.erase(std::remove_if(next.input_bindings.begin(), next.input_bindings.end(),
                                                  [&](const auto& binding) {
                                                      if (binding.node_id != node_id || binding.slot_id != slot_id)
                                                          return false;
                                                      return true;
                                                  }),
                                   next.input_bindings.end());
        for (auto& binding : next.input_bindings)
            binding.edge_ids.erase(std::remove_if(binding.edge_ids.begin(), binding.edge_ids.end(),
                                                  [&](const auto& edge_id) { return removed_edges.contains(edge_id); }),
                                   binding.edge_ids.end());
        uint32_t order = 0;
        for (auto& remaining : next.variadic_slots)
            if (remaining.node_id == node_id && remaining.group_id == group_id)
                remaining.order = order++;
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::move_variadic_slot(std::string_view node_id, uint64_t group_id,
                                                                 uint64_t slot_id, int direction) {
    if (node_id.empty() || group_id == 0 || slot_id == 0 || (direction != -1 && direction != 1))
        return foundation::invalid_argument("variadic slot move request is incomplete");
    return document_.execute("Move variadic input slot", [&](PipelineDocument& next) {
        std::vector<std::reference_wrapper<VariadicInputSlotDocument>> ordered;
        for (auto& slot : next.variadic_slots)
            if (slot.node_id == node_id && slot.group_id == group_id)
                ordered.emplace_back(slot);
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return left.get().order < right.get().order;
        });
        const auto found = std::find_if(ordered.begin(), ordered.end(),
                                        [&](const auto& slot) { return slot.get().slot_id == slot_id; });
        if (found == ordered.end())
            return foundation::not_found("variadic input slot does not exist");
        const auto index = static_cast<std::ptrdiff_t>(std::distance(ordered.begin(), found));
        const auto other = index + direction;
        if (other < 0 || other >= static_cast<std::ptrdiff_t>(ordered.size()))
            return foundation::invalid_state("variadic input slot is already at the boundary");
        std::swap(ordered[static_cast<std::size_t>(index)].get().order,
                  ordered[static_cast<std::size_t>(other)].get().order);
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::duplicate_node(std::string_view node_id, std::string new_node_id) {
    if (node_id.empty() || new_node_id.empty())
        return foundation::invalid_argument("node duplication request is incomplete");
    return document_.execute("Duplicate authored node", [&](PipelineDocument& next) {
        if (has_node(next, new_node_id))
            return foundation::invalid_state("duplicate node ID is already in use");
        std::map<std::string, std::string> remap;
        const std::string old_prefix{node_id};
        const std::string new_prefix{new_node_id};
        const auto map_id = [&](std::string_view value) -> std::string {
            if (const auto mapped = remap.find(std::string{value}); mapped != remap.end())
                return mapped->second;
            if (value == old_prefix)
                return new_prefix;
            const std::string prefix = old_prefix + "/";
            if (value.rfind(prefix, 0) == 0) {
                std::string mapped{value};
                mapped.replace(0, old_prefix.size(), new_prefix);
                return mapped;
            }
            return {};
        };
        const auto child_id = [&](std::string_view value) {
            const std::string prefix = old_prefix + "/";
            if (value.rfind(prefix, 0) == 0) {
                std::string mapped{value};
                mapped.replace(0, old_prefix.size(), new_prefix);
                return mapped;
            }
            return new_prefix + "/" + std::string{value};
        };
        bool copied = false;
        for (const auto& node : next.pipeline_nodes) {
            if (node.node_id != node_id)
                continue;
            auto copy = node;
            copy.node_id = new_node_id;
            next.pipeline_nodes.push_back(std::move(copy));
            copied = true;
            break;
        }
        if (copied) {
            std::vector<FeatureNodeDocument> feature_copies;
            for (const auto& node : next.feature_nodes)
                if (node.pipeline_node == node_id || node.node_id.rfind(old_prefix + "/", 0) == 0) {
                    auto copy = node;
                    copy.node_id = child_id(node.node_id);
                    copy.pipeline_node = new_prefix;
                    remap.emplace(node.node_id, copy.node_id);
                    feature_copies.push_back(std::move(copy));
                }
            next.feature_nodes.insert(next.feature_nodes.end(), feature_copies.begin(), feature_copies.end());
        }
        if (!copied)
            for (const auto& node : next.feature_nodes) {
                if (node.node_id != node_id)
                    continue;
                auto copy = node;
                copy.node_id = new_node_id;
                next.feature_nodes.push_back(std::move(copy));
                copied = true;
                break;
            }
        if (copied && remap.empty()) {
            std::vector<PassNodeDocument> pass_copies;
            for (const auto& node : next.pass_nodes)
                if (node.feature_node == node_id) {
                    auto copy = node;
                    copy.node_id = child_id(node.node_id);
                    copy.feature_node = new_prefix;
                    remap.emplace(node.node_id, copy.node_id);
                    pass_copies.push_back(std::move(copy));
                }
            next.pass_nodes.insert(next.pass_nodes.end(), pass_copies.begin(), pass_copies.end());
        }
        else if (!copied)
            for (const auto& node : next.pass_nodes) {
                if (node.node_id != node_id)
                    continue;
                auto copy = node;
                copy.node_id = new_node_id;
                next.pass_nodes.push_back(std::move(copy));
                copied = true;
                break;
            }
        if (!copied)
            return foundation::not_found("authored node does not exist");
        // A pipeline copy has feature descendants; a feature copy has pass
        // descendants. Materialize resource descendants by the same stable
        // owner prefix and retain their logical resource identity.
        if (std::any_of(next.pipeline_nodes.begin(), next.pipeline_nodes.end(),
                        [&](const auto& value) { return value.node_id == new_node_id; })) {
            std::vector<PassNodeDocument> pass_copies;
            for (const auto& node : next.pass_nodes)
                if (remap.contains(node.feature_node) && !remap.contains(node.node_id)) {
                    auto copy = node;
                    copy.feature_node = remap.at(node.feature_node);
                    copy.node_id = copy.feature_node + "/" + node.node_id;
                    remap.emplace(node.node_id, copy.node_id);
                    pass_copies.push_back(std::move(copy));
                }
            next.pass_nodes.insert(next.pass_nodes.end(), pass_copies.begin(), pass_copies.end());
        }
        std::vector<ResourceNodeDocument> resource_copies;
        for (const auto& resource : next.resources) {
            const std::string mapped = map_id(resource.node_id);
            if (mapped.empty())
                continue;
            auto copy = resource;
            copy.node_id = mapped;
            resource_copies.push_back(std::move(copy));
            remap.emplace(resource.node_id, mapped);
        }
        next.resources.insert(next.resources.end(), resource_copies.begin(), resource_copies.end());
        // Root descendants may not all have been in remap for a direct
        // Feature copy; include the root itself before copying authoring data.
        remap.emplace(std::string{node_id}, new_node_id);
        std::vector<InputBindingDocument> binding_copies;
        for (const auto& binding : next.input_bindings) {
            const auto mapped = map_id(binding.node_id);
            if (mapped.empty())
                continue;
            auto copy = binding;
            copy.node_id = mapped;
            if (copy.source == InputSourceKind::kEdge) {
                copy.source = InputSourceKind::kUnset;
                copy.status = InputBindingStatus::kUnset;
                copy.edge_ids.clear();
            }
            binding_copies.push_back(std::move(copy));
        }
        next.input_bindings.insert(next.input_bindings.end(), binding_copies.begin(), binding_copies.end());
        std::vector<VariadicInputSlotDocument> slot_copies;
        for (const auto& slot : next.variadic_slots) {
            const auto mapped = map_id(slot.node_id);
            if (mapped.empty())
                continue;
            auto copy = slot;
            copy.node_id = mapped;
            slot_copies.push_back(std::move(copy));
        }
        next.variadic_slots.insert(next.variadic_slots.end(), slot_copies.begin(), slot_copies.end());
        std::vector<DocumentNodeLayout> layout_copies;
        for (const auto& layout : next.layouts) {
            const auto mapped = map_id(layout.node_id);
            if (mapped.empty())
                continue;
            auto copy = layout;
            copy.node_id = mapped;
            copy.x += 40.0;
            copy.y += 40.0;
            layout_copies.push_back(std::move(copy));
        }
        next.layouts.insert(next.layouts.end(), layout_copies.begin(), layout_copies.end());
        // Copy only edges wholly inside the source subtree. External input
        // edges are intentionally unset on the duplicate.
        std::vector<DocumentEdge> edge_copies;
        for (const auto& edge : next.edges) {
            const auto source = map_id(edge.source.node_id);
            const auto destination = map_id(edge.destination.node_id);
            if (source.empty() || destination.empty())
                continue;
            auto copy = edge;
            copy.edge_id = new_prefix + "/" + edge.edge_id;
            copy.source.node_id = source;
            copy.destination.node_id = destination;
            edge_copies.push_back(std::move(copy));
        }
        next.edges.insert(next.edges.end(), edge_copies.begin(), edge_copies.end());
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::begin_transaction(std::string label) {
    if (transaction_)
        return foundation::invalid_state("an editor composition transaction is already active");
    if (label.empty())
        return foundation::invalid_argument("editor composition transaction label is empty");
    transaction_ = Transaction{std::move(label), document_.snapshot(), *document_.snapshot()};
    return foundation::Result::success();
}

foundation::Result EditorCompositionCommands::update_transaction(
    std::function<foundation::Result(PipelineDocument&)> mutation) {
    if (!transaction_)
        return foundation::invalid_state("no editor composition transaction is active");
    if (!mutation)
        return foundation::invalid_argument("editor composition transaction mutation is empty");
    try {
        return mutation(transaction_->draft);
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::Result EditorCompositionCommands::commit_transaction() {
    if (!transaction_)
        return foundation::invalid_state("no editor composition transaction is active");
    if (document_.snapshot() != transaction_->before)
        return foundation::invalid_state("editor composition changed during transaction");
    // Transactions are authoring edits, not compilation gates. A designer
    // must be able to stage an incomplete graph (for example, add a required
    // input before deciding whether it comes from an Asset or an Edge) and
    // let the document diagnostics explain the unfinished state. Compile and
    // Apply are the explicit validation boundaries.
    const auto label = transaction_->label;
    auto draft = std::move(transaction_->draft);
    const auto result = document_.execute(label, [draft = std::move(draft)](PipelineDocument& next) mutable {
        next = std::move(draft);
        return foundation::Result::success();
    });
    if (result)
        transaction_.reset();
    return result;
}

foundation::Result EditorCompositionCommands::cancel_transaction() noexcept {
    if (!transaction_)
        return foundation::invalid_state("no editor composition transaction is active");
    transaction_.reset();
    return foundation::Result::success();
}

}  // namespace aeg::application
