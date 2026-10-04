#include "editor_node_definition.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

namespace aeg::application {
namespace {

using pipeline::InputPortContract;
using pipeline::ParameterDescriptor;
using pipeline::PortDescriptor;

bool input_default_equal(const std::optional<pipeline::InputDefaultDescriptor>& left,
                         const std::optional<pipeline::InputDefaultDescriptor>& right) {
    if (left.has_value() != right.has_value())
        return false;
    return !left || (left->id == right->id && left->revision == right->revision && left->value == right->value);
}

bool parameter_equal(const ParameterDescriptor& left, const ParameterDescriptor& right) {
    return left.id == right.id && left.name == right.name && left.display_name == right.display_name &&
           left.kind == right.kind && left.default_value == right.default_value && left.minimum == right.minimum &&
           left.maximum == right.maximum && left.enum_values == right.enum_values && left.unit == right.unit &&
           left.schema_version == right.schema_version && left.affects_topology == right.affects_topology;
}

bool input_equal(const std::optional<InputPortContract>& left, const std::optional<InputPortContract>& right) {
    if (left.has_value() != right.has_value())
        return false;
    if (!left)
        return true;
    const auto& a = *left;
    const auto& b = *right;
    return a.allowed_sources == b.allowed_sources && a.presence == b.presence && a.min_edges == b.min_edges &&
           a.max_edges == b.max_edges && a.variadic_group_id == b.variadic_group_id &&
           input_default_equal(a.default_value, b.default_value) && a.context_keys == b.context_keys &&
           (!a.literal_schema || !b.literal_schema ? a.literal_schema.has_value() == b.literal_schema.has_value()
                                                    : parameter_equal(*a.literal_schema, *b.literal_schema));
}

bool port_equal(const PortDescriptor& left, const PortDescriptor& right) {
    return left.id == right.id && left.name == right.name && left.direction == right.direction &&
           left.value_kind == right.value_kind && left.schema_id == right.schema_id &&
           left.schema_version == right.schema_version && left.required == right.required &&
           left.min_connections == right.min_connections && left.max_connections == right.max_connections &&
           left.binding_id == right.binding_id &&
           left.resource_constraints.resource_class == right.resource_constraints.resource_class &&
           left.resource_constraints.formats == right.resource_constraints.formats &&
           left.resource_constraints.size_rule == right.resource_constraints.size_rule &&
           left.resource_constraints.sample_count == right.resource_constraints.sample_count &&
           left.resource_constraints.capability_mask == right.resource_constraints.capability_mask &&
           left.resource_constraints.usage_mask == right.resource_constraints.usage_mask &&
           input_equal(left.input, right.input);
}

bool parameter_contract_equal(const ParameterDescriptor& left, const ParameterDescriptor& right) {
    auto a = left;
    auto b = right;
    a.name.clear();
    b.name.clear();
    return parameter_equal(a, b);
}

template <typename T, typename IdFn>
const T* find_by_id(std::span<const T> values, uint64_t id, IdFn id_fn) {
    const auto found = std::find_if(values.begin(), values.end(), [&](const T& item) { return id_fn(item) == id; });
    return found == values.end() ? nullptr : &*found;
}

bool contains(const std::vector<uint64_t>& ids, uint64_t value) {
    return std::find(ids.begin(), ids.end(), value) != ids.end();
}

void append_unique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

void append_unique(std::vector<uint64_t>& values, uint64_t value) {
    if (value && !contains(values, value))
        values.push_back(value);
}

}  // namespace

std::span<const NodePortSchema> node_port_schemas() noexcept {
    static constexpr std::array<NodePortSchema, 5> schemas{{
        {pipeline::PortValueKind::kArtifact, 1, "Artifact", pipeline::ResourceClass::kNone},
        {pipeline::PortValueKind::kRenderResource, 1, "Render Resource", pipeline::ResourceClass::kNone},
        {pipeline::PortValueKind::kRenderState, 1, "Render State", pipeline::ResourceClass::kNone},
        {pipeline::PortValueKind::kBuffer, 1, "Buffer", pipeline::ResourceClass::kBuffer},
        {pipeline::PortValueKind::kTexture, 1, "Texture", pipeline::ResourceClass::kTexture}}};
    return schemas;
}

foundation::Result validate_node_definition(const NodeDefinitionDraft& draft) {
    const auto& entry = draft.entry;
    if (auto result = pipeline::validate_type_catalog_entry(entry); !result)
        return result;

    std::unordered_set<uint64_t> retired_ports;
    for (const uint64_t id : draft.retired_port_ids) {
        if (!id || !retired_ports.insert(id).second ||
            std::any_of(entry.ports.begin(), entry.ports.end(), [id](const auto& port) { return port.id == id; }))
            return foundation::invalid_argument("retired port id must be unique and absent from the draft");
    }
    std::unordered_set<uint64_t> retired_parameters;
    for (const uint64_t id : draft.retired_parameter_ids) {
        if (!id || !retired_parameters.insert(id).second ||
            std::any_of(entry.parameters.begin(), entry.parameters.end(),
                        [id](const auto& parameter) { return parameter.id == id; }))
            return foundation::invalid_argument("retired parameter id must be unique and absent from the draft");
    }
    for (const auto& port : entry.ports) {
        if (draft.source == NodeContractSource::kEditorDraft &&
            std::none_of(node_port_schemas().begin(), node_port_schemas().end(), [&](const auto& schema) {
                return port.value_kind == schema.kind && port.schema_id == schema.id && port.schema_version == 1;
            }))
            return foundation::invalid_argument("Port " + std::to_string(port.id) + ": schema is not in the editor directory");
    }
    return foundation::Result::success();
}

NodeContractDiff diff_node_definition(const pipeline::TypeCatalogEntry& before,
                                     const NodeDefinitionDraft& after,
                                     std::span<const NodeContractUsage> usages) {
    NodeContractDiff diff;
    const auto& next = after.entry;

    for (const auto& old_port : before.ports) {
        const auto* new_port = find_by_id<PortDescriptor>(next.ports, old_port.id,
                                                          [](const auto& port) { return port.id; });
        if (!new_port) {
            diff.removed_ports.push_back(old_port.id);
            continue;
        }
        if (old_port.name != new_port->name)
            diff.renamed_ports.push_back(old_port.id);
        auto old_without_name = old_port;
        auto new_without_name = *new_port;
        old_without_name.name.clear();
        new_without_name.name.clear();
        if (!port_equal(old_without_name, new_without_name))
            diff.changed_ports.push_back(old_port.id);
    }
    for (const auto& new_port : next.ports)
        if (!find_by_id<PortDescriptor>(before.ports, new_port.id, [](const auto& port) { return port.id; }))
            diff.added_ports.push_back(new_port.id);

    for (const auto& old_parameter : before.parameters) {
        const auto* new_parameter =
            find_by_id<ParameterDescriptor>(next.parameters, old_parameter.id, [](const auto& parameter) {
                return parameter.id;
            });
        if (!new_parameter) {
            diff.removed_parameters.push_back(old_parameter.id);
            continue;
        }
        if (old_parameter.name != new_parameter->name)
            diff.renamed_parameters.push_back(old_parameter.id);
        if (!parameter_contract_equal(old_parameter, *new_parameter))
            diff.changed_parameters.push_back(old_parameter.id);
    }
    for (const auto& new_parameter : next.parameters)
        if (!find_by_id<ParameterDescriptor>(before.parameters, new_parameter.id,
                                             [](const auto& parameter) { return parameter.id; }))
            diff.added_parameters.push_back(new_parameter.id);

    const bool destructive = !diff.removed_ports.empty() || !diff.removed_parameters.empty() ||
                             !diff.changed_ports.empty() || !diff.changed_parameters.empty();
    if (destructive)
        diff.version_level = NodeContractVersionLevel::kMajor;
    else if (!diff.added_ports.empty() || !diff.added_parameters.empty())
        diff.version_level = NodeContractVersionLevel::kMinor;
    else if (!diff.renamed_ports.empty() || !diff.renamed_parameters.empty() || before.version != next.version)
        diff.version_level = NodeContractVersionLevel::kPatch;

    std::vector<uint64_t> affected_ports = diff.removed_ports;
    affected_ports.insert(affected_ports.end(), diff.changed_ports.begin(), diff.changed_ports.end());
    const auto port_affected = [&](uint64_t id) { return contains(affected_ports, id); };
    for (const auto& usage : usages) {
        if (!destructive)
            continue;
        if (!usage.edges.empty()) {
            for (const auto& edge : usage.edges) {
                if (!port_affected(edge.port_id))
                    continue;
                append_unique(diff.affected_edge_ids, edge.edge_id);
                append_unique(diff.affected_binding_ids, edge.binding_id);
                diff.impacts.push_back(
                    {usage.document_path, usage.node_id, edge.edge_id, edge.port_id, 0, edge.binding_id,
                     usage.document_identity});
            }
        } else {
            // Legacy callers only provided aggregate identities. Keep that
            // behavior conservative so a breaking edit cannot silently miss
            // an edge while newer callers get exact per-port impact rows.
            for (const auto& edge_id : usage.edge_ids)
                append_unique(diff.affected_edge_ids, edge_id);
            for (const uint64_t binding_id : usage.binding_ids)
                if (contains(diff.removed_ports, binding_id) || contains(diff.changed_ports, binding_id))
                    append_unique(diff.affected_binding_ids, binding_id);
            for (const auto& edge_id : usage.edge_ids)
                diff.impacts.push_back(
                    {usage.document_path, usage.node_id, edge_id, 0, 0, 0, usage.document_identity});
        }
        if (!usage.bindings.empty()) {
            for (const auto& binding : usage.bindings) {
                if (!port_affected(binding.port_id))
                    continue;
                append_unique(diff.affected_binding_ids, binding.binding_id);
                diff.impacts.push_back(
                    {usage.document_path, usage.node_id, {}, binding.port_id, 0, binding.binding_id,
                     usage.document_identity});
            }
        }
    }
    diff.requires_confirmation = !diff.affected_edge_ids.empty() || !diff.affected_binding_ids.empty();
    return diff;
}

}  // namespace aeg::application

\n