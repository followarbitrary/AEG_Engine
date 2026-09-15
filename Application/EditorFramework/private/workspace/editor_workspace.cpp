/**
 * @file editor_workspace.cpp
 * @brief Implements the application-owned editor workspace orchestration.
 */
#include "editor_workspace.hpp"
#include "editor_toolbar.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace aeg::editor {
namespace {
float editor_rail_width() {
    return std::max(64.0f, ImGui::CalcTextSize("Settings").x + 16.0f);
}
constexpr float editor_status_height = 28.0f;
bool editor_rail_button(const char* label, bool active) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size{ImGui::GetContentRegionAvail().x, 64.0f};
    const bool pressed = ImGui::InvisibleButton(label, size);
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = active ? IM_COL32(115,230,195,255) : IM_COL32(144,165,176,255);
    if (active || ImGui::IsItemHovered()) draw->AddRectFilled(pos, ImVec2{pos.x + size.x, pos.y + size.y},
        active ? IM_COL32(18,51,47,255) : IM_COL32(16,35,44,255), 4.0f);
    if (active) draw->AddLine(pos, ImVec2{pos.x, pos.y + size.y}, color, 2.0f);
    const ImVec2 center{pos.x + size.x * 0.5f, pos.y + 22.0f};
    if (std::string_view(label) == "Pipeline") {
        for (int i=0; i<3; ++i) draw->AddRect(ImVec2{center.x - 8.0f + i*6, center.y + 6.0f - (i+1)*5},
            ImVec2{center.x - 4.0f + i*6, center.y + 7.0f}, color, 0, 0, 1.3f);
    } else if (std::string_view(label) == "Settings") {
        draw->AddCircle(center, 8.0f, color, 8, 1.5f);
        draw->AddCircle(center, 3.0f, color, 12, 1.5f);
    } else if (std::string_view(label) == "Assets") {
        draw->AddRect(ImVec2{center.x-9,center.y-5}, ImVec2{center.x+9,center.y+7}, color, 2, 0, 1.5f);
        draw->AddLine(ImVec2{center.x-8,center.y-8}, ImVec2{center.x-1,center.y-8}, color, 1.5f);
    } else {
        draw->AddQuad(ImVec2{center.x,center.y-9}, ImVec2{center.x+8,center.y-4},
            ImVec2{center.x,center.y+1}, ImVec2{center.x-8,center.y-4}, color, 1.5f);
        draw->AddLine(ImVec2{center.x-8,center.y-4}, ImVec2{center.x-8,center.y+5}, color,1.5f);
        draw->AddLine(ImVec2{center.x+8,center.y-4}, ImVec2{center.x+8,center.y+5}, color,1.5f);
        draw->AddLine(ImVec2{center.x-8,center.y+5}, ImVec2{center.x,center.y+10}, color,1.5f);
        draw->AddLine(ImVec2{center.x+8,center.y+5}, ImVec2{center.x,center.y+10}, color,1.5f);
    }
    draw->AddText(ImVec2{center.x-ImGui::CalcTextSize(label).x*0.5f,pos.y+43}, color,label);
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

foundation::Result EditorWorkspace::register_panel(EditorPanelDescriptor descriptor,
                                                   std::unique_ptr<EditorPanel> panel) {
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

        panels_.push_back(PanelEntry{std::move(descriptor), registered_panel});
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
    const PanelEntry* entry = find_panel(panel_key);
    if (entry == nullptr || layout_ == nullptr) {
        return foundation::not_found("Editor workspace panel key is not registered");
    }
    if (const foundation::Result result = layout_->set_panel_visible(panel_key, visible); !result) {
        return result;
    }
    if (const foundation::Result result = imgui_host_->set_panel_visible(entry->panel_id, visible); !result) {
        static_cast<void>(layout_->set_panel_visible(panel_key, !visible));
        return result;
    }
    return foundation::Result::success();
}

foundation::Result EditorWorkspace::register_view_action(EditorWorkspaceActionDescriptor descriptor) {
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
    try {
        return action->callback();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor workspace action callback allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "Editor workspace action callback threw an unknown exception");
    }
}

