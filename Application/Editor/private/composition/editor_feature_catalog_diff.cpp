#include "editor_project_feature_authoring.hpp"

#include <algorithm>

namespace aeg::application {
namespace {
[[nodiscard]] const feature::FeatureCatalogEntry* find_catalog_entry(
    const feature::FeatureCatalog& catalog, feature::FeatureDefinitionId id) {
    const auto found = std::find_if(catalog.entries().begin(), catalog.entries().end(),
        [id](const auto& entry) { return entry.definition_id == id; });
    return found == catalog.entries().end() ? nullptr : &*found;
}

[[nodiscard]] bool side_effects_equal(const std::vector<feature::SideEffectContract>& left,
                                      const std::vector<feature::SideEffectContract>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].id != right[index].id || left[index].version != right[index].version ||
            left[index].kind != right[index].kind || left[index].commit != right[index].commit)
            return false;
    }
    return true;
}

template <typename Entry, typename Id, typename IdOf>
void compare_contract_items(const std::vector<Entry>& before, const std::vector<Entry>& after, IdOf id_of,
                            std::vector<Id>& added, std::vector<Id>& removed, std::vector<Id>& changed) {
    for (const auto& old_item : before) {
        const auto found = std::find_if(after.begin(), after.end(), [&](const auto& item) {
            return id_of(item) == id_of(old_item);
        });
        if (found == after.end()) removed.push_back(id_of(old_item));
        else if (old_item != *found) changed.push_back(id_of(old_item));
    }
    for (const auto& new_item : after) {
        if (std::none_of(before.begin(), before.end(), [&](const auto& item) {
                return id_of(item) == id_of(new_item);
            })) added.push_back(id_of(new_item));
    }
}

[[nodiscard]] std::vector<std::string> changed_contract_fields(
    const feature::FeatureCatalogEntry& before, const feature::FeatureCatalogEntry& after) {
    std::vector<std::string> fields;
    if (before.version != after.version) fields.emplace_back("version");
    if (before.owner_module != after.owner_module) fields.emplace_back("owner_module");
    if (before.domains != after.domains) fields.emplace_back("domains");
    if (before.execution != after.execution) fields.emplace_back("execution");
    if (before.state != after.state) fields.emplace_back("state");
    if (before.clock != after.clock || !feature::same_clock_schedule(before.contract.clock, after.contract.clock) ||
        before.contract.clock.work_budget_ns != after.contract.clock.work_budget_ns)
        fields.emplace_back("clock");
    if (before.ports != after.ports) fields.emplace_back("ports");
    if (before.contract.parameters != after.contract.parameters) fields.emplace_back("parameters");
    if (before.contract.structure != after.contract.structure) fields.emplace_back("structure");
    if (before.contract.scope.id != after.contract.scope.id) fields.emplace_back("scope");
    if (before.contract.failure != after.contract.failure) fields.emplace_back("failure_policy");
    if (before.contract.retry.max_attempts != after.contract.retry.max_attempts ||
        before.contract.retry.backoff_ticks != after.contract.retry.backoff_ticks)
        fields.emplace_back("retry_policy");
    if (before.contract.permissions.capability_mask != after.contract.permissions.capability_mask ||
        before.contract.permissions.version != after.contract.permissions.version)
        fields.emplace_back("permissions");
    if (!side_effects_equal(before.contract.side_effects, after.contract.side_effects))
        fields.emplace_back("side_effects");
    if (before.contract.child_port_mappings != after.contract.child_port_mappings ||
        before.contract.children != after.contract.children ||
        before.contract.allows_child_process != after.contract.allows_child_process)
        fields.emplace_back("composition");
    if (before.contract.has_continuation != after.contract.has_continuation)
        fields.emplace_back("continuation");
    if (before.contract.state_payload != after.contract.state_payload)
        fields.emplace_back("state_payload");
    return fields;
}

}  // namespace

