#include <filesystem>
#include <fstream>
#include <iostream>

#include "editor_composition_test_support.hpp"
#include "editor_node_contract_storage.hpp"
#include "editor_node_definition_service.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
}

int main() {
    using namespace aeg;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aeg_editor_node_definition_service_smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);

    tests::EditorCompositionFixture fixture;
    application::EditorCompositionDocument editor{fixture.document, fixture.catalog};
    application::EditorNodeDefinitionService service;

    const auto* original = editor.catalog().find({pipeline::AuthoringTypeKind::kFeature,
                                                  pipeline::FeatureId("Game.EditorFeature").value()});
    check(original != nullptr, "service fixture feature is missing");
    if (!original)
        return 1;
    const auto original_key = original->key;
    const auto original_display_name = original->display_name;

    application::NodeDefinitionDraft add_only{};
    add_only.entry = *original;
    add_only.entry.display_name = "Draft Feature";
    add_only.source = application::NodeContractSource::kEditorDraft;
    add_only.generated = false;
    application::NodeDefinitionSaveRequest draft_request{add_only, false, false};
    const auto draft_save = service.save(editor, draft_request, root);
    check(static_cast<bool>(draft_save) && draft_save.value.draft_saved, "Save Draft did not persist");
    check(editor.catalog().find(original_key)->display_name == original_display_name,
          "Save Draft changed the runtime catalog");
    const auto saved_contract = application::read_editor_node_contract(
        root / "Source" / "FlowGraph" / "EditorFeature.flowgraph.contract.json");
    check(static_cast<bool>(saved_contract) && !saved_contract.value.draft.generated,
          "Save Draft was marked as generated");

    add_only.generated = true;
    const auto generated_save = service.save(editor, {add_only, true, false}, root);
    check(static_cast<bool>(generated_save) && generated_save.value.generated_stub,
          "Save & Generate Stub did not complete");
    check(editor.catalog().find(original_key)->display_name == "Draft Feature",
          "generated save did not update the catalog");
    const auto sidecar_draft = root / "Source" / "FlowGraph" / "EditorFeature.flowgraph.draft.json";
    check(static_cast<bool>(service.save(editor, {add_only, false, false}, root)) &&
              std::filesystem::exists(sidecar_draft),
          "Save Draft did not create a sidecar over a generated contract");
    check(static_cast<bool>(service.save(editor, {add_only, true, false}, root)) &&
              !std::filesystem::exists(sidecar_draft),
          "Save & Generate Stub left a stale draft sidecar");

    auto removed = add_only;
    removed.entry.ports.erase(removed.entry.ports.begin());
    const auto before_failed = editor.snapshot();
    const auto rejected = service.save(editor, {removed, true, false}, root);
    check(!rejected && editor.snapshot() == before_failed,
          "breaking save without disconnect confirmation changed the document");

    const auto disconnected = service.save(editor, {removed, true, true}, root);
    check(static_cast<bool>(disconnected) && editor.snapshot()->edges.empty(),
          "confirmed breaking save did not disconnect the affected edge");
    check(static_cast<bool>(editor.undo()) && !editor.snapshot()->edges.empty() &&
              editor.catalog().find(original_key)->ports.size() == 1,
          "undo did not restore the contract and edge together");
    check(static_cast<bool>(editor.redo()) && editor.snapshot()->edges.empty() &&
              editor.catalog().find(original_key)->ports.empty(),
          "redo did not reapply the contract and edge removal");

    auto readonly = add_only;
    readonly.source = application::NodeContractSource::kCompiled;
    check(!service.save(editor, {readonly, false, false}, root), "compiled node accepted an editor save");
    auto invalid_schema = add_only;
    invalid_schema.entry.ports.front().schema_id = 998877;
    check(!application::validate_node_definition(invalid_schema), "unknown schema was accepted");
    check(static_cast<bool>(service.save(editor, {removed, false, false}, root)),
          "Save Draft incorrectly required a disconnect confirmation");

    tests::EditorCompositionFixture precise_fixture;
    precise_fixture.document.input_bindings = {{.node_id="feature", .port_id=22},
                                                {.node_id="other-node", .port_id=22}};
    application::EditorCompositionDocument precise{precise_fixture.document, precise_fixture.catalog};
    check(static_cast<bool>(service.save(precise, {removed, true, true}, root)) &&
              precise.snapshot()->input_bindings.size() == 1 &&
              precise.snapshot()->input_bindings.front().node_id == "other-node",
          "save removed another node's binding with the same PortId");

    tests::EditorCompositionFixture failed_fixture;
    application::EditorCompositionDocument failed_editor{failed_fixture.document, failed_fixture.catalog};
    const auto failed_before = failed_editor.snapshot();
    application::EditorNodeDefinitionService failed_service;
    const fs::path blocked_root = root / "blocked-root";
    { std::ofstream blocked(blocked_root, std::ios::binary); blocked << "file"; }
    const auto failed = failed_service.save(failed_editor, {add_only, true, false}, blocked_root);
    check(!failed && failed_editor.snapshot() == failed_before,
          "generator failure did not leave the document unchanged");

    fs::remove_all(root, error);
    if (failures)
        return 1;
    std::cout << "Editor node definition service smoke passed\n";
    return 0;
}
