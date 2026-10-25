/** @brief Exercises draft validation/compilation with real ImGui mouse input. */
#include "editor_feature_graph_panel.hpp"

#include <imgui_internal.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace aeg;
using Document = application::EditorFeatureGraphDocument;
constexpr feature::FeatureNodeId kCompositeNode{uint64_t{7}};
constexpr feature::FeatureNodeId kRenderNode{uint64_t{8}};
constexpr feature::FeatureNodeId kAddedNode{uint64_t{9}};
constexpr feature::PortId kChildOutput{uint64_t{11}};
constexpr feature::PortId kOutput{uint64_t{12}};
constexpr feature::PortId kInput{uint64_t{13}};

void require(bool condition, std::string_view message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

feature::FeatureDefinition atomic(uint64_t id, feature::FeatureDomain domain, feature::ClockId clock) {
    feature::FeatureDefinition definition;
    definition.id = feature::FeatureDefinitionId{id}; definition.version = {1, 0, 0};
    auto& contract = definition.contract;
    contract.definition_id = definition.id; contract.version = definition.version;
    contract.domains = feature::to_mask(domain);
    contract.execution = feature::ExecutionKind::kCall;
    contract.state = feature::StatePolicy::kStateless;
    contract.scope = {feature::ScopeId{"World"}};
    contract.clock = feature::make_periodic_clock_contract(clock, 60, 4,
        feature::LateDataPolicy::kReject, feature::DeadlinePolicy::kObserve, 250000, 0);
    contract.permissions.version = 1;
    return definition;
}

feature::FeatureCatalog make_catalog(bool same_domain = false) {
    feature::FeatureCatalog catalog;
    auto child = atomic(2, feature::FeatureDomain::kLogic, feature::ClockId{"UIClock"});
    child.contract.ports = {{kChildOutput, feature::PortDirection::kOutput, feature::ValueSchemaId{"S"}, 1}};
    require(bool(catalog.export_definition(child)), "Logic child fixture was rejected");
    auto composite = atomic(1, feature::FeatureDomain::kLogic, feature::ClockId{"UIClock"});
    auto& contract = composite.contract;
    contract.structure = feature::FeatureStructure::kComposite;
    contract.clock.work_budget_ns = 0;
    contract.children = {child.id};
    contract.domains |= feature::to_mask(feature::FeatureDomain::kRender);
    contract.ports = {{kOutput, feature::PortDirection::kOutput, feature::ValueSchemaId{"S"}, 1}};
    contract.child_port_mappings = {{kOutput, child.id, kChildOutput, feature::PortDirection::kOutput}};
    const auto exported = catalog.export_definition(composite);
    require(bool(exported), exported.message);
    auto render = atomic(3, same_domain ? feature::FeatureDomain::kLogic : feature::FeatureDomain::kRender,
                         feature::ClockId{"RenderClock"});
    render.contract.ports = {{kInput, feature::PortDirection::kInput, feature::ValueSchemaId{"S"}, 1}};
    require(bool(catalog.export_definition(render)), "Render fixture was rejected");
    return catalog;
}

feature::FeatureGraphDocument make_graph() {
    feature::FeatureGraphDocument graph;
    graph.asset_uuid = "00000000-0000-0000-0000-000000000081";
    graph.schema_version = 1; graph.graph_version = 5; graph.draft_revision = 7;
    graph.nodes = {{kCompositeNode, feature::FeatureDefinitionId{uint64_t{1}}, {1, 0, 0}, {}, {}},
                   {kRenderNode, feature::FeatureDefinitionId{uint64_t{3}}, {1, 0, 0}, {}, {}}};
    graph.edges = {{kCompositeNode, kOutput, kRenderNode, kInput, feature::EdgeKind::kSnapshot,
                    feature::ClockId{"UIClock"}, feature::ClockId{"RenderClock"}, 1, 0, 1}};
    return graph;
}

class Harness final {
public:
    Harness() {
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = {1900, 5000}; io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigErrorRecoveryEnableAssert = io.ConfigErrorRecoveryEnableDebugLog =
            io.ConfigErrorRecoveryEnableTooltip = false;
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        context->ErrorCallbackUserData = &errors;
        context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
            static_cast<std::vector<std::string>*>(data)->emplace_back(message);
        };
        // Trap accidental Runtime inspection from draft preview. No executable module is supplied.
        actions.inspect_execution = [this](std::string_view) {
            ++runtime_calls;
            return foundation::ResultValue<application::runtime::FeatureRuntimeExecutionSnapshot>{
                foundation::invalid_state("Draft preview must not inspect or publish Runtime"), {}};
        };
    }
    ~Harness() { ImGui::DestroyContext(context); }

    void draw(Document& document) {
        ImGui::NewFrame();
        ImGui::Begin("Orchestration capture", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        panel.draw(document, source_path, actions);
        rendered = context->LogBuffer.c_str();
        ImGui::LogFinish(); ImGui::End(); ImGui::Render();
        for (const auto& error : errors) std::cerr << error << '\n';
        require(errors.empty(), "ImGui reported an error while drawing orchestration");
    }
    bool contains(std::string_view text) const { return rendered.find(text) != std::string::npos; }
    void expect(std::string_view text) const {
        if (!contains(text)) { std::cerr << "Missing UI text: " << text << "\n" << rendered; std::exit(1); }
    }
    std::string_view section(std::string_view start, std::string_view end) const {
        const auto first = rendered.find(start);
        require(first != std::string::npos, std::string{"Missing section: "} + std::string{start});
        const auto last = rendered.find(end, first + start.size());
        require(last != std::string::npos, std::string{"Missing section terminator: "} + std::string{end});
        return std::string_view{rendered}.substr(first, last - first);
    }
    void mouse(Document& document, ImVec2 position, int button = -1, bool down = false) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(position.x, position.y);
        if (button >= 0) io.AddMouseButtonEvent(button, down);
        draw(document);
    }
    ImGuiID click(Document& document, ImVec2 position) {
        mouse(document, position);
        const auto hovered = context->HoveredId;
        require(hovered != 0, "Mouse target is not an actual interactive item");
        mouse(document, position, 0, true);
        mouse(document, position, 0, false);
        draw(document); // Feedback above the button becomes visible on the next frame.
        return hovered;
    }
    ImVec2 button_position(Document& document, const char* label) {
        auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        require(window != nullptr, "Flow Graph window is absent");
        const ImGuiID wanted = window->GetID(label);
        if (context->NavWindow == nullptr || context->NavWindow->RootWindow != window) {
            // The independent Definition window is also rendered by the panel. Focus this
            // window through its visible title bar before tabbing through its controls.
            const ImVec2 title{window->Pos.x + 100.0f, window->Pos.y + ImGui::GetFontSize() * 0.5f};
            mouse(document, title); mouse(document, title, 0, true); mouse(document, title, 0, false);
        }
        if ((context->NavIdItemFlags & ImGuiItemFlags_Disabled) != 0) {
            // Disabling the focused item leaves its navigation ID alive, but Tab
            // scoring skips disabled items and never encounters that starting ID.
            // A backwards Tab can choose an enabled item even when the former
            // focus is no longer eligible, restoring normal forward navigation.
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Shift, true); io.AddKeyEvent(ImGuiKey_Tab, true); draw(document);
            io.AddKeyEvent(ImGuiKey_Tab, false); io.AddKeyEvent(ImGuiMod_Shift, false); draw(document);
            require(context->NavId != 0 && (context->NavIdItemFlags & ImGuiItemFlags_Disabled) == 0,
                    "Backwards Tab did not recover an enabled navigation target");
        }
        std::string navigation_trace;
        ImGuiID previous_navigation = 0;
        // Discover the actual rectangle with keyboard navigation. This sends Tab input only;
        // it never writes NavId/NavActivateId or calls panel internals to trigger an action.
        for (unsigned attempt = 0; attempt < 300; ++attempt) {
            if (previous_navigation != context->NavId) {
                previous_navigation = context->NavId;
                navigation_trace += std::to_string(previous_navigation) + ":" +
                    (context->NavWindow ? context->NavWindow->Name : "(none)") + " ";
            }
            if (context->NavId == wanted && context->NavWindow == window && context->NavIdIsAlive) {
                // ImGui stores navigation rectangles relative to CursorStartPos (which
                // includes title/content padding and scrolling), not window->Pos.
                const auto point = ImGui::WindowRectRelToAbs(window,
                    window->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
                // A real pointer movement also exits keyboard-only hover mode. The
                // first position after startup has no delta, so settle at a nearby
                // position before moving onto the observed item center.
                mouse(document, {point.x + 2.0f, point.y});
                mouse(document, point);
                if (context->HoveredId != wanted) {
                    std::cerr << "Navigation rectangle miss: " << label << " point=" << point.x << ',' << point.y
                              << " expected=" << wanted << " hovered=" << context->HoveredId
                              << " active=" << context->ActiveId << " mouse=" << context->IO.MousePos.x << ',' << context->IO.MousePos.y
                              << " events=" << context->InputEventsQueue.Size
                              << " navHighlight=" << context->NavHighlightItemUnderNav
                              << " nav=" << context->NavId << " window="
                              << (context->NavWindow ? context->NavWindow->Name : "(none)")
                              << " hovered-window=" << (context->HoveredWindow ? context->HoveredWindow->Name : "(none)")
                              << " nav-rect=" << window->NavRectRel[ImGuiNavLayer_Main].Min.x << ','
                              << window->NavRectRel[ImGuiNavLayer_Main].Min.y << ".."
                              << window->NavRectRel[ImGuiNavLayer_Main].Max.x << ','
                              << window->NavRectRel[ImGuiNavLayer_Main].Max.y
                              << " clip=" << window->ClipRect.Min.x << ',' << window->ClipRect.Min.y << ".."
                              << window->ClipRect.Max.x << ',' << window->ClipRect.Max.y
                              << " viewport=" << context->MouseViewport->Pos.x << ',' << context->MouseViewport->Pos.y
                              << " size=" << context->MouseViewport->Size.x << ',' << context->MouseViewport->Size.y << '\n';
                }
                require(context->HoveredId == wanted, "Discovered button rectangle did not hit its actual ID");
                return point;
            }
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiKey_Tab, true); draw(document);
            io.AddKeyEvent(ImGuiKey_Tab, false); draw(document);
        }
        std::cerr << "Navigation exhausted: expected=" << wanted << " trace=" << navigation_trace
                  << " active=" << context->ActiveId << " nav-active=" << context->IO.NavActive
                  << " events=" << context->InputEventsQueue.Size << " apply=" << bool(actions.apply_graph)
                  << " load=" << bool(actions.select_saved_module) << "\n";
        for (const char* candidate : {"Node catalog", "Check draft", "Process editor", "Module manifest", "Configuration", "Apply", "Load Saved Project"})
            std::cerr << candidate << '=' << window->GetID(candidate) << '\n';
        require(false, std::string{"Cannot navigate to enabled button: "} + label);
        return {};
    }
    void click_button(Document& document, const char* label) {
        const auto point = button_position(document, label);
        auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        require(click(document, point) == window->GetID(label), "Mouse click was delivered to a different widget");
    }
    void enter_text(Document& document, const char* label, const char* value) {
        click_button(document, label);
        require(ImGui::GetIO().WantTextInput, "Real text input did not receive focus from a mouse click");
        auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); draw(document);
        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); draw(document);
        io.AddInputCharactersUTF8(value); draw(document);
        io.AddKeyEvent(ImGuiKey_Enter, true); draw(document);
        io.AddKeyEvent(ImGuiKey_Enter, false); draw(document);
    }
    ImVec2 lane_node(unsigned column, unsigned row) const {
        auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        const auto* table = window ? ImGui::TableFindByID(window->GetID("FiveDomainLanes")) : nullptr;
        require(table && table->ColumnsCount == 5, "Five domain lane table is absent");
        return {table->Columns[column].WorkMinX + 12.0f,
            table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y +
                ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(row + 1) +
                ImGui::GetTextLineHeight() * 0.5f};
    }

    ImGuiContext* context{};
    application::EditorFeatureGraphPanel panel;
    application::EditorFeatureGraphPanel::Actions actions{};
    unsigned runtime_calls{0};
    std::filesystem::path source_path{"Orchestration.aegfeature"};
    std::string rendered;
    std::vector<std::string> errors;
};

