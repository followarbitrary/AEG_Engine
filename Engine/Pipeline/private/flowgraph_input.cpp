/** @file flowgraph_input.cpp @brief Validates explicit FlowGraph input sources and slots. */
#include "pipeline_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace aeg::pipeline {
namespace {

using InputKey = std::tuple<std::string, uint64_t, uint64_t>;

void add_diagnostic(std::vector<DocumentDiagnostic>& result, std::string code, std::string message,
                   std::string node = {}, std::string edge = {}, uint64_t port = 0, uint64_t parameter = 0,
                   uint64_t slot = 0) {
    result.push_back({std::move(code), std::move(message), std::move(node), std::move(edge), port, parameter, slot});
}

const PortDescriptor* find_port(const TypeCatalogEntry& entry, uint64_t port_id) {
    const auto found = std::find_if(entry.ports.begin(), entry.ports.end(),
                                    [port_id](const PortDescriptor& port) { return port.id == port_id; });
    return found == entry.ports.end() ? nullptr : &*found;
}

const VariadicInputDescriptor* find_group(const TypeCatalogEntry& entry, uint64_t group_id) {
    const auto found = std::find_if(entry.variadic_inputs.begin(), entry.variadic_inputs.end(),
                                    [group_id](const VariadicInputDescriptor& group) { return group.id == group_id; });
    return found == entry.variadic_inputs.end() ? nullptr : &*found;
}

bool valid_source(InputSourceKind source) noexcept {
    return static_cast<uint8_t>(source) <= static_cast<uint8_t>(InputSourceKind::kNone);
}

bool valid_status(InputBindingStatus status) noexcept {
    return static_cast<uint8_t>(status) <= static_cast<uint8_t>(InputBindingStatus::kCancelled);
}

bool valid_asset(const AssetReference& reference) noexcept {
    if (reference.uuid.size() != 36)
        return false;
    bool non_zero = false;
    for (size_t index = 0; index < reference.uuid.size(); ++index) {
        const char value = reference.uuid[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-')
                return false;
            continue;
        }
        const bool hex = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                         (value >= 'A' && value <= 'F');
        if (!hex)
            return false;
        non_zero |= value != '0';
    }
    return non_zero;
}

bool finite_value(const ParameterValue& value) {
    if (const auto* scalar = std::get_if<double>(&value))
        return std::isfinite(*scalar);
    if (const auto* vector = std::get_if<std::vector<double>>(&value))
        return std::all_of(vector->begin(), vector->end(), [](double item) { return std::isfinite(item); });
    return true;
}

bool is_data_edge_for(const DocumentEdge& edge, const std::string& node, uint64_t port, uint64_t slot) {
    return edge.kind != DocumentEdgeKind::kOrdering && edge.destination.node_id == node &&
           edge.destination.port_id == port && edge.destination.slot_id == slot;
}

const DocumentEdge* find_edge(const PipelineDocument& document, std::string_view edge_id) {
    const auto found = std::find_if(document.edges.begin(), document.edges.end(),
                                    [edge_id](const DocumentEdge& edge) { return edge.edge_id == edge_id; });
    return found == document.edges.end() ? nullptr : &*found;
}

struct NodeInputInfo final {
    const TypeCatalogEntry* entry{};
    std::string node_id{};
};

}  // namespace

