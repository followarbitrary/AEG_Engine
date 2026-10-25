/**
 * @file feature_graph_editor_layout_cook_smoke.cpp
 * @brief Editor Process and Feature layouts remain source-only and cannot affect Cook.
 */
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <feature_graph_asset.hpp>
#include <feature_graph_asset_importer.hpp>
#include <feature_graph_io.hpp>

namespace {
using namespace aeg;

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const std::vector<std::string> kRuntimeExtensions{
    "vendor:runtime-data",
    "aeg.editor.process-layout",
    "aeg.editor.process-layout-v1:keep",
    "vendor:aeg.editor.process-layout.v1:keep",
    " aeg.editor.process-layout.v1:keep",
    "aeg.editor.canvas.v1:keep",
    "aeg.editor.feature-layout-v1:keep",
    "vendor:aeg.editor.feature-layout.v1:keep",
    " aeg.editor.feature-layout.v1:keep",
    "aeg.editor.feature-layout.v2:keep",
    ""};

std::string layout_record(feature::FeatureNodeId owner, bool moved) {
    return "aeg.editor.process-layout.v1:{\"grid_size\":20.0,\"grid_snap\":true,\"owner\":\"" +
        std::to_string(owner.value()) + "\",\"pan_offset\":" + (moved ? "[-30.0,90.0]" : "[10.0,20.0]") +
        ",\"show_conditions\":true,\"show_descriptions\":true,\"state_colors\":[]," +
        "\"state_positions\":[[\"Active\",300.0,100.0],[\"Complete\",520.0,100.0],[\"Dormant\",80.0,100.0]]," +
        "\"transition_control_points\":[],\"zoom\":" + (moved ? "2.0}" : "1.0}");
}

std::string feature_layout_record(feature::FeatureNodeId node, bool moved) {
    return "aeg.editor.feature-layout.v1:{\"node_positions\":[[\"" + std::to_string(node.value()) +
        "\"," + (moved ? "280.0,160.0" : "40.0,60.0") +
        "]],\"pan_offset\":[0.0,0.0],\"zoom\":1.0}";
}

feature::FeatureGraphDocument make_source() {
    using namespace feature;
    FeatureGraphDocument graph{};
    graph.asset_uuid = "00000000-0000-0000-0000-000000000051";
    graph.schema_version = 1;
    graph.graph_version = 17;
    graph.draft_revision = 23;
    graph.layout_revision = 31;
    graph.module_requirements = {"com.example.game@2", "com.example.render@1"};
    graph.nodes = {
        {FeatureNodeId("Owner"), FeatureDefinitionId("Logic.Sequence"), {2, 4, 6}, {}, {7, 9, 11}},
        {FeatureNodeId("Child"), FeatureDefinitionId("Logic.Child"), {1, 2, 3},
         OwnerPath{{FeatureNodeId("Owner")}}, {13, 15}}};
    graph.edges = {{FeatureNodeId("Owner"), PortId("Out"), FeatureNodeId("Child"), PortId("In"),
                    EdgeKind::kQueue, ClockId("LogicTick"), ClockId("FixedTick"), 2, 8, 4}};
    graph.process_subgraphs = {{FeatureNodeId("Owner"), {"Dormant", "Active", "Complete"},
                                {"Dormant->Active", "Active->Complete"}, 3, "Dormant", {"Complete"}}};
    graph.input_bindings = {"Input.Fire", "Input.Cancel"};
    graph.variadic_slots = {"Children"};
    graph.extensions = kRuntimeExtensions;
    graph.extensions.insert(graph.extensions.begin() + 1,
                            layout_record(graph.nodes.front().node_id, false));
    graph.extensions.push_back("aeg.editor.process-layout.v2:future-layout");
    graph.extensions.push_back("aeg.editor.process-layout.");
    graph.extensions.push_back(feature_layout_record(graph.nodes.front().node_id, false));
    graph.contract_digest = 0xfedcba9876543210ull;
    return graph;
}

bool runtime_data_matches(const feature::FeatureGraphDocument& cooked,
                          const feature::FeatureGraphDocument& source) {
    if (!require(cooked.asset_uuid == source.asset_uuid && cooked.schema_version == source.schema_version &&
                 cooked.graph_version == source.graph_version && cooked.draft_revision == source.draft_revision,
                 "formal Cook changed FeatureGraph identity or semantic revisions") ||
        !require(cooked.extensions == kRuntimeExtensions,
                 "formal Cook retained Process/Feature layout records or removed unrelated extensions") ||
        !require(cooked.layout_revision == 0, "formal Cook retained the editor layout revision") ||
        !require(cooked.module_requirements == source.module_requirements &&
                 cooked.input_bindings == source.input_bindings && cooked.variadic_slots == source.variadic_slots &&
                 cooked.contract_digest == source.contract_digest,
                 "formal Cook changed runtime graph metadata") ||
        !require(cooked.nodes.size() == source.nodes.size() && cooked.edges.size() == source.edges.size() &&
                 cooked.process_subgraphs.size() == source.process_subgraphs.size(),
                 "formal Cook changed runtime graph collections")) return false;
    for (std::size_t index = 0; index < source.nodes.size(); ++index) {
        const auto& actual = cooked.nodes[index];
        const auto& expected = source.nodes[index];
        if (!require(actual.node_id == expected.node_id && actual.definition_id == expected.definition_id &&
                     actual.definition_version.major == expected.definition_version.major &&
                     actual.definition_version.minor == expected.definition_version.minor &&
                     actual.definition_version.patch == expected.definition_version.patch &&
                     actual.owner_path.nodes == expected.owner_path.nodes &&
                     actual.parameter_blob == expected.parameter_blob,
                     "formal Cook changed a Feature node")) return false;
    }
    for (std::size_t index = 0; index < source.edges.size(); ++index) {
        const auto& actual = cooked.edges[index];
        const auto& expected = source.edges[index];
        if (!require(actual.source_node == expected.source_node && actual.source_port == expected.source_port &&
                     actual.target_node == expected.target_node && actual.target_port == expected.target_port &&
                     actual.kind == expected.kind && actual.source_clock == expected.source_clock &&
                     actual.target_clock == expected.target_clock && actual.version == expected.version &&
                     actual.capacity == expected.capacity && actual.deadline_ticks == expected.deadline_ticks,
                     "formal Cook changed a Feature edge")) return false;
    }
    for (std::size_t index = 0; index < source.process_subgraphs.size(); ++index) {
        const auto& actual = cooked.process_subgraphs[index];
        const auto& expected = source.process_subgraphs[index];
        if (!require(actual.owner_node == expected.owner_node && actual.states == expected.states &&
                     actual.transitions == expected.transitions && actual.state_schema_version == expected.state_schema_version &&
                     actual.initial_state == expected.initial_state && actual.terminal_states == expected.terminal_states,
                     "formal Cook changed Process state-machine semantics")) return false;
    }
    return true;
}

bool test_editor_layout_is_source_only(const std::filesystem::path& file) {
    auto source = make_source();
    const auto id = foundation::AssetId::parse(source.asset_uuid);
    if (!require(static_cast<bool>(id), "fixture AssetId is invalid")) return false;
    tooling::AssetMeta meta{};
    meta.products = {{id.value, foundation::AssetKind::kFeatureGraph, "main"}};
    const tooling::FeatureGraphAssetImporter importer;
    std::vector<std::byte> baseline_payload;
    for (unsigned edit = 0; edit < 3; ++edit) {
        if (!require(static_cast<bool>(application::authoring::save_feature_graph_document(file, source)),
                     "could not save source FeatureGraph TOML")) return false;
        const auto source_bytes = read_bytes(file);
        const auto loaded = application::authoring::load_feature_graph_document(file);
        if (!require(loaded && loaded.value.extensions == source.extensions &&
                     loaded.value.layout_revision == source.layout_revision,
                     "Authoring did not preserve Process/Feature layout metadata")) return false;
        const auto cooked = importer.cook(file, meta);
        if (!require(cooked && cooked.value.size() == 1, "FeatureGraph importer failed to Cook") ||
            !require(read_bytes(file) == source_bytes, "formal Cook modified the source TOML bytes")) return false;
        const auto& product = cooked.value.front();
        if (!require(product.id == id.value && product.kind == foundation::AssetKind::kFeatureGraph &&
                     product.schema_version == 1, "formal Cook changed the product identity")) return false;
        const auto decoded = asset::decode_feature_graph_asset(product.payload);
        if (!require(static_cast<bool>(decoded), "cooked AEGF payload did not decode") ||
            !runtime_data_matches(decoded.value, source)) return false;
        if (edit == 0) {
            baseline_payload = product.payload;
        } else if (!require(product.payload == baseline_payload,
                            "layout-only source edits changed the cooked AEGF payload")) return false;
        source.layout_revision += 7;
        source.extensions = kRuntimeExtensions;
        if (edit == 0) {
            source.extensions.push_back(layout_record(source.nodes.front().node_id, true));
            source.extensions.push_back(feature_layout_record(source.nodes.front().node_id, true));
        }
    }
    return true;
}
}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("aeg_feature_graph_editor_layout_cook_smoke_" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (!require(!error, "could not create temporary source directory")) return 1;
    const bool passed = test_editor_layout_is_source_only(root / "Process.aegfeature");
    std::filesystem::remove_all(root, error);
    if (!passed) return 2;
    std::cout << "FeatureGraph editor layout Cook smoke passed\n";
    return 0;
}
