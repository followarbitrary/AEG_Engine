/**
 * @file editor_workspace.cpp
 * @brief Implements the application-owned editor workspace orchestration.
 */
#include "editor_workspace.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "editor_error.hpp"
#include "editor_toolbar.hpp"

namespace aeg::editor {
namespace {
float editor_rail_width() {
    return std::max(64.0f, ImGui::CalcTextSize("Settings").x + 16.0f);
}
constexpr float editor_status_height = 28.0f;
constexpr float workspace_brand_height = 30.0f;
constexpr float workspace_navigation_height = 30.0f;
constexpr float workspace_command_height =
    EditorWorkspace::chrome_height() - workspace_brand_height - workspace_navigation_height;
bool editor_rail_button(const char* label, bool active) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size{ImGui::GetContentRegionAvail().x, 64.0f};
    const bool pressed = ImGui::InvisibleButton(label, size);
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = active ? IM_COL32(115, 230, 195, 255) : IM_COL32(144, 165, 176, 255);
    if (active || ImGui::IsItemHovered())
        draw->AddRectFilled(pos, ImVec2{pos.x + size.x, pos.y + size.y},
                            active ? IM_COL32(18, 51, 47, 255) : IM_COL32(16, 35, 44, 255), 4.0f);
    if (active)
        draw->AddLine(pos, ImVec2{pos.x, pos.y + size.y}, color, 2.0f);
    const ImVec2 center{pos.x + size.x * 0.5f, pos.y + 22.0f};
    if (std::string_view(label) == "Pipeline") {
        for (int i = 0; i < 3; ++i)
            draw->AddRect(ImVec2{center.x - 8.0f + i * 6, center.y + 6.0f - (i + 1) * 5},
                          ImVec2{center.x - 4.0f + i * 6, center.y + 7.0f}, color, 0, 0, 1.3f);
    } else if (std::string_view(label) == "Settings") {
        draw->AddCircle(center, 8.0f, color, 8, 1.5f);
        draw->AddCircle(center, 3.0f, color, 12, 1.5f);
    } else if (std::string_view(label) == "Assets") {
        draw->AddRect(ImVec2{center.x - 9, center.y - 5}, ImVec2{center.x + 9, center.y + 7}, color, 2, 0, 1.5f);
        draw->AddLine(ImVec2{center.x - 8, center.y - 8}, ImVec2{center.x - 1, center.y - 8}, color, 1.5f);
    } else {
        draw->AddQuad(ImVec2{center.x, center.y - 9}, ImVec2{center.x + 8, center.y - 4},
                      ImVec2{center.x, center.y + 1}, ImVec2{center.x - 8, center.y - 4}, color, 1.5f);
        draw->AddLine(ImVec2{center.x - 8, center.y - 4}, ImVec2{center.x - 8, center.y + 5}, color, 1.5f);
        draw->AddLine(ImVec2{center.x + 8, center.y - 4}, ImVec2{center.x + 8, center.y + 5}, color, 1.5f);
        draw->AddLine(ImVec2{center.x - 8, center.y + 5}, ImVec2{center.x, center.y + 10}, color, 1.5f);
        draw->AddLine(ImVec2{center.x + 8, center.y + 5}, ImVec2{center.x, center.y + 10}, color, 1.5f);
    }
    draw->AddText(ImVec2{center.x - ImGui::CalcTextSize(label).x * 0.5f, pos.y + 43}, color, label);
    return pressed;
}
[[nodiscard]] ImGuiID dock_node_for_region(EditorDockRegion region, ImGuiID center, ImGuiID left, ImGuiID right,
                                           ImGuiID bottom) noexcept {
    switch (region) {
        case EditorDockRegion::kCenter:
            return center;
        case EditorDockRegion::kLeft:
            return left;
        case EditorDockRegion::kRight:
            return right;
        case EditorDockRegion::kBottom:
            return bottom;
    }
    return center;
}
}  // namespace

EditorWorkspace::EditorWorkspace(EditorImGuiHost& imgui_host) noexcept : imgui_host_(&imgui_host) {}

