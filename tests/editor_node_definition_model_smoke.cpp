#include "editor_feature_definition_fixture.hpp"
#include <iostream>
int main() {
    using namespace aeg;
    auto draft = tests::feature_definition_draft();
    if (const auto valid = application::validate_node_definition(draft); !valid) { std::cerr << valid.message << "\n"; return 1; }
    auto renamed = draft; renamed.port_labels[0].name = "Renamed";
    auto diff = application::diff_node_definition(draft, renamed, {});
    if (diff.requires_confirmation || diff.renamed_ports != std::vector<uint64_t>{11}) return 2;
    auto reordered = draft; std::swap(reordered.entry.contract.ports[0], reordered.entry.contract.ports[1]);
    if (!application::diff_node_definition(draft, reordered, {}).changed_ports.empty()) return 3;
    auto removed = draft; removed.entry.contract.ports.erase(removed.entry.contract.ports.begin()); removed.port_labels.erase(removed.port_labels.begin());
    application::NodeContractUsage usage{}; usage.node_id = feature::FeatureNodeId{102}; usage.edges.push_back({"edge",11,0});
    diff = application::diff_node_definition(draft, removed, std::span(&usage,1));
    if (!diff.requires_confirmation || diff.removed_ports != std::vector<uint64_t>{11} || diff.affected_edge_ids != std::vector<std::string>{"edge"}) return 4;
    auto invalid = draft; invalid.entry.contract.ports[0].id = invalid.entry.contract.ports[1].id;
    if (application::validate_node_definition(invalid)) return 5;
    invalid = draft; invalid.entry.contract.parameters[0].metadata->default_value = 3.0;
    if (application::validate_node_definition(invalid)) return 6;
    auto partial = draft; partial.entry.contract.structure = feature::FeatureStructure::kComposite; partial.entry.contract.clock.work_budget_ns = 0;
    if (!application::validate_node_definition(partial)) {
        std::cerr << "incomplete Composite cannot be saved as an editor draft\n"; return 40;
    }
    invalid = draft;
    invalid.entry.contract.clock = {draft.entry.contract.clock.id, 60, 8, feature::LateDataPolicy::kQueue};
    if (application::validate_node_definition(invalid)) return 41;
    for (unsigned field = 0; field != 4; ++field) {
        auto changed = draft;
        auto& clock = changed.entry.contract.clock;
        if (field == 0) clock.capability_version.minor = 1;
        if (field == 1) clock.timing->phase_ns = 11;
        if (field == 2) clock.timing->deadline_policy = feature::DeadlinePolicy::kRejectCommit;
        if (field == 3) ++clock.work_budget_ns;
        const auto change = application::diff_node_definition(draft, changed, std::span(&usage, 1));
        if (change.version_level != application::NodeContractVersionLevel::kMajor || !change.requires_confirmation ||
            change.affected_edge_ids != std::vector<std::string>{"edge"}) return 42;
    }
    std::cout << "Feature definition model passed\n";
}
