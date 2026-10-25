/** @file editor_process_layout_smoke.cpp
 *  @brief Author-only Process layout persistence, isolation and transactions.
 */
#include "feature_clock_test_fixture.hpp"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

#include "editor_feature_graph_document.hpp"
#include "feature_process_editor.hpp"
#include "feature_graph_io.hpp"
#include "feature_registry.hpp"

namespace {
using namespace aeg;
using namespace aeg::application;
using namespace aeg::feature;

constexpr FeatureNodeId kOwner{0xffff000000000001ull};
constexpr FeatureNodeId kOtherOwner{0x0000000100000001ull};
const std::string kState = "Wait:|.[]/\"\\\n\xe7\xad\x89\xe5\xbe\x85";
constexpr std::string_view kLayoutPrefix = "aeg.editor.process-layout.v1:";

FeatureDefinition process_definition() {
    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId("Logic.LayoutTest");
    definition.version = {1, 0, 0};
    auto& contract = definition.contract;
    contract.definition_id = definition.id;
    contract.version = definition.version;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kProcess;
    contract.state = StatePolicy::kProcessState;
    contract.scope = ScopeContract{ScopeId("World")};
    contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("LogicTick"), 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.has_continuation = true;
    return definition;
}

FeatureGraphDocument make_graph() {
    const auto definition = process_definition();
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://process-layout";
    graph.schema_version = 1;
    graph.graph_version = 7;
    graph.draft_revision = 11;
    graph.layout_revision = 13;
    graph.nodes = {{kOwner, definition.id, definition.version, {}, {}},
                   {kOtherOwner, definition.id, definition.version, {}, {}}};
    graph.process_subgraphs = {{kOwner, {kState, "Done"}, {kState + "->Done"}, 1, kState, {"Done"}},
                               {kOtherOwner, {kState, "Done"}, {kState + "->Done"}, 1, kState, {"Done"}}};
    graph.extensions = {"vendor:keep:first", "vendor:keep:last"};
    return graph;
}

ProcessEditorLayout make_layout() {
    ProcessEditorLayout layout{};
    layout.zoom = 1.25f;
    layout.pan_offset = {-37.5f, 16.0f};
    layout.grid_snap = false;
    layout.grid_size = 12.5f;
    layout.show_descriptions = false;
    layout.show_conditions = false;
    layout.state_positions = {{kState, {80.5f, -10.25f}}, {"Done", {421.0f, 312.0f}}};
    layout.state_colors = {{kState, "#12abef"}, {"Done", "blue:|\"\\\n"}};
    layout.transition_control_points = {{{100.5f, 200.25f}, {-50.0f, 60.0f}}};
    return layout;
}

std::string metadata_with_positions(std::string positions, std::string colors = "[]") {
    return std::string(kLayoutPrefix) + "{\"owner\":\"" + std::to_string(kOwner.value()) +
        "\",\"zoom\":1,\"pan_offset\":[0,0],\"grid_snap\":true,\"grid_size\":20,"
        "\"show_descriptions\":true,\"show_conditions\":true,\"state_positions\":" + positions +
        ",\"state_colors\":" + colors + ",\"transition_control_points\":[]}";
}

void expect_layout(const ProcessEditorLayout& actual, const ProcessEditorLayout& expected) {
    assert(actual.zoom == expected.zoom && actual.pan_offset == expected.pan_offset);
    assert(actual.grid_snap == expected.grid_snap && actual.grid_size == expected.grid_size);
    assert(actual.show_descriptions == expected.show_descriptions);
    assert(actual.show_conditions == expected.show_conditions);
    assert(actual.state_positions == expected.state_positions);
    assert(actual.state_colors == expected.state_colors);
    assert(actual.transition_control_points == expected.transition_control_points);
}

void expect_same_compilation(const CompiledFeatureGraph& before, const CompiledFeatureGraph& after) {
    assert(before.plans.size() == after.plans.size());
    for (std::size_t index = 0; index < before.plans.size(); ++index) {
        assert(before.plans[index].clock == after.plans[index].clock);
        assert(before.plans[index].instances == after.plans[index].instances);
    }
    assert(before.provenance.size() == after.provenance.size());
    for (std::size_t index = 0; index < before.provenance.size(); ++index) {
        const auto& a = before.provenance[index];
        const auto& b = after.provenance[index];
        assert(a.definition_id == b.definition_id && a.node_id == b.node_id);
        assert(a.instance_id == b.instance_id && a.process_id == b.process_id);
        assert(a.invocation_id == b.invocation_id && a.owner_path.nodes == b.owner_path.nodes);
        assert(a.clock == b.clock && a.domains == b.domains);
    }
}

void test_roundtrip_keeps_exact_names_and_full_owner_identity() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const auto definition = process_definition();
    assert(aeg::feature::test::register_clock_test_definition(registry, definition));
    auto graph = make_graph();
    graph.contract_digest = registry.contract_digest();
    const auto compiled_before = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled_before);
    EditorFeatureGraphDocument document{graph};
    const auto absent = read_process_editor_layout(document.snapshot(), kOwner);
    assert(absent && absent.value.state_positions.empty());
    auto layout = make_layout();
    assert(document.set_process_layout(kOwner, layout));
    assert(document.dirty() && document.revision() == 1);
    assert(document.snapshot().graph_version == 7 && document.snapshot().draft_revision == 11);
    assert(document.snapshot().layout_revision == 14);
    assert(document.snapshot().contract_digest == graph.contract_digest);
    assert(document.snapshot().extensions.front() == "vendor:keep:first");
    assert(document.snapshot().extensions[1] == "vendor:keep:last");
    const auto other_absent = read_process_editor_layout(document.snapshot(), kOtherOwner);
    assert(other_absent && other_absent.value.state_positions.empty());
    auto other_layout = make_layout();
    other_layout.state_positions[kState] = {777.0f, 888.0f};
    assert(document.set_process_layout(kOtherOwner, other_layout));
    assert(document.snapshot().layout_revision == 15);
    const auto file = std::filesystem::temp_directory_path() / "aeg_editor_process_layout_smoke.aegfeature";
    assert(authoring::save_feature_graph_document(file, document.snapshot()));
    const auto loaded = authoring::load_feature_graph_document(file);
    assert(loaded);
    const auto first = read_process_editor_layout(loaded.value, kOwner);
    const auto second = read_process_editor_layout(loaded.value, kOtherOwner);
    assert(first && second);
    expect_layout(first.value, layout);
    expect_layout(second.value, other_layout);
    assert(loaded.value.extensions == document.snapshot().extensions);
    const auto compiled_after = FeatureGraphCompiler{}.compile(loaded.value, registry);
    assert(compiled_after);
    expect_same_compilation(compiled_before.value, compiled_after.value);
    assert(loaded.value.contract_digest == graph.contract_digest);
    std::error_code error;
    assert(std::filesystem::remove(file, error) && !error);
}