foundation::Result EditorWorkspace::register_panel(EditorPanelDescriptor descriptor, std::unique_ptr<EditorPanel> panel,
                                                   std::function<void(bool)> sync_visibility) {
    if (building_ui_)
        return foundation::invalid_state("Cannot register workspace panels during UI traversal");
    if (imgui_host_ == nullptr || !imgui_host_->is_initialized()) {
        return foundation::invalid_state("Editor workspace requires an initialized ImGui host");
    }
    if (!descriptor.is_valid()) {
        return foundation::invalid_argument("Editor workspace panel descriptor is incomplete");
    }
    if (panel == nullptr) {
        return foundation::invalid_argument("Cannot register a null editor workspace panel");
    }
    if (find_panel(descriptor.key) != nullptr) {
        return foundation::invalid_argument("Editor workspace panel key is already registered");
    }

    const std::string panel_key = descriptor.key;
    EditorPanelId registered_panel{};
    bool host_registered = false;
    bool layout_registered = false;
    const bool created_layout = layout_ == nullptr;
    const auto rollback = [&]() noexcept {
        if (!panels_.empty() && panels_.back().panel_id == registered_panel) {
            panels_.pop_back();
        }
        if (layout_registered && layout_ != nullptr) {
            static_cast<void>(layout_->unregister_panel(panel_key));
        }
        if (created_layout && panels_.empty()) {
            layout_.reset();
        }
        if (host_registered) {
            static_cast<void>(imgui_host_->unregister_panel(registered_panel));
        }
    };
    try {
        const foundation::ResultValue<EditorPanelId> panel_result = imgui_host_->register_panel(std::move(panel));
        if (!panel_result) {
            return panel_result.result;
        }
        registered_panel = panel_result.value;
        host_registered = true;

        if (created_layout) {
            const EditorPanelDescriptor descriptor_copy = descriptor;
            const foundation::ResultValue<EditorLayoutModel> layout_result =
                EditorLayoutModel::create_default(std::span<const EditorPanelDescriptor>{&descriptor_copy, 1});
            if (!layout_result) {
                rollback();
                return layout_result.result;
            }
            layout_ = std::make_unique<EditorLayoutModel>(std::move(layout_result.value));
        } else if (const foundation::Result result = layout_->register_panel(descriptor); !result) {
            rollback();
            return result;
        }
        layout_registered = true;

        panels_.push_back(PanelEntry{std::move(descriptor), registered_panel, std::move(sync_visibility)});
        const EditorPanelLayoutState* state = layout_->find_panel(panel_key);
        if (state == nullptr) {
            rollback();
            return foundation::invalid_state("Editor workspace layout lost a registered panel");
        }
        if (const foundation::Result result = imgui_host_->set_panel_visible(registered_panel, state->visible);
            !result) {
            rollback();
            return result;
        }
        if (panels_.back().sync_visibility) {
            panels_.back().sync_visibility(state->visible);
        }
        dockspace_initialized_ = false;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        rollback();
        return foundation::out_of_memory("Editor workspace panel registration allocation failed");
    } catch (const std::exception& exception) {
        rollback();
        return foundation::unknown_exception(exception);
    }
}

foundation::Result EditorWorkspace::unregister_panel(std::string_view panel_key) {
    if (building_ui_)
        return foundation::invalid_state("Cannot unregister workspace panels during UI traversal");
    PanelEntry* entry = find_panel(panel_key);
    if (entry == nullptr || layout_ == nullptr) {
        return foundation::not_found("Editor workspace panel key is not registered");
    }
    if (const foundation::Result result = imgui_host_->unregister_panel(entry->panel_id); !result) {
        return result;
    }
    if (const foundation::Result result = layout_->unregister_panel(panel_key); !result) {
        return result;
    }
    panels_.erase(
        std::remove_if(panels_.begin(), panels_.end(),
                       [panel_key](const PanelEntry& candidate) { return candidate.descriptor.key == panel_key; }),
        panels_.end());
    dockspace_initialized_ = false;
    return foundation::Result::success();
}

foundation::Result EditorWorkspace::set_panel_visible(std::string_view panel_key, bool visible) {
    PanelEntry* entry = find_panel(panel_key);
    if (entry == nullptr || layout_ == nullptr) {
        return foundation::not_found("Editor workspace panel key is not registered");
    }
    const EditorPanelLayoutState* current_state = layout_->find_panel(panel_key);
    const bool visibility_changed = current_state == nullptr || current_state->visible != visible;
    if (const foundation::Result result = layout_->set_panel_visible(panel_key, visible); !result) {
        return result;
    }
    if (const foundation::Result result = imgui_host_->set_panel_visible(entry->panel_id, visible); !result) {
        static_cast<void>(layout_->set_panel_visible(panel_key, !visible));
        return result;
    }
    if (visibility_changed && entry->sync_visibility) {
        entry->sync_visibility(visible);
    }
    return foundation::Result::success();
}

