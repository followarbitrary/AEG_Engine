/**
 * @file editor_application.cpp
 * @brief Minimal editor application assembled from the public engine contracts.
 */
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Components/component_ids.hpp"
#include "application_platform.hpp"
#include "editor_error.hpp"
#include "editor_imgui.hpp"
#include "editor_preferences.hpp"
#include "editor_scene_runtime.hpp"
#include "editor_toolbar.hpp"
#include "editor_ui_interaction.hpp"
#include "editor_workspace.hpp"
#include "engine_frame_orchestrator.hpp"
#include "font/editor_system_font_catalog.hpp"
#include "frame_context.hpp"
#include "hierarchy_ui_layout.hpp"
#include "imgui_renderer.hpp"
#include "imgui_texture_registry.hpp"
#include "input_system.hpp"
#include "logging.hpp"
#include "pipeline.hpp"
#include "pipeline_overview_layout.hpp"
#include "project_dialog_ui.hpp"
#include "render_graph.hpp"
#include "render_state_flow.hpp"
#include "renderer.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "texture_uploader.hpp"

namespace {
using aeg::application::ComponentUpdateView;
using aeg::application::EditorAssetStatus;
using aeg::application::EditorResourceDescriptor;
using aeg::application::EditorResourceKind;
using aeg::application::EditorSceneRuntime;
using aeg::application::EditorSystemFontCatalog;
using aeg::application::HierarchyTransformPolicy;
using aeg::application::SelectionOperation;
using aeg::application::ViewportGizmoAxis;
using aeg::application::ViewportGizmoMode;
using aeg::application::ViewportGizmoProjection;
using aeg::application::ViewportPoint;
using aeg::editor::EditorDockRegion;
using aeg::editor::EditorErrorNotice;
using aeg::editor::EditorErrorSeverity;
using aeg::editor::EditorErrorState;
using aeg::editor::EditorFontCatalog;
using aeg::editor::EditorImGuiFrameDesc;
using aeg::editor::EditorImGuiHost;
using aeg::editor::EditorPanelDescriptor;
using aeg::editor::EditorPreferenceModel;
using aeg::editor::EditorPreferencePanel;
using aeg::editor::EditorPreferenceState;
using aeg::editor::EditorWorkspace;
using aeg::editor::IEditorPreferenceActions;
using aeg::editor::ImGuiDrawPacket;
using aeg::editor::ImGuiFontAtlasData;
using aeg::editor::ImGuiRenderer;
using aeg::editor::ImGuiRenderTargetDesc;
using aeg::editor::ImGuiTextureBinding;
using aeg::editor::ImGuiTextureHandle;
using aeg::editor::ImGuiTextureRegistry;
namespace scene = aeg::scene;
using aeg::ecs::ComponentTypeId;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::input::InputSystem;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::PipelineScheduler;
using aeg::pipeline::PipelineScope;
using aeg::pipeline::PipelineScopeHost;
using aeg::platform::DisplayWorkArea;
using aeg::platform::IWindow;
using aeg::platform::IWindowSystem;
using aeg::platform::WindowEvent;
using aeg::platform::WindowEventType;
using aeg::render_graph::FrameAllocation;
using aeg::render_graph::FrameContext;
using aeg::render_graph::RenderGraph;
using aeg::resource::GpuTexture;
using aeg::resource::TextureData;
using aeg::resource::TextureSubresourceData;
using aeg::resource::TextureUploader;
using aeg::rhi::ClearColorValue;
using aeg::rhi::Device;
using aeg::rhi::DeviceCreateDesc;
using aeg::rhi::Format;
using aeg::rhi::MemoryClass;
using aeg::rhi::PipelineStage;
using aeg::rhi::PresentMode;
using aeg::rhi::QueueClass;
using aeg::rhi::ResourceState;
using aeg::rhi::ResourceUsage;
using aeg::rhi::SamplerDesc;
using aeg::rhi::SurfaceDesc;
using aeg::rhi::SurfaceHandle;
using aeg::rhi::SwapchainDesc;
using aeg::rhi::SwapchainHandle;
using aeg::rhi::SwapchainInfo;
using aeg::rhi::TextureCapability;
using aeg::rhi::TextureDesc;

bool report_result(const Result& result, std::string_view operation) {
    if (result) {
        return true;
    }
    AEG_LOG_ERROR(aeg::logging::Category::kApplication, "Editor operation {} failed [{}]: {}", operation,
                  aeg::foundation::result_code_name(result.code), result.message);
    try {
        static_cast<void>(aeg::application::show_error_dialog("Aether Engine Error",
                                                              "The editor could not complete an initialization step."));
    } catch (...) {
        static_cast<void>(aeg::application::show_error_dialog("Aether Engine Error",
                                                              "The editor could not complete an initialization step."));
    }
    return false;
}

void apply_editor_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2{12.0f, 12.0f};
    style.FramePadding = ImVec2{7.0f, 4.0f};
    style.ItemSpacing = ImVec2{7.0f, 5.0f};
    style.ItemInnerSpacing = ImVec2{6.0f, 5.0f};
    style.ScrollbarSize = 13.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4{0.91f, 0.95f, 0.96f, 1.0f};
    colors[ImGuiCol_MenuBarBg] = ImVec4{0.031f, 0.067f, 0.098f, 1.0f};
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_MenuBarBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_MenuBarBg];
    colors[ImGuiCol_Tab] = ImVec4{0.043f, 0.09f, 0.13f, 1.0f};
    colors[ImGuiCol_TabSelected] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TabDimmed] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TabDimmedSelected] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TabHovered] = ImVec4{0.08f, 0.19f, 0.21f, 1.0f};
    colors[ImGuiCol_TabSelectedOverline] = ImVec4{0.45f, 0.90f, 0.76f, 1.0f};
    colors[ImGuiCol_DockingEmptyBg] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TextDisabled] = ImVec4{0.43f, 0.52f, 0.57f, 1.0f};
    colors[ImGuiCol_WindowBg] = ImVec4{0.027f, 0.063f, 0.090f, 1.0f};
    colors[ImGuiCol_ChildBg] = ImVec4{0.035f, 0.086f, 0.118f, 1.0f};
    colors[ImGuiCol_PopupBg] = ImVec4{0.055f, 0.11f, 0.14f, 0.98f};
    colors[ImGuiCol_Border] = ImVec4{0.12f, 0.22f, 0.27f, 1.0f};
    colors[ImGuiCol_FrameBg] = ImVec4{0.065f, 0.13f, 0.17f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = ImVec4{0.10f, 0.22f, 0.25f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = ImVec4{0.09f, 0.27f, 0.25f, 1.0f};
    colors[ImGuiCol_Button] = ImVec4{0.07f, 0.15f, 0.19f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = ImVec4{0.11f, 0.25f, 0.28f, 1.0f};
    colors[ImGuiCol_ButtonActive] = ImVec4{0.10f, 0.35f, 0.31f, 1.0f};
    colors[ImGuiCol_Header] = ImVec4{0.07f, 0.20f, 0.20f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = ImVec4{0.10f, 0.28f, 0.28f, 1.0f};
    colors[ImGuiCol_HeaderActive] = ImVec4{0.12f, 0.38f, 0.33f, 1.0f};
    colors[ImGuiCol_CheckMark] = ImVec4{0.45f, 0.90f, 0.76f, 1.0f};
    colors[ImGuiCol_SliderGrab] = ImVec4{0.35f, 0.72f, 0.65f, 1.0f};
    colors[ImGuiCol_SliderGrabActive] = ImVec4{0.45f, 0.90f, 0.76f, 1.0f};
    colors[ImGuiCol_Separator] = ImVec4{0.12f, 0.22f, 0.27f, 1.0f};
    colors[ImGuiCol_SeparatorHovered] = ImVec4{0.30f, 0.65f, 0.61f, 1.0f};
    colors[ImGuiCol_SeparatorActive] = ImVec4{0.45f, 0.90f, 0.76f, 1.0f};
}

template <typename T>
bool report_result(const aeg::foundation::ResultValue<T>& result, std::string_view operation) {
    return report_result(result.result, operation);
}

std::filesystem::path editor_log_path() {
    return std::filesystem::temp_directory_path() / "AEG-Engine" / "Editor" / "logs" / "aeg-editor.log";
}

struct ResourceDragPayload final {
    uint32_t kind{0};
    uint64_t id{0};
};

inline constexpr const char* kResourcePayload = "AEG.EditorResource";
struct PipelineInspectorSelection {
    std::string title;
    std::string execution;
    std::size_t inputs{0}, outputs{0};
};
std::optional<PipelineInspectorSelection> pipeline_inspector_selection;

enum class EditorDocumentActionType : uint8_t {
    kNewScene,
    kOpenScene,
    kSaveScene,
    kSaveAsScene,
    kNewProject,
    kImportProject,
    kCloseEditor,
};

struct EditorDocumentAction final {
    EditorDocumentActionType type{EditorDocumentActionType::kNewScene};
    std::filesystem::path path{};
    std::string name{};
};

using EditorDocumentActionCallback = std::function<void(EditorDocumentAction)>;

[[nodiscard]] uint32_t resource_kind_value(EditorResourceKind kind) noexcept {
    return static_cast<uint32_t>(kind);
}

class SceneViewportPanel final : public aeg::editor::EditorPanel {
public:
    SceneViewportPanel(EditorSceneRuntime& runtime, ImGuiTextureHandle& texture_handle,
                       EditorDocumentActionCallback document_action) noexcept
        : runtime_(runtime), texture_handle_(texture_handle), document_action_(std::move(document_action)) {}

    void build_ui() override {
        ImGuiWindowClass panel_class;
        panel_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::SetNextWindowClass(&panel_class);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
        ImGui::Begin("Scene Viewport");
        ImGui::PopStyleVar();
        const ImVec2 viewport_origin = ImGui::GetCursorScreenPos();
        ImDrawList* viewport_draw = ImGui::GetWindowDrawList();
        viewport_draw->ChannelsSplit(2);
        viewport_draw->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2{viewport_origin.x + 10.0f, viewport_origin.y + 10.0f});
        if (aeg::editor::toolbar_button("Scene", "New, open, save or save the scene as a new file.",
                                        aeg::editor::ToolbarIcon::None, false, true))
            ImGui::OpenPopup("SceneActions");
        if (ImGui::BeginPopup("SceneActions")) {
            if (ImGui::Button("New")) {
                document_action_(EditorDocumentAction{EditorDocumentActionType::kNewScene});
            }
            ImGui::SameLine();
            if (ImGui::Button("Open")) {
                document_action_(EditorDocumentAction{EditorDocumentActionType::kOpenScene, runtime_.scene_path()});
            }
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                document_action_(EditorDocumentAction{EditorDocumentActionType::kSaveScene});
            }
            ImGui::SameLine();
            if (ImGui::Button("Save As")) {
                const std::filesystem::path copy =
                    runtime_.scene_path().parent_path() / (runtime_.scene_path().stem().string() + "_copy.aegscene");
                document_action_(EditorDocumentAction{EditorDocumentActionType::kSaveAsScene, copy});
            }
            ImGui::SameLine();
            if (aeg::editor::toolbar_button("Play", "Start or resume the game preview.", aeg::editor::ToolbarIcon::Play,
                                            true)) {
                last_operation_ = runtime_.play();
            }
            ImGui::SameLine();
            if (aeg::editor::toolbar_button("Pause", "Pause the running game preview.",
                                            aeg::editor::ToolbarIcon::Pause)) {
                last_operation_ = runtime_.pause();
            }
            ImGui::SameLine();
            if (aeg::editor::toolbar_button("Stop", "Stop preview and return to editing.",
                                            aeg::editor::ToolbarIcon::Stop)) {
                last_operation_ = runtime_.stop();
            }
            ImGui::SameLine();
            ImGui::Text("[%s] %s", mode_name(runtime_.mode()), runtime_.dirty() ? "*" : "");
            if (!last_operation_) {
                ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.3f, 1.0f}, "%s", last_operation_.message.c_str());
            }
            if (!runtime_.diagnostic().empty()) {
                ImGui::TextColored(ImVec4{1.0f, 0.75f, 0.25f, 1.0f}, "%s", std::string(runtime_.diagnostic()).c_str());
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (aeg::editor::toolbar_button(viewport_projection_ == 0 ? "Perspective" : "Orthographic",
                                        "Projection selector: UI preview only; scene camera is unchanged.")) {
            viewport_projection_ = viewport_projection_ == 0 ? 1 : 0;
        }
        ImGui::SameLine();
        if (aeg::editor::toolbar_button(viewport_lit_ ? "Lit" : "Unlit",
                                        "Shading selector: UI preview only; renderer shading is unchanged.")) {
            viewport_lit_ = !viewport_lit_;
        }
        ImGui::SameLine();
        if (aeg::editor::toolbar_button("Show", "Show or hide viewport overlays and transform gizmos.",
                                        aeg::editor::ToolbarIcon::None, viewport_overlays_)) {
            viewport_overlays_ = !viewport_overlays_;
        }
        ImGui::SameLine();
        if (aeg::editor::toolbar_button("Move", "Translate the selected object using its axis handles.",
                                        aeg::editor::ToolbarIcon::None, gizmo_mode_ == ViewportGizmoMode::kTranslate)) {
            gizmo_mode_ = ViewportGizmoMode::kTranslate;
        }
        ImGui::SameLine();
        if (aeg::editor::toolbar_button("Rotate", "Rotate the selected object using the rotation gizmo.",
                                        aeg::editor::ToolbarIcon::None, gizmo_mode_ == ViewportGizmoMode::kRotate)) {
            gizmo_mode_ = ViewportGizmoMode::kRotate;
        }
        ImGui::SameLine();
        if (aeg::editor::toolbar_button("Scale", "Resize the selected object along a chosen axis.",
                                        aeg::editor::ToolbarIcon::None, gizmo_mode_ == ViewportGizmoMode::kScale)) {
            gizmo_mode_ = ViewportGizmoMode::kScale;
        }
        const float toolbar_bottom = ImGui::GetItemRectMax().y + 10.0f;
        viewport_draw->ChannelsSetCurrent(0);
        ImGui::SetCursorScreenPos(viewport_origin);
        const ImVec2 extent = ImGui::GetContentRegionAvail();
        const uint32_t width = static_cast<uint32_t>(std::max(extent.x, 1.0f));
        const uint32_t height = static_cast<uint32_t>(std::max(extent.y, 1.0f));
        static_cast<void>(runtime_.request_viewport_extent(width, height));
        const ImVec2 image_min = ImGui::GetCursorScreenPos();
        const bool present_connected = runtime_.viewport_present_connected();
        if (texture_handle_.is_valid() && runtime_.viewport_ready()) {
            ImGui::Image(static_cast<ImTextureID>(aeg::editor::make_imgui_texture_id(texture_handle_)), extent);
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload);
                if (payload != nullptr && payload->DataSize == static_cast<int>(sizeof(ResourceDragPayload))) {
                    const auto* resource = static_cast<const ResourceDragPayload*>(payload->Data);
                    last_operation_ =
                        runtime_.drop_resource(static_cast<EditorResourceKind>(resource->kind), resource->id);
                }
                ImGui::EndDragDropTarget();
            }
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                 ImGui::GetIO().MousePos.y > toolbar_bottom;
            if (present_connected) {
                draw_gizmo(image_min, extent, width, height);
                handle_input(image_min, extent, width, height, hovered);
            } else {
                viewport_draw->AddText({image_min.x + 18.0f, image_min.y + 18.0f}, IM_COL32(130, 160, 174, 255),
                                       "No render state reaches Present.");
            }
        } else {
            ImGui::TextUnformatted("Scene target is preparing...");
        }
        viewport_draw->ChannelsMerge();
        ImGui::End();
    }

private:
    [[nodiscard]] static float distance_to_segment(ImVec2 point, ImVec2 start, ImVec2 end) noexcept {
        const ImVec2 direction{end.x - start.x, end.y - start.y};
        const ImVec2 offset{point.x - start.x, point.y - start.y};
        const float length_squared = direction.x * direction.x + direction.y * direction.y;
        if (length_squared <= 0.0001f) {
            return std::sqrt(offset.x * offset.x + offset.y * offset.y);
        }
        const float factor = std::clamp((offset.x * direction.x + offset.y * direction.y) / length_squared, 0.0f, 1.0f);
        const ImVec2 closest{start.x + direction.x * factor, start.y + direction.y * factor};
        const ImVec2 delta{point.x - closest.x, point.y - closest.y};
        return std::sqrt(delta.x * delta.x + delta.y * delta.y);
    }

    [[nodiscard]] static ImVec2 screen_point(ImVec2 image_min, ViewportPoint point) noexcept {
        return {image_min.x + point.x, image_min.y + point.y};
    }

    [[nodiscard]] static bool hit_axis(ImVec2 mouse, const ViewportGizmoProjection& projection,
                                       ViewportGizmoAxis& axis) noexcept {
        const ImVec2 center{projection.center.x, projection.center.y};
        const std::array<std::pair<ViewportGizmoAxis, ImVec2>, 3> endpoints{{
            {ViewportGizmoAxis::kX, {projection.x_axis.x, projection.x_axis.y}},
            {ViewportGizmoAxis::kY, {projection.y_axis.x, projection.y_axis.y}},
            {ViewportGizmoAxis::kZ, {projection.z_axis.x, projection.z_axis.y}},
        }};
        float closest = 10.0f;
        bool found = false;
        for (const auto& candidate : endpoints) {
            const float distance = distance_to_segment(mouse, center, candidate.second);
            if (distance <= closest) {
                closest = distance;
                axis = candidate.first;
                found = true;
            }
        }
        return found;
    }

    void draw_gizmo(ImVec2 image_min, ImVec2 extent, uint32_t width, uint32_t height) const {
        const auto projection = runtime_.selected_gizmo_projection(width, height);
        if (!projection || !projection.value.valid) {
            return;
        }
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 image_max{image_min.x + extent.x, image_min.y + extent.y};
        draw_list->PushClipRect(image_min, image_max, true);
        const ImVec2 center = screen_point(image_min, projection.value.center);
        const ImVec2 x_axis = screen_point(image_min, projection.value.x_axis);
        const ImVec2 y_axis = screen_point(image_min, projection.value.y_axis);
        const ImVec2 z_axis = screen_point(image_min, projection.value.z_axis);
        draw_list->AddLine(center, x_axis, IM_COL32(235, 80, 80, 255), 3.0f);
        draw_list->AddLine(center, y_axis, IM_COL32(90, 225, 110, 255), 3.0f);
        draw_list->AddLine(center, z_axis, IM_COL32(90, 145, 245, 255), 3.0f);
        draw_list->AddCircleFilled(center, 5.0f, IM_COL32(245, 245, 245, 255));
        if (gizmo_mode_ == ViewportGizmoMode::kRotate) {
            draw_list->AddCircle(center, 28.0f, IM_COL32(245, 205, 80, 230), 48, 2.0f);
        } else if (gizmo_mode_ == ViewportGizmoMode::kScale) {
            draw_list->AddRectFilled({x_axis.x - 4.0f, x_axis.y - 4.0f}, {x_axis.x + 4.0f, x_axis.y + 4.0f},
                                     IM_COL32(235, 80, 80, 255));
            draw_list->AddRectFilled({y_axis.x - 4.0f, y_axis.y - 4.0f}, {y_axis.x + 4.0f, y_axis.y + 4.0f},
                                     IM_COL32(90, 225, 110, 255));
            draw_list->AddRectFilled({z_axis.x - 4.0f, z_axis.y - 4.0f}, {z_axis.x + 4.0f, z_axis.y + 4.0f},
                                     IM_COL32(90, 145, 245, 255));
        }
        draw_list->PopClipRect();
    }

    void handle_input(ImVec2 image_min, ImVec2 extent, uint32_t width, uint32_t height, bool hovered) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 raw_local{mouse.x - image_min.x, mouse.y - image_min.y};
        const ViewportPoint local{
            std::clamp(raw_local.x, 0.0f, std::max(extent.x - 0.001f, 0.0f)),
            std::clamp(raw_local.y, 0.0f, std::max(extent.y - 0.001f, 0.0f)),
        };
        if (runtime_.transform_edit_active()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                last_operation_ = runtime_.cancel_transform_edit();
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                last_operation_ = runtime_.update_transform_edit(local, width, height);
            } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                last_operation_ = runtime_.update_transform_edit(local, width, height);
                if (last_operation_) {
                    last_operation_ = runtime_.commit_transform_edit();
                }
            }
            return;
        }
        if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            return;
        }
        const auto projection = runtime_.selected_gizmo_projection(width, height);
        if (projection && projection.value.valid) {
            ViewportGizmoAxis axis{};
            if (hit_axis(raw_local, projection.value, axis)) {
                last_operation_ = runtime_.begin_transform_edit(gizmo_mode_, axis, local, width, height);
                if (last_operation_) {
                    return;
                }
            }
        }
        const auto picked = runtime_.pick_viewport(local, width, height);
        if (picked && picked.value.hit) {
            const ImGuiIO& io = ImGui::GetIO();
            const SelectionOperation operation =
                io.KeyCtrl ? SelectionOperation::kToggle
                           : (io.KeyShift ? SelectionOperation::kAdd : SelectionOperation::kReplace);
            last_operation_ = runtime_.select(picked.value.entity, operation);
        } else if (!picked) {
            last_operation_ = picked.result;
        }
    }

    [[nodiscard]] static const char* mode_name(EditorSceneRuntime::Mode mode) noexcept {
        switch (mode) {
            case EditorSceneRuntime::Mode::kEdit:
                return "Edit";
            case EditorSceneRuntime::Mode::kPlay:
                return "Play";
            case EditorSceneRuntime::Mode::kPause:
                return "Pause";
        }
        return "Unknown";
    }

    EditorSceneRuntime& runtime_;
    ImGuiTextureHandle& texture_handle_;
    EditorDocumentActionCallback document_action_;
    ViewportGizmoMode gizmo_mode_{ViewportGizmoMode::kTranslate};
    int viewport_projection_{0};
    bool viewport_lit_{true};
    bool viewport_overlays_{true};
    Result last_operation_{Result::success()};
};

class HierarchyPanel final : public aeg::editor::EditorPanel {
public:
    explicit HierarchyPanel(EditorSceneRuntime& runtime) noexcept : runtime_(runtime) {}

    void build_ui() override {
        ImGuiWindowClass panel_class;
        panel_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::SetNextWindowClass(&panel_class);
        ImGui::Begin("Hierarchy");
        ImGui::TextUnformatted("Hierarchy");
        ImGui::SameLine();
        if (ImGui::SmallButton("Find"))
            search_visible_ = !search_visible_;
        ImGui::SameLine();
        if (ImGui::SmallButton("..."))
            ImGui::OpenPopup("TreeActions");
        if (ImGui::BeginPopup("TreeActions")) {
            if (ImGui::Button("Expand All")) {
                expand_all_ = true;
                collapse_all_ = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Collapse All")) {
                collapse_all_ = true;
                expand_all_ = false;
            }
            ImGui::EndPopup();
        }
        if (search_visible_) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##entity-filter", "Find entity...", search_, sizeof(search_));
        }
        if (!last_operation_) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", last_operation_.message.c_str());
        }
        const auto compact_layout = aeg::application::compact_hierarchy_tree_layout();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, compact_layout.item_spacing);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, compact_layout.frame_padding);
        const auto roots = runtime_.children({});
        const bool world_open =
            ImGui::TreeNodeEx("World", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding |
                                           ImGuiTreeNodeFlags_SpanAvailWidth);
        if (roots && world_open) {
            for (std::size_t index = 0; index < roots.value.size(); ++index) {
                draw_drop_zone({}, index);
                draw_entity(roots.value[index]);
            }
            draw_drop_zone({}, roots.value.size());
        }
        if (world_open)
            ImGui::TreePop();
        ImGui::Dummy(ImVec2{0.0f, compact_layout.footer_gap});
        if (ImGui::BeginDragDropTarget()) {
            accept_drop({}, static_cast<std::size_t>(-1), HierarchyTransformPolicy::kKeepWorld);
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextWindow("HierarchyContext", ImGuiPopupFlags_MouseButtonRight)) {
            const auto selected = selected_for_context({});
            if (ImGui::MenuItem("Create Root Entity")) {
                last_operation_ = runtime_.create_scene_entity({}, "Entity");
            }
            if (ImGui::MenuItem("Paste / Duplicate Selection", nullptr, false, !selected.empty())) {
                last_operation_ = runtime_.duplicate_entities(selected);
            }
            ImGui::EndPopup();
        }
        expand_all_ = false;
        collapse_all_ = false;
        ImGui::PopStyleVar(2);
        ImGui::End();
    }