void test_bulk_layout_and_move_have_one_history_entry_and_noop_is_clean() {
    EditorFeatureGraphDocument document{make_graph()};
    const ProcessEditorLayout defaults{};
    assert(document.set_process_layout(kOwner, defaults));
    assert(!document.dirty() && !document.can_undo() && document.revision() == 0);
    const auto machine = canonicalize_process_subgraph(document.snapshot().process_subgraphs.front(),
                                                       process_definition().contract);
    assert(machine);
    auto layout = defaults;
    auto_layout_state_machine(machine.value, layout, 800.0f, 600.0f);
    assert(layout.state_positions.size() == 2);
    assert(document.set_process_layout(kOwner, layout));
    const auto placed = document.snapshot_owner();
    assert(document.undo());
    assert(!document.can_undo() && !document.dirty());
    assert(document.redo());
    document.mark_saved();
    assert(document.set_process_layout(kOwner, layout));
    assert(document.snapshot_owner() == placed && !document.dirty() && document.revision() == 3);
    layout.state_positions[kState] = {13.0f, 27.0f};
    assert(document.set_process_layout(kOwner, layout));
    assert(document.revision() == 4 && document.snapshot().layout_revision == 15);
    assert(document.undo());
    assert(document.snapshot_owner() == placed && !document.dirty());
    assert(document.redo());
    const auto moved = read_process_editor_layout(document.snapshot(), kOwner);
    assert(moved && moved.value.state_positions.at(kState) == std::make_pair(13.0f, 27.0f));
}

