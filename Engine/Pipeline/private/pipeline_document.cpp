#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "pipeline_authoring.hpp"

namespace aeg::pipeline {
namespace {

void diagnostic(std::vector<DocumentDiagnostic>& result, std::string code, std::string message, std::string node = {},
                std::string edge = {}, uint64_t port = 0, uint64_t parameter = 0) {
    result.push_back({std::move(code), std::move(message), std::move(node), std::move(edge), port, parameter});
}

[[nodiscard]] bool compatible_version(Version document, Version declared) noexcept {
    return document.is_valid() && declared.is_valid() && document.major == declared.major && document <= declared;
}

[[nodiscard]] const ParameterDescriptor* parameter_schema(const TypeCatalogEntry& entry, uint64_t id) {
    const auto found = std::find_if(entry.parameters.begin(), entry.parameters.end(),
                                    [id](const ParameterDescriptor& value) { return value.id == id; });
    return found == entry.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] const PortDescriptor* port_schema(const TypeCatalogEntry& entry, uint64_t id) {
    const auto found = std::find_if(entry.ports.begin(), entry.ports.end(),
                                    [id](const PortDescriptor& value) { return value.id == id; });
    return found == entry.ports.end() ? nullptr : &*found;
}

[[nodiscard]] bool edge_matches_port(DocumentEdgeKind kind, const PortDescriptor& port) noexcept {
    switch (kind) {
        case DocumentEdgeKind::kArtifact:
            return port.value_kind == PortValueKind::kArtifact;
        case DocumentEdgeKind::kRenderState:
            return port.value_kind == PortValueKind::kRenderState;
        case DocumentEdgeKind::kRenderResource:
            return port.value_kind == PortValueKind::kRenderResource || port.value_kind == PortValueKind::kBuffer ||
                   port.value_kind == PortValueKind::kTexture;
        case DocumentEdgeKind::kOrdering:
            return false;
    }
    return false;
}

[[nodiscard]] const TypeCatalogEntry* lookup(const TypeCatalog& catalog, AuthoringTypeKind kind, uint64_t id,
                                             std::vector<DocumentDiagnostic>& result, const std::string& node,
                                             const char* label) {
    const TypeCatalogEntry* entry = catalog.find({kind, id});
    if (entry == nullptr) {
        diagnostic(result, "type.missing", std::string(label) + " type is not registered", node);
    }
    return entry;
}

[[nodiscard]] bool has_scope(PipelineScopeMask scopes, PipelineScope scope) noexcept {
    return (scopes & to_mask(scope)) != 0;
}

[[nodiscard]] bool has_domain(FeatureDomainMask domains, FeatureDomainMask allowed) noexcept {
    return (domains & allowed) != 0;
}

[[nodiscard]] const AuthoringFeaturePoint* find_point(const PipelineAuthoringDeclaration& declaration,
                                                      FeaturePointId id) {
    const auto found = std::find_if(declaration.feature_points.begin(), declaration.feature_points.end(),
                                    [id](const AuthoringFeaturePoint& value) { return value.id == id; });
    return found == declaration.feature_points.end() ? nullptr : &*found;
}

[[nodiscard]] bool allowed_pass(const FeatureAuthoringDeclaration& declaration, RenderPassTypeId type) noexcept {
    return std::any_of(declaration.allowed_pass_types.begin(), declaration.allowed_pass_types.end(),
                       [type](RenderPassTypeId value) { return value == type; });
}

[[nodiscard]] bool has_part(const FeatureAuthoringDeclaration& declaration, std::string_view part) noexcept {
    return std::any_of(declaration.parts.begin(), declaration.parts.end(),
                       [part](const AuthoringPart& value) { return value.name == part; });
}

[[nodiscard]] const TypeCatalogEntry* entry_for_endpoint(
    const std::unordered_map<std::string, const TypeCatalogEntry*>& node_entries, const DocumentEndpoint& endpoint) {
    const auto found = node_entries.find(endpoint.node_id);
    return found == node_entries.end() ? nullptr : found->second;
}

void validate_parameters(const TypeCatalogEntry& entry, const ParameterBlock& values, const std::string& node,
                         std::vector<DocumentDiagnostic>& result) {
    if (const foundation::Result check = validate_parameter_block(values, entry.parameters); !check) {
        diagnostic(result, "parameter.invalid", check.message, node);
    }
    for (const auto& [id, ignored] : values) {
        if (parameter_schema(entry, id) == nullptr) {
            diagnostic(result, "parameter.unknown", "parameter is not declared by the type", node, {}, 0, id);
        }
        (void)ignored;
    }
}

}  // namespace

std::vector<DocumentDiagnostic> validate_pipeline_document(const PipelineDocument& document,
                                                           const TypeCatalog& catalog) {
    std::vector<DocumentDiagnostic> result;
    if (document.schema_version == 0) {
        diagnostic(result, "document.schema", "schema version is required");
    }
    if (document.asset_uuid.empty()) {
        diagnostic(result, "document.asset_uuid", "asset UUID is required");
    }
    if (document.name.empty()) {
        diagnostic(result, "document.name", "document name is required");
    }

    std::unordered_set<std::string> node_ids;
    std::unordered_map<std::string, const TypeCatalogEntry*> node_entries;
    std::unordered_map<std::string, const PipelineNodeDocument*> pipelines;
    std::unordered_map<std::string, const FeatureNodeDocument*> features;
    std::unordered_map<std::string, const PassNodeDocument*> passes;
    std::unordered_map<std::string, const ResourceNodeDocument*> resources;

    const auto claim_node = [&](const std::string& id, const char* kind) {
        if (id.empty()) {
            diagnostic(result, "node.id", std::string(kind) + " node ID is required");
            return false;
        }
        if (!node_ids.insert(id).second) {
            diagnostic(result, "node.duplicate", "duplicate node ID", id);
            return false;
        }
        return true;
    };

    for (const auto& node : document.pipeline_nodes) {
        if (!claim_node(node.node_id, "pipeline")) {
            continue;
        }
        pipelines.emplace(node.node_id, &node);
        const TypeCatalogEntry* entry =
            lookup(catalog, AuthoringTypeKind::kPipeline, node.type.value(), result, node.node_id, "pipeline");
        if (entry == nullptr) {
            continue;
        }
        node_entries.emplace(node.node_id, entry);
        if (!std::holds_alternative<PipelineAuthoringDeclaration>(entry->declaration)) {
            diagnostic(result, "type.kind", "catalog entry is not a Pipeline declaration", node.node_id);
        }
        if (!compatible_version(node.version, entry->version)) {
            diagnostic(result, "type.version", "pipeline node version is incompatible", node.node_id);
        }
        if (!has_scope(entry->scopes, node.scope)) {
            diagnostic(result, "scope.unsupported", "pipeline type does not support this scope", node.node_id);
        }
        validate_parameters(*entry, node.parameters, node.node_id, result);
    }

    for (const auto& node : document.feature_nodes) {
        if (!claim_node(node.node_id, "feature")) {
            continue;
        }
        features.emplace(node.node_id, &node);
        const TypeCatalogEntry* entry =
            lookup(catalog, AuthoringTypeKind::kFeature, node.type.value(), result, node.node_id, "feature");
        if (entry != nullptr) {
            node_entries.emplace(node.node_id, entry);
            if (!std::holds_alternative<FeatureAuthoringDeclaration>(entry->declaration)) {
                diagnostic(result, "type.kind", "catalog entry is not a Feature declaration", node.node_id);
            }
            if (!compatible_version(node.version, entry->version)) {
                diagnostic(result, "type.version", "feature node version is incompatible", node.node_id);
            }
            validate_parameters(*entry, node.parameters, node.node_id, result);
        }
        const auto parent = pipelines.find(node.pipeline_node);
        if (parent == pipelines.end()) {
            diagnostic(result, "parent.missing", "feature references an unknown Pipeline node", node.node_id);
            continue;
        }
        const auto pipeline_entry_it = node_entries.find(parent->first);
        if (pipeline_entry_it == node_entries.end()) {
            diagnostic(result, "type.missing", "parent Pipeline type is unavailable", node.node_id);
            continue;
        }
        const TypeCatalogEntry* pipeline_entry = pipeline_entry_it->second;
        const auto* pipeline_declaration = std::get_if<PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
        const AuthoringFeaturePoint* point =
            pipeline_declaration == nullptr ? nullptr : find_point(*pipeline_declaration, node.point);
        if (point == nullptr) {
            diagnostic(result, "feature_point.missing", "feature point is not declared by the Pipeline", node.node_id);
        } else {
            if (entry != nullptr && !has_domain(entry->domains, point->domains)) {
                diagnostic(result, "domain.incompatible", "Feature domain is not allowed at this FeaturePoint",
                           node.node_id);
            }
            if (entry != nullptr && !has_scope(entry->scopes, parent->second->scope)) {
                diagnostic(result, "scope.incompatible", "Feature type is not valid for the Pipeline scope",
                           node.node_id);
            }
            if (!has_scope(point->scopes, parent->second->scope)) {
                diagnostic(result, "scope.feature_point", "FeaturePoint does not allow the Pipeline scope",
                           node.node_id);
            }
        }
    }
    for (const auto& node : document.pass_nodes) {
        if (!claim_node(node.node_id, "pass")) {
            continue;
        }
        passes.emplace(node.node_id, &node);
        const TypeCatalogEntry* entry =
            lookup(catalog, AuthoringTypeKind::kRenderPass, node.type.value(), result, node.node_id, "render pass");
        if (entry != nullptr) {
            node_entries.emplace(node.node_id, entry);
            if (!std::holds_alternative<PassAuthoringDeclaration>(entry->declaration)) {
                diagnostic(result, "type.kind", "catalog entry is not a RenderPass declaration", node.node_id);
            }
            if (!compatible_version(node.version, entry->version)) {
                diagnostic(result, "type.version", "pass node version is incompatible", node.node_id);
            }
            validate_parameters(*entry, node.parameters, node.node_id, result);
        }
        const auto parent = features.find(node.feature_node);
        if (parent == features.end()) {
            diagnostic(result, "parent.missing", "pass references an unknown Feature node", node.node_id);
            continue;
        }
        const auto feature_entry_it = node_entries.find(parent->first);
        if (feature_entry_it == node_entries.end()) {
            diagnostic(result, "type.missing", "parent Feature type is unavailable", node.node_id);
            continue;
        }
        const TypeCatalogEntry* feature_entry = feature_entry_it->second;
        const auto* feature_declaration = std::get_if<FeatureAuthoringDeclaration>(&feature_entry->declaration);
        if (feature_declaration == nullptr) {
            diagnostic(result, "feature.declaration", "parent Feature has no render composition declaration",
                       node.node_id);
        } else {
            if (!allowed_pass(*feature_declaration, node.type)) {
                diagnostic(result, "pass.unauthorized", "RenderPass type is not allowed by the Feature", node.node_id);
            }
            if (!feature_declaration->allow_add_passes) {
                const bool is_declared_default = std::any_of(
                    feature_declaration->default_passes.begin(), feature_declaration->default_passes.end(),
                    [&](const DefaultPassNode& pass) {
                        return node.node_id == pass.node_id || node.node_id == node.feature_node + "/" + pass.node_id;
                    });
                if (!is_declared_default) {
                    diagnostic(result, "pass.additional", "Feature does not allow additional RenderPass nodes",
                               node.node_id);
                }
            }
            if (feature_declaration->parts.empty()) {
                if (!node.part_id.empty()) {
                    diagnostic(result, "part.missing", "RenderPass part is not declared by the Feature", node.node_id);
                }
            } else if (node.part_id.empty()) {
                diagnostic(result, "part.missing", "RenderPass requires a declared render part", node.node_id);
            } else if (!has_part(*feature_declaration, node.part_id)) {
                diagnostic(result, "part.missing", "RenderPass part is not declared by the Feature", node.node_id);
            } else {
                const auto part = std::find_if(feature_declaration->parts.begin(), feature_declaration->parts.end(),
                                               [&](const AuthoringPart& value) { return value.name == node.part_id; });
                if (part->domain != FeatureDomain::kRender) {
                    diagnostic(result, "part.domain", "RenderPass part must belong to the render domain", node.node_id);
                }
            }
        }
    }

    std::unordered_set<uint64_t> resource_ids;
    for (const auto& resource : document.resources) {
        if (!claim_node(resource.node_id, "resource")) {
            continue;
        }
        resources.emplace(resource.node_id, &resource);
        if (!resource.resource_id.is_valid() || !resource_ids.insert(resource.resource_id.value()).second) {
            diagnostic(result, "resource.duplicate", "resource identity is invalid or duplicated", resource.node_id);
        }
        if (!resource.type.is_valid() || resource.version == 0) {
            diagnostic(result, "resource.schema", "resource type and version are required", resource.node_id);
        }
        if (!has_scope(resource.visible_to, resource.owner_scope)) {
            diagnostic(result, "resource.scope", "resource visibility must include its owner scope", resource.node_id);
        }
        if (!resource.resource_constraints.formats.empty() &&
            resource.resource_constraints.resource_class == ResourceClass::kNone) {
            diagnostic(result, "resource.constraints", "resource constraints require a resource class",
                       resource.node_id);
        }
    }

    std::unordered_set<std::string> edge_ids;
    std::unordered_map<std::string, uint32_t> incoming_connections;
    std::map<std::pair<DocumentEdgeKind, uint64_t>, std::string> producers;
    std::unordered_map<std::string, std::vector<std::string>> graph;
    for (const auto& edge : document.edges) {
        if (edge.edge_id.empty() || !edge_ids.insert(edge.edge_id).second) {
            diagnostic(result, "edge.duplicate", "edge ID is empty or duplicated", {}, edge.edge_id);
            continue;
        }
        if (!node_ids.contains(edge.source.node_id) || !node_ids.contains(edge.destination.node_id)) {
            diagnostic(result, "edge.node", "edge references an unknown node", {}, edge.edge_id);
            continue;
        }
        graph[edge.source.node_id].push_back(edge.destination.node_id);
        if (edge.kind == DocumentEdgeKind::kOrdering) {
            if (edge.binding_id != 0 || edge.source.port_id != 0 || edge.destination.port_id != 0 ||
                edge.source.node_id == edge.destination.node_id) {
                diagnostic(result, "edge.ordering", "ordering edges cannot carry ports or bindings", {}, edge.edge_id);
            }
            const auto source_pass = passes.find(edge.source.node_id);
            const auto destination_pass = passes.find(edge.destination.node_id);
            if (source_pass == passes.end() || destination_pass == passes.end()) {
                diagnostic(result, "edge.ordering.type", "ordering edges must connect two RenderPass nodes", {},
                           edge.edge_id);
            } else {
                if (source_pass->second->feature_node != destination_pass->second->feature_node) {
                    diagnostic(result, "edge.ordering.feature", "Pass ordering edges must stay within one Feature", {},
                               edge.edge_id);
                } else if (source_pass->second->part_id != destination_pass->second->part_id) {
                    diagnostic(result, "edge.ordering.part", "Pass ordering edges must stay within one Render Part", {},
                               edge.edge_id);
                }
            }
            continue;
        }
        if (edge.binding_id == 0 || edge.source.port_id == 0 || edge.destination.port_id == 0) {
            diagnostic(result, "edge.binding", "data edges require stable ports and a binding", {}, edge.edge_id);
            continue;
        }
        const TypeCatalogEntry* source_entry = entry_for_endpoint(node_entries, edge.source);
        const TypeCatalogEntry* destination_entry = entry_for_endpoint(node_entries, edge.destination);
        PortDescriptor source_resource_port{}, destination_resource_port{};
        const auto resource_port = [&](const DocumentEndpoint& endpoint, PortDirection direction,
                                       PortDescriptor& port) -> const PortDescriptor* {
            const auto resource = resources.find(endpoint.node_id);
            if (resource == resources.end() || edge.kind != DocumentEdgeKind::kRenderResource ||
                endpoint.port_id != resource->second->resource_id.value())
                return nullptr;
            port = {.id = endpoint.port_id,
                    .name = endpoint.node_id,
                    .direction = direction,
                    .value_kind = PortValueKind::kRenderResource,
                    .schema_id = resource->second->type.value(),
                    .binding_id = resource->second->resource_id.value(),
                    .resource_constraints = resource->second->resource_constraints};
            return &port;
        };
        const PortDescriptor* source_port =
            source_entry == nullptr ? resource_port(edge.source, PortDirection::kOutput, source_resource_port)
                                    : port_schema(*source_entry, edge.source.port_id);
        const PortDescriptor* destination_port =
            destination_entry == nullptr
                ? resource_port(edge.destination, PortDirection::kInput, destination_resource_port)
                : port_schema(*destination_entry, edge.destination.port_id);
        if (source_port == nullptr || destination_port == nullptr) {
            diagnostic(result, "port.missing", "edge references an undeclared port", {}, edge.edge_id,
                       source_port == nullptr ? edge.source.port_id : edge.destination.port_id);
            continue;
        }
        if (source_port->direction != PortDirection::kOutput || destination_port->direction != PortDirection::kInput) {
            diagnostic(result, "port.direction", "data edge must connect output to input", {}, edge.edge_id);
        }
        if (!edge_matches_port(edge.kind, *source_port) || !edge_matches_port(edge.kind, *destination_port)) {
            diagnostic(result, "port.kind", "edge kind does not match both port schemas", {}, edge.edge_id);
        }
        if (source_port->binding_id != edge.binding_id || destination_port->binding_id != edge.binding_id) {
            diagnostic(result, "port.binding", "edge binding does not match declared port bindings", {}, edge.edge_id);
        }
        const std::string destination_key = edge.destination.node_id + ":" + std::to_string(edge.destination.port_id);
        if (++incoming_connections[destination_key] > destination_port->max_connections) {
            diagnostic(result, "port.multiplicity", "input port has too many connections", {}, edge.edge_id,
                       edge.destination.port_id);
        }
        if (!resources.contains(edge.source.node_id)) {
            const auto producer_key = std::make_pair(edge.kind, edge.binding_id);
            const auto [producer, inserted] = producers.emplace(producer_key, edge.source.node_id);
            if (!inserted && producer->second != edge.source.node_id) {
                diagnostic(result, "binding.producers", "binding has multiple producers", {}, edge.edge_id);
            }
        }
        if (edge.kind == DocumentEdgeKind::kRenderResource) {
            const auto& source = source_port->resource_constraints;
            const auto& destination = destination_port->resource_constraints;
            bool format_matches = source.formats.empty() || destination.formats.empty();
            for (const auto& format : source.formats)
                format_matches |= std::find(destination.formats.begin(), destination.formats.end(), format) !=
                                  destination.formats.end();
            if (source.resource_class != destination.resource_class || !format_matches ||
                (source.sample_count && destination.sample_count && source.sample_count != destination.sample_count))
                diagnostic(result, "resource.incompatible", "connected resource schemas are incompatible", {},
                           edge.edge_id);
        }
    }

    for (const auto& [node, entry] : node_entries) {
        if (const auto pass = passes.find(node); pass != passes.end() && !pass->second->enabled)
            continue;
        if (const auto feature = features.find(node); feature != features.end() && !feature->second->enabled)
            continue;
        for (const auto& port : entry->ports) {
            if (port.direction == PortDirection::kInput && port.required &&
                incoming_connections[node + ":" + std::to_string(port.id)] < port.min_connections)
                diagnostic(result, "port.required", "required input port is disconnected", node, {}, port.id);
        }
    }

    std::unordered_set<std::string> layout_ids;
    for (const auto& layout : document.layouts) {
        if (!node_ids.contains(layout.node_id)) {
            diagnostic(result, "layout.node", "layout references an unknown node", layout.node_id);
        }
        if (!layout_ids.insert(layout.node_id).second) {
            diagnostic(result, "layout.duplicate", "node layout is duplicated", layout.node_id);
        }
        if (!std::isfinite(layout.x) || !std::isfinite(layout.y)) {
            diagnostic(result, "layout.coordinate", "node layout coordinates must be finite", layout.node_id);
        }
    }

    std::unordered_map<std::string, uint8_t> marks;
    const auto visit = [&](const auto& self, const std::string& node) -> bool {
        auto& mark = marks[node];
        if (mark == 1) {
            return false;
        }
        if (mark == 2) {
            return true;
        }
        mark = 1;
        for (const auto& next : graph[node]) {
            if (!self(self, next)) {
                return false;
            }
        }
        mark = 2;
        return true;
    };
    for (const auto& node : node_ids) {
        if (marks[node] == 0 && !visit(visit, node)) {
            diagnostic(result, "graph.cycle", "authored Pipeline graph contains a cycle");
            break;
        }
    }

    bool requires_present = false;
    for (const auto& [node_id, entry] : node_entries) {
        static_cast<void>(node_id);
        if (entry == nullptr) {
            continue;
        }
        const auto* declaration = std::get_if<PipelineAuthoringDeclaration>(&entry->declaration);
        if (declaration == nullptr) {
            continue;
        }
        requires_present = std::any_of(declaration->feature_points.begin(), declaration->feature_points.end(),
                                       [](const AuthoringFeaturePoint& point) {
                                           return has_flag(point.flags, FeaturePointFlags::kMayWriteFinalOutput);
                                       });
        if (requires_present) {
            break;
        }
    }
    if (requires_present) {
        if (document.present_connections.empty()) {
            diagnostic(result, "present.missing", "rendering Pipeline requires a Present connection");
        }
        std::unordered_set<std::string> present_sources;
        for (const std::string& source_node : document.present_connections) {
            if (!present_sources.insert(source_node).second) {
                diagnostic(result, "present.duplicate", "Present connection is duplicated", source_node);
            }
            const auto pass = passes.find(source_node);
            const auto feature = features.find(source_node);
            if (pass == passes.end() && feature == features.end()) {
                diagnostic(result, "present.source", "Present connection references an unknown pass", source_node);
            } else if ((pass != passes.end() &&
                        [&] {
                            const auto parent = features.find(pass->second->feature_node);
                            return !pass->second->enabled || parent == features.end() || !parent->second->enabled;
                        }()) ||
                       (feature != features.end() && !feature->second->enabled)) {
                diagnostic(result, "present.disabled", "Present connection references a disabled source", source_node);
            }
        }
        for (const auto& [disabled_id, disabled] : passes) {
            if (disabled == nullptr || disabled->enabled) {
                continue;
            }
            std::vector<std::string> pending{disabled_id};
            std::unordered_set<std::string> visited;
            bool reaches_present = false;
            for (size_t index = 0; index < pending.size(); ++index) {
                const std::string& current = pending[index];
                if (!visited.insert(current).second) {
                    continue;
                }
                if (present_sources.contains(current)) {
                    reaches_present = true;
                    break;
                }
                const auto next = graph.find(current);
                if (next != graph.end()) {
                    pending.insert(pending.end(), next->second.begin(), next->second.end());
                }
            }
            if (reaches_present) {
                diagnostic(result, "pass.required_disabled", "a disabled pass lies on the authored Present route",
                           disabled_id);
            }
        }
    }
    return result;
}

std::string document_semantic_digest(const PipelineDocument& document) {
    std::ostringstream out;
    out << document.schema_version << '|' << document.asset_uuid << '|' << document.name;
    for (const auto& node : document.pipeline_nodes) {
        out << "|P:" << node.node_id << ':' << node.type.value() << ':' << static_cast<uint32_t>(node.scope);
    }
    for (const auto& node : document.feature_nodes) {
        out << "|F:" << node.node_id << ':' << node.type.value() << ':' << node.pipeline_node << ':'
            << node.point.value() << ':' << node.enabled;
    }
    for (const auto& node : document.pass_nodes) {
        out << "|R:" << node.node_id << ':' << node.type.value() << ':' << node.feature_node << ':' << node.part_id
            << ':' << node.enabled;
    }
    for (const auto& resource : document.resources) {
        out << "|Q:" << resource.node_id << ':' << resource.resource_id.value();
    }
    for (const auto& edge : document.edges) {
        out << "|E:" << edge.edge_id << ':' << static_cast<uint32_t>(edge.kind) << ':' << edge.source.node_id << ':'
            << edge.source.port_id << ':' << edge.destination.node_id << ':' << edge.destination.port_id << ':'
            << edge.binding_id;
    }
    for (const auto& source_node : document.present_connections)
        out << "|Present:" << source_node;
    return out.str();
}

foundation::ResultValue<PreparedPipelineDocument> prepare_pipeline_document(const PipelineDocument& document,
                                                                            const TypeCatalog& catalog,
                                                                            const PipelineRegistry&,
                                                                            const RenderPassRegistry& pass_registry) {
    const auto diagnostics = validate_pipeline_document(document, catalog);
    if (!diagnostics.empty()) {
        return {foundation::invalid_argument(diagnostics.front().message), {}};
    }
    PreparedPipelineDocument prepared;
    prepared.semantic_digest = document_semantic_digest(document);
    std::unordered_map<std::string, const ResourceNodeDocument*> resource_nodes;
    resource_nodes.reserve(document.resources.size());
    for (const ResourceNodeDocument& resource : document.resources) {
        resource_nodes.emplace(resource.node_id, &resource);
    }
    const auto choose_queue = [&](const RenderPassDescriptor& descriptor,
                                  rhi::QueueClass& queue) -> foundation::Result {
        const auto supports = [&](rhi::QueueClass candidate) {
            return (descriptor.allowed_queue_mask & (uint64_t{1} << static_cast<uint8_t>(candidate))) != 0;
        };
        if (supports(queue)) {
            return foundation::Result::success();
        }
        for (const rhi::QueueClass candidate :
             {rhi::QueueClass::kGraphics, rhi::QueueClass::kCompute, rhi::QueueClass::kCopy}) {
            if (supports(candidate)) {
                queue = candidate;
                return foundation::Result::success();
            }
        }
        return foundation::unsupported("render pass has no supported default queue");
    };

    const auto resolve_part = [](const FeatureAuthoringDeclaration& declaration, FeatureInstanceId instance,
                                 std::string_view name, FeaturePartId& resolved) -> foundation::Result {
        if (name.empty()) {
            resolved = {};
            return foundation::Result::success();
        }
        for (const AuthoringPart& part : declaration.parts) {
            if (part.name == name) {
                resolved = make_feature_part_id(instance, part.part_index);
                return foundation::Result::success();
            }
        }
        return foundation::invalid_argument("render pass references an unknown feature part");
    };

    const auto sort_passes = [](std::vector<RenderPassInstanceConfiguration>& passes,
                                std::span<const DocumentEdge> edges) -> foundation::Result {
        if (passes.size() < 2) {
            return foundation::Result::success();
        }
        std::unordered_map<std::string, uint32_t> indices;
        indices.reserve(passes.size());
        for (uint32_t index = 0; index < passes.size(); ++index) {
            indices.emplace(passes[index].source, index);
        }
        std::vector<std::vector<uint32_t>> outgoing(passes.size());
        std::vector<uint32_t> incoming(passes.size(), 0);
        for (const DocumentEdge& edge : edges) {
            if (edge.kind != DocumentEdgeKind::kOrdering) {
                continue;
            }
            const auto source = indices.find(edge.source.node_id);
            const auto destination = indices.find(edge.destination.node_id);
            if (source == indices.end() || destination == indices.end()) {
                continue;
            }
            outgoing[source->second].push_back(destination->second);
            ++incoming[destination->second];
            passes[destination->second].ordered_after.push_back(passes[source->second].instance);
        }
        std::vector<uint32_t> ready;
        for (uint32_t index = 0; index < incoming.size(); ++index) {
            if (incoming[index] == 0) {
                ready.push_back(index);
            }
        }
        std::vector<RenderPassInstanceConfiguration> ordered;
        ordered.reserve(passes.size());
        while (!ready.empty()) {
            const auto minimum = std::min_element(ready.begin(), ready.end());
            const uint32_t current = *minimum;
            ready.erase(minimum);
            ordered.push_back(std::move(passes[current]));
            for (const uint32_t next : outgoing[current]) {
                if (--incoming[next] == 0) {
                    ready.push_back(next);
                }
            }
        }
        if (ordered.size() != passes.size()) {
            return foundation::invalid_argument("authored render pass ordering contains a cycle");
        }
        passes = std::move(ordered);
        return foundation::Result::success();
    };

    for (const auto& node : document.pipeline_nodes) {
        auto configuration = std::make_shared<PipelineInstanceConfiguration>();
        configuration->instance = PipelineInstanceId(node.node_id);
        configuration->pipeline = node.type;
        configuration->scope = node.scope;
        configuration->source = node.node_id;

        const auto* pipeline_entry = catalog.find({AuthoringTypeKind::kPipeline, node.type.value()});
        const auto* pipeline_declaration =
            pipeline_entry == nullptr ? nullptr
                                      : std::get_if<PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
        if (pipeline_declaration == nullptr) {
            return {foundation::invalid_argument("pipeline declaration is unavailable"), {}};
        }

        struct FeatureSource final {
            std::string node_id{};
            FeatureId type{};
            FeaturePointId point{};
            ParameterBlock parameters{};
            bool explicit_node{false};
        };
        std::vector<FeatureSource> feature_sources;
        std::set<std::pair<FeatureId, FeaturePointId>> explicit_default_keys;
        for (const auto& feature : document.feature_nodes) {
            if (feature.pipeline_node != node.node_id) {
                continue;
            }
            explicit_default_keys.emplace(feature.type, feature.point);
            if (!feature.enabled) {
                continue;
            }
            feature_sources.push_back({feature.node_id, feature.type, feature.point, feature.parameters, true});
        }
        for (const DefaultFeatureNode& feature : pipeline_declaration->default_features) {
            if (explicit_default_keys.contains({feature.type, feature.point})) {
                continue;
            }
            feature_sources.push_back({feature.node_id, feature.type, feature.point, feature.parameters, false});
        }

        for (const FeatureSource& feature : feature_sources) {
            const auto* feature_entry = catalog.find({AuthoringTypeKind::kFeature, feature.type.value()});
            const auto* feature_declaration =
                feature_entry == nullptr ? nullptr
                                         : std::get_if<FeatureAuthoringDeclaration>(&feature_entry->declaration);
            if (feature_declaration == nullptr) {
                return {foundation::invalid_argument("feature declaration is unavailable"), {}};
            }
            FeatureInstanceConfiguration feature_configuration;
            feature_configuration.instance =
                feature.explicit_node ? FeatureInstanceId(feature.node_id)
                                      : make_feature_instance_id(feature.type, node.type, node.scope, feature.point);
            feature_configuration.feature = feature.type;
            feature_configuration.point = feature.point;
            feature_configuration.parameters = feature.parameters;
            feature_configuration.expand_authored_passes = feature_declaration->allow_add_passes;
            feature_configuration.source = feature.node_id;

            std::vector<DocumentEdge> ordering_edges;
            const auto append_pass = [&](std::string node_id, RenderPassTypeId type, ParameterBlock parameters,
                                         bool enabled, std::string part, std::span<const DocumentEdge> edges,
                                         const std::unordered_map<std::string, uint64_t>& default_resources) {
                RenderPassInstanceConfiguration pass_configuration;
                pass_configuration.type = type;
                const auto registered = pass_registry.find_pass(type);
                if (!registered) {
                    return foundation::ResultValue<bool>{registered.result, false};
                }
                if (const foundation::Result queue_result =
                        choose_queue(registered.value->descriptor(), pass_configuration.queue);
                    !queue_result) {
                    return foundation::ResultValue<bool>{queue_result, false};
                }
                pass_configuration.instance = RenderPassInstanceId(node_id);
                pass_configuration.parameters = std::move(parameters);
                pass_configuration.enabled = enabled;
                pass_configuration.part = std::move(part);
                pass_configuration.source = pass_configuration.instance.is_valid() ? node_id : std::string{};
                if (const foundation::Result part_result =
                        resolve_part(*feature_declaration, feature_configuration.instance, pass_configuration.part,
                                     pass_configuration.resolved_part);
                    !part_result) {
                    return foundation::ResultValue<bool>{part_result, false};
                }
                for (const DocumentEdge& edge : edges) {
                    if (edge.kind != DocumentEdgeKind::kRenderResource) {
                        continue;
                    }
                    const DocumentEndpoint* other_endpoint = nullptr;
                    if (edge.source.node_id == node_id) {
                        other_endpoint = &edge.destination;
                    } else if (edge.destination.node_id == node_id) {
                        other_endpoint = &edge.source;
                    }
                    if (other_endpoint == nullptr) {
                        continue;
                    }
                    uint64_t resource_id = 0;
                    if (const auto resource = resource_nodes.find(other_endpoint->node_id);
                        resource != resource_nodes.end()) {
                        resource_id = resource->second->resource_id.value();
                    } else if (const auto resource = default_resources.find(other_endpoint->node_id);
                               resource != default_resources.end()) {
                        resource_id = resource->second;
                    }
                    if (resource_id == 0) {
                        continue;
                    }
                    const auto [binding, inserted] =
                        pass_configuration.resource_bindings.emplace(edge.binding_id, resource_id);
                    if (!inserted && binding->second != resource_id) {
                        return foundation::ResultValue<bool>{
                            foundation::invalid_argument("render pass resource binding is ambiguous: " + node_id),
                            false};
                    }
                }
                feature_configuration.passes.push_back(std::move(pass_configuration));
                return foundation::ResultValue<bool>{foundation::Result::success(), true};
            };

            std::unordered_map<std::string, uint64_t> default_resources;
            for (const DefaultResourceNode& resource : feature_declaration->default_resources) {
                default_resources.emplace(resource.node_id, resource.resource_id.value());
            }
            bool has_explicit_passes = false;
            for (const auto& pass : document.pass_nodes) {
                if (pass.feature_node != feature.node_id) {
                    continue;
                }
                has_explicit_passes = true;
                if (const auto result = append_pass(pass.node_id, pass.type, pass.parameters, pass.enabled,
                                                    pass.part_id, document.edges, default_resources);
                    !result) {
                    return {result.result, {}};
                }
            }
            if (has_explicit_passes) {
                ordering_edges.assign(document.edges.begin(), document.edges.end());
            } else {
                ordering_edges = feature_declaration->default_edges;
                for (const DefaultPassNode& pass : feature_declaration->default_passes) {
                    if (const auto result =
                            append_pass(pass.node_id, pass.type, pass.parameters, pass.enabled, pass.part_id,
                                        feature_declaration->default_edges, default_resources);
                        !result) {
                        return {result.result, {}};
                    }
                }
            }
            if (const foundation::Result order_result = sort_passes(feature_configuration.passes, ordering_edges);
                !order_result) {
                return {order_result, {}};
            }
            configuration->features.push_back(std::move(feature_configuration));
        }
        prepared.configurations.push_back(std::move(configuration));
    }
    prepared.composition_edges = document.edges;
    for (const auto& node : document.pipeline_nodes) {
        prepared.source_map.emplace(node.node_id, node.node_id);
    }
    for (const auto& node : document.feature_nodes) {
        prepared.source_map.emplace(node.node_id, node.node_id);
    }
    for (const auto& node : document.pass_nodes) {
        prepared.source_map.emplace(node.node_id, node.node_id);
    }
    return {foundation::Result::success(), std::move(prepared)};
}

}  // namespace aeg::pipeline