foundation::Result EditorWorkspace::set_panel_presentation(std::string_view panel_key, PanelPresentation presentation) {
    const PanelEntry* entry = find_panel(panel_key);
    if (entry == nullptr || layout_ == nullptr) {
        return foundation::not_found("Editor workspace panel key is not registered");
    }
    if (!entry->descriptor.closable) {
        return foundation::unsupported("Editor workspace panel is fixed to the dockspace");
    }
    const bool native = presentation == PanelPresentation::kNativeWindow;
    if (const foundation::Result result = imgui_host_->set_panel_native(entry->panel_id, native); !result) {
        return result;
    }
    const auto iterator = std::find(native_panel_keys_.begin(), native_panel_keys_.end(), panel_key);
    if (native) {
        if (iterator == native_panel_keys_.end()) {
            try {
                native_panel_keys_.emplace_back(panel_key);
            } catch (const std::bad_alloc&) {
                return foundation::out_of_memory("Editor native viewport selection allocation failed");
            }
        }
    } else if (iterator != native_panel_keys_.end()) {
        native_panel_keys_.erase(iterator);
    }
    dockspace_initialized_ = false;
    return set_panel_visible(panel_key, true);
}

[[nodiscard]] bool EditorWorkspace::is_native_panel(std::string_view panel_key) const noexcept {
    if (const auto iterator = std::ranges::find(native_panel_keys_, panel_key); iterator == native_panel_keys_.end()) {
        return false;
    }
    return true;
}

EditorWorkspace::PanelPresentation EditorWorkspace::panel_presentation(std::string_view panel_key) const noexcept {
    return is_native_panel(panel_key) ? PanelPresentation::kNativeWindow : PanelPresentation::kDockWindow;
}

foundation::Result EditorWorkspace::register_view_action(EditorWorkspaceActionDescriptor descriptor) {
    if (building_ui_)
        return foundation::invalid_state("Cannot register workspace actions during UI traversal");
    if (!descriptor.is_valid()) {
        return foundation::invalid_argument("Editor workspace action descriptor is incomplete");
    }
    if (find_view_action(descriptor.key) != nullptr) {
        return foundation::invalid_argument("Editor workspace View action key is already registered");
    }
    try {
        view_actions_.push_back(std::move(descriptor));
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor workspace action registration allocation failed");
    }
}

foundation::Result EditorWorkspace::invoke_view_action(std::string_view action_key) {
    EditorWorkspaceActionDescriptor* action = find_view_action(action_key);
    if (action == nullptr) {
        return foundation::not_found("Editor workspace View action key is not registered");
    }
    ImGuiContext* context = ImGui::GetCurrentContext();
    const bool has_frame = context != nullptr && context->WithinFrameScope;
    ImGuiWindow* owner_window = has_frame ? context->CurrentWindow : nullptr;
    const bool owner_menu_bar = owner_window != nullptr && owner_window->DC.MenuBarAppending;
    ImGuiErrorRecoveryState checkpoint{};
    if (has_frame)
        ImGui::ErrorRecoveryStoreState(&checkpoint);
    const auto result = guard_editor_operation([&] {
        // A callback may register another action outside UI traversal.
        const auto callback = action->callback;
        return callback();
    });
    const auto recovery = guard_editor_operation([&] {
        ImGui::SetCurrentContext(context);
        if (has_frame) {
            // ImGui's recovery also closes menu bars belonging to the current
            // window. This bar belongs to the workspace, not the action.
            struct MenuBarScope {
                ImGuiWindow* window;
                bool appending;
                ~MenuBarScope() { window->DC.MenuBarAppending = appending; }
            } menu_bar{owner_window, owner_menu_bar};
            owner_window->DC.MenuBarAppending = false;
            ImGui::ErrorRecoveryTryToRecoverState(&checkpoint);
        }
        return foundation::Result::success();
    });
    return result ? recovery : result;
}

