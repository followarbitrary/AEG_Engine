#include "editor_feature_definition_fixture.hpp"
#include "editor_node_definition_service.hpp"
#include "editor_node_contract_storage.hpp"
#include "feature_graph_io.hpp"
#include <iostream>
int main() {
    using namespace aeg;
    const auto root=std::filesystem::temp_directory_path() / "aeg_editor_node_definition_end_to_end_smoke";
    std::error_code error; std::filesystem::remove_all(root,error); std::filesystem::create_directories(root / "Assets");
    auto draft=tests::feature_definition_draft();
    draft.entry.contract.execution=feature::ExecutionKind::kProcess; draft.entry.contract.state=feature::StatePolicy::kProcessState;
    draft.entry.contract.state_payload={{7,2,1},65}; draft.entry.contract.has_continuation=true;
    auto graph=tests::feature_graph(draft.entry); graph.edges.clear(); graph.nodes.resize(1);
    auto value=feature::serialize_parameter_value(foundation::ParameterValue{1.5},feature::ParameterType::kDouble);
    if (!value) return 1;
    const std::array records{feature::FeatureParameterRecord{feature::ParameterId{21},1,value.value}};
    const auto blob=feature::encode_feature_parameters(records,draft.entry.contract.parameters); if (!blob) return 2;
    graph.nodes[0].parameter_blob=blob.value;
    application::EditorFeatureGraphDocument editor{graph,tests::feature_catalog(draft.entry)};
    application::EditorNodeDefinitionService service;
    const auto saved=service.save(editor,{draft,true,false},root);
    if (!saved) { std::cerr << saved.result.message << "\n"; return 3; }
    if (!std::filesystem::exists(saved.value.generated.registration_target.path)) return 4;
    const auto contract=application::read_editor_node_contract(saved.value.generated.files[3]);
    if (!contract || contract.value.draft.entry.contract.parameters!=draft.entry.contract.parameters ||
        contract.value.draft.entry.contract.state_payload!=draft.entry.contract.state_payload) return 5;
    const auto path=root / "Assets/Graph.aegfeature";
    if (!application::authoring::save_feature_graph_document(path,editor.snapshot())) return 6;
    const auto loaded=application::authoring::load_feature_graph_document(path);
    if (!loaded || loaded.value.nodes.size()!=1 || loaded.value.nodes[0].definition_id!=draft.entry.id ||
        loaded.value.nodes[0].definition_version!=draft.entry.version || loaded.value.nodes[0].parameter_blob!=blob.value ||
        loaded.value.contract_digest!=editor.catalog().contract_digest()) return 7;
    const auto decoded=feature::decode_feature_parameters(loaded.value.nodes[0].parameter_blob,contract.value.draft.entry.contract.parameters);
    if (!decoded || decoded.value.size()!=1 || decoded.value[0].payload!=value.value || decoded.value[0].version!=1) return 8;
    std::filesystem::remove_all(root,error); std::cout << "Feature definition generate/save/reopen passed\n";
}