private:
    static constexpr const char* kEntityPayload = "AEG.SceneEntitySelection";

    [[nodiscard]] std::vector<scene::SceneEntityId> selected_for_context(scene::SceneEntityId fallback) const {
        std::vector<scene::SceneEntityId> result;
        const auto selected = runtime_.selected_entities();
        if (!selected.empty()) {
            result.assign(selected.begin(), selected.end());
        } else if (fallback.is_valid()) {
            result.push_back(fallback);
        }
        return result;
    }

    void draw_drop_zone(scene::SceneEntityId parent, std::size_t sibling_index) {
        aeg::application::push_hierarchy_item_id(parent.high(), parent.low());
        const uint64_t sibling_identity = static_cast<uint64_t>(sibling_index);
        ImGui::PushID(static_cast<int>(static_cast<uint32_t>(sibling_identity >> 32u)));
        ImGui::PushID(static_cast<int>(static_cast<uint32_t>(sibling_identity)));
        ImGui::InvisibleButton("##HierarchyDropZone",
                               ImVec2{-1.0f, aeg::application::compact_hierarchy_tree_layout().drop_zone_height});
        if (ImGui::BeginDragDropTarget()) {
            accept_drop(parent, sibling_index, HierarchyTransformPolicy::kKeepWorld);
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        ImGui::PopID();
        aeg::application::pop_hierarchy_item_id();
    }

    void accept_drop(scene::SceneEntityId parent, std::size_t sibling_index,
                     HierarchyTransformPolicy transform_policy) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload);
        if (payload == nullptr || payload->DataSize <= 0 ||
            payload->DataSize % static_cast<int>(sizeof(scene::SceneEntityId)) != 0) {
            return;
        }
        const auto* ids = static_cast<const scene::SceneEntityId*>(payload->Data);
        const std::span<const scene::SceneEntityId> dragged(
            ids, static_cast<std::size_t>(payload->DataSize) / sizeof(scene::SceneEntityId));
        last_operation_ = runtime_.reparent_entities(dragged, parent, sibling_index, transform_policy);
    }

    void draw_entity(scene::SceneEntityId entity) {
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            pipeline_inspector_selection.reset();
        const auto record = std::find_if(runtime_.entities().begin(), runtime_.entities().end(),
                                         [entity](const auto& value) { return value.id == entity; });
        if (record == runtime_.entities().end()) {
            return;
        }
        if (search_visible_ && search_[0] && runtime_.hierarchy_path(entity).value.find(search_) == std::string::npos &&
            !contains_match(entity))
            return;
        const auto child_entities = runtime_.children(entity);
        const bool has_children = child_entities && !child_entities.value.empty();
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
        if (!has_children) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
        }
        if (runtime_.is_selected(entity)) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (expand_all_ || (search_visible_ && search_[0])) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        } else if (collapse_all_) {
            ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        }
        aeg::application::push_hierarchy_item_id(entity.high(), entity.low());
        const bool open = ImGui::TreeNodeEx("##HierarchyEntity", flags, "%s", record->name.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const ImGuiIO& io = ImGui::GetIO();
            const SelectionOperation operation =
                io.KeyCtrl ? SelectionOperation::kToggle
                           : (io.KeyShift ? SelectionOperation::kAdd : SelectionOperation::kReplace);
            last_operation_ = runtime_.select(entity, operation);
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            drag_entities_.clear();
            if (runtime_.is_selected(entity)) {
                const auto selected = runtime_.selected_entities();
                drag_entities_.assign(selected.begin(), selected.end());
            } else {
                drag_entities_.push_back(entity);
            }
            ImGui::SetDragDropPayload(kEntityPayload, drag_entities_.data(),
                                      drag_entities_.size() * sizeof(scene::SceneEntityId));
            ImGui::Text("Move %s", record->name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            accept_drop(entity, static_cast<std::size_t>(-1), HierarchyTransformPolicy::kKeepWorld);
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (!runtime_.is_selected(entity)) {
                last_operation_ = runtime_.select(entity);
            }
            const auto selected = selected_for_context(entity);
            if (ImGui::MenuItem("Create Child")) {
                last_operation_ = runtime_.create_scene_entity(entity, "Entity");
            }
            if (ImGui::MenuItem("Duplicate")) {
                last_operation_ = runtime_.duplicate_entities(selected);
            }
            if (ImGui::MenuItem("Set Root (Keep World)")) {
                last_operation_ = runtime_.reparent_entities(selected, {}, static_cast<std::size_t>(-1),
                                                             HierarchyTransformPolicy::kKeepWorld);
            }
            if (ImGui::MenuItem("Set Root (Keep Local)")) {
                last_operation_ = runtime_.reparent_entities(selected, {}, static_cast<std::size_t>(-1),
                                                             HierarchyTransformPolicy::kKeepLocal);
            }
            if (ImGui::MenuItem("Delete")) {
                last_operation_ = runtime_.remove_entities(selected, scene::SceneEntityRemovalPolicy::kCascade);
            }
            if (ImGui::MenuItem("Delete and Promote Children")) {
                last_operation_ = runtime_.remove_entities(selected, scene::SceneEntityRemovalPolicy::kPromoteChildren);
            }
            ImGui::EndPopup();
        }
        if (open) {
            if (child_entities) {
                for (std::size_t index = 0; index < child_entities.value.size(); ++index) {
                    draw_drop_zone(entity, index);
                    draw_entity(child_entities.value[index]);
                }
                draw_drop_zone(entity, child_entities.value.size());
            }
            ImGui::TreePop();
        }
        aeg::application::pop_hierarchy_item_id();
    }

    bool contains_match(scene::SceneEntityId entity) const {
        const auto path = runtime_.hierarchy_path(entity);
        if (path && path.value.find(search_) != std::string::npos)
            return true;
        const auto children = runtime_.children(entity);
        if (children)
            for (auto child : children.value)
                if (contains_match(child))
                    return true;
        return false;
    }
    EditorSceneRuntime& runtime_;
    bool search_visible_{false};
    char search_[128]{};
    std::vector<scene::SceneEntityId> drag_entities_{};
    bool expand_all_{false};
    bool collapse_all_{false};
    Result last_operation_{Result::success()};
};

class PipelinePanel final : public aeg::editor::EditorPanel {
public:
    PipelinePanel(PipelineScopeHost& host, EditorSceneRuntime& runtime,
                  aeg::application::PipelineOrderingDraftQueue& drafts,
                  aeg::application::PipelineLayoutSaveState& layouts, std::function<Result()> save_changes)
        : host_(host), runtime_(runtime), drafts_(drafts), layouts_(layouts), save_changes_(std::move(save_changes)) {}
    void build_ui() override {
        ImGuiWindowClass wc;
        wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::SetNextWindowClass(&wc);
        if (expand_) {
            const auto* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowDockID(0);
            ImGui::SetNextWindowPos({viewport->WorkPos.x + 80, viewport->WorkPos.y + 60});
            ImGui::SetNextWindowSize(
                {std::max(400.0f, viewport->WorkSize.x - 160), std::max(300.0f, viewport->WorkSize.y - 120)});
            expand_ = false;
        }
        ImGui::Begin("Pipeline");
        auto sources = runtime_.pipeline_snapshots();
        if (aeg::application::builtin_pipeline_debug_enabled()) {
            if (auto snapshot = host_.active_plan_snapshot())
                sources.emplace_back("Built-in / Editor UI", std::move(snapshot));
        }
        if (sources.empty()) {
            ImGui::TextDisabled("This project has no Pipeline yet.");
            if (runtime_.project_loaded() && ImGui::Button("Create Renderer Pipeline")) {
                Result result = runtime_.create_project_renderer_pipeline();
                if (result)
                    result = save_changes_();
                feedback_ = result ? "Renderer Pipeline created." : result.message;
            }
            if (!feedback_.empty())
                ImGui::TextWrapped("%s", feedback_.c_str());
            ImGui::End();
            return;
        }
        source_ = std::min(source_, sources.size() - 1);
        ImGui::TextUnformatted("Pipeline");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(210);
        if (ImGui::BeginCombo("##pipeline-source", sources[source_].first.c_str())) {
            for (std::size_t i = 0; i < sources.size(); ++i)
                if (ImGui::Selectable(sources[i].first.c_str(), i == source_)) {
                    source_ = i;
                    signature_.clear();
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Mode");
        ImGui::SameLine();
        ImGui::BeginDisabled(!details_);
        if (ImGui::SmallButton("Overview")) {
            details_ = false;
            signature_.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(details_);
        if (ImGui::SmallButton("Details")) {
            details_ = true;
            signature_.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Arrange")) {
            arrange();
            capture_layout();
            fit_ = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Frame All"))
            fit_ = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("Expand"))
            expand_ = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!drafts_.dirty() && !layout_dirty_ && !blend_dirty_);
        if (ImGui::SmallButton("Save Changes")) {
            Result result = apply_pending_blends();
            if (result)
                result = save_changes_();
            if (result) {
                layout_dirty_ = false;
                blend_dirty_ = false;
                pending_blends_.clear();
            } else
                rollback_pending_blends();
            feedback_ = result ? "Pipeline graph saved; state links apply at the next frame boundary." : result.message;
        }
        ImGui::EndDisabled();
        const auto& plan = *sources[source_].second;
        active_source_ = sources[source_].first;
        const bool render_state_graph = details_ && sources[source_].first == "Game / Renderer";
        std::string state_override_signature;
        if (render_state_graph) {
            for (const auto& override : runtime_.render_state_link_overrides()) {
                state_override_signature += ':' + std::to_string(override.source_node) + '>' +
                                            std::to_string(override.destination_node) +
                                            (override.connected ? '+' : '-');
            }
        }
        const std::vector<aeg::render_graph::RenderStateLinkOverride> state_overrides =
            active_source_ == "Game / Renderer" ? runtime_.render_state_link_overrides()
                                                : std::vector<aeg::render_graph::RenderStateLinkOverride>{};
        const std::vector<aeg::render_graph::RenderStateBlendOverride> blend_overrides =
            render_state_graph ? runtime_.render_state_blend_overrides()
                               : std::vector<aeg::render_graph::RenderStateBlendOverride>{};
        for (const auto& override : blend_overrides) {
            state_override_signature +=
                ':' + std::to_string(override.node) + '=' + std::to_string(static_cast<uint32_t>(override.blend_mode));
        }
        const std::string signature = sources[source_].first + std::to_string(plan.pipeline.value()) + ":" +
                                      std::to_string(plan.revision) + (details_ ? ":details" : ":overview") +
                                      state_override_signature;
        if (signature != signature_) {
            rebuild(plan, state_overrides, blend_overrides, render_state_graph);
            signature_ = signature;
        }
        ImGui::TextDisabled("%zu nodes / %zu features   |   %s   |   Wheel: zoom / middle-drag: pan", nodes_.size(),
                            plan.feature_implementations.size(),
                            details_ ? "Renderer links edit render state; save to apply changes"
                                     : "Switch to Details to edit saved ordering");
        if (drafts_.dirty())
            ImGui::TextDisabled("%zu pipeline change(s) pending — Save Changes to apply.", drafts_.changes().size());
        if (!pending_blends_.empty())
            ImGui::TextDisabled("%zu state blend change(s) pending — Save Changes to apply.", pending_blends_.size());
        if (!feedback_.empty())
            ImGui::TextWrapped("%s", feedback_.c_str());
        ImGui::BeginChild("PipelineGraph", ImVec2{0, 0}, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x < 80 || size.y < 40) {
            ImGui::EndChild();
            ImGui::End();
            return;
        }
        const float font = ImGui::GetFontSize();
        const float old_width = width_, old_height = height_;
        width_ = 250;
        height_ = font * 3 + 40;
        for (const auto& node : nodes_)
            width_ = std::max(width_, ImGui::CalcTextSize(node.title.c_str()).x + 42);
        if (old_width != width_ || old_height != height_) {
            arrange();
            restore_layout(plan);
            fit_ = true;
        }
        if (std::abs(size.x - last_size_.x) > 1 || std::abs(size.y - last_size_.y) > 1)
            fit_ = true;
        last_size_ = size;
        if (fit_) {
            float max_x = width_, max_y = height_;
            for (const auto& node : nodes_) {
                max_x = std::max(max_x, node.pos.x + width_);
                max_y = std::max(max_y, node.pos.y + height_);
            }
            zoom_ = std::clamp(std::min((size.x - 40) / max_x, (size.y - 30) / max_y), 0.65f, 1.0f);
            offset_ = {std::max(20.0f, (size.x - max_x * zoom_) * 0.5f),
                       std::max(15.0f, (size.y - max_y * zoom_) * 0.5f)};
            fit_ = false;
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // Own blank-canvas presses so ImGui does not drag the docked/floating window.
        ImGui::InvisibleButton(
            "##GraphCanvas", size,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        const auto screen = [&](ImVec2 p) {
            return ImVec2{origin.x + offset_.x + p.x * zoom_, origin.y + offset_.y + p.y * zoom_};
        };
        const auto& io = ImGui::GetIO();
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hovered && io.MouseWheel != 0) {
            const float next = std::clamp(zoom_ + io.MouseWheel * 0.08f, 0.15f, 1.8f);
            const ImVec2 anchor{io.MousePos.x - origin.x, io.MousePos.y - origin.y};
            offset_ = {anchor.x - (anchor.x - offset_.x) * next / zoom_,
                       anchor.y - (anchor.y - offset_.y) * next / zoom_};
            zoom_ = next;
        }
        if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            offset_.x += io.MouseDelta.x;
            offset_.y += io.MouseDelta.y;
        }
        int hit = -1, port = -1;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const ImVec2 p = screen(nodes_[i].pos);
            const float dx = io.MousePos.x - (p.x + width_ * zoom_),
                        dy = io.MousePos.y - (p.y + height_ * zoom_ * 0.5f);
            const bool has_state_input = std::any_of(edges_.begin(), edges_.end(),
                                                     [i](const Edge& edge) { return edge.to == static_cast<int>(i); });
            if (hovered && dx * dx + dy * dy < 100.0f &&
                (!render_state_graph ||
                 (nodes_[i].phase != aeg::pipeline::EnginePhase::kPresent &&
                  aeg::application::can_start_render_state_link(nodes_[i].writes_render_state, has_state_input)))) {
                port = static_cast<int>(i);
            }
            if (hovered && io.MousePos.x >= p.x && io.MousePos.x <= p.x + width_ * zoom_ && io.MousePos.y >= p.y &&
                io.MousePos.y <= p.y + height_ * zoom_)
                hit = static_cast<int>(i);
        }
        const bool cancel_gesture = ImGui::IsKeyPressed(ImGuiKey_Escape) || io.AppFocusLost;
        const ImVec2 pan_delta =
            pan_.update(hovered, hit < 0 && port < 0, connecting_ >= 0, ImGui::IsMouseClicked(ImGuiMouseButton_Left),
                        ImGui::IsMouseDown(ImGuiMouseButton_Left), ImGui::IsMouseDragging(ImGuiMouseButton_Left),
                        cancel_gesture, io.MouseDelta);
        offset_.x += pan_delta.x;
        offset_.y += pan_delta.y;
        if (cancel_gesture) {
            connecting_ = -1;
            dragging_ = -1;
        }
        int double_clicked_edge = -1;
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            for (int index = static_cast<int>(edges_.size()) - 1; index >= 0; --index) {
                const auto& edge = edges_[index];
                const auto& from = nodes_[edge.from];
                const auto& to = nodes_[edge.to];
                if (aeg::application::graph_link_hit(screen({from.pos.x + width_, from.pos.y + height_ * 0.5f}),
                                                     screen({to.pos.x, to.pos.y + height_ * 0.5f}), io.MousePos,
                                                     zoom_)) {
                    double_clicked_edge = index;
                    break;
                }
            }
        }
        if (double_clicked_edge >= 0) {
            // Double-clicking a wire always wins over a pending port/node gesture.
            connecting_ = -1;
            dragging_ = -1;
            const Edge edge = edges_[double_clicked_edge];
            drafts_.stage_remove(active_source_, nodes_[edge.from].plan_node, nodes_[edge.to].plan_node);
            edges_.erase(edges_.begin() + double_clicked_edge);
            feedback_ = "Connection removal is pending. Save Changes to apply it.";
        }
        if (double_clicked_edge < 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (port >= 0) {
                connecting_ = port;
                latched_ = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                dragging_ = -1;
            } else if (connecting_ >= 0 && hit >= 0) {
                connect(connecting_, hit);
                connecting_ = -1;
            } else if (hit >= 0) {
                selected_ = hit;
                dragging_ = hit;
                PipelineInspectorSelection selection{nodes_[hit].title, nodes_[hit].subtitle};
                for (const auto& edge : edges_) {
                    selection.inputs += edge.to == hit;
                    selection.outputs += edge.from == hit;
                }
                pipeline_inspector_selection = std::move(selection);
            }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit >= 0 &&
            active_source_ == "Game / Renderer" && nodes_[hit].writes_render_state) {
            blend_popup_node_ = hit;
            ImGui::OpenPopup("##RenderStateBlend");
        }
        if (ImGui::BeginPopup("##RenderStateBlend")) {
            if (blend_popup_node_ >= 0 && blend_popup_node_ < static_cast<int>(nodes_.size())) {
                Node& node = nodes_[blend_popup_node_];
                ImGui::TextUnformatted("State blend");
                constexpr std::array<const char*, 3> names{"Replace", "Over", "Add"};
                for (uint32_t index = 0; index < names.size(); ++index) {
                    if (ImGui::MenuItem(names[index], nullptr, node.blend_mode == index)) {
                        stage_blend(node, index);
                    }
                }
            }
            ImGui::EndPopup();
        }
        if (dragging_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            nodes_[dragging_].pos.x += io.MouseDelta.x / zoom_;
            nodes_[dragging_].pos.y += io.MouseDelta.y / zoom_;
            capture_layout_node(nodes_[dragging_]);
            layout_dirty_ = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            dragging_ = -1;
            if (connecting_ >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (hit >= 0 && hit != connecting_) {
                    connect(connecting_, hit);
                    connecting_ = -1;
                } else if (!latched_)
                    connecting_ = -1;
            }
        }
        auto* draw = ImGui::GetWindowDrawList();
        const auto link = [&](ImVec2 from, ImVec2 to, ImU32 color) {
            const float bend = std::max(50.0f * zoom_, std::abs(to.x - from.x) * 0.45f);
            draw->AddBezierCubic(from, {from.x + bend, from.y}, {to.x - bend, to.y}, to, color, 1.8f);
            draw->AddLine({to.x - 10, to.y - 4}, {to.x - 4, to.y}, color, 1.6f);
            draw->AddLine({to.x - 10, to.y + 4}, {to.x - 4, to.y}, color, 1.6f);
        };
        for (auto edge : edges_) {
            const auto& from = nodes_[edge.from];
            const auto& to = nodes_[edge.to];
            const bool carries_state = from.state.reaches_output && to.state.reaches_output;
            link(screen({from.pos.x + width_, from.pos.y + height_ * 0.5f}),
                 screen({to.pos.x, to.pos.y + height_ * 0.5f}),
                 !carries_state ? IM_COL32(65, 88, 100, 145)
                                : (edge.runtime_override || edge.from == selected_ ? IM_COL32(115, 230, 195, 255)
                                                                                   : IM_COL32(105, 169, 184, 230)));
        }
        if (connecting_ >= 0) {
            const auto& node = nodes_[connecting_];
            link(screen({node.pos.x + width_, node.pos.y + height_ * 0.5f}), io.MousePos, IM_COL32(240, 182, 106, 255));
        }
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const auto& node = nodes_[i];
            const auto p = screen(node.pos);
            const bool active_output = node.state.reaches_output;
            const ImU32 border = i == selected_
                                     ? IM_COL32(115, 230, 195, 255)
                                     : (active_output ? IM_COL32(58, 110, 121, 255) : IM_COL32(48, 78, 91, 150));
            draw->AddRectFilled(p, {p.x + width_ * zoom_, p.y + height_ * zoom_},
                                i == selected_
                                    ? IM_COL32(23, 58, 53, 255)
                                    : (active_output ? IM_COL32(17, 43, 53, 255) : IM_COL32(15, 28, 37, 220)),
                                6);
            draw->AddRect(p, {p.x + width_ * zoom_, p.y + height_ * zoom_}, border, 6);
            const auto text = [&](float y, const char* value, ImU32 color) {
                draw->AddText(ImGui::GetFont(), font * zoom_, {p.x + 16 * zoom_, p.y + y * zoom_}, color, value);
            };
            text(12, node.title.c_str(), IM_COL32(235, 245, 246, 255));
            text(18 + font, node.subtitle.c_str(), IM_COL32(145, 179, 191, 255));
            text(24 + font * 2, node.detail.c_str(), IM_COL32(115, 149, 162, 255));
            for (float x : {p.x, p.x + width_ * zoom_}) {
                draw->AddCircleFilled({x, p.y + height_ * zoom_ * 0.5f}, 4, IM_COL32(10, 23, 32, 255));
                draw->AddCircle({x, p.y + height_ * zoom_ * 0.5f}, 4, border, 12, 1.5f);
            }
        }
        ImGui::EndChild();
        ImGui::End();
    }

