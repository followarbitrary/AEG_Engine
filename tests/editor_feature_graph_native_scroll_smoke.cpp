/** @brief Detached FeatureGraph content remains reachable by real mouse wheel input. */
#include "editor_feature_graph_panel.hpp"
#include <imgui_internal.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace aeg;
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
feature::FeatureCatalog catalog() {
    feature::FeatureCatalog result;
    for (uint64_t index = 1; index <= 32; ++index) {
        feature::FeatureDefinition definition;
        definition.id = feature::FeatureDefinitionId{index}; definition.version = {1, 0, 0};
        auto& contract = definition.contract;
        contract.definition_id = definition.id; contract.version = definition.version;
        contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
        contract.scope = {feature::ScopeId{"World"}};
        contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId{"LogicTick"}, 60, 4, feature::LateDataPolicy::kReject,
            aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
        contract.permissions.version = 1;
        require(static_cast<bool>(result.export_definition(definition)), "Native scroll fixture rejected");
    }
    return result;
}
}

int main() {
    feature::FeatureGraphDocument graph;
    graph.asset_uuid = "asset://native-scroll"; graph.schema_version = 1; graph.graph_version = 1;
    application::EditorFeatureGraphDocument document{graph, catalog()};
    const auto original = document.snapshot_owner();
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = {1680, 945}; io.DeltaTime = 1.0f / 60.0f;
    io.ConfigErrorRecoveryEnableAssert = io.ConfigErrorRecoveryEnableDebugLog = io.ConfigErrorRecoveryEnableTooltip = false;
    unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::vector<std::string> errors;
    context->ErrorCallbackUserData = &errors;
    context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
        static_cast<std::vector<std::string>*>(data)->emplace_back(message);
    };
    application::EditorFeatureGraphPanel panel;
    bool selector_in_root = false;
    ImRect selector_rect;
    application::EditorFeatureGraphPanel::Actions actions;
    actions.native_window = true;
    actions.draw_graph_selector = [&] {
        auto* root = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        require(ImGui::GetCurrentWindow() == root && root->WindowClass.ClassId == ImHashStr("Native scroll regression"),
                "Native root selector lost window class or chrome scope");
        selector_in_root = true;
        if (ImGui::BeginCombo("Select FeatureGraph", "Native.aegfeature")) ImGui::EndCombo();
        selector_rect = ImRect{ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    };
    const auto draw = [&] {
        selector_in_root = false;
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGuiWindowClass window_class; window_class.ClassId = ImHashStr("Native scroll regression");
        window_class.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&window_class);
        panel.draw(document, "Native.aegfeature", actions); ImGui::Render();
        require(selector_in_root, "Native selector not drawn in current frame");
    };
    const auto content = [&]() -> ImGuiWindow* {
        for (auto* window : context->Windows)
            if (std::string_view{window->Name}.find("FeatureGraphContent") != std::string_view::npos) return window;
        return ImGui::FindWindowByName("Flow Graph###FlowGraph");
    };
    const auto chrome = [&]() -> ImGuiWindow* {
        for (auto* window : context->Windows)
            if (std::string_view{window->Name}.find("##editor-window-chrome") != std::string_view::npos)
                return window;
        return nullptr;
    };
    draw(); draw(); require(selector_in_root, "Native selector not drawn");
    auto* before = content(); require(before && before->ScrollMax.y > 0, "Fixture did not overflow native viewport");
    const auto selector_before = selector_rect;
    auto* chrome_before = chrome(); require(chrome_before, "Native chrome child missing");
    const auto chrome_position = chrome_before->Pos;
    const auto chrome_size = chrome_before->Size;
    const auto require_fixed_chrome = [&] {
        auto* root = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        auto* header = chrome();
        require(root && root->Scroll.y == 0 && header && header->Scroll.y == 0 &&
                header->Pos.x == chrome_position.x && header->Pos.y == chrome_position.y &&
                header->Size.x == chrome_size.x && header->Size.y == chrome_size.y &&
                selector_rect.Min.x == selector_before.Min.x && selector_rect.Min.y == selector_before.Min.y &&
                selector_rect.Max.x == selector_before.Max.x && selector_rect.Max.y == selector_before.Max.y,
                "Native mouse interaction moved fixed chrome or graph selector");
    };
    // Read the final catalog row's current mouse target from the real table
    // layout. Do not assign NavId/NavActivateId or request an automatic scroll.
    const auto final_add_rect = [&] {
        auto* body = content();
        auto* table = context->Tables.GetByKey(body->GetID("FeatureGraphCatalog"));
        require(table && table->ColumnsCount == 6 && table->CurrentRow == 32,
                "Native final catalog row missing");
        const ImVec2 position{table->Columns[5].WorkMinX,
            table->RowPosY1 + table->RowCellPaddingY + table->RowTextBaseline};
        const auto label_size = ImGui::CalcTextSize("Add node");
        return ImRect{position, {position.x + label_size.x + ImGui::GetStyle().FramePadding.x * 2,
                                 position.y + label_size.y}};
    };
    io.AddMousePosEvent(800, 500); draw();
    for (unsigned step = 0; step < 64 && !content()->ClipRect.Contains(final_add_rect()); ++step) {
        const auto target = final_add_rect();
        io.AddMouseWheelEvent(0, target.Max.y > content()->ClipRect.Max.y ? -3.0f : 3.0f);
        draw(); draw();
        require_fixed_chrome();
        require(document.snapshot_owner() == original && !document.dirty() && !document.can_undo(),
                "Native content scrolling changed document/history");
    }
    auto* body = content();
    require(body && body->Scroll.y > 0, "Native FeatureGraph mouse wheel could not reveal overflowing controls");
    require(body->ClipRect.Contains(final_add_rect()), "Mouse wheel did not expose the final Add node control");
    auto* table = context->Tables.GetByKey(body->GetID("FeatureGraphCatalog"));
    const ImVec2 add_position = final_add_rect().GetCenter();
    const ImGuiID add_id = ImHashStr("Add node", 0, ImHashStr("32", 0, table->ID));
    const float scroll_before_click = body->Scroll.y;
    io.AddMousePosEvent(add_position.x, add_position.y); draw(); draw();
    require(context->HoveredId == add_id && content()->Scroll.y == scroll_before_click,
            "Mouse did not hover the final catalog Add node without automatic scrolling");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); draw();
    require(context->ActiveId == add_id && context->ActiveIdSource == ImGuiInputSource_Mouse,
            "Native Add node was not activated by the left mouse button");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); draw();
    require_fixed_chrome();
    const auto& edited = document.snapshot();
    require(document.snapshot_owner() != original && document.dirty() && document.can_undo() &&
            !document.can_redo() && document.revision() == 1 && edited.nodes.size() == 1,
            "Scrolled Add node did not create exactly one source document transaction");
    const auto& node = edited.nodes.front();
    require(node.node_id == feature::FeatureNodeId{"Editor.Feature.32.1"} &&
            node.definition_id == feature::FeatureDefinitionId{32} &&
            node.definition_version == feature::Version{1, 0, 0} &&
            node.owner_path.empty() && node.parameter_blob.empty(),
            "Scrolled Add node added the wrong catalog definition or node payload");
    require(edited.asset_uuid == original->asset_uuid && edited.schema_version == original->schema_version &&
            edited.graph_version == original->graph_version + 1 &&
            edited.draft_revision == original->draft_revision + 1 &&
            edited.layout_revision == original->layout_revision &&
            edited.contract_digest == document.catalog().contract_digest() &&
            edited.edges.empty() && edited.process_subgraphs.empty() && edited.extensions.empty(),
            "Scrolled Add node changed unrelated source data or missed version/catalog metadata");
    require(static_cast<bool>(document.undo()), "Scrolled Add node transaction could not be undone");
    require(document.snapshot_owner() == original && !document.dirty() && !document.can_undo() &&
            document.can_redo() && document.revision() == 2,
            "Undo did not restore the exact source snapshot after the scrolled mouse edit");
    draw(); require_fixed_chrome();
    // Move off the button before wheeling back, so the wheel still targets the
    // overflowing content rather than any focused or keyboard-navigation item.
    io.AddMousePosEvent(800, 500); draw();
    for (unsigned step = 0; step < 64 && content()->Scroll.y > 0; ++step) {
        io.AddMouseWheelEvent(0, 3); draw(); draw();
        require_fixed_chrome();
    }
    require(content()->Scroll.y == 0, "Mouse wheel could not return to content start");
    require_fixed_chrome();
    ImGui::DestroyContext(context);
    for (const auto& message : errors) std::cerr << message << '\n';
    require(errors.empty(), "Native FeatureGraph ImGui error");
    std::cout << "Native FeatureGraph scrolling passed\n";
}
