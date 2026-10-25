/** @brief Drives the main Feature canvas through real ImGui mouse and keyboard input. */
#include "editor_feature_graph_panel.hpp"

#include <imgui_internal.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace aeg;
constexpr feature::FeatureNodeId kFirst{uint64_t{7}};
constexpr feature::FeatureNodeId kSecond{uint64_t{0x8000000100000007ull}};
constexpr feature::FeatureNodeId kOutside{uint64_t{19}};
constexpr feature::PortId kIn{uint64_t{0x8000000100000011ull}};
constexpr feature::PortId kOut{uint64_t{0x8000000100000012ull}};
constexpr feature::PortId kBad{uint64_t{0x8000000100000013ull}};
constexpr std::string_view kLayoutPrefix{"aeg.editor.feature-layout.v1:"};

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

feature::FeatureCatalog make_catalog() {
    feature::FeatureCatalog catalog;
    for (uint64_t id : {101ull, 102ull, 103ull}) {
        feature::FeatureDefinition definition;
        definition.id = feature::FeatureDefinitionId{id}; definition.version = {1, 0, 0};
        auto& contract = definition.contract;
        contract.definition_id = definition.id; contract.version = definition.version;
        contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
        contract.execution = feature::ExecutionKind::kCall;
        contract.scope = {feature::ScopeId{"World"}};
        contract.clock = feature::make_periodic_clock_contract(feature::ClockId{"CanvasTick"}, 60, 4,
            feature::LateDataPolicy::kReject, feature::DeadlinePolicy::kObserve, 250000, 0);
        contract.permissions.version = 1;
        contract.ports = {
            {kIn, feature::PortDirection::kInput, feature::ValueSchemaId{"Canvas.Int.v1"}, 3},
            {kOut, feature::PortDirection::kOutput, feature::ValueSchemaId{"Canvas.Int.v1"}, 3},
            {kBad, feature::PortDirection::kInput, feature::ValueSchemaId{"Canvas.Text.v1"}, 9}};
        require(static_cast<bool>(catalog.export_definition(definition)), "Canvas contract fixture rejected");
    }
    return catalog;
}

feature::FeatureEdge edge(feature::FeatureNodeId from, feature::FeatureNodeId to) {
    return {from, kOut, to, kIn, feature::EdgeKind::kData, feature::ClockId{"CanvasTick"},
        feature::ClockId{"CanvasTick"}, 3, 0, 0};
}

feature::FeatureGraphDocument make_graph(bool edges = false) {
    feature::FeatureGraphDocument graph;
    graph.asset_uuid = "00000000-0000-0000-0000-000000000071";
    graph.schema_version = 1; graph.graph_version = 11; graph.draft_revision = 13;
    graph.layout_revision = 17; graph.contract_digest = 0xfedcba9876543210ull;
    graph.nodes = {{kFirst, feature::FeatureDefinitionId{uint64_t{101}}, {1, 0, 0}, {}, {4, 5}},
                   {kSecond, feature::FeatureDefinitionId{uint64_t{102}}, {1, 0, 0}, {}, {6, 7}},
                   {kOutside, feature::FeatureDefinitionId{uint64_t{103}}, {1, 0, 0}, {}, {8, 9}}};
    if (edges) graph.edges = {edge(kFirst, kSecond), edge(kSecond, kOutside)};
    graph.extensions = {"vendor:runtime-data", "aeg.editor.canvas.v1:keep",
        std::string{kLayoutPrefix} + "{\"node_positions\":[[\"7\",40.0,60.0],[\"" +
            std::to_string(kSecond.value()) + "\",390.0,60.0],[\"19\",740.0,60.0]],"
            "\"pan_offset\":[0.0,0.0],\"zoom\":1.0}"};
    return graph;
}