private:
    struct Node {
        std::string title, subtitle, detail;
        aeg::pipeline::EnginePhase phase{aeg::pipeline::EnginePhase::kFrameBegin};
        aeg::application::PipelineGraphState state{};
        ImVec2 pos{};
        uint32_t plan_node{aeg::pipeline::kInvalidPipelineHandleIndex};
        int render_contribution{-1};
        bool writes_render_state{false};
        uint8_t blend_mode{0};
    };
    struct Edge {
        int from{-1};
        int to{-1};
        bool runtime_override{false};
    };
    struct PendingBlend final {
        uint32_t node{aeg::pipeline::kInvalidPipelineHandleIndex};
        uint8_t mode{0};
        uint8_t previous_mode{0};
    };
    static std::string hex(uint64_t id) {
        char text[24];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(id));
        return text;
    }
    static const char* phase_name(aeg::pipeline::EnginePhase phase) {
        constexpr const char* names[]{"Startup", "Frame Begin",  "Input",          "Pre Update",   "Fixed Update",
                                      "Update",  "World Commit", "Render Extract", "Render Build", "Render Execute",
                                      "Present", "Frame End",    "Shutdown"};
        const auto i = static_cast<unsigned>(phase);
        return i < 13 ? names[i] : "Custom Phase";
    }
    static const char* blend_name(uint8_t blend_mode) {
        constexpr std::array<const char*, 3> names{"Replace", "Over", "Add"};
        return blend_mode < names.size() ? names[blend_mode] : "Replace";
    }
    void rebuild(const aeg::pipeline::PipelinePlan& plan,
                 std::span<const aeg::render_graph::RenderStateLinkOverride> state_overrides = {},
                 std::span<const aeg::render_graph::RenderStateBlendOverride> blend_overrides = {},
                 bool render_state_graph = false) {
        if (render_state_graph) {
            rebuild_render_state_graph(plan, state_overrides, blend_overrides);
            return;
        }
        nodes_.clear();
        edges_.clear();
        connecting_ = dragging_ = -1;
        feedback_.clear();
        const aeg::render_graph::RenderStateFlow render_state_flow =
            aeg::render_graph::build_render_state_flow(plan, state_overrides);
        std::vector<std::string> contribution_names(plan.feature_contributions.size());
        std::vector<uint8_t> contribution_writes_state(plan.feature_contributions.size(), 0);
        for (std::size_t contribution_index = 0; contribution_index < plan.feature_contributions.size();
             ++contribution_index) {
            const auto& contribution = plan.feature_contributions[contribution_index];
            if (contribution.domain != aeg::pipeline::FeatureDomain::kRender)
                continue;
            for (const auto& implementation : plan.feature_implementations) {
                if (implementation.feature_instance != contribution.feature_instance || !implementation.implementation)
                    continue;
                contribution_names[contribution_index] = std::string(implementation.implementation->descriptor().name);
                contribution_writes_state[contribution_index] =
                    implementation.implementation->uses_render_state_output() ? 1u : 0u;
                break;
            }
        }
        std::vector<std::string> keys;
        std::vector<int> mapping(plan.nodes.size(), -1);
        std::vector<uint32_t> overview_task_counts;
        std::vector<std::unordered_set<uint64_t>> overview_feature_keys;
        for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
            const auto& task = plan.nodes[i];
            std::string feature;
            uint64_t feature_key = 0;
            int render_contribution = -1;
            for (std::size_t contribution_index = 0; contribution_index < plan.feature_contributions.size();
                 ++contribution_index) {
                const auto& contribution = plan.feature_contributions[contribution_index];
                if (task.definition_stage_index < contribution.first_stage_index ||
                    task.definition_stage_index >= contribution.first_stage_index + contribution.stage_count)
                    continue;
                feature_key = contribution.feature_instance.value();
                if (contribution.domain == aeg::pipeline::FeatureDomain::kRender) {
                    render_contribution = static_cast<int>(contribution_index);
                    feature = contribution_names[contribution_index];
                }
                break;
            }
            const std::string key = details_
                                        ? std::to_string(i)
                                        : aeg::application::pipeline_overview_key(static_cast<uint32_t>(task.phase));
            auto found = std::find(keys.begin(), keys.end(), key);
            if (found == keys.end()) {
                mapping[i] = static_cast<int>(nodes_.size());
                keys.push_back(key);
                const std::string title = feature.empty() ? phase_name(task.phase) : feature;
                const std::string subtitle =
                    details_ ? (task.synthetic ? "Stage anchor" : "Task " + hex(task.task.value()).substr(8))
                             : "0 compiled steps · 0 features";
                nodes_.push_back({details_ ? title : phase_name(task.phase),
                                  subtitle,
                                  details_ ? phase_name(task.phase) : "Execution phase",
                                  task.phase,
                                  {},
                                  {},
                                  details_ ? static_cast<uint32_t>(i) : aeg::pipeline::kInvalidPipelineHandleIndex,
                                  details_ ? render_contribution : -1,
                                  details_ && render_contribution >= 0 &&
                                      contribution_writes_state[static_cast<std::size_t>(render_contribution)] != 0});
                overview_task_counts.push_back(0);
                overview_feature_keys.emplace_back();
            } else
                mapping[i] = static_cast<int>(found - keys.begin());
            if (!details_) {
                const auto node_index = static_cast<std::size_t>(mapping[i]);
                ++overview_task_counts[node_index];
                if (feature_key != 0)
                    overview_feature_keys[node_index].insert(feature_key);
                nodes_[node_index].subtitle = std::to_string(overview_task_counts[node_index]) + " compiled steps · " +
                                              std::to_string(overview_feature_keys[node_index].size()) + " features";
            }
        }
        if (details_) {
            for (const auto& dependency : plan.dependencies) {
                if (dependency.source_node < mapping.size() && dependency.destination_node < mapping.size() &&
                    mapping[dependency.source_node] >= 0 && mapping[dependency.destination_node] >= 0) {
                    edges_.push_back({mapping[dependency.source_node], mapping[dependency.destination_node],
                                      dependency.runtime_override});
                }
            }
        } else
            for (std::size_t source = 0; source < mapping.size(); ++source)
                if (mapping[source] >= 0) {
                    std::vector<std::size_t> queue{source};
                    std::vector<bool> seen(mapping.size(), false);
                    for (std::size_t q = 0; q < queue.size(); ++q) {
                        const auto current = queue[q];
                        if (seen[current])
                            continue;
                        seen[current] = true;
                        for (const auto& edge : plan.dependencies)
                            if (edge.source_node == current && edge.destination_node < mapping.size()) {
                                const int destination = mapping[edge.destination_node];
                                if (destination < 0 || destination == mapping[source])
                                    queue.push_back(edge.destination_node);
                                else {
                                    const Edge pair{mapping[source], destination, false};
                                    if (std::none_of(edges_.begin(), edges_.end(), [&](const Edge& edge) {
                                            return edge.from == pair.from && edge.to == pair.to;
                                        }))
                                        edges_.push_back(pair);
                                }
                            }
                    }
                }
        if (details_) {
            for (const auto& override : state_overrides) {
                if (override.source_node >= mapping.size() || override.destination_node >= mapping.size() ||
                    mapping[override.source_node] < 0 || mapping[override.destination_node] < 0) {
                    continue;
                }
                const Edge edge{mapping[override.source_node], mapping[override.destination_node], true};
                const auto found = std::find_if(edges_.begin(), edges_.end(), [&](const Edge& candidate) {
                    return candidate.from == edge.from && candidate.to == edge.to;
                });
                if (override.connected) {
                    if (found == edges_.end()) {
                        edges_.push_back(edge);
                    } else {
                        found->runtime_override = true;
                    }
                } else if (found != edges_.end()) {
                    edges_.erase(found);
                }
            }
        }
        std::vector<std::pair<int, int>> state_edges;
        state_edges.reserve(edges_.size());
        for (const auto& edge : edges_)
            state_edges.emplace_back(edge.from, edge.to);
        std::vector<int> output_nodes;
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            if (nodes_[index].phase == aeg::pipeline::EnginePhase::kPresent)
                output_nodes.push_back(static_cast<int>(index));
        }
        const auto states = aeg::application::pipeline_graph_state_overlay(nodes_.size(), state_edges, output_nodes);
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            nodes_[index].state = states[index];
            if (details_ && nodes_[index].render_contribution >= 0) {
                const auto& state =
                    render_state_flow.contributions[static_cast<std::size_t>(nodes_[index].render_contribution)];
                nodes_[index].state = {state.reaches_present, state.input_count, state.layer_count};
                if (!state.reaches_present) {
                    nodes_[index].detail = "Detached state · no route to Present";
                } else if (nodes_[index].writes_render_state) {
                    nodes_[index].detail = "State output · " + std::to_string(state.layer_count) + " layer(s) · " +
                                           std::to_string(state.input_count) + " input(s)";
                } else {
                    nodes_[index].detail = "State consumer · " + std::to_string(state.layer_count) + " layer(s)";
                }
            } else if (!states[index].reaches_output) {
                nodes_[index].detail = "Detached · no state reaches Present";
            } else if (nodes_[index].phase == aeg::pipeline::EnginePhase::kPresent) {
                nodes_[index].detail =
                    "Final output · " + std::to_string(states[index].layer_count) + " state layer(s)";
            } else {
                nodes_[index].detail = "State " + std::to_string(states[index].layer_count) + " layer(s) · " +
                                       std::to_string(states[index].input_count) + " input(s)";
            }
        }
        selected_ = nodes_.empty() ? -1 : 0;
        arrange();
        restore_layout(plan);
        fit_ = true;
    }
    void rebuild_render_state_graph(const aeg::pipeline::PipelinePlan& plan,
                                    std::span<const aeg::render_graph::RenderStateLinkOverride> state_overrides,
                                    std::span<const aeg::render_graph::RenderStateBlendOverride> blend_overrides) {
        nodes_.clear();
        edges_.clear();
        connecting_ = dragging_ = -1;
        feedback_.clear();
        const auto flow = aeg::render_graph::build_render_state_flow(plan, state_overrides);
        std::vector<int> contribution_nodes(plan.feature_contributions.size(), -1);
        for (std::size_t contribution_index = 0; contribution_index < plan.feature_contributions.size();
             ++contribution_index) {
            const auto& contribution = plan.feature_contributions[contribution_index];
            if (contribution.domain != aeg::pipeline::FeatureDomain::kRender)
                continue;
            uint32_t plan_node = aeg::pipeline::kInvalidPipelineHandleIndex;
            for (uint32_t node_index = 0; node_index < plan.nodes.size(); ++node_index) {
                const auto stage = plan.nodes[node_index].definition_stage_index;
                if (stage >= contribution.first_stage_index &&
                    stage < contribution.first_stage_index + contribution.stage_count) {
                    plan_node = node_index;
                    break;
                }
            }
            if (plan_node == aeg::pipeline::kInvalidPipelineHandleIndex)
                continue;
            std::string title = "Render Feature";
            bool writes_state = false;
            uint8_t blend_mode = static_cast<uint8_t>(aeg::pipeline::RenderStateBlendMode::kReplace);
            for (const auto& implementation : plan.feature_implementations) {
                if (implementation.feature_instance == contribution.feature_instance && implementation.implementation) {
                    title = std::string(implementation.implementation->descriptor().name);
                    writes_state = implementation.implementation->uses_render_state_output();
                    blend_mode = static_cast<uint8_t>(implementation.implementation->render_state_blend_mode());
                    break;
                }
            }
            for (const auto& override : blend_overrides)
                if (override.node == plan_node) {
                    blend_mode = static_cast<uint8_t>(override.blend_mode);
                    break;
                }
            for (const PendingBlend& pending : pending_blends_)
                if (pending.node == plan_node) {
                    blend_mode = pending.mode;
                    break;
                }
            const auto& state = flow.contributions[contribution_index];
            const std::string subtitle = writes_state ? "State output" : "State consumer";
            const std::string detail =
                !state.reaches_present
                    ? "Detached · no route to Present"
                    : (writes_state ? std::string("State ") + blend_name(blend_mode) + " · " : std::string("State ")) +
                          std::to_string(state.layer_count) + " layer(s) · " + std::to_string(state.input_count) +
                          " input(s)";
            contribution_nodes[contribution_index] = static_cast<int>(nodes_.size());
            nodes_.push_back({std::move(title),
                              subtitle,
                              detail,
                              plan.nodes[plan_node].phase,
                              {state.reaches_present, state.input_count, state.layer_count},
                              {},
                              plan_node,
                              static_cast<int>(contribution_index),
                              writes_state,
                              blend_mode});
        }
        int present = -1;
        for (uint32_t plan_node = 0; plan_node < plan.nodes.size(); ++plan_node) {
            if (plan.nodes[plan_node].phase != aeg::pipeline::EnginePhase::kPresent)
                continue;
            present = static_cast<int>(nodes_.size());
            nodes_.push_back({"Present",
                              "Final state target",
                              "Composited SceneColor",
                              aeg::pipeline::EnginePhase::kPresent,
                              {true, 0, 0},
                              {},
                              plan_node,
                              -1,
                              false});
            break;
        }
        for (std::size_t source = 0; source < flow.contributions.size(); ++source) {
            const int from = contribution_nodes[source];
            if (from < 0 || !flow.contributions[source].reaches_present)
                continue;
            bool has_output = false;
            for (const uint32_t destination : flow.contributions[source].output_contributions) {
                if (destination < contribution_nodes.size() && contribution_nodes[destination] >= 0) {
                    edges_.push_back({from, contribution_nodes[destination], false});
                    has_output = true;
                }
            }
            if ((flow.contributions[source].connects_to_present || !has_output) && present >= 0) {
                edges_.push_back({from, present, false});
            }
        }
        selected_ = nodes_.empty() ? -1 : 0;
        arrange();
        restore_layout(plan);
        fit_ = true;
    }
    void capture_layout_node(const Node& node) {
        if (node.plan_node == aeg::pipeline::kInvalidPipelineHandleIndex)
            return;
        aeg::pipeline::PipelinePlanHandle plan{};
        if (active_source_ == "Built-in / Editor UI")
            plan = host_.active_plan_snapshot();
        else {
            const auto sources = runtime_.pipeline_snapshots();
            const auto found = std::find_if(sources.begin(), sources.end(),
                                            [this](const auto& source) { return source.first == active_source_; });
            if (found != sources.end())
                plan = found->second;
        }
        if (!plan || node.plan_node >= plan->nodes.size())
            return;
        const auto& plan_node = plan->nodes[node.plan_node];
        layouts_.stage({active_source_, {plan_node.stage.value(), plan_node.task.value()}, node.pos.x, node.pos.y});
    }
    void capture_layout() {
        for (const Node& node : nodes_)
            capture_layout_node(node);
        layout_dirty_ = true;
    }
    void restore_layout(const aeg::pipeline::PipelinePlan& plan) {
        for (Node& node : nodes_) {
            if (node.plan_node == aeg::pipeline::kInvalidPipelineHandleIndex || node.plan_node >= plan.nodes.size())
                continue;
            const auto& plan_node = plan.nodes[node.plan_node];
            const auto saved = layouts_.find(active_source_, {plan_node.stage.value(), plan_node.task.value()});
            if (saved)
                node.pos = {saved->x, saved->y};
        }
    }
    void arrange() {
        std::vector<int> rank(nodes_.size(), 0), degree(nodes_.size(), 0), queue;
        for (const auto& edge : edges_)
            ++degree[edge.to];
        for (std::size_t i = 0; i < nodes_.size(); ++i)
            if (!degree[i])
                queue.push_back(static_cast<int>(i));
        for (std::size_t p = 0; p < queue.size(); ++p)
            for (const auto& edge : edges_)
                if (edge.from == queue[p]) {
                    rank[edge.to] = std::max(rank[edge.to], rank[edge.from] + 1);
                    if (--degree[edge.to] == 0)
                        queue.push_back(edge.to);
                }
        std::vector<int> rows(nodes_.size() + 1, 0);
        for (std::size_t i = 0; i < nodes_.size(); ++i)
            nodes_[i].pos = {rank[i] * (width_ + 100), rows[rank[i]]++ * (height_ + 48)};
    }
    void connect(int from, int to) {
        if (from == to) {
            feedback_ = "Cannot connect a node to itself.";
            return;
        }
        if (!details_ || nodes_[from].plan_node == aeg::pipeline::kInvalidPipelineHandleIndex ||
            nodes_[to].plan_node == aeg::pipeline::kInvalidPipelineHandleIndex) {
            feedback_ = "Switch to Details to edit the live pipeline.";
            return;
        }
        const bool source_has_state_input =
            std::any_of(edges_.begin(), edges_.end(), [from](const Edge& edge) { return edge.to == from; });
        if (active_source_ == "Game / Renderer" && (nodes_[from].phase == aeg::pipeline::EnginePhase::kPresent ||
                                                    !aeg::application::can_start_render_state_link(
                                                        nodes_[from].writes_render_state, source_has_state_input))) {
            feedback_ = "A State Link source needs a State output or an incoming State Link.";
            return;
        }
        if (std::any_of(edges_.begin(), edges_.end(),
                        [&](const Edge& edge) { return edge.from == from && edge.to == to; })) {
            feedback_ = "This connection already exists.";
            return;
        }
        std::vector<int> pending{to}, visited(nodes_.size(), 0);
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const int next = pending[i];
            if (next == from) {
                feedback_ = "Connection rejected: it would create a cycle.";
                return;
            }
            if (visited[next]++)
                continue;
            for (const auto& edge : edges_)
                if (edge.from == next)
                    pending.push_back(edge.to);
        }
        drafts_.stage_add(active_source_, nodes_[from].plan_node, nodes_[to].plan_node);
        edges_.push_back({from, to, true});
        feedback_ = "Connection is pending. Save Changes to apply it.";
    }
    void stage_blend(Node& node, uint8_t mode) {
        if (mode > static_cast<uint8_t>(aeg::pipeline::RenderStateBlendMode::kAdd))
            return;
        const auto found =
            std::find_if(pending_blends_.begin(), pending_blends_.end(),
                         [&node](const PendingBlend& pending) { return pending.node == node.plan_node; });
        if (found == pending_blends_.end())
            pending_blends_.push_back({node.plan_node, mode, node.blend_mode});
        else
            found->mode = mode;
        node.blend_mode = mode;
        blend_dirty_ = true;
        feedback_ = "Blend change pending save.";
    }
    [[nodiscard]] Result apply_pending_blends() {
        for (const PendingBlend& pending : pending_blends_) {
            const Result result = runtime_.set_render_state_blend(
                pending.node, static_cast<aeg::pipeline::RenderStateBlendMode>(pending.mode));
            if (!result)
                return result;
        }
        return Result::success();
    }
    void rollback_pending_blends() {
        for (const PendingBlend& pending : pending_blends_) {
            (void)runtime_.set_render_state_blend(
                pending.node, static_cast<aeg::pipeline::RenderStateBlendMode>(pending.previous_mode));
        }
    }
    PipelineScopeHost& host_;
    EditorSceneRuntime& runtime_;
    aeg::application::PipelineOrderingDraftQueue& drafts_;
    aeg::application::PipelineLayoutSaveState& layouts_;
    std::function<Result()> save_changes_;
    std::size_t source_{0};
    bool details_{true}, fit_{true}, latched_{false}, expand_{false}, layout_dirty_{false}, blend_dirty_{false};
    std::string signature_, feedback_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<PendingBlend> pending_blends_{};
    std::string active_source_{};
    int selected_{-1}, connecting_{-1}, dragging_{-1}, blend_popup_node_{-1};
    aeg::application::GraphPanGesture pan_{};
    float width_{250}, height_{90}, zoom_{1};
    ImVec2 offset_{}, last_size_{};
};

class InspectorPanel final : public aeg::editor::EditorPanel {
public:
    explicit InspectorPanel(EditorSceneRuntime& runtime) noexcept : runtime_(runtime) {}

