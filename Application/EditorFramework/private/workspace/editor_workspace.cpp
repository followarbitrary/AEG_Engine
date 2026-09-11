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

namespace aeg::editor {
namespace {
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

foundation::Result EditorWorkspace::build_ui() {
    if (imgui_host_ == nullptr || !imgui_host_->is_initialized() || !imgui_host_->is_frame_active()) {
        return foundation::invalid_state("Editor workspace UI requires an active ImGui frame");
    }
    if (layout_ == nullptr) {
        return foundation::invalid_state("Editor workspace has no registered panels");
    }
    if (ImGui::BeginMainMenuBar()) {
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
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (const foundation::Result result = initialize_dockspace(); !result) {
        return result;
    }
    return imgui_host_->build_panels();
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

foundation::Result EditorWorkspace::initialize_dockspace() {
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (dockspace_id == 0) {
        return foundation::invalid_state("Editor workspace dockspace creation failed");
    }
    if (dockspace_initialized_) {
        return foundation::Result::success();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f) {
        return foundation::invalid_state("Editor workspace main viewport is invalid");
    }
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

    ImGuiID left_node = 0;
    ImGuiID remaining_node = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.22f, &left_node, &remaining_node);
    ImGuiID right_node = 0;
    ImGui::DockBuilderSplitNode(remaining_node, ImGuiDir_Right, 0.22f, &right_node, &remaining_node);
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
