/** @brief Exercises the real Process state subview through ImGui input. */
#include "editor_feature_graph_panel.hpp"
#include <imgui_internal.h>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <string_view>
#include <vector>

namespace {
using namespace aeg;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

feature::FeatureCatalog make_catalog() {
    feature::FeatureDefinition definition{};
    definition.id = feature::FeatureDefinitionId{"Logic.ProcessCanvas"};
    definition.version = {1, 0, 0};
    auto& contract = definition.contract;
    contract.definition_id = definition.id;
    contract.version = definition.version;
    contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
    contract.execution = feature::ExecutionKind::kProcess;
    contract.state = feature::StatePolicy::kProcessState;
    contract.has_continuation = true;
    contract.scope = {feature::ScopeId{"World"}};
    contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId{"LogicTick"}, 60, 4, feature::LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    feature::FeatureCatalog catalog;
    require(static_cast<bool>(catalog.export_definition(definition)), "Process fixture contract rejected");
    return catalog;
}

feature::FeatureGraphDocument make_graph() {
    feature::FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://process-canvas";
    graph.schema_version = 1;
    graph.graph_version = 7;
    graph.draft_revision = 4;
    graph.layout_revision = 5;
    graph.nodes = {{feature::FeatureNodeId{1}, feature::FeatureDefinitionId{"Logic.ProcessCanvas"}, {1, 0, 0}}};
    graph.process_subgraphs = {{feature::FeatureNodeId{1}, {"Idle", "Active", "Done"},
                               {"Idle->Active", "Active->Done"}, 1, "Idle", {"Done"}}};
    return graph;
}

class PanelHarness final {
public:
    PanelHarness() {
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1400, 3200};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableDebugLog = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        context->ErrorCallbackUserData = &errors;
        context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
            static_cast<std::vector<std::string>*>(data)->emplace_back(message);
        };
    }
    ~PanelHarness() {
        ImGui::DestroyContext(context);
        for (const auto& message : errors) std::cerr << message << '\n';
        require(errors.empty(), "ImGui reported a Process panel error");
    }
    void draw(application::EditorFeatureGraphDocument& document, const char* button = nullptr,
              bool select_node = false, const char* delete_state = nullptr) {
        ImGui::NewFrame();
        if (button || select_node || delete_state) {
            auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
            require(window != nullptr, "FeatureGraph window missing");
            ImGuiID id{};
            if (delete_state) {
                const auto& process = document.snapshot().process_subgraphs.front();
                const auto state = std::find(process.states.begin(), process.states.end(), delete_state);
                require(state != process.states.end(), "Requested inspector state missing");
                const auto owner_id = ImHashStr(std::to_string(process.owner_node.value()).c_str(), 0, window->ID);
                const int state_index = static_cast<int>(state - process.states.begin());
                id = ImHashStr("Delete state", 0, ImHashData(&state_index, sizeof(state_index), owner_id));
            } else id = select_node ? ImHashStr("1", 0, window->GetID("FeatureGraphNodes"))
                                    : window->GetID(button);
            context->NavWindow = window;
            context->NavId = id;
            context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = id;
        }
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        application::EditorFeatureGraphPanel::Actions actions;
        if (graph_selector) {
            ImGuiWindowClass window_class;
            window_class.ClassId = ImHashStr("FlowGraph selector regression");
            window_class.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
            ImGui::SetNextWindowClass(&window_class);
            actions.draw_graph_selector = [&] {
                auto* window = ImGui::GetCurrentWindow();
                require(window == ImGui::FindWindowByName("Flow Graph###FlowGraph") &&
                        window->WindowClass.ClassId == ImHashStr("FlowGraph selector regression"),
                        "Graph selector consumed native hints before the FlowGraph Begin");
                selector_in_window = true;
                if (ImGui::BeginCombo("Select FeatureGraph", "Process.aegfeature")) ImGui::EndCombo();
            };
        }
        panel.draw(document, "Process.aegfeature", actions);
        ImGui::Render();
    }
    void draw_canvas(application::EditorFeatureGraphDocument& document, const char* button = nullptr) {
        ImGui::NewFrame();
        if (button) {
            auto* window = ImGui::FindWindowByName("Process subview test");
            require(window != nullptr, "Canvas window missing");
            context->NavWindow = window;
            context->NavId = window->GetID(button);
            context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = context->NavId;
        }
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1100, 800});
        ImGui::Begin("Process subview test");
        canvas.draw(document, feature::FeatureNodeId{1}, feedback);
        ImGui::End();
        if (overlay) {
            ImGui::SetNextWindowPos({origin_for_overlay.x + 530, origin_for_overlay.y + 40});
            ImGui::SetNextWindowSize({230, 150}); ImGui::SetNextWindowFocus();
            ImGui::Begin("Blocking overlay"); ImGui::TextUnformatted("The target is covered."); ImGui::End();
        }
        ImGui::Render();
    }
    ImVec2 canvas_origin() const {
        for (const auto* window : context->Windows)
            if (std::string_view{window->Name}.find("Process subview test/") == 0 &&
                std::string_view{window->Name}.find("ProcessCanvas") != std::string_view::npos)
                return window->DC.CursorStartPos;
        require(false, "Process canvas child window missing"); return {};
    }
    void mouse(application::EditorFeatureGraphDocument& document, ImVec2 position, int button = -1, bool down = false) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        if (button >= 0) io.AddMouseButtonEvent(button, down);
        draw_canvas(document);
    }
    ImGuiContext* context{};
    application::EditorFeatureGraphPanel panel{};
    application::EditorProcessGraphCanvas canvas{};
    std::string feedback{};
    bool overlay{false};
    bool graph_selector{false};
    bool selector_in_window{false};
    ImVec2 origin_for_overlay{};
    std::vector<std::string> errors{};
};
}

