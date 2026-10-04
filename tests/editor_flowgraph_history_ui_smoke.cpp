#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "editor_composition_test_support.hpp"
#include "editor_flowgraph_panel.hpp"

int main() {
    using namespace aeg;
    tests::EditorCompositionFixture fixture;
    application::EditorCompositionDocument document{fixture.document, fixture.catalog};
    application::EditorFlowGraphPanel panel;
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600.0f, 1000.0f};
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableDebugLog = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::vector<std::string> errors;
    context->ErrorCallbackUserData = &errors;
    context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
        static_cast<std::vector<std::string>*>(data)->emplace_back(message);
    };
    bool balanced = true;
    bool native_window = false;
    bool parent_hover = false;
    bool show_overlay = false;
    ImVec2 window_position{};
    ImVec2 overlay_position{};
    const auto frame = [&](const char* activate = nullptr) {
        ImGui::NewFrame();
        if (parent_hover)
            context->HoveredWindow = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        if (activate) {
            auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
            const auto id = window->GetID(activate);
            context->NavWindow = window;
            context->NavId = id;
            context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = id;
        }
        ImGui::SetNextWindowPos(window_position);
        ImGui::SetNextWindowSize(io.DisplaySize);
        application::EditorFlowGraphPanel::Actions actions{};
        actions.native_window = native_window;
        panel.draw(document, {}, {}, actions);
        if (show_overlay) {
            ImGui::SetNextWindowPos(overlay_position);
            ImGui::SetNextWindowSize({180.0f, 140.0f});
            ImGui::Begin("Canvas pan blocker", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
            ImGui::TextUnformatted("Node configuration overlay");
            ImGui::End();
        }
        balanced = balanced && context->DisabledStackSize == 0;
        ImGui::Render();
    };
    frame();
    frame();
    const bool enabled = document.snapshot()->feature_nodes.front().enabled;
    if (!document.execute("Toggle feature", [enabled](auto& next) {
            next.feature_nodes.front().enabled = !enabled;
            return foundation::Result::success();
        }))
        return 1;

    // Activating the last available history operation changes its enabled
    // state during draw. ImGui scopes must still balance in that same frame.
    frame("Undo##flowgraph");
    const bool undone = !document.can_undo() && document.can_redo() &&
                        document.snapshot()->feature_nodes.front().enabled == enabled;
    const auto after_undo = document.snapshot();
    frame("Undo##flowgraph");
    const bool disabled_undo = document.snapshot() == after_undo;
    frame("Redo##flowgraph");
    const bool redone = document.can_undo() && !document.can_redo() &&
                        document.snapshot()->feature_nodes.front().enabled == !enabled;
    const auto after_redo = document.snapshot();
    frame("Redo##flowgraph");
    const bool disabled_redo = document.snapshot() == after_redo;

    // Exercise the actual panel input/drawing path with a graph smaller than
    // its viewport. Using ImGui scroll as a camera fails here: ScrollMax is
    // zero, and dragging right/down is also clamped at the scroll origin.
    ImGuiWindow* canvas = nullptr;
    for (auto* window : context->Windows) {
        if (std::strstr(window->Name, "##flowgraph-canvas") != nullptr) {
            canvas = window;
            break;
        }
    }
    if (canvas == nullptr)
        return 4;
    const auto rendered_pipeline_min = [&] {
        ImVec2 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        // Locate the only Pipeline header in the canvas draw list, then assert
        // its movement, rather than checking a private camera field/helper.
        for (const auto& vertex : canvas->DrawList->VtxBuffer) {
            if (vertex.col == IM_COL32(67, 157, 218, 255)) {
                minimum.x = std::min(minimum.x, vertex.pos.x);
                minimum.y = std::min(minimum.y, vertex.pos.y);
            }
        }
        return minimum;
    };
    const ImVec2 start{canvas->InnerRect.Min.x + canvas->InnerRect.GetWidth() * 0.6f,
                       canvas->InnerRect.Min.y + canvas->InnerRect.GetHeight() * 0.6f};
    io.AddMousePosEvent(start.x, start.y);
    frame();
    frame();
    const auto before_pan = rendered_pipeline_min();
    const auto before_pan_document = document.snapshot();
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
    frame();
    io.AddMousePosEvent(start.x + 48.0f, start.y + 32.0f);
    frame();
    frame();
    const auto after_forward_pan = rendered_pipeline_min();
    io.AddMousePosEvent(start.x + 12.0f, start.y + 16.0f);
    frame();
    frame();
    const auto after_reverse_pan = rendered_pipeline_min();
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
    frame();
    const auto near = [](float actual, float expected) { return std::abs(actual - expected) < 0.1f; };
    const bool canvas_panned = near(after_forward_pan.x - before_pan.x, 48.0f) &&
                               near(after_forward_pan.y - before_pan.y, 32.0f) &&
                               near(after_reverse_pan.x - before_pan.x, 12.0f) &&
                               near(after_reverse_pan.y - before_pan.y, 16.0f) &&
                               document.snapshot() == before_pan_document;
    if (!canvas_panned)
        std::cerr << "Canvas middle-drag did not move the rendered node in both directions: "
                  << after_forward_pan.x - before_pan.x << ", "
                  << after_forward_pan.y - before_pan.y << "; reverse "
                  << after_reverse_pan.x - before_pan.x << ", "
                  << after_reverse_pan.y - before_pan.y << "; ScrollMax "
                  << canvas->ScrollMax.x << ", " << canvas->ScrollMax.y << '\n';

    // Empty-canvas left-drag is the discoverable fallback for users who do
    // not reach for the middle button. The same gesture on a node or port
    // must continue to belong to node/connection editing.
    const ImVec2 empty_press{canvas->InnerRect.Min.x + canvas->InnerRect.GetWidth() * 0.75f,
                             canvas->InnerRect.Min.y + canvas->InnerRect.GetHeight() * 0.75f};
    io.AddMousePosEvent(empty_press.x, empty_press.y);
    frame();
    frame();
    const auto before_empty_left_pan = rendered_pipeline_min();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMousePosEvent(empty_press.x + 30.0f, empty_press.y + 20.0f);
    frame();
    frame();
    const auto after_empty_left_pan = rendered_pipeline_min();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    const bool empty_left_pan = near(after_empty_left_pan.x - before_empty_left_pan.x, 30.0f) &&
                                near(after_empty_left_pan.y - before_empty_left_pan.y, 20.0f) &&
                                document.snapshot() == before_pan_document;
    if (!empty_left_pan)
        std::cerr << "Canvas empty left-drag did not move the rendered node: "
                  << after_empty_left_pan.x - before_empty_left_pan.x << ", "
                  << after_empty_left_pan.y - before_empty_left_pan.y << '\n';

    // Native window chrome and an offset screen origin use the same pan
    // controller. A parent hover must not discard a click inside the canvas.
    native_window = true;
    window_position = {200.0f, 100.0f};
    frame();
    frame();
    const ImVec2 native_press{canvas->InnerRect.Min.x + canvas->InnerRect.GetWidth() * 0.65f,
                              canvas->InnerRect.Min.y + canvas->InnerRect.GetHeight() * 0.65f};
    io.AddMousePosEvent(native_press.x, native_press.y);
    frame();
    frame();
    const auto before_native_pan = rendered_pipeline_min();
    parent_hover = true;
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMousePosEvent(native_press.x + 40.0f, native_press.y + 24.0f);
    frame();
    frame();
    const auto after_native_pan = rendered_pipeline_min();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    parent_hover = false;
    const bool native_panned = near(after_native_pan.x - before_native_pan.x, 40.0f) &&
                               near(after_native_pan.y - before_native_pan.y, 24.0f) &&
                               document.snapshot() == before_pan_document;

    // A separate window over the canvas must own the gesture; a rectangle
    // fallback alone would pan the underlying graph through that window.
    show_overlay = true;
    overlay_position = {native_press.x - 40.0f, native_press.y - 40.0f};
    frame();
    frame();
    io.AddMousePosEvent(native_press.x, native_press.y);
    frame();
    frame();
    const auto before_blocked_pan = rendered_pipeline_min();
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
    frame();
    io.AddMousePosEvent(native_press.x + 20.0f, native_press.y + 16.0f);
    frame();
    frame();
    const auto after_blocked_pan = rendered_pipeline_min();
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
    frame();
    show_overlay = false;
    const bool overlay_blocked_pan = near(after_blocked_pan.x, before_blocked_pan.x) &&
                                     near(after_blocked_pan.y, before_blocked_pan.y);
    if (!native_panned || !overlay_blocked_pan)
        std::cerr << "Native canvas pan or overlay gesture isolation failed\n";

    // Reproduce the DemoProject migration case: the old mixed canvas saved a
    // hidden Resource layout and placed two visible Passes beyond the first
    // strict Pass viewport.  The panel must migrate all Pass cards back into
    // view instead of making the graph look like it contains only two nodes.
    auto layout_document_source = fixture.document;
    layout_document_source.pass_nodes.push_back(layout_document_source.pass_nodes.front());
    layout_document_source.pass_nodes.back().node_id = "pass2";
    layout_document_source.pass_nodes.push_back(layout_document_source.pass_nodes.front());
    layout_document_source.pass_nodes.back().node_id = "pass3";
    layout_document_source.pass_nodes.push_back(layout_document_source.pass_nodes.front());
    layout_document_source.pass_nodes.back().node_id = "pass4";
    layout_document_source.resources.push_back({.node_id = "feature/Relay",
                                                .resource_id = pipeline::RenderResourceId("Game.Relay"),
                                                .type = pipeline::ArtifactTypeId("Pipeline.DefaultTexture")});
    layout_document_source.edges = {
        {.edge_id = "pass-relay-producer",
         .kind = pipeline::DocumentEdgeKind::kRenderResource,
         .source = {"pass", 11},
         .destination = {"feature/Relay", 77}},
        {.edge_id = "pass-relay-consumer",
         .kind = pipeline::DocumentEdgeKind::kRenderResource,
         .source = {"feature/Relay", 77},
         .destination = {"pass2", 12}},
    };
    layout_document_source.layouts = {
        {.node_id = "pass", .x = 1600.0, .y = 900.0},
        {.node_id = "pass2", .x = 1400.0, .y = 32.0},
        {.node_id = "pass3", .x = 32.0, .y = 900.0},
        {.node_id = "pass4", .x = 32.0, .y = 32.0},
        {.node_id = "feature/Relay", .x = 320.0, .y = 32.0},
    };
    application::EditorCompositionDocument layout_document{layout_document_source, fixture.catalog};
    application::EditorFlowGraphPanel layout_panel;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    layout_panel.draw(layout_document,
                      {application::EditorGraphKind::kPass, "pipeline", "feature", "render"});
    ImGui::Render();
    const auto migrated_layouts = layout_document.snapshot()->layouts;
    const auto pass_layouts_in_view = std::all_of(
        layout_document_source.pass_nodes.begin(), layout_document_source.pass_nodes.end(),
        [&](const auto& pass) {
            const auto found = std::find_if(migrated_layouts.begin(), migrated_layouts.end(),
                                             [&](const auto& layout) { return layout.node_id == pass.node_id; });
            return found != migrated_layouts.end() && found->x >= 0.0 && found->y >= 0.0 && found->x < 900.0 &&
                   found->y < 700.0;
        });

    ImGui::DestroyContext(context);
    for (const auto& error : errors)
        std::cerr << error << '\n';
    if (!undone || !redone || !disabled_undo || !disabled_redo) {
        std::cerr << "FlowGraph history buttons did not apply or disable correctly\n";
        return 2;
    }
    if (!errors.empty() || !balanced || !pass_layouts_in_view)
        return 3;
    if (!canvas_panned)
        return 4;
    if (!empty_left_pan)
        return 5;
    if (!native_panned || !overlay_blocked_pan)
        return 6;
    std::cout << "FlowGraph history UI smoke passed\n";
    return 0;
}
