#include "editor_feature_definition_fixture.hpp"
#include "editor_custom_node_workflow.hpp"
#include "editor_node_contract_storage.hpp"
#include "feature_contract_json.hpp"
#include <iostream>
int main(int argc, char** argv) {
    using namespace aeg;
    const bool generate = argc == 3 && std::string_view(argv[1]) == "--generate";
    const auto root = generate ? std::filesystem::path{argv[2]} : std::filesystem::temp_directory_path() / "aeg_flowgraph_custom_node_workflow_smoke";
    std::error_code error; std::filesystem::remove_all(root,error);
    auto draft = tests::feature_definition_draft(); draft.generated = true;
    const auto scaffold = [&](const application::NodeDefinitionDraft& value, std::string name) {
        application::CustomNodeScaffoldRequest request{}; request.project_root = root;
        request.namespace_name = "game::graph"; request.type_name = name;
        const auto result = application::scaffold_custom_node(request,value);
        if (!result) { std::cerr << result.result.message << "\n"; return false; }
        if (result.value.definition_id != value.entry.id || result.value.files.size() != 4 || result.value.implementation_target.path.empty() || result.value.registration_target.symbol.empty()) return false;
        for (const auto& file : result.value.files) if (!std::filesystem::is_regular_file(file)) return false;
        const auto read = application::read_editor_node_contract(result.value.files[3]);
        if (!read || read.value.draft.entry.owner_module != value.entry.owner_module ||
            feature::feature_contract_digest(read.value.draft.entry) != feature::feature_contract_digest(value.entry) ||
            read.value.draft.entry.contract.parameters != value.entry.contract.parameters ||
            read.value.draft.entry.contract.state_payload != value.entry.contract.state_payload) return false;
        if (tests::read_text(result.value.files[3]).find("\"catalog\"") != std::string::npos ||
            tests::read_text(result.value.files[2]).find("AEG_REGISTER_GAME_FEATURE") == std::string::npos ||
            tests::read_text(result.value.files[1]).find("decode_feature_contract_json") == std::string::npos) return false;
        if (application::scaffold_custom_node(request,value)) return false;
        request.overwrite = true; return static_cast<bool>(application::scaffold_custom_node(request,value));
    };
    auto composite = draft; composite.entry.contract.structure = feature::FeatureStructure::kComposite; composite.entry.contract.clock.work_budget_ns = 0;
    composite.entry.contract.children.push_back(feature::FeatureDefinitionId{"Game.Child"});
    if (!scaffold(composite,"EditorCompositeNode")) return 1;
    for (unsigned domain = 0; domain < 5; ++domain) {
        auto leaf = draft; leaf.entry.contract.domains = feature::to_mask(static_cast<feature::FeatureDomain>(domain));
        if (domain == 0) {
            leaf.entry.contract.execution = feature::ExecutionKind::kProcess; leaf.entry.contract.state = feature::StatePolicy::kProcessState;
            leaf.entry.contract.has_continuation = true; leaf.entry.contract.state_payload = {{7,2,1},65};
        }
        if (!scaffold(leaf,"EditorFeatureNode" + std::to_string(domain))) return 2;
    }
    application::CustomNodeScaffoldRequest invalid{}; invalid.project_root=root; invalid.type_name="Bad"; invalid.source_subdirectory="../outside";
    if (application::validate_custom_node_scaffold_request(invalid,draft)) return 3;
    invalid.source_subdirectory="Source"; invalid.type_name="bad-name";
    if (application::validate_custom_node_scaffold_request(invalid,draft)) return 4;
    invalid.type_name="class"; if (application::validate_custom_node_scaffold_request(invalid,draft)) return 5;
    invalid.type_name="Valid"; invalid.namespace_name="bad-namespace";
    if (application::validate_custom_node_scaffold_request(invalid,draft)) return 6;
    if (!generate) std::filesystem::remove_all(root,error);
    std::cout << "Feature-only custom node workflow passed\n";
}