struct AuthorState final {
    std::shared_ptr<const feature::FeatureGraphDocument> snapshot;
    uint64_t revision, catalog_digest;
    bool dirty, undo, redo;
    explicit AuthorState(const Document& document)
        : snapshot(document.snapshot_owner()), revision(document.revision()),
          catalog_digest(document.catalog().contract_digest()), dirty(document.dirty()),
          undo(document.can_undo()), redo(document.can_redo()) {}
    void unchanged(const Document& document) const {
        require(document.snapshot_owner() == snapshot && document.revision() == revision &&
            document.catalog().contract_digest() == catalog_digest && document.dirty() == dirty &&
            document.can_undo() == undo && document.can_redo() == redo,
            "Preview UI changed the document, catalog, dirty checkpoint, or undo/redo history");
    }
};

void test_real_validate_compile_and_last_good() {
    Document document{make_graph(), make_catalog()};
    Harness ui; const AuthorState initial{document};
    ui.draw(document); ui.draw(document);
    for (const char* label : {"Logic", "Physics", "Render", "Audio", "Stream", "Validate draft", "Compile preview"})
        ui.expect(label);
    require(!ui.contains("Preview: last-good"), "Opening a graph silently compiled a successful preview before a click");
    ui.click_button(document, "Validate draft");
    ui.expect("Draft validation passed");
    require(!ui.contains("Preview: last-good"), "Validate draft published a compiled preview");
    initial.unchanged(document);
    ui.click_button(document, "Compile preview");
    ui.expect("Authoring preview compiled"); ui.expect("Authoring preview");
    ui.expect("Preview: last-good"); ui.expect("Clock groups"); ui.expect("Provenance (2)");
    ui.expect("Compiled edge 0: Snapshot");
    require(!ui.contains("last-good (stale)"), "Fresh successful compile was marked stale");
    initial.unchanged(document);

    // Import/edit a malformed draft through the normal undo transaction boundary. The UI's
    // connection editor correctly rejects this edge, but files/catalog upgrades may expose it.
    require(bool(document.execute("Imported invalid cross-domain Data edge", [](auto& graph) {
        graph.edges.front().kind = feature::EdgeKind::kData;
        graph.edges.front().deadline_ticks = 0;
        graph.nodes.push_back({kAddedNode, feature::FeatureDefinitionId{uint64_t{3}}, {1, 0, 0}, {}, {}});
        ++graph.graph_version; ++graph.draft_revision;
        return foundation::Result::success();
    })), "Could not install the malformed draft fixture");
    const AuthorState invalid{document};
    auto validation_graph = document.snapshot();
    const auto invalid_edge = validation_graph.edges.front(); validation_graph.edges.clear();
    const auto rejected = feature::validate_feature_edge(validation_graph, invalid_edge, document.catalog());
    require(!rejected, "Illegal cross-domain Data edge fixture unexpectedly passed the production validator");
    ui.draw(document); ui.expect("last-good (stale)");
    ui.click_button(document, "Compile preview");
    ui.expect("Compile preview blocked"); ui.expect("last-good (stale)");
    ui.expect(rejected.message); ui.expect("Node 7"); ui.expect("Port 12"); ui.expect("Edge 0");
    ui.expect("Provenance (2)"); // Retained successful snapshot, not the malformed three-node draft.
    ui.expect("Compiled edge 0: Snapshot");
    const auto lanes = ui.section("Current draft -", "Contract Inspector");
    require(lanes.find("Render (3)") != std::string_view::npos,
            "Invalid current draft lanes still project the old two-node last-good graph");
    const auto edges = ui.section("Cross-domain edges / transport diagnostics", "Compiled authoring preview");
    require(edges.find("| Data |") != std::string_view::npos && edges.find(rejected.message) != std::string_view::npos,
            "Current edge row is missing the illegal Data transport or its production diagnostic");
    invalid.unchanged(document);
    ui.click_button(document, "Validate draft"); ui.expect("Draft validation found errors");
    ui.expect("last-good (stale)"); ui.expect(rejected.message); invalid.unchanged(document);

    require(bool(document.undo()), "Undo of the imported draft failed");
    require(document.snapshot_owner() == initial.snapshot, "Undo did not restore the successful authored snapshot");
    const AuthorState undone{document};
    ui.draw(document); ui.click_button(document, "Compile preview");
    ui.expect("Authoring preview compiled"); require(!ui.contains("last-good (stale)"), "Recompile after Undo stayed stale");
    undone.unchanged(document);
    require(bool(document.redo()), "Redo of the malformed draft failed");
    const AuthorState redone{document};
    ui.draw(document); ui.click_button(document, "Compile preview");
    ui.expect("Compile preview blocked"); ui.expect("last-good (stale)"); ui.expect("Provenance (2)");
    redone.unchanged(document);
    require(ui.runtime_calls == 0, "Validate/Compile called Runtime inspection");
}