std::vector<DocumentDiagnostic> validate_flowgraph_inputs(const PipelineDocument& document, const TypeCatalog& catalog) {
    std::vector<DocumentDiagnostic> result;
    std::unordered_map<std::string, NodeInputInfo> nodes;
    for (const auto& node : document.pipeline_nodes)
        if (const auto* entry = catalog.find({AuthoringTypeKind::kPipeline, node.type.value()}))
            nodes.emplace(node.node_id, NodeInputInfo{entry, node.node_id});
    for (const auto& node : document.feature_nodes)
        if (const auto* entry = catalog.find({AuthoringTypeKind::kFeature, node.type.value()}))
            nodes.emplace(node.node_id, NodeInputInfo{entry, node.node_id});
    for (const auto& node : document.pass_nodes)
        if (const auto* entry = catalog.find({AuthoringTypeKind::kRenderPass, node.type.value()}))
            nodes.emplace(node.node_id, NodeInputInfo{entry, node.node_id});

    std::map<InputKey, const InputBindingDocument*> bindings;
    for (const auto& binding : document.input_bindings) {
        const auto node = nodes.find(binding.node_id);
        if (node == nodes.end()) {
            // Resource nodes are authored directly in the document rather
            // than through TypeCatalog. Their intrinsic Resource port still
            // follows the same source contract as a typed node input.
            const auto resource = std::find_if(document.resources.begin(), document.resources.end(),
                                               [&](const auto& value) { return value.node_id == binding.node_id; });
            if (resource != document.resources.end()) {
                if (binding.port_id != resource->resource_id.value() || binding.slot_id != 0) {
                    add_diagnostic(result, "input.port", "resource input binding references an invalid port",
                                   binding.node_id, {}, binding.port_id, 0, binding.slot_id);
                    continue;
                }
                const InputKey resource_key{binding.node_id, binding.port_id, 0};
                if (!bindings.emplace(resource_key, &binding).second) {
                    add_diagnostic(result, "input.duplicate", "resource input binding is duplicated",
                                   binding.node_id, {}, binding.port_id);
                    continue;
                }
                if (!valid_source(binding.source) || !valid_status(binding.status)) {
                    add_diagnostic(result, "input.state", "resource input source or status is invalid",
                                   binding.node_id, {}, binding.port_id);
                    continue;
                }
                if (binding.source == InputSourceKind::kUnset && binding.status != InputBindingStatus::kUnset)
                    add_diagnostic(result, "input.state", "Unset resource input cannot carry a resolved status",
                                   binding.node_id, {}, binding.port_id);
                if (binding.source != InputSourceKind::kUnset && binding.source != InputSourceKind::kNone &&
                    binding.status == InputBindingStatus::kUnset)
                    add_diagnostic(result, "input.unset", "active resource input must first be Bound",
                                   binding.node_id, {}, binding.port_id);
                if (binding.status == InputBindingStatus::kPending)
                    add_diagnostic(result, "input.pending", "resource input source resolution is pending",
                                   binding.node_id, {}, binding.port_id);
                if (binding.status == InputBindingStatus::kInvalid)
                    add_diagnostic(result, "input.invalid", "resource input source resolution failed",
                                   binding.node_id, {}, binding.port_id);
                if (binding.status == InputBindingStatus::kCancelled)
                    add_diagnostic(result, "input.cancelled", "resource input source resolution was cancelled",
                                   binding.node_id, {}, binding.port_id);
                if (binding.source != InputSourceKind::kUnset && binding.source != InputSourceKind::kNone &&
                    binding.source != InputSourceKind::kAsset && binding.source != InputSourceKind::kEdge) {
                    add_diagnostic(result, "input.source", "resource input accepts only Asset, Edge or None",
                                   binding.node_id, {}, binding.port_id);
                }
                if (binding.source == InputSourceKind::kAsset && !valid_asset(binding.asset))
                    add_diagnostic(result, "input.asset", "resource Asset source requires a stable AssetId",
                                   binding.node_id, {}, binding.port_id);
                std::unordered_set<std::string> edge_ids;
                for (const auto& edge_id : binding.edge_ids) {
                    if (!edge_ids.insert(edge_id).second) {
                        add_diagnostic(result, "input.edge", "resource input binding repeats an edge",
                                       binding.node_id, edge_id, binding.port_id);
                        continue;
                    }
                    const auto* edge = find_edge(document, edge_id);
                    if (edge == nullptr || !is_data_edge_for(*edge, binding.node_id, binding.port_id, 0))
                        add_diagnostic(result, "input.edge", "resource input binding does not match its data edge",
                                       binding.node_id, edge_id, binding.port_id);
                }
                if (binding.source == InputSourceKind::kEdge && binding.edge_ids.empty())
                    add_diagnostic(result, "input.edge", "resource Edge source requires at least one data edge",
                                   binding.node_id, {}, binding.port_id);
                if (binding.source != InputSourceKind::kEdge && !binding.edge_ids.empty())
                    add_diagnostic(result, "input.source", "resource non-Edge source cannot carry data edges",
                                   binding.node_id, {}, binding.port_id);
                const auto incoming_count = static_cast<uint32_t>(std::count_if(
                    document.edges.begin(), document.edges.end(), [&](const DocumentEdge& edge) {
                        return is_data_edge_for(edge, binding.node_id, binding.port_id, 0);
                    }));
                if (binding.source == InputSourceKind::kEdge && incoming_count > 1)
                    add_diagnostic(result, "input.multiplicity", "resource input has too many data edges",
                                   binding.node_id, {}, binding.port_id);
                if (!resource->external_input &&
                    (binding.source == InputSourceKind::kUnset || binding.source == InputSourceKind::kNone))
                    add_diagnostic(result, "input.required", "produced resource input has no active source",
                                   binding.node_id, {}, binding.port_id);
                continue;
            }
            add_diagnostic(result, "input.node", "input binding references an unknown node", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
            continue;
        }
        const auto* port = find_port(*node->second.entry, binding.port_id);
        if (port == nullptr || port->direction != PortDirection::kInput || !port->input) {
            // Legacy ports are validated by the historical document validator. An explicit
            // binding for one is still rejected so the document has one unambiguous model.
            add_diagnostic(result, "input.port", "input binding references an undeclared explicit input port",
                           binding.node_id, {}, binding.port_id, 0, binding.slot_id);
            continue;
        }
        const InputKey key{binding.node_id, binding.port_id, binding.slot_id};
        if (!bindings.emplace(key, &binding).second) {
            add_diagnostic(result, "input.duplicate", "input binding is duplicated", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
            continue;
        }
        const auto& contract = *port->input;
        const auto* group = contract.variadic_group_id ? find_group(*node->second.entry, contract.variadic_group_id)
                                                        : nullptr;
        if (contract.variadic_group_id && group == nullptr) {
            add_diagnostic(result, "input.group", "input binding references an undeclared variadic group",
                           binding.node_id, {}, binding.port_id, 0, binding.slot_id);
        }
        if (group == nullptr && binding.slot_id != 0)
            add_diagnostic(result, "input.slot", "fixed input binding cannot carry a SlotId", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        if (group != nullptr && binding.slot_id == 0)
            add_diagnostic(result, "input.slot", "variadic input binding requires a SlotId", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        if (!valid_source(binding.source) || !valid_status(binding.status)) {
            add_diagnostic(result, "input.state", "input binding source or status is invalid", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
            continue;
        }
        if (binding.source == InputSourceKind::kUnset && binding.status != InputBindingStatus::kUnset)
            add_diagnostic(result, "input.state", "Unset input cannot carry a resolved status", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        if (binding.source != InputSourceKind::kUnset && binding.source != InputSourceKind::kNone &&
            binding.status == InputBindingStatus::kUnset)
            add_diagnostic(result, "input.unset", "active input source must first be Bound", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        if (binding.status == InputBindingStatus::kPending)
            add_diagnostic(result, "input.pending", "input binding source resolution is pending", binding.node_id,
                           {}, binding.port_id, 0, binding.slot_id);
        if (binding.status == InputBindingStatus::kInvalid)
            add_diagnostic(result, "input.invalid", "input binding source resolution failed", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        if (binding.status == InputBindingStatus::kCancelled)
            add_diagnostic(result, "input.cancelled", "input binding source resolution was cancelled",
                           binding.node_id, {}, binding.port_id, 0, binding.slot_id);

        const InputSourceMask source_mask = input_source_mask(binding.source);
        if (binding.source != InputSourceKind::kUnset && binding.source != InputSourceKind::kNone &&
            (contract.allowed_sources & source_mask) == 0) {
            add_diagnostic(result, "input.source", "input source is not allowed by the port contract", binding.node_id,
                           {}, binding.port_id, 0, binding.slot_id);
        }
        std::unordered_set<std::string> edge_ids;
        for (const auto& edge_id : binding.edge_ids) {
            if (!edge_ids.insert(edge_id).second) {
                add_diagnostic(result, "input.edge", "input binding repeats an edge", binding.node_id, edge_id,
                               binding.port_id, 0, binding.slot_id);
                continue;
            }
            const auto* edge = find_edge(document, edge_id);
            if (edge == nullptr || !is_data_edge_for(*edge, binding.node_id, binding.port_id, binding.slot_id)) {
                add_diagnostic(result, "input.edge", "input binding does not match its destination data edge",
                               binding.node_id, edge_id, binding.port_id, 0, binding.slot_id);
            }
        }
        if (binding.source == InputSourceKind::kEdge && binding.edge_ids.empty())
            add_diagnostic(result, "input.edge", "Edge source requires at least one data edge", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        if (binding.source != InputSourceKind::kEdge && !binding.edge_ids.empty())
            add_diagnostic(result, "input.source", "non-Edge source cannot carry data edges", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        const auto incoming_count = static_cast<uint32_t>(std::count_if(
            document.edges.begin(), document.edges.end(), [&](const DocumentEdge& edge) {
                return is_data_edge_for(edge, binding.node_id, binding.port_id, binding.slot_id);
            }));
        if (binding.source != InputSourceKind::kEdge) {
            for (const auto& edge : document.edges) {
                if (is_data_edge_for(edge, binding.node_id, binding.port_id, binding.slot_id) &&
                    std::find(binding.edge_ids.begin(), binding.edge_ids.end(), edge.edge_id) == binding.edge_ids.end())
                    add_diagnostic(result, "input.source", "active non-Edge source conflicts with an incoming data edge",
                                   binding.node_id, edge.edge_id, binding.port_id, 0, binding.slot_id);
            }
        } else {
            for (const auto& edge : document.edges) {
                if (is_data_edge_for(edge, binding.node_id, binding.port_id, binding.slot_id) &&
                    std::find(binding.edge_ids.begin(), binding.edge_ids.end(), edge.edge_id) == binding.edge_ids.end())
                    add_diagnostic(result, "input.edge", "Edge source must enumerate every incoming data edge",
                                   binding.node_id, edge.edge_id, binding.port_id, 0, binding.slot_id);
            }
        }
        if (binding.source == InputSourceKind::kEdge && contract.max_edges != 0 && incoming_count > contract.max_edges)
            add_diagnostic(result, "input.multiplicity", "input slot has too many data edges", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        if (binding.source == InputSourceKind::kEdge && incoming_count < contract.min_edges)
            add_diagnostic(result, "input.required", "input slot has too few data edges", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);
        if (contract.presence == InputPresenceRequirement::kRequired &&
            (binding.source == InputSourceKind::kUnset || binding.source == InputSourceKind::kNone))
            add_diagnostic(result, "input.required", "required input has no valid active source", binding.node_id, {},
                           binding.port_id, 0, binding.slot_id);

        switch (binding.source) {
            case InputSourceKind::kAsset:
                if (!valid_asset(binding.asset))
                    add_diagnostic(result, "input.asset", "Asset source requires a stable AssetId",
                                   binding.node_id, {}, binding.port_id, 0, binding.slot_id);
                break;
            case InputSourceKind::kLiteral:
                if (!contract.literal_schema ||
                    !validate_parameter_block({{binding.port_id, binding.literal}},
                                              std::span(&*contract.literal_schema, 1)))
                    add_diagnostic(result, "input.literal", "Literal source does not match its input schema",
                                   binding.node_id, {}, binding.port_id, 0, binding.slot_id);
                break;
            case InputSourceKind::kDefault:
                if (!contract.default_value || binding.default_id != contract.default_value->id ||
                    binding.default_revision != contract.default_value->revision)
                    add_diagnostic(result, "input.default", "Default source does not match its contract revision",
                                   binding.node_id, {}, binding.port_id, 0, binding.slot_id);
                break;
            case InputSourceKind::kContext:
                if (std::find(contract.context_keys.begin(), contract.context_keys.end(), binding.context_key) ==
                    contract.context_keys.end())
                    add_diagnostic(result, "input.context", "Context key is not declared by the input contract",
                                   binding.node_id, {}, binding.port_id, 0, binding.slot_id);
                break;
            default:
                break;
        }
    }

    std::map<std::pair<std::string, uint64_t>, const VariadicInputSlotDocument*> slots;
    std::map<std::pair<std::string, uint64_t>, std::set<uint32_t>> orders;
    for (const auto& slot : document.variadic_slots) {
        if (!slot.slot_id) {
            add_diagnostic(result, "input.slot", "variadic SlotId must be non-zero", slot.node_id, {}, 0, 0,
                           slot.slot_id);
            continue;
        }
        const auto node = nodes.find(slot.node_id);
        const auto* group = node == nodes.end() ? nullptr : find_group(*node->second.entry, slot.group_id);
        if (node == nodes.end())
            add_diagnostic(result, "input.node", "variadic slot references an unknown node", slot.node_id, {}, 0, 0,
                           slot.slot_id);
        else if (group == nullptr)
            add_diagnostic(result, "input.group", "variadic slot references an undeclared group", slot.node_id, {},
                           0, 0, slot.slot_id);
        const auto slot_key = std::make_pair(slot.node_id, slot.slot_id);
        if (!slots.emplace(slot_key, &slot).second)
            add_diagnostic(result, "input.slot", "variadic SlotId is duplicated", slot.node_id, {}, 0, 0,
                           slot.slot_id);
        if (group != nullptr && group->ordered && !orders[std::make_pair(slot.node_id, slot.group_id)].insert(slot.order).second)
            add_diagnostic(result, "input.order", "ordered variadic slots must have unique order values", slot.node_id,
                           {}, 0, 0, slot.slot_id);
    }

    std::map<std::pair<std::string, uint64_t>, std::vector<const VariadicInputSlotDocument*>> group_slots;
    for (const auto& [key, slot] : slots) {
        if (const auto node = nodes.find(slot->node_id); node != nodes.end())
            group_slots[{slot->node_id, slot->group_id}].push_back(slot);
    }
    for (const auto& [key, binding] : bindings) {
        const auto node = nodes.find(binding->node_id);
        if (node == nodes.end())
            continue;
        const auto* port = find_port(*node->second.entry, binding->port_id);
        if (port == nullptr || !port->input || !port->input->variadic_group_id || binding->slot_id == 0)
            continue;
        if (!slots.contains({binding->node_id, binding->slot_id}))
            add_diagnostic(result, "input.slot", "input binding references an undeclared variadic SlotId",
                           binding->node_id, {}, binding->port_id, 0, binding->slot_id);
    }
    for (const auto& [group_key, declared_slots] : group_slots) {
        const auto node = nodes.find(group_key.first);
        const auto* group = node == nodes.end() ? nullptr : find_group(*node->second.entry, group_key.second);
        if (group == nullptr)
            continue;
        if (declared_slots.size() > group->max_slots)
            add_diagnostic(result, "input.multiplicity", "variadic input group has too many slots", group_key.first,
                           {}, group->member_port_id);
    }

    // A declared slot is a first-class input even when it has no wire. This is
    // what permits an Asset/Literal/Default source to satisfy a Texture slot.
    for (const auto& [slot_key, slot] : slots) {
        const auto node = nodes.find(slot->node_id);
        if (node == nodes.end())
            continue;
        const auto* group = find_group(*node->second.entry, slot->group_id);
        if (group == nullptr)
            continue;
        const InputKey key{slot->node_id, group->member_port_id, slot->slot_id};
        if (!bindings.contains(key)) {
            const auto* port = find_port(*node->second.entry, group->member_port_id);
            if (port != nullptr && port->input->presence == InputPresenceRequirement::kRequired)
                add_diagnostic(result, "input.required", "declared variadic slot has no active source", slot->node_id,
                               {}, group->member_port_id, 0, slot->slot_id);
        }
    }

    for (const auto& [node_id, info] : nodes) {
        for (const auto& group : info.entry->variadic_inputs) {
            const auto count = group_slots[std::make_pair(node_id, group.id)].size();
            if (count < group.min_slots && !(count == 0 && group.empty_is_valid))
                add_diagnostic(result, "input.required", "variadic input group has too few slots", node_id, {},
                               group.member_port_id);
        }
    }

    // Validate every explicit port, including a fixed port with no binding and
    // a variadic group whose empty state is legal.
    for (const auto& [node_id, info] : nodes) {
        for (const auto& port : info.entry->ports) {
            if (port.direction != PortDirection::kInput || !port.input)
                continue;
            const auto& contract = *port.input;
            const auto* group = contract.variadic_group_id ? find_group(*info.entry, contract.variadic_group_id) : nullptr;
            if (group != nullptr)
                continue;
            const InputKey key{node_id, port.id, 0};
            if (bindings.contains(key))
                continue;
            if (contract.presence == InputPresenceRequirement::kRequired)
                add_diagnostic(result, "input.required", "required input has no active source", node_id, {}, port.id);
        }
    }

    // Every data edge into an explicit input must be represented by that
    // input's source binding. This prevents a wire from silently becoming a
    // second, implicit source for an optional or variadic port.
    for (const auto& edge : document.edges) {
        if (edge.kind == DocumentEdgeKind::kOrdering)
            continue;
        const auto node = nodes.find(edge.destination.node_id);
        if (node == nodes.end()) {
            const auto resource = std::find_if(document.resources.begin(), document.resources.end(),
                                               [&](const auto& value) { return value.node_id == edge.destination.node_id; });
            if (resource != document.resources.end() && edge.destination.port_id == resource->resource_id.value()) {
                const InputKey key{edge.destination.node_id, edge.destination.port_id, edge.destination.slot_id};
                const auto binding = bindings.find(key);
                if (binding == bindings.end() || binding->second->source != InputSourceKind::kEdge ||
                    std::find(binding->second->edge_ids.begin(), binding->second->edge_ids.end(), edge.edge_id) ==
                        binding->second->edge_ids.end())
                    add_diagnostic(result, "input.binding", "resource data edge has no matching Edge binding",
                                   edge.destination.node_id, edge.edge_id, edge.destination.port_id, 0,
                                   edge.destination.slot_id);
            }
            continue;
        }
        const auto* port = find_port(*node->second.entry, edge.destination.port_id);
        if (port == nullptr || !port->input)
            continue;
        const InputKey key{edge.destination.node_id, edge.destination.port_id, edge.destination.slot_id};
        if (!bindings.contains(key))
            add_diagnostic(result, "input.binding", "data edge into an explicit input has no Edge binding",
                           edge.destination.node_id, edge.edge_id, edge.destination.port_id, 0,
                           edge.destination.slot_id);
    }

    // The edge is the only ordering primitive. Check that explicit slots agree
    // with their declared group before the general edge validator runs.
    for (const auto& edge : document.edges) {
        if (edge.kind == DocumentEdgeKind::kOrdering)
            continue;
        const auto node = nodes.find(edge.destination.node_id);
        if (node == nodes.end())
            continue;
        const auto* port = find_port(*node->second.entry, edge.destination.port_id);
        if (port == nullptr || !port->input)
            continue;
        const auto* group = port->input->variadic_group_id ? find_group(*node->second.entry, port->input->variadic_group_id)
                                                            : nullptr;
        if (group == nullptr && edge.destination.slot_id != 0)
            add_diagnostic(result, "edge.slot", "fixed input edge cannot carry a SlotId", edge.destination.node_id,
                           edge.edge_id, edge.destination.port_id, 0, edge.destination.slot_id);
        if (group != nullptr && edge.destination.slot_id == 0)
            add_diagnostic(result, "edge.slot", "variadic input edge requires a SlotId", edge.destination.node_id,
                           edge.edge_id, edge.destination.port_id, 0, edge.destination.slot_id);
        if (group != nullptr && !slots.contains({edge.destination.node_id, edge.destination.slot_id}))
            add_diagnostic(result, "edge.slot", "edge targets an undeclared variadic SlotId", edge.destination.node_id,
                           edge.edge_id, edge.destination.port_id, 0, edge.destination.slot_id);
    }

    return result;
}

foundation::ResultValue<PipelineDocument> migrate_legacy_ordering_edges(const PipelineDocument& document,
                                                                         const TypeCatalog& catalog) {
    PipelineDocument migrated = document;
    const auto entry_for = [&](std::string_view node_id) -> const TypeCatalogEntry* {
        for (const auto& node : migrated.pipeline_nodes)
            if (node.node_id == node_id)
                return catalog.find({AuthoringTypeKind::kPipeline, node.type.value()});
        for (const auto& node : migrated.feature_nodes)
            if (node.node_id == node_id)
                return catalog.find({AuthoringTypeKind::kFeature, node.type.value()});
        for (const auto& node : migrated.pass_nodes)
            if (node.node_id == node_id)
                return catalog.find({AuthoringTypeKind::kRenderPass, node.type.value()});
        return nullptr;
    };
    const auto compatible = [](const PortDescriptor& source, const PortDescriptor& destination) {
        return source.binding_id != 0 && source.binding_id == destination.binding_id &&
               source.value_kind == destination.value_kind && source.resource_constraints.resource_class ==
                   destination.resource_constraints.resource_class;
    };
    const auto edge_kind = [](PortValueKind kind) {
        return kind == PortValueKind::kArtifact       ? DocumentEdgeKind::kArtifact
               : kind == PortValueKind::kRenderState  ? DocumentEdgeKind::kRenderState
                                                       : DocumentEdgeKind::kRenderResource;
    };
    for (auto& edge : migrated.edges) {
        if (edge.kind != DocumentEdgeKind::kOrdering)
            continue;
        const auto* source_entry = entry_for(edge.source.node_id);
        const auto* destination_entry = entry_for(edge.destination.node_id);
        if (source_entry == nullptr || destination_entry == nullptr)
            return {foundation::unsupported("legacy ordering edge references an unavailable node type"), {}};
        const PortDescriptor* source_port = nullptr;
        const PortDescriptor* destination_port = nullptr;
        for (const auto& candidate_source : source_entry->ports) {
            if (candidate_source.direction != PortDirection::kOutput)
                continue;
            for (const auto& candidate_destination : destination_entry->ports) {
                if (candidate_destination.direction == PortDirection::kInput &&
                    candidate_destination.input && candidate_destination.input->variadic_group_id != 0)
                    continue;
                if (candidate_destination.direction == PortDirection::kInput &&
                    compatible(candidate_source, candidate_destination)) {
                    if (source_port != nullptr)
                        return {foundation::invalid_argument("legacy ordering edge has ambiguous data ports"), {}};
                    source_port = &candidate_source;
                    destination_port = &candidate_destination;
                }
            }
        }
        if (source_port == nullptr || destination_port == nullptr)
            return {foundation::unsupported("legacy ordering edge cannot be migrated to a unique data flow"), {}};
        edge.kind = edge_kind(source_port->value_kind);
        edge.source.port_id = source_port->id;
        edge.source.slot_id = 0;
        edge.destination.port_id = destination_port->id;
        edge.destination.slot_id = 0;
        edge.binding_id = source_port->binding_id;
        if (destination_port->input) {
            const auto found = std::find_if(migrated.input_bindings.begin(), migrated.input_bindings.end(),
                                            [&](const InputBindingDocument& binding) {
                                                return binding.node_id == edge.destination.node_id &&
                                                       binding.port_id == destination_port->id && binding.slot_id == 0;
                                            });
            if (found == migrated.input_bindings.end()) {
                migrated.input_bindings.push_back({.node_id = edge.destination.node_id,
                                                   .port_id = destination_port->id,
                                                   .source = InputSourceKind::kEdge,
                                                   .edge_ids = {edge.edge_id},
                                                   .status = InputBindingStatus::kBound});
            } else if (found->source != InputSourceKind::kEdge) {
                return {foundation::invalid_argument("legacy ordering edge conflicts with an input source"), {}};
            } else if (std::find(found->edge_ids.begin(), found->edge_ids.end(), edge.edge_id) == found->edge_ids.end()) {
                found->edge_ids.push_back(edge.edge_id);
            }
        }
    }
    return {foundation::Result::success(), std::move(migrated)};
}

}  // namespace aeg::pipeline
