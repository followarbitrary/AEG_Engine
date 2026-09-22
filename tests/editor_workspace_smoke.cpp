/**
 * @file editor_workspace_smoke.cpp
 * @brief Verifies the customizable editor workspace orchestration.
 */
#include <imgui.h>
#include <imgui_internal.h>

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>

#include "../Application/Editor/private/editor_ui_interaction.hpp"
#include "../Application/Editor/private/hierarchy_ui_layout.hpp"
#include "../Application/Editor/private/pipeline_overview_layout.hpp"
#include "editor_imgui.hpp"
#include "editor_toolbar.hpp"
#include "editor_workspace.hpp"

namespace {
class TestPanel final : public aeg::editor::EditorPanel {
public:
    explicit TestPanel(uint32_t& build_count, const char* title = "Workspace Test") noexcept
        : build_count_(build_count), title_(title) {}

    void build_ui() override {
        ImGui::Begin(title_);
        ImGui::TextUnformatted("AEG");
        ImGui::End();
        ++build_count_;
    }

private:
    uint32_t& build_count_;
    const char* title_;
};
}  // namespace

int main() {
    aeg::editor::EditorImGuiHost host;
    assert(host.initialize());

    aeg::editor::EditorWorkspace workspace(host);
    // The global application chrome owns a dedicated brand row, menu row,
    // and command row.  The dock host must begin beneath all three.
    assert(aeg::editor::EditorWorkspace::chrome_height() >= 96.0f);
    assert(aeg::editor::EditorWorkspace::title_control_width() >= 120.0f);
    uint32_t build_count = 0;
    assert(workspace.register_panel(
        aeg::editor::EditorPanelDescriptor{"scene_viewport", "Workspace Test", aeg::editor::EditorDockRegion::kCenter,
                                           true, true, true},
        std::make_unique<TestPanel>(build_count)));
    assert(workspace.panel_count() == 1);
    assert(workspace.panel_presentation("scene_viewport") ==
           aeg::editor::EditorWorkspace::PanelPresentation::kDockWindow);
    assert(workspace.set_panel_presentation("scene_viewport",
                                            aeg::editor::EditorWorkspace::PanelPresentation::kNativeWindow));
    assert(workspace.panel_presentation("scene_viewport") ==
           aeg::editor::EditorWorkspace::PanelPresentation::kNativeWindow);
    assert(workspace.set_panel_presentation("scene_viewport",
                                            aeg::editor::EditorWorkspace::PanelPresentation::kDockWindow));
    const auto hierarchy_layout = aeg::application::compact_hierarchy_tree_layout();
    assert(hierarchy_layout.item_spacing.y <= 2.0f);
    assert(hierarchy_layout.frame_padding.y <= 2.0f);
    assert(hierarchy_layout.drop_zone_height <= 1.0f);
    assert(hierarchy_layout.footer_gap <= 4.0f);
    // Overview must not split a phase into one card per contributing feature.
    assert(aeg::application::pipeline_overview_key(8) == aeg::application::pipeline_overview_key(8));
    assert(aeg::application::pipeline_overview_key(8) != aeg::application::pipeline_overview_key(10));
    assert(workspace.layout() != nullptr);
    uint32_t hierarchy_build_count = 0;
    assert(workspace.register_panel(
        aeg::editor::EditorPanelDescriptor{"hierarchy", "Hierarchy", aeg::editor::EditorDockRegion::kLeft, true, true,
                                           true},
        std::make_unique<TestPanel>(hierarchy_build_count, "Hierarchy")));

    uint32_t preferences_action_count = 0;
    bool fail_preferences_action = false;
    // Preferences is launched from the Settings rail; it must not duplicate
    // that entry in the workspace View menu.
    assert(!aeg::application::preferences_action_shows_in_view_menu());
    const auto preference_window_scale = aeg::application::preference_window_work_area_scale();
    assert(preference_window_scale.width_percent == 48);
    assert(preference_window_scale.height_percent == 54);
    assert(workspace.register_view_action(aeg::editor::EditorWorkspaceActionDescriptor{
        "preferences", "Preferences",
        [&preferences_action_count, &fail_preferences_action]() {
            ++preferences_action_count;
            if (fail_preferences_action)
                throw std::runtime_error("recoverable Settings action failure");
            return aeg::foundation::Result::success();
        },
        aeg::application::preferences_action_shows_in_view_menu()}));
    assert(workspace.view_action_count() == 1);
    assert(workspace.invoke_view_action("preferences"));
    assert(preferences_action_count == 1);

    uint32_t play_action_count = 0;
    assert(workspace.register_view_action(
        aeg::editor::EditorWorkspaceActionDescriptor{"play", "Play",
                                                     [&play_action_count]() {
                                                         ++play_action_count;
                                                         return aeg::foundation::Result::success();
                                                     },
                                                     false}));
    assert(workspace.view_action_count() == 2);
    assert(workspace.view_menu_action_count() == 0);
    assert(workspace.invoke_view_action("play"));
    assert(play_action_count == 1);

    uint32_t refresh_action_count = 0;
    uint32_t rebuild_action_count = 0;
    assert(workspace.register_view_action(
        aeg::editor::EditorWorkspaceActionDescriptor{"refresh_assets", "Refresh Project Assets",
                                                     [&refresh_action_count]() {
                                                         ++refresh_action_count;
                                                         return aeg::foundation::Result::success();
                                                     },
                                                     false}));
    assert(workspace.register_view_action(
        aeg::editor::EditorWorkspaceActionDescriptor{"rebuild_pipeline", "Rebuild Editor Pipeline",
                                                     [&rebuild_action_count]() {
                                                         ++rebuild_action_count;
                                                         return aeg::foundation::Result::success();
                                                     },
                                                     false}));
    assert(workspace.view_action_count() == 4);
    assert(workspace.invoke_view_action("refresh_assets"));
    assert(workspace.invoke_view_action("rebuild_pipeline"));
    assert(refresh_action_count == 1 && rebuild_action_count == 1);

    const aeg::platform::WindowId window{0, 1};
    aeg::editor::EditorImGuiFrameDesc frame_desc{};
    frame_desc.window = window;
    frame_desc.display_extent = {1280, 720};
    assert(host.begin_frame(frame_desc));
    assert(workspace.build_ui());
    assert(host.end_frame());
    assert(build_count == 1);

    // Empty dock branches must not collapse the workspace underneath the rail.
    for (const auto extent :
         {decltype(frame_desc.display_extent){1280, 720}, decltype(frame_desc.display_extent){640, 480},
          decltype(frame_desc.display_extent){1920, 1080}}) {
        frame_desc.display_extent = extent;
        for (int frame = 0; frame < 4; ++frame) {
            assert(host.begin_frame(frame_desc));
            assert(workspace.build_ui());
            const auto* rail = ImGui::FindWindowByName("##EditorRail");
            const auto* navigation = ImGui::FindWindowByName("##WorkspaceNavigation");
            const auto* commands = ImGui::FindWindowByName("##WorkspaceCommands");
            const auto* panel = ImGui::FindWindowByName("Workspace Test");
            const auto* hierarchy = ImGui::FindWindowByName("Hierarchy");
            assert(rail && navigation && commands && panel);
            const auto* viewport = ImGui::GetMainViewport();
            assert(viewport != nullptr);
            // BeginMainMenuBar reserves the brand row in WorkPos; the
            // navigation row must start exactly there with no extra blank
            // viewport band.
            assert(navigation->Pos.y == viewport->WorkPos.y);
            assert(commands->Pos.y == navigation->Pos.y + navigation->Size.y);
            assert(rail->Pos.y == commands->Pos.y + commands->Size.y);
            assert(rail->Pos.y >= viewport->WorkPos.y + aeg::editor::EditorWorkspace::chrome_height() - 31.0f);
            assert(panel->Pos.y >= rail->Pos.y);
            assert(panel->Pos.x >= rail->Pos.x + rail->Size.x);
            assert(hierarchy && hierarchy->Pos.x >= rail->Pos.x + rail->Size.x);
            assert(rail->Size.x >= ImGui::CalcTextSize("Settings").x + 16.0f);
            const auto* status = ImGui::FindWindowByName("##EditorStatusBar");
            assert(status && panel->Pos.y + panel->Size.y <= status->Pos.y + 1.0f);
            assert(ImGui::GetCurrentContext()->ColorStack.Size == 0);
            assert(host.end_frame());
        }
    }
    ImGui::ActivateItemByID(ImGui::FindWindowByName("##EditorRail")->GetID("Settings"));
    for (int frame = 0; frame < 3; ++frame) {
        assert(host.begin_frame(frame_desc));
        assert(workspace.build_ui());
        assert(ImGui::GetCurrentContext()->ColorStack.Size == 0);
        assert(host.end_frame());
    }
    assert(preferences_action_count == 2);
    fail_preferences_action = true;
    ImGui::ActivateItemByID(ImGui::FindWindowByName("##EditorRail")->GetID("Settings"));
    bool action_failure_reported = false;
    for (int frame = 0; frame < 3; ++frame) {
        assert(host.begin_frame(frame_desc));
        const auto result = workspace.build_ui();
        if (!result) {
            action_failure_reported = true;
            assert(result.message.find("Settings action failure") != std::string::npos);
        }
        assert(ImGui::GetCurrentContext()->ColorStack.Size == 0);
        assert(ImGui::GetCurrentContext()->StyleVarStack.Size == 0);
        assert(host.end_frame());
    }
    assert(action_failure_reported);
    fail_preferences_action = false;
    uint32_t bottom_build_count = 0;
    for (const auto* title : {"Pipeline", "Assets"}) {
        assert(workspace.register_panel(
            aeg::editor::EditorPanelDescriptor{std::string(title) == "Pipeline" ? "pipeline" : "assets", title,
                                               aeg::editor::EditorDockRegion::kBottom, true, true, true},
            std::make_unique<TestPanel>(bottom_build_count, title)));
    }
    for (int frame = 0; frame < 4; ++frame) {
        assert(host.begin_frame(frame_desc));
        assert(workspace.build_ui());
        assert(host.end_frame());
    }
    for (const auto* title : {"Pipeline", "Assets", "Pipeline"}) {
        ImGui::ActivateItemByID(ImGui::FindWindowByName("##EditorRail")->GetID(title));
        for (int frame = 0; frame < 3; ++frame) {
            assert(host.begin_frame(frame_desc));
            assert(workspace.build_ui());
            assert(host.end_frame());
        }
        const auto* panel = ImGui::FindWindowByName(title);
        assert(panel && panel->DockNode && panel->DockNode->VisibleWindow == panel);
    }
    const auto visible_build_count = build_count;

    assert(workspace.set_panel_visible("assets", false));
    ImGui::ActivateItemByID(ImGui::FindWindowByName("##EditorRail")->GetID("Assets"));
    for (int frame = 0; frame < 3; ++frame) {
        assert(host.begin_frame(frame_desc));
        assert(workspace.build_ui());
        assert(host.end_frame());
    }
    const auto* reopened_assets = ImGui::FindWindowByName("Assets");
    assert(reopened_assets && reopened_assets->DockNode->VisibleWindow == reopened_assets);

    assert(host.begin_frame(frame_desc));
    ImGui::Begin("Asset identity regression");
    for (const auto* title : {"Scene", "Perspective", "Move", "Play", "Pause", "Stop", "Build"}) {
        aeg::editor::toolbar_button(title, "Tooltip");
        assert(ImGui::GetItemRectSize().y >= ImGui::GetFontSize() + 16.0f);
        assert(ImGui::GetItemRectSize().y == aeg::editor::toolbar_height());
    }
    ImGui::PushFont(ImGui::GetFont(), 28.0f);
    for (const auto icon :
         {aeg::editor::ToolbarIcon::Play, aeg::editor::ToolbarIcon::Pause, aeg::editor::ToolbarIcon::Stop}) {
        ImGui::PushID(static_cast<int>(icon));
        aeg::editor::toolbar_button("Large text", "Tooltip", icon);
        assert(ImGui::GetItemRectSize().y == 44.0f);
        assert(ImGui::GetItemRectSize().x >= ImGui::CalcTextSize("Large text").x + 46.0f);
        ImGui::PopID();
    }
    ImGui::PopFont();
    assert(ImGui::GetCurrentContext()->ColorStack.Size == 0);
    ImGuiID mesh_popup = 0, material_popup = 0, large_id_popup = 0;
    {
        aeg::application::AssetRowId row(1, 2);
        mesh_popup = ImGui::GetID("AssetContext");
    }
    {
        aeg::application::AssetRowId row(2, 2);
        material_popup = ImGui::GetID("AssetContext");
    }
    {
        aeg::application::AssetRowId row(1, 0x100000002ULL);
        large_id_popup = ImGui::GetID("AssetContext");
    }
    assert(mesh_popup != material_popup && mesh_popup != large_id_popup);
    ImGui::End();
    assert(host.end_frame());

    aeg::application::GraphPanGesture pan;
    std::vector<std::pair<int, int>> draft_edges{{0, 1}, {1, 2}};
    assert(aeg::application::remove_graph_edge(draft_edges, 0, 1));
    assert((draft_edges.size() == 1 && draft_edges.front() == std::pair{1, 2}));
    assert(!aeg::application::remove_graph_edge(draft_edges, 0, 1));
    aeg::application::PipelineOrderingDraftQueue pipeline_drafts;
    pipeline_drafts.stage_add("Game / Renderer", 3, 5);
    assert(pipeline_drafts.dirty() && pipeline_drafts.changes().size() == 1 && pipeline_drafts.changes().front().add);
    pipeline_drafts.stage_remove("Game / Renderer", 3, 5);
    assert(!pipeline_drafts.dirty());
    pipeline_drafts.stage_remove("Game / Renderer", 3, 5);
    assert(pipeline_drafts.dirty() && !pipeline_drafts.changes().front().add);
    aeg::application::PipelineOrderingSaveState persisted_orderings;
    persisted_orderings.stage({"Game / Renderer", {11, 101}, {12, 102}, true});
    persisted_orderings.stage({"Game / Renderer", {11, 101}, {12, 102}, false});
    assert(persisted_orderings.changes().size() == 1 && !persisted_orderings.changes().front().add);
    persisted_orderings.stage({"Game / Renderer", {11, 101}, {12, 102}, true});
    assert(persisted_orderings.changes().size() == 1 && persisted_orderings.changes().front().add);
    aeg::application::PipelineLayoutSaveState persisted_layout;
    persisted_layout.stage({"Game / Renderer", {11, 101}, 240.0f, 96.0f});
    persisted_layout.stage({"Game / Renderer", {11, 101}, 320.0f, 144.0f});
    const auto restored_layout = persisted_layout.find("Game / Renderer", {11, 101});
    assert(restored_layout && restored_layout->x == 320.0f && restored_layout->y == 144.0f);
    assert(!persisted_layout.find("Game / Renderer", {12, 102}));
    aeg::application::PipelineBlendSaveState persisted_blends;
    persisted_blends.stage({"Game / Renderer", {11, 101}, 1});
    persisted_blends.stage({"Game / Renderer", {11, 101}, 2});
    assert(persisted_blends.blends().size() == 1 && persisted_blends.blends().front().blend_mode == 2);
    assert(aeg::application::graph_link_hit({0, 0}, {100, 0}, {50, 0}, 1.0f));
    assert(aeg::application::graph_link_hit({0, 0}, {100, 0}, {50, 9}, 1.0f));
    assert(!aeg::application::graph_link_hit({0, 0}, {100, 0}, {50, 20}, 1.0f));
    // A read-only RenderFeature with an existing state input must be able to
    // forward that state to Present.  Its input count is still zero until the
    // missing outgoing link is created, so reachability cannot gate its port.
    assert(aeg::application::can_start_render_state_link(false, true));
    assert(aeg::application::can_start_render_state_link(true, false));
    assert(!aeg::application::can_start_render_state_link(false, false));
    const auto card_layout = aeg::application::PipelineNodeCardLayout::for_font_size(17.0f);
    assert(card_layout.title_y < card_layout.subtitle_y);
    assert(card_layout.subtitle_y < card_layout.detail_y);
    assert(card_layout.detail_y < card_layout.divider_y);
    assert(card_layout.divider_y < card_layout.footer_y);
    assert(card_layout.footer_y + 17.0f <= card_layout.height);
    assert(aeg::application::PipelineNodeCardLayout::text_size(17.0f, 0.65f) >= 13.0f);
    assert(aeg::application::pipeline_node_copy_text("Custom Outline Feature", "Feature", "Authored Pipeline node") ==
           "Custom Outline Feature\nFeature\nAuthored Pipeline node");
    const auto present_consumer = aeg::application::required_pipeline_present_consumer();
    assert(present_consumer.node_id == "__aeg_present_consumer");
    assert(present_consumer.title == "Present");
    assert(present_consumer.subtitle == "Display consumer");
    assert(present_consumer.detail == "No state link connected");
    assert(!aeg::application::builtin_pipeline_debug_enabled());
    const auto state_overlay = aeg::application::pipeline_graph_state_overlay(
        5, std::vector<std::pair<int, int>>{{0, 1}, {1, 3}, {2, 3}, {3, 4}}, std::vector<int>{4});
    assert(state_overlay.size() == 5);
    assert(state_overlay[0].reaches_output && state_overlay[0].input_count == 0 && state_overlay[0].layer_count == 1);
    assert(state_overlay[2].reaches_output && state_overlay[2].input_count == 0 && state_overlay[2].layer_count == 1);
    assert(state_overlay[3].reaches_output && state_overlay[3].input_count == 2 && state_overlay[3].layer_count == 2);
    assert(state_overlay[4].reaches_output && state_overlay[4].input_count == 1 && state_overlay[4].layer_count == 2);
    assert(aeg::application::asset_edit_block_reason(false, false, true) != nullptr);
    assert(aeg::application::asset_edit_block_reason(false, true, true) != nullptr);
    assert(aeg::application::asset_edit_block_reason(true, false, true) != nullptr);
    assert(aeg::application::asset_edit_block_reason(true, true, false) != nullptr);
    assert(aeg::application::asset_edit_block_reason(true, true, true) == nullptr);
    auto delta = pan.update(true, true, false, true, true, false, false, {0, 0});
    assert(delta.x == 0 && delta.y == 0);
    delta = pan.update(true, true, false, false, true, true, false, {12, -5});
    assert(delta.x == 12 && delta.y == -5);
    delta = pan.update(true, true, false, false, false, false, false, {9, 9});
    assert(delta.x == 0 && delta.y == 0);
    delta = pan.update(true, true, false, false, true, true, false, {9, 9});
    assert(delta.x == 0 && delta.y == 0);  // No press originating on this canvas.
    pan.update(true, false, false, true, true, false, false, {});
    delta = pan.update(true, true, false, false, true, true, false, {9, 9});
    assert(delta.x == 0 && delta.y == 0);  // Node drag must not pan.
    pan.update(true, true, true, true, true, false, false, {});
    delta = pan.update(true, true, true, false, true, true, false, {9, 9});
    assert(delta.x == 0 && delta.y == 0);  // Connecting must not pan.
    pan.update(true, true, false, true, true, false, false, {});
    delta = pan.update(false, true, false, false, true, true, true, {9, 9});
    assert(delta.x == 0 && delta.y == 0);  // Escape / focus loss cancels.

    assert(workspace.set_panel_visible("scene_viewport", false));
    assert(host.begin_frame(frame_desc));
    assert(workspace.build_ui());
    assert(host.end_frame());
    assert(build_count == visible_build_count + 3);

    std::string serialized;
    assert(workspace.serialize_layout(serialized));
    assert(workspace.deserialize_layout(serialized));
    assert(host.shutdown());
    return 0;
}