foundation::Result EditorWorkspace::build_ui() {
    if (imgui_host_ == nullptr || !imgui_host_->is_initialized() || !imgui_host_->is_frame_active()) {
        return foundation::invalid_state("Editor workspace UI requires an active ImGui frame");
    }
    if (layout_ == nullptr) {
        return foundation::invalid_state("Editor workspace has no registered panels");
    }
    if (building_ui_)
        return foundation::invalid_state("Cannot recursively build the editor workspace");
    struct UiTraversalScope {
        bool& active;
        explicit UiTraversalScope(bool& value) : active(value) { active = true; }
        ~UiTraversalScope() { active = false; }
    } traversal(building_ui_);
    return guard_editor_operation([&]() -> foundation::Result {
        foundation::Result action_result;
        const auto invoke_action = [&](std::string_view key) {
            const auto result = invoke_view_action(key);
            if (!result && action_result)
                action_result = result;
        };
        // The editor's top chrome is intentionally split into three rows.  Keeping
        // commands out of the native menu bar prevents the old one-line layout
        // from collapsing the editor hierarchy on wide displays.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{12.0f, 6.0f});
        if (ImGui::BeginMainMenuBar()) {
            ImGui::TextColored(ImVec4{0.18f, 0.93f, 0.86f, 1.0f}, "AEG");
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextDisabled("|");
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextUnformatted("AEG Engine  —  Editor");
            const char* status_text = playing_ ? "Playing" : "Ready";
            const float ready_width = ImGui::CalcTextSize(status_text).x + 18.0f;
            const float workspace_width = ImGui::CalcTextSize("Workspace").x + 8.0f + 112.0f + 16.0f + ready_width +
                                          EditorWorkspace::title_control_width();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - workspace_width));
            ImGui::TextDisabled("Workspace");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(112.0f);
            constexpr const char* workspace_modes[]{"Game", "Editor"};
            workspace_mode_ = std::clamp(workspace_mode_, 0, 1);
            if (ImGui::BeginCombo("##WorkspaceMode", workspace_modes[workspace_mode_])) {
                for (int mode = 0; mode < 2; ++mode) {
                    if (ImGui::Selectable(workspace_modes[mode], workspace_mode_ == mode))
                        workspace_mode_ = mode;
                    if (workspace_mode_ == mode)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(0.0f, 16.0f);
            const ImVec2 status_position = ImGui::GetCursorScreenPos();
            const float status_height = ImGui::GetFrameHeight();
            ImGui::InvisibleButton("##EditorStatus", ImVec2{ready_width, status_height});
            const ImVec4 status_color = playing_ ? ImVec4{0.38f, 0.73f, 1.0f, 1.0f} : ImVec4{0.20f, 0.91f, 0.80f, 1.0f};
            ImDrawList* status_draw = ImGui::GetWindowDrawList();
            const float status_center_y = status_position.y + status_height * 0.5f;
            status_draw->AddCircleFilled(ImVec2{status_position.x + 4.0f, status_center_y}, 3.0f,
                                         ImGui::GetColorU32(status_color));
            status_draw->AddText(ImVec2{status_position.x + 12.0f,
                                        status_position.y + (status_height - ImGui::GetTextLineHeight()) * 0.5f},
                                 ImGui::GetColorU32(status_color), status_text);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s", playing_ ? "The game runtime is active." : "The editor is ready.");
            }
            ImGui::SameLine(0.0f, 8.0f);
            const auto title_control = [&](const char* id, int kind,
                                           const std::function<foundation::Result()>& callback) {
                const ImVec2 position = ImGui::GetCursorScreenPos();
                constexpr ImVec2 size{36.0f, 24.0f};
                const bool pressed = ImGui::InvisibleButton(id, size);
                const bool hovered = ImGui::IsItemHovered();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImU32 background = hovered ? (kind == 2 ? IM_COL32(190, 52, 56, 255) : IM_COL32(22, 62, 70, 255))
                                                 : IM_COL32(3, 24, 30, 255);
                const ImU32 foreground = IM_COL32(174, 207, 211, 255);
                draw->AddRectFilled(position, ImVec2{position.x + size.x, position.y + size.y}, background);
                const ImVec2 center{position.x + size.x * 0.5f, position.y + size.y * 0.5f};
                if (kind == 0)
                    draw->AddLine(ImVec2{center.x - 5.0f, center.y}, ImVec2{center.x + 5.0f, center.y}, foreground,
                                  1.2f);
                if (kind == 1)
                    draw->AddRect(ImVec2{center.x - 4.0f, center.y - 4.0f}, ImVec2{center.x + 4.0f, center.y + 4.0f},
                                  foreground, 0.0f, 0, 1.1f);
                if (kind == 2) {
                    draw->AddLine(ImVec2{center.x - 4.0f, center.y - 4.0f}, ImVec2{center.x + 4.0f, center.y + 4.0f},
                                  foreground, 1.2f);
                    draw->AddLine(ImVec2{center.x + 4.0f, center.y - 4.0f}, ImVec2{center.x - 4.0f, center.y + 4.0f},
                                  foreground, 1.2f);
                }
                if (pressed && callback) {
                    const foundation::Result result = callback();
                    if (!result && action_result)
                        action_result = result;
                }
            };
            title_control("##WindowMinimize", 0, window_controls_.minimize);
            ImGui::SameLine(0.0f, 0.0f);
            title_control("##WindowMaximize", 1, window_controls_.toggle_maximize);
            ImGui::SameLine(0.0f, 0.0f);
            title_control("##WindowClose", 2, window_controls_.close);
            ImGui::EndMainMenuBar();
        }
        ImGui::PopStyleVar();

        const ImGuiViewport* chrome_viewport = ImGui::GetMainViewport();
        if (chrome_viewport != nullptr) {
            // BeginMainMenuBar() reserves the brand row in WorkPos. The two
            // following rows belong to the same full viewport chrome, so anchor
            // them to Pos to avoid reserving the brand row twice.
            const ImVec2 chrome_pos = chrome_viewport->Pos;
            const ImVec2 chrome_size = chrome_viewport->Size;
            const float brand_height = std::max(0.0f, chrome_viewport->WorkPos.y - chrome_viewport->Pos.y);
            ImGui::SetNextWindowPos(ImVec2{chrome_pos.x, chrome_pos.y + brand_height});
            ImGui::SetNextWindowSize(ImVec2{chrome_size.x, workspace_navigation_height});
            ImGui::SetNextWindowViewport(chrome_viewport->ID);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.018f, 0.075f, 0.095f, 1.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{12.0f, 3.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{0.0f, 0.0f});
            if (ImGui::Begin("##WorkspaceNavigation", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_MenuBar)) {
                if (ImGui::BeginMenuBar()) {
                    if (main_menu_content_) {
                        main_menu_content_();
                    } else {
                        for (const char* title : {"File", "Edit", "Assets", "Pipeline", "Help"}) {
                            if (ImGui::BeginMenu(title)) {
                                ImGui::TextDisabled("No actions registered");
                                ImGui::EndMenu();
                            }
                        }
                    }
                    if (ImGui::BeginMenu("Window")) {
                        for (const PanelEntry& entry : panels_) {
                            const EditorPanelLayoutState* state = layout_->find_panel(entry.descriptor.key);
                            if (state == nullptr)
                                continue;
                            bool visible = state->visible;
                            if (!entry.descriptor.closable)
                                ImGui::BeginDisabled();
                            if (ImGui::MenuItem(entry.descriptor.title.c_str(), nullptr, &visible) &&
                                entry.descriptor.closable) {
                                if (const foundation::Result result = set_panel_visible(entry.descriptor.key, visible);
                                    !result && action_result)
                                    action_result = result;
                            }
                            if (!entry.descriptor.closable)
                                ImGui::EndDisabled();
                            if (entry.descriptor.closable &&
                                ImGui::BeginMenu((entry.descriptor.title + " Window").c_str())) {
                                const bool docked =
                                    panel_presentation(entry.descriptor.key) == PanelPresentation::kDockWindow;
                                if (ImGui::MenuItem("Dock Window", nullptr, docked) && !docked) {
                                    const foundation::Result result =
                                        set_panel_presentation(entry.descriptor.key, PanelPresentation::kDockWindow);
                                    if (!result && action_result)
                                        action_result = result;
                                }
                                if (ImGui::MenuItem("Native Window", nullptr, !docked) && docked) {
                                    const foundation::Result result =
                                        set_panel_presentation(entry.descriptor.key, PanelPresentation::kNativeWindow);
                                    if (!result && action_result)
                                        action_result = result;
                                }
                                ImGui::EndMenu();
                            }
                        }
                        for (const EditorWorkspaceActionDescriptor& action : view_actions_) {
                            if (action.show_in_view_menu && ImGui::MenuItem(action.title.c_str()))
                                invoke_action(action.key);
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenuBar();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();

            ImGui::SetNextWindowPos(ImVec2{chrome_pos.x, chrome_pos.y + brand_height + workspace_navigation_height});
            ImGui::SetNextWindowSize(ImVec2{chrome_size.x, workspace_command_height});
            ImGui::SetNextWindowViewport(chrome_viewport->ID);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.016f, 0.09f, 0.115f, 1.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{14.0f, 0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::Begin("##WorkspaceCommands", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
                const ImVec2 command_origin = ImGui::GetWindowPos();
                ImGui::SetCursorScreenPos(
                    ImVec2{command_origin.x + 14.0f,
                           command_origin.y + std::max(0.0f, (workspace_command_height - toolbar_height()) * 0.5f)});
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{9.0f, 5.0f});
                const auto command = [&](const char* label, std::string_view action,
                                         ToolbarIcon icon = ToolbarIcon::None, bool emphasized = false) {
                    if (toolbar_button(label, label, icon, emphasized) && find_view_action(action))
                        invoke_action(action);
                };
                command("Save", "save_scene");
                ImGui::SameLine();
                ImGui::BeginDisabled(find_view_action("undo") == nullptr);
                command("Undo", "undo");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(find_view_action("redo") == nullptr);
                command("Redo", "redo");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled();
                toolbar_button("Auto-Layout", "Available in the Pipeline editor.");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled();
                toolbar_button("Validate", "Available in the Pipeline editor.");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled();
                toolbar_button("Preview", "Available in the Pipeline editor.", ToolbarIcon::Play);
                ImGui::EndDisabled();

                const ImGuiStyle& style = ImGui::GetStyle();
                constexpr float platform_combo_width = 132.0f;
                constexpr float config_combo_width = 118.0f;
                constexpr float more_button_width = 32.0f;
                const bool has_refresh_action = find_view_action("refresh_assets") != nullptr;
                const bool has_rebuild_action = find_view_action("rebuild_pipeline") != nullptr;
                const bool has_more_actions = has_refresh_action || has_rebuild_action;
                const float command_right_width = ImGui::CalcTextSize("Platform").x + style.ItemSpacing.x +
                                                  platform_combo_width + 18.0f + ImGui::CalcTextSize("Config").x +
                                                  style.ItemSpacing.x + config_combo_width +
                                                  (has_more_actions ? 14.0f + more_button_width : 0.0f);
                ImGui::SameLine();
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 20.0f,
                                              ImGui::GetWindowContentRegionMax().x - command_right_width));
                ImGui::TextDisabled("Platform");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(platform_combo_width);
                if (ImGui::BeginCombo("##WorkspacePlatform", "PC (Vulkan)")) {
                    ImGui::Selectable("PC (Vulkan)", true);
                    ImGui::Selectable("PC (DX12)", false);
                    ImGui::EndCombo();
                }
                ImGui::SameLine(0.0f, 18.0f);
                ImGui::TextDisabled("Config");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(config_combo_width);
                if (ImGui::BeginCombo("##WorkspaceConfig", "Editor")) {
                    ImGui::Selectable("Editor", true);
                    ImGui::Selectable("Deferred", false);
                    ImGui::EndCombo();
                }
                if (has_more_actions) {
                    ImGui::SameLine(0.0f, 14.0f);
                    const ImVec2 more_size{more_button_width, toolbar_height()};
                    const bool more_pressed = ImGui::Button("...", more_size);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                        ImGui::SetTooltip("Project and pipeline actions");
                    if (more_pressed)
                        ImGui::OpenPopup("WorkspaceMore");
                    if (ImGui::BeginPopup("WorkspaceMore")) {
                        if (has_refresh_action && ImGui::MenuItem("Refresh Project Assets")) {
                            invoke_action("refresh_assets");
                        }
                        if (has_rebuild_action && ImGui::MenuItem("Rebuild Editor Pipeline")) {
                            invoke_action("rebuild_pipeline");
                        }
                        ImGui::EndPopup();
                    }
                }
                ImGui::PopStyleVar();
            }
            const ImVec2 command_origin = ImGui::GetWindowPos();
            const ImVec2 command_size = ImGui::GetWindowSize();
            const float separator_y = command_origin.y + command_size.y - 1.0f;
            ImGui::GetWindowDrawList()->AddLine(ImVec2{command_origin.x, separator_y},
                                                ImVec2{command_origin.x + command_size.x, separator_y},
                                                IM_COL32(23, 76, 84, 255));
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }

        const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        if (main_viewport != nullptr) {
            const float remaining_chrome_height = workspace_navigation_height + workspace_command_height;
            const ImVec2 workspace_pos{main_viewport->WorkPos.x, main_viewport->WorkPos.y + remaining_chrome_height};
            ImGui::SetNextWindowPos(workspace_pos);
            ImGui::SetNextWindowSize(
                ImVec2{editor_rail_width(),
                       std::max(0.0f, main_viewport->WorkSize.y - remaining_chrome_height - editor_status_height)});
            ImGui::SetNextWindowViewport(main_viewport->ID);
            constexpr ImGuiWindowFlags rail_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                                    ImGuiWindowFlags_NoBringToFrontOnFocus;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{4.0f, 8.0f});
            if (ImGui::Begin("##EditorRail", nullptr, rail_flags)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{4.0f, 8.0f});
                const auto rail_button = [this](const char* label, const char* panel_key) {
                    const bool active = !preferences_open_ && active_rail_ == label;
                    if (active)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.07f, 0.25f, 0.22f, 1.0f});
                    if (editor_rail_button(label, active)) {
                        active_rail_ = label;
                        static_cast<void>(set_panel_visible(panel_key, true));
                        pending_panel_focus_ = panel_key;
                    }
                    if (active)
                        ImGui::PopStyleColor();
                };
                rail_button("Scene", "scene_viewport");
                rail_button("Pipeline", "pipeline");
                rail_button("Assets", "assets");
                const bool settings_active = preferences_open_;
                if (settings_active)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.07f, 0.25f, 0.22f, 1.0f});
                if (editor_rail_button("Settings", settings_active)) {
                    const auto action = find_view_action("preferences");
                    if (action != nullptr)
                        invoke_action(action->key);
                }
                if (settings_active)
                    ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        if (const foundation::Result result = initialize_dockspace(); !result) {
            return result;
        }
        const foundation::Result panel_result = imgui_host_->build_panels();
        if (!panel_result) {
            return panel_result;
        }
        if (!pending_panel_focus_.empty()) {
            if (const auto* entry = find_panel(pending_panel_focus_)) {
                ImGui::SetWindowFocus(entry->descriptor.title.c_str());
            }
            pending_panel_focus_.clear();
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (viewport != nullptr && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 30.0f) {
            ImGui::SetNextWindowPos(ImVec2{viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - 28.0f});
            ImGui::SetNextWindowSize(ImVec2{viewport->WorkSize.x, 28.0f});
            ImGui::SetNextWindowViewport(viewport->ID);
            constexpr ImGuiWindowFlags status_flags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
            if (ImGui::Begin("##EditorStatusBar", nullptr, status_flags)) {
                ImGui::TextDisabled("AEG Engine / Editor");
                ImGui::SameLine();
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 360.0f));
                const float fps = ImGui::GetIO().Framerate;
                ImGui::TextDisabled("Frame %.1f ms  |  %.0f FPS  |  Game Draw Calls %u",
                                    fps > 0.0f ? 1000.0f / fps : 0.0f, fps,
                                    static_cast<unsigned int>(game_draw_calls_));
            }
            ImGui::End();
        }
        return action_result;
    });
}