    void build_ui() override {
        ImGuiWindowClass panel_class;
        panel_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::SetNextWindowClass(&panel_class);
        ImGui::Begin("Inspector");
        ImGui::TextUnformatted("Inspector");
        ImGui::Separator();
        if (pipeline_inspector_selection) {
            const auto& selection = *pipeline_inspector_selection;
            ImGui::Spacing();
            ImGui::TextDisabled("Pipeline Stage");
            ImGui::TextUnformatted(selection.title.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Inputs    %llu", static_cast<unsigned long long>(selection.inputs));
            ImGui::TextWrapped("Incoming dependencies in the active execution plan.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Outputs    %llu", static_cast<unsigned long long>(selection.outputs));
            ImGui::TextWrapped("Outgoing dependencies to subsequent stages.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted("Scope");
            ImGui::TextDisabled("Execution");
            ImGui::SameLine();
            ImGui::TextUnformatted(selection.execution.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted("Diagnostics");
            ImGui::TextWrapped("Timing is unavailable until a profiler sample is recorded.");
            ImGui::End();
            return;
        }
        const auto selected_entities = runtime_.selected_entities();
        if (selected_entities.empty()) {
            finish_component_transaction();
            ImGui::TextDisabled("Inspector");
            ImGui::Separator();
            ImGui::TextUnformatted("No entity selected.");
            ImGui::End();
            return;
        }
        ImGui::TextColored(ImVec4{0.45f, 0.90f, 0.76f, 1.0f}, "● Active Selection");
        ImGui::Text("Entities: %llu%s", static_cast<unsigned long long>(selected_entities.size()),
                    selected_entities.size() > 1 ? " (multi-edit)" : "");
        if (selected_entities.size() == 1) {
            if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
                const auto path = runtime_.hierarchy_path(selected_entities.front());
                if (path) {
                    ImGui::Text("Path: %s", path.value.c_str());
                }
                draw_entity_metadata(selected_entities.front());
            }
        }
        ImGui::Separator();

        const auto common_components = runtime_.selected_component_types();
        if (ImGui::CollapsingHeader("Components", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (common_components.empty()) {
                ImGui::TextUnformatted("No common editable components.");
            }
            if (ImGui::Button("Add Component")) {
                ImGui::OpenPopup("InspectorAddComponent");
            }
            if (ImGui::BeginPopup("InspectorAddComponent")) {
                const bool single_selection = selected_entities.size() == 1;
                const auto has_common = [&common_components](ComponentTypeId type) {
                    return std::find(common_components.begin(), common_components.end(), type) !=
                           common_components.end();
                };
                const ComponentTypeId transform_type(aeg::engine::components::kTransformName);
                const ComponentTypeId camera_type(aeg::engine::components::kCameraName);
                const ComponentTypeId mesh_type(aeg::engine::components::kMeshRendererName);
                const ComponentTypeId light_type(aeg::engine::components::kDirectionalLightName);
                if (ImGui::MenuItem("Transform", nullptr, false, single_selection && !has_common(transform_type))) {
                    last_operation_ = runtime_.add_component(selected_entities, transform_type);
                }
                if (ImGui::MenuItem("Camera", nullptr, false, single_selection && !has_common(camera_type))) {
                    last_operation_ = runtime_.add_component(selected_entities, camera_type);
                }
                if (ImGui::MenuItem("MeshRenderer", nullptr, false, single_selection && !has_common(mesh_type))) {
                    last_operation_ = runtime_.add_component(selected_entities, mesh_type);
                }
                if (ImGui::MenuItem("Directional Light", nullptr, false, single_selection && !has_common(light_type))) {
                    last_operation_ = runtime_.add_component(selected_entities, light_type);
                }
                ImGui::EndPopup();
            }
            if (runtime_.component_edit_active() && std::find(common_components.begin(), common_components.end(),
                                                              active_component_) == common_components.end()) {
                finish_component_transaction();
            }
            for (const ComponentTypeId component : common_components) {
                draw_component(component, selected_entities);
            }
        }
        if (!resource_feedback_.empty()) {
            ImGui::TextColored(ImVec4{1.0f, 0.72f, 0.2f, 1.0f}, "%s", resource_feedback_.c_str());
        }
        if (!last_operation_) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", last_operation_.message.c_str());
        }
        ImGui::End();
    }

private:
    [[nodiscard]] const scene::SceneEntityRecord* selected_record(scene::SceneEntityId entity) const noexcept {
        const auto records = runtime_.entities();
        const auto found =
            std::find_if(records.begin(), records.end(), [entity](const auto& record) { return record.id == entity; });
        return found == records.end() ? nullptr : &*found;
    }

    void draw_entity_metadata(scene::SceneEntityId entity) {
        const auto* record = selected_record(entity);
        if (record == nullptr) {
            return;
        }
        if (metadata_entity_ != entity) {
            metadata_entity_ = entity;
            std::fill(name_buffer_.begin(), name_buffer_.end(), '\0');
            std::fill(tag_buffer_.begin(), tag_buffer_.end(), '\0');
            std::memcpy(name_buffer_.data(), record->name.data(),
                        std::min(record->name.size(), name_buffer_.size() - 1u));
            std::memcpy(tag_buffer_.data(), record->tag.data(), std::min(record->tag.size(), tag_buffer_.size() - 1u));
        }
        ImGui::TextDisabled("Entity");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ID: %016llx:%016llx", static_cast<unsigned long long>(entity.high()),
                              static_cast<unsigned long long>(entity.low()));
        if (ImGui::InputText("Name", name_buffer_.data(), name_buffer_.size()) && ImGui::IsItemDeactivatedAfterEdit()) {
            last_operation_ = runtime_.rename_entity(entity, name_buffer_.data());
        }
        if (ImGui::InputText("Tag", tag_buffer_.data(), tag_buffer_.size()) && ImGui::IsItemDeactivatedAfterEdit()) {
            last_operation_ = runtime_.set_entity_tag(entity, tag_buffer_.data());
        }
    }

    [[nodiscard]] static bool is_resource_field(ComponentTypeId component, std::string_view name,
                                                EditorResourceKind& kind) noexcept {
        if (component == ComponentTypeId(aeg::engine::components::kMeshRendererName) && name == "mesh_asset_id") {
            kind = EditorResourceKind::kMesh;
            return true;
        }
        if (component == ComponentTypeId(aeg::engine::components::kMeshRendererName) && name == "material_asset_id") {
            kind = EditorResourceKind::kMaterial;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::string resource_label(EditorResourceKind kind, uint64_t id) const {
        if (id == 0) {
            return "None";
        }
        for (const EditorResourceDescriptor& descriptor : runtime_.resource_descriptors()) {
            if (descriptor.kind == kind && descriptor.id == id) {
                if (!descriptor.valid || descriptor.status != EditorAssetStatus::kImported) {
                    return "Missing (" + std::string(descriptor.name) + ")";
                }
                return std::string(descriptor.name);
            }
        }
        return "Missing (" + std::to_string(id) + ")";
    }

    bool draw_resource_field(ComponentTypeId component, const aeg::reflection::FieldDescriptor& field,
                             void* field_value) {
        EditorResourceKind expected_kind{};
        if (!is_resource_field(component, field.name, expected_kind)) {
            return false;
        }
        auto* id = static_cast<uint64_t*>(field_value);
        ImGui::TextUnformatted(field.name.c_str());
        ImGui::Button(resource_label(expected_kind, *id).c_str(),
                      ImVec2{std::max(40.0f, ImGui::GetContentRegionAvail().x - 55.0f), 0.0f});
        bool changed = false;
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload);
            if (payload != nullptr) {
                if (payload->DataSize != static_cast<int>(sizeof(ResourceDragPayload))) {
                    resource_feedback_ = "Invalid resource drag payload.";
                } else {
                    const auto* resource = static_cast<const ResourceDragPayload*>(payload->Data);
                    const auto actual_kind = static_cast<EditorResourceKind>(resource->kind);
                    if (actual_kind != expected_kind) {
                        resource_feedback_ = "Resource type does not match this field.";
                    } else if (!runtime_.is_resource_valid(actual_kind, resource->id)) {
                        resource_feedback_ = "Resource is missing or invalid.";
                    } else {
                        *id = resource->id;
                        changed = true;
                        resource_feedback_.clear();
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            if (*id != 0) {
                *id = 0;
                changed = true;
            }
        }
        return changed;
    }

    void draw_component(ComponentTypeId component, std::span<const scene::SceneEntityId> selected_entities) {
        const auto descriptor = runtime_.component_descriptor(component);
        if (!descriptor || !descriptor.value.schema_type.is_valid()) {
            return;
        }
        const auto schema = runtime_.reflection_registry().find_type(descriptor.value.schema_type);
        if (!schema) {
            return;
        }
        if (runtime_.component_edit_active() && active_component_ != component) {
            finish_component_transaction();
        }
        std::vector<std::vector<std::byte>> component_values;
        component_values.reserve(selected_entities.size());
        for (const scene::SceneEntityId entity : selected_entities) {
            const void* value = runtime_.component_value(entity, component);
            if (value == nullptr) {
                return;
            }
            component_values.emplace_back(descriptor.value.size);
            std::memcpy(component_values.back().data(), value, descriptor.value.size);
        }
        if (component_values.empty()) {
            return;
        }
        std::vector<std::byte> edited = component_values.front();
        std::vector<const aeg::reflection::FieldDescriptor*> changed_fields;
        bool deactivated = false;
        bool discrete_change = false;
        ImGui::PushID(static_cast<int>(component.value() & 0x7fffffff));
        const bool open = ImGui::TreeNodeEx(schema.value->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            finish_component_transaction();
            last_operation_ = runtime_.remove_component(selected_entities, component);
            ImGui::PopID();
            return;
        }
        if (open) {
            for (const auto& field : schema.value->fields) {
                if (!aeg::reflection::has_flag(field.flags, aeg::reflection::FieldFlags::kEditable) ||
                    aeg::reflection::has_flag(field.flags, aeg::reflection::FieldFlags::kReadOnly)) {
                    continue;
                }
                void* field_value = field.accessor.mutable_access(edited.data());
                const void* first_field_value = field.accessor.const_access(component_values.front().data());
                if (field_value == nullptr || first_field_value == nullptr) {
                    continue;
                }
                bool mixed = false;
                for (std::size_t value_index = 1; value_index < component_values.size(); ++value_index) {
                    const void* other_field_value = field.accessor.const_access(component_values[value_index].data());
                    if (other_field_value == nullptr ||
                        std::memcmp(first_field_value, other_field_value, field.accessor.value_size) != 0) {
                        mixed = true;
                        break;
                    }
                }
                ImGui::PushID(static_cast<int>(field.id.value() & 0x7fffffff));
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed);
                EditorResourceKind resource_kind{};
                bool changed = draw_resource_field(component, field, field_value);
                if (changed) {
                    discrete_change = true;
                } else if (!is_resource_field(component, field.name, resource_kind)) {
                    ImGui::TextUnformatted(field.name.c_str());
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool normalized_color =
                        (component == ComponentTypeId(aeg::engine::components::kMeshRendererName) &&
                         (field.name == "tint" || field.name == "tint_alpha")) ||
                        (component == ComponentTypeId(aeg::engine::components::kDirectionalLightName) &&
                         field.name == "color");
                    switch (field.value_kind) {
                        case aeg::reflection::FieldValueKind::kFloat: {
                            float minimum = -std::numeric_limits<float>::max();
                            float maximum = std::numeric_limits<float>::max();
                            ImGuiSliderFlags flags = ImGuiSliderFlags_None;
                            if (normalized_color || field.name == "intensity") {
                                minimum = 0.0f;
                                maximum = normalized_color ? 1.0f : std::numeric_limits<float>::max();
                                flags = normalized_color ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
                            } else if (field.name == "vertical_fov_radians") {
                                minimum = 0.001f;
                                maximum = aeg::math::kPi - 0.001f;
                                flags = ImGuiSliderFlags_AlwaysClamp;
                            } else if (field.name == "near_plane" || field.name == "far_plane" ||
                                       field.name == "bounds_radius") {
                                minimum = 0.001f;
                                flags = ImGuiSliderFlags_AlwaysClamp;
                            }
                            changed = ImGui::DragFloat("##value", static_cast<float*>(field_value), 0.01f, minimum,
                                                       maximum, "%.4f", flags);
                            break;
                        }
                        case aeg::reflection::FieldValueKind::kUInt32:
                            changed = ImGui::InputScalar("##value", ImGuiDataType_U32, field_value);
                            break;
                        case aeg::reflection::FieldValueKind::kUInt64:
                            changed = ImGui::InputScalar("##value", ImGuiDataType_U64, field_value);
                            break;
                        case aeg::reflection::FieldValueKind::kBool:
                            changed = ImGui::Checkbox("##value", static_cast<bool*>(field_value));
                            break;
                        case aeg::reflection::FieldValueKind::kVec3: {
                            auto* vector = static_cast<aeg::math::Vec3*>(field_value);
                            if (normalized_color) {
                                changed = ImGui::ColorEdit3("##value", &vector->x);
                                if (component == ComponentTypeId(aeg::engine::components::kMeshRendererName) &&
                                    field.name == "tint") {
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip(
                                            "Tint multiplies the sampled material texture; it is not a final-color "
                                            "override.");
                                    }
                                    const bool tint_item_deactivated = ImGui::IsItemDeactivatedAfterEdit();
                                    if (tint_item_deactivated) {
                                        deactivated = true;
                                    }
                                }
                            } else {
                                changed = ImGui::DragFloat3("##value", &vector->x, 0.01f);
                            }
                            break;
                        }
                        case aeg::reflection::FieldValueKind::kQuat: {
                            auto* quaternion = static_cast<aeg::math::Quat*>(field_value);
                            changed = ImGui::DragFloat4("##value", &quaternion->x, 0.01f);
                            break;
                        }
                        default:
                            ImGui::Text("%s: unsupported field kind", field.name.c_str());
                            break;
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    deactivated = true;
                }
                ImGui::PopItemFlag();
                ImGui::PopID();
                if (changed) {
                    changed_fields.push_back(&field);
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        if (changed_fields.empty()) {
            if (deactivated && runtime_.component_edit_active() && active_component_ == component) {
                finish_component_transaction();
            }
            return;
        }
        for (const auto* field : changed_fields) {
            const void* source = field->accessor.const_access(edited.data());
            for (auto& value : component_values) {
                void* destination = field->accessor.mutable_access(value.data());
                if (source != nullptr && destination != nullptr) {
                    std::memcpy(destination, source, field->accessor.value_size);
                }
            }
        }
        std::vector<std::vector<std::byte>> storage;
        std::vector<ComponentUpdateView> updates;
        storage.reserve(component_values.size());
        updates.reserve(component_values.size());
        for (std::size_t index = 0; index < selected_entities.size(); ++index) {
            storage.push_back(std::move(component_values[index]));
            updates.push_back({selected_entities[index], component, storage.back()});
        }
        const Result update_result = runtime_.component_edit_active() ? runtime_.update_component_edit(updates)
                                                                      : runtime_.begin_component_edit(updates);
        if (!update_result) {
            last_operation_ = update_result;
        } else {
            active_component_ = component;
            if (deactivated || discrete_change) {
                finish_component_transaction();
            }
        }
    }

    void finish_component_transaction() {
        if (!runtime_.component_edit_active()) {
            active_component_ = {};
            return;
        }
        last_operation_ = runtime_.commit_component_edit();
        active_component_ = {};
    }

    EditorSceneRuntime& runtime_;
    std::array<char, 256> name_buffer_{};
    std::array<char, 256> tag_buffer_{};
    scene::SceneEntityId metadata_entity_{};
    ComponentTypeId active_component_{};
    std::string resource_feedback_{};
    Result last_operation_{Result::success()};
};

class AssetsPanel final : public aeg::editor::EditorPanel {
public:
    explicit AssetsPanel(EditorSceneRuntime& runtime) noexcept : runtime_(runtime) {}

    void build_ui() override {
        ImGui::Begin("Assets");
        if (ImGui::Button("Refresh")) {
            last_operation_ = runtime_.refresh_assets();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputText("Search", search_buffer_.data(), search_buffer_.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        const char* filters[] = {"All", "Mesh", "Material", "Texture", "Scene", "Other"};
        ImGui::Combo("Type", &filter_, filters, IM_ARRAYSIZE(filters));
        if (runtime_.project_loaded()) {
            ImGui::TextDisabled("Project: %s", std::string(runtime_.project_root().string()).c_str());
        } else {
            ImGui::TextDisabled("No game project is open. Use Project > New Project or Import Project.");
        }
        ImGui::Separator();
        std::vector<EditorResourceDescriptor> descriptors = runtime_.resource_descriptors();
        std::sort(descriptors.begin(), descriptors.end(), [](const auto& left, const auto& right) {
            if (left.kind != right.kind) {
                return static_cast<uint8_t>(left.kind) < static_cast<uint8_t>(right.kind);
            }
            return left.name < right.name;
        });
        uint64_t pending_delete = 0;
        bool open_preview = false;
        bool open_rename = false;
        for (const EditorResourceDescriptor& descriptor : descriptors) {
            const std::string name(descriptor.name);
            if (search_buffer_[0] != '\0' && name.find(search_buffer_.data()) == std::string::npos) {
                continue;
            }
            if (filter_ != 0 && static_cast<int>(descriptor.kind) != filter_kind_value(filter_)) {
                continue;
            }
            const aeg::application::AssetRowId row_id(static_cast<uint32_t>(descriptor.kind), descriptor.id);
            const char* kind = resource_kind_name(descriptor.kind);
            const bool project_asset = !descriptor.path.empty();
            if (ImGui::Selectable(name.c_str(), selected_id_ == descriptor.id && selected_kind_ == descriptor.kind,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_id_ = descriptor.id;
                selected_kind_ = descriptor.kind;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    preview_id_ = descriptor.id;
                    preview_kind_ = descriptor.kind;
                    open_preview = true;
                }
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                const ResourceDragPayload payload{resource_kind_value(descriptor.kind), descriptor.id};
                ImGui::SetDragDropPayload(kResourcePayload, &payload, sizeof(payload));
                ImGui::Text("%s: %s", kind, name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("AssetContext")) {
                selected_id_ = descriptor.id;
                selected_kind_ = descriptor.kind;
                if (ImGui::MenuItem("Inspect Asset")) {
                    preview_id_ = descriptor.id;
                    preview_kind_ = descriptor.kind;
                    open_preview = true;
                }
                if (ImGui::MenuItem("Copy Name"))
                    ImGui::SetClipboardText(name.c_str());
                ImGui::Separator();
                const char* blocked = aeg::application::asset_edit_block_reason(
                    project_asset, runtime_.project_loaded(), runtime_.mode() == EditorSceneRuntime::Mode::kEdit);
                if (blocked) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 280.0f);
                    ImGui::TextDisabled("%s", blocked);
                    ImGui::PopTextWrapPos();
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Reimport", nullptr, false, blocked == nullptr)) {
                    last_operation_ = runtime_.reimport_asset(descriptor.id);
                }
                if (ImGui::MenuItem("Rename", nullptr, false, blocked == nullptr)) {
                    rename_id_ = descriptor.id;
                    std::fill(rename_buffer_.begin(), rename_buffer_.end(), '\0');
                    std::memcpy(rename_buffer_.data(), name.data(), std::min(name.size(), rename_buffer_.size() - 1u));
                    open_rename = true;
                }
                if (ImGui::MenuItem("Delete", nullptr, false, blocked == nullptr)) {
                    pending_delete = descriptor.id;
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s #%llu [%s]", kind, static_cast<unsigned long long>(descriptor.id),
                                editor_asset_status_name(descriptor.status).data());
        }
        if (pending_delete != 0) {
            last_operation_ = runtime_.remove_asset(pending_delete);
            if (selected_id_ == pending_delete) {
                selected_id_ = 0;
            }
            if (preview_id_ == pending_delete) {
                preview_id_ = 0;
            }
        }
        if (selected_id_ != 0) {
            const auto selected = std::find_if(descriptors.begin(), descriptors.end(), [this](const auto& descriptor) {
                return descriptor.id == selected_id_ && descriptor.kind == selected_kind_;
            });
            if (selected != descriptors.end()) {
                ImGui::Separator();
                ImGui::TextUnformatted("Selection");
                ImGui::Text("Name: %s", std::string(selected->name).c_str());
                ImGui::Text("Type: %s", resource_kind_name(selected->kind));
                ImGui::Text("Status: %s", editor_asset_status_name(selected->status).data());
                ImGui::Text("Asset ID: %llu", static_cast<unsigned long long>(selected->id));
                ImGui::TextWrapped("Path: %s",
                                   selected->path.empty() ? "Built-in editor resource" : selected->path.data());
            } else {
                selected_id_ = 0;
            }
        }
        if (open_rename) {
            ImGui::OpenPopup("RenameAssetPopup");
        }
        if (open_preview) {
            ImGui::OpenPopup("AssetPreviewPopup");
        }
        if (ImGui::BeginPopupModal("RenameAssetPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Rename asset");
            ImGui::InputText("Name", rename_buffer_.data(), rename_buffer_.size());
            if (ImGui::Button("Rename", ImVec2{80.0f, 0.0f})) {
                last_operation_ = runtime_.rename_asset(rename_id_, rename_buffer_.data());
                if (last_operation_) {
                    selected_id_ = rename_id_;
                }
                rename_id_ = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2{80.0f, 0.0f})) {
                rename_id_ = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("AssetPreviewPopup")) {
            const auto found = std::find_if(descriptors.begin(), descriptors.end(), [this](const auto& descriptor) {
                return descriptor.id == preview_id_ && descriptor.kind == preview_kind_;
            });
            if (found != descriptors.end()) {
                ImGui::Text("%s: %s", resource_kind_name(found->kind), std::string(found->name).c_str());
                ImGui::Text("Status: %s", editor_asset_status_name(found->status).data());
                ImGui::Text("Asset ID: %llu", static_cast<unsigned long long>(found->id));
                ImGui::TextWrapped("Path: %s", found->path.empty() ? "Built-in editor resource" : found->path.data());
            } else {
                ImGui::TextUnformatted("Asset is no longer present in the database.");
            }
            if (ImGui::Button("Close")) {
                preview_id_ = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (!last_operation_) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", last_operation_.message.c_str());
        }
        ImGui::End();
    }

private:
    [[nodiscard]] static const char* resource_kind_name(EditorResourceKind kind) noexcept {
        switch (kind) {
            case EditorResourceKind::kMesh:
                return "Mesh";
            case EditorResourceKind::kMaterial:
                return "Material";
            case EditorResourceKind::kTexture:
                return "Texture";
            case EditorResourceKind::kScene:
                return "Scene";
            case EditorResourceKind::kUnknown:
                return "Other";
        }
        return "Unknown";
    }

    [[nodiscard]] static int filter_kind_value(int filter) noexcept {
        switch (filter) {
            case 1:
                return static_cast<int>(EditorResourceKind::kMesh);
            case 2:
                return static_cast<int>(EditorResourceKind::kMaterial);
            case 3:
                return static_cast<int>(EditorResourceKind::kTexture);
            case 4:
                return static_cast<int>(EditorResourceKind::kScene);
            case 5:
                return static_cast<int>(EditorResourceKind::kUnknown);
            default:
                return -1;
        }
    }

    EditorSceneRuntime& runtime_;
    std::array<char, 128> search_buffer_{};
    int filter_{0};
    uint64_t selected_id_{0};
    EditorResourceKind selected_kind_{EditorResourceKind::kUnknown};
    EditorResourceKind preview_kind_{EditorResourceKind::kUnknown};
    uint64_t rename_id_{0};
    uint64_t preview_id_{0};
    std::array<char, 128> rename_buffer_{};
    Result last_operation_{Result::success()};
};

class ConsolePanel final : public aeg::editor::EditorPanel {
public:
    explicit ConsolePanel(const uint64_t& frame_count) noexcept : frame_count_(frame_count) {}

    void build_ui() override {
        ImGui::Begin("Console");
        ImGui::TextUnformatted("Runtime diagnostics, logs, and validation output.");
        ImGui::Separator();
        ImGui::Text("Editor frame: %llu", static_cast<unsigned long long>(frame_count_));
        ImGui::TextUnformatted("ImGui is rendered through Engine RHI and RenderGraph.");
        ImGui::End();
    }

private:
    const uint64_t& frame_count_;
};

class EditorPreferenceActions final : public IEditorPreferenceActions {
public:
    enum class Operation : uint8_t {
        kPreview,
        kApply,
        kSave,
        kCancel,
    };

    struct PendingRequest final {
        Operation operation{Operation::kApply};
        std::string font_key{};
        float font_size_pixels{16.0f};
    };

    [[nodiscard]] Result preview(const EditorPreferenceState& state) override {
        return schedule(Operation::kPreview, state.active_font_key, state.font_size_pixels);
    }

    [[nodiscard]] Result apply(const EditorPreferenceState& state) override {
        return schedule(Operation::kApply, state.active_font_key, state.font_size_pixels);
    }

    [[nodiscard]] Result save(const EditorPreferenceState& state) override {
        return schedule(Operation::kSave, state.active_font_key, state.font_size_pixels);
    }

    [[nodiscard]] Result cancel(const EditorPreferenceState& state) override {
        return schedule(Operation::kCancel, state.active_font_key, state.font_size_pixels);
    }

    [[nodiscard]] bool take_pending(PendingRequest& request) {
        if (!pending_) {
            return false;
        }
        request.operation = pending_operation_;
        request.font_key = std::move(pending_font_key_);
        request.font_size_pixels = pending_font_size_pixels_;
        pending_ = false;
        return true;
    }

private:
    [[nodiscard]] Result schedule(Operation operation, std::string_view font_key, float size_pixels) {
        if (font_key.empty()) {
            return aeg::foundation::invalid_argument("Editor preference font key is empty");
        }
        try {
            std::string next_font_key{font_key};
            pending_font_key_ = std::move(next_font_key);
            pending_font_size_pixels_ = size_pixels;
            pending_operation_ = operation;
            pending_ = true;
            return Result::success();
        } catch (const std::bad_alloc&) {
            return aeg::foundation::out_of_memory("Editor preference pending state allocation failed");
        }
    }

    std::string pending_font_key_{};
    float pending_font_size_pixels_{16.0f};
    Operation pending_operation_{Operation::kApply};
    bool pending_{false};
};

class EditorApplication final {
public:
    EditorApplication() : workspace_(imgui_host_) {}
    ~EditorApplication() { cleanup(); }

    EditorApplication(const EditorApplication&) = delete;
    EditorApplication& operator=(const EditorApplication&) = delete;

    [[nodiscard]] bool initialize(const std::filesystem::path& resource_root = {},
                                  const std::filesystem::path& startup_project = {}) {
        resource_root_ = resource_root;
        startup_project_ = startup_project;
        const std::filesystem::path log_path = editor_log_path();
        std::error_code log_directory_error;
        std::filesystem::create_directories(log_path.parent_path(), log_directory_error);
        if (log_directory_error) {
            static_cast<void>(aeg::application::show_error_dialog(
                "Aether Engine Startup Failure",
                "The editor could not create its log directory. Check the user data directory."));
            return false;
        }
        aeg::logging::Config logging_config{};
        logging_config.file_enabled = true;
        logging_config.file_path = log_path;
        const aeg::logging::Status logging_status = aeg::logging::initialize(logging_config);
        if (!logging_status) {
            static_cast<void>(aeg::application::show_error_dialog(
                "Aether Engine Startup Failure",
                "The editor could not initialize its log file. Check the user data directory."));
            return false;
        }
        aeg::application::StartupSplashDesc splash_desc{};
        splash_desc.image_path = resource_root_ / "branding" / "aether-engine-graph-mark.png";
        auto splash_result = aeg::application::show_startup_splash(splash_desc);
        if (splash_result) {
            startup_splash_ = std::move(splash_result.value);
        } else {
            AEG_LOG_WARNING(aeg::logging::Category::kApplication, "Startup splash unavailable [{}]: {}",
                            aeg::foundation::result_code_name(splash_result.result.code), splash_result.result.message);
        }
        auto window_system_result = aeg::application::create_window_system();
        if (!report_result(window_system_result, "create_window_system")) {
            return false;
        }
        window_system_ = std::move(window_system_result.value);

        aeg::platform::WindowDesc window_desc{};
        window_desc.title = "Aether Engine";
        window_desc.extent = {1, 1};
        window_desc.visible = false;
        const auto window_result = window_system_->create_window(window_desc);
        if (!report_result(window_result, "create_editor_window")) {
            return false;
        }
        window_ = window_result.value;
        if (!report_result(configure_window_for_work_area(*window_, 80, 80, false), "configure_editor_window")) {
            return false;
        }

        const DeviceCreateDesc device_desc{
            aeg::rhi::BackendType::kVulkan, "Aether Engine", std::numeric_limits<uint32_t>::max(), true, true,
        };
        auto device_result = aeg::rhi::create_device(device_desc);
        if (!report_result(device_result, "create_device")) {
            return false;
        }
        device_ = std::move(device_result.value);

        scene_runtime_ = std::make_unique<EditorSceneRuntime>();
        const std::filesystem::path shader_package =
            resource_root_.empty() ? std::filesystem::path{}
                                   : resource_root_ / "shaders" / "core" / "opaque_unlit_mesh.aegshader";
        if (!report_result(scene_runtime_->initialize(*device_, shader_package), "initialize_editor_scene_runtime")) {
            return false;
        }
        if (startup_project_.empty()) {
            const std::filesystem::path bundled_demo = resource_root_ / "DemoProject" / "project.aegproject";
            if (std::filesystem::exists(bundled_demo)) {
                startup_project_ = bundled_demo;
            }
        }
        if (!startup_project_.empty() &&
            !report_result(scene_runtime_->import_project(startup_project_), "open_startup_project")) {
            return false;
        }
        if (!startup_project_.empty() &&
            !report_result(restore_pipeline_document(), "restore_startup_pipeline_document")) {
            return false;
        }

        const aeg::platform::WindowSurfaceBinding surface_binding = window_->surface_binding();
        if (!surface_binding.is_valid()) {
            static_cast<void>(report_result(
                aeg::foundation::invalid_state("Editor window has no valid surface binding"), "create_editor_surface"));
            return false;
        }
        SurfaceDesc surface_desc{};
        surface_desc.native_display_handle = surface_binding.display_handle;
        surface_desc.native_window_handle = surface_binding.window_handle;
        surface_desc.name = aeg::rhi::DebugName{"Editor.Surface"};
        const auto surface_result = device_->create_surface(surface_desc);
        if (!report_result(surface_result, "create_surface")) {
            return false;
        }
        surface_ = surface_result.value;

        if (!create_swapchain()) {
            return false;
        }

        frame_context_ = std::make_unique<FrameContext>(*device_);
        render_graph_ = std::make_unique<RenderGraph>(*device_);
        texture_uploader_ = std::make_unique<TextureUploader>(*device_);
        const auto system_font_result = EditorSystemFontCatalog{}.load();
        if (!report_result(system_font_result, "load_system_fonts")) {
            return false;
        }
        font_catalog_ = std::move(system_font_result.value.catalog);
        default_font_key_ = std::move(system_font_result.value.default_font_key);

        aeg::editor::EditorImGuiDesc imgui_desc{};
        imgui_desc.platform_services = &window_system_->platform_services();
        imgui_desc.font_catalog = &font_catalog_;
        imgui_desc.default_font_key = default_font_key_;
        imgui_desc.font_size_pixels = 16.0f;
        if (!report_result(imgui_host_.initialize(imgui_desc), "initialize_editor_imgui")) {
            return false;
        }
        apply_editor_theme();
        if (!report_result(upload_font_texture(imgui_host_, *frame_context_, "Editor.ImGui.FontTexture",
                                               font_texture_desc_, font_texture_),
                           "upload_imgui_font_texture")) {
            return false;
        }
        if (!create_font_sampler()) {
            return false;
        }
        if (!report_result(register_font_texture(font_texture_, font_texture_desc_, font_texture_handle_),
                           "register_imgui_font_texture")) {
            return false;
        }
        if (!report_result(imgui_host_.set_font_texture(font_texture_handle_), "set_imgui_font_texture")) {
            return false;
        }
        if (!initialize_preference_model()) {
            return false;
        }

        imgui_renderer_ = std::make_unique<ImGuiRenderer>(*device_, texture_registry_);
        if (!report_result(imgui_renderer_->initialize(), "initialize_imgui_renderer")) {
            return false;
        }
        if (!report_result(aeg::renderer::register_render_pipeline(registry_), "register_render_pipeline")) {
            return false;
        }
        if (!report_result(aeg::editor::register_imgui_render_feature(registry_, *imgui_renderer_),
                           "register_imgui_render_feature")) {
            return false;
        }

        if (!report_result(frame_host_.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline")),
                           "bind_editor_render_pipeline")) {
            return false;
        }
        orchestrator_ = std::make_unique<aeg::engine::EngineFrameOrchestrator>(registry_, scheduler_, *frame_context_,
                                                                               *render_graph_);
        if (!report_result(orchestrator_->register_pipeline(frame_host_), "register_editor_frame_pipeline")) {
            return false;
        }
        if (!report_result(orchestrator_->initialize(), "initialize_engine_frame_orchestrator")) {
            return false;
        }

        if (!report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"scene_viewport", "Scene Viewport", EditorDockRegion::kCenter,
                                                     true, false, true},
                               std::make_unique<SceneViewportPanel>(*scene_runtime_, viewport_texture_handle_,
                                                                    [this](EditorDocumentAction action) {
                                                                        request_document_action(std::move(action));
                                                                    })),
                           "register_scene_viewport_panel") ||
            !report_result(
                workspace_.register_panel(
                    EditorPanelDescriptor{"pipeline", "Pipeline", EditorDockRegion::kBottom, true, true, true},
                    std::make_unique<PipelinePanel>(frame_host_, *scene_runtime_, pipeline_drafts_, pipeline_layouts_,
                                                    [this] { return save_scene_and_apply_pipeline_drafts(); })),
                "register_pipeline_panel") ||
            !report_result(workspace_.register_panel(EditorPanelDescriptor{"hierarchy", "Hierarchy",
                                                                           EditorDockRegion::kLeft, true, true, true},
                                                     std::make_unique<HierarchyPanel>(*scene_runtime_)),
                           "register_hierarchy_panel") ||
            !report_result(workspace_.register_panel(EditorPanelDescriptor{"inspector", "Inspector",
                                                                           EditorDockRegion::kRight, true, true, true},
                                                     std::make_unique<InspectorPanel>(*scene_runtime_)),
                           "register_inspector_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"assets", "Assets", EditorDockRegion::kBottom, false, true, true},
                               std::make_unique<AssetsPanel>(*scene_runtime_)),
                           "register_assets_panel") ||
            !report_result(
                workspace_.register_panel(
                    EditorPanelDescriptor{"console", "Console", EditorDockRegion::kBottom, false, true, true},
                    std::make_unique<ConsolePanel>(frame_count_)),
                "register_console_panel") ||
            !report_result(workspace_.register_view_action(aeg::editor::EditorWorkspaceActionDescriptor{
                               "preferences", "Preferences",
                               [this]() {
                                   preferences_open_requested_ = true;
                                   return Result::success();
                               },
                               aeg::application::preferences_action_shows_in_view_menu()}),
                           "register_preferences_action")) {
            return false;
        }
        for (auto action : std::vector<aeg::editor::EditorWorkspaceActionDescriptor>{
                 {"play", "Play", [this] { return scene_runtime_->play(); }, false},
                 {"pause", "Pause", [this] { return scene_runtime_->pause(); }, false},
                 {"stop", "Stop", [this] { return scene_runtime_->stop(); }, false},
                 {"refresh_assets", "Refresh Project Assets", [this] { return scene_runtime_->refresh_assets(); },
                  false},
                 {"rebuild_pipeline", "Rebuild Editor Pipeline", [this] { return frame_host_.rebuild(registry_); },
                  false}}) {
            if (!report_result(workspace_.register_view_action(std::move(action)), "register_toolbar_action"))
                return false;
        }
        last_frame_time_ = std::chrono::steady_clock::now();
        if (!report_result(window_->set_visible(true), "show_editor_window") ||
            !report_result(window_->focus(), "focus_editor_window")) {
            return false;
        }
        if (startup_splash_ != nullptr) {
            startup_splash_->close();
            startup_splash_.reset();
        }
        initialized_ = true;
        return true;
    }

    [[nodiscard]] int run(uint32_t frame_limit, bool resize_regression, bool open_preferences_regression,
                          const aeg::application::IConsoleInterruptController& console_interrupt) {
        regression_mode_ = resize_regression || open_preferences_regression;
        resize_regression_ = resize_regression;
        preferences_regression_mode_ = open_preferences_regression;
        preferences_regression_opened_ = false;
        preferences_regression_closed_ = false;
        preferences_regression_reopened_ = false;
        preferences_regression_preview_requested_ = false;
        preferences_regression_preview_completed_ = false;
        preferences_open_requested_ = open_preferences_regression;
        while (!close_requested() && !console_interrupt.interrupted()) {
            if (frame_limit != 0 && frame_count_ >= frame_limit) {
                break;
            }

            Result event_result = Result::success();
            const Result poll_result = window_system_->poll_events([this, &event_result](const WindowEvent& event) {
                if (window_ != nullptr && event.window == window_->id()) {
                    if (event.type == WindowEventType::kWindowResized) {
                        resize_pending_ = true;
                    }
                    if (event.type == WindowEventType::kWindowCloseRequested) {
                        close_confirmation_requested_ = true;
                        return;
                    }
                    if (!main_window_input_enabled_) {
                        return;
                    }
                    if (event_result) {
                        event_result = input_system_.submit_event(event);
                    }
                    if (event_result) {
                        event_result = imgui_host_.submit_event(event);
                    }
                    return;
                }
                if (preferences_window_ != nullptr && event.window == preferences_window_->id()) {
                    if (event.type == WindowEventType::kWindowCloseRequested) {
                        preferences_close_requested_ = true;
                        return;
                    }
                    if (event.type == WindowEventType::kWindowResized) {
                        preferences_resize_pending_ = true;
                    }
                    if (event_result && preferences_imgui_host_.is_initialized()) {
                        event_result = preferences_imgui_host_.submit_event(event);
                    }
                }
            });
            if (!poll_result) {
                notify_runtime_result(poll_result, "poll_editor_events");
                if (fatal_runtime_error_) {
                    return 1;
                }
            }
            if (!event_result) {
                notify_runtime_result(event_result, "submit_editor_event");
                if (fatal_runtime_error_) {
                    return 1;
                }
            }
            if (preferences_close_requested_) {
                const bool closing_regression_preferences =
                    preferences_regression_mode_ && preferences_window_ != nullptr;
                const Result close_result = close_preferences();
                if (!close_result) {
                    notify_runtime_result(close_result, "close_preferences");
                    if (regression_mode_) {
                        return 1;
                    }
                    if (fatal_runtime_error_) {
                        return 1;
                    }
                }
                if (closing_regression_preferences) {
                    if (preferences_window_ != nullptr || !main_window_input_enabled_) {
                        return 1;
                    }
                    preferences_regression_closed_ = true;
                }
            }
            if (preferences_open_requested_) {
                preferences_open_requested_ = false;
                const Result open_result = open_preferences();
                if (!open_result) {
                    notify_runtime_result(open_result, "open_preferences");
                    if (regression_mode_) {
                        return 1;
                    }
                    if (fatal_runtime_error_) {
                        return 1;
                    }
                }
                if (open_result && preferences_regression_mode_) {
                    if (preferences_window_ == nullptr || main_window_input_enabled_) {
                        return 1;
                    }
                    preferences_regression_opened_ = true;
                    if (preferences_regression_closed_) {
                        preferences_regression_reopened_ = true;
                    }
                }
            }
            if (close_requested()) {
                break;
            }

            if (resize_regression_ && (frame_count_ == 10 || frame_count_ == 30 || frame_count_ == 50)) {
                const aeg::platform::WindowExtent extent = make_regression_extent(frame_count_);
                const Result resize_result = window_->set_extent(extent);
                if (!resize_result) {
                    notify_runtime_result(resize_result, "resize_regression_window");
                }
                resize_pending_ = true;
            }

            if (resize_pending_) {
                const Result recreate_result = recreate_swapchain();
                if (!recreate_result) {
                    notify_runtime_result(recreate_result, "recreate_swapchain");
                    if (fatal_runtime_error_) {
                        return 1;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    continue;
                }
            }

            const Result begin_input_result = input_system_.begin_frame();
            if (!begin_input_result) {
                notify_runtime_result(begin_input_result, "begin_input_frame");
                if (fatal_runtime_error_) {
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
            const auto finish_input_frame = [this]() noexcept { input_system_.end_frame(); };

            if (window_->state().minimized) {
                finish_input_frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            float delta_seconds = std::chrono::duration<float>(now - last_frame_time_).count();
            last_frame_time_ = now;
            if (!(delta_seconds > 0.0f) || delta_seconds > 0.25f) {
                delta_seconds = 1.0f / 60.0f;
            }

            if (scene_runtime_ != nullptr) {
                Result scene_frame_result = Result::success();
                if (scene_runtime_->viewport_resize_pending()) {
                    // The previous viewport image is referenced by the editor UI draw packet.
                    // Release its frame-local descriptors before SceneRuntime replaces it.
                    if (imgui_renderer_ != nullptr) {
                        scene_frame_result = imgui_renderer_->release_prepared_frame();
                        if (!scene_frame_result) {
                            notify_runtime_result(scene_frame_result, "release_scene_viewport_frame");
                        }
                    }
                    if (scene_frame_result && render_graph_ != nullptr) {
                        // The editor graph also caches logical imports of SceneColor. Rebuild
                        // its orchestrator after dropping those records so a resized target is
                        // observed as a fresh imported resource with its current RHI state.
                        orchestrator_.reset();
                        render_graph_->reset();
                        try {
                            orchestrator_ = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
                                registry_, scheduler_, *frame_context_, *render_graph_);
                        } catch (const std::bad_alloc&) {
                            scene_frame_result = aeg::foundation::out_of_memory(
                                "Editor frame orchestrator recreation allocation failed");
                        }
                        if (scene_frame_result) {
                            scene_frame_result = orchestrator_->register_pipeline(frame_host_);
                        }
                        if (scene_frame_result) {
                            scene_frame_result = orchestrator_->initialize();
                        }
                        if (!scene_frame_result) {
                            notify_runtime_result(scene_frame_result, "rebuild_editor_frame_graph");
                        }
                    }
                }
                if (scene_frame_result) {
                    scene_frame_result = scene_runtime_->render_frame(frame_count_);
                }
                if (!scene_frame_result) {
                    notify_runtime_result(scene_frame_result, "render_editor_scene");
                    if (fatal_runtime_error_) {
                        finish_input_frame();
                        return 1;
                    }
                } else if (scene_runtime_->viewport_ready()) {
                    const ImGuiTextureBinding binding = scene_runtime_->viewport_binding();
                    if (!viewport_texture_handle_.is_valid()) {
                        const auto register_result = texture_registry_.register_texture(binding);
                        if (!register_result) {
                            notify_runtime_result(register_result.result, "register_scene_viewport_texture");
                            if (fatal_runtime_error_) {
                                finish_input_frame();
                                return 1;
                            }
                        } else {
                            viewport_texture_handle_ = register_result.value;
                        }
                    } else if (const Result update_result =
                                   texture_registry_.update_texture(viewport_texture_handle_, binding);
                               !update_result) {
                        notify_runtime_result(update_result, "update_scene_viewport_texture");
                        if (fatal_runtime_error_) {
                            finish_input_frame();
                            return 1;
                        }
                    }
                }
            }

            const aeg::platform::WindowState window_state = window_->state();
            const EditorImGuiFrameDesc imgui_frame_desc{
                window_->id(), window_state.extent, delta_seconds, 1.0f, 1.0f,
            };
            bool imgui_frame_active = false;
            const Result begin_imgui_result = imgui_host_.begin_frame(imgui_frame_desc);
            if (!begin_imgui_result) {
                notify_runtime_result(begin_imgui_result, "begin_imgui_frame");
                finish_input_frame();
                if (fatal_runtime_error_) {
                    return 1;
                }
                continue;
            }
            imgui_frame_active = true;
            build_editor_menu();
            workspace_.set_preferences_open(preferences_window_ != nullptr || preferences_open_requested_);
            workspace_.set_playing(scene_runtime_->mode() != EditorSceneRuntime::Mode::kEdit);
            const Result build_workspace_result = workspace_.build_ui();
            if (!build_workspace_result) {
                notify_runtime_result(build_workspace_result, "build_editor_workspace");
                static_cast<void>(imgui_host_.end_frame());
                finish_input_frame();
                if (fatal_runtime_error_) {
                    return 1;
                }
                continue;
            }
            if (scene_runtime_ != nullptr && scene_runtime_->mode() != last_scene_mode_) {
                input_system_.reset();
                last_scene_mode_ = scene_runtime_->mode();
            }
            const Result end_imgui_result = imgui_host_.end_frame();
            if (!end_imgui_result) {
                notify_runtime_result(end_imgui_result, "end_imgui_frame");
                finish_input_frame();
                if (fatal_runtime_error_) {
                    return 1;
                }
                continue;
            }
            imgui_frame_active = false;

            ImGuiDrawPacket draw_packet;
            const Result capture_draw_result = imgui_host_.capture_draw_packet(draw_packet);
            if (!capture_draw_result) {
                notify_runtime_result(capture_draw_result, "capture_imgui_draw_packet");
                finish_input_frame();
                if (fatal_runtime_error_) {
                    return 1;
                }
                continue;
            }

            const Result frame_result = run_engine_frame(draw_packet);
            if (imgui_frame_active) {
                static_cast<void>(imgui_host_.end_frame());
            }
            const Result preferences_frame_result = run_preferences_frame(delta_seconds);
            finish_input_frame();
            if (!frame_result) {
                if (frame_result.code == ResultCode::kOutOfDate || frame_result.code == ResultCode::kSurfaceLost) {
                    resize_pending_ = true;
                    continue;
                }
                notify_runtime_result(frame_result, "run_editor_frame");
                if (regression_mode_) {
                    return 1;
                }
                if (fatal_runtime_error_) {
                    return 1;
                }
                continue;
            }
            if (!preferences_frame_result) {
                if (preferences_frame_result.code == ResultCode::kOutOfDate ||
                    preferences_frame_result.code == ResultCode::kSurfaceLost) {
                    preferences_resize_pending_ = true;
                    continue;
                }
                notify_runtime_result(preferences_frame_result, "run_preferences_frame");
                if (regression_mode_) {
                    return 1;
                }
                if (fatal_runtime_error_) {
                    return 1;
                }
                continue;
            }
            ++frame_count_;
            if (preferences_regression_mode_ && !preferences_regression_preview_requested_ && frame_count_ == 1 &&
                preference_model_ != nullptr) {
                std::string preview_font_key;
                for (const aeg::editor::EditorFontDescriptor& font : preference_model_->state().fonts) {
                    if (font.key != preference_model_->state().active_font_key) {
                        preview_font_key = font.key;
                        break;
                    }
                }
                if (preview_font_key.empty()) {
                    return 1;
                }
                if (!preference_model_->select_font(preview_font_key) || !preference_model_->set_font_size(20.0f) ||
                    !preference_actions_.preview(preference_model_->state())) {
                    return 1;
                }
                preferences_regression_preview_requested_ = true;
            }
            if (preferences_regression_mode_ && frame_count_ == 30 && preferences_window_ != nullptr) {
                preferences_close_requested_ = true;
            }
            if (preferences_regression_mode_ && frame_count_ == 40 && preferences_regression_closed_ &&
                !preferences_regression_reopened_ && preferences_window_ == nullptr) {
                preferences_open_requested_ = true;
            }
            if (preferences_regression_mode_ && frame_count_ == 52 && preferences_regression_reopened_ &&
                preferences_window_ != nullptr) {
                preferences_close_requested_ = true;
            }
            if (!process_pending_preference()) {
                return 1;
            }
        }
        if (preferences_regression_mode_ &&
            (!preferences_regression_opened_ || !preferences_regression_closed_ || !preferences_regression_reopened_ ||
             preferences_window_ != nullptr || !main_window_input_enabled_ ||
             !preferences_regression_preview_requested_ || !preferences_regression_preview_completed_ ||
             preference_model_ == nullptr ||
             imgui_host_.active_font_key() != preference_model_->applied_state().active_font_key ||
             imgui_host_.active_font_size_pixels() != preference_model_->applied_state().font_size_pixels)) {
            return 1;
        }
        return 0;
    }

private:
    enum class ProjectDialogMode : uint8_t {
        kNew,
        kImport,
    };

    enum class SceneDialogMode : uint8_t {
        kOpen,
        kSaveAs,
    };

    [[nodiscard]] Result apply_pipeline_drafts() {
        std::vector<aeg::application::PipelineSavedOrdering> persisted_changes;
        persisted_changes.reserve(pipeline_drafts_.changes().size());
        for (const auto& change : pipeline_drafts_.changes()) {
            const auto source = pipeline_node_identity(change.source, change.source_node);
            const auto destination = pipeline_node_identity(change.source, change.destination_node);
            if (!source || !destination) {
                return aeg::foundation::invalid_state("Pipeline graph changed before its edits could be saved");
            }
            persisted_changes.push_back({change.source, *source, *destination, change.add});
        }
        for (const auto& change : pipeline_drafts_.changes()) {
            Result result =
                aeg::foundation::invalid_argument("The selected pipeline does not support saved ordering changes");
            if (change.source == "Built-in / Editor UI") {
                if (!aeg::application::builtin_pipeline_debug_enabled()) {
                    return aeg::foundation::invalid_state(
                        "Built-in / Editor UI Pipeline editing is disabled outside an explicit Debug build");
                }
                result = change.add ? frame_host_.add_runtime_ordering(change.source_node, change.destination_node)
                                    : frame_host_.remove_runtime_ordering(change.source_node, change.destination_node);
            } else if (change.source == "Game / Renderer") {
                result = change.add
                             ? scene_runtime_->add_render_state_link(change.source_node, change.destination_node)
                             : scene_runtime_->remove_render_state_link(change.source_node, change.destination_node);
            } else if (change.source == "Game / World") {
                result =
                    change.add
                        ? scene_runtime_->add_pipeline_runtime_ordering(EditorSceneRuntime::PipelineTarget::kWorld,
                                                                        change.source_node, change.destination_node)
                        : scene_runtime_->remove_pipeline_runtime_ordering(EditorSceneRuntime::PipelineTarget::kWorld,
                                                                           change.source_node, change.destination_node);
            }
            // A graph refresh can make an already-removed edge stale. Treat that
            // delete as complete so it cannot prevent later rewiring from saving.
            if (!result && !(!change.add && result.code == ResultCode::kNotFound))
                return result;
        }
        for (const auto& change : persisted_changes)
            pipeline_saved_orderings_.stage(change);
        pipeline_drafts_.clear();
        return Result::success();
    }

    [[nodiscard]] Result save_scene_and_apply_pipeline_drafts() {
        if (const Result result = scene_runtime_->save_scene(); !result)
            return result;
        if (const Result result = apply_pipeline_drafts(); !result)
            return result;
        return save_pipeline_document();
    }

    [[nodiscard]] Result save_scene_as_and_apply_pipeline_drafts(const std::filesystem::path& path) {
        if (const Result result = scene_runtime_->save_scene_as(path); !result)
            return result;
        if (const Result result = apply_pipeline_drafts(); !result)
            return result;
        return save_pipeline_document();
    }

    [[nodiscard]] std::optional<aeg::pipeline::PipelinePlanHandle> pipeline_snapshot_for_source(
        std::string_view source) const {
        if (source == "Built-in / Editor UI") {
            if (!aeg::application::builtin_pipeline_debug_enabled())
                return std::nullopt;
            return frame_host_.active_plan_snapshot();
        }
        if (scene_runtime_ == nullptr)
            return std::nullopt;
        for (const auto& [name, plan] : scene_runtime_->pipeline_snapshots()) {
            if (name == source)
                return plan;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<aeg::application::PipelineNodeIdentity> pipeline_node_identity(
        std::string_view source, uint32_t node_index) const {
        const auto plan = pipeline_snapshot_for_source(source);
        if (!plan || *plan == nullptr || node_index >= (*plan)->nodes.size())
            return std::nullopt;
        const auto& node = (*plan)->nodes[node_index];
        return aeg::application::PipelineNodeIdentity{node.stage.value(), node.task.value()};
    }

    [[nodiscard]] std::optional<uint32_t> pipeline_node_index(std::string_view source,
                                                              aeg::application::PipelineNodeIdentity identity) const {
        const auto plan = pipeline_snapshot_for_source(source);
        if (!plan || *plan == nullptr)
            return std::nullopt;
        for (uint32_t index = 0; index < (*plan)->nodes.size(); ++index) {
            const auto& node = (*plan)->nodes[index];
            if (node.stage.value() == identity.stage && node.task.value() == identity.task)
                return index;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::filesystem::path pipeline_document_path() const {
        if (scene_runtime_ == nullptr)
            return {};
        std::filesystem::path path = scene_runtime_->scene_path();
        path += ".pipeline";
        return path;
    }

    [[nodiscard]] Result save_pipeline_document() {
        const std::filesystem::path path = pipeline_document_path();
        if (path.empty())
            return aeg::foundation::invalid_state("Pipeline document has no scene path");
        // Blend choices change the state compositor immediately, but they are not
        // durable until this sidecar document is written with the scene.
        pipeline_saved_blends_.clear();
        if (scene_runtime_ != nullptr) {
            for (const auto& blend : scene_runtime_->render_state_blend_overrides()) {
                const auto node = pipeline_node_identity("Game / Renderer", blend.node);
                if (!node)
                    continue;
                pipeline_saved_blends_.stage({"Game / Renderer", *node, static_cast<uint8_t>(blend.blend_mode)});
            }
        }
        const std::filesystem::path temporary = path.string() + ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
                return Result::failure(ResultCode::kBackendError, "Unable to create Pipeline document");
            stream << "AEG_PIPELINE_EDITS 3\n";
            for (const auto& change : pipeline_saved_orderings_.changes()) {
                if (change.source == "Built-in / Editor UI" && !aeg::application::builtin_pipeline_debug_enabled()) {
                    continue;
                }
                stream << "EDGE " << std::quoted(change.source) << ' ' << change.source_node.stage << ' '
                       << change.source_node.task << ' ' << change.destination_node.stage << ' '
                       << change.destination_node.task << ' ' << (change.add ? 1 : 0) << '\n';
            }
            for (const auto& layout : pipeline_layouts_.layouts()) {
                if (layout.source == "Built-in / Editor UI" && !aeg::application::builtin_pipeline_debug_enabled()) {
                    continue;
                }
                stream << "LAYOUT " << std::quoted(layout.source) << ' ' << layout.node.stage << ' ' << layout.node.task
                       << ' ' << layout.x << ' ' << layout.y << '\n';
            }
            for (const auto& blend : pipeline_saved_blends_.blends()) {
                stream << "BLEND " << std::quoted(blend.source) << ' ' << blend.node.stage << ' ' << blend.node.task
                       << ' ' << static_cast<uint32_t>(blend.blend_mode) << '\n';
            }
            stream.flush();
            if (!stream)
                return Result::failure(ResultCode::kBackendError, "Unable to write Pipeline document");
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error)
            return Result::failure(ResultCode::kBackendError, "Unable to replace Pipeline document");
        return Result::success();
    }

    [[nodiscard]] Result restore_pipeline_document() {
        pipeline_saved_orderings_.clear();
        pipeline_layouts_.clear();
        pipeline_saved_blends_.clear();
        const std::filesystem::path path = pipeline_document_path();
        if (path.empty() || !std::filesystem::exists(path))
            return Result::success();
        if (scene_runtime_ != nullptr && scene_runtime_->project_loaded()) {
            if (const Result result = scene_runtime_->create_project_renderer_pipeline(); !result)
                return result;
        }
        std::ifstream stream(path, std::ios::binary);
        std::string signature;
        uint32_t version = 0;
        if (!(stream >> signature >> version) || signature != "AEG_PIPELINE_EDITS" ||
            (version != 1 && version != 2 && version != 3)) {
            return aeg::foundation::invalid_state("Pipeline document format is invalid");
        }
        const auto restore_edge = [this](aeg::application::PipelineSavedOrdering saved) -> Result {
            if (saved.source == "Built-in / Editor UI" && !aeg::application::builtin_pipeline_debug_enabled()) {
                return Result::success();
            }
            const auto source = pipeline_node_index(saved.source, saved.source_node);
            const auto destination = pipeline_node_index(saved.source, saved.destination_node);
            if (!source || !destination)
                return Result::success();
            aeg::application::PipelineOrderingDraft draft{saved.source, *source, *destination, saved.add};
            Result result = aeg::foundation::invalid_argument("The saved Pipeline source is unavailable");
            if (draft.source == "Built-in / Editor UI") {
                result = draft.add ? frame_host_.add_runtime_ordering(draft.source_node, draft.destination_node)
                                   : frame_host_.remove_runtime_ordering(draft.source_node, draft.destination_node);
            } else if (draft.source == "Game / Renderer") {
                result = draft.add
                             ? scene_runtime_->add_render_state_link(draft.source_node, draft.destination_node)
                             : scene_runtime_->remove_render_state_link(draft.source_node, draft.destination_node);
            } else if (draft.source == "Game / World") {
                result =
                    draft.add
                        ? scene_runtime_->add_pipeline_runtime_ordering(EditorSceneRuntime::PipelineTarget::kWorld,
                                                                        draft.source_node, draft.destination_node)
                        : scene_runtime_->remove_pipeline_runtime_ordering(EditorSceneRuntime::PipelineTarget::kWorld,
                                                                           draft.source_node, draft.destination_node);
            }
            if (result || (!draft.add && result.code == ResultCode::kNotFound))
                pipeline_saved_orderings_.stage(saved);
            return result || (!draft.add && result.code == ResultCode::kNotFound) ? Result::success() : result;
        };
        const auto restore_blend = [this](aeg::application::PipelineSavedBlend saved) -> Result {
            if (saved.source != "Game / Renderer" ||
                saved.blend_mode > static_cast<uint8_t>(aeg::pipeline::RenderStateBlendMode::kAdd)) {
                return Result::success();
            }
            const auto node = pipeline_node_index(saved.source, saved.node);
            if (!node)
                return Result::success();
            const Result result = scene_runtime_->set_render_state_blend(
                *node, static_cast<aeg::pipeline::RenderStateBlendMode>(saved.blend_mode));
            if (!result)
                return result;
            pipeline_saved_blends_.stage(std::move(saved));
            return Result::success();
        };
        if (version == 1) {
            aeg::application::PipelineSavedOrdering saved{};
            int add = 0;
            while (stream >> std::quoted(saved.source) >> saved.source_node.stage >> saved.source_node.task >>
                   saved.destination_node.stage >> saved.destination_node.task >> add) {
                saved.add = add != 0;
                if (const Result result = restore_edge(saved); !result)
                    return result;
            }
        } else {
            std::string record;
            while (stream >> record) {
                if (record == "EDGE") {
                    aeg::application::PipelineSavedOrdering saved{};
                    int add = 0;
                    if (!(stream >> std::quoted(saved.source) >> saved.source_node.stage >> saved.source_node.task >>
                          saved.destination_node.stage >> saved.destination_node.task >> add))
                        break;
                    saved.add = add != 0;
                    if (const Result result = restore_edge(saved); !result)
                        return result;
                } else if (record == "LAYOUT") {
                    aeg::application::PipelineNodeLayout layout{};
                    if (!(stream >> std::quoted(layout.source) >> layout.node.stage >> layout.node.task >> layout.x >>
                          layout.y))
                        break;
                    if (!std::isfinite(layout.x) || !std::isfinite(layout.y)) {
                        return aeg::foundation::invalid_state("Pipeline document contains a non-finite node position");
                    }
                    if (layout.source != "Built-in / Editor UI" || aeg::application::builtin_pipeline_debug_enabled()) {
                        pipeline_layouts_.stage(std::move(layout));
                    }
                } else if (record == "BLEND" && version >= 3) {
                    aeg::application::PipelineSavedBlend blend{};
                    uint32_t blend_mode = 0;
                    if (!(stream >> std::quoted(blend.source) >> blend.node.stage >> blend.node.task >> blend_mode))
                        break;
                    if (blend_mode > static_cast<uint32_t>(aeg::pipeline::RenderStateBlendMode::kAdd)) {
                        return aeg::foundation::invalid_state("Pipeline document contains an invalid blend mode");
                    }
                    blend.blend_mode = static_cast<uint8_t>(blend_mode);
                    if (const Result result = restore_blend(std::move(blend)); !result)
                        return result;
                } else {
                    return aeg::foundation::invalid_state("Pipeline document contains an unknown record");
                }
            }
        }
        if (!stream.eof())
            return aeg::foundation::invalid_state("Pipeline document contains an invalid record");
        return Result::success();
    }

    void request_document_action(EditorDocumentAction action) {
        if (scene_runtime_ == nullptr) {
            return;
        }
        if (action.type == EditorDocumentActionType::kOpenScene ||
            action.type == EditorDocumentActionType::kSaveAsScene) {
            scene_dialog_mode_ =
                action.type == EditorDocumentActionType::kOpenScene ? SceneDialogMode::kOpen : SceneDialogMode::kSaveAs;
            const std::string path = action.path.empty() ? scene_runtime_->scene_path().string() : action.path.string();
            std::fill(scene_path_buffer_.begin(), scene_path_buffer_.end(), '\0');
            std::memcpy(scene_path_buffer_.data(), path.data(), std::min(path.size(), scene_path_buffer_.size() - 1u));
            scene_dialog_pending_ = true;
            scene_operation_ = Result::success();
            return;
        }
        request_confirmed_document_action(std::move(action));
    }

    void request_confirmed_document_action(EditorDocumentAction action) {
        if (scene_runtime_ == nullptr) {
            return;
        }
        const bool destructive = action.type == EditorDocumentActionType::kNewScene ||
                                 action.type == EditorDocumentActionType::kOpenScene ||
                                 action.type == EditorDocumentActionType::kNewProject ||
                                 action.type == EditorDocumentActionType::kImportProject ||
                                 action.type == EditorDocumentActionType::kCloseEditor;
        if (destructive && (scene_runtime_->dirty() || pipeline_drafts_.dirty())) {
            pending_document_action_ = std::move(action);
            unsaved_changes_pending_ = true;
            return;
        }
        execute_document_action(std::move(action));
    }

    void execute_document_action(EditorDocumentAction action) {
        if (scene_runtime_ == nullptr) {
            return;
        }
        if ((action.type == EditorDocumentActionType::kOpenScene ||
             action.type == EditorDocumentActionType::kSaveAsScene) &&
            action.path.is_relative() && scene_runtime_->project_loaded()) {
            action.path = scene_runtime_->project_root() / action.path;
        }
        Result result = Result::success();
        switch (action.type) {
            case EditorDocumentActionType::kNewScene:
                result = scene_runtime_->new_scene();
                if (result) {
                    pipeline_saved_orderings_.clear();
                    pipeline_layouts_.clear();
                    pipeline_saved_blends_.clear();
                }
                break;
            case EditorDocumentActionType::kOpenScene:
                result = scene_runtime_->open_scene(action.path);
                if (result)
                    result = restore_pipeline_document();
                break;
            case EditorDocumentActionType::kSaveScene:
                result = save_scene_and_apply_pipeline_drafts();
                break;
            case EditorDocumentActionType::kSaveAsScene:
                result = save_scene_as_and_apply_pipeline_drafts(action.path);
                break;
            case EditorDocumentActionType::kNewProject:
                result = scene_runtime_->new_project(action.path, action.name);
                if (result) {
                    pipeline_saved_orderings_.clear();
                    pipeline_layouts_.clear();
                    pipeline_saved_blends_.clear();
                }
                break;
            case EditorDocumentActionType::kImportProject:
                result = scene_runtime_->import_project(action.path);
                if (result)
                    result = restore_pipeline_document();
                break;
            case EditorDocumentActionType::kCloseEditor:
                close_confirmation_requested_ = false;
                force_shutdown_ = true;
                return;
        }
        if (!result) {
            notify_runtime_result(result, "editor_document_action");
        }
    }

    void build_document_dialogs() {
        if (scene_dialog_pending_) {
            ImGui::OpenPopup("SceneFileDialog");
            scene_dialog_pending_ = false;
        }
        if (ImGui::BeginPopupModal("SceneFileDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool save_as = scene_dialog_mode_ == SceneDialogMode::kSaveAs;
            ImGui::TextUnformatted(save_as ? "Save scene as" : "Open scene");
            ImGui::InputText("Scene file", scene_path_buffer_.data(), scene_path_buffer_.size());
            if (!scene_operation_) {
                ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", scene_operation_.message.c_str());
            }
            if (ImGui::Button(save_as ? "Save" : "Open", ImVec2{90.0f, 0.0f})) {
                EditorDocumentAction action{};
                action.type = save_as ? EditorDocumentActionType::kSaveAsScene : EditorDocumentActionType::kOpenScene;
                action.path = scene_path_buffer_.data();
                scene_operation_ = Result::success();
                ImGui::CloseCurrentPopup();
                request_confirmed_document_action(std::move(action));
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2{90.0f, 0.0f})) {
                scene_operation_ = Result::success();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (close_confirmation_requested_ && !unsaved_changes_pending_) {
            request_confirmed_document_action(EditorDocumentAction{EditorDocumentActionType::kCloseEditor});
        }
        if (unsaved_changes_pending_) {
            ImGui::OpenPopup("UnsavedChanges");
            unsaved_changes_pending_ = false;
        }
        if (ImGui::BeginPopupModal("UnsavedChanges", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("The current scene has unsaved changes.");
            ImGui::TextUnformatted("Save before continuing?");
            if (!unsaved_operation_) {
                ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", unsaved_operation_.message.c_str());
            }
            if (ImGui::Button("Save", ImVec2{90.0f, 0.0f})) {
                unsaved_operation_ = save_scene_and_apply_pipeline_drafts();
                if (unsaved_operation_) {
                    EditorDocumentAction next = std::move(pending_document_action_);
                    ImGui::CloseCurrentPopup();
                    execute_document_action(std::move(next));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2{90.0f, 0.0f})) {
                EditorDocumentAction next = std::move(pending_document_action_);
                unsaved_operation_ = Result::success();
                ImGui::CloseCurrentPopup();
                execute_document_action(std::move(next));
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2{90.0f, 0.0f})) {
                if (pending_document_action_.type == EditorDocumentActionType::kCloseEditor && window_ != nullptr) {
                    static_cast<void>(window_->clear_close_request());
                    close_confirmation_requested_ = false;
                }
                pending_document_action_ = {};
                unsaved_operation_ = Result::success();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    /** @brief Handles Edit history shortcuts and exposes their current availability in the menu bar. */
    void build_editor_menu() {
        if (scene_runtime_ == nullptr) {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        const auto report_history_result = [this](const Result& result, std::string_view operation) {
            if (!result && result.code != ResultCode::kNotFound) {
                notify_runtime_result(result, operation);
            }
        };
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift) {
                const Result result = scene_runtime_->redo();
                report_history_result(result, "redo_editor_shortcut");
            } else {
                const Result result = scene_runtime_->undo();
                report_history_result(result, "undo_editor_shortcut");
            }
        } else if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            const Result result = scene_runtime_->redo();
            report_history_result(result, "redo_editor_shortcut");
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10, 10});
        if (!ImGui::BeginMainMenuBar()) {
            ImGui::PopStyleVar();
            return;
        }
        ImGui::TextUnformatted("AEG");
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, scene_runtime_->can_undo())) {
                const Result result = scene_runtime_->undo();
                report_history_result(result, "undo_editor_menu");
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y / Ctrl+Shift+Z", false, scene_runtime_->can_redo())) {
                const Result result = scene_runtime_->redo();
                report_history_result(result, "redo_editor_menu");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Project")) {
            if (ImGui::MenuItem("New Project...")) {
                project_dialog_mode_ = ProjectDialogMode::kNew;
                project_dialog_pending_ = true;
                std::fill(project_path_buffer_.begin(), project_path_buffer_.end(), '\0');
                std::fill(project_name_buffer_.begin(), project_name_buffer_.end(), '\0');
                const std::string default_path =
                    (std::filesystem::current_path().parent_path() / "AEG Game Project").string();
                const std::string default_name = "Game Project";
                std::memcpy(project_path_buffer_.data(), default_path.data(),
                            std::min(default_path.size(), project_path_buffer_.size() - 1u));
                std::memcpy(project_name_buffer_.data(), default_name.data(),
                            std::min(default_name.size(), project_name_buffer_.size() - 1u));
                project_operation_ = Result::success();
            }
            if (ImGui::MenuItem("Import Project...")) {
                project_dialog_mode_ = ProjectDialogMode::kImport;
                project_dialog_pending_ = true;
                std::fill(project_path_buffer_.begin(), project_path_buffer_.end(), '\0');
                const std::string default_path = std::filesystem::current_path().string();
                std::memcpy(project_path_buffer_.data(), default_path.data(),
                            std::min(default_path.size(), project_path_buffer_.size() - 1u));
                project_operation_ = Result::success();
            }
            ImGui::EndMenu();
        }
        ImGui::TextDisabled("/");
        ImGui::TextUnformatted(scene_runtime_->project_loaded() ? std::string(scene_runtime_->project_name()).c_str()
                                                                : "Untitled Project");
        ImGui::EndMainMenuBar();
        ImGui::PopStyleVar();

        if (project_dialog_pending_) {
            ImGui::OpenPopup("ProjectDialog");
            project_dialog_pending_ = false;
        }
        if (ImGui::BeginPopupModal("ProjectDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool creating = project_dialog_mode_ == ProjectDialogMode::kNew;
            ImGui::TextUnformatted(creating ? "Create a game project" : "Import a game project");
            static_cast<void>(aeg::application::draw_project_dialog_field(
                "Project directory", "##ProjectDirectory", project_path_buffer_.data(), project_path_buffer_.size()));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##ProjectDirectory")) {
                const auto selection = aeg::application::select_directory(
                    window_ != nullptr ? window_->surface_binding() : aeg::platform::WindowSurfaceBinding{},
                    creating ? "Select the new project directory" : "Select a game project directory",
                    project_path_buffer_.data());
                if (selection) {
                    std::fill(project_path_buffer_.begin(), project_path_buffer_.end(), '\0');
                    std::memcpy(project_path_buffer_.data(), selection.value.data(),
                                std::min(selection.value.size(), project_path_buffer_.size() - 1u));
                    project_operation_ = Result::success();
                } else if (selection.result.code != ResultCode::kCancelled) {
                    project_operation_ = selection.result;
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Choose a project folder");
            }
            if (creating) {
                static_cast<void>(aeg::application::draw_project_dialog_field(
                    "Project name", "##ProjectName", project_name_buffer_.data(), project_name_buffer_.size()));
            }
            if (!project_operation_) {
                ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", project_operation_.message.c_str());
            }
            if (ImGui::Button(creating ? "Create" : "Import", ImVec2{90.0f, 0.0f})) {
                EditorDocumentAction action{};
                action.type =
                    creating ? EditorDocumentActionType::kNewProject : EditorDocumentActionType::kImportProject;
                action.path = project_path_buffer_.data();
                action.name = project_name_buffer_.data();
                project_operation_ = Result::success();
                ImGui::CloseCurrentPopup();
                request_confirmed_document_action(std::move(action));
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2{90.0f, 0.0f})) {
                project_operation_ = Result::success();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        build_document_dialogs();
    }

    /** @brief Routes one runtime failure to the editor notification or fatal fallback. */
    void notify_runtime_result(const Result& result, std::string_view operation) noexcept {
        if (result) {
            return;
        }
        AEG_LOG_ERROR(aeg::logging::Category::kApplication, "Editor operation {} failed [{}]: {}", operation,
                      aeg::foundation::result_code_name(result.code), result.message);
        // Automated editor runs must fail deterministically instead of waiting
        // behind a native modal error dialog.
        if (regression_mode_) {
            fatal_runtime_error_ = true;
            return;
        }
        const EditorErrorSeverity severity =
            is_fatal_runtime_error(result.code) ? EditorErrorSeverity::kFatal : EditorErrorSeverity::kRecoverable;
        if (severity == EditorErrorSeverity::kFatal) {
            static_cast<void>(report_result(result, operation));
            fatal_runtime_error_ = true;
            return;
        }
        const Result notification_result = error_state_.report(operation, result, severity);
        if (!notification_result) {
            static_cast<void>(report_result(notification_result, "report_editor_error"));
            fatal_runtime_error_ = true;
            return;
        }
        if (!error_state_.consume_notification_request()) {
            return;
        }
        const EditorErrorNotice* notice = error_state_.pending();
        if (notice == nullptr) {
            return;
        }
        try {
            std::string message;
            message.reserve(notice->operation.size() + notice->message.size() + 96u);
            message.append("The editor could not complete the requested operation.");
            if (notice->occurrence_count > 1) {
                message.append("\nOccurrences: ");
                message.append(std::to_string(notice->occurrence_count));
            }
            static_cast<void>(aeg::application::show_error_dialog("Aether Engine Error", message));
        } catch (...) {
            static_cast<void>(aeg::application::show_error_dialog(
                "Aether Engine Error", "The editor could not display a runtime error notification."));
        }
    }

    /** @brief Identifies runtime failures that cannot be recovered by another frame. */
    [[nodiscard]] static bool is_fatal_runtime_error(ResultCode code) noexcept {
        return code == ResultCode::kDeviceLost || code == ResultCode::kOutOfMemory;
    }

    /** @brief Sizes and places one hidden window relative to its nearest display work area. */
    [[nodiscard]] Result configure_window_for_work_area(IWindow& window, uint32_t width_percent,
                                                        uint32_t height_percent, bool show_and_focus) {
        if (width_percent == 0 || width_percent > 100 || height_percent == 0 || height_percent > 100) {
            return aeg::foundation::invalid_argument("Window work-area scale must be between 1 and 100 percent");
        }
        const auto work_area_result = window_system_->platform_services().query_display_work_area(window.id());
        if (!work_area_result) {
            return work_area_result.result;
        }
        const DisplayWorkArea work_area = work_area_result.value;
        if (window_ != nullptr && window_.get() == &window) {
            work_area_ = work_area;
        }
        const auto scaled_extent = [](uint32_t value, uint32_t percent) noexcept {
            const uint64_t scaled = static_cast<uint64_t>(value) * percent / 100u;
            return static_cast<uint32_t>(std::max<uint64_t>(scaled, 1u));
        };
        const aeg::platform::WindowExtent extent{scaled_extent(work_area.extent.width, width_percent),
                                                 scaled_extent(work_area.extent.height, height_percent)};
        const aeg::platform::WindowPosition position{
            work_area.position.x + static_cast<int32_t>((work_area.extent.width - extent.width) / 2u),
            work_area.position.y + static_cast<int32_t>((work_area.extent.height - extent.height) / 2u)};
        if (const Result result = window.set_extent(extent); !result) {
            return result;
        }
        if (const Result result = window.set_position(position); !result) {
            return result;
        }
        if (!show_and_focus) {
            return Result::success();
        }
        if (const Result result = window.set_visible(true); !result) {
            return result;
        }
        return window.focus();
    }

    /** @brief Returns a display-relative extent used by the resize regression sequence. */
    [[nodiscard]] aeg::platform::WindowExtent make_regression_extent(uint64_t frame) const noexcept {
        uint32_t width_percent = 80;
        uint32_t height_percent = 80;
        if (frame == 10) {
            width_percent = 90;
            height_percent = 88;
        } else if (frame == 30) {
            width_percent = 52;
            height_percent = 54;
        } else if (frame == 50) {
            width_percent = 76;
            height_percent = 74;
        }
        const auto scaled_extent = [](uint32_t value, uint32_t percent) noexcept {
            const uint64_t scaled = static_cast<uint64_t>(value) * percent / 100u;
            return static_cast<uint32_t>(std::max<uint64_t>(scaled, 1u));
        };
        return {scaled_extent(work_area_.extent.width, width_percent),
                scaled_extent(work_area_.extent.height, height_percent)};
    }

    /** @brief Opens the Preferences document in an independent Engine-rendered native window. */
    [[nodiscard]] Result open_preferences() {
        if (preferences_window_ != nullptr) {
            return preferences_window_->focus();
        }
        if (window_system_ == nullptr || device_ == nullptr || preference_model_ == nullptr ||
            !font_texture_handle_.is_valid()) {
            return aeg::foundation::invalid_state("Preferences requires initialized editor resources");
        }

        aeg::platform::WindowDesc window_desc{};
        window_desc.title = "Aether Engine Preferences";
        window_desc.extent = {1, 1};
        window_desc.visible = false;
        const auto window_result = window_system_->create_window(window_desc);
        if (!window_result) {
            return window_result.result;
        }
        preferences_window_ = window_result.value;

        const auto preference_window_scale = aeg::application::preference_window_work_area_scale();
        Result result = configure_window_for_work_area(*preferences_window_, preference_window_scale.width_percent,
                                                       preference_window_scale.height_percent, false);
        if (!result) {
            static_cast<void>(close_preferences());
            return result;
        }
        const aeg::platform::WindowSurfaceBinding surface_binding = preferences_window_->surface_binding();
        if (!surface_binding.is_valid()) {
            static_cast<void>(close_preferences());
            return aeg::foundation::invalid_state("Preferences window has no valid surface binding");
        }
        SurfaceDesc surface_desc{};
        surface_desc.native_display_handle = surface_binding.display_handle;
        surface_desc.native_window_handle = surface_binding.window_handle;
        surface_desc.name = aeg::rhi::DebugName{"Editor.Preferences.Surface"};
        const auto surface_result = device_->create_surface(surface_desc);
        if (!surface_result) {
            static_cast<void>(close_preferences());
            return surface_result.result;
        }
        preferences_surface_ = surface_result.value;

        const aeg::platform::WindowState window_state = preferences_window_->state();
        SwapchainDesc swapchain_desc{};
        swapchain_desc.surface = preferences_surface_;
        swapchain_desc.extent = {window_state.extent.width, window_state.extent.height};
        swapchain_desc.format = Format::kUnknown;
        swapchain_desc.present_mode = PresentMode::kVsync;
        swapchain_desc.min_image_count = 2;
        swapchain_desc.frames_in_flight = 2;
        swapchain_desc.image_capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        swapchain_desc.name = aeg::rhi::DebugName{"Editor.Preferences.Swapchain"};
        const auto swapchain_result = device_->create_swapchain(swapchain_desc);
        if (!swapchain_result) {
            static_cast<void>(close_preferences());
            return swapchain_result.result;
        }
        preferences_swapchain_ = swapchain_result.value;
        const auto info_result = device_->get_swapchain_info(preferences_swapchain_);
        if (!info_result) {
            static_cast<void>(close_preferences());
            return info_result.result;
        }
        preferences_swapchain_info_ = info_result.value;
        if (preferences_swapchain_info_.format == Format::kUnknown || preferences_swapchain_info_.extent.width == 0 ||
            preferences_swapchain_info_.extent.height == 0) {
            static_cast<void>(close_preferences());
            return aeg::foundation::invalid_state("Preferences swapchain information is invalid");
        }

        try {
            preferences_frame_context_ = std::make_unique<FrameContext>(*device_);
            preferences_render_graph_ = std::make_unique<RenderGraph>(*device_);
            preferences_imgui_renderer_ = std::make_unique<ImGuiRenderer>(*device_, texture_registry_);
        } catch (const std::bad_alloc&) {
            static_cast<void>(close_preferences());
            return aeg::foundation::out_of_memory("Preferences rendering allocation failed");
        }

        aeg::editor::EditorImGuiDesc imgui_desc{};
        imgui_desc.platform_services = &window_system_->platform_services();
        imgui_desc.font_catalog = &font_catalog_;
        imgui_desc.default_font_key = imgui_host_.active_font_key();
        imgui_desc.font_size_pixels = imgui_host_.active_font_size_pixels();
        imgui_desc.enable_preference_fonts = true;
        if (!(result = preferences_imgui_host_.initialize(imgui_desc))) {
            static_cast<void>(close_preferences());
            return result;
        }
        if (!(result = preferences_imgui_host_.set_preview_font(preference_model_->state().active_font_key,
                                                                preference_model_->state().font_size_pixels))) {
            static_cast<void>(close_preferences());
            return result;
        }
        if (!(result = initialize_preferences_font_texture())) {
            static_cast<void>(close_preferences());
            return result;
        }
        const auto panel_result = preferences_imgui_host_.register_panel(
            std::make_unique<EditorPreferencePanel>(*preference_model_, preference_actions_, &preferences_imgui_host_));
        if (!panel_result) {
            static_cast<void>(close_preferences());
            return panel_result.result;
        }
        if (!(result = preferences_imgui_renderer_->initialize())) {
            static_cast<void>(close_preferences());
            return result;
        }
        if (!(result = aeg::renderer::register_render_pipeline(preferences_registry_))) {
            static_cast<void>(close_preferences());
            return result;
        }
        if (!(result =
                  aeg::editor::register_imgui_render_feature(preferences_registry_, *preferences_imgui_renderer_))) {
            static_cast<void>(close_preferences());
            return result;
        }
        if (!(result = preferences_frame_host_.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline")))) {
            static_cast<void>(close_preferences());
            return result;
        }
        preferences_orchestrator_ = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
            preferences_registry_, preferences_scheduler_, *preferences_frame_context_, *preferences_render_graph_);
        if (!(result = preferences_orchestrator_->register_pipeline(preferences_frame_host_))) {
            static_cast<void>(close_preferences());
            return result;
        }
        if (!(result = preferences_orchestrator_->initialize())) {
            static_cast<void>(close_preferences());
            return result;
        }
        if (!(result = window_->set_input_enabled(false))) {
            static_cast<void>(close_preferences());
            return result;
        }
        main_window_input_enabled_ = false;
        if (!(result = preferences_window_->set_visible(true))) {
            static_cast<void>(close_preferences());
            return result;
        }
        return preferences_window_->focus();
    }

    /** @brief Closes Preferences after GPU work and restores the main editor input gate. */
    [[nodiscard]] Result close_preferences() noexcept {
        if (preference_model_ != nullptr) {
            preference_model_->discard();
        }
        Result first_result = Result::success();
        const auto keep_first_error = [&first_result](const Result& result) noexcept {
            if (!result && first_result) {
                first_result = result;
            }
        };
        if (preferences_orchestrator_ != nullptr) {
            keep_first_error(preferences_orchestrator_->shutdown());
            preferences_orchestrator_.reset();
        }
        keep_first_error(aeg::editor::unregister_imgui_render_feature(preferences_registry_));
        if (preferences_imgui_host_.is_initialized()) {
            keep_first_error(preferences_imgui_host_.shutdown());
        }
        preferences_render_graph_.reset();
        preferences_frame_context_.reset();
        if (device_ != nullptr) {
            keep_first_error(device_->wait_idle());
            if (preferences_swapchain_.is_valid()) {
                keep_first_error(device_->destroy_swapchain(preferences_swapchain_));
                preferences_swapchain_ = {};
            }
            if (preferences_surface_.is_valid()) {
                keep_first_error(device_->destroy_surface(preferences_surface_));
                preferences_surface_ = {};
            }
        }
        if (preferences_imgui_renderer_ != nullptr) {
            keep_first_error(preferences_imgui_renderer_->shutdown());
            preferences_imgui_renderer_.reset();
        }
        if (preferences_font_texture_handle_.is_valid()) {
            const Result unregister_result = texture_registry_.unregister_texture(preferences_font_texture_handle_);
            if (!unregister_result && unregister_result.code != ResultCode::kNotFound) {
                keep_first_error(unregister_result);
            } else {
                preferences_font_texture_handle_ = {};
            }
        }
        if (texture_uploader_ != nullptr && preferences_font_texture_.is_valid()) {
            keep_first_error(texture_uploader_->destroy_texture(preferences_font_texture_));
        }
        preferences_font_texture_desc_ = {};
        if (window_system_ != nullptr && preferences_window_ != nullptr) {
            keep_first_error(window_system_->destroy_window(preferences_window_->id()));
            preferences_window_.reset();
        }
        preferences_swapchain_info_ = {};
        preferences_resize_pending_ = false;
        preferences_close_requested_ = false;
        if (!main_window_input_enabled_ && window_ != nullptr) {
            keep_first_error(window_->set_input_enabled(true));
            main_window_input_enabled_ = true;
            const Result focus_result = window_->focus();
            // Windows can reject a foreground transfer when another process owns the input queue.
            // Input is already restored above; let the normal event loop recover focus later.
            if (!focus_result && focus_result.code != ResultCode::kBackendError) {
                keep_first_error(focus_result);
            }
        }
        return first_result;
    }

    /** @brief Recreates the Preferences swapchain after a native resize. */
    [[nodiscard]] Result recreate_preferences_swapchain() {
        if (preferences_window_ == nullptr || !preferences_swapchain_.is_valid()) {
            return aeg::foundation::invalid_state("Preferences swapchain is not initialized");
        }
        if (const Result result = device_->wait_idle(); !result) {
            return result;
        }
        const aeg::platform::WindowState state = preferences_window_->state();
        if (state.minimized || state.extent.width == 0 || state.extent.height == 0) {
            preferences_resize_pending_ = false;
            return Result::success();
        }
        SwapchainDesc desc{};
        desc.surface = preferences_surface_;
        desc.extent = {state.extent.width, state.extent.height};
        desc.present_mode = PresentMode::kVsync;
        desc.min_image_count = 2;
        desc.frames_in_flight = 2;
        desc.image_capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        if (const Result result = device_->recreate_swapchain(preferences_swapchain_, desc); !result) {
            return result;
        }
        const auto info_result = device_->get_swapchain_info(preferences_swapchain_);
        if (!info_result) {
            return info_result.result;
        }
        preferences_swapchain_info_ = info_result.value;
        preferences_render_graph_->reset();
        preferences_resize_pending_ = false;
        return Result::success();
    }

    /** @brief Renders one Preferences frame through the Engine-owned RenderGraph. */
    [[nodiscard]] Result run_preferences_frame(float delta_seconds) {
        if (preferences_window_ == nullptr || preferences_orchestrator_ == nullptr ||
            preferences_imgui_renderer_ == nullptr) {
            return Result::success();
        }
        if (preferences_resize_pending_) {
            if (const Result result = recreate_preferences_swapchain(); !result) {
                return result;
            }
        }
        if (preferences_window_->state().minimized) {
            return Result::success();
        }

        const aeg::platform::WindowState window_state = preferences_window_->state();
        const EditorImGuiFrameDesc frame_desc{preferences_window_->id(), window_state.extent, delta_seconds, 1.0f,
                                              1.0f};
        bool imgui_frame_active = false;
        if (const Result result = preferences_imgui_host_.begin_frame(frame_desc); !result) {
            return result;
        }
        imgui_frame_active = true;
        if (const Result result = preferences_imgui_host_.build_panels(); !result) {
            static_cast<void>(preferences_imgui_host_.end_frame());
            return result;
        }
        if (const Result result = preferences_imgui_host_.end_frame(); !result) {
            return result;
        }
        imgui_frame_active = false;

        ImGuiDrawPacket draw_packet;
        if (const Result result = preferences_imgui_host_.capture_draw_packet(draw_packet); !result) {
            return result;
        }
        const ImGuiRenderTargetDesc target{aeg::engine::kSwapchainImageResourceId, preferences_swapchain_info_.format,
                                           preferences_swapchain_info_.extent,
                                           regression_mode_ ? ResourceUsage::kCopySource : ResourceUsage::kPresent};
        aeg::engine::EngineFrameDesc engine_frame_desc{};
        engine_frame_desc.fixed_tick = frame_count_;
        engine_frame_desc.acquire_swapchain_image = true;
        engine_frame_desc.swapchain = preferences_swapchain_;
        engine_frame_desc.swapchain_image_desc =
            make_swapchain_image_desc_for(preferences_swapchain_info_, "Editor.Preferences.SwapchainImage");
        engine_frame_desc.present_acquired_frame = true;
        FrameAllocation readback_allocation{};
        bool has_readback = false;
        engine_frame_desc.render_feature_preparation = [this, &draw_packet, target, &readback_allocation,
                                                        &has_readback](
                                                           const aeg::engine::EngineFramePreparationContext& context) {
            if (context.acquired_frame == nullptr || !context.swapchain_resource.is_valid()) {
                return aeg::foundation::invalid_state("Preferences requires an acquired swapchain image");
            }
            const auto clear_result = context.render_graph.add_pass(
                "Editor.Preferences.ClearSwapchain", QueueClass::kGraphics,
                [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                    return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kClear, aeg::rhi::StoreOp::kStore,
                                                        ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f},
                                                        ResourceUsage::kColorAttachmentWrite);
                },
                [](aeg::render_graph::PassExecutionContext& pass) {
                    if (const Result result = pass.begin_rendering(); !result) {
                        return result;
                    }
                    return pass.end_rendering();
                });
            if (!clear_result) {
                return clear_result.result;
            }
            if (const Result result = preferences_imgui_renderer_->prepare_frame(
                    context.frame_context, context.render_graph, draw_packet, target);
                !result) {
                return result;
            }
            if (!regression_mode_) {
                return Result::success();
            }

            const uint64_t pixel_count = static_cast<uint64_t>(target.extent.width) * target.extent.height;
            const auto allocation_result = context.frame_context.allocate_readback(pixel_count * 4u, 256);
            if (!allocation_result) {
                return allocation_result.result;
            }
            readback_allocation = allocation_result.value;
            has_readback = true;

            aeg::rhi::BufferDesc readback_desc{};
            readback_desc.size = readback_allocation.buffer_size;
            readback_desc.capabilities = aeg::rhi::BufferCapability::kTransferSource |
                                         aeg::rhi::BufferCapability::kTransferDestination |
                                         aeg::rhi::BufferCapability::kVertex | aeg::rhi::BufferCapability::kIndex |
                                         aeg::rhi::BufferCapability::kUniform;
            readback_desc.memory = aeg::rhi::MemoryClass::kReadback;
            readback_desc.initial_usage = ResourceUsage::kHostRead;
            readback_desc.initial_stages = PipelineStage::kHost;
            readback_desc.initial_queue = QueueClass::kCopy;
            readback_desc.name = aeg::rhi::DebugName{"Editor.Preferences.FramebufferReadback"};
            const auto readback_resource_result =
                context.render_graph.import_buffer(readback_allocation.buffer, readback_desc,
                                                   ResourceState{ResourceUsage::kHostRead, aeg::rhi::ShaderStage::kAll,
                                                                 QueueClass::kCopy, false, PipelineStage::kHost});
            if (!readback_resource_result) {
                return readback_resource_result.result;
            }
            const auto readback_pass_result = context.render_graph.add_pass(
                "Editor.Preferences.FramebufferReadback", QueueClass::kCopy,
                [swapchain_resource = context.swapchain_resource, readback_resource = readback_resource_result.value,
                 readback_allocation, pixel_count](aeg::render_graph::PassBuilder& builder) {
                    if (const Result result =
                            builder.read_to(swapchain_resource, ResourceUsage::kCopySource, PipelineStage::kCopy,
                                            aeg::rhi::ShaderStage::kAll, ResourceUsage::kPresent);
                        !result) {
                        return result;
                    }
                    if (const Result result = builder.write_to(
                            readback_resource, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                            aeg::rhi::ShaderStage::kAll, ResourceUsage::kHostRead,
                            aeg::render_graph::RenderGraphBufferRange{readback_allocation.offset, pixel_count * 4u},
                            true);
                        !result) {
                        return result;
                    }
                    builder.set_side_effects();
                    return Result::success();
                },
                [swapchain_resource = context.swapchain_resource, readback_resource = readback_resource_result.value,
                 readback_allocation, target](aeg::render_graph::PassExecutionContext& pass) {
                    return pass.copy_texture_to_buffer(
                        swapchain_resource, aeg::rhi::TextureSubresourceLayers{aeg::rhi::Aspect::kColor, 0, 0, 1},
                        aeg::rhi::Offset3D{0, 0, 0}, aeg::rhi::Extent3D{target.extent.width, target.extent.height, 1},
                        readback_resource, readback_allocation.offset, 0, 0);
                });
            if (!readback_pass_result) {
                return readback_pass_result.result;
            }
            const auto present_pass_result = context.render_graph.add_pass(
                "Editor.Preferences.PresentSwapchain", QueueClass::kGraphics,
                [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                    return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kLoad, aeg::rhi::StoreOp::kStore,
                                                        ClearColorValue{}, ResourceUsage::kPresent);
                },
                [](aeg::render_graph::PassExecutionContext& pass) {
                    if (const Result result = pass.begin_rendering(); !result) {
                        return result;
                    }
                    return pass.end_rendering();
                });
            return present_pass_result ? Result::success() : present_pass_result.result;
        };
        const Result result = preferences_orchestrator_->run_frame(engine_frame_desc);
        if (imgui_frame_active) {
            static_cast<void>(preferences_imgui_host_.end_frame());
        }
        if (!result || !has_readback) {
            return result;
        }
        if (const Result wait_result = device_->wait_idle(); !wait_result) {
            return wait_result;
        }
        std::vector<std::byte> pixels(static_cast<size_t>(readback_allocation.size));
        if (const Result read_result = preferences_frame_context_->read_readback(readback_allocation, pixels);
            !read_result) {
            return read_result;
        }
        constexpr uint32_t kClearProbeExtent = 8;
        const auto pixel_is_clear = [&pixels, width = target.extent.width](uint32_t x, uint32_t y) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            // The full-bleed Settings surface now covers the formerly black
            // padding. Corners must contain an opaque, dark themed background,
            // not uninitialized swapchain pixels left over after resize.
            return std::to_integer<unsigned>(pixels[offset]) <= 32 &&
                   std::to_integer<unsigned>(pixels[offset + 1]) <= 32 &&
                   std::to_integer<unsigned>(pixels[offset + 2]) <= 32 && pixels[offset + 3] == std::byte{255};
        };
        bool clear_probes_are_clean = true;
        for (uint32_t y = 0; y < kClearProbeExtent; ++y) {
            for (uint32_t x = 0; x < kClearProbeExtent; ++x) {
                clear_probes_are_clean = clear_probes_are_clean && pixel_is_clear(x, y) &&
                                         pixel_is_clear(target.extent.width - 1u - x, y) &&
                                         pixel_is_clear(x, target.extent.height - 1u - y) &&
                                         pixel_is_clear(target.extent.width - 1u - x, target.extent.height - 1u - y);
            }
        }
        if (!clear_probes_are_clean) {
            return aeg::foundation::invalid_state("preferences framebuffer regression check failed");
        }
        return result;
    }

    /** @brief Creates the editor-only Preferences model from the system font snapshot. */
    [[nodiscard]] bool initialize_preference_model() {
        try {
            EditorPreferenceState state{};
            const std::span<const aeg::editor::EditorFontAsset> assets = font_catalog_.assets();
            state.fonts.reserve(assets.size());
            for (const aeg::editor::EditorFontAsset& asset : assets) {
                state.fonts.push_back(asset.descriptor);
            }
            state.active_font_key = default_font_key_;
            state.font_size_pixels = imgui_host_.active_font_size_pixels();
            preference_model_ = std::make_unique<EditorPreferenceModel>(std::move(state));
            return preference_model_ != nullptr;
        } catch (const std::bad_alloc&) {
            static_cast<void>(report_result(aeg::foundation::out_of_memory("Editor preference model allocation failed"),
                                            "initialize_preference_model"));
            return false;
        }
    }

    /** @brief Applies one queued preference preview or font change after the previous frame has completed. */
    [[nodiscard]] bool process_pending_preference() {
        EditorPreferenceActions::PendingRequest request{};
        if (!preference_actions_.take_pending(request)) {
            return true;
        }

        if (request.operation == EditorPreferenceActions::Operation::kPreview) {
            const Result preview_result = preview_preference_font(request.font_key, request.font_size_pixels);
            if (!preview_result) {
                notify_runtime_result(preview_result, "preview_editor_font");
                if (preference_model_ != nullptr) {
                    static_cast<void>(
                        preference_model_->set_error("The font preview could not be updated. Please try again."));
                }
                return !fatal_runtime_error_;
            }
            preferences_regression_preview_completed_ = preferences_regression_mode_;
            return true;
        }

        std::string previous_font_key;
        try {
            previous_font_key = imgui_host_.active_font_key();
        } catch (const std::bad_alloc&) {
            notify_runtime_result(aeg::foundation::Result::failure(aeg::foundation::ResultCode::kOutOfMemory),
                                  "snapshot_editor_font");
            if (preference_model_ != nullptr) {
                static_cast<void>(preference_model_->set_error("Could not snapshot the active editor font."));
            }
            return !fatal_runtime_error_;
        }
        const float previous_font_size = imgui_host_.active_font_size_pixels();
        Result set_font_result = imgui_host_.set_font(request.font_key, request.font_size_pixels);
        if (!set_font_result) {
            notify_runtime_result(set_font_result, "set_editor_font");
            const Result restore_result = restore_editor_font(previous_font_key, previous_font_size);
            if (!restore_result) {
                static_cast<void>(report_result(restore_result, "restore_editor_font"));
                fatal_runtime_error_ = true;
                return false;
            }
            if (preference_model_ != nullptr) {
                static_cast<void>(preference_model_->set_error("The selected editor font could not be loaded."));
            }
            return true;
        }

        const Result refresh_result = refresh_font_texture();
        if (!refresh_result) {
            notify_runtime_result(refresh_result, "refresh_editor_font_texture");
            const Result restore_result = restore_editor_font(previous_font_key, previous_font_size);
            if (!restore_result) {
                static_cast<void>(report_result(restore_result, "restore_editor_font"));
                fatal_runtime_error_ = true;
                return false;
            }
            if (preference_model_ != nullptr) {
                static_cast<void>(
                    preference_model_->set_error("The editor font could not be applied. Please try again."));
            }
            return true;
        }

        if ((request.operation == EditorPreferenceActions::Operation::kApply ||
             request.operation == EditorPreferenceActions::Operation::kSave) &&
            preferences_imgui_host_.is_initialized()) {
            const Result preferences_result = apply_preferences_font(request.font_key, request.font_size_pixels);
            if (!preferences_result) {
                notify_runtime_result(preferences_result, "apply_preferences_window_font");
                const Result restore_result = restore_editor_font(previous_font_key, previous_font_size);
                if (!restore_result) {
                    static_cast<void>(report_result(restore_result, "restore_editor_font"));
                    fatal_runtime_error_ = true;
                    return false;
                }
                if (preference_model_ != nullptr) {
                    static_cast<void>(
                        preference_model_->set_error("The Settings font could not be applied. Please try again."));
                }
                return true;
            }
        }

        if (preference_model_ != nullptr) {
            switch (request.operation) {
                case EditorPreferenceActions::Operation::kApply:
                    preference_model_->accept();
                    break;
                case EditorPreferenceActions::Operation::kSave:
                    preference_model_->accept();
                    break;
                case EditorPreferenceActions::Operation::kCancel:
                    preference_model_->discard();
                    break;
                case EditorPreferenceActions::Operation::kPreview:
                    break;
            }
        }
        if (request.operation == EditorPreferenceActions::Operation::kSave ||
            request.operation == EditorPreferenceActions::Operation::kCancel) {
            const Result close_result = close_preferences();
            if (!close_result) {
                notify_runtime_result(close_result, "close_preferences");
                return !fatal_runtime_error_;
            }
        }
        return true;
    }

    /** @brief Restores the previous main-editor CPU atlas and its engine-owned GPU texture identity. */
    [[nodiscard]] Result restore_editor_font(std::string_view font_key, float font_size_pixels) {
        if (const Result result = imgui_host_.set_font(font_key, font_size_pixels); !result) {
            return result;
        }
        return refresh_font_texture();
    }

    /** @brief Rebuilds and replaces the engine-owned GPU font texture for the current CPU atlas. */
    [[nodiscard]] Result refresh_font_texture() {
        const GpuTexture previous_texture = font_texture_;
        const TextureDesc previous_desc = font_texture_desc_;
        const ImGuiTextureHandle previous_handle = font_texture_handle_;

        if (previous_handle.is_valid()) {
            if (const Result result = imgui_renderer_->release_prepared_frame(); !result) {
                return result;
            }
        }

        GpuTexture replacement_texture{};
        TextureDesc replacement_desc{};
        if (const Result result = upload_font_texture(replacement_desc, replacement_texture); !result) {
            return result;
        }

        if (!previous_handle.is_valid()) {
            static_cast<void>(texture_uploader_->destroy_texture(replacement_texture));
            return aeg::foundation::invalid_state("Editor font texture identity is not registered");
        }

        const ImGuiTextureBinding previous_binding = make_font_texture_binding(previous_texture, previous_desc);
        const ImGuiTextureBinding replacement_binding =
            make_font_texture_binding(replacement_texture, replacement_desc);
        if (const Result result = texture_registry_.update_texture(previous_handle, replacement_binding); !result) {
            static_cast<void>(texture_uploader_->destroy_texture(replacement_texture));
            return result;
        }
        if (const Result result = imgui_host_.set_font_texture(previous_handle); !result) {
            static_cast<void>(texture_registry_.update_texture(previous_handle, previous_binding));
            static_cast<void>(texture_uploader_->destroy_texture(replacement_texture));
            return result;
        }

        GpuTexture retired_texture = previous_texture;
        if (const Result result = texture_uploader_->destroy_texture(retired_texture); !result) {
            static_cast<void>(texture_registry_.update_texture(previous_handle, previous_binding));
            static_cast<void>(texture_uploader_->destroy_texture(replacement_texture));
            return result;
        }
        font_texture_ = replacement_texture;
        font_texture_desc_ = replacement_desc;
        return Result::success();
    }

    [[nodiscard]] bool close_requested() const noexcept {
        return window_ == nullptr || force_shutdown_ ||
               (window_->state().close_requested && !close_confirmation_requested_);
    }

    [[nodiscard]] bool create_swapchain() {
        const aeg::platform::WindowState state = window_->state();
        SwapchainDesc desc{};
        desc.surface = surface_;
        desc.extent = {state.extent.width, state.extent.height};
        desc.format = Format::kUnknown;
        desc.present_mode = PresentMode::kVsync;
        desc.min_image_count = 2;
        desc.frames_in_flight = 2;
        desc.image_capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        desc.name = aeg::rhi::DebugName{"Editor.Swapchain"};
        const auto swapchain_result = device_->create_swapchain(desc);
        if (!report_result(swapchain_result, "create_swapchain")) {
            return false;
        }
        swapchain_ = swapchain_result.value;
        return report_result(refresh_swapchain_info(), "get_swapchain_info");
    }

    [[nodiscard]] Result refresh_swapchain_info() {
        const auto info_result = device_->get_swapchain_info(swapchain_);
        if (!info_result) {
            return info_result.result;
        }
        swapchain_info_ = info_result.value;
        if (swapchain_info_.format == Format::kUnknown || swapchain_info_.extent.width == 0 ||
            swapchain_info_.extent.height == 0) {
            return aeg::foundation::invalid_state("Swapchain information is invalid");
        }
        return Result::success();
    }

    [[nodiscard]] Result recreate_swapchain() {
        if (const Result result = device_->wait_idle(); !result) {
            return result;
        }
        const aeg::platform::WindowState state = window_->state();
        if (state.minimized || !state.extent.is_valid()) {
            resize_pending_ = false;
            return Result::success();
        }
        SwapchainDesc desc{};
        desc.surface = surface_;
        desc.extent = {state.extent.width, state.extent.height};
        desc.present_mode = PresentMode::kVsync;
        desc.min_image_count = 2;
        desc.frames_in_flight = 2;
        desc.image_capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        if (const Result result = device_->recreate_swapchain(swapchain_, desc); !result) {
            return result;
        }
        if (const Result result = refresh_swapchain_info(); !result) {
            return result;
        }
        render_graph_->reset();
        resize_pending_ = false;
        return Result::success();
    }

    [[nodiscard]] Result upload_font_texture(EditorImGuiHost& host, FrameContext& frame_context,
                                             std::string_view texture_name, TextureDesc& output_desc,
                                             GpuTexture& output_texture) {
        ImGuiFontAtlasData atlas;
        if (const Result result = host.capture_font_atlas(atlas); !result) {
            return result;
        }
        if (!atlas.is_valid()) {
            return aeg::foundation::invalid_state("Captured ImGui font atlas is invalid");
        }
        TextureDesc texture_desc{};
        texture_desc.extent = {atlas.width, atlas.height, 1};
        texture_desc.format = Format::kRgba8Unorm;
        texture_desc.mip_levels = 1;
        texture_desc.array_layers = 1;
        texture_desc.capabilities = TextureCapability::kTransferDestination | TextureCapability::kSampled;
        texture_desc.memory = MemoryClass::kDeviceLocal;
        texture_desc.initial_usage = ResourceUsage::kUndefined;
        texture_desc.initial_stages = PipelineStage::kAllCommands;
        texture_desc.initial_queue = QueueClass::kAuto;
        texture_desc.name = aeg::rhi::DebugName{texture_name};

        const TextureSubresourceData subresource{
            std::span<const std::byte>{atlas.rgba8_pixels.data(), atlas.rgba8_pixels.size()},
            atlas.row_pitch,
            atlas.row_pitch * atlas.height,
        };
        const TextureData texture_data{
            texture_desc,
            std::span<const TextureSubresourceData>{&subresource, 1},
        };

        RenderGraph upload_graph(*device_);
        const Result begin_result = frame_context.begin_frame();
        if (!begin_result) {
            return begin_result;
        }
        const auto upload_result =
            texture_uploader_->upload_texture(frame_context, upload_graph, texture_data, texture_name);
        if (!upload_result) {
            static_cast<void>(frame_context.abort_frame());
            return upload_result.result;
        }
        GpuTexture uploaded_texture = upload_result.value.texture;

        const auto execution_result = upload_graph.execute(frame_context);
        if (!execution_result) {
            static_cast<void>(frame_context.abort_frame());
            static_cast<void>(texture_uploader_->destroy_texture(uploaded_texture));
            return execution_result.result;
        }
        if (execution_result.value.completion.is_valid()) {
            const Result wait_result =
                device_->wait(execution_result.value.completion, std::numeric_limits<uint64_t>::max());
            if (!wait_result) {
                static_cast<void>(frame_context.abort_frame());
                static_cast<void>(texture_uploader_->destroy_texture(uploaded_texture));
                return wait_result;
            }
        }
        const Result end_result = frame_context.end_frame();
        if (!end_result) {
            static_cast<void>(frame_context.abort_frame());
            static_cast<void>(texture_uploader_->destroy_texture(uploaded_texture));
            return end_result;
        }
        output_desc = texture_desc;
        output_texture = uploaded_texture;
        return Result::success();
    }

    [[nodiscard]] Result upload_font_texture(TextureDesc& output_desc, GpuTexture& output_texture) {
        return upload_font_texture(imgui_host_, *frame_context_, "Editor.ImGui.FontTexture", output_desc,
                                   output_texture);
    }

    [[nodiscard]] bool create_font_sampler() {
        const SamplerDesc desc{
            aeg::rhi::Filter::kLinear,
            aeg::rhi::Filter::kLinear,
            aeg::rhi::Filter::kLinear,
            aeg::rhi::AddressMode::kClampToEdge,
            aeg::rhi::AddressMode::kClampToEdge,
            aeg::rhi::AddressMode::kClampToEdge,
            0.0f,
            0.0f,
            1.0f,
            false,
            1.0f,
            aeg::rhi::DebugName{"Editor.ImGui.FontSampler"},
        };
        const auto sampler_result = device_->create_sampler(desc);
        if (!report_result(sampler_result, "create_imgui_font_sampler")) {
            return false;
        }
        font_sampler_ = sampler_result.value;
        return true;
    }

    [[nodiscard]] Result register_font_texture(const GpuTexture& texture, const TextureDesc& texture_desc,
                                               ImGuiTextureHandle& output_handle) {
        const ImGuiTextureBinding binding = make_font_texture_binding(texture, texture_desc);
        const auto texture_result = texture_registry_.register_texture(binding);
        if (!texture_result) {
            return texture_result.result;
        }
        output_handle = texture_result.value;
        return Result::success();
    }

    [[nodiscard]] ImGuiTextureBinding make_font_texture_binding(const GpuTexture& texture,
                                                                const TextureDesc& texture_desc) const noexcept {
        const ResourceState upload_state{
            ResourceUsage::kCopyDestination,
            aeg::rhi::ShaderStage::kAll,
            QueueClass::kCopy,
            false,
            PipelineStage::kCopy,
        };
        const ImGuiTextureBinding binding{
            texture.sampled_view, font_sampler_, texture.texture, texture_desc, upload_state,
        };
        return binding;
    }

    [[nodiscard]] Result register_font_texture() {
        return register_font_texture(font_texture_, font_texture_desc_, font_texture_handle_);
    }

    /** @brief Creates the initial Engine-owned font texture for the independent Preferences window. */
    [[nodiscard]] Result initialize_preferences_font_texture() {
        if (!preferences_imgui_host_.is_initialized() || preferences_frame_context_ == nullptr) {
            return aeg::foundation::invalid_state("Preferences font preview host is not initialized");
        }
        GpuTexture texture{};
        TextureDesc desc{};
        if (const Result result = upload_font_texture(preferences_imgui_host_, *preferences_frame_context_,
                                                      "Editor.Preferences.ImGui.FontTexture", desc, texture);
            !result) {
            return result;
        }
        ImGuiTextureHandle handle{};
        if (const Result result = register_font_texture(texture, desc, handle); !result) {
            static_cast<void>(texture_uploader_->destroy_texture(texture));
            return result;
        }
        if (const Result result = preferences_imgui_host_.set_font_texture(handle); !result) {
            static_cast<void>(texture_registry_.unregister_texture(handle));
            static_cast<void>(texture_uploader_->destroy_texture(texture));
            return result;
        }
        preferences_font_texture_ = texture;
        preferences_font_texture_desc_ = desc;
        preferences_font_texture_handle_ = handle;
        return Result::success();
    }

    /** @brief Replaces the Preferences preview texture after a draft font change. */
    [[nodiscard]] Result refresh_preferences_font_texture() {
        if (!preferences_imgui_host_.is_initialized() || preferences_frame_context_ == nullptr ||
            preferences_imgui_renderer_ == nullptr) {
            return aeg::foundation::invalid_state("Preferences font preview resources are not initialized");
        }
        if (const Result result = preferences_imgui_renderer_->release_prepared_frame(); !result) {
            return result;
        }

        GpuTexture replacement_texture{};
        TextureDesc replacement_desc{};
        if (const Result result =
                upload_font_texture(preferences_imgui_host_, *preferences_frame_context_,
                                    "Editor.Preferences.ImGui.FontTexture", replacement_desc, replacement_texture);
            !result) {
            return result;
        }
        const ImGuiTextureHandle previous_handle = preferences_font_texture_handle_;
        if (!previous_handle.is_valid()) {
            static_cast<void>(texture_uploader_->destroy_texture(replacement_texture));
            return aeg::foundation::invalid_state("Preferences font texture identity is not registered");
        }

        const ImGuiTextureBinding previous_binding =
            make_font_texture_binding(preferences_font_texture_, preferences_font_texture_desc_);
        const ImGuiTextureBinding replacement_binding =
            make_font_texture_binding(replacement_texture, replacement_desc);
        if (const Result result = texture_registry_.update_texture(previous_handle, replacement_binding); !result) {
            static_cast<void>(texture_uploader_->destroy_texture(replacement_texture));
            return result;
        }
        if (const Result result = preferences_imgui_host_.set_font_texture(previous_handle); !result) {
            static_cast<void>(texture_registry_.update_texture(previous_handle, previous_binding));
            static_cast<void>(texture_uploader_->destroy_texture(replacement_texture));
            return result;
        }

        GpuTexture previous_texture = preferences_font_texture_;
        if (const Result result = texture_uploader_->destroy_texture(previous_texture); !result) {
            static_cast<void>(texture_registry_.update_texture(previous_handle, previous_binding));
            static_cast<void>(texture_uploader_->destroy_texture(replacement_texture));
            return result;
        }
        preferences_font_texture_ = replacement_texture;
        preferences_font_texture_desc_ = replacement_desc;
        return Result::success();
    }

    /** @brief Applies a draft font only to the Preferences preview host. */
    [[nodiscard]] Result preview_preference_font(std::string_view font_key, float font_size_pixels) {
        if (!preferences_imgui_host_.is_initialized()) {
            return aeg::foundation::invalid_state("Preferences window is not open");
        }
        const std::string previous_font_key{preferences_imgui_host_.preview_font_key()};
        const float previous_font_size = preferences_imgui_host_.preview_font_size_pixels();
        if (const Result result = preferences_imgui_host_.set_preview_font(font_key, font_size_pixels); !result) {
            return result;
        }
        if (const Result result = refresh_preferences_font_texture(); !result) {
            const Result restore_result =
                preferences_imgui_host_.set_preview_font(previous_font_key, previous_font_size);
            if (!restore_result) {
                return restore_result;
            }
            if (preferences_font_texture_handle_.is_valid()) {
                static_cast<void>(preferences_imgui_host_.set_font_texture(preferences_font_texture_handle_));
            }
            return result;
        }
        return Result::success();
    }

    /** Applies the accepted font to Settings while preserving its draft preview. */
    [[nodiscard]] Result apply_preferences_font(std::string_view font_key, float font_size_pixels) {
        const std::string previous_font_key{preferences_imgui_host_.active_font_key()};
        const float previous_font_size = preferences_imgui_host_.active_font_size_pixels();
        if (const Result result = preferences_imgui_host_.set_font(font_key, font_size_pixels); !result) {
            return result;
        }
        if (const Result result = refresh_preferences_font_texture(); !result) {
            const Result restore_result = preferences_imgui_host_.set_font(previous_font_key, previous_font_size);
            if (!restore_result)
                return restore_result;
            if (preferences_font_texture_handle_.is_valid()) {
                static_cast<void>(preferences_imgui_host_.set_font_texture(preferences_font_texture_handle_));
            }
            return result;
        }
        return Result::success();
    }

    [[nodiscard]] Result run_engine_frame(const ImGuiDrawPacket& draw_packet) {
        const ImGuiRenderTargetDesc target{
            aeg::engine::kSwapchainImageResourceId,
            swapchain_info_.format,
            swapchain_info_.extent,
            regression_mode_ ? ResourceUsage::kCopySource : ResourceUsage::kPresent,
        };
        aeg::engine::EngineFrameDesc frame_desc{};
        frame_desc.fixed_tick = frame_count_;
        frame_desc.acquire_swapchain_image = true;
        frame_desc.swapchain = swapchain_;
        frame_desc.swapchain_image_desc = make_swapchain_image_desc();
        frame_desc.present_acquired_frame = true;
        frame_desc.user_data = this;
        FrameAllocation readback_allocation{};
        bool has_readback = false;
        frame_desc.render_feature_preparation = [this, &draw_packet, target, &readback_allocation, &has_readback](
                                                    const aeg::engine::EngineFramePreparationContext& context) {
            if (context.acquired_frame == nullptr) {
                return aeg::foundation::invalid_state("ImGui preparation requires an acquired swapchain image");
            }
            if (!context.swapchain_resource.is_valid()) {
                return aeg::foundation::invalid_state("ImGui preparation requires an imported swapchain resource");
            }
            const auto clear_result = context.render_graph.add_pass(
                "Editor.ClearSwapchain", QueueClass::kGraphics,
                [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                    return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kClear, aeg::rhi::StoreOp::kStore,
                                                        ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f},
                                                        ResourceUsage::kColorAttachmentWrite);
                },
                [](aeg::render_graph::PassExecutionContext& pass) {
                    if (const Result result = pass.begin_rendering(); !result) {
                        return result;
                    }
                    return pass.end_rendering();
                });
            if (!clear_result) {
                return clear_result.result;
            }
            if (const Result result =
                    imgui_renderer_->prepare_frame(context.frame_context, context.render_graph, draw_packet, target);
                !result) {
                return result;
            }
            if (!regression_mode_) {
                return Result::success();
            }

            const uint64_t pixel_count = static_cast<uint64_t>(target.extent.width) * target.extent.height;
            const auto allocation_result = context.frame_context.allocate_readback(pixel_count * 4u, 256);
            if (!allocation_result) {
                return allocation_result.result;
            }
            readback_allocation = allocation_result.value;
            has_readback = true;

            aeg::rhi::BufferDesc readback_desc{};
            readback_desc.size = readback_allocation.buffer_size;
            readback_desc.capabilities = aeg::rhi::BufferCapability::kTransferSource |
                                         aeg::rhi::BufferCapability::kTransferDestination |
                                         aeg::rhi::BufferCapability::kVertex | aeg::rhi::BufferCapability::kIndex |
                                         aeg::rhi::BufferCapability::kUniform;
            readback_desc.memory = aeg::rhi::MemoryClass::kReadback;
            readback_desc.initial_usage = ResourceUsage::kHostRead;
            readback_desc.initial_stages = PipelineStage::kHost;
            readback_desc.initial_queue = QueueClass::kCopy;
            readback_desc.name = aeg::rhi::DebugName{"Editor.FramebufferReadback"};
            const auto readback_resource_result =
                context.render_graph.import_buffer(readback_allocation.buffer, readback_desc,
                                                   ResourceState{ResourceUsage::kHostRead, aeg::rhi::ShaderStage::kAll,
                                                                 QueueClass::kCopy, false, PipelineStage::kHost});
            if (!readback_resource_result) {
                return readback_resource_result.result;
            }
            const auto readback_pass_result = context.render_graph.add_pass(
                "Editor.FramebufferReadback", QueueClass::kCopy,
                [swapchain_resource = context.swapchain_resource, readback_resource = readback_resource_result.value,
                 readback_allocation, pixel_count](aeg::render_graph::PassBuilder& builder) {
                    if (const Result result =
                            builder.read_to(swapchain_resource, ResourceUsage::kCopySource, PipelineStage::kCopy,
                                            aeg::rhi::ShaderStage::kAll, ResourceUsage::kPresent);
                        !result) {
                        return result;
                    }
                    if (const Result result = builder.write_to(
                            readback_resource, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                            aeg::rhi::ShaderStage::kAll, ResourceUsage::kHostRead,
                            aeg::render_graph::RenderGraphBufferRange{readback_allocation.offset, pixel_count * 4u},
                            true);
                        !result) {
                        return result;
                    }
                    builder.set_side_effects();
                    return Result::success();
                },
                [swapchain_resource = context.swapchain_resource, readback_resource = readback_resource_result.value,
                 readback_allocation, target](aeg::render_graph::PassExecutionContext& pass) {
                    return pass.copy_texture_to_buffer(
                        swapchain_resource, aeg::rhi::TextureSubresourceLayers{aeg::rhi::Aspect::kColor, 0, 0, 1},
                        aeg::rhi::Offset3D{0, 0, 0}, aeg::rhi::Extent3D{target.extent.width, target.extent.height, 1},
                        readback_resource, readback_allocation.offset, 0, 0);
                });
            if (!readback_pass_result) {
                return readback_pass_result.result;
            }
            const auto present_pass_result = context.render_graph.add_pass(
                "Editor.PresentSwapchain", QueueClass::kGraphics,
                [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                    return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kLoad, aeg::rhi::StoreOp::kStore,
                                                        ClearColorValue{}, ResourceUsage::kPresent);
                },
                [](aeg::render_graph::PassExecutionContext& pass) {
                    if (const Result result = pass.begin_rendering(); !result) {
                        return result;
                    }
                    return pass.end_rendering();
                });
            return present_pass_result ? Result::success() : present_pass_result.result;
        };
        const Result frame_result = orchestrator_->run_frame(frame_desc);
        if (!frame_result || !has_readback) {
            return frame_result;
        }
        if (const Result wait_result = device_->wait_idle(); !wait_result) {
            return wait_result;
        }
        std::vector<std::byte> pixels(static_cast<size_t>(readback_allocation.size));
        if (const Result read_result = frame_context_->read_readback(readback_allocation, pixels); !read_result) {
            return read_result;
        }
        constexpr uint32_t kClearProbeExtent = 8;
        const auto pixel_is_clear = [&pixels, width = target.extent.width](uint32_t x, uint32_t y) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            return pixels[offset] == std::byte{0} && pixels[offset + 1] == std::byte{0} &&
                   pixels[offset + 2] == std::byte{0};
        };
        bool clear_probes_are_clean = true;
        for (uint32_t y = 0; y < kClearProbeExtent; ++y) {
            for (uint32_t x = 0; x < kClearProbeExtent; ++x) {
                clear_probes_are_clean = clear_probes_are_clean && pixel_is_clear(x, y) &&
                                         pixel_is_clear(target.extent.width - 1u - x, y) &&
                                         pixel_is_clear(x, target.extent.height - 1u - y) &&
                                         pixel_is_clear(target.extent.width - 1u - x, target.extent.height - 1u - y);
            }
        }
        if (!clear_probes_are_clean) {
            return aeg::foundation::invalid_state("editor framebuffer regression check failed");
        }
        return frame_result;
    }

    [[nodiscard]] TextureDesc make_swapchain_image_desc_for(const SwapchainInfo& info,
                                                            std::string_view name) const noexcept {
        TextureDesc desc{};
        desc.extent = {info.extent.width, info.extent.height, 1};
        desc.format = info.format;
        desc.mip_levels = 1;
        desc.array_layers = 1;
        desc.capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        desc.memory = MemoryClass::kDeviceLocal;
        desc.initial_usage = ResourceUsage::kPresent;
        desc.initial_stages = PipelineStage::kAllCommands;
        desc.initial_queue = QueueClass::kGraphics;
        desc.name = aeg::rhi::DebugName{name};
        return desc;
    }

    [[nodiscard]] TextureDesc make_swapchain_image_desc() const noexcept {
        return make_swapchain_image_desc_for(swapchain_info_, "Editor.SwapchainImage");
    }

    void cleanup() noexcept {
        if (startup_splash_ != nullptr) {
            startup_splash_->close();
            startup_splash_.reset();
        }
        static_cast<void>(close_preferences());
        if (viewport_texture_handle_.is_valid()) {
            static_cast<void>(texture_registry_.unregister_texture(viewport_texture_handle_));
            viewport_texture_handle_ = {};
        }
        if (imgui_renderer_ != nullptr) {
            static_cast<void>(imgui_renderer_->release_prepared_frame());
        }
        if (scene_runtime_ != nullptr) {
            static_cast<void>(scene_runtime_->shutdown());
            scene_runtime_.reset();
        }
        if (orchestrator_ != nullptr) {
            static_cast<void>(orchestrator_->shutdown());
            orchestrator_.reset();
        }
        if (imgui_renderer_ != nullptr) {
            static_cast<void>(imgui_renderer_->shutdown());
            imgui_renderer_.reset();
        }
        if (imgui_host_.is_initialized()) {
            static_cast<void>(imgui_host_.shutdown());
        }
        if (font_texture_handle_.is_valid()) {
            static_cast<void>(texture_registry_.unregister_texture(font_texture_handle_));
            font_texture_handle_ = {};
        }
        if (device_ != nullptr) {
            static_cast<void>(device_->wait_idle());
            if (font_sampler_.is_valid()) {
                static_cast<void>(device_->destroy_sampler(font_sampler_));
                font_sampler_ = {};
            }
            if (texture_uploader_ != nullptr && font_texture_.is_valid()) {
                static_cast<void>(texture_uploader_->destroy_texture(font_texture_));
            }
        }
        render_graph_.reset();
        frame_context_.reset();
        texture_uploader_.reset();
        if (device_ != nullptr && swapchain_.is_valid()) {
            static_cast<void>(device_->destroy_swapchain(swapchain_));
            swapchain_ = {};
        }
        if (device_ != nullptr && surface_.is_valid()) {
            static_cast<void>(device_->destroy_surface(surface_));
            surface_ = {};
        }
        device_.reset();
        if (window_system_ != nullptr && window_ != nullptr) {
            static_cast<void>(window_system_->destroy_window(window_->id()));
            window_.reset();
        }
        window_system_.reset();
        initialized_ = false;
        aeg::logging::flush();
        aeg::logging::shutdown();
    }

    std::unique_ptr<aeg::application::IStartupSplash> startup_splash_{};
    std::unique_ptr<IWindowSystem> window_system_{};
    std::shared_ptr<IWindow> window_{};
    std::unique_ptr<Device> device_{};
    SurfaceHandle surface_{};
    SwapchainHandle swapchain_{};
    SwapchainInfo swapchain_info_{};
    std::unique_ptr<FrameContext> frame_context_{};
    std::unique_ptr<RenderGraph> render_graph_{};
    std::unique_ptr<TextureUploader> texture_uploader_{};
    /** @brief Work area used for adaptive startup sizing and resize regression extents. */
    DisplayWorkArea work_area_{};
    /** @brief Independent native Preferences window owned by the editor application. */
    std::shared_ptr<IWindow> preferences_window_{};
    SurfaceHandle preferences_surface_{};
    SwapchainHandle preferences_swapchain_{};
    SwapchainInfo preferences_swapchain_info_{};
    std::unique_ptr<FrameContext> preferences_frame_context_{};
    std::unique_ptr<RenderGraph> preferences_render_graph_{};
    EditorImGuiHost preferences_imgui_host_{};
    std::unique_ptr<ImGuiRenderer> preferences_imgui_renderer_{};
    PipelineRegistry preferences_registry_{};
    PipelineScheduler preferences_scheduler_{{1, 1, 1, 1}};
    PipelineScopeHost preferences_frame_host_{PipelineScope::kFrame,
                                              aeg::pipeline::PipelineInstanceId("Editor.Preferences.Frame")};
    std::unique_ptr<aeg::engine::EngineFrameOrchestrator> preferences_orchestrator_{};
    /** @brief Application-owned system font bytes shared with the editor host. */
    EditorFontCatalog font_catalog_{};
    /** @brief Stable key selected as the initial system font. */
    std::string default_font_key_{};
    /** @brief Editor-only Preferences draft state. */
    std::unique_ptr<EditorPreferenceModel> preference_model_{};
    /** @brief Safe-frame command queue owned by the Application layer. */
    EditorPreferenceActions preference_actions_{};
    /** @brief Independent Preferences font atlas texture owned through Engine Resource/RHI APIs. */
    GpuTexture preferences_font_texture_{};
    /** @brief Description of the independent Preferences font atlas texture. */
    TextureDesc preferences_font_texture_desc_{};
    /** @brief Registry identity bound to the independent Preferences font atlas texture. */
    ImGuiTextureHandle preferences_font_texture_handle_{};
    GpuTexture font_texture_{};
    TextureDesc font_texture_desc_{};
    aeg::rhi::SamplerHandle font_sampler_{};
    ImGuiTextureRegistry texture_registry_{};
    ImGuiTextureHandle font_texture_handle_{};
    ImGuiTextureHandle viewport_texture_handle_{};
    std::unique_ptr<EditorSceneRuntime> scene_runtime_{};
    EditorImGuiHost imgui_host_{};
    /** @brief Application-owned five-panel editor workspace. */
    EditorWorkspace workspace_;
    /** @brief Pipeline edits remain local until the next successful scene save. */
    aeg::application::PipelineOrderingDraftQueue pipeline_drafts_{};
    /** @brief Stable graph edits restored from and persisted beside the active scene document. */
    aeg::application::PipelineOrderingSaveState pipeline_saved_orderings_{};
    /** Free-form Pipeline node placement saved beside the scene document. */
    aeg::application::PipelineLayoutSaveState pipeline_layouts_{};
    aeg::application::PipelineBlendSaveState pipeline_saved_blends_{};
    std::unique_ptr<ImGuiRenderer> imgui_renderer_{};
    PipelineRegistry registry_{};
    PipelineScheduler scheduler_{{1, 1, 1, 1}};
    PipelineScopeHost frame_host_{PipelineScope::kFrame, aeg::pipeline::PipelineInstanceId("Editor.Frame")};
    std::unique_ptr<aeg::engine::EngineFrameOrchestrator> orchestrator_{};
    InputSystem input_system_{};
    EditorSceneRuntime::Mode last_scene_mode_{EditorSceneRuntime::Mode::kEdit};
    /** @brief Application-owned recoverable error notice shown by the editor UI. */
    EditorErrorState error_state_{};
    std::chrono::steady_clock::time_point last_frame_time_{};
    uint64_t frame_count_{0};
    bool resize_pending_{false};
    bool preferences_resize_pending_{false};
    bool preferences_open_requested_{false};
    bool preferences_close_requested_{false};
    bool main_window_input_enabled_{true};
    /** @brief Enables the automatic main-window resize sequence. */
    bool resize_regression_{false};
    /** @brief Enables the independent Preferences lifecycle regression. */
    bool preferences_regression_mode_{false};
    /** @brief Records that the Preferences regression window was opened successfully. */
    bool preferences_regression_opened_{false};
    /** @brief Records that the Preferences regression window was closed successfully. */
    bool preferences_regression_closed_{false};
    /** @brief Records that the Preferences regression window was opened successfully a second time. */
    bool preferences_regression_reopened_{false};
    /** @brief Records that the Preferences regression requested a real font preview replacement. */
    bool preferences_regression_preview_requested_{false};
    /** @brief Records that the Preferences regression completed the font preview replacement. */
    bool preferences_regression_preview_completed_{false};
    bool regression_mode_{false};
    bool fatal_runtime_error_{false};
    bool initialized_{false};
    std::filesystem::path resource_root_{};
    std::filesystem::path startup_project_{};
    bool close_confirmation_requested_{false};
    bool force_shutdown_{false};
    bool scene_dialog_pending_{false};
    bool unsaved_changes_pending_{false};
    SceneDialogMode scene_dialog_mode_{SceneDialogMode::kOpen};
    EditorDocumentAction pending_document_action_{};
    std::array<char, 512> scene_path_buffer_{};
    Result scene_operation_{Result::success()};
    Result unsaved_operation_{Result::success()};
    ProjectDialogMode project_dialog_mode_{ProjectDialogMode::kNew};
    bool project_dialog_pending_{false};
    std::array<char, 512> project_path_buffer_{};
    std::array<char, 128> project_name_buffer_{};
    Result project_operation_{Result::success()};
};

uint32_t parse_frame_limit(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view{argv[index]} != "--frames") {
            continue;
        }
        const unsigned long long value = std::strtoull(argv[index + 1], nullptr, 10);
        return value > std::numeric_limits<uint32_t>::max() ? 0 : static_cast<uint32_t>(value);
    }
    return 0;
}

std::filesystem::path parse_path_argument(int argc, char** argv, std::string_view argument) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view{argv[index]} == argument) {
            return argv[index + 1];
        }
    }
    return {};
}

bool has_argument(int argc, char** argv, std::string_view argument) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} == argument) {
            return true;
        }
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    const auto console_interrupt_result = aeg::application::install_console_interrupt_controller();
    if (!console_interrupt_result || console_interrupt_result.value == nullptr) {
        return 1;
    }
    EditorApplication application;
    const std::filesystem::path resource_root = std::filesystem::absolute(std::filesystem::path{argv[0]}).parent_path();
    if (!application.initialize(resource_root, parse_path_argument(argc, argv, "--project"))) {
        return 1;
    }
    const int result = application.run(parse_frame_limit(argc, argv), has_argument(argc, argv, "--stress-resize"),
                                       has_argument(argc, argv, "--open-preferences"),
                                       *console_interrupt_result.value);
    return result;
}