class Harness final {
public:
    Harness() {
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = {1600, 3400}; io.DeltaTime = 1.0f / 60.0f;
        io.ConfigErrorRecoveryEnableAssert = io.ConfigErrorRecoveryEnableDebugLog =
            io.ConfigErrorRecoveryEnableTooltip = false;
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        context->ErrorCallbackUserData = &errors;
        context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
            static_cast<std::vector<std::string>*>(data)->emplace_back(message);
        };
    }
    ~Harness() {
        ImGui::DestroyContext(context);
        for (const auto& error : errors) std::cerr << error << '\n';
        require(errors.empty(), "ImGui reported a Feature canvas error");
    }
    void draw(application::EditorFeatureGraphDocument& document) {
        ImGui::NewFrame();
        // The outer window owns logging; nested panel End must not finish it.
        ImGui::Begin("Feature canvas capture", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        if (external_selection_mode) {
            ImGui::Begin("Flow Graph###FlowGraph");
            if (ImGui::Selectable("Inspect second node", selected_node == std::to_string(kSecond.value())))
                selected_node = std::to_string(kSecond.value());
            external_select = context->LastItemData.Rect.GetCenter();
            standalone_canvas.draw(document, &document.catalog(), selected_node, feedback);
            // The actual canvas emits Delete selected as its last item when a
            // selection exists. Capture that widget's rectangle, not a guessed
            // position or synthetic activation of its callback.
            const auto selection_seed = ImHashStr("Main Feature canvas selection", 0, ImGui::GetCurrentWindow()->ID);
            if (context->LastItemData.ID == ImHashStr("Delete selected", 0, selection_seed))
                delete_selected = context->LastItemData.Rect.GetCenter();
            ImGui::End();
        } else panel.draw(document, "Canvas.aegfeature", {});
        rendered = context->LogBuffer.c_str();
        ImGui::LogFinish(); ImGui::End(); ImGui::Render();
    }
    ImGuiWindow* canvas() const {
        for (auto* window : context->Windows) {
            const std::string_view name{window->Name};
            if (window->Active && name.find("Flow Graph###FlowGraph/") == 0 &&
                name.find("FeatureGraphCanvas") != std::string_view::npos) return window;
        }
        require(false, "Main FeatureGraph canvas child is missing (Process state canvas is insufficient)");
        return nullptr;
    }
    ImVec2 at(float x, float y) const {
        const auto origin = canvas()->DC.CursorStartPos;
        return {origin.x + x, origin.y + y};
    }
    ImGuiID node_id(feature::FeatureNodeId node) const {
        return ImHashStr("node", 0, ImHashStr(std::to_string(node.value()).c_str(), 0, canvas()->ID));
    }
    ImGuiID port_id(feature::FeatureNodeId node, feature::PortId port) const {
        const auto node_seed = ImHashStr(std::to_string(node.value()).c_str(), 0, canvas()->ID);
        return ImHashStr("port", 0, ImHashStr(std::to_string(port.value()).c_str(), 0, node_seed));
    }
    void mouse(application::EditorFeatureGraphDocument& document, ImVec2 point,
               int button = -1, bool down = false) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(point.x, point.y);
        if (button >= 0) io.AddMouseButtonEvent(button, down);
        draw(document);
    }
    void click(application::EditorFeatureGraphDocument& document, ImVec2 point) {
        mouse(document, point); mouse(document, point, 0, true); mouse(document, point, 0, false);
    }
    void key(application::EditorFeatureGraphDocument& document, ImGuiKey key, bool ctrl = false) {
        auto& io = ImGui::GetIO();
        if (ctrl) { io.AddKeyEvent(ImGuiMod_Ctrl, true); draw(document); }
        io.AddKeyEvent(key, true); draw(document);
        io.AddKeyEvent(key, false); draw(document);
        if (ctrl) { io.AddKeyEvent(ImGuiMod_Ctrl, false); draw(document); }
    }
    ImVec2 connection_control_point(float x_offset = 70.0f) const {
        // The real toolbar is immediately above the canvas: combo row, then one help line.
        return {canvas()->Pos.x + x_offset, canvas()->Pos.y - ImGui::GetTextLineHeightWithSpacing() -
            ImGui::GetStyle().ItemSpacing.y - ImGui::GetFrameHeight() * 0.5f};
    }
    void select_connection_kind(application::EditorFeatureGraphDocument& document, int index) {
        click(document, connection_control_point());
        auto* popup = ImGui::FindWindowByName("##Combo_00");
        require(popup && !context->OpenPopupStack.empty(), "Connection kind combo did not open from a real mouse click");
        const float row_height = ImGui::GetFrameHeight();
        const ImVec2 choice{popup->DC.CursorStartPos.x + 18,
            popup->DC.CursorStartPos.y + static_cast<float>(index) * row_height + row_height * 0.5f};
        click(document, choice); draw(document);
        require(context->OpenPopupStack.empty(), "Connection kind mouse selection did not close its popup");
    }
    void edit_deadline(application::EditorFeatureGraphDocument& document, const char* value) {
        const auto& style = ImGui::GetStyle();
        const float offset = 140.0f + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Connection kind").x +
            style.ItemSpacing.x + 16.0f;
        click(document, connection_control_point(offset));
        require(ImGui::GetIO().WantTextInput, "Deadline input did not gain focus from a real mouse click");
        key(document, ImGuiKey_A, true);
        ImGui::GetIO().AddInputCharactersUTF8(value); draw(document);
        key(document, ImGuiKey_Enter);
    }
    ImGuiContext* context{};
    application::EditorFeatureGraphPanel panel;
    application::EditorFeatureGraphCanvas standalone_canvas;
    bool external_selection_mode{false};
    std::string selected_node, feedback;
    ImVec2 external_select{}, delete_selected{};
    std::vector<std::string> errors;
    std::string rendered;
};