void test_invalid_layout_rolls_back_without_history_or_dirty_state() {
    EditorFeatureGraphDocument document{make_graph()};
    assert(document.set_process_layout(kOwner, make_layout()));
    document.mark_saved();
    const auto saved = document.snapshot_owner();
    const auto expect_rejected = [&](const ProcessEditorLayout& invalid) {
        assert(!document.set_process_layout(kOwner, invalid));
        assert(document.snapshot_owner() == saved && document.revision() == 1 && !document.dirty());
    };
    auto invalid = make_layout();
    invalid.zoom = 0.0f; expect_rejected(invalid);
    invalid = make_layout(); invalid.zoom = std::numeric_limits<float>::infinity(); expect_rejected(invalid);
    invalid = make_layout(); invalid.pan_offset.second = std::numeric_limits<float>::quiet_NaN(); expect_rejected(invalid);
    invalid = make_layout(); invalid.grid_size = -1.0f; expect_rejected(invalid);
    invalid = make_layout(); invalid.grid_size = std::numeric_limits<float>::infinity(); expect_rejected(invalid);
    invalid = make_layout(); invalid.state_positions[kState].first = std::numeric_limits<float>::infinity(); expect_rejected(invalid);
    invalid = make_layout(); invalid.state_positions["Missing"] = {0.0f, 0.0f}; expect_rejected(invalid);
    invalid = make_layout(); invalid.state_colors["Missing"] = "red"; expect_rejected(invalid);
    invalid = make_layout(); invalid.transition_control_points.front().front().second = std::numeric_limits<float>::quiet_NaN(); expect_rejected(invalid);
    invalid = make_layout(); invalid.transition_control_points.push_back({}); expect_rejected(invalid);
    assert(!document.set_process_layout(FeatureNodeId(42), make_layout()));
    assert(document.undo() && !document.can_undo());
}

