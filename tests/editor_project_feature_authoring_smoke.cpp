#include "feature_definition_fixture.hpp"
#include <fstream>
#include <algorithm>
#include <array>
#include "editor_project_feature_authoring.hpp"
#include "feature_graph_io.hpp"
#include "module_bundle.hpp"
#include "project_scaffold.hpp"
#include "composition/editor_feature_graph_preview.hpp"
#include "GameModule.hpp"
#include <iostream>

namespace {
bool verify_fresh_project() {
    using namespace aeg;
    const auto root = std::filesystem::temp_directory_path() / "aeg_new_project_authoring_starter";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto created = application::project::create_project({root, "Authoring starter", "GameModule"});
    if (!created) { std::cerr << created.result.message << '\n'; return false; }
    const auto authored = application::load_editor_project_feature_authoring(root, root / "Source", root / "Assets");
    if (!authored || authored.value.feature_graphs.size() != 1 ||
        !authored.value.feature_graphs.front().load_result || !authored.value.contract_diagnostics.empty() ||
        authored.value.feature_catalog.entries().size() != 2 || !authored.value.conflicts.empty() ||
        authored.value.presentations.size() != 2) {
        std::cerr << "Fresh project did not load its offline catalog and starter FeatureGraph\n";
        return false;
    }
    const auto& graph = authored.value.feature_graphs.front().graph;
    if (graph.nodes.size() != 1 || graph.process_subgraphs.size() != 1) {
        std::cerr << "Fresh project opens an empty graph instead of the starter process\n";
        return false;
    }
    const auto gameplay_label = std::find_if(authored.value.presentations.begin(), authored.value.presentations.end(),
        [&](const auto& value) { return value.entry.id == graph.nodes.front().definition_id; });
    const auto outline_label = std::find_if(authored.value.presentations.begin(), authored.value.presentations.end(),
        [](const auto& value) { return value.entry.id == feature::FeatureDefinitionId{"Game.Render.Outline"}; });
    if (gameplay_label == authored.value.presentations.end() || gameplay_label->name != "Game.Logic.Gameplay" ||
        gameplay_label->display_name != "Gameplay starter" || outline_label == authored.value.presentations.end() ||
        outline_label->display_name != "Outline contract example" || outline_label->port_labels.size() != 2 ||
        outline_label->port_labels[0].id != feature::PortId{"SceneColor"} ||
        outline_label->port_labels[0].name != "Scene color" ||
        outline_label->port_labels[1].id != feature::PortId{"ColorOut"} ||
        outline_label->port_labels[1].name != "Color output") {
        std::cerr << "Starter definitions have no catalog-matched display names and port labels\n";
        return false;
    }
    const auto preview = application::EditorFeatureGraphPreviewCompiler{}.compile(graph, authored.value.feature_catalog);
    if (!preview || preview.value.has_errors() || preview.value.lanes.size() != 5 ||
        preview.value.lanes.front().node_ids != std::vector<feature::FeatureNodeId>{graph.nodes.front().node_id} ||
        preview.value.provenance.size() != 1) {
        std::cerr << "Fresh project cannot compile an authoring preview before building its module\n";
        if (preview) for (const auto& diagnostic : preview.value.diagnostics) std::cerr << diagnostic.message << '\n';
        return false;
    }
    if (!std::filesystem::is_empty(root / "Build") || !std::filesystem::is_empty(root / "Binaries")) {
        std::cerr << "Offline authoring unexpectedly produced an executable module\n";
        return false;
    }
    // Compile the same template factory into this test: the offline snapshot must
    // describe exactly the contracts the real DLL will eventually export.
    const auto module = aeg_define_GameModule();
    feature::FeatureCatalog exported;
    if (!module.status) return false;
    for (const auto& definition : module.feature_definitions)
        if (!exported.export_definition(definition)) return false;
    if (exported.contract_digest() != authored.value.feature_catalog.contract_digest()) {
        std::cerr << "Template source metadata differs from its executable factory\n";
        return false;
    }
    if (graph.contract_digest == 0 || graph.contract_digest != exported.contract_digest()) {
        std::cerr << "Nonempty starter graph is not bound to its actual module catalog for Cook\n";
        return false;
    }
    const auto definition = std::find_if(module.feature_definitions.begin(), module.feature_definitions.end(), [&](const auto& entry) {
        return entry.id == graph.nodes.front().definition_id;
    });
    if (definition == module.feature_definitions.end() || !definition->execute) return false;
    const auto machine = feature::canonicalize_process_subgraph(graph.process_subgraphs.front(), definition->contract);
    if (!machine) return false;
    feature::StateSnapshot state;
    state.current_state = "Ready";
    for (const auto expected : {"Running", "Done"}) {
        const auto delta = definition->execute({}, state, {}, {});
        if (!delta || delta.value.next_state != expected ||
            std::find(graph.process_subgraphs.front().transitions.begin(), graph.process_subgraphs.front().transitions.end(),
                      state.current_state + "->" + delta.value.next_state) == graph.process_subgraphs.front().transitions.end()) {
            std::cerr << "Starter callback does not follow its authored Process transitions\n";
            return false;
        }
        state.current_state = delta.value.next_state;
    }
    const auto terminal = definition->execute({}, state, {}, {});
    if (!terminal || !terminal.value.next_state.empty()) return false;

    // A metadata duplicate is presentation-only only when owner, version and
    // semantic contract match. Changed drafts stay available as conflicts but
    // must not relabel or replace the canonical metadata-owned definition.
    for (unsigned mismatch = 0; mismatch != 3; ++mismatch) {
        application::NodeContractFile file;
        file.path = root / "Source/FlowGraph" / ("Mismatch" + std::to_string(mismatch) + ".flowgraph.draft.json");
        file.draft = *gameplay_label;
        file.draft.source = application::NodeContractSource::kEditorDraft;
        file.draft.generated = false;
        file.draft.display_name = "Wrong stale label";
        if (mismatch == 0) {
            ++file.draft.entry.version.major;
            file.draft.entry.contract.version = file.draft.entry.version;
        } else if (mismatch == 1) file.draft.entry.owner_module = feature::ModuleIdentity{"OtherModule"};
        else file.draft.entry.contract.scope.id = feature::ScopeId{"OtherWorld"};
        const auto written = application::write_editor_node_contract_atomic(file, root);
        if (!written) { std::cerr << written.message << '\n'; return false; }
    }
    const auto conflicted = application::load_editor_project_feature_authoring(root, root / "Source", root / "Assets");
    if (!conflicted || conflicted.value.conflicts.size() != 3 || conflicted.value.presentations.size() != 2 ||
        conflicted.value.feature_catalog.contract_digest() != authored.value.feature_catalog.contract_digest() ||
        std::any_of(conflicted.value.presentations.begin(), conflicted.value.presentations.end(),
                    [](const auto& value) { return value.display_name == "Wrong stale label"; })) {
        std::cerr << "Conflicting sidecars replaced catalog authority or supplied stale labels\n";
        return false;
    }
    // Identical visible names remain keyed by independent stable definitions.
    application::NodeContractFile renamed;
    renamed.path = root / "Source/FlowGraph/OutlineRename.flowgraph.draft.json";
    renamed.draft = *outline_label;
    renamed.draft.generated = false;
    renamed.draft.source = application::NodeContractSource::kEditorDraft;
    renamed.draft.display_name = gameplay_label->display_name;
    if (!application::write_editor_node_contract_atomic(renamed, root)) return false;
    const auto same_names = application::load_editor_project_feature_authoring(root, root / "Source", root / "Assets");
    if (!same_names || same_names.value.presentations.size() != 2 || same_names.value.conflicts.size() != 3 ||
        same_names.value.presentations.front().entry.id == same_names.value.presentations.back().entry.id ||
        std::count_if(same_names.value.presentations.begin(), same_names.value.presentations.end(),
                      [](const auto& value) { return value.display_name == "Gameplay starter"; }) != 2) {
        std::cerr << "Matching recovery labels did not retain independent Definition identities\n";
        return false;
    }
    std::filesystem::remove_all(root, error);
    return true;
}
} // namespace