foundation::Result EditorWorkspace::serialize_layout(std::string& serialized) const {
    if (layout_ == nullptr) {
        return foundation::invalid_state("Editor workspace has no registered panels");
    }
    return layout_->serialize(serialized);
}

foundation::Result EditorWorkspace::deserialize_layout(std::string_view serialized) {
    if (imgui_host_ == nullptr || !imgui_host_->is_initialized()) {
        return foundation::invalid_state("Editor workspace requires an initialized ImGui host");
    }
    const foundation::ResultValue<EditorLayoutModel> layout_result = EditorLayoutModel::deserialize(serialized);
    if (!layout_result) {
        return layout_result.result;
    }
    for (const PanelEntry& entry : panels_) {
        if (layout_result.value.find_panel(entry.descriptor.key) == nullptr) {
            return foundation::invalid_argument("Editor layout is missing a registered workspace panel");
        }
    }
    auto candidate_layout = std::make_unique<EditorLayoutModel>(layout_result.value);
    std::unique_ptr<EditorLayoutModel> previous_layout = std::move(layout_);
    layout_ = std::move(candidate_layout);
    dockspace_initialized_ = false;
    if (const foundation::Result result = sync_host_visibility(); !result) {
        layout_ = std::move(previous_layout);
        dockspace_initialized_ = false;
        static_cast<void>(sync_host_visibility());
        return result;
    }
    return foundation::Result::success();
}