FeatureCatalogDiff diff_feature_catalogs(
    const feature::FeatureCatalog& before, const feature::FeatureCatalog& after,
    std::span<const EditorProjectFeatureGraphDocument> documents) {
    FeatureCatalogDiff result{};
    result.before_digest = before.contract_digest();
    result.after_digest = after.contract_digest();
    std::vector<feature::FeatureDefinitionId> identities;
    identities.reserve(before.entries().size() + after.entries().size());
    for (const auto& entry : before.entries()) identities.push_back(entry.definition_id);
    for (const auto& entry : after.entries())
        if (std::find(identities.begin(), identities.end(), entry.definition_id) == identities.end())
            identities.push_back(entry.definition_id);
    std::sort(identities.begin(), identities.end(), [](auto left, auto right) {
        return left.value() < right.value();
    });

    for (const auto id : identities) {
        const auto* old_entry = find_catalog_entry(before, id);
        const auto* new_entry = find_catalog_entry(after, id);
        if (old_entry != nullptr && new_entry != nullptr &&
            old_entry->contract_digest == new_entry->contract_digest)
            continue;
        FeatureCatalogEntryChange change{};
        change.definition_id = id;
        if (old_entry == nullptr) {
            change.kind = FeatureCatalogChangeKind::kAdded;
            change.after_version = new_entry->version;
            change.after_digest = new_entry->contract_digest;
            change.changed_fields.emplace_back("definition");
            compare_contract_items<feature::PortContract, feature::PortId>(
                {}, new_entry->ports, [](const auto& item) { return item.id; }, change.added_ports,
                change.removed_ports, change.changed_ports);
            compare_contract_items<feature::ParameterContract, feature::ParameterId>(
                {}, new_entry->contract.parameters, [](const auto& item) { return item.id; },
                change.added_parameters, change.removed_parameters, change.changed_parameters);
        } else if (new_entry == nullptr) {
            change.kind = FeatureCatalogChangeKind::kRemoved;
            change.before_version = old_entry->version;
            change.before_digest = old_entry->contract_digest;
            change.changed_fields.emplace_back("definition");
            compare_contract_items<feature::PortContract, feature::PortId>(
                old_entry->ports, {}, [](const auto& item) { return item.id; }, change.added_ports,
                change.removed_ports, change.changed_ports);
            compare_contract_items<feature::ParameterContract, feature::ParameterId>(
                old_entry->contract.parameters, {}, [](const auto& item) { return item.id; },
                change.added_parameters, change.removed_parameters, change.changed_parameters);
        } else {
            change.kind = FeatureCatalogChangeKind::kChanged;
            change.before_version = old_entry->version;
            change.after_version = new_entry->version;
            change.before_digest = old_entry->contract_digest;
            change.after_digest = new_entry->contract_digest;
            change.changed_fields = changed_contract_fields(*old_entry, *new_entry);
            if (change.changed_fields.empty()) change.changed_fields.emplace_back("contract");
            compare_contract_items<feature::PortContract, feature::PortId>(
                old_entry->ports, new_entry->ports, [](const auto& item) { return item.id; }, change.added_ports,
                change.removed_ports, change.changed_ports);
            compare_contract_items<feature::ParameterContract, feature::ParameterId>(
                old_entry->contract.parameters, new_entry->contract.parameters,
                [](const auto& item) { return item.id; }, change.added_parameters,
                change.removed_parameters, change.changed_parameters);
        }
        result.changes.push_back(std::move(change));
    }

    for (const auto& document : documents) {
        if (!document.load_result) continue;
        FeatureCatalogGraphImpact impact{};
        impact.path = document.path;
        impact.asset_uuid = document.graph.asset_uuid;
        for (const auto& change : result.changes) {
            const bool used = std::any_of(document.graph.nodes.begin(), document.graph.nodes.end(),
                [&](const auto& node) { return node.definition_id == change.definition_id; });
            if (!used) continue;
            impact.definitions.push_back(change.definition_id);
            for (const auto& node : document.graph.nodes)
                if (node.definition_id == change.definition_id &&
                    std::find(impact.node_ids.begin(), impact.node_ids.end(), node.node_id) == impact.node_ids.end())
                    impact.node_ids.push_back(node.node_id);
        }
        if (impact.node_ids.empty()) continue;
        for (const auto& edge : document.graph.edges) {
            if (std::find(impact.node_ids.begin(), impact.node_ids.end(), edge.source_node) != impact.node_ids.end() ||
                std::find(impact.node_ids.begin(), impact.node_ids.end(), edge.target_node) != impact.node_ids.end())
                impact.edges.push_back(edge);
        }
        result.impacts.push_back(std::move(impact));
    }
    return result;
}

}  // namespace aeg::application