void test_visible_nodes_and_ports() {
    application::EditorFeatureGraphDocument document{make_graph(), make_catalog()};
    const auto original = document.snapshot_owner(); Harness ui;
    ui.draw(document); ui.draw(document);
    require(ui.canvas()->DrawList->VtxBuffer.Size > 0, "Feature canvas has no visible drawing");
    require(ui.rendered.find(std::to_string(kFirst.value())) != std::string::npos &&
            ui.rendered.find(std::to_string(kSecond.value())) != std::string::npos &&
            ui.rendered.find(std::to_string(kIn.value())) != std::string::npos &&
            ui.rendered.find(std::to_string(kOut.value())) != std::string::npos,
            "Main canvas does not display full node and formal port identities");
    ui.mouse(document, ui.at(140, 80));
    require(ui.context->HoveredId == ui.node_id(kFirst), "First full-width node item is not interactive");
    ui.mouse(document, ui.at(490, 80));
    require(ui.context->HoveredId == ui.node_id(kSecond) && ui.node_id(kFirst) != ui.node_id(kSecond),
            "Node item identity truncated the high 64-bit identity");
    ui.mouse(document, ui.at(290, 130));
    require(ui.context->HoveredId == ui.port_id(kFirst, kOut), "Formal output port is not a real canvas item");
    ui.mouse(document, ui.at(400, 130));
    require(ui.context->HoveredId == ui.port_id(kSecond, kIn), "Formal input port is not a real canvas item");
    require(document.snapshot_owner() == original && !document.dirty() && !document.can_undo(),
            "Opening, hovering or selecting the canvas changed authoring state");
}

void test_port_drag_transaction_and_rejection() {
    application::EditorFeatureGraphDocument document{make_graph(), make_catalog()};
    const auto original = document.snapshot_owner(); Harness ui;
    ui.draw(document); ui.draw(document);
    const auto from = ui.at(290, 130), to = ui.at(400, 130);
    ui.mouse(document, from); ui.mouse(document, from, 0, true); ui.mouse(document, to);
    require(document.snapshot_owner() == original, "Port drag committed before mouse release");
    ui.mouse(document, to, 0, false);
    require(document.snapshot().edges.size() == 1, "Real output-to-input drag did not add an edge");
    const auto& added = document.snapshot().edges.front();
    require(added.source_node == kFirst && added.target_node == kSecond && added.source_port == kOut &&
            added.target_port == kIn && added.version == 3 && added.kind == feature::EdgeKind::kData &&
            added.source_clock == feature::ClockId{"CanvasTick"} && added.target_clock == added.source_clock,
            "Canvas edge did not retain the actual formal port and Clock contract");
    require(added.capacity == 0 && added.deadline_ticks == 0,
            "Default Data drag silently authored a hidden capacity or one-tick TTL");
    require(document.snapshot().graph_version == original->graph_version + 1 &&
            document.snapshot().draft_revision == original->draft_revision + 1 &&
            document.snapshot().layout_revision == original->layout_revision,
            "Edge drag was not exactly one semantic transaction");
    require(static_cast<bool>(document.undo()) && document.snapshot_owner() == original && !document.can_undo(),
            "One Undo did not remove the entire drag connection");
    const auto revision = document.revision();
    // Wrong schema/version and wrong direction must not create a failed history entry.
    for (const auto invalid : {ui.at(400, 156), ui.at(640, 130)}) {
        ui.mouse(document, from); ui.mouse(document, from, 0, true);
        ui.mouse(document, invalid); ui.mouse(document, invalid, 0, false);
        require(document.snapshot_owner() == original && document.revision() == revision && !document.can_undo(),
                "An incompatible canvas connection changed the graph or undo history");
    }
    // Configure each visible TTL mode, then change back through the actual combo.
    // A stale transport setting must never become an invisible Data lifetime.
    for (const int kind : {static_cast<int>(feature::EdgeKind::kSnapshot), static_cast<int>(feature::EdgeKind::kCommand)}) {
        ui.select_connection_kind(document, kind);
        ui.edit_deadline(document, "17");
        ui.mouse(document, from); ui.mouse(document, from, 0, true); ui.mouse(document, to);
        ui.mouse(document, to, 0, false);
        require(document.snapshot().edges.size() == 1 && document.snapshot().edges.front().kind ==
                static_cast<feature::EdgeKind>(kind) && document.snapshot().edges.front().deadline_ticks == 17,
                "Visible Snapshot/Command deadline configuration was not authored by port dragging");
        require(static_cast<bool>(document.undo()) && document.snapshot_owner() == original,
                "Undo did not restore the graph before transport-to-Data switching");
        ui.draw(document); ui.select_connection_kind(document, static_cast<int>(feature::EdgeKind::kData));
        ui.mouse(document, from); ui.mouse(document, from, 0, true); ui.mouse(document, to);
        ui.mouse(document, to, 0, false);
        require(document.snapshot().edges.size() == 1 && document.snapshot().edges.front().kind == feature::EdgeKind::kData &&
                document.snapshot().edges.front().capacity == 0 && document.snapshot().edges.front().deadline_ticks == 0,
                "Switching Snapshot/Command to Data retained an invisible capacity or stale TTL");
        require(static_cast<bool>(document.undo()) && document.snapshot_owner() == original,
                "Undo did not restore the graph after Data switch");
        ui.draw(document);
    }
}