void test_apply_gate_and_same_domain_cross_clock() {
    Document document{make_graph(), make_catalog(true)};
    Harness ui; const AuthorState original{document};
    unsigned publications = 0;
    unsigned loads = 0;
    const auto publish = [&](const feature::FeatureGraphDocument&) {
        ++publications; return foundation::Result::success();
    };
    ui.actions.select_saved_module = [&](const std::filesystem::path&, std::string_view) {
        ++loads; return foundation::Result::success();
    };
    // Calibrate the real Apply rectangle with an available host action. The positive
    // control later remeasures and activates the actual button in its enabled layout.
    ui.actions.apply_graph = publish;
    ui.draw(document); ui.draw(document);
    const auto enabled_anchor = ui.button_position(document, "Load Saved Project");
    const auto apply_point = ui.button_position(document, "Apply");
    const auto apply_id = ImGui::FindWindowByName("Flow Graph###FlowGraph")->GetID("Apply");
    require(apply_point.y == enabled_anchor.y && apply_point.x > enabled_anchor.x,
            "Apply calibration is not on the same toolbar row as Load Saved Project");
    ui.actions.apply_graph = {};
    ui.mouse(document, {1800, 10}); ui.draw(document);
    // Removing the host action adds no-module guidance above the toolbar. Locate the
    // enabled adjacent item again and translate within the actual new row, rather than
    // clicking the now-obsolete enabled Apply position and calling that a disabled test.
    const auto disabled_anchor = ui.button_position(document, "Load Saved Project");
    const ImVec2 disabled_apply{disabled_anchor.x + apply_point.x - enabled_anchor.x, disabled_anchor.y};
    ui.mouse(document, disabled_apply);
    require(ui.context->HoveredId == apply_id && ui.context->HoveredIdIsDisabled,
            "No-module Apply target was not the actual disabled button");
    ui.mouse(document, disabled_apply, 0, true);
    require(ui.context->ActiveId != apply_id, "No-module Apply accepted a mouse press");
    ui.mouse(document, disabled_apply, 0, false); ui.draw(document);
    require(publications == 0 && loads == 0, "Disabled Apply activated a publication or adjacent load action");
    ui.click_button(document, "Validate draft"); ui.expect("Draft validation passed");
    ui.click_button(document, "Compile preview"); ui.expect("Authoring preview compiled");
    const auto edges = ui.section("Cross-domain edges / transport diagnostics", "Compiled authoring preview");
    require(edges.find("Cross-clock") != std::string_view::npos && edges.find("| Snapshot |") != std::string_view::npos &&
                edges.find("| OK |") != std::string_view::npos,
            "Same-domain cross-clock Snapshot edge is absent from current transport diagnostics");
    require(publications == 0 && ui.runtime_calls == 0, "Draft actions invoked the publish or Runtime inspection callback");
    original.unchanged(document);
    ui.actions.apply_graph = publish;
    ui.draw(document);
    const auto enabled_again = ui.button_position(document, "Apply");
    require(ui.click(document, enabled_again) == apply_id, "Apply calibration hit a different item");
    require(publications == 1, "Available Apply positive control did not activate from the measured mouse position");
    require(loads == 0, "Apply test unexpectedly invoked the adjacent saved-project load action");
    original.unchanged(document);
}