foundation::Result EditorWorkspace::build_ui() {
    if (imgui_host_ == nullptr || !imgui_host_->is_initialized() || !imgui_host_->is_frame_active()) {
        return foundation::invalid_state("Editor workspace UI requires an active ImGui frame");
    }
    if (layout_ == nullptr) {
        return foundation::invalid_state("Editor workspace has no registered panels");
    }
    if (ImGui::BeginMainMenuBar()) {
        const float controls_width = ImGui::CalcTextSize("PlayPauseStopBuildView").x + 230.0f;
        const float search_width = std::clamp(ImGui::GetWindowWidth() - ImGui::GetCursorPosX() - controls_width, 0.0f, 300.0f);
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - controls_width - search_width));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0f,8.0f});
        ImGui::SetNextItemWidth(std::max(80.0f, search_width));
        static char global_search[128]{};
        if (search_width >= 80.0f) {
        ImGui::InputTextWithHint("##GlobalSearch", "Search assets, entities, settings...", global_search,
                                 sizeof(global_search));
        ImGui::SameLine();
        }
        if (toolbar_button("Play", "Start or resume the game preview.", ToolbarIcon::Play, true)) {
            if (find_view_action("play")) static_cast<void>(invoke_view_action("play"));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!playing_);
        if (toolbar_button("Pause", "Pause the running game preview.", ToolbarIcon::Pause)) {
            if (find_view_action("pause")) static_cast<void>(invoke_view_action("pause"));
        }
        ImGui::SameLine();
        if (toolbar_button("Stop", "Stop preview and return to the editable scene.", ToolbarIcon::Stop)) {
            if (find_view_action("stop")) static_cast<void>(invoke_view_action("stop"));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if(toolbar_button("Build", "Refresh project assets or rebuild the editor pipeline. Game packaging is not available yet.", ToolbarIcon::None, false, true)) ImGui::OpenPopup("BuildActions");
        if(ImGui::BeginPopup("BuildActions")) {
            for (const char* key : {"refresh_assets","rebuild_pipeline"}) {
                const auto* action=find_view_action(key);
                if(action && ImGui::MenuItem(action->title.c_str())) static_cast<void>(invoke_view_action(key));
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::PopStyleVar();
        if (ImGui::BeginMenu("View")) {
            for (const PanelEntry& entry : panels_) {
                const EditorPanelLayoutState* state = layout_->find_panel(entry.descriptor.key);
                if (state == nullptr) {
                    ImGui::EndMenu();
                    ImGui::EndMainMenuBar();
                    return foundation::invalid_state("Editor workspace layout lost a registered panel");
                }
                bool visible = state->visible;
                if (!entry.descriptor.closable) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::MenuItem(entry.descriptor.title.c_str(), nullptr, &visible) && entry.descriptor.closable) {
                    if (const foundation::Result result = set_panel_visible(entry.descriptor.key, visible); !result) {
                        ImGui::EndDisabled();
                        ImGui::EndMenu();
                        ImGui::EndMainMenuBar();
                        return result;
                    }
                }
                if (!entry.descriptor.closable) {
                    ImGui::EndDisabled();
                }
            }
            for (const EditorWorkspaceActionDescriptor& action : view_actions_) {
                if (!action.show_in_view_menu) {
                    continue;
                }
                if (ImGui::MenuItem(action.title.c_str())) {
                    if (const foundation::Result result = invoke_view_action(action.key); !result) {
                        ImGui::EndMenu();
                        ImGui::EndMainMenuBar();
                        return result;
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    if (main_viewport != nullptr) {
        ImGui::SetNextWindowPos(main_viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2{editor_rail_width(), std::max(0.0f, main_viewport->WorkSize.y - editor_status_height)});
        constexpr ImGuiWindowFlags rail_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{4.0f, 8.0f});
        if (ImGui::Begin("##EditorRail", nullptr, rail_flags)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{4.0f, 8.0f});
            const auto rail_button = [this](const char* label, const char* panel_key) {
                const bool active = !preferences_open_ && active_rail_ == label;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.07f, 0.25f, 0.22f, 1.0f});
                if (editor_rail_button(label, active)) {
                    active_rail_ = label;
                    static_cast<void>(set_panel_visible(panel_key, true));
                    pending_panel_focus_ = panel_key;
                }
                if (active) ImGui::PopStyleColor();
            };
            rail_button("Scene", "scene_viewport");
            rail_button("Pipeline", "pipeline");
            rail_button("Assets", "assets");
            const bool settings_active = preferences_open_;
            if (settings_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.07f, 0.25f, 0.22f, 1.0f});
            if (editor_rail_button("Settings", settings_active)) {
                const auto action = find_view_action("preferences");
                if (action != nullptr) static_cast<void>(invoke_view_action(action->key));
            }
            if (settings_active) ImGui::PopStyleColor();
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
        constexpr ImGuiWindowFlags status_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##EditorStatusBar", nullptr, status_flags)) {
            ImGui::TextDisabled("AEG Engine / Editor");
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 240.0f));
            const float fps = ImGui::GetIO().Framerate;
            ImGui::TextDisabled("Frame %.1f ms  |  %.0f FPS", fps > 0.0f ? 1000.0f / fps : 0.0f, fps);
        }
        ImGui::End();
    }
    return foundation::Result::success();
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
    return static_cast<std::size_t>(std::count_if(view_actions_.begin(), view_actions_.end(),
                                                   [](const EditorWorkspaceActionDescriptor& action) {
                                                       return action.show_in_view_menu;
                                                   }));
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
    const auto iterator = std::find_if(view_actions_.begin(), view_actions_.end(), [action_key](const auto& action) {
        return action.key == action_key;
    });
    return iterator == view_actions_.end() ? nullptr : &*iterator;
}

