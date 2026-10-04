#include <filesystem>
#include <fstream>
#include <iostream>

#include "editor_composition_test_support.hpp"
#include "editor_node_contract_storage.hpp"
#include "editor_project_pipeline_authoring.hpp"
#include "module_bundle.hpp"

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
    const fs::path root = fs::temp_directory_path() / "aeg_editor_node_contract_storage_smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / "Source" / "FlowGraph", error);
    fs::create_directories(root / "Assets", error);

    tests::EditorCompositionFixture fixture;
    const auto* original = fixture.catalog.find({pipeline::AuthoringTypeKind::kFeature,
                                                  pipeline::FeatureId("Game.EditorFeature").value()});
    check(original != nullptr, "storage fixture feature is missing");
    if (!original)
        return 1;

    application::NodeContractFile file{};
    file.path = root / "Source" / "FlowGraph" / "EditorFeature.flowgraph.contract.json";
    file.draft.entry = *original;
    file.draft.generated = true;
    file.draft.draft_revision_hash = "draft-revision-1";
    file.draft.retired_port_ids = {99, 77};
    file.draft.source = application::NodeContractSource::kEditorDraft;
    check(static_cast<bool>(application::write_editor_node_contract_atomic(file, root)),
          "valid node contract did not write");
    const auto read = application::read_editor_node_contract(file.path);
    check(static_cast<bool>(read) && read.value.draft.entry.key == file.draft.entry.key &&
              read.value.draft.entry.ports.size() == file.draft.entry.ports.size() &&
              read.value.draft.retired_port_ids.size() == 2 && read.value.draft.generated,
          "node contract did not round-trip");
    const auto scan = application::scan_editor_node_contracts(root);
    check(static_cast<bool>(scan) && scan.value.size() == 1 && scan.value.front().path == file.path,
          "contract scan was not deterministic");

    const fs::path invalid_path = root / "Source" / "FlowGraph" / "Invalid.flowgraph.contract.json";
    { std::ofstream invalid(invalid_path, std::ios::binary); invalid << "{ invalid"; }
    const auto invalid_read = application::read_editor_node_contract(invalid_path);
    check(!invalid_read, "invalid contract JSON was accepted");

    module::ModuleMetadata metadata{};
    metadata.module = {original->owner_module, "Game Module", {1, 0, 0}, {}};
    metadata.descriptor = metadata.module;
    metadata.build_id = "compiled-build";
    metadata.target = "windows-x64-vulkan";
    metadata.configuration = "Debug";
    check(static_cast<bool>(metadata.catalog.add(*original)), "compiled fixture catalog did not add");
    const auto encoded = module::encode_module_metadata(metadata);
    check(static_cast<bool>(encoded), "compiled metadata did not encode");
    { std::ofstream output(root / "Source" / "GameModule.metadata.json", std::ios::binary); output << encoded.value; }

    application::NodeContractFile draft_only{};
    draft_only.path = root / "Source" / "FlowGraph" / "DraftOnly.flowgraph.draft.json";
    draft_only.draft.entry = *original;
    draft_only.draft.entry.key.id = pipeline::FeatureId("Game.DraftOnly").value();
    draft_only.draft.entry.name = "Game.DraftOnly";
    draft_only.draft.entry.display_name = "Draft Only";
    draft_only.draft.generated = false;
    draft_only.draft.source = application::NodeContractSource::kEditorDraft;
    check(static_cast<bool>(application::write_editor_node_contract_atomic(draft_only, root)),
          "draft-only node contract did not write");

    const auto loaded = application::load_editor_project_pipeline_authoring(root, root / "Source", root / "Assets");
    check(static_cast<bool>(loaded) && loaded.value.contracts.size() == 1 && loaded.value.conflicts.size() == 1,
          "project loader did not preserve draft/compiled conflict");
    if (loaded) {
        const auto* compiled = loaded.value.catalog.find(original->key);
        check(compiled != nullptr && compiled->display_name == original->display_name,
              "compiled contract did not remain authoritative");
        check(loaded.value.drafts.size() == 1 &&
                  loaded.value.catalog.find(draft_only.draft.entry.key) == nullptr,
              "draft-only contract incorrectly entered the runtime catalog");
        check(!loaded.value.contract_diagnostics.empty(), "invalid contract diagnostic was not reported");
    }

    fs::remove_all(root, error);
    if (failures)
        return 1;
    std::cout << "Editor node contract storage smoke passed\n";
    return 0;
}

\n