const EditorLayoutModel* EditorWorkspace::layout() const noexcept {
    return layout_.get();
}

std::size_t EditorWorkspace::panel_count() const noexcept {
    return panels_.size();
}

std::size_t EditorWorkspace::view_action_count() const noexcept {
    return view_actions_.size();
}

std::size_t EditorWorkspace::view_menu_action_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(view_actions_.begin(), view_actions_.end(),
                      [](const EditorWorkspaceActionDescriptor& action) { return action.show_in_view_menu; }));
}

EditorWorkspace::PanelEntry* EditorWorkspace::find_panel(std::string_view panel_key) noexcept {
    const auto iterator = std::find_if(panels_.begin(), panels_.end(), [panel_key](const PanelEntry& entry) {
        return entry.descriptor.key == panel_key;
    });
    return iterator == panels_.end() ? nullptr : &*iterator;
}

const EditorWorkspace::PanelEntry* EditorWorkspace::find_panel(std::string_view panel_key) const noexcept {
    const auto iterator = std::find_if(panels_.begin(), panels_.end(), [panel_key](const PanelEntry& entry) {
        return entry.descriptor.key == panel_key;
    });
    return iterator == panels_.end() ? nullptr : &*iterator;
}

EditorWorkspaceActionDescriptor* EditorWorkspace::find_view_action(std::string_view action_key) noexcept {
    const auto iterator = std::find_if(view_actions_.begin(), view_actions_.end(),
                                       [action_key](const auto& action) { return action.key == action_key; });
    return iterator == view_actions_.end() ? nullptr : &*iterator;
}