void test_selection_copy_internal_edges_only() {
    application::EditorFeatureGraphDocument document{make_graph(true), make_catalog()};
    const auto original = document.snapshot_owner(); Harness ui;
    ui.draw(document); ui.draw(document); ui.click(document, ui.at(140, 80));
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); ui.draw(document);
    ui.click(document, ui.at(490, 80));
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); ui.draw(document);
    require(document.snapshot_owner() == original, "Multi-selection persisted into the authored document");
    ui.key(document, ImGuiKey_D, true);
    const auto& copied = document.snapshot();
    require(copied.nodes.size() == 5 && copied.edges.size() == 3,
            "Ctrl+D did not copy exactly the selected nodes and their internal edge");
    std::set<uint64_t> identities;
    feature::FeatureNodeId first_copy{}, second_copy{};
    for (const auto& node : copied.nodes) {
        require(identities.insert(node.node_id.value()).second, "Copy reused a node identity");
        if (node.node_id == kFirst || node.node_id == kSecond || node.node_id == kOutside) continue;
        if (node.definition_id == original->nodes[0].definition_id) {
            first_copy = node.node_id;
            require(node.parameter_blob == original->nodes[0].parameter_blob, "Copy changed first node parameters");
        } else if (node.definition_id == original->nodes[1].definition_id) {
            second_copy = node.node_id;
            require(node.parameter_blob == original->nodes[1].parameter_blob, "Copy changed second node parameters");
        } else require(false, "Copy included the external node");
    }
    require(first_copy.is_valid() && second_copy.is_valid() && first_copy != second_copy,
            "Copy did not allocate independent full-width identities");
    require(std::count_if(copied.edges.begin(), copied.edges.end(), [&](const auto& item) {
        return item.source_node == first_copy && item.target_node == second_copy &&
               item.source_port == kOut && item.target_port == kIn && item.version == 3;
    }) == 1, "Copy did not remap its internal edge to both new node identities");
    require(std::none_of(copied.edges.begin(), copied.edges.end(), [&](const auto& item) {
        return (item.source_node == first_copy || item.source_node == second_copy) && item.target_node == kOutside;
    }), "Copy duplicated an outgoing external edge");
    require(copied.graph_version == original->graph_version + 1 && copied.draft_revision == original->draft_revision + 1,
            "Multi-node copy created more than one semantic transaction");
    require(static_cast<bool>(document.undo()) && document.snapshot_owner() == original && !document.can_undo(),
            "One Undo did not remove both copied nodes and the copied edge");
}