void test_metadata_rejects_duplicates_unknown_states_and_malformed_records() {
    EditorFeatureGraphDocument document{make_graph()};
    assert(document.set_process_layout(kOwner, make_layout()));
    const auto source = document.snapshot();
    const auto layout_record = source.extensions.back();
    assert(layout_record.starts_with(kLayoutPrefix));
    auto duplicate = source;
    duplicate.extensions.push_back(layout_record);
    assert(!read_process_editor_layout(duplicate, kOwner));
    EditorFeatureGraphDocument invalid_document{duplicate};
    assert(!invalid_document.set_process_layout(kOwner, make_layout()));
    assert(!invalid_document.erase_process_state(kOwner, kState));
    assert(!invalid_document.erase_node(kOwner));
    assert(!invalid_document.dirty() && !invalid_document.can_undo());
    for (const auto& payload : {std::string("{broken"),
             std::string("{\"owner\":\"18446462598732840961\",\"owner\":\"18446462598732840961\"}"),
             std::string("{\"owner\":\"18446462598732840961junk\"}"),
             std::string("{\"owner\":\"18446744073709551616\"}")}) {
        auto malformed = source;
        malformed.extensions.back() = std::string(kLayoutPrefix) + payload;
        assert(!read_process_editor_layout(malformed, kOwner));
    }
    // Importing the same metadata onto a renamed schema must fail instead of
    // silently losing the state placement.
    auto renamed = source;
    renamed.process_subgraphs.front().states.front() = "Renamed";
    assert(!read_process_editor_layout(renamed, kOwner));
    auto duplicate_state = source;
    duplicate_state.process_subgraphs.front().states.push_back(kState);
    assert(!read_process_editor_layout(duplicate_state, kOwner));
    auto duplicate_owner = source;
    duplicate_owner.process_subgraphs.push_back(duplicate_owner.process_subgraphs.front());
    assert(!read_process_editor_layout(duplicate_owner, kOwner));
    for (const auto& record : {
            metadata_with_positions("[[\"Done\",1,2],[\"Done\",3,4]]"),
            metadata_with_positions("[]", "[[\"Done\",\"red\"],[\"Done\",\"blue\"]]"),
            metadata_with_positions("[[\"Ghost\",1,2]]"),
            metadata_with_positions("[[\"Done\",null,2]]"),
            metadata_with_positions("[[\"Done\",1e100,2]]"),
            metadata_with_positions("[[\"Done\",\"1\",2]]"),
            std::string("aeg.editor.process-layout.v2:{}")}) {
        auto malformed = source;
        malformed.extensions.back() = record;
        assert(!read_process_editor_layout(malformed, kOwner));
    }
    auto duplicate_key = metadata_with_positions("[]");
    duplicate_key.insert(duplicate_key.size() - 1, ",\"zoom\":2");
    auto malformed = source;
    malformed.extensions.back() = duplicate_key;
    assert(!read_process_editor_layout(malformed, kOwner));
    for (const std::string bad_owner : {"18446462598732840961junk", "18446744073709551616", "0", "-1"}) {
        auto record = metadata_with_positions("[]");
        const auto valid_owner = std::to_string(kOwner.value());
        record.replace(record.find(valid_owner), valid_owner.size(), bad_owner);
        malformed.extensions.back() = record;
        assert(!read_process_editor_layout(malformed, kOwner));
    }
    auto exhausted = make_graph();
    exhausted.layout_revision = std::numeric_limits<uint64_t>::max();
    EditorFeatureGraphDocument exhausted_document{exhausted};
    assert(!exhausted_document.set_process_layout(kOwner, make_layout()));
    assert(!exhausted_document.dirty() && !exhausted_document.can_undo());
}

void test_deletion_prunes_layout_in_the_same_undoable_transaction() {
    auto graph = make_graph();
    graph.nodes[1].owner_path.nodes = {kOwner};
    EditorFeatureGraphDocument document{graph};
    assert(document.set_process_layout(kOwner, make_layout()));
    assert(document.set_process_layout(kOtherOwner, make_layout()));
    document.mark_saved();
    const auto saved = document.snapshot_owner();
    assert(document.erase_process_state(kOwner, kState));
    assert(document.revision() == 3 && document.snapshot().layout_revision == 16);
    const auto pruned = read_process_editor_layout(document.snapshot(), kOwner);
    assert(pruned && !pruned.value.state_positions.contains(kState) && !pruned.value.state_colors.contains(kState));
    assert(pruned.value.state_positions.at("Done") == std::make_pair(421.0f, 312.0f));
    assert(pruned.value.transition_control_points.empty());
    const auto unaffected = read_process_editor_layout(document.snapshot(), kOtherOwner);
    assert(unaffected);
    expect_layout(unaffected.value, make_layout());
    assert(document.undo() && document.snapshot_owner() == saved && !document.dirty());
    assert(document.redo());
    assert(document.erase_process_state(kOwner, "Done"));
    assert(document.snapshot().process_subgraphs.size() == 1);
    assert(document.snapshot().extensions.size() == 3);
    assert(document.undo() && document.undo());
    assert(document.snapshot_owner() == saved);
    assert(document.erase_node(kOwner));
    assert(document.snapshot().nodes.empty() && document.snapshot().process_subgraphs.empty());
    assert(document.snapshot().extensions == std::vector<std::string>({"vendor:keep:first", "vendor:keep:last"}));
    assert(document.snapshot().layout_revision == 16);
    assert(document.undo() && document.snapshot_owner() == saved && !document.dirty());
    assert(document.redo() && document.snapshot().extensions.size() == 2);
}

