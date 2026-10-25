#include "editor_feature_definition_fixture.hpp"
#include "editor_node_contract_storage.hpp"
#include "editor_project_feature_authoring.hpp"
#include "editor_node_definition_service.hpp"
#include "editor_feature_implementation_resolver.hpp"
#include "project_context.hpp"
#include "feature_contract_json.hpp"
#include "module_bundle.hpp"
#include <algorithm>
#include <iostream>
int main() {
    using namespace aeg;
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_node_contract_storage_smoke";
    std::error_code error; std::filesystem::remove_all(root,error);
    std::filesystem::create_directories(root / "Source/FlowGraph"); std::filesystem::create_directories(root / "Assets");
    auto draft = tests::feature_definition_draft(); draft.generated=true; draft.draft_revision_hash="revision1";
    draft.retired_port_ids={99,77}; draft.retired_parameter_ids={88,66};
    draft.entry.contract.execution=feature::ExecutionKind::kProcess; draft.entry.contract.state=feature::StatePolicy::kProcessState;
    draft.entry.contract.has_continuation=true; draft.entry.contract.state_payload={{7,2,1},65};
    application::NodeContractFile file{root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json",draft,{},draft.source};
    if (!application::write_editor_node_contract_atomic(file,root)) return 1;
    const auto read=application::read_editor_node_contract(file.path);
    if (!read || read.value.draft.source != draft.source || !read.value.draft.generated ||
        read.value.draft.draft_revision_hash != draft.draft_revision_hash || read.value.draft.retired_port_ids != draft.retired_port_ids ||
        read.value.draft.retired_parameter_ids != draft.retired_parameter_ids || read.value.draft.entry.contract.parameters != draft.entry.contract.parameters ||
        read.value.draft.entry.contract.state_payload != draft.entry.contract.state_payload ||
        feature::encode_feature_contract_json(read.value.draft.entry.contract).value != feature::encode_feature_contract_json(draft.entry.contract).value) return 2;
    const auto scan=application::scan_editor_node_contracts(root);
    if (!scan || scan.value.size()!=1 || scan.value.front().path!=file.path) return 3;
    const auto current_bytes = tests::read_text(file.path);
    const auto outer_version = current_bytes.find("\"schema_version\": 3");
    if (outer_version == std::string::npos || current_bytes.find("\"schema_version\": 2") == std::string::npos) return 11;
    auto old_wrapper = current_bytes;
    old_wrapper.replace(outer_version, std::string("\"schema_version\": 3").size(), "\"schema_version\": 2");
    { std::ofstream out(file.path, std::ios::binary); out << old_wrapper; }
    if (application::read_editor_node_contract(file.path)) return 12;
    { std::ofstream out(file.path, std::ios::binary); out << current_bytes; }
    auto tampered=tests::read_text(file.path); const auto offset=tampered.find("revision1"); tampered.replace(offset,9,"revision2");
    { std::ofstream out(file.path); out << tampered; }
    if (application::read_editor_node_contract(file.path)) return 4;
    if (!application::write_editor_node_contract_atomic(file,root)) return 5;
    { std::ofstream out(root / "Source/FlowGraph/Invalid.flowgraph.contract.json"); out << "{ invalid"; }
    module::ModuleMetadata metadata{}; metadata.module={draft.entry.owner_module,"Fixture",{1,0,0},{}};
    if (!metadata.feature_catalog.export_definition(draft.entry)) return 6;
    const auto encoded=module::encode_module_metadata(metadata);
    if (!encoded) return 7;
    { std::ofstream out(root / "Source/GameModule.metadata.json"); out << encoded.value; }
    auto partial=draft; partial.generated=false; partial.name="Game.DraftOnly"; partial.entry.id=feature::FeatureDefinitionId{"Game.DraftOnly"};
    partial.entry.contract.definition_id=partial.entry.id; partial.entry.contract.structure=feature::FeatureStructure::kComposite; partial.entry.contract.clock.work_budget_ns = 0;
    partial.entry.contract.execution=feature::ExecutionKind::kCall; partial.entry.contract.state=feature::StatePolicy::kStateless;
    partial.entry.contract.state_payload={}; partial.entry.contract.has_continuation=false;
    application::NodeContractFile draft_file{root / "Source/FlowGraph/DraftOnly.flowgraph.draft.json",partial,{},partial.source};
    if (!application::write_editor_node_contract_atomic(draft_file,root) || !application::read_editor_node_contract(draft_file.path)) return 8;
    const auto loaded=application::load_editor_project_feature_authoring(root,root / "Source",root / "Assets");
    if (!loaded || loaded.value.contracts.size()!=1 || !loaded.value.conflicts.empty() || loaded.value.drafts.size()!=1 ||
        loaded.value.presentations.size()!=1 || loaded.value.presentations.front().display_name!=draft.display_name ||
        loaded.value.feature_catalog.entries().size()!=1 || loaded.value.feature_catalog.entries().front().definition_id!=draft.entry.id ||
        loaded.value.feature_catalog.entries().front().contract.parameters != draft.entry.contract.parameters || loaded.value.contract_diagnostics.empty()) return 9;
    // Only labels may accompany an identical metadata contract; changed
    // contracts still produce conflicts and cannot replace catalog authority.
    auto conflicting = file;
    ++conflicting.draft.entry.contract.state_payload.max_size;
    if (!application::write_editor_node_contract_atomic(conflicting, root)) return 13;
    const auto conflicted = application::load_editor_project_feature_authoring(root, root / "Source", root / "Assets");
    if (!conflicted || conflicted.value.conflicts.size()!=1 || !conflicted.value.presentations.empty() ||
        conflicted.value.feature_catalog.contract_digest()!=loaded.value.feature_catalog.contract_digest()) return 14;
    // A configured source directory is the sole authoring authority. Obsolete
    // Source sidecars (including its malformed file above) cannot leak into
    // Native metadata, and new Native-only generated contracts must be found.
    auto native = draft;
    native.entry.id = feature::FeatureDefinitionId{"Game.Native"};
    native.entry.contract.definition_id = native.entry.id;
    native.name = "Game.Native"; native.display_name = "Native contract";
    application::NodeContractFile native_file{root / "Native/FlowGraph/Native.flowgraph.contract.json", native, {}, native.source};
    if (!application::write_editor_node_contract_atomic(native_file, root)) return 15;
    module::ModuleMetadata native_metadata{};
    native_metadata.module = {native.entry.owner_module, "Native", {1,0,0}, {}};
    if (!native_metadata.feature_catalog.export_definition(native.entry)) return 16;
    const auto native_json = module::encode_module_metadata(native_metadata);
    if (!native_json) return 17;
    { std::ofstream out(root / "Native/GameModule.metadata.json"); out << native_json.value; }
    auto native_only = native_file;
    native_only.path = root / "Native/FlowGraph/GeneratedOnly.flowgraph.contract.json";
    native_only.draft.entry.id = feature::FeatureDefinitionId{"Game.Native.GeneratedOnly"};
    native_only.draft.entry.contract.definition_id = native_only.draft.entry.id;
    native_only.draft.name = "Game.Native.GeneratedOnly";
    if (!application::write_editor_node_contract_atomic(native_only, root)) return 18;
    const auto configured = application::load_editor_project_feature_authoring(root, root / "Native", root / "Assets");
    if (!configured || configured.value.feature_catalog.entries().size()!=2 ||
        configured.value.compiled_feature_ids != std::vector<feature::FeatureDefinitionId>{native.entry.id} ||
        configured.value.presentations.size()!=2 || !configured.value.conflicts.empty() ||
        !configured.value.contract_diagnostics.empty() ||
        std::none_of(configured.value.feature_catalog.entries().begin(), configured.value.feature_catalog.entries().end(),
            [&](const auto& entry) { return entry.definition_id == native_only.draft.entry.id; }) ||
        std::any_of(configured.value.feature_catalog.entries().begin(), configured.value.feature_catalog.entries().end(),
            [&](const auto& entry) { return entry.definition_id == draft.entry.id; })) {
        std::cerr << "Configured Native authoring fell back to obsolete Source sidecars\n";
        return 19;
    }
    application::project::ProjectConfig project_config;
    project_config.name = "Configured source";
    project_config.module.id = "com.aeg.editor.fixture";
    project_config.paths.source = "Native";
    const auto selected_project = root / "Selected.aegproj";
    if (!application::project::ProjectFileCodec{}.save(selected_project, project_config)) return 20;
    auto other_config = project_config;
    other_config.paths.source = "Source";
    if (!application::project::ProjectFileCodec{}.save(root / "Other.aegproj", other_config)) return 21;
    // Save Draft, Generate, and implementation navigation must use the same
    // exact selected project's source directory even when roots are ambiguous.
    auto saved_native = native;
    saved_native.entry.id = feature::FeatureDefinitionId{"Game.NativeSaved"};
    saved_native.entry.contract.definition_id = saved_native.entry.id;
    saved_native.name = "Game.NativeSaved";
    saved_native.generated = false;
    application::EditorFeatureGraphDocument native_document{tests::feature_graph(native.entry), tests::feature_catalog(native.entry)};
    application::EditorNodeDefinitionService service;
    application::NodeDefinitionSaveRequest save_request{saved_native, false, false};
    save_request.selected_project_file = selected_project;
    if (!service.save(native_document, save_request, root) ||
        !std::filesystem::exists(root / "Native/FlowGraph/NativeSaved.flowgraph.contract.json") ||
        std::filesystem::exists(root / "Source/FlowGraph/NativeSaved.flowgraph.contract.json")) return 22;
    save_request.generate_stub = true;
    if (!service.save(native_document, save_request, root)) return 23;
    const auto implementation = application::locate_feature_implementation(selected_project, saved_native.entry.id);
    if (!implementation || !std::filesystem::equivalent(implementation.value.path,
            root / "Native/FlowGraph/NativeSaved.flowgraph.cpp") ||
        application::locate_feature_implementation(root, saved_native.entry.id)) return 24;
    auto retired=draft; retired.retired_port_ids.push_back(11);
    if (application::validate_node_definition(retired)) return 10;
    std::filesystem::remove_all(root,error); std::cout << "Feature contract storage passed\n";
}