void test_move_is_one_layout_transaction() {
    application::EditorFeatureGraphDocument document{make_graph(), make_catalog()};
    const auto original = document.snapshot_owner(); Harness ui;
    ui.draw(document); ui.draw(document);
    const auto start = ui.at(140, 80), moved = ui.at(200, 120);
    ui.mouse(document, start); ui.mouse(document, start, 0, true); ui.mouse(document, moved);
    require(document.snapshot_owner() == original, "Node move committed during a drag preview");
    ui.mouse(document, moved, 0, false);
    require(document.snapshot().layout_revision == original->layout_revision + 1 &&
            document.snapshot().graph_version == original->graph_version &&
            document.snapshot().draft_revision == original->draft_revision &&
            document.snapshot().contract_digest == original->contract_digest &&
            document.snapshot().extensions != original->extensions,
            "Node move changed semantic revisions/digest or did not persist its layout");
    require(document.snapshot().extensions[0] == original->extensions[0] &&
            document.snapshot().extensions[1] == original->extensions[1], "Node move damaged business extensions");
    ui.mouse(document, moved);
    require(ui.context->HoveredId == ui.node_id(kFirst), "Committed node position was not displayed");
    require(static_cast<bool>(document.undo()) && document.snapshot_owner() == original && !document.can_undo(),
            "Node move was not one undoable layout transaction");
    ui.draw(document);
    ui.mouse(document, start); ui.mouse(document, start, 0, true); ui.mouse(document, moved);
    ui.key(document, ImGuiKey_Escape); ui.mouse(document, moved, 0, false);
    require(document.snapshot_owner() == original && !document.can_undo(), "Escape committed a canceled node drag");
    ui.mouse(document, start);
    require(ui.context->HoveredId == ui.node_id(kFirst), "Escape did not restore the original visible position");
}

void test_delete_and_undo() {
    application::EditorFeatureGraphDocument document{make_graph(true), make_catalog()};
    const auto original = document.snapshot_owner(); Harness ui;
    ui.draw(document); ui.draw(document); ui.click(document, ui.at(490, 80));
    ui.key(document, ImGuiKey_Delete);
    require(document.snapshot().nodes.size() == 2 && document.snapshot().edges.empty(),
            "Delete did not remove the selected canvas node and both incident edges");
    require(std::none_of(document.snapshot().nodes.begin(), document.snapshot().nodes.end(), [](const auto& node) {
        return node.node_id == kSecond;
    }), "Delete removed the low identity instead of the selected high identity");
    require(document.snapshot().graph_version == original->graph_version + 1 &&
            document.snapshot().draft_revision == original->draft_revision + 1,
            "Canvas Delete did not use one semantic edit");
    require(static_cast<bool>(document.undo()) && document.snapshot_owner() == original && !document.can_undo(),
            "Undo did not restore the exact deleted graph and layout snapshot");
}

void test_external_selection_drives_canvas_actions() {
    application::EditorFeatureGraphDocument document{make_graph(true), make_catalog()};
    const auto original = document.snapshot_owner();
    Harness ui; ui.external_selection_mode = true;
    ui.draw(document); ui.draw(document);
    ui.click(document, ui.at(140, 80));
    require(ui.selected_node == std::to_string(kFirst.value()), "Canvas did not publish primary selection");
    ui.click(document, ui.external_select);
    require(document.snapshot_owner() == original && !document.can_undo(),
            "External selection mutated document or history");
    require(ui.selected_node == std::to_string(kSecond.value()) &&
            ui.rendered.find("Selected nodes: 1") != std::string::npos,
            "External primary selection did not reach the canvas");
    ui.click(document, ui.delete_selected);
    require(document.snapshot().nodes.size() == 2 &&
            std::none_of(document.snapshot().nodes.begin(), document.snapshot().nodes.end(), [](const auto& node) {
                return node.node_id == kSecond;
            }) && std::any_of(document.snapshot().nodes.begin(), document.snapshot().nodes.end(), [](const auto& node) {
                return node.node_id == kFirst;
            }), "Delete selected targeted the old canvas selection instead of the externally selected node");
    require(static_cast<bool>(document.undo()) && document.snapshot_owner() == original,
            "Undo did not restore the external-selection delete");
    ui.draw(document); ui.click(document, ui.at(140, 80));
    ui.click(document, ui.at(1250, 300));
    require(ui.selected_node.empty() && ui.rendered.find("Selected nodes:") == std::string::npos,
            "Blank canvas click left the shared Inspector selection active");
    require(document.snapshot_owner() == original && !document.can_undo(),
            "Selection synchronization or deselection created an authoring transaction");
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view selected = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
    const auto run = [&](std::string_view name, auto test) {
        if (!selected.empty() && selected != name) return;
        test(); std::cout << name << " passed\n";
    };
    run("--visible", test_visible_nodes_and_ports);
    run("--drag", test_port_drag_transaction_and_rejection);
    run("--copy", test_selection_copy_internal_edges_only);
    run("--move", test_move_is_one_layout_transaction);
    run("--delete", test_delete_and_undo);
    run("--external-selection", test_external_selection_drives_canvas_actions);
    require(selected.empty() || selected == "--visible" || selected == "--drag" || selected == "--copy" ||
            selected == "--move" || selected == "--delete" || selected == "--external-selection", "Unknown canvas test case");
}
