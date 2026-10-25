#include "editor_node_definition.hpp"
#include "feature_contract_json.hpp"
#include <algorithm>
#include <unordered_set>

namespace aeg::application {
namespace {
template<class T, class Id> const T* find_id(const std::vector<T>& values, Id id) {
    auto found = std::find_if(values.begin(), values.end(), [id](const T& value) { return value.id == id; });
    return found == values.end() ? nullptr : &*found;
}
bool contains(const std::vector<uint64_t>& values, uint64_t id) {
    return std::find(values.begin(), values.end(), id) != values.end();
}
void append_unique(std::vector<std::string>& values, const std::string& value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}
}
NodeDefinitionDraft node_definition_from_catalog(const feature::FeatureCatalogEntry& entry) {
    NodeDefinitionDraft result{};
    result.entry.id = entry.definition_id; result.entry.version = entry.version;
    result.entry.owner_module = entry.owner_module; result.entry.contract = entry.contract;
    result.name = "Feature" + std::to_string(entry.definition_id.value());
    result.display_name = result.name;
    result.source = NodeContractSource::kCompiled;
    return result;
}
foundation::Result validate_node_definition(const NodeDefinitionDraft& draft) {
    const auto& definition = draft.entry;
    if (!definition.owner_module.is_valid() || definition.id != definition.contract.definition_id ||
        definition.version != definition.contract.version)
        return foundation::invalid_argument("Feature draft identity, version and owner module must be consistent");
    if (const auto valid = feature::validate_feature_contract_draft(definition.contract); !valid) return valid;
    std::unordered_set<uint64_t> ports, parameters;
    for (auto id : draft.retired_port_ids)
        if (!id || !ports.insert(id).second || find_id(definition.contract.ports, feature::PortId{id}))
            return foundation::invalid_argument("retired port id must be unique and absent from the draft");
    for (auto id : draft.retired_parameter_ids)
        if (!id || !parameters.insert(id).second || find_id(definition.contract.parameters, feature::ParameterId{id}))
            return foundation::invalid_argument("retired parameter id must be unique and absent from the draft");
    std::unordered_set<uint64_t> labels;
    for (const auto& label : draft.port_labels)
        if (!find_id(definition.contract.ports, label.id) || !labels.insert(label.id.value()).second)
            return foundation::invalid_argument("port labels must reference unique live Feature ports");
    // The structural codec also rejects invalid enum representations and malformed typed metadata.
    const auto json = feature::encode_feature_contract_json(definition.contract);
    if (!json) return json.result;
    const auto parsed = feature::decode_feature_contract_json(json.value);
    return parsed.result;
}
NodeContractDiff diff_node_definition(const NodeDefinitionDraft& before, const NodeDefinitionDraft& after,
                                     std::span<const NodeContractUsage> usages) {
    NodeContractDiff diff{};
    const auto& old = before.entry.contract; const auto& next = after.entry.contract;
    for (const auto& port : old.ports) {
        const auto* replacement = find_id(next.ports, port.id);
        if (!replacement) diff.removed_ports.push_back(port.id.value());
        else if (replacement->direction != port.direction || replacement->schema != port.schema || replacement->version != port.version)
            diff.changed_ports.push_back(port.id.value());
        const auto* old_label = find_id(before.port_labels, port.id);
        const auto* new_label = find_id(after.port_labels, port.id);
        if (replacement && ((old_label ? old_label->name : "") != (new_label ? new_label->name : "")))
            diff.renamed_ports.push_back(port.id.value());
    }
    for (const auto& port : next.ports)
        if (!find_id(old.ports, port.id)) diff.added_ports.push_back(port.id.value());
    for (const auto& parameter : old.parameters) {
        const auto* replacement = find_id(next.parameters, parameter.id);
        if (!replacement) diff.removed_parameters.push_back(parameter.id.value());
        else {
            if (parameter.metadata && replacement->metadata && parameter.metadata->display_name != replacement->metadata->display_name)
                diff.renamed_parameters.push_back(parameter.id.value());
            if (parameter.version != replacement->version || parameter.max_size != replacement->max_size ||
                parameter.required != replacement->required || parameter.metadata.has_value() != replacement->metadata.has_value() ||
                (parameter.metadata && feature::parameter_metadata_digest(*parameter.metadata) != feature::parameter_metadata_digest(*replacement->metadata)))
                diff.changed_parameters.push_back(parameter.id.value());
        }
    }
    for (const auto& parameter : next.parameters)
        if (!find_id(old.parameters, parameter.id)) diff.added_parameters.push_back(parameter.id.value());
    auto old_semantics = old; auto new_semantics = next;
    old_semantics.ports.clear(); new_semantics.ports.clear(); old_semantics.parameters.clear(); new_semantics.parameters.clear();
    old_semantics.version = {}; new_semantics.version = {};
    auto old_definition = before.entry; auto new_definition = after.entry;
    old_definition.contract = old_semantics; new_definition.contract = new_semantics;
    old_definition.version = {}; new_definition.version = {};
    const bool changed_semantics = feature::feature_contract_digest(old_definition) != feature::feature_contract_digest(new_definition);
    const bool destructive = changed_semantics || !diff.removed_ports.empty() || !diff.changed_ports.empty() ||
        !diff.removed_parameters.empty() || !diff.changed_parameters.empty();
    if (destructive) diff.version_level = NodeContractVersionLevel::kMajor;
    else if (!diff.added_ports.empty() || !diff.added_parameters.empty()) diff.version_level = NodeContractVersionLevel::kMinor;
    else if (!diff.renamed_ports.empty() || !diff.renamed_parameters.empty() || before.display_name != after.display_name || before.entry.version != after.entry.version)
        diff.version_level = NodeContractVersionLevel::kPatch;
    for (const auto& usage : usages) {
        if (!destructive) continue;
        for (const auto& edge : usage.edges) {
            if (!changed_semantics && !contains(diff.removed_ports, edge.port_id) && !contains(diff.changed_ports, edge.port_id)) continue;
            append_unique(diff.affected_edge_ids, edge.edge_id);
            diff.impacts.push_back({usage.document_path, usage.node_id, edge.edge_id, edge.port_id, 0, edge.edge_index, usage.document_identity});
        }
        diff.requires_confirmation = true;
    }
    return diff;
}
}  // namespace aeg::application