void test_transition_edits_keep_control_points_attached_to_the_same_transition() {
    EditorFeatureGraphDocument document{make_graph()};
    assert(document.set_process_layout(kOwner, make_layout()));
    document.mark_saved();
    const auto saved = document.snapshot_owner();
    assert(document.add_process_transition(kOwner, "Done", kState));
    const auto added = read_process_editor_layout(document.snapshot(), kOwner);
    assert(added && added.value.transition_control_points.size() == 2);
    assert((added.value.transition_control_points[0] ==
            std::vector<std::pair<float, float>>({{100.5f, 200.25f}, {-50.0f, 60.0f}})));
    assert(added.value.transition_control_points[1].empty());
    assert(document.snapshot().graph_version == 8 && document.snapshot().draft_revision == 12);
    assert(document.snapshot().layout_revision == 15 && document.revision() == 2);
    assert(document.undo() && document.snapshot_owner() == saved && !document.dirty());
    assert(document.redo());
    auto layout = added.value;
    layout.transition_control_points[1] = {{71.0f, 72.0f}};
    assert(document.set_process_layout(kOwner, layout));
    const auto before_delete = document.snapshot_owner();
    assert(document.erase_process_transition(kOwner, kState, "Done"));
    const auto removed = read_process_editor_layout(document.snapshot(), kOwner);
    assert(removed && removed.value.transition_control_points.size() == 1);
    assert((removed.value.transition_control_points[0] ==
            std::vector<std::pair<float, float>>({{71.0f, 72.0f}})));
    assert(document.snapshot().layout_revision == 17);
    assert(document.undo() && document.snapshot_owner() == before_delete);
    assert(document.redo());

    auto graph = make_graph();
    graph.process_subgraphs[0].states.push_back("Stay");
    graph.process_subgraphs[0].transitions.push_back("Done->Stay");
    EditorFeatureGraphDocument state_document{graph};
    layout = make_layout();
    layout.transition_control_points.push_back({{91.0f, 92.0f}});
    assert(state_document.set_process_layout(kOwner, layout));
    assert(state_document.erase_process_state(kOwner, kState));
    const auto pruned = read_process_editor_layout(state_document.snapshot(), kOwner);
    assert(pruned && pruned.value.transition_control_points.size() == 1);
    assert((pruned.value.transition_control_points[0] ==
            std::vector<std::pair<float, float>>({{91.0f, 92.0f}})));
}

void test_semantic_edits_preserve_layout_extension_order_and_skip_unchanged_layout_revision() {
    EditorFeatureGraphDocument document{make_graph()};
    auto layout = make_layout();
    layout.transition_control_points.clear();
    assert(document.set_process_layout(kOwner, layout));
    assert(document.set_process_layout(kOtherOwner, layout));
    const auto original_extensions = document.snapshot().extensions;
    assert(document.add_process_transition(kOwner, "Done", kState));
    assert(document.snapshot().extensions == original_extensions);
    assert(document.snapshot().layout_revision == 15);
    assert(document.erase_process_transition(kOwner, "Done", kState));
    assert(document.snapshot().extensions == original_extensions);
    assert(document.snapshot().layout_revision == 15);
    assert(document.erase_process_state(kOwner, kState));
    assert(document.snapshot().extensions.front() == "vendor:keep:first");
    assert(document.snapshot().extensions[1] == "vendor:keep:last");
    assert(document.snapshot().extensions[3] == original_extensions[3]);
    assert(document.snapshot().layout_revision == 16);
}