const EditorWorkspaceActionDescriptor* EditorWorkspace::find_view_action(std::string_view action_key) const noexcept {
    const auto iterator = std::find_if(view_actions_.begin(), view_actions_.end(), [action_key](const auto& action) {
        return action.key == action_key;
    });
    return iterator == view_actions_.end() ? nullptr : &*iterator;
}

foundation::Result EditorWorkspace::initialize_dockspace() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f) {
        return foundation::invalid_state("Editor workspace main viewport is invalid");
    }
    // The rail is outside the dock tree. Empty dock leaves are collapsed by ImGui,
    // so reserve its space in the host rectangle instead of an empty split node.
    const ImVec2 dock_pos{viewport->WorkPos.x + editor_rail_width(), viewport->WorkPos.y};
    const ImVec2 dock_size{std::max(1.0f, viewport->WorkSize.x - editor_rail_width()),
                           std::max(1.0f, viewport->WorkSize.y - editor_status_height)};
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
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, hierarchy_width / dock_size.x, &left_node, &remaining_node);
    ImGuiID right_node = 0;
    const float inspector_width = std::clamp(dock_size.x * 0.13f, 280.0f, 340.0f);
    ImGui::DockBuilderSplitNode(remaining_node, ImGuiDir_Right, inspector_width / std::max(1.0f, dock_size.x - hierarchy_width), &right_node, &remaining_node);
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
    for (const PanelEntry& entry : panels_) {
        const EditorPanelLayoutState* state = layout_->find_panel(entry.descriptor.key);
        if (state == nullptr) {
            return foundation::invalid_state("Editor workspace layout lost a registered panel");
        }
        if (const foundation::Result result = imgui_host_->set_panel_visible(entry.panel_id, state->visible); !result) {
            return result;
        }
    }
    return foundation::Result::success();
}
}  // namespace aeg::editor