const EditorWorkspaceActionDescriptor* EditorWorkspace::find_view_action(std::string_view action_key) const noexcept {
    const auto iterator = std::find_if(view_actions_.begin(), view_actions_.end(),
                                       [action_key](const auto& action) { return action.key == action_key; });
    return iterator == view_actions_.end() ? nullptr : &*iterator;
}

foundation::Result EditorWorkspace::initialize_dockspace() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f) {
        return foundation::invalid_state("Editor workspace main viewport is invalid");
    }
    // The rail is outside the dock tree. Empty dock leaves are collapsed by ImGui,
    // so reserve its space in the host rectangle instead of an empty split node.
    const float remaining_chrome_height = workspace_navigation_height + workspace_command_height;
    const ImVec2 dock_pos{viewport->WorkPos.x + editor_rail_width(), viewport->WorkPos.y + remaining_chrome_height};
    const ImVec2 dock_size{std::max(1.0f, viewport->WorkSize.x - editor_rail_width()),
                           std::max(1.0f, viewport->WorkSize.y - remaining_chrome_height - editor_status_height)};
    ImGui::SetNextWindowPos(dock_pos);
    ImGui::SetNextWindowSize(dock_size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##EditorDockHost", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus);
    const ImGuiID dockspace_id = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspace_id, dock_size);
    ImGui::End();
    ImGui::PopStyleVar(3);
    if (dockspace_id == 0) {
        return foundation::invalid_state("Editor workspace dockspace creation failed");
    }
    if (dockspace_initialized_) {
        return foundation::Result::success();
    }

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(dockspace_id, dock_pos);
    ImGui::DockBuilderSetNodeSize(dockspace_id, dock_size);

    ImGuiID left_node = 0;
    ImGuiID remaining_node = 0;
    const float hierarchy_width = std::clamp(dock_size.x * 0.095f, 180.0f, 240.0f);
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, hierarchy_width / dock_size.x, &left_node,
                                &remaining_node);
    ImGuiID right_node = 0;
    const float inspector_width = std::clamp(dock_size.x * 0.13f, 280.0f, 340.0f);
    ImGui::DockBuilderSplitNode(remaining_node, ImGuiDir_Right,
                                inspector_width / std::max(1.0f, dock_size.x - hierarchy_width), &right_node,
                                &remaining_node);
    ImGuiID bottom_node = 0;
    ImGui::DockBuilderSplitNode(remaining_node, ImGuiDir_Down, 0.28f, &bottom_node, &remaining_node);

    for (const PanelEntry& entry : panels_) {
        const EditorPanelLayoutState* state = layout_->find_panel(entry.descriptor.key);
        if (state == nullptr || state->floating) {
            continue;
        }
        ImGui::DockBuilderDockWindow(
            entry.descriptor.title.c_str(),
            dock_node_for_region(state->region, remaining_node, left_node, right_node, bottom_node));
    }
    ImGui::DockBuilderFinish(dockspace_id);
    dockspace_initialized_ = true;
    return foundation::Result::success();
}

foundation::Result EditorWorkspace::sync_host_visibility() {
    for (PanelEntry& entry : panels_) {
        const EditorPanelLayoutState* state = layout_->find_panel(entry.descriptor.key);
        if (state == nullptr) {
            return foundation::invalid_state("Editor workspace layout lost a registered panel");
        }
        if (const foundation::Result result = imgui_host_->set_panel_visible(entry.panel_id, state->visible); !result) {
            return result;
        }
        if (const foundation::Result result =
                imgui_host_->set_panel_native(entry.panel_id, is_native_panel(entry.descriptor.key));
            !result) {
            return result;
        }
        if (entry.sync_visibility) {
            entry.sync_visibility(state->visible);
        }
    }
    return foundation::Result::success();
}
}  // namespace aeg::editor