void test_reopen_same_object_and_switch_path() {
    Document document{make_graph(), make_catalog()}; Harness ui;
    ui.draw(document); ui.draw(document); ui.click_button(document, "Compile preview"); ui.expect("Preview: last-good");
    const auto* address = &document;
    // Editor reload can replace a document value without changing its storage address/path/asset/revisions.
    document = Document{make_graph(), make_catalog()};
    require(address == &document, "Reopen fixture did not preserve the document address");
    const AuthorState reopened{document};
    ui.draw(document);
    require(!ui.contains("Preview: last-good"), "Same-address reopen retained the previous document's compiled preview");
    reopened.unchanged(document);
    ui.click_button(document, "Compile preview"); ui.expect("Preview: last-good");
    ui.source_path = "Other.aegfeature"; ui.draw(document);
    require(!ui.contains("Preview: last-good"), "Switching source path retained a compiled preview from another graph");
    reopened.unchanged(document);
}

void test_multi_domain_lane_selection() {
    Document document{make_graph(), make_catalog()}; Harness ui; const AuthorState original{document};
    std::vector<application::NodeDefinitionDraft> names;
    for (const auto& entry : document.catalog().entries()) {
        auto presentation = application::node_definition_from_catalog(entry);
        presentation.name = "Contract" + std::to_string(entry.definition_id.value());
        presentation.display_name = entry.definition_id.value() == 2 ? "Hidden child" : "Shared behavior";
        auto generated = presentation;
        generated.generated = true;
        generated.display_name = "Old generated label";
        names.push_back(std::move(generated));
        presentation.generated = false; // Matching recovery draft rename wins, regardless of scan order.
        names.push_back(std::move(presentation));
    }
    ui.actions.feature_presentations = names;
    ui.draw(document); ui.draw(document);
    ui.expect("Shared behavior");
    require(!ui.contains("Old generated label"), "Generated labels hid a matching saved draft rename");
    ui.click(document, ui.lane_node(2, 1)); ui.expect("Definition: 3");
    const auto logic_projection = ui.click(document, ui.lane_node(0, 0)); ui.expect("Definition: 1");
    ui.click(document, ui.lane_node(2, 1)); ui.expect("Definition: 3");
    const auto render_projection = ui.click(document, ui.lane_node(2, 0)); ui.expect("Definition: 1");
    require(logic_projection != render_projection,
        "Composite projections share a widget ID and interfere across lanes (they must share only the authored NodeId)");
    ui.expect("Contract Inspector"); ui.expect("Domain:"); ui.expect("Composite children:");
    const auto inspector = ui.section("Contract Inspector", "Cross-domain edges / transport diagnostics");
    require(inspector.find("Shared behavior") != std::string_view::npos && inspector.find("Definition: 1") != std::string_view::npos,
            "Equal presentation names obscured the selected contract's canonical identity");
    ui.enter_text(document, "Search name or definition id###Search definition id", "shared BEHAVIOR");
    const auto palette = ui.section("Choose a contract and Add node.", "Connect ports");
    require(palette.find("ID 1") != std::string_view::npos && palette.find("ID 3") != std::string_view::npos &&
                palette.find("ID 2") == std::string_view::npos,
            "Name filtering did not retain both equal-name definitions and exclude the unrelated child");
    ui.enter_text(document, "Search name or definition id###Search definition id", "Contract3");
    const auto exact_palette = ui.section("Choose a contract and Add node.", "Connect ports");
    require(exact_palette.find("ID 3") != std::string_view::npos && exact_palette.find("ID 1") == std::string_view::npos,
            "Technical name filtering did not identify the intended equal-display-name definition");
    original.unchanged(document);
}
} // namespace

int main() {
    test_real_validate_compile_and_last_good();
    test_apply_gate_and_same_domain_cross_clock();
    test_reopen_same_object_and_switch_path();
    test_multi_domain_lane_selection();
    std::cout << "Orchestration real-click acceptance passed\n";
}