void test_semantic_process_setters_reject_invalid_layout_metadata_without_healing_it() {
    auto graph = make_graph();
    graph.extensions.push_back(metadata_with_positions("[[\"Ghost\",1,2]]"));
    EditorFeatureGraphDocument document{graph};
    const auto original = document.snapshot_owner();
    assert(!document.add_process_state(kOwner, "Ghost"));
    assert(!document.set_process_initial_state(kOwner, "Done"));
    assert(!document.set_process_terminal_state(kOwner, kState, true));
    assert(document.snapshot_owner() == original && !document.dirty() && !document.can_undo());
    assert(document.revision() == 0 && !read_process_editor_layout(document.snapshot(), kOwner));

    graph = make_graph();
    graph.extensions.push_back(metadata_with_positions("[]"));
    graph.extensions.push_back(graph.extensions.back());
    EditorFeatureGraphDocument duplicate{graph};
    assert(!duplicate.add_process_state(kOwner, "Ghost"));
    assert(!duplicate.set_process_initial_state(kOwner, kState));
    assert(!duplicate.set_process_terminal_state(kOwner, "Done", true));
    assert(!duplicate.dirty() && duplicate.revision() == 0);

    EditorFeatureGraphDocument absent{make_graph()};
    assert(absent.set_process_initial_state(kOwner, kState));
    assert(absent.set_process_terminal_state(kOwner, "Done", true));
    assert(!absent.dirty() && absent.revision() == 0);
    assert(absent.add_process_state(kOwner, "Ghost"));
    assert(absent.snapshot().layout_revision == 13 && absent.snapshot().extensions.size() == 2);
    graph = make_graph();
    graph.process_subgraphs.erase(graph.process_subgraphs.begin());
    EditorFeatureGraphDocument first_state{graph};
    assert(first_state.add_process_state(kOwner, "First"));
    assert(first_state.snapshot().layout_revision == 13 && first_state.snapshot().extensions.size() == 2);
}

void test_unchanged_imported_layout_metadata_keeps_its_original_bytes_and_revision() {
    for (const std::string positions : {"[]", "[[\"Done\",20,30]]"}) {
        auto graph = make_graph();
        auto record = metadata_with_positions(positions);
        record.insert(kLayoutPrefix.size(), " \n");
        graph.extensions.insert(graph.extensions.begin() + 1, record);
        EditorFeatureGraphDocument document{graph};
        const auto original_extensions = graph.extensions;
        assert(document.add_process_transition(kOwner, "Done", kState));
        assert(document.snapshot().extensions == original_extensions);
        assert(document.snapshot().layout_revision == 13);
        assert(document.erase_process_transition(kOwner, "Done", kState));
        assert(document.snapshot().extensions == original_extensions);
        assert(document.snapshot().layout_revision == 13);
        assert(document.erase_process_state(kOwner, kState));
        assert(document.snapshot().extensions == original_extensions);
        assert(document.snapshot().layout_revision == 13);
        assert(document.erase_process_state(kOwner, "Done"));
        assert(document.snapshot().extensions.size() == 2 && document.snapshot().layout_revision == 14);
    }
}
}  // namespace

int main() {
    test_roundtrip_keeps_exact_names_and_full_owner_identity();
    test_bulk_layout_and_move_have_one_history_entry_and_noop_is_clean();
    test_invalid_layout_rolls_back_without_history_or_dirty_state();
    test_metadata_rejects_duplicates_unknown_states_and_malformed_records();
    test_deletion_prunes_layout_in_the_same_undoable_transaction();
    test_transition_edits_keep_control_points_attached_to_the_same_transition();
    test_semantic_edits_preserve_layout_extension_order_and_skip_unchanged_layout_revision();
    test_semantic_process_setters_reject_invalid_layout_metadata_without_healing_it();
    test_unchanged_imported_layout_metadata_keeps_its_original_bytes_and_revision();
    std::cout << "editor Process layout smoke passed\n";
}