int main() {
    {
    auto graph = make_graph();
    auto& process = graph.process_subgraphs.front();
    process.states = {"A###same", "B###same"}; process.transitions = {"A###same->B###same"};
    process.initial_state = "A###same"; process.terminal_states = {"B###same"};
    application::EditorFeatureGraphDocument document{std::move(graph), make_catalog()};
    PanelHarness ui;
    ui.draw(document); ui.draw(document, nullptr, false, "A###same");
    require(document.snapshot().process_subgraphs.front().states == std::vector<std::string>{"B###same"},
            "Text state inspector deleted another literal-name state");
    }
    {
    application::EditorFeatureGraphDocument document{make_graph(), make_catalog()};
    const auto original = document.snapshot_owner();
    PanelHarness ui;
    ui.graph_selector = true;
    ui.draw(document);
    require(ui.selector_in_window, "FeatureGraph selector did not draw inside its owning window");
    ui.draw(document, nullptr, true);
    ui.draw(document, "Use selected as Process owner");
    ui.draw(document, "Auto layout states");
    require(document.snapshot().layout_revision == original->layout_revision + 1,
            "Process state graph Auto layout did not persist a layout transaction");
    require(document.snapshot().graph_version == original->graph_version &&
            document.snapshot().draft_revision == original->draft_revision,
            "Canvas layout changed executable semantics");
    require(document.can_undo(), "Process layout cannot be undone");
    require(static_cast<bool>(document.undo()), "Process layout Undo failed");
    require(document.snapshot().layout_revision == original->layout_revision && !document.dirty(),
            "Undo did not restore the clean original graph");
    require(static_cast<bool>(document.redo()), "Process layout Redo failed");
    }
    {
    application::EditorFeatureGraphDocument document{make_graph(), make_catalog()};
    application::ProcessEditorLayout layout;
    layout.state_positions = {{"Idle", {40.0f, 60.0f}}, {"Active", {40.0f, 60.0f}}, {"Done", {560.0f, 60.0f}}};
    require(static_cast<bool>(document.set_process_layout(feature::FeatureNodeId{1}, layout)), "Overlapping layout failed");
    PanelHarness ui;
    ui.draw_canvas(document); ui.draw_canvas(document);
    const auto origin = ui.canvas_origin(); const ImVec2 top_card{origin.x + 65, origin.y + 85};
    ui.mouse(document, top_card); ui.mouse(document, top_card, 0, true);
    require(ui.canvas.selected_state() == "Active", "Click selected covered state instead of topmost visible card");
    ui.mouse(document, top_card, 0, false);
    }
    {
    auto graph = make_graph();
    graph.process_subgraphs.front().states = {"A###same", "B###same"};
    graph.process_subgraphs.front().transitions = {"A###same->B###same"};
    graph.process_subgraphs.front().initial_state = "A###same";
    graph.process_subgraphs.front().terminal_states = {"B###same"};
    application::EditorFeatureGraphDocument document{std::move(graph), make_catalog()};
    application::ProcessEditorLayout layout;
    layout.state_positions = {{"A###same", {40.0f, 60.0f}}, {"B###same", {300.0f, 60.0f}}};
    require(static_cast<bool>(document.set_process_layout(feature::FeatureNodeId{1}, layout)), "Literal-name layout failed");
    PanelHarness ui;
    ui.draw_canvas(document); ui.draw_canvas(document);
    const auto origin = ui.canvas_origin(); const ImVec2 a{origin.x + 65, origin.y + 85};
    ui.mouse(document, a); ui.mouse(document, a, 0, true);
    require(ui.canvas.selected_state() == "A###same", "Distinct literal state names share one ImGui interaction ID");
    ui.mouse(document, a, 0, false);
    document = application::EditorFeatureGraphDocument{document.snapshot(), make_catalog()};
    ui.draw_canvas(document);
    require(ui.canvas.selected_state().empty(), "Same-object reopen retained old state selection");
    }
    {
    application::EditorFeatureGraphDocument document{make_graph(), make_catalog()};
    application::ProcessEditorLayout seed;
    seed.state_positions = {{"Idle", {40.0f, 60.0f}}, {"Active", {300.0f, 60.0f}}, {"Done", {560.0f, 60.0f}}};
    require(static_cast<bool>(document.set_process_layout(feature::FeatureNodeId{1}, seed)), "Seed layout rejected");
    document.mark_saved();
    const auto base = document.snapshot_owner();
    PanelHarness ui;
    ui.draw_canvas(document); ui.draw_canvas(document);
    require(!document.dirty(), "Opening Process canvas dirtied document");
    const auto origin = ui.canvas_origin();
    const ImVec2 idle{origin.x + 65, origin.y + 85};
    ui.mouse(document, idle); ui.mouse(document, idle, 0, true);
    ui.mouse(document, {idle.x + 23, idle.y + 14});
    ui.mouse(document, {idle.x + 63, idle.y + 34});
    require(document.snapshot_owner() == base, "Drag preview committed before release");
    ui.mouse(document, {idle.x + 63, idle.y + 34}, 0, false);
    const auto moved = application::read_process_editor_layout(document.snapshot(), feature::FeatureNodeId{1});
    require(moved && moved.value.state_positions.at("Idle") == std::pair{100.0f, 100.0f},
            "Mouse drag did not persist snapped world coordinates");
    require(document.snapshot().layout_revision == base->layout_revision + 1 &&
            document.snapshot().graph_version == base->graph_version, "Drag was not one layout-only transaction");
    require(static_cast<bool>(document.undo()) && document.snapshot_owner() == base && !document.dirty(),
            "One Undo did not revert complete drag");
    ui.draw_canvas(document);

    // Click without moving keeps an unsnapped authored coordinate unchanged.
    seed.state_positions.at("Idle") = {43.0f, 63.0f};
    require(static_cast<bool>(document.set_process_layout(feature::FeatureNodeId{1}, seed)), "Unsnapped seed failed");
    document.mark_saved(); ui.draw_canvas(document);
    const auto click_base = document.snapshot_owner();
    const ImVec2 unsnapped{origin.x + 65, origin.y + 85};
    ui.mouse(document, unsnapped); ui.mouse(document, unsnapped, 0, true); ui.mouse(document, unsnapped, 0, false);
    require(document.snapshot_owner() == click_base, "Selecting a state snapped or persisted its layout");
    require(ui.canvas.selected_state() == "Idle", "Mouse did not select state");

    application::EditorFeatureGraphDocument idle_other{make_graph(), make_catalog()};
    ui.draw_canvas(idle_other);
    require(ui.canvas.selected_state().empty(), "Root B inherited idle state selection from root A");
    ui.draw_canvas(document);

    // A preview from root A cannot mutate root B with the same IDs/revisions.
    ui.mouse(document, unsnapped, 0, true); ui.mouse(document, {unsnapped.x + 80, unsnapped.y + 40});
    application::EditorFeatureGraphDocument other{make_graph(), make_catalog()};
    const auto other_base = other.snapshot_owner();
    ui.mouse(other, {unsnapped.x + 80, unsnapped.y + 40}, 0, false);
    require(other.snapshot_owner() == other_base && !other.dirty(), "Cross-root release wrote old drag preview");
    ui.draw_canvas(document);
    require(document.snapshot_owner() == click_base, "Switching roots committed abandoned preview");

    // Escape cancels a live drag; no history transaction is created on release.
    ui.mouse(document, unsnapped); ui.mouse(document, unsnapped, 0, true); ui.mouse(document, {unsnapped.x + 40, unsnapped.y + 20});
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); ui.draw_canvas(document);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    ui.mouse(document, {unsnapped.x + 40, unsnapped.y + 20}, 0, false);
    require(document.snapshot_owner() == click_base, "Escape committed canceled drag");

    // Connect the real output dot to Done, then Undo only that semantic edit.
    ui.draw_canvas(document);
    const ImVec2 output{origin.x + 43 + 140, origin.y + 63 + 29};
    const ImVec2 done{origin.x + 585, origin.y + 85};
    ui.mouse(document, output); ui.mouse(document, output, 0, true); ui.mouse(document, done);
    require(document.snapshot_owner() == click_base, "Connection preview committed before release");
    ui.mouse(document, done, 0, false);
    const auto& transitions = document.snapshot().process_subgraphs.front().transitions;
    require(std::find(transitions.begin(), transitions.end(), "Idle->Done") != transitions.end(),
            "Output-port drag did not add Process transition");
    require(document.snapshot().graph_version == click_base->graph_version + 1,
            "Connection did not use semantic Process transaction");
    require(static_cast<bool>(document.undo()) && document.snapshot_owner() == click_base,
            "Undo did not remove connection in one transaction");

    ui.draw_canvas(document); ui.mouse(document, output); ui.mouse(document, output, 0, true);
    ui.origin_for_overlay = origin; ui.overlay = true;
    ui.mouse(document, done); ui.mouse(document, done, 0, false);
    require(document.snapshot_owner() == click_base, "Connection dropped through a covering window");
    ui.overlay = false; ui.draw_canvas(document);

    // An incomplete draft remains visible and auto-layoutable before validation.
    require(static_cast<bool>(document.add_process_state(feature::FeatureNodeId{1}, "Unconnected")), "Draft state add failed");
    const auto draft = document.snapshot_owner();
    ui.draw_canvas(document); ui.draw_canvas(document, "Auto layout states");
    const auto draft_layout = application::read_process_editor_layout(document.snapshot(), feature::FeatureNodeId{1});
    require(draft_layout && draft_layout.value.state_positions.contains("Unconnected") &&
            document.snapshot().graph_version == draft->graph_version, "Incomplete Process draft could not be laid out");
    }
    std::cout << "Process canvas UI transactions passed\n";
}