int main(int argc,char** argv) {
    using namespace aeg;
    if (argc != 2 && !verify_fresh_project()) return 15;
    const auto root=argc==2 ? std::filesystem::path{argv[1]} : std::filesystem::temp_directory_path() / "aeg_editor_project_feature_authoring";
    std::error_code error;
    if (argc!=2) {
        std::filesystem::remove_all(root,error); std::filesystem::create_directories(root / "Source"); std::filesystem::create_directories(root / "Assets/Features");
        auto draft=tests::feature_definition();
        module::ModuleMetadata metadata{}; metadata.module={draft.owner_module,"Game Module",{1,0,0},{}};
        if (const auto result = metadata.feature_catalog.export_definition(draft); !result) { std::cerr << result.message << "\n"; return 1; }
        auto render=draft; render.id=feature::FeatureDefinitionId{"Game.Fog"}; render.contract.definition_id=render.id;
        render.contract.domains=feature::to_mask(feature::FeatureDomain::kRender);
        if (const auto result = metadata.feature_catalog.export_definition(render); !result) { std::cerr << result.message << "\n"; return 1; }
        auto composite=draft; composite.id=feature::FeatureDefinitionId{"Game.Composite"}; composite.contract.definition_id=composite.id;
        composite.contract.structure=feature::FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0; composite.contract.children={draft.id,render.id};
        composite.contract.domains=feature::to_mask(feature::FeatureDomain::kLogic)|feature::to_mask(feature::FeatureDomain::kRender);
        if (const auto result = metadata.feature_catalog.export_definition(composite); !result) { std::cerr << result.message << "\n"; return 1; }
        const auto json=module::encode_module_metadata(metadata); if (!json) return 2;
        { std::ofstream out(root / "Source/GameModule.metadata.json"); out << json.value; }
        auto graph=tests::feature_graph(draft); graph.nodes.resize(1); graph.edges.clear();
        if (!application::authoring::save_feature_graph_document(root / "Assets/Features/Default.aegfeature",graph)) return 3;
        { std::ofstream out(root / "Assets/Features/Renderer.aegframe"); out << "schema_version = 1\ngraph_version = 1\nasset_uuid = \"frame://renderer\"\nname = \"Renderer\"\n"; }
        { std::ofstream out(root / "Assets/Features/Broken.aegframe"); out << "pipeline_nodes = [invalid"; }
    }
    const auto loaded=application::load_editor_project_feature_authoring(root,root / "Source",root / "Assets");
    if (!loaded) { std::cerr << loaded.result.message << "\n"; return 4; }
    if (loaded.value.compiled_feature_ids.size()!=3 || loaded.value.feature_catalog.entries().size()!=3 ||
        loaded.value.feature_graphs.size()!=1 || loaded.value.feature_graphs.front().graph.nodes.size()!=1) return 5;
    if (argc != 2) {
        auto changed = tests::feature_definition();
        changed.version = {2, 0, 0};
        changed.contract.version = changed.version;
        changed.contract.ports.front().version = 2;
        module::ModuleMetadata candidate{};
        candidate.module = {changed.owner_module, "Game Module", {1, 0, 0}, {}};
        if (!candidate.feature_catalog.export_definition(changed)) return 8;
        auto render = tests::feature_definition();
        render.id = feature::FeatureDefinitionId{"Game.Fog"}; render.contract.definition_id = render.id;
        render.contract.domains = feature::to_mask(feature::FeatureDomain::kRender);
        if (!candidate.feature_catalog.export_definition(render)) return 9;
        auto added = changed;
        added.id = feature::FeatureDefinitionId{"Game.New"}; added.contract.definition_id = added.id;
        if (!candidate.feature_catalog.export_definition(added)) return 11;
        auto impact_graph = loaded.value.feature_graphs.front();
        impact_graph.path = root / "Assets/Features/Default.aegfeature";
        impact_graph.graph.edges.push_back({impact_graph.graph.nodes.front().node_id, feature::PortId{"out"},
                                            impact_graph.graph.nodes.front().node_id, feature::PortId{"in"}});
        const std::array<application::EditorProjectFeatureGraphDocument, 1> documents{impact_graph};
        const auto review = application::diff_feature_catalogs(loaded.value.feature_catalog, candidate.feature_catalog, documents);
        const auto changed_review = std::find_if(review.changes.begin(), review.changes.end(),
            [&](const auto& change) { return change.definition_id == changed.id; });
        const auto added_review = std::find_if(review.changes.begin(), review.changes.end(),
            [&](const auto& change) { return change.definition_id == added.id; });
        const feature::FeatureDefinitionId removed_id{"Game.Composite"};
        const auto removed_review = std::find_if(review.changes.begin(), review.changes.end(),
            [&](const auto& change) { return change.definition_id == removed_id; });
        if (review.changes.size() != 3 || review.impacts.size() != 1 ||
            review.impacts.front().path != impact_graph.path ||
            review.impacts.front().node_ids != std::vector<feature::FeatureNodeId>{impact_graph.graph.nodes.front().node_id} ||
            review.impacts.front().edges.size() != 1 ||
            review.impacts.front().edges.front().source_node != impact_graph.graph.edges.front().source_node ||
            review.impacts.front().edges.front().target_node != impact_graph.graph.edges.front().target_node ||
            changed_review == review.changes.end() ||
            changed_review->kind != application::FeatureCatalogChangeKind::kChanged ||
            std::find(changed_review->changed_fields.begin(), changed_review->changed_fields.end(), "version") ==
                changed_review->changed_fields.end() ||
            changed_review->changed_ports != std::vector<feature::PortId>{changed.contract.ports.front().id} ||
            added_review == review.changes.end() || added_review->kind != application::FeatureCatalogChangeKind::kAdded ||
            removed_review == review.changes.end() || removed_review->kind != application::FeatureCatalogChangeKind::kRemoved) return 12;
        for (unsigned field = 0; field != 4; ++field) {
            const auto original = tests::feature_definition();
            auto timed = original;
            auto& clock = timed.contract.clock;
            if (field == 0) clock.timing->phase_ns = 9;
            if (field == 1) ++clock.timing->relative_deadline_ns;
            if (field == 2) clock.timing->deadline_policy = feature::DeadlinePolicy::kRejectCommit;
            if (field == 3) ++clock.work_budget_ns;
            feature::FeatureCatalog before, after;
            if (!before.export_definition(original) || !after.export_definition(timed)) return 13;
            const auto timing_review = application::diff_feature_catalogs(before, after, documents);
            if (timing_review.changes.size() != 1 || timing_review.impacts.size() != 1 ||
                timing_review.changes.front().changed_fields != std::vector<std::string>{"clock"}) return 14;
        }
    }
    if (std::ranges::count_if(loaded.value.contract_diagnostics, [](const auto& diagnostic) {
            return diagnostic.find("Unsupported retired authoring asset") != std::string::npos;
        }) != 2) return 6;
    if (argc != 2) {
        { std::ofstream out(root / "Assets/Features/Old.aegpipe"); out << "unreadable retired format"; }
        { std::ofstream out(root / "Assets/Features/Upper.AEGFRAME"); out << "retired format with Windows extension case"; }
        { std::ofstream out(root / "Assets/Features/Renamed.aegfeature"); out << "schema_version = 1\ngraph_version = 1\nasset_uuid = \"old\"\npipeline_nodes = []\n"; }
        { std::ofstream out(root / "Assets/Features/Broken.aegfeature"); out << "nodes = [invalid"; }
        const auto diagnosed = application::load_editor_project_feature_authoring(root,root / "Source",root / "Assets");
        if (!diagnosed || diagnosed.value.feature_catalog.contract_digest() != loaded.value.feature_catalog.contract_digest() ||
            diagnosed.value.feature_graphs.size() != 3 ||
            std::ranges::count_if(diagnosed.value.contract_diagnostics, [](const auto& message) {
                return message.find("Unsupported retired authoring asset") != std::string::npos;
            }) != 4 ||
            std::ranges::count_if(diagnosed.value.feature_graphs, [](const auto& source) {
                return !source.load_result && source.load_result.message.size() && source.graph.nodes.empty();
            }) != 2) return 7;
    }
    if (argc!=2) std::filesystem::remove_all(root,error);
    std::cout << "Feature module/project authoring passed\n";
}
