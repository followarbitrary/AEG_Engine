/**
 * @file editor_application.cpp
 * @brief Minimal editor application assembled from the public engine contracts.
 */
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

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
#include "imgui_internal.h"
#include "imgui_renderer.hpp"
#include "imgui_texture_registry.hpp"
#include "input_system.hpp"
#include "logging.hpp"
#include "pipeline.hpp"
#include "pipeline_overview_layout.hpp"
#include "preferences/editor_preference_controller.hpp"
#include "project_context.hpp"
#include "render_graph.hpp"
#include "render_state_flow.hpp"
#include "renderer.hpp"
#include "renderer_variants.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "texture_uploader.hpp"

struct ImGuiWindow;

namespace {
using aeg::application::ComponentUpdateView;
using aeg::application::EditorAssetStatus;
using aeg::application::EditorAssetImportRequest;
using aeg::application::EditorFontRepository;
using aeg::application::EditorProjectManager;
using aeg::application::EditorRecentProject;
using aeg::application::EditorPreferenceController;
using aeg::application::EditorPreferenceControllerDesc;
using aeg::application::EditorPreferenceStore;
using aeg::application::EditorResourceDescriptor;
using aeg::application::EditorResourceKind;
using aeg::application::EditorSceneRuntime;
using aeg::application::ProjectCreateOptions;
using aeg::application::project::ProjectConfig;
using aeg::application::project::ProjectFileCodec;
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
using aeg::foundation::ResultValue;
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
using aeg::platform::WindowId;
using aeg::platform::WindowIdHash;
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

// Bounded CLI runs must never wait behind a native error dialog.
bool error_dialogs_enabled = true;

bool report_result(const Result& result, std::string_view operation) noexcept {
    if (result) {
        return true;
    }
    static_cast<void>(aeg::editor::guard_editor_operation([&] {
        AEG_LOG_ERROR(aeg::logging::Category::kApplication, "Editor operation {} failed [{}]: {}", operation,
                      aeg::foundation::result_code_name(result.code), result.message);
        return Result::success();
    }));
    if (error_dialogs_enabled) {
        static_cast<void>(aeg::editor::guard_editor_operation([] {
            return aeg::application::show_error_dialog(
                "Aether Engine Error",
                "The editor could not complete the requested operation. Details are available in the log.");
        }));
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
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path{} : temporary / "AEG-Engine" / "Editor" / "logs" / "aeg-editor.log";
}

std::filesystem::path editor_preference_root() {
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data != nullptr && *local_app_data != '\0') {
        return std::filesystem::path(std::string(local_app_data)) / "AEG-Engine" / "Editor";
    }
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path{} : temporary / "AEG-Engine" / "Editor";
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

template <std::size_t Size>
void set_text_buffer(std::array<char, Size>& buffer, std::string_view value) {
    std::fill(buffer.begin(), buffer.end(), '\0');
    const std::size_t length = std::min(value.size(), buffer.size() - 1u);
    if (length != 0u) {
        std::memcpy(buffer.data(), value.data(), length);
    }
}

enum class EditorDocumentActionType : uint8_t {
    kNewScene,
    kOpenScene,
    kSaveScene,
    kSaveAsScene,
    kNewProject,
    kImportProject,
    kCloseProject,
    kCloseEditor,
};

struct EditorDocumentAction final {
    EditorDocumentActionType type{EditorDocumentActionType::kNewScene};
    std::filesystem::path path{};
    std::string name{};
    std::optional<ProjectCreateOptions> project_options{};
};

using EditorDocumentActionCallback = std::function<void(EditorDocumentAction)>;

[[nodiscard]] uint32_t resource_kind_value(EditorResourceKind kind) noexcept {
    return static_cast<uint32_t>(kind);
}

[[nodiscard]] const char* editor_resource_kind_name(EditorResourceKind kind) noexcept {
    switch (kind) {
        case EditorResourceKind::kMesh:
            return "Mesh";
        case EditorResourceKind::kMaterial:
            return "Material";
        case EditorResourceKind::kTexture:
            return "Texture";
        case EditorResourceKind::kScene:
            return "Scene";
        case EditorResourceKind::kFont:
            return "Font";
        case EditorResourceKind::kUnknown:
            return "File";
    }
    return "File";
}

class SceneViewportPanel final : public aeg::editor::EditorPanel {
public:
    SceneViewportPanel(EditorSceneRuntime& runtime, ImGuiTextureHandle& texture_handle,
                       EditorDocumentActionCallback document_action) noexcept
        : runtime_(runtime), texture_handle_(texture_handle), document_action_(std::move(document_action)) {}

    void build_ui() override {
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
        if (!runtime_.renderer_pipeline_diagnostic().empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{1.0f, 0.75f, 0.25f, 1.0f}, "Default preview");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", std::string(runtime_.renderer_pipeline_diagnostic()).c_str());
        }
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
                                 ImGui::GetMousePos().y > toolbar_bottom;
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
        const ImVec2 mouse = ImGui::GetMousePos();
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
    PipelinePanel(PipelineScopeHost& host, EditorImGuiHost& imgui_host, EditorSceneRuntime& runtime,
                  aeg::application::PipelineOrderingDraftQueue& drafts,
                  aeg::application::PipelineLayoutSaveState& layouts, std::function<Result()> save_changes,
                  std::function<void(bool)> visibility_changed = {}, std::function<bool()> native_presentation = {})
        : host_(host),
          imgui_host_(imgui_host),
          runtime_(runtime),
          drafts_(drafts),
          layouts_(layouts),
          save_changes_(std::move(save_changes)),
          visibility_changed_(std::move(visibility_changed)),
          native_presentation_(std::move(native_presentation)) {}

    [[nodiscard]] bool is_visible() const noexcept { return visible_; }
    void set_visible(bool visible) {
        if (visible_ == visible)
            return;
        visible_ = visible;
        if (visibility_changed_)
            visibility_changed_(visible_);
    }

    void build_ui() override {
        if (!is_visible())
            return;
        const bool native_requested = native_presentation_ && native_presentation_();
        ImGuiWindow* window = ImGui::FindWindowByName("Pipeline");
        const ImGuiViewport* main_vp = ImGui::GetMainViewport();
        const bool is_native = window != nullptr && window->Viewport != nullptr && window->Viewport != main_vp &&
                               (window->Viewport->Flags & ImGuiViewportFlags_IsPlatformWindow) != 0;
        // The presentation choice is known before Dear ImGui has finished
        // moving the window to its platform viewport. Use it together with
        // the observed viewport state so the native size policy applies on
        // the transition frame as well.
        const bool native_mode = native_requested || is_native;
        if (expand_) {
            const auto* viewport = main_vp;
            ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
            // A native panel must keep its platform viewport. Only docked
            // panels are explicitly placed in the main viewport here.
            if (!native_mode)
                ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::SetNextWindowPos({viewport->WorkPos.x + 80, viewport->WorkPos.y + 60}, ImGuiCond_Once);
            ImGui::SetNextWindowSize(
                {std::max(400.0f, viewport->WorkSize.x - 160), std::max(300.0f, viewport->WorkSize.y - 120)},
                ImGuiCond_Once);
            expand_ = false;
        } else {
            if (native_requested && !is_native) {
                // Let Dear ImGui undock the window and create its platform
                // viewport. The platform backend only supplies native window
                // callbacks; it does not own the presentation choice.
                ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
                ImGui::SetNextWindowPos({main_vp->WorkPos.x + 96.0f, main_vp->WorkPos.y + 96.0f}, ImGuiCond_Once);
            }
            ImGui::SetNextWindowSizeConstraints({320.0f, 220.0f},
                                                {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()});
        }

        ImGuiWindowFlags flags = ImGuiWindowFlags_None;
        if (native_mode) {
            // Scrolling belongs to the catalog, graph, inspector, and
            // validation children. Keeping the parent window non-scrolling
            // leaves the inline chrome's close control above those children
            // and prevents the parent scrollbar from covering it.
            flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        }

        ImGuiContext& g = *GImGui;
        const bool is_dragging = (window != nullptr && g.MovingWindow == window);

        if (is_dragging) {
            ImGui::SetNextWindowBgAlpha(0.8f);
        } else {
            ImGui::SetNextWindowBgAlpha(1.0f);
        }

        bool open = visible_;
        if (!ImGui::Begin("Pipeline", &open, flags)) {
            if (!open)
                set_visible(false);
            ImGui::End();
            return;
        }

        if (native_mode) {
            draw_inline_aeg_chrome(&open);
        }

        ImGuiWindow* current_win = ImGui::GetCurrentWindow();

        if (is_dragging && current_win->DockNode) {
            current_win->DockNode->LocalFlags |= static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDockingOverMe);
            current_win->DockNode->LocalFlags |=
                static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDockingInCentralNode);
        }

        if (!open) {
            set_visible(false);
            ImGui::End();
            return;
        }

        if (is_dragging) {
            ImDrawList* bg_draw_list = ImGui::GetWindowDrawList();
            ImVec2 win_pos = ImGui::GetWindowPos();
            ImVec2 win_size = ImGui::GetWindowSize();
            bg_draw_list->AddRectFilled(win_pos, ImVec2(win_pos.x + win_size.x, win_pos.y + win_size.y),
                                        IM_COL32(15, 22, 28, 200));
        }

        const bool compact_dock = ImGui::GetWindowHeight() < 720.0f;
        // Keep the pipeline authoring surface visually independent from the
        // editor workspace so it reads like the dedicated design reference.
        if (!compact_dock) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0f, 6.0f});
            ImGui::TextColored(ImVec4{0.25f, 0.92f, 0.88f, 1.0f}, "AEG");
            ImGui::SameLine();
            ImGui::TextUnformatted("|   AEG Engine  —  Pipeline Editor");
            ImGui::SameLine(ImGui::GetWindowWidth() - 300.0f);
            ImGui::TextDisabled("Workspace");
            ImGui::SameLine();
            ImGui::Button("Game  v");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{0.30f, 0.88f, 0.83f, 1.0f}, "Ready");
            ImGui::Separator();
            ImGui::TextDisabled("File");
            ImGui::SameLine();
            ImGui::TextDisabled("Edit");
            ImGui::SameLine();
            ImGui::TextDisabled("Assets");
            ImGui::SameLine();
            ImGui::TextDisabled("Pipeline");
            ImGui::SameLine();
            ImGui::TextDisabled("Window");
            ImGui::SameLine();
            ImGui::TextDisabled("Help");
            ImGui::PopStyleVar();
            ImGui::Separator();
        }
        // Authoring toolbar mirrors the visual language of the Pipeline editor
        // concept: high-frequency graph actions stay in one compact row above
        // the canvas while the existing source/mode controls remain below it.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{8.0f, 5.0f});
        if (ImGui::Button("Save")) {
            Result result = apply_pending_blends();
            if (result)
                result = save_changes_();
            if (result) {
                layout_dirty_ = false;
                blend_dirty_ = false;
                pending_blends_.clear();
            } else {
                rollback_pending_blends();
            }
            feedback_ = result ? "Pipeline saved." : result.message;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled();
        if (ImGui::Button("Undo"))
            feedback_ = "Undo is available from the editor history menu.";
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled();
        if (ImGui::Button("Redo"))
            feedback_ = "Redo is available from the editor history menu.";
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Auto-Layout")) {
            arrange();
            capture_layout();
            fit_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Validate")) {
            feedback_ = edges_.empty() ? "Validation warning: graph has no links."
                                       : "Validation complete: graph links are structurally connected.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Preview")) {
            feedback_ = "Preview uses the active runtime plan; save changes before starting the scene.";
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Platform");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(126.0f);
        ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##pipeline-platform", "PC (Vulkan)"))
            ImGui::EndCombo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Config");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(112.0f);
        ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##pipeline-config", "Editor"))
            ImGui::EndCombo();
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::Separator();
        if (!compact_dock) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0f, 6.0f});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.08f, 0.21f, 0.25f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.11f, 0.31f, 0.33f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.13f, 0.38f, 0.37f, 1.0f});
            ImGui::Button("MainRenderPipeline.pipeline*");
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::Button("+");
            ImGui::SameLine();
            ImGui::TextDisabled("AEG Engine  /  Pipeline Editor");
            ImGui::PopStyleVar();
            ImGui::Separator();
        }
        struct PipelineSource final {
            std::string name{};
            aeg::pipeline::PipelinePlanHandle plan{};
            const aeg::application::EditorProjectPipelineDocument* authored_document{nullptr};
        };
        std::vector<PipelineSource> sources;
        for (auto&& [name, plan] : runtime_.pipeline_snapshots())
            sources.push_back({std::move(name), std::move(plan), nullptr});
        const std::span<const aeg::application::EditorProjectPipelineDocument> authored_documents =
            runtime_.authored_pipeline_documents();
        for (const auto& document : authored_documents) {
            const std::string label =
                document.document.name.empty() ? document.path.stem().string() : document.document.name;
            sources.push_back({"Project / " + label, {}, &document});
        }
        if (aeg::application::builtin_pipeline_debug_enabled()) {
            if (auto snapshot = host_.active_plan_snapshot())
                sources.push_back({"Built-in / Editor UI", std::move(snapshot), nullptr});
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
        std::string authored_sources_signature;
        for (const auto& document : authored_documents)
            authored_sources_signature += document.path.generic_string() + '|';
        if (authored_sources_signature_ != authored_sources_signature) {
            if (!authored_documents.empty()) {
                source_ = static_cast<std::size_t>(std::distance(
                    sources.begin(), std::find_if(sources.begin(), sources.end(), [](const PipelineSource& source) {
                        return source.authored_document != nullptr;
                    })));
            }
            source_preferred_ = true;
            signature_.clear();
            authored_sources_signature_ = std::move(authored_sources_signature);
        } else if (!source_preferred_ && aeg::application::builtin_pipeline_debug_enabled() && sources.size() > 1) {
            source_ = sources.size() - 1;
            source_preferred_ = true;
            signature_.clear();
        }
        source_ = std::min(source_, sources.size() - 1);
        ImGui::TextUnformatted("Pipeline");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(210);
        if (ImGui::BeginCombo("##pipeline-source", sources[source_].name.c_str())) {
            for (std::size_t i = 0; i < sources.size(); ++i)
                if (ImGui::Selectable(sources[i].name.c_str(), i == source_)) {
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
        if (ImGui::SmallButton("Frame All"))
            fit_ = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("Expand"))
            expand_ = true;
        const PipelineSource& selected_source = sources[source_];
        const bool authored_source = selected_source.authored_document != nullptr;
        if (authored_source && !selected_source.authored_document->load_result) {
            ImGui::BeginChild("PipelineLoadError", ImVec2{0, 0}, true);
            ImGui::TextWrapped("Could not load %s: %s", selected_source.authored_document->path.string().c_str(),
                               selected_source.authored_document->load_result.message.c_str());
            ImGui::TextWrapped("Repair the file, then reload the project pipelines.");
            if (ImGui::Button("Reload Pipelines")) {
                const auto result = runtime_.refresh_assets();
                feedback_ = result ? "Project pipelines reloaded." : result.message;
                signature_.clear();
            }
            if (!feedback_.empty())
                ImGui::TextWrapped("%s", feedback_.c_str());
            ImGui::EndChild();
            ImGui::End();
            return;
        }
        const bool selectable_renderer =
            authored_source &&
            std::any_of(selected_source.authored_document->document.pipeline_nodes.begin(),
                        selected_source.authored_document->document.pipeline_nodes.end(), [](const auto& node) {
                            return node.type == aeg::renderer::kDeferredRenderPipelineId ||
                                   node.type == aeg::renderer::kForwardRenderPipelineId;
                        });
        if (selectable_renderer) {
            ImGui::BeginDisabled(runtime_.mode() != EditorSceneRuntime::Mode::kEdit);
            if (ImGui::SmallButton("Apply to Viewport")) {
                const auto result =
                    runtime_.activate_renderer_pipeline_document(selected_source.authored_document->path);
                feedback_ = result ? "Renderer applied to viewport and saved for this project." : result.message;
            }
            ImGui::EndDisabled();
            if (runtime_.active_renderer_pipeline_document() == selected_source.authored_document->path) {
                ImGui::SameLine();
                ImGui::TextDisabled("Active in viewport");
            }
        }
        if (!runtime_.active_renderer_pipeline_document().empty()) {
            if (selectable_renderer)
                ImGui::SameLine();
            ImGui::BeginDisabled(runtime_.mode() != EditorSceneRuntime::Mode::kEdit);
            if (ImGui::SmallButton("Use Default Renderer")) {
                const auto result = runtime_.restore_default_renderer_pipeline();
                feedback_ = result ? "Default renderer restored." : result.message;
            }
            ImGui::EndDisabled();
        }
        const auto* plan = selected_source.plan.get();
        active_source_ = selected_source.name;
        authoring_source_ = authored_source;
        authored_document_path_ = authored_source ? selected_source.authored_document->path : std::filesystem::path{};
        const bool render_state_graph = !authored_source && details_ && selected_source.name == "Game / Renderer";
        std::string state_override_signature;
        if (render_state_graph) {
            for (const auto& override : runtime_.render_state_link_overrides()) {
                state_override_signature += ':' + std::to_string(override.source_node) + '>' +
                                            std::to_string(override.destination_node) +
                                            (override.connected ? '+' : '-');
            }
        }
        const std::vector<aeg::render_graph::RenderStateLinkOverride> state_overrides =
            !authored_source && active_source_ == "Game / Renderer"
                ? runtime_.render_state_link_overrides()
                : std::vector<aeg::render_graph::RenderStateLinkOverride>{};
        const std::vector<aeg::render_graph::RenderStateBlendOverride> blend_overrides =
            render_state_graph ? runtime_.render_state_blend_overrides()
                               : std::vector<aeg::render_graph::RenderStateBlendOverride>{};
        for (const auto& override : blend_overrides) {
            state_override_signature +=
                ':' + std::to_string(override.node) + '=' + std::to_string(static_cast<uint32_t>(override.blend_mode));
        }
        const std::string signature =
            authored_source
                ? selected_source.name + ":" +
                      aeg::pipeline::document_semantic_digest(selected_source.authored_document->document)
                : selected_source.name + std::to_string(plan->pipeline.value()) + ":" + std::to_string(plan->revision) +
                      (details_ ? ":details" : ":overview") + state_override_signature;
        if (signature != signature_) {
            if (authored_source)
                rebuild_authored(selected_source.authored_document->document, runtime_.authored_pipeline_catalog());
            else
                rebuild(*plan, state_overrides, blend_overrides, render_state_graph);
            signature_ = signature;
        }
        ImGui::TextDisabled("%zu nodes / %zu features   |   %s   |   Wheel: zoom / middle-drag: pan", nodes_.size(),
                            authored_source ? selected_source.authored_document->document.feature_nodes.size()
                                            : plan->feature_implementations.size(),
                            authored_source ? "Authored project Pipeline; type declarations load from Source metadata"
                            : details_      ? "Renderer links edit render state; save to apply changes"
                                            : "Switch to Details to edit saved ordering");
        if (drafts_.dirty())
            ImGui::TextDisabled("%zu pipeline change(s) pending — Save Changes to apply.", drafts_.changes().size());
        if (!pending_blends_.empty())
            ImGui::TextDisabled("%zu state blend change(s) pending — Save Changes to apply.", pending_blends_.size());
        if (!feedback_.empty())
            ImGui::TextWrapped("%s", feedback_.c_str());
        const float content_height = std::max(1.0f, ImGui::GetContentRegionAvail().y);
        const float vertical_spacing = ImGui::GetStyle().ItemSpacing.y;
        const float minimum_validation_height = std::min(72.0f, std::max(24.0f, content_height * 0.35f));
        const float maximum_validation_height =
            std::max(minimum_validation_height, content_height - vertical_spacing - 1.0f);
        const float validation_height =
            std::clamp(content_height * 0.22f, minimum_validation_height, maximum_validation_height);
        const float body_height = std::max(1.0f, content_height - validation_height - vertical_spacing);
        const bool authoring_ui = ImGui::GetContentRegionAvail().x >= 850.0f;
        const bool table_open =
            authoring_ui && ImGui::BeginTable("PipelineAuthoringLayout", 3,
                                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
                                              ImVec2{0, body_height});
        if (table_open) {
            ImGui::TableSetupColumn("Catalog", ImGuiTableColumnFlags_WidthFixed, 270.0f);
            ImGui::TableSetupColumn("Graph", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthFixed, 320.0f);
            ImGui::TableNextColumn();
            ImGui::BeginChild("PipelineNodeCatalog", ImVec2{0, body_height}, ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_None);
            ImGui::TextUnformatted("Node Catalog");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##pipeline-node-search", "Search types... (Ctrl+K)", catalog_search_,
                                     sizeof(catalog_search_));
            const std::array<const char*, 6> catalog_filters{"All", "Pipeline", "Stage", "Feature", "Pass", "Util"};
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4.0f, 3.0f});
            for (int filter = 0; filter < static_cast<int>(catalog_filters.size()); ++filter) {
                if (filter != 0)
                    ImGui::SameLine();
                const bool filter_active = filter == catalog_filter_;
                if (filter_active)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.10f, 0.35f, 0.31f, 1.0f});
                if (ImGui::SmallButton(catalog_filters[filter]))
                    catalog_filter_ = filter;
                if (filter_active)
                    ImGui::PopStyleColor();
            }
            ImGui::PopStyleVar();
            ImGui::Separator();
            struct CatalogItem {
                std::string name;
                std::string detail;
            };
            struct CatalogGroup {
                const char* name;
                std::vector<CatalogItem> items;
            };
            std::array<CatalogGroup, 5> groups{
                {{"Pipeline", {}}, {"Stage", {}}, {"Feature", {}}, {"Pass", {}}, {"Util", {}}}};
            const auto add_catalog_item = [](std::vector<CatalogItem>& items, std::string name, std::string detail) {
                if (name.empty())
                    return;
                const auto found = std::find_if(items.begin(), items.end(),
                                                [&name](const CatalogItem& item) { return item.name == name; });
                if (found == items.end())
                    items.push_back(CatalogItem{std::move(name), std::move(detail)});
            };
            for (const auto& source : sources)
                add_catalog_item(groups[0].items, source.name,
                                 source.authored_document ? "Authored .aegpipe" : "Pipeline source");
            if (authored_source) {
                const auto present_consumer = aeg::application::required_pipeline_present_consumer();
                add_catalog_item(groups[3].items, std::string(present_consumer.title), "Required display consumer");
                for (const auto& entry : runtime_.authored_pipeline_catalog().entries()) {
                    const std::size_t group = entry.key.kind == aeg::pipeline::AuthoringTypeKind::kPipeline  ? 0U
                                              : entry.key.kind == aeg::pipeline::AuthoringTypeKind::kFeature ? 2U
                                                                                                             : 3U;
                    const std::string& title = entry.display_name.empty() ? entry.name : entry.display_name;
                    add_catalog_item(groups[group].items, title, "Project module declaration");
                    if (entry.key.kind != aeg::pipeline::AuthoringTypeKind::kPipeline)
                        continue;
                    if (const auto* declaration =
                            std::get_if<aeg::pipeline::PipelineAuthoringDeclaration>(&entry.declaration)) {
                        for (const auto& stage : declaration->stages)
                            add_catalog_item(groups[1].items, stage.name, "Project Pipeline stage");
                    }
                }
            } else {
                std::unordered_set<uint64_t> stage_ids;
                std::unordered_set<uint64_t> task_ids;
                for (const auto& plan_node : plan->nodes) {
                    const uint64_t stage_id = plan_node.stage.value();
                    if (stage_ids.insert(stage_id).second) {
                        add_catalog_item(
                            groups[1].items,
                            std::string(phase_name(plan_node.phase)) + " / Stage " + hex(stage_id).substr(8),
                            std::string("Phase ") + phase_name(plan_node.phase));
                    }
                    const uint64_t task_id = plan_node.task.value();
                    if (task_ids.insert(task_id).second)
                        add_catalog_item(groups[3].items, "Task " + hex(task_id).substr(8),
                                         std::string("Pass task / ") + phase_name(plan_node.phase));
                }
                for (const auto& implementation : plan->feature_implementations) {
                    if (implementation.implementation == nullptr)
                        continue;
                    const auto& descriptor = implementation.implementation->descriptor();
                    add_catalog_item(groups[2].items, std::string(descriptor.name), "Registered feature");
                }
                for (const auto& contribution : plan->feature_contributions) {
                    if (contribution.domain == aeg::pipeline::FeatureDomain::kRender)
                        add_catalog_item(groups[2].items, "Feature " + hex(contribution.feature.value()).substr(8),
                                         "Render contribution");
                }
                for (const auto& artifact : plan->artifacts)
                    add_catalog_item(groups[4].items, "Artifact " + hex(artifact.id.value()).substr(8),
                                     artifact.external_input ? "External input" : "Pipeline artifact");
            }
            const auto contains_search = [this](const CatalogItem& item) {
                return catalog_search_[0] == '\0' || item.name.find(catalog_search_) != std::string::npos ||
                       item.detail.find(catalog_search_) != std::string::npos;
            };
            for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
                if (catalog_filter_ != 0 && catalog_filter_ != static_cast<int>(group_index + 1))
                    continue;
                const auto& group = groups[group_index];
                int visible = 0;
                for (const auto& item : group.items)
                    visible += contains_search(item) ? 1 : 0;
                if (visible == 0 && catalog_search_[0] != '\0')
                    continue;
                ImGui::PushID(static_cast<int>(group_index));
                const std::string group_label =
                    std::string("v  ") + group.name + " (" + std::to_string(group.items.size()) + ")";
                if (ImGui::CollapsingHeader(group_label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (group.items.empty()) {
                        ImGui::TextDisabled("No %s registered in this pipeline.", group.name);
                    }
                    for (const auto& item : group.items) {
                        if (!contains_search(item))
                            continue;
                        ImGui::PushID(item.name.c_str());
                        ImGui::TextColored(group_index == 2 ? ImVec4{0.42f, 0.88f, 0.56f, 1.0f}
                                                            : (group_index >= 3 ? ImVec4{0.65f, 0.48f, 0.90f, 1.0f}
                                                                                : ImVec4{0.30f, 0.70f, 0.90f, 1.0f}),
                                           group_index == 0 ? ">" : (group_index == 1 ? "-" : "+"));
                        ImGui::SameLine();
                        if (ImGui::Selectable(item.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                            const auto found = std::find_if(nodes_.begin(), nodes_.end(), [&item](const Node& node) {
                                return node.title == item.name;
                            });
                            if (found != nodes_.end())
                                selected_ = static_cast<int>(std::distance(nodes_.begin(), found));
                        }
                        ImGui::TextDisabled("%s", item.detail.c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::TableNextColumn();
        }
        const float graph_height = table_open ? body_height : std::max(1.0f, ImGui::GetContentRegionAvail().y);
        ImGui::BeginChild("PipelineGraph", ImVec2{0, graph_height}, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x < 80 || size.y < 40) {
            ImGui::EndChild();
            if (table_open)
                ImGui::EndTable();
            ImGui::End();
            return;
        }
        const float font = ImGui::GetFontSize();
        const auto card_layout = aeg::application::PipelineNodeCardLayout::for_font_size(font);
        const float old_width = width_, old_height = height_;
        width_ = 280;
        height_ = card_layout.height;
        for (const auto& node : nodes_)
            width_ = std::max(width_, ImGui::CalcTextSize(node.title.c_str()).x + 58);
        if (old_width != width_ || old_height != height_) {
            arrange();
            if (!authored_source)
                restore_layout(*plan);
            fit_ = true;
        }
        if (std::abs(size.x - last_size_.x) > 1 || std::abs(size.y - last_size_.y) > 1)
            fit_ = true;
        last_size_ = size;
        if (fit_) {
            // Saved layouts can contain coordinates from a wider workspace. Normalize
            // them into the current canvas before fitting so the complete graph is visible.
            float min_x = 0.0f, min_y = 0.0f;
            for (const auto& node : nodes_) {
                min_x = std::min(min_x, node.pos.x);
                min_y = std::min(min_y, node.pos.y);
            }
            if (min_x < 0.0f || min_y < 0.0f) {
                for (auto& node : nodes_)
                    node.pos = {node.pos.x - min_x, node.pos.y - min_y};
            }
            float max_x = width_, max_y = height_;
            for (const auto& node : nodes_) {
                max_x = std::max(max_x, node.pos.x + width_);
                max_y = std::max(max_y, node.pos.y + height_);
            }
            // Keep graph content at a readable scale while resizing.  A narrow
            // viewport may show only part of the graph; it should not shrink the
            // nodes and labels until the whole graph becomes unreadable.
            zoom_ = std::clamp(std::min((size.x - 40) / max_x, (size.y - 30) / max_y), 0.65f, 1.0f);
            offset_ = {std::max(8.0f, (size.x - max_x * zoom_) * 0.5f),
                       std::max(8.0f, (size.y - max_y * zoom_) * 0.5f)};
            fit_ = false;
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* canvas_draw = ImGui::GetWindowDrawList();
        // Reserve the complete graph surface as one ImGui item so the canvas
        // keeps receiving mouse gestures while the graph is redrawn with the
        // draw list below. Overlay controls are submitted after the canvas
        // item and therefore remain clickable.
        ImGui::InvisibleButton(
            "##GraphCanvas", size,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        const ImU32 grid_color = IM_COL32(25, 58, 70, 72);
        for (float x = std::fmod(offset_.x, 32.0f * zoom_); x < size.x; x += 32.0f * zoom_)
            canvas_draw->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + size.y}, grid_color);
        for (float y = std::fmod(offset_.y, 32.0f * zoom_); y < size.y; y += 32.0f * zoom_)
            canvas_draw->AddLine({origin.x, origin.y + y}, {origin.x + size.x, origin.y + y}, grid_color);
        const std::string canvas_title =
            authored_source ? "Pipeline  >  " + active_source_
                            : (details_ ? "Pipeline  >  RenderPipeline" : "Pipeline  >  Execution Overview");
        canvas_draw->AddText({origin.x + 12.0f, origin.y + 10.0f}, IM_COL32(139, 185, 194, 235), canvas_title.c_str());
        const ImVec2 zoom_min{origin.x + size.x - 92.0f, origin.y + 10.0f};
        const auto screen = [&](ImVec2 p) {
            return ImVec2{origin.x + offset_.x + p.x * zoom_, origin.y + offset_.y + p.y * zoom_};
        };
        const auto& io = ImGui::GetIO();
        const ImVec2 mouse_pos = ImGui::GetMousePos();
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hovered && io.MouseWheel != 0) {
            const float next = std::clamp(zoom_ + io.MouseWheel * 0.08f, 0.15f, 1.8f);
            const ImVec2 anchor{mouse_pos.x - origin.x, mouse_pos.y - origin.y};
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
            const float dx = mouse_pos.x - (p.x + width_ * zoom_), dy = mouse_pos.y - (p.y + height_ * zoom_ * 0.5f);
            const bool has_state_input = std::any_of(edges_.begin(), edges_.end(),
                                                     [i](const Edge& edge) { return edge.to == static_cast<int>(i); });
            if (hovered && dx * dx + dy * dy < 100.0f &&
                (!render_state_graph ||
                 (nodes_[i].phase != aeg::pipeline::EnginePhase::kPresent &&
                  aeg::application::can_start_render_state_link(nodes_[i].writes_render_state, has_state_input)))) {
                port = static_cast<int>(i);
            }
            if (hovered && mouse_pos.x >= p.x && mouse_pos.x <= p.x + width_ * zoom_ && mouse_pos.y >= p.y &&
                mouse_pos.y <= p.y + height_ * zoom_)
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
                                                     screen({to.pos.x, to.pos.y + height_ * 0.5f}), mouse_pos, zoom_)) {
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
            if (authoring_source_) {
                if (!nodes_[edge.to].display_terminal || nodes_[edge.from].authoring_node_id.empty()) {
                    feedback_ = "Only Present display connections can be changed in an authored project Pipeline.";
                } else if (const Result result = runtime_.set_authored_present_connection(
                               authored_document_path_, nodes_[edge.from].authoring_node_id, false);
                           !result) {
                    feedback_ = result.message;
                } else {
                    edges_.erase(edges_.begin() + double_clicked_edge);
                    feedback_ = "Present connection removed from the .aegpipe document.";
                    signature_.clear();
                }
            } else {
                drafts_.stage_remove(active_source_, nodes_[edge.from].plan_node, nodes_[edge.to].plan_node);
                edges_.erase(edges_.begin() + double_clicked_edge);
                feedback_ = "Connection removal is pending. Save Changes to apply it.";
            }
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
                select_node(hit);
                dragging_ = hit;
            }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit >= 0) {
            select_node(hit);
            const bool opens_render_state_menu = active_source_ == "Game / Renderer" && nodes_[hit].writes_render_state;
            if (!opens_render_state_menu)
                ImGui::OpenPopup("##PipelineNodeActions");
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
                constexpr std::array<const char*, 3> kNames{"Replace", "Over", "Add"};
                for (uint32_t index = 0; index < kNames.size(); ++index) {
                    if (ImGui::MenuItem(kNames[index], nullptr, node.blend_mode == index)) {
                        stage_blend(node, index);
                    }
                }
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##PipelineNodeActions")) {
            if (selected_ >= 0 && selected_ < static_cast<int>(nodes_.size())) {
                const Node& node = nodes_[selected_];
                if (ImGui::MenuItem("Copy Node")) {
                    ImGui::SetClipboardText(
                        aeg::application::pipeline_node_copy_text(node.title, node.subtitle, node.detail).c_str());
                    feedback_ = "Node copied to the clipboard.";
                }
                if (ImGui::MenuItem("Copy Name")) {
                    ImGui::SetClipboardText(node.title.c_str());
                    feedback_ = "Node name copied to the clipboard.";
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
        const bool show_pipeline_root =
            details_ && !render_state_graph && !authoring_source_ && !nodes_.empty() && size.y > 300.0f;
        ImVec2 pipeline_root{};
        if (show_pipeline_root) {
            const float root_width = 260.0f;
            const float root_height = 82.0f;
            pipeline_root = screen({std::max(16.0f, (size.x / zoom_ - root_width) * 0.5f), 20.0f});
            const ImVec2 root_max{pipeline_root.x + root_width * zoom_, pipeline_root.y + root_height * zoom_};
            draw->AddRectFilled(pipeline_root, root_max, IM_COL32(12, 58, 61, 245), 7.0f);
            draw->AddRect(pipeline_root, root_max, IM_COL32(41, 225, 211, 255), 7.0f, 0, 2.0f);
            draw->AddRectFilled({pipeline_root.x + 12.0f * zoom_, pipeline_root.y + 16.0f * zoom_},
                                {pipeline_root.x + 32.0f * zoom_, pipeline_root.y + 36.0f * zoom_},
                                IM_COL32(37, 204, 192, 255), 4.0f);
            draw->AddText(ImGui::GetFont(), font * zoom_,
                          {pipeline_root.x + 44.0f * zoom_, pipeline_root.y + 13.0f * zoom_},
                          IM_COL32(239, 255, 255, 255), "RenderPipeline");
            draw->AddText(ImGui::GetFont(), font * zoom_,
                          {pipeline_root.x + 44.0f * zoom_, pipeline_root.y + 34.0f * zoom_},
                          IM_COL32(150, 204, 208, 255), active_source_.c_str());
            draw->AddText(ImGui::GetFont(), font * zoom_, {root_max.x - 86.0f * zoom_, pipeline_root.y + 14.0f * zoom_},
                          IM_COL32(150, 204, 208, 255), "Pipeline");
            draw->AddText(ImGui::GetFont(), font * zoom_,
                          {pipeline_root.x + 14.0f * zoom_, pipeline_root.y + 61.0f * zoom_},
                          IM_COL32(181, 230, 229, 255), "In   RenderGraph    Out   Present");
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                const bool has_input = std::any_of(edges_.begin(), edges_.end(),
                                                   [i](const Edge& edge) { return edge.to == static_cast<int>(i); });
                if (has_input)
                    continue;
                const ImVec2 target = screen({nodes_[i].pos.x + width_ * 0.5f, nodes_[i].pos.y});
                const ImVec2 start{pipeline_root.x + root_width * zoom_ * 0.5f, root_max.y};
                const float bend = std::max(26.0f * zoom_, std::abs(target.y - start.y) * 0.35f);
                draw->AddBezierCubic(start, {start.x, start.y + bend}, {target.x, target.y - bend}, target,
                                     IM_COL32(77, 210, 205, 215), 1.8f);
                draw->AddCircleFilled(target, 3.5f * zoom_, IM_COL32(77, 210, 205, 255));
            }
        }
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
            link(screen({node.pos.x + width_, node.pos.y + height_ * 0.5f}), mouse_pos, IM_COL32(240, 182, 106, 255));
        }
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const auto& node = nodes_[i];
            const auto p = screen(node.pos);
            ImFont* card_font = const_cast<ImFont*>(static_cast<const ImFont*>(imgui_host_.display_font()));
            if (card_font == nullptr)
                card_font = ImGui::GetFont();
            const float card_text_size = aeg::application::PipelineNodeCardLayout::text_size(font, zoom_);
            const bool active_output = node.state.reaches_output;
            const bool pass_node = node.phase == aeg::pipeline::EnginePhase::kPresent;
            const ImU32 category = node.render_contribution >= 0
                                       ? IM_COL32(104, 214, 166, 255)
                                       : (pass_node ? IM_COL32(176, 133, 230, 255) : IM_COL32(100, 173, 236, 255));
            const ImU32 border =
                i == selected_ ? IM_COL32(115, 230, 195, 255) : (active_output ? category : IM_COL32(48, 78, 91, 150));
            draw->AddRectFilled(p, {p.x + width_ * zoom_, p.y + height_ * zoom_},
                                i == selected_
                                    ? IM_COL32(23, 58, 53, 255)
                                    : (active_output ? IM_COL32(17, 43, 53, 255) : IM_COL32(15, 28, 37, 220)),
                                6);
            draw->AddRect(p, {p.x + width_ * zoom_, p.y + height_ * zoom_}, border, 6);
            draw->AddRectFilled(p, {p.x + 5.0f * zoom_, p.y + height_ * zoom_ - 5.0f * zoom_}, category, 2.0f);
            draw->AddRectFilled({p.x + 12.0f * zoom_, p.y + 12.0f * zoom_}, {p.x + 24.0f * zoom_, p.y + 24.0f * zoom_},
                                category, 3.0f);
            // Node labels are draw-list text, so they must explicitly inherit
            // the card clip rect. Long project-defined names otherwise paint
            // through the card border and into adjacent nodes.
            draw->PushClipRect({p.x + 7.0f * zoom_, p.y + 7.0f * zoom_},
                               {p.x + (width_ - 7.0f) * zoom_, p.y + (height_ - 7.0f) * zoom_}, true);
            const auto text = [&](float y, const char* value, ImU32 color) {
                draw->AddText(card_font, card_text_size, {p.x + 34.0f * zoom_, p.y + y * zoom_}, color, value);
            };
            text(card_layout.title_y, node.title.c_str(), IM_COL32(235, 245, 246, 255));
            text(card_layout.subtitle_y, node.subtitle.c_str(), IM_COL32(145, 179, 191, 255));
            text(card_layout.detail_y, node.detail.c_str(), IM_COL32(115, 149, 162, 255));
            draw->AddLine({p.x + 12.0f * zoom_, p.y + card_layout.divider_y * zoom_},
                          {p.x + (width_ - 12.0f) * zoom_, p.y + card_layout.divider_y * zoom_},
                          IM_COL32(53, 101, 112, 180), 1.0f);
            const char* type = node.display_terminal
                                   ? "Present"
                                   : (node.render_contribution >= 0 ? "Feature" : (pass_node ? "Pass" : "Stage"));
            draw->AddText(card_font, card_text_size, {p.x + 34.0f * zoom_, p.y + card_layout.footer_y * zoom_},
                          category, type);
            const std::string id = "0x" + hex(node.plan_node).substr(10);
            draw->AddText(card_font, card_text_size,
                          {p.x + (width_ - 92.0f) * zoom_, p.y + card_layout.footer_y * zoom_},
                          IM_COL32(126, 172, 181, 230), id.c_str());
            draw->PopClipRect();
            for (float x : {p.x, p.x + width_ * zoom_}) {
                draw->AddCircleFilled({x, p.y + height_ * zoom_ * 0.5f}, 4, IM_COL32(10, 23, 32, 255));
                draw->AddCircle({x, p.y + height_ * zoom_ * 0.5f}, 4, border, 12, 1.5f);
            }
        }
        const ImVec2 map_min{origin.x + size.x - 166.0f, origin.y + 46.0f};
        const ImVec2 map_max{map_min.x + 154.0f, map_min.y + 92.0f};
        draw->AddRectFilled(map_min, map_max, IM_COL32(7, 20, 28, 225), 4.0f);
        draw->AddRect(map_min, map_max, IM_COL32(62, 118, 126, 210), 4.0f);
        float map_min_x = 0.0f, map_min_y = 0.0f, map_max_x = width_, map_max_y = height_;
        for (const auto& node : nodes_) {
            map_min_x = std::min(map_min_x, node.pos.x);
            map_min_y = std::min(map_min_y, node.pos.y);
            map_max_x = std::max(map_max_x, node.pos.x + width_);
            map_max_y = std::max(map_max_y, node.pos.y + height_);
        }
        const float map_scale =
            std::min(140.0f / std::max(map_max_x - map_min_x, 1.0f), 76.0f / std::max(map_max_y - map_min_y, 1.0f));
        const ImVec2 map_content_min{map_min.x + 7.0f, map_min.y + 7.0f};
        const ImVec2 map_content_max{map_max.x - 7.0f, map_max.y - 7.0f};
        draw->PushClipRect(map_content_min, map_content_max, true);
        for (const auto& node : nodes_) {
            const ImVec2 mini{map_content_min.x + (node.pos.x - map_min_x) * map_scale,
                              map_content_min.y + (node.pos.y - map_min_y) * map_scale};
            draw->AddRectFilled(
                mini, {mini.x + std::max(4.0f, width_ * map_scale), mini.y + std::max(3.0f, height_ * map_scale)},
                IM_COL32(82, 170, 172, 210), 1.5f);
        }
        draw->PopClipRect();

        // Use ImGui widgets for zoom controls so hover, keyboard navigation,
        // focus and hit testing follow the library instead of a hand-drawn
        // decorative control.
        ImGui::SetCursorScreenPos(zoom_min);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4.0f, 2.0f});
        if (ImGui::Button("-", ImVec2{24.0f, 24.0f})) {
            const float next = std::clamp(zoom_ - 0.08f, 0.15f, 1.8f);
            const ImVec2 anchor{size.x * 0.5f, size.y * 0.5f};
            offset_ = {anchor.x - (anchor.x - offset_.x) * next / zoom_,
                       anchor.y - (anchor.y - offset_.y) * next / zoom_};
            zoom_ = next;
        }
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Text("%d%%", static_cast<int>(std::lround(zoom_ * 100.0f)));
        ImGui::SameLine(0.0f, 0.0f);
        if (ImGui::Button("+", ImVec2{24.0f, 24.0f})) {
            const float next = std::clamp(zoom_ + 0.08f, 0.15f, 1.8f);
            const ImVec2 anchor{size.x * 0.5f, size.y * 0.5f};
            offset_ = {anchor.x - (anchor.x - offset_.x) * next / zoom_,
                       anchor.y - (anchor.y - offset_.y) * next / zoom_};
            zoom_ = next;
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        if (table_open) {
            ImGui::TableNextColumn();
            ImGui::BeginChild("PipelineNodeInspector", ImVec2{0, graph_height}, ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_None);
            ImGui::TextUnformatted("Inspector");
            ImGui::SameLine();
            if (ImGui::SmallButton(inspector_metadata_ ? "Inspector" : "Metadata"))
                inspector_metadata_ = !inspector_metadata_;
            ImGui::Separator();
            if (inspector_metadata_ && selected_ >= 0 && selected_ < static_cast<int>(nodes_.size())) {
                const auto& node = nodes_[selected_];
                ImGui::TextColored(ImVec4{0.45f, 0.90f, 0.76f, 1.0f}, "Node Metadata");
                ImGui::Separator();
                ImGui::Text("Plan node  %u", node.plan_node);
                ImGui::Text("Pipeline   %s", active_source_.c_str());
                ImGui::Text("Phase      %s", phase_name(node.phase));
                ImGui::TextWrapped("Stable module metadata is resolved when an authoring document is loaded.");
            } else if (selected_ >= 0 && selected_ < static_cast<int>(nodes_.size())) {
                const auto& node = nodes_[selected_];
                const int input_count = static_cast<int>(std::count_if(
                    edges_.begin(), edges_.end(), [this](const Edge& edge) { return edge.to == selected_; }));
                const int output_count = static_cast<int>(std::count_if(
                    edges_.begin(), edges_.end(), [this](const Edge& edge) { return edge.from == selected_; }));
                const ImVec4 accent =
                    node.render_contribution >= 0
                        ? ImVec4{0.35f, 0.86f, 0.66f, 1.0f}
                        : (node.phase == aeg::pipeline::EnginePhase::kPresent ? ImVec4{0.67f, 0.50f, 0.90f, 1.0f}
                                                                              : ImVec4{0.38f, 0.68f, 0.94f, 1.0f});
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{5.0f, 3.0f});
                ImGui::ColorButton("##inspector_node_color", accent,
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                   ImVec2{16.0f, 16.0f});
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextColored(accent, "%s", node.title.c_str());
                ImGui::TextDisabled("%s", node.subtitle.c_str());
                ImGui::EndGroup();
                ImGui::SameLine();
                ImGui::TextDisabled("  %d in / %d out", input_count, output_count);
                ImGui::Separator();

                const auto read_only_field = [](const char* label, const char* id, const std::string& value) {
                    ImGui::TextDisabled("%s", label);
                    ImGui::SameLine(88.0f);
                    char buffer[192]{};
                    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
                    ImGui::PushID(id);
                    ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputText("##value", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                };
                ImGui::TextUnformatted("Node properties");
                read_only_field("Name", "name", node.title);
                read_only_field("Type", "type", node.subtitle);
                read_only_field("TypeId", "type_id", "0x" + hex(node.plan_node).substr(10) + "  (stable)");
                read_only_field("Category", "category",
                                node.phase == aeg::pipeline::EnginePhase::kPresent
                                    ? "Rendering / Present"
                                    : (node.render_contribution >= 0 ? "Rendering / Feature" : "Rendering / Stage"));
                read_only_field("Module", "module", "AEG.Render");
                read_only_field("Version", "version", "Runtime plan");
                ImGui::Separator();
                ImGui::TextUnformatted("Description");
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", node.detail.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Separator();
                ImGui::TextUnformatted("Parameters");
                ImGui::BeginDisabled();
                const char* execution = node.phase == aeg::pipeline::EnginePhase::kPresent ? "Present" : "Execute";
                if (ImGui::BeginCombo("Operator", execution)) {
                    ImGui::Selectable("Execute", std::strcmp(execution, "Execute") == 0);
                    ImGui::Selectable("Present", std::strcmp(execution, "Present") == 0);
                    ImGui::EndCombo();
                }
                if (ImGui::BeginCombo("Execution Domain", "Main thread")) {
                    ImGui::Selectable("Main thread", true);
                    ImGui::Selectable("Worker thread", false);
                    ImGui::EndCombo();
                }
                float priority = node.state.reaches_output ? 1.0f : 0.5f;
                ImGui::SliderFloat("Priority", &priority, 0.0f, 1.0f, "%.2f");
                bool reaches_present = node.state.reaches_output;
                ImGui::Checkbox("Reaches Present", &reaches_present);
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextUnformatted("Inputs");
                ImGui::BeginDisabled();
                char input_buffer[64]{};
                std::snprintf(input_buffer, sizeof(input_buffer), "%d connection(s)", input_count);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##inputs", input_buffer, sizeof(input_buffer), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndDisabled();
                ImGui::TextUnformatted("Outputs");
                ImGui::BeginDisabled();
                char output_buffer[64]{};
                std::snprintf(output_buffer, sizeof(output_buffer), "%d connection(s)", output_count);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##outputs", output_buffer, sizeof(output_buffer), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextUnformatted("Tags");
                ImGui::BeginDisabled();
                ImGui::SameLine();
                ImGui::SmallButton(node.render_contribution >= 0 ? "Feature" : "Stage");
                ImGui::SameLine();
                ImGui::SmallButton(node.phase == aeg::pipeline::EnginePhase::kPresent ? "Present" : "Runtime");
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextUnformatted("Notes");
                ImGui::TextDisabled("Live values are read from the active project pipeline plan.");
                ImGui::PopStyleVar();
            } else {
                ImGui::TextDisabled("Select a node to inspect its execution details.");
            }
            ImGui::EndChild();
            ImGui::EndTable();
            ImGui::BeginChild("PipelineValidation", ImVec2{0, validation_height}, true);
            const bool has_links = !edges_.empty();
            const auto renderer_diagnostic = runtime_.renderer_pipeline_diagnostic();
            const int issue_count = (has_links ? 0 : 1) + (renderer_diagnostic.empty() ? 0 : 1);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.10f, 0.26f, 0.27f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.14f, 0.34f, 0.34f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.18f, 0.40f, 0.38f, 1.0f});
            ImGui::SmallButton(("Validation (" + std::to_string(issue_count) + ")").c_str());
            ImGui::SameLine();
            ImGui::SmallButton("Build Output");
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::SmallButton("Clear");
            ImGui::SameLine(ImGui::GetWindowWidth() - 230.0f);
            ImGui::TextColored(ImVec4{0.42f, 0.90f, 0.76f, 1.0f}, "OK  %d Errors", has_links ? 0 : 0);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{1.0f, 0.72f, 0.30f, 1.0f}, "!  %d Warnings", issue_count);
            ImGui::Separator();
            if (!renderer_diagnostic.empty())
                ImGui::TextWrapped("%s", std::string(renderer_diagnostic).c_str());
            if (ImGui::BeginTable(
                    "PipelineValidationTable", 4,
                    ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthFixed, 190.0f);
                ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableHeadersRow();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(has_links ? ImVec4{0.45f, 0.90f, 0.76f, 1.0f} : ImVec4{1.0f, 0.72f, 0.30f, 1.0f},
                                   has_links ? "OK" : "Warning");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(has_links && selected_ >= 0 ? nodes_[selected_].title.c_str()
                                                                   : "Pipeline graph");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(has_links ? "Connected execution paths" : "No graph links detected");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Live plan");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4{0.42f, 0.75f, 0.90f, 1.0f}, "Info");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Pipeline authoring");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("TypeId, ports and render-state reachability checked on save");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%zu pending", drafts_.changes().size() + pending_blends_.size());
                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

private:
    void draw_inline_aeg_chrome(bool* open) {
        const ImVec2 window_pos = ImGui::GetWindowPos();
        const float window_width = ImGui::GetWindowWidth();
        const ImVec2 chrome_min = window_pos;
        const ImVec2 chrome_max{window_pos.x + window_width, window_pos.y + 30.0f};
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(chrome_min, chrome_max, IM_COL32(4, 22, 29, 255));
        draw->AddLine(ImVec2{chrome_min.x, chrome_max.y - 1.0f}, ImVec2{chrome_max.x, chrome_max.y - 1.0f},
                      IM_COL32(28, 83, 92, 255));
        draw->AddText(ImVec2{chrome_min.x + 12.0f, chrome_min.y + 7.0f}, IM_COL32(223, 240, 242, 255),
                      "AEG | Pipeline");

        const ImVec2 close_pos{chrome_max.x - 36.0f, chrome_min.y + 3.0f};
        ImGui::SetCursorScreenPos(close_pos);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{0.0f, 0.0f});
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.016f, 0.086f, 0.114f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.745f, 0.204f, 0.220f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.62f, 0.14f, 0.16f, 1.0f});
        const bool close_pressed = ImGui::Button("×", ImVec2{30.0f, 24.0f});
        const bool close_hovered = ImGui::IsItemHovered();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (close_hovered)
            ImGui::SetTooltip("Close Pipeline");
        if (close_pressed && open != nullptr)
            *open = false;

        // The chrome is an overlay drawn in the same ImGui window. Restore the
        // layout cursor explicitly so toolbar and graph content start below
        // the 30 px strip on every frame and after every resize.
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::SetCursorPos(ImVec2{style.WindowPadding.x, 30.0f + style.WindowPadding.y});
    }

    struct Node {
        std::string title, subtitle, detail;
        aeg::pipeline::EnginePhase phase{aeg::pipeline::EnginePhase::kFrameBegin};
        aeg::application::PipelineGraphState state{};
        ImVec2 pos{};
        uint32_t plan_node{aeg::pipeline::kInvalidPipelineHandleIndex};
        int render_contribution{-1};
        bool writes_render_state{false};
        uint8_t blend_mode{0};
        bool display_terminal{false};
        std::string authoring_node_id{};
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
        constexpr const char* kNames[]{"Startup", "Frame Begin",  "Input",          "Pre Update",   "Fixed Update",
                                       "Update",  "World Commit", "Render Extract", "Render Build", "Render Execute",
                                       "Present", "Frame End",    "Shutdown"};
        const auto i = static_cast<unsigned>(phase);
        return i < 13 ? kNames[i] : "Custom Phase";
    }
    static const char* blend_name(uint8_t blend_mode) {
        constexpr std::array<const char*, 3> kNames{"Replace", "Over", "Add"};
        return blend_mode < kNames.size() ? kNames[blend_mode] : "Replace";
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
                    details_ ? (render_contribution >= 0
                                    ? "Feature"
                                    : (task.synthetic ? "Stage anchor" : "Task " + hex(task.task.value()).substr(8)))
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
        selected_ = -1;
        if (details_) {
            const auto feature = std::find_if(nodes_.begin(), nodes_.end(),
                                              [](const Node& node) { return node.render_contribution >= 0; });
            if (feature != nodes_.end())
                selected_ = static_cast<int>(std::distance(nodes_.begin(), feature));
        }
        if (selected_ < 0 && !nodes_.empty())
            selected_ = 0;
        arrange();
        restore_layout(plan);
        fit_ = true;
    }
    void rebuild_authored(const aeg::pipeline::PipelineDocument& document, const aeg::pipeline::TypeCatalog& catalog) {
        nodes_.clear();
        edges_.clear();
        connecting_ = dragging_ = -1;
        feedback_.clear();
        std::unordered_map<std::string, int> indices;
        const auto display_name = [&catalog](aeg::pipeline::AuthoringTypeKind kind, uint64_t id,
                                             std::string_view fallback) {
            const auto* entry = catalog.find({kind, id});
            if (entry == nullptr)
                return std::string(fallback) + " (missing type)";
            return entry->display_name.empty() ? entry->name : entry->display_name;
        };
        const auto append = [&indices, this](std::string node_id, std::string title, std::string subtitle,
                                             std::string detail, aeg::pipeline::EnginePhase phase,
                                             bool display_terminal = false) {
            const int index = static_cast<int>(nodes_.size());
            indices.emplace(node_id, index);
            Node node{};
            node.title = std::move(title);
            node.subtitle = std::move(subtitle);
            node.detail = std::move(detail);
            node.phase = phase;
            node.state = {true, 0, 0};
            node.display_terminal = display_terminal;
            node.authoring_node_id = std::move(node_id);
            nodes_.push_back(std::move(node));
        };
        for (const auto& pipeline : document.pipeline_nodes) {
            append(pipeline.node_id,
                   display_name(aeg::pipeline::AuthoringTypeKind::kPipeline, pipeline.type.value(), pipeline.node_id),
                   "Pipeline", "Authored Pipeline node", aeg::pipeline::EnginePhase::kFrameBegin);
        }
        for (const auto& feature : document.feature_nodes) {
            append(feature.node_id,
                   display_name(aeg::pipeline::AuthoringTypeKind::kFeature, feature.type.value(), feature.node_id),
                   feature.enabled ? "Feature" : "Feature · disabled", "Authored Feature node",
                   aeg::pipeline::EnginePhase::kRenderBuild);
        }
        for (const auto& pass : document.pass_nodes) {
            append(pass.node_id,
                   display_name(aeg::pipeline::AuthoringTypeKind::kRenderPass, pass.type.value(), pass.node_id),
                   pass.enabled ? "Pass" : "Pass · disabled", "Authored Pass node",
                   aeg::pipeline::EnginePhase::kRenderExecute);
        }
        const auto present_consumer = aeg::application::required_pipeline_present_consumer();
        // Present is an editor-owned display/consumption terminal.  It is
        // intentionally absent from .aegpipe type metadata and receives no
        // implicit edge, so authors may leave it disconnected while still
        // seeing the output target in every project graph.
        append(std::string(present_consumer.node_id), std::string(present_consumer.title),
               std::string(present_consumer.subtitle), std::string(present_consumer.detail),
               aeg::pipeline::EnginePhase::kPresent, true);
        const auto add_edge = [this, &indices](std::string_view from, std::string_view to) {
            const auto source = indices.find(std::string(from));
            const auto destination = indices.find(std::string(to));
            if (source == indices.end() || destination == indices.end() || source->second == destination->second)
                return;
            if (std::none_of(edges_.begin(), edges_.end(), [&](const Edge& edge) {
                    return edge.from == source->second && edge.to == destination->second;
                })) {
                edges_.push_back({source->second, destination->second, false});
            }
        };
        for (const auto& feature : document.feature_nodes)
            add_edge(feature.pipeline_node, feature.node_id);
        for (const auto& pass : document.pass_nodes)
            add_edge(pass.feature_node, pass.node_id);
        for (const auto& source_node : document.present_connections)
            add_edge(source_node, present_consumer.node_id);
        selected_ = nodes_.empty() ? -1 : 0;
        arrange();
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
        if (nodes_.empty())
            return;
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
        // The editor-owned Present card is intentionally allowed to be
        // disconnected.  Keep that terminal visually after authored work
        // instead of treating its lack of an incoming edge as a root node.
        const int terminal_rank = *std::max_element(rank.begin(), rank.end()) + 1;
        for (std::size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].display_terminal)
                rank[i] = terminal_rank;
        std::vector<int> rows(nodes_.size() + 1, 0);
        const float detail_y_offset = details_ ? 120.0f : 0.0f;
        for (std::size_t i = 0; i < nodes_.size(); ++i)
            nodes_[i].pos = {rank[i] * (width_ + 100), detail_y_offset + rows[rank[i]]++ * (height_ + 48)};
        // The built-in editor plan contains several same-phase stage anchors.
        // Wrap each dependency rank into compact columns; retaining the
        // topological direction keeps the real fan-in/fan-out readable.
        if (active_source_ == "Built-in / Editor UI" && nodes_.size() > 6) {
            std::vector<int> lane_rows(nodes_.size() + 1, 0);
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                const int lane = lane_rows[rank[i]]++;
                nodes_[i].pos = {(rank[i] + lane / 4) * (width_ + 100), detail_y_offset + (lane % 4) * (height_ + 48)};
            }
        }
    }
    void select_node(int node_index) {
        if (node_index < 0 || node_index >= static_cast<int>(nodes_.size()))
            return;
        selected_ = node_index;
        PipelineInspectorSelection selection{nodes_[node_index].title, nodes_[node_index].subtitle};
        for (const auto& edge : edges_) {
            selection.inputs += edge.to == node_index;
            selection.outputs += edge.from == node_index;
        }
        pipeline_inspector_selection = std::move(selection);
    }
    void connect(int from, int to) {
        if (authoring_source_) {
            if (from < 0 || to < 0 || from >= static_cast<int>(nodes_.size()) ||
                to >= static_cast<int>(nodes_.size()) || nodes_[from].display_terminal ||
                !nodes_[to].display_terminal || nodes_[from].authoring_node_id.empty()) {
                feedback_ = "Connect an authored node to Present to create a display-consumer link.";
                return;
            }
            if (std::any_of(edges_.begin(), edges_.end(),
                            [from, to](const Edge& edge) { return edge.from == from && edge.to == to; })) {
                feedback_ = "This Present connection already exists.";
                return;
            }
            if (const Result result = runtime_.set_authored_present_connection(authored_document_path_,
                                                                               nodes_[from].authoring_node_id, true);
                !result) {
                feedback_ = result.message;
                return;
            }
            edges_.push_back({from, to, false});
            feedback_ = "Present connection saved to the .aegpipe document.";
            signature_.clear();
            return;
        }
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
    EditorImGuiHost& imgui_host_;
    EditorSceneRuntime& runtime_;
    aeg::application::PipelineOrderingDraftQueue& drafts_;
    aeg::application::PipelineLayoutSaveState& layouts_;
    std::function<Result()> save_changes_;
    bool visible_{true};
    std::function<void(bool)> visibility_changed_{};
    std::function<bool()> native_presentation_{};
    std::size_t source_{0};
    bool source_preferred_{false};
    bool details_{true}, fit_{true}, latched_{false}, expand_{false}, layout_dirty_{false}, blend_dirty_{false};
    bool authoring_source_{false};
    bool inspector_metadata_{false};
    int catalog_filter_{0};
    char catalog_search_[128]{};
    std::string signature_, feedback_, authored_sources_signature_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<PendingBlend> pending_blends_{};
    std::string active_source_{};
    std::filesystem::path authored_document_path_{};
    int selected_{-1}, connecting_{-1}, dragging_{-1}, blend_popup_node_{-1};
    aeg::application::GraphPanGesture pan_{};
    float width_{250}, height_{90}, zoom_{1};
    ImVec2 offset_{}, last_size_{};
};

class InspectorPanel final : public aeg::editor::EditorPanel {
public:
    explicit InspectorPanel(EditorSceneRuntime& runtime) noexcept : runtime_(runtime) {}

    void build_ui() override {
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
                const ComponentTypeId point_type(aeg::engine::components::kPointLightName);
                const ComponentTypeId spot_type(aeg::engine::components::kSpotLightName);
                const ComponentTypeId environment_type(aeg::engine::components::kSceneEnvironmentName);
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
                if (ImGui::MenuItem("Point Light", nullptr, false, single_selection && !has_common(point_type))) {
                    last_operation_ = runtime_.add_component(selected_entities, point_type);
                }
                if (ImGui::MenuItem("Spot Light", nullptr, false, single_selection && !has_common(spot_type))) {
                    last_operation_ = runtime_.add_component(selected_entities, spot_type);
                }
                if (ImGui::MenuItem("Scene Environment", nullptr, false,
                                    single_selection && !has_common(environment_type))) {
                    last_operation_ = runtime_.add_component(selected_entities, environment_type);
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
                        ((component == ComponentTypeId(aeg::engine::components::kDirectionalLightName) ||
                          component == ComponentTypeId(aeg::engine::components::kPointLightName) ||
                          component == ComponentTypeId(aeg::engine::components::kSpotLightName)) &&
                         field.name == "color");
                    switch (field.value_kind) {
                        case aeg::reflection::FieldValueKind::kFloat: {
                            float minimum = -std::numeric_limits<float>::max();
                            float maximum = std::numeric_limits<float>::max();
                            ImGuiSliderFlags flags = ImGuiSliderFlags_None;
                            if (normalized_color || field.name == "intensity" || field.name == "range" ||
                                field.name == "exposure") {
                                minimum = 0.0f;
                                maximum = normalized_color ? 1.0f : std::numeric_limits<float>::max();
                                flags = normalized_color ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
                            } else if (field.name == "gamma") {
                                minimum = 0.001f;
                                maximum = std::numeric_limits<float>::max();
                            } else if (field.name == "inner_cone_cosine" || field.name == "outer_cone_cosine") {
                                minimum = -1.0f;
                                maximum = 1.0f;
                                flags = ImGuiSliderFlags_AlwaysClamp;
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
                            if (component == ComponentTypeId(aeg::engine::components::kSceneEnvironmentName)) {
                                changed = ImGui::ColorEdit3("##value", &vector->x,
                                                            ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                            } else if (normalized_color) {
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

/** Presents the real project filesystem rather than a flattened asset database. */
class ProjectPanel final : public aeg::editor::EditorPanel {
public:
    ProjectPanel(EditorSceneRuntime& runtime, std::function<void()> close_project) noexcept
        : runtime_(runtime), close_project_(std::move(close_project)) {}

    void build_ui() override {
        ImGui::Begin("Project");
        if (!runtime_.project_loaded()) {
            ImGui::TextDisabled("No project is open.");
            ImGui::TextWrapped("Create or import a project to browse its folders and assets.");
            ImGui::End();
            return;
        }

        const std::filesystem::path root = runtime_.project_root();
        const std::string root_name = root.filename().empty() ? root.string() : root.filename().string();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.040f, 0.102f, 0.134f, 1.0f});
        ImGui::BeginChild("ProjectHeader", ImVec2{0.0f, 38.0f}, true, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPos(ImVec2{9.0f, 5.0f});
        ImGui::TextColored(ImVec4{0.42f, 0.91f, 0.77f, 1.0f}, "PROJECT");
        ImGui::SameLine(0.0f, 7.0f);
        ImGui::TextUnformatted(root_name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImVec4{0.33f, 0.76f, 0.68f, 1.0f}, "●");
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextDisabled("Open");
        const float close_width = ImGui::CalcTextSize("Close Project").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float sync_width = ImGui::CalcTextSize("Sync").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                      ImGui::GetWindowWidth() - close_width - sync_width -
                                          ImGui::GetStyle().ItemSpacing.x - 12.0f));
        if (ImGui::SmallButton("Close Project##ProjectClose") && close_project_) {
            close_project_();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Sync##ProjectSync")) {
            last_operation_ = runtime_.refresh_assets();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##project-tree-filter", "Search project...", filter_.data(), filter_.size());
        ImGui::Separator();

        const std::vector<EditorResourceDescriptor> descriptors = runtime_.resource_descriptors();
        const float details_height = selected_path_.empty() ? 0.0f : 74.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.028f, 0.078f, 0.102f, 1.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{5.0f, 6.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 15.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{4.0f, 2.0f});
        ImGui::BeginChild("ProjectDirectoryTree", ImVec2{0.0f, -details_height}, true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushID("ProjectRoot");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.63f, 0.92f, 0.84f, 1.0f});
        const std::string root_label = "   " + root_name;
        const bool root_open =
            ImGui::TreeNodeEx(root_label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        draw_root_icon(ImGui::GetItemRectMin());
        ImGui::PopStyleColor();
        if (root_open) {
            draw_directory(root, root, descriptors);
            ImGui::TreePop();
        }
        ImGui::PopID();
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();

        if (!selected_path_.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.050f, 0.122f, 0.145f, 1.0f});
            ImGui::BeginChild("ProjectSelection", ImVec2{0.0f, details_height - 5.0f}, true,
                              ImGuiWindowFlags_NoScrollbar);
            ImGui::TextDisabled("SELECTED FILE");
            ImGui::SameLine();
            ImGui::TextUnformatted(selected_path_.filename().string().c_str());
            const std::filesystem::path relative = selected_path_.lexically_relative(root);
            ImGui::TextDisabled("%s", relative.empty() ? selected_path_.string().c_str() : relative.string().c_str());
            if (const EditorResourceDescriptor* descriptor = find_descriptor(selected_path_, root, descriptors);
                descriptor != nullptr) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{0.40f, 0.74f, 0.94f, 1.0f}, "%s", resource_kind_name(descriptor->kind));
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        if (!last_operation_) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", last_operation_.message.c_str());
        }
        ImGui::End();
    }

private:
    [[nodiscard]] bool matches_filter(std::string_view name) const noexcept {
        return filter_[0] == '\0' || name.find(filter_.data()) != std::string_view::npos;
    }

    [[nodiscard]] static bool is_editor_metadata(const std::filesystem::path& path) noexcept {
        const std::string name = path.filename().string();
        return !name.empty() && name.front() == '.';
    }

    static const char* resource_kind_name(EditorResourceKind kind) noexcept {
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
                return "File";
        }
        return "File";
    }

    static void draw_root_icon(const ImVec2& item_min) noexcept {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float x = item_min.x + 25.0f;
        const float y = item_min.y + 4.0f;
        draw->AddQuadFilled(ImVec2{x + 5.5f, y}, ImVec2{x + 11.0f, y + 3.0f}, ImVec2{x + 5.5f, y + 6.0f},
                            ImVec2{x, y + 3.0f}, IM_COL32(102, 232, 202, 255));
        draw->AddQuadFilled(ImVec2{x, y + 3.0f}, ImVec2{x + 5.5f, y + 6.0f}, ImVec2{x + 5.5f, y + 11.0f},
                            ImVec2{x, y + 8.0f}, IM_COL32(48, 168, 154, 255));
        draw->AddQuadFilled(ImVec2{x + 5.5f, y + 6.0f}, ImVec2{x + 11.0f, y + 3.0f}, ImVec2{x + 11.0f, y + 8.0f},
                            ImVec2{x + 5.5f, y + 11.0f}, IM_COL32(69, 202, 181, 255));
    }

    static void draw_folder_icon(const ImVec2& item_min, const ImVec4& color, bool generated) noexcept {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 tint = ImGui::ColorConvertFloat4ToU32(color);
        const float x = item_min.x + 25.0f;
        const float y = item_min.y + 4.0f;
        const ImU32 shadow = generated ? IM_COL32(83, 113, 120, 255) : IM_COL32(33, 78, 82, 255);
        draw->AddRectFilled(ImVec2{x, y + 3.0f}, ImVec2{x + 13.0f, y + 11.5f}, shadow, 2.5f);
        draw->AddRectFilled(ImVec2{x + 0.5f, y + 2.5f}, ImVec2{x + 12.5f, y + 10.5f}, tint, 2.0f);
        draw->AddRectFilled(ImVec2{x + 1.0f, y + 0.5f}, ImVec2{x + 6.5f, y + 5.0f}, tint, 1.5f);
    }

    static void draw_file_icon(const ImVec2& item_min, const std::filesystem::path& path,
                               const ImVec4& color) noexcept {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 tint = ImGui::ColorConvertFloat4ToU32(color);
        const float x = item_min.x + 6.0f;
        const float y = item_min.y + 3.0f;
        const std::string extension = path.extension().string();
        draw->AddRectFilled(ImVec2{x, y}, ImVec2{x + 9.5f, y + 12.5f}, tint, 1.8f);
        draw->AddTriangleFilled(ImVec2{x + 6.5f, y}, ImVec2{x + 9.5f, y}, ImVec2{x + 9.5f, y + 3.0f},
                                IM_COL32(224, 245, 247, 110));
        draw->AddRect(ImVec2{x, y}, ImVec2{x + 9.5f, y + 12.5f}, IM_COL32(6, 24, 31, 200), 1.8f);

        if (extension == ".aegscene") {
            draw->AddCircleFilled(ImVec2{x + 3.5f, y + 5.0f}, 1.4f, IM_COL32(5, 27, 37, 230));
            draw->AddLine(ImVec2{x + 2.0f, y + 9.5f}, ImVec2{x + 7.4f, y + 9.5f}, IM_COL32(5, 27, 37, 230), 1.1f);
            draw->AddLine(ImVec2{x + 5.5f, y + 6.5f}, ImVec2{x + 7.4f, y + 9.5f}, IM_COL32(5, 27, 37, 230), 1.1f);
        } else if (extension == ".aegpipeline" || extension == ".pipeline") {
            draw->AddCircleFilled(ImVec2{x + 2.5f, y + 5.5f}, 1.2f, IM_COL32(18, 28, 42, 235));
            draw->AddCircleFilled(ImVec2{x + 7.0f, y + 8.5f}, 1.2f, IM_COL32(18, 28, 42, 235));
            draw->AddLine(ImVec2{x + 3.5f, y + 6.2f}, ImVec2{x + 6.0f, y + 7.8f}, IM_COL32(18, 28, 42, 235), 1.1f);
        } else if (extension == ".cpp" || extension == ".hpp" || extension == ".h") {
            draw->AddLine(ImVec2{x + 2.0f, y + 5.3f}, ImVec2{x + 7.3f, y + 5.3f}, IM_COL32(31, 19, 50, 235), 1.1f);
            draw->AddLine(ImVec2{x + 3.5f, y + 8.3f}, ImVec2{x + 7.3f, y + 8.3f}, IM_COL32(31, 19, 50, 235), 1.1f);
        } else if (extension == ".json" || extension == ".meta") {
            draw->AddLine(ImVec2{x + 2.5f, y + 5.0f}, ImVec2{x + 4.0f, y + 6.2f}, IM_COL32(49, 33, 13, 235), 1.1f);
            draw->AddLine(ImVec2{x + 4.0f, y + 6.2f}, ImVec2{x + 2.5f, y + 7.4f}, IM_COL32(49, 33, 13, 235), 1.1f);
            draw->AddLine(ImVec2{x + 7.0f, y + 5.0f}, ImVec2{x + 5.5f, y + 6.2f}, IM_COL32(49, 33, 13, 235), 1.1f);
            draw->AddLine(ImVec2{x + 5.5f, y + 6.2f}, ImVec2{x + 7.0f, y + 7.4f}, IM_COL32(49, 33, 13, 235), 1.1f);
        } else {
            draw->AddLine(ImVec2{x + 2.0f, y + 5.0f}, ImVec2{x + 7.1f, y + 5.0f}, IM_COL32(6, 24, 31, 220), 1.0f);
            draw->AddLine(ImVec2{x + 2.0f, y + 7.8f}, ImVec2{x + 6.0f, y + 7.8f}, IM_COL32(6, 24, 31, 220), 1.0f);
        }
    }

    static ImVec4 folder_color(std::string_view name, bool generated) noexcept {
        if (generated)
            return {0.36f, 0.47f, 0.51f, 1.0f};
        if (name == "Assets")
            return {0.50f, 0.87f, 0.72f, 1.0f};
        if (name == "Scenes")
            return {0.40f, 0.72f, 0.95f, 1.0f};
        if (name == "Pipelines")
            return {0.87f, 0.63f, 0.96f, 1.0f};
        if (name == "Source" || name == "Render")
            return {0.76f, 0.64f, 0.96f, 1.0f};
        return {0.55f, 0.76f, 0.73f, 1.0f};
    }

    static ImVec4 file_color(const std::filesystem::path& path) noexcept {
        const std::string extension = path.extension().string();
        if (extension == ".aegscene")
            return {0.45f, 0.74f, 0.96f, 1.0f};
        if (extension == ".aegpipeline" || extension == ".pipeline")
            return {0.90f, 0.64f, 0.96f, 1.0f};
        if (extension == ".aegproj" || extension == ".aegproject")
            return {0.47f, 0.88f, 0.73f, 1.0f};
        if (extension == ".cpp" || extension == ".hpp" || extension == ".h")
            return {0.78f, 0.62f, 0.96f, 1.0f};
        if (extension == ".json" || extension == ".meta")
            return {0.93f, 0.72f, 0.38f, 1.0f};
        if (extension == ".png" || extension == ".jpg" || extension == ".dds")
            return {0.92f, 0.67f, 0.38f, 1.0f};
        return {0.52f, 0.64f, 0.70f, 1.0f};
    }

    [[nodiscard]] static const EditorResourceDescriptor* find_descriptor(
        const std::filesystem::path& path, const std::filesystem::path& root,
        const std::vector<EditorResourceDescriptor>& descriptors) noexcept {
        const std::filesystem::path normalized_path = path.lexically_normal();
        for (const EditorResourceDescriptor& descriptor : descriptors) {
            if (descriptor.path.empty())
                continue;
            std::filesystem::path candidate{descriptor.path};
            if (candidate.is_relative())
                candidate = root / candidate;
            if (candidate.lexically_normal() == normalized_path)
                return &descriptor;
        }
        return nullptr;
    }

    void draw_directory(const std::filesystem::path& directory, const std::filesystem::path& root,
                        const std::vector<EditorResourceDescriptor>& descriptors) {
        std::error_code error;
        std::vector<std::filesystem::directory_entry> entries;
        for (std::filesystem::directory_iterator iterator{
                 directory, std::filesystem::directory_options::skip_permission_denied, error};
             !error && iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
            if (!is_editor_metadata(iterator->path()))
                entries.push_back(*iterator);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            std::error_code left_error, right_error;
            const bool left_directory = left.is_directory(left_error);
            const bool right_directory = right.is_directory(right_error);
            if (left_directory != right_directory)
                return left_directory;
            return left.path().filename().string() < right.path().filename().string();
        });
        for (const std::filesystem::directory_entry& entry : entries) {
            const std::filesystem::path path = entry.path();
            const std::string name = path.filename().string();
            std::error_code type_error;
            if (entry.is_directory(type_error)) {
                ImGui::PushID(path.string().c_str());
                const bool generated = name == "Binaries" || name == "Build" || name == "Cache" || name == "Saved";
                ImGui::PushStyleColor(
                    ImGuiCol_Text, generated ? ImVec4{0.42f, 0.50f, 0.54f, 1.0f} : ImVec4{0.76f, 0.85f, 0.87f, 1.0f});
                const std::string folder_label = "   " + name;
                const bool open = ImGui::TreeNodeEx(folder_label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
                draw_folder_icon(ImGui::GetItemRectMin(), folder_color(name, generated), generated);
                ImGui::PopStyleColor();
                if (open) {
                    draw_directory(path, root, descriptors);
                    ImGui::TreePop();
                }
                ImGui::PopID();
                continue;
            }
            if (!matches_filter(name))
                continue;
            ImGui::PushID(path.string().c_str());
            const EditorResourceDescriptor* descriptor = find_descriptor(path, root, descriptors);
            const bool selected = selected_path_.lexically_normal() == path.lexically_normal();
            const std::string label = "   " + name;
            if (ImGui::Selectable(label.c_str(), selected)) {
                selected_path_ = path;
            }
            draw_file_icon(ImGui::GetItemRectMin(), path, file_color(path));
            ImGui::PopID();
        }
    }

    EditorSceneRuntime& runtime_;
    std::function<void()> close_project_;
    std::array<char, 128> filter_{};
    std::filesystem::path selected_path_{};
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
        kImport,
        kRemove,
    };

    struct PendingRequest final {
        Operation operation{Operation::kApply};
        std::string font_key{};
        float font_size_pixels{16.0f};
    };

    [[nodiscard]] Result request_import_font() override { return schedule(Operation::kImport, {}, 0.0f); }

    [[nodiscard]] Result request_remove_font(std::string_view font_key) override {
        return schedule(Operation::kRemove, font_key, 0.0f);
    }

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
        // Font atlas construction and GPU upload are synchronous. Coalesce a
        // burst of slider/button edits and wait for a short quiet period so
        // the editor remains responsive while the draft value changes.
        if (pending_operation_ == Operation::kPreview &&
            std::chrono::steady_clock::now() - pending_since_ < std::chrono::milliseconds(120)) {
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
        if ((operation == Operation::kPreview || operation == Operation::kApply || operation == Operation::kSave ||
             operation == Operation::kCancel || operation == Operation::kRemove) &&
            font_key.empty()) {
            return aeg::foundation::invalid_argument("Editor preference font key is empty");
        }
        try {
            std::string next_font_key{font_key};
            pending_font_key_ = std::move(next_font_key);
            pending_font_size_pixels_ = size_pixels;
            pending_operation_ = operation;
            pending_since_ = std::chrono::steady_clock::now();
            pending_ = true;
            return Result::success();
        } catch (const std::bad_alloc&) {
            return aeg::foundation::out_of_memory("Editor preference pending state allocation failed");
        }
    }

    std::string pending_font_key_{};
    float pending_font_size_pixels_{16.0f};
    Operation pending_operation_{Operation::kApply};
    std::chrono::steady_clock::time_point pending_since_{};
    bool pending_{false};
};

class EditorApplication final {
    /** RHI state for one Dear ImGui platform viewport created by the workspace. */
    struct NativeViewportRenderState final {
        std::shared_ptr<IWindow> window{};
        SurfaceHandle surface{};
        SwapchainHandle swapchain{};
        SwapchainInfo swapchain_info{};
        std::unique_ptr<FrameContext> frame_context{};
        std::unique_ptr<RenderGraph> render_graph{};
        std::unique_ptr<ImGuiRenderer> imgui_renderer{};
        PipelineRegistry registry{};
        PipelineScheduler scheduler{{1, 1, 1, 1}};
        PipelineScopeHost frame_host{PipelineScope::kFrame,
                                     aeg::pipeline::PipelineInstanceId("Editor.NativeViewport.Frame")};
        std::unique_ptr<aeg::engine::EngineFrameOrchestrator> orchestrator{};
        bool resize_pending{false};
    };

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
        if (!log_path.empty())
            std::filesystem::create_directories(log_path.parent_path(), log_directory_error);
        aeg::logging::Config logging_config{};
        logging_config.file_enabled = !log_path.empty() && !log_directory_error;
        logging_config.file_path = log_path;
        aeg::logging::Status logging_status = aeg::logging::initialize(logging_config);
        if (!logging_status) {
            // An unavailable log file must not prevent the editor from opening.
            logging_config.file_enabled = false;
            logging_status = aeg::logging::initialize(logging_config);
            if (!logging_status) {
                return false;
            }
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
        window_desc.client_decorated = false;
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
        if (!startup_project_.empty()) {
            const auto project_result =
                aeg::editor::guard_editor_operation([&] { return scene_runtime_->import_project(startup_project_); });
            if (!project_result) {
                notify_runtime_result(project_result, "open_startup_project");
                if (fatal_runtime_error_)
                    return false;
            } else {
                if (!scene_runtime_->renderer_pipeline_diagnostic().empty()) {
                    AEG_LOG_WARNING(aeg::logging::Category::kApplication, "{}",
                                    scene_runtime_->renderer_pipeline_diagnostic());
                }
                const auto pipeline_result =
                    aeg::editor::guard_editor_operation([&] { return restore_pipeline_document(); });
                if (!pipeline_result) {
                    notify_runtime_result(pipeline_result, "restore_startup_pipeline_document");
                    if (fatal_runtime_error_)
                        return false;
                }
            }
        }
        // Keep startup non-modal. Users can open the hub from File > Project Hub
        // when they want to create or choose a project.
        project_hub_pending_ = false;

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
        auto system_font_result = EditorSystemFontCatalog{}.load();
        if (!system_font_result) {
            notify_runtime_result(system_font_result.result, "load_system_fonts");
            if (fatal_runtime_error_)
                return false;
            // An empty catalog selects ImGui's embedded default font.
        } else {
            font_catalog_ = std::move(system_font_result.value.catalog);
            default_font_key_ = std::move(system_font_result.value.default_font_key);
        }

        aeg::editor::EditorImGuiDesc imgui_desc{};
        imgui_desc.platform_services = &window_system_->platform_services();
        imgui_desc.window_system = window_system_.get();
        imgui_desc.enable_viewports = true;
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
        if (preference_controller_ != nullptr && preference_model_ != nullptr) {
            const EditorPreferenceState& initial_state = preference_model_->state();
            if (!report_result(preference_controller_->preview(initial_state), "restore_editor_preference_font")) {
                return false;
            }
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

        auto pipeline_panel = std::make_unique<PipelinePanel>(
            frame_host_, imgui_host_, *scene_runtime_, pipeline_drafts_, pipeline_layouts_,
            [this] { return save_scene_and_apply_pipeline_drafts(); },
            [this](bool visible) { static_cast<void>(workspace_.set_panel_visible("pipeline", visible)); },
            [this] {
                return workspace_.panel_presentation("pipeline") ==
                       aeg::editor::EditorWorkspace::PanelPresentation::kNativeWindow;
            });
        PipelinePanel* pipeline_panel_state = pipeline_panel.get();
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
                    std::move(pipeline_panel),
                    [pipeline_panel_state](bool visible) { pipeline_panel_state->set_visible(visible); }),
                "register_pipeline_panel") ||
            !report_result(workspace_.register_panel(EditorPanelDescriptor{"hierarchy", "Hierarchy",
                                                                           EditorDockRegion::kLeft, true, true, true},
                                                     std::make_unique<HierarchyPanel>(*scene_runtime_)),
                           "register_hierarchy_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"project", "Project", EditorDockRegion::kLeft, true, true, true},
                               std::make_unique<ProjectPanel>(*scene_runtime_, [this] {
                                   request_confirmed_document_action(
                                       EditorDocumentAction{EditorDocumentActionType::kCloseProject});
                               })),
                           "register_project_panel") ||
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
                 {"save_scene", "Save", [this] { return scene_runtime_->save_scene(); }, false},
                 {"undo", "Undo", [this] { return scene_runtime_->undo(); }, false},
                 {"redo", "Redo", [this] { return scene_runtime_->redo(); }, false},
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
        workspace_.set_main_menu_content([this] { build_workspace_navigation_menu(); });
        workspace_.set_window_controls(aeg::editor::EditorWindowControls{
            [this] {
                return window_ != nullptr ? window_->set_minimized(true)
                                          : aeg::foundation::invalid_state("Editor window is unavailable");
            },
            [this] {
                return window_ != nullptr ? window_->set_maximized(!window_->state().maximized)
                                          : aeg::foundation::invalid_state("Editor window is unavailable");
            },
            [this] {
                return window_ != nullptr ? window_->request_close()
                                          : aeg::foundation::invalid_state("Editor window is unavailable");
            },
        });
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
                          bool native_viewport_regression,
                          const aeg::application::IConsoleInterruptController& console_interrupt) {
        regression_mode_ = resize_regression || open_preferences_regression || native_viewport_regression;
        resize_regression_ = resize_regression;
        preferences_regression_mode_ = open_preferences_regression;
        preferences_regression_opened_ = false;
        preferences_regression_closed_ = false;
        preferences_regression_reopened_ = false;
        preferences_regression_preview_requested_ = false;
        preferences_regression_preview_completed_ = false;
        preferences_open_requested_ = open_preferences_regression;
        uint32_t attempted_frames = 0;
        while (!close_requested() && !console_interrupt.interrupted()) {
            try {
                // A bounded automated run must finish even when every attempted
                // frame reports a recoverable error before it can be presented.
                if (frame_limit != 0 && attempted_frames >= frame_limit) {
                    break;
                }
                ++attempted_frames;

                // Exercise the same presentation command as the workspace menu,
                // including re-docking and exiting with a native viewport alive.
                if (native_viewport_regression &&
                    (attempted_frames == 2 || attempted_frames == 16 || attempted_frames == 28 ||
                     attempted_frames == 40 || attempted_frames == 52)) {
                    const bool native = attempted_frames != 16 && attempted_frames != 40;
                    const auto presentation = native ? aeg::editor::EditorWorkspace::PanelPresentation::kNativeWindow
                                                     : aeg::editor::EditorWorkspace::PanelPresentation::kDockWindow;
                    if (!report_result(workspace_.set_panel_presentation("pipeline", presentation),
                                       "switch_pipeline_presentation"))
                        return 1;
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
                        return;
                    }
                    // ImGui platform viewports own their native windows, while the
                    // application owns the RHI targets used to render them.
                    auto native_viewport = native_viewports_.find(event.window);
                    if (native_viewport != native_viewports_.end() && event.type == WindowEventType::kWindowResized) {
                        native_viewport->second->resize_pending = true;
                    }
                    if (event_result && imgui_host_.is_initialized()) {
                        event_result = imgui_host_.submit_event(event);
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
                workspace_.set_game_draw_calls(scene_runtime_ != nullptr ? scene_runtime_->draw_call_count() : 0);
                const Result build_workspace_result = workspace_.build_ui();
                if (!build_workspace_result) {
                    notify_runtime_result(build_workspace_result, "build_editor_workspace");
                    static_cast<void>(imgui_host_.abort_frame());
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

                std::vector<ImGuiDrawPacket> draw_packets;
                const Result capture_draw_result = imgui_host_.capture_draw_packets(draw_packets);
                if (!capture_draw_result) {
                    notify_runtime_result(capture_draw_result, "capture_imgui_draw_packets");
                    finish_input_frame();
                    if (fatal_runtime_error_) {
                        return 1;
                    }
                    continue;
                }

                const auto main_packet = std::find_if(
                    draw_packets.begin(), draw_packets.end(),
                    [this](const auto& packet) { return window_ != nullptr && packet.window == window_->id(); });
                if (main_packet == draw_packets.end()) {
                    notify_runtime_result(
                        aeg::foundation::invalid_state("Main editor ImGui viewport has no draw packet"),
                        "capture_main_imgui_draw_packet");
                    finish_input_frame();
                    return 1;
                }
                Result frame_result = run_engine_frame(*main_packet);
                for (const ImGuiDrawPacket& packet : draw_packets) {
                    if (!frame_result || packet.window == main_packet->window) {
                        continue;
                    }
                    frame_result = run_native_viewport_frame(packet);
                }
                release_stale_native_viewports(draw_packets);
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
                if (native_viewport_regression &&
                    (attempted_frames == 12 || attempted_frames == 24 || attempted_frames == 36 ||
                     attempted_frames == 48 || attempted_frames == 60)) {
                    const bool expected_native = attempted_frames != 24 && attempted_frames != 48;
                    if (!report_result(native_viewports_.empty() == !expected_native
                                           ? Result::success()
                                           : aeg::foundation::invalid_state(
                                                 "Pipeline viewport presentation did not change at frame " +
                                                 std::to_string(attempted_frames) +
                                                 "; native windows: " + std::to_string(native_viewports_.size())),
                                       "verify_pipeline_presentation"))
                        return 1;
                }
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
            } catch (...) {
                // A user action or platform callback may throw after opening UI
                // scopes. Discard that frame before the next iteration can render.
                const Result failure = aeg::editor::guard_editor_operation([]() -> Result { throw; });
                static_cast<void>(imgui_host_.abort_frame());
                static_cast<void>(preferences_imgui_host_.abort_frame());
                input_system_.end_frame();
                for (FrameContext* frame : {frame_context_.get(), preferences_frame_context_.get()}) {
                    if (frame != nullptr && frame->is_frame_active()) {
                        const auto abort_result =
                            aeg::editor::guard_editor_operation([&] { return frame->abort_frame(); });
                        if (!abort_result)
                            notify_runtime_result(abort_result, "abort_interrupted_frame");
                    }
                }
                notify_runtime_result(failure, "editor_operation_exception");
                if (fatal_runtime_error_ || regression_mode_)
                    return 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
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
        // Saved project graph overrides must not modify the Editor fallback preview.
        if (scene_runtime_ != nullptr && !scene_runtime_->renderer_pipeline_diagnostic().empty())
            return Result::success();
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
                                 action.type == EditorDocumentActionType::kCloseProject ||
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
                result = action.project_options.has_value()
                             ? scene_runtime_->new_project(*action.project_options)
                             : scene_runtime_->new_project(action.path, action.name);
                if (result) {
                    pipeline_saved_orderings_.clear();
                    pipeline_layouts_.clear();
                    pipeline_saved_blends_.clear();
                    project_hub_operation_ = Result::success();
                }
                break;
            case EditorDocumentActionType::kImportProject:
                result = scene_runtime_->import_project(action.path);
                if (result)
                    result = restore_pipeline_document();
                if (result)
                    project_hub_operation_ = Result::success();
                break;
            case EditorDocumentActionType::kCloseProject:
                result = scene_runtime_->close_project();
                if (result) {
                    pipeline_saved_orderings_.clear();
                    pipeline_layouts_.clear();
                    pipeline_saved_blends_.clear();
                    project_hub_pending_ = false;
                }
                break;
            case EditorDocumentActionType::kCloseEditor:
                close_confirmation_requested_ = false;
                force_shutdown_ = true;
                return;
        }
        if (!result) {
            if (action.type == EditorDocumentActionType::kNewProject ||
                action.type == EditorDocumentActionType::kImportProject) {
                project_hub_operation_ = result;
                project_hub_pending_ = true;
            }
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

    /** @brief Renders application commands in the workspace-owned navigation row. */
    void build_workspace_navigation_menu() {
        if (scene_runtime_ == nullptr)
            return;
        const bool has_project = scene_runtime_->project_loaded();
        const bool can_edit = has_project && scene_runtime_->mode() == EditorSceneRuntime::Mode::kEdit &&
                              !scene_runtime_->component_edit_active();
        const auto report_history_result = [this](const Result& result, std::string_view operation) {
            if (!result && result.code != ResultCode::kNotFound)
                notify_runtime_result(result, operation);
        };
        if (ImGui::BeginMenu("File")) {
            const auto open_project_hub = [this] {
                project_hub_operation_ = Result::success();
                project_hub_pending_ = true;
            };
            if (ImGui::MenuItem("Project Hub...")) {
                open_project_hub();
            }
            if (ImGui::MenuItem("New Project...")) {
                open_project_hub();
            }
            if (ImGui::MenuItem("Open Project...", "Ctrl+Shift+O")) {
                open_project_hub();
            }
            if (ImGui::MenuItem("Import Project...")) {
                open_project_hub();
            }
            if (ImGui::MenuItem("Import Asset...", "Ctrl+Alt+I", false, can_edit)) {
                import_options_pending_ = true;
            }
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, can_edit)) {
                request_document_action(EditorDocumentAction{EditorDocumentActionType::kSaveScene});
            }
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, can_edit)) {
                request_document_action(EditorDocumentAction{EditorDocumentActionType::kSaveAsScene,
                                                             scene_runtime_->scene_path()});
            }
            if (ImGui::MenuItem("Project Settings...", nullptr, false, has_project)) {
                project_settings_pending_ = true;
            }
            if (ImGui::MenuItem("Close Project", "Ctrl+Shift+W", false, has_project)) {
                request_confirmed_document_action(EditorDocumentAction{EditorDocumentActionType::kCloseProject});
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                close_confirmation_requested_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, scene_runtime_->can_undo()))
                report_history_result(scene_runtime_->undo(), "undo_editor_menu");
            if (ImGui::MenuItem("Redo", "Ctrl+Y / Ctrl+Shift+Z", false, scene_runtime_->can_redo()))
                report_history_result(scene_runtime_->redo(), "redo_editor_menu");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Assets")) {
            if (ImGui::MenuItem("Refresh Project Assets", "F5", false, has_project))
                notify_runtime_result(workspace_.invoke_view_action("refresh_assets"), "refresh_project_assets_menu");
            if (ImGui::MenuItem("Asset Registry", nullptr, false, has_project)) {
                registry_viewer_pending_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Pipeline")) {
            if (ImGui::MenuItem("Rebuild Editor Pipeline"))
                notify_runtime_result(workspace_.invoke_view_action("rebuild_pipeline"), "rebuild_pipeline_menu");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("AEG Engine Editor");
            ImGui::TextDisabled("Pipeline workspace");
            ImGui::EndMenu();
        }
    }

    void build_project_hub() {
        if (scene_runtime_ == nullptr) {
            return;
        }
        if (!project_hub_initialized_) {
            std::filesystem::path default_base = std::filesystem::current_path().parent_path();
            if (!resource_root_.empty()) {
                default_base = resource_root_;
                std::string leaf = default_base.filename().string();
                std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                if (leaf == "debug" || leaf == "release") {
                    default_base = default_base.parent_path();
                    leaf = default_base.filename().string();
                    std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                }
                if (leaf == "build" || leaf == "cmake-build-debug" || leaf == "cmake-build-release") {
                    default_base = default_base.parent_path();
                }
            }
            const std::string default_path = default_base.string();
            const std::string default_name = "AEG Game Project";
            std::memcpy(project_hub_path_buffer_.data(), default_path.data(),
                        std::min(default_path.size(), project_hub_path_buffer_.size() - 1u));
            std::memcpy(project_hub_name_buffer_.data(), default_name.data(),
                        std::min(default_name.size(), project_hub_name_buffer_.size() - 1u));
            set_text_buffer(project_hub_open_path_buffer_, {});
            project_hub_initialized_ = true;
        }
        if (project_hub_pending_) {
            ImGui::OpenPopup("ProjectHub");
            project_hub_pending_ = false;
            project_hub_close_requested_ = true;
        }
        if (const ImGuiViewport* viewport = ImGui::GetMainViewport(); viewport != nullptr) {
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2{0.5f, 0.5f});
            ImGui::SetNextWindowSize(
                ImVec2{std::min(1280.0f, viewport->WorkSize.x * 0.82f), std::min(700.0f, viewport->WorkSize.y * 0.72f)},
                ImGuiCond_Always);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{20.0f, 16.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{8.0f, 6.0f});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0.065f, 0.108f, 0.132f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0.23f, 0.38f, 0.44f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0.105f, 0.170f, 0.205f, 1.0f});
        if (!ImGui::BeginPopupModal(
                "ProjectHub", &project_hub_close_requested_,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(4);
            return;
        }
        const ImU32 accent = IM_COL32(103, 224, 194, 255);
        const ImU32 icon_blue = IM_COL32(166, 209, 224, 255);
        const ImU32 muted = IM_COL32(156, 177, 188, 255);
        const ImU32 card_fill = IM_COL32(25, 42, 51, 255);
        const ImU32 card_border = IM_COL32(73, 99, 111, 255);
        const auto draw_project_cube_icon = [](ImDrawList* draw, ImVec2 center, float radius, ImU32 color) {
            const ImVec2 top{center.x, center.y - radius};
            const ImVec2 upper_right{center.x + radius * 0.88f, center.y - radius * 0.48f};
            const ImVec2 lower_right{center.x + radius * 0.88f, center.y + radius * 0.55f};
            const ImVec2 bottom{center.x, center.y + radius};
            const ImVec2 lower_left{center.x - radius * 0.88f, center.y + radius * 0.55f};
            const ImVec2 upper_left{center.x - radius * 0.88f, center.y - radius * 0.48f};
            draw->AddLine(top, upper_right, color, 2.4f);
            draw->AddLine(upper_right, lower_right, color, 2.4f);
            draw->AddLine(lower_right, bottom, color, 2.4f);
            draw->AddLine(bottom, lower_left, color, 2.4f);
            draw->AddLine(lower_left, upper_left, color, 2.4f);
            draw->AddLine(upper_left, top, color, 2.4f);
            draw->AddLine(upper_left, center, color, 2.4f);
            draw->AddLine(upper_right, center, color, 2.4f);
            draw->AddLine(center, bottom, color, 2.4f);
        };
        const auto draw_project_file_icon = [icon_blue](ImDrawList* draw, ImVec2 top_left) {
            draw->AddRect(top_left, ImVec2{top_left.x + 22.0f, top_left.y + 29.0f}, icon_blue, 2.0f, 0, 2.0f);
            draw->AddLine(ImVec2{top_left.x + 14.0f, top_left.y}, ImVec2{top_left.x + 22.0f, top_left.y + 8.0f},
                          icon_blue, 2.0f);
            draw->AddLine(ImVec2{top_left.x + 14.0f, top_left.y}, ImVec2{top_left.x + 14.0f, top_left.y + 8.0f},
                          icon_blue, 2.0f);
        };
        const auto fit_project_hub_text = [](std::string text, float width, bool prefix_ellipsis = false) {
            constexpr const char* ellipsis = "...";
            if (width <= 0.0f) {
                return std::string{};
            }
            if (ImGui::CalcTextSize(text.c_str()).x <= width) {
                return text;
            }
            if (ImGui::CalcTextSize(ellipsis).x > width) {
                return std::string{};
            }
            if (prefix_ellipsis) {
                std::size_t first = 0u;
                while (first < text.size() &&
                       ImGui::CalcTextSize((std::string{ellipsis} + text.substr(first)).c_str()).x > width) {
                    ++first;
                    while (first < text.size() && (static_cast<unsigned char>(text[first]) & 0xc0u) == 0x80u) {
                        ++first;
                    }
                }
                return std::string{ellipsis} + text.substr(first);
            }
            while (!text.empty() && ImGui::CalcTextSize((text + ellipsis).c_str()).x > width) {
                std::size_t last = text.size() - 1u;
                while (last > 0u && (static_cast<unsigned char>(text[last]) & 0xc0u) == 0x80u) {
                    --last;
                }
                text.resize(last);
            }
            return text.empty() ? std::string{} : text + ellipsis;
        };
        const auto open_project_from_path = [this](const std::filesystem::path& path) {
            EditorDocumentAction action{};
            action.type = EditorDocumentActionType::kImportProject;
            action.path = path;
            ImGui::CloseCurrentPopup();
            request_confirmed_document_action(std::move(action));
        };
        if (!project_hub_operation_) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", project_hub_operation_.message.c_str());
            ImGui::Separator();
        }

        EditorProjectManager recent_manager;
        const auto recent_result = recent_manager.recent_projects();
        const std::vector<EditorRecentProject> recent_projects =
            recent_result ? std::move(recent_result.value) : std::vector<EditorRecentProject>{};
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{16.0f, 0.0f});
        if (ImGui::BeginTable(
                "ProjectHubColumns", 2,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                ImVec2{0.0f, std::max(200.0f, ImGui::GetContentRegionAvail().y - 55.0f)})) {
            ImGui::TableSetupColumn("Create New Project", ImGuiTableColumnFlags_WidthStretch, 0.61f);
            ImGui::TableSetupColumn("Recent Projects", ImGuiTableColumnFlags_WidthStretch, 0.39f);
            ImGui::TableNextColumn();
            {
                const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2{62.0f, 64.0f});
                draw_project_cube_icon(ImGui::GetWindowDrawList(), ImVec2{icon_pos.x + 30.0f, icon_pos.y + 31.0f},
                                       23.0f, accent);
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::BeginGroup();
                ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.32f);
                ImGui::TextUnformatted("Create New Project");
                ImGui::PopFont();
                ImGui::TextDisabled("Name your project and choose a starting setup.");
                ImGui::EndGroup();
            }
            ImGui::Separator();
            ImGui::Dummy(ImVec2{0.0f, 16.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0f, 8.0f});
            if (ImGui::BeginTable("ProjectHubNameLocation", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.48f);
                ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 0.52f);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Project Name");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##ProjectHubName", project_hub_name_buffer_.data(), project_hub_name_buffer_.size());
                ImGui::TextDisabled("Used for the new project folder.");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Project Location");
                ImGui::SetNextItemWidth(-105.0f);
                ImGui::InputText("##ProjectHubCreatePath", project_hub_path_buffer_.data(),
                                 project_hub_path_buffer_.size());
                ImGui::SameLine();
                if (ImGui::Button("Browse...##ProjectHubCreatePath")) {
                    const auto selection = aeg::application::select_directory(
                        window_ != nullptr ? window_->surface_binding() : aeg::platform::WindowSurfaceBinding{},
                        "Select the parent directory for the new project", project_hub_path_buffer_.data());
                    if (selection) {
                        set_text_buffer(project_hub_path_buffer_, selection.value);
                    } else if (selection.result.code != ResultCode::kCancelled) {
                        project_hub_operation_ = selection.result;
                    }
                }
                ImGui::TextDisabled("A new project folder will be created here.");
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2{0.0f, 10.0f});
            ImGui::TextUnformatted("Template");
            if (ImGui::BeginTable("ProjectHubTemplates", 3, ImGuiTableFlags_SizingStretchSame)) {
                const char* titles[] = {"Empty", "Forward", "Deferred"};
                const char* descriptions[] = {"No preset", "Forward profile", "Deferred profile"};
                for (int index = 0; index < 3; ++index) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(index);
                    const ImVec2 card_size{ImGui::GetContentRegionAvail().x, 76.0f};
                    ImGui::InvisibleButton("TemplateCard", card_size);
                    const bool clicked = ImGui::IsItemClicked();
                    const bool hovered = ImGui::IsItemHovered();
                    if (clicked) {
                        project_hub_template_index_ = index;
                    }
                    if (hovered) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    const ImVec2 card_min = ImGui::GetItemRectMin();
                    const ImVec2 card_max = ImGui::GetItemRectMax();
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    const bool selected = project_hub_template_index_ == index;
                    draw->AddRectFilled(card_min, card_max,
                                        selected  ? IM_COL32(29, 68, 68, 255)
                                        : hovered ? IM_COL32(34, 56, 67, 255)
                                                  : card_fill,
                                        7.0f);
                    draw->AddRect(card_min, card_max, selected ? accent : card_border, 7.0f, 0, selected ? 2.0f : 1.0f);
                    if (index == 0) {
                        draw_project_cube_icon(draw, ImVec2{card_min.x + 30.0f, card_min.y + 38.0f}, 16.0f, accent);
                    } else if (index == 1) {
                        draw->AddTriangle(ImVec2{card_min.x + 20.0f, card_min.y + 22.0f},
                                          ImVec2{card_min.x + 20.0f, card_min.y + 54.0f},
                                          ImVec2{card_min.x + 46.0f, card_min.y + 38.0f}, icon_blue, 2.2f);
                    } else {
                        for (int layer = 0; layer < 3; ++layer) {
                            const float y = card_min.y + 25.0f + layer * 10.0f;
                            draw->AddPolyline(std::array<ImVec2, 4>{ImVec2{card_min.x + 18.0f, y},
                                                                    ImVec2{card_min.x + 32.0f, y - 8.0f},
                                                                    ImVec2{card_min.x + 46.0f, y},
                                                                    ImVec2{card_min.x + 32.0f, y + 8.0f}}
                                                  .data(),
                                              4, icon_blue, ImDrawFlags_Closed, 2.2f);
                        }
                    }
                    const float card_text_width = std::max(0.0f, card_max.x - (card_min.x + 62.0f) - 10.0f);
                    const std::string title = fit_project_hub_text(titles[index], card_text_width);
                    const std::string description = fit_project_hub_text(descriptions[index], card_text_width);
                    draw->PushClipRect(card_min, card_max, true);
                    draw->AddText(ImVec2{card_min.x + 62.0f, card_min.y + 18.0f}, IM_COL32(233, 242, 246, 255),
                                  title.c_str());
                    draw->AddText(ImVec2{card_min.x + 62.0f, card_min.y + 43.0f}, muted, description.c_str());
                    draw->PopClipRect();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::Dummy(ImVec2{0.0f, 14.0f});
            const char* engine_sdks[] = {"1.0", "2.0"};
            const char* pipelines[] = {"Forward", "Deferred"};
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0f, 8.0f});
            if (ImGui::BeginTable("ProjectHubEngineRenderer", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Engine Version");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::Combo("##ProjectHubSdk", &project_hub_sdk_index_, engine_sdks, IM_ARRAYSIZE(engine_sdks));
                ImGui::TextDisabled("Saved as this project's version.");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Startup Renderer");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::Combo("##ProjectHubPipeline", &project_hub_pipeline_index_, pipelines, IM_ARRAYSIZE(pipelines));
                ImGui::TextDisabled("The renderer used when the project opens.");
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2{0.0f, 14.0f});
            const bool can_create = project_hub_name_buffer_[0] != '\0' && project_hub_path_buffer_[0] != '\0';
            ImGui::BeginDisabled(!can_create);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.25f, 0.58f, 0.53f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.30f, 0.66f, 0.59f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.20f, 0.50f, 0.46f, 1.0f});
            if (ImGui::Button("+  Create Project", ImVec2{-1.0f, 56.0f})) {
                ProjectCreateOptions options{};
                options.root = std::filesystem::path(project_hub_path_buffer_.data()) / project_hub_name_buffer_.data();
                options.name = project_hub_name_buffer_.data();
                options.template_id = project_hub_template_index_ == 2
                                          ? "deferred"
                                          : (project_hub_template_index_ == 1 ? "forward" : "empty");
                options.engine_sdk = project_hub_sdk_index_ == 1 ? "2.0" : "1.0";
                options.render_pipeline = project_hub_pipeline_index_ == 1 ? "Assets/Pipelines/Deferred.aegpipe"
                                                                           : "Assets/Pipelines/Forward.aegpipe";
                EditorDocumentAction action{};
                action.type = EditorDocumentActionType::kNewProject;
                action.project_options = std::move(options);
                ImGui::CloseCurrentPopup();
                request_confirmed_document_action(std::move(action));
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Both renderers are included. You can change the startup choice later.");

            ImGui::TableNextColumn();
            {
                const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2{58.0f, 64.0f});
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec2 center{icon_pos.x + 29.0f, icon_pos.y + 31.0f};
                draw->AddCircle(center, 21.0f, icon_blue, 32, 2.2f);
                draw->AddLine(center, ImVec2{center.x, center.y - 12.0f}, icon_blue, 2.2f);
                draw->AddLine(center, ImVec2{center.x + 10.0f, center.y + 8.0f}, icon_blue, 2.2f);
                ImGui::SameLine(0.0f, 10.0f);
                ImGui::BeginGroup();
                ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.28f);
                ImGui::TextUnformatted("Recent Projects");
                ImGui::PopFont();
                ImGui::TextDisabled("Open a project you've worked on before.");
                ImGui::EndGroup();
            }
            ImGui::Separator();
            ImGui::Dummy(ImVec2{0.0f, 11.0f});
            const float recent_list_height =
                std::min(330.0f, std::max(170.0f, ImGui::GetContentRegionAvail().y - 105.0f));
            ImGui::BeginChild("ProjectHubRecentList", ImVec2{0.0f, recent_list_height}, false);
            if (recent_projects.empty()) {
                ImGui::TextDisabled("No recent projects yet.");
            }
            for (const auto& recent : recent_projects) {
                ImGui::PushID(recent.root.string().c_str());
                ImGui::InvisibleButton("RecentProjectCard", ImVec2{-1.0f, 60.0f});
                const bool hovered = ImGui::IsItemHovered();
                if (hovered) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s", recent.root.string().c_str());
                }
                if (ImGui::IsItemClicked()) {
                    project_hub_selected_root_ = recent.root;
                    open_project_from_path(recent.root);
                }
                const ImVec2 card_min = ImGui::GetItemRectMin();
                const ImVec2 card_max = ImGui::GetItemRectMax();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(card_min, card_max, hovered ? IM_COL32(34, 57, 67, 255) : card_fill, 7.0f);
                draw->AddRect(card_min, card_max, hovered ? icon_blue : card_border, 7.0f, 0, 1.0f);
                draw_project_file_icon(draw, ImVec2{card_min.x + 18.0f, card_min.y + 15.0f});
                const float text_x = card_min.x + 66.0f;
                const float available_text_width = card_max.x - text_x - 15.0f;
                const std::string display_name = fit_project_hub_text(recent.name, available_text_width);
                const std::string display_path = fit_project_hub_text(recent.root.string(), available_text_width, true);
                draw->PushClipRect(card_min, card_max, true);
                draw->AddText(ImVec2{text_x, card_min.y + 11.0f}, IM_COL32(233, 242, 246, 255), display_name.c_str());
                draw->AddText(ImVec2{text_x, card_min.y + 33.0f}, muted, display_path.c_str());
                draw->PopClipRect();
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (!recent_result) {
                ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", recent_result.result.message.c_str());
            }
            ImGui::Dummy(ImVec2{0.0f, 8.0f});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.16f, 0.28f, 0.34f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.20f, 0.35f, 0.42f, 1.0f});
            if (ImGui::Button("Open Existing Project...", ImVec2{-1.0f, 48.0f})) {
                const auto selection = aeg::application::select_directory(
                    window_ != nullptr ? window_->surface_binding() : aeg::platform::WindowSurfaceBinding{},
                    "Select a game project directory", project_hub_open_path_buffer_.data());
                if (selection) {
                    set_text_buffer(project_hub_open_path_buffer_, selection.value);
                    open_project_from_path(selection.value);
                } else if (selection.result.code != ResultCode::kCancelled) {
                    project_hub_operation_ = selection.result;
                }
            }
            ImGui::PopStyleColor(2);
            ImGui::TextDisabled("Find a project folder on this computer.");
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        ImGui::Separator();
        const float close_hub_width = 150.0f;
        const float close_hub_height = 32.0f;
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),
                                      ImGui::GetWindowHeight() - close_hub_height - ImGui::GetStyle().WindowPadding.y));
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - close_hub_width - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("Close Hub", ImVec2{close_hub_width, close_hub_height})) {
            project_hub_operation_ = Result::success();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(4);
    }

    void begin_project_settings() {
        project_settings_loaded_ = false;
        if (scene_runtime_ == nullptr || !scene_runtime_->project_loaded()) {
            project_settings_operation_ = aeg::foundation::invalid_state("No project is open");
            return;
        }
        const auto project_file = aeg::application::project::discover_project_file(scene_runtime_->project_root());
        if (!project_file) {
            project_settings_operation_ = project_file.result;
            return;
        }
        const auto loaded = ProjectFileCodec{}.load(project_file.value);
        if (!loaded) {
            project_settings_operation_ = loaded.result;
            return;
        }
        project_settings_draft_ = loaded.value.config;
        set_text_buffer(project_settings_name_, project_settings_draft_.name);
        set_text_buffer(project_settings_sdk_, project_settings_draft_.engine.sdk_requirement);
        set_text_buffer(project_settings_target_, project_settings_draft_.target);
        set_text_buffer(project_settings_scene_, project_settings_draft_.startup_scene.generic_string());
        set_text_buffer(project_settings_assets_, project_settings_draft_.paths.assets.generic_string());
        set_text_buffer(project_settings_source_, project_settings_draft_.paths.source.generic_string());
        set_text_buffer(project_settings_cache_, project_settings_draft_.paths.cache.generic_string());
        set_text_buffer(project_settings_pipeline_, project_settings_draft_.startup_pipelines.empty()
                                                       ? std::string_view{}
                                                       : project_settings_draft_.startup_pipelines.front().generic_string());
        project_settings_operation_ = Result::success();
        project_settings_loaded_ = true;
    }

    void build_project_settings_modal() {
        if (project_settings_pending_) {
            project_settings_pending_ = false;
            begin_project_settings();
            ImGui::OpenPopup("ProjectSettings");
        }
        if (!ImGui::BeginPopupModal("ProjectSettings", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
            return;
        }
        ImGui::TextUnformatted("Project Settings");
        ImGui::TextDisabled("Saved to Project.aegproj; startup changes take effect when the project is reopened.");
        ImGui::Separator();
        if (!project_settings_operation_) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", project_settings_operation_.message.c_str());
        }
        if (project_settings_loaded_) {
            if (ImGui::BeginTabBar("ProjectSettingsTabs")) {
                if (ImGui::BeginTabItem("General")) {
                    ImGui::InputText("Name", project_settings_name_.data(), project_settings_name_.size());
                    ImGui::InputText("Engine SDK", project_settings_sdk_.data(), project_settings_sdk_.size());
                    ImGui::InputText("Target", project_settings_target_.data(), project_settings_target_.size());
                    ImGui::InputText("Startup Scene", project_settings_scene_.data(), project_settings_scene_.size());
                    ImGui::InputText("Startup Pipeline", project_settings_pipeline_.data(),
                                     project_settings_pipeline_.size());
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Directories")) {
                    ImGui::BeginDisabled();
                    ImGui::InputText("Assets", project_settings_assets_.data(), project_settings_assets_.size());
                    ImGui::InputText("Source", project_settings_source_.data(), project_settings_source_.size());
                    ImGui::InputText("Cache", project_settings_cache_.data(), project_settings_cache_.size());
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("Directory mapping is fixed for the current editor session.");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        if (ImGui::Button("Apply", ImVec2{90.0f, 0.0f}) && project_settings_loaded_) {
            project_settings_draft_.name = project_settings_name_.data();
            project_settings_draft_.engine.sdk_requirement = project_settings_sdk_.data();
            project_settings_draft_.target = project_settings_target_.data();
            project_settings_draft_.startup_scene = project_settings_scene_.data();
            project_settings_draft_.paths.assets = project_settings_assets_.data();
            project_settings_draft_.paths.source = project_settings_source_.data();
            project_settings_draft_.paths.cache = project_settings_cache_.data();
            project_settings_draft_.startup_pipelines.clear();
            if (project_settings_pipeline_[0] != '\0') {
                project_settings_draft_.startup_pipelines.emplace_back(project_settings_pipeline_.data());
            }
            if (project_settings_draft_.name.empty() || project_settings_draft_.paths.assets.empty()) {
                project_settings_operation_ = aeg::foundation::invalid_argument(
                    "Project name and Assets directory are required");
            } else {
                project_settings_operation_ = scene_runtime_->update_project_settings(project_settings_draft_);
                if (project_settings_operation_) {
                    ImGui::CloseCurrentPopup();
                    project_settings_loaded_ = false;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{90.0f, 0.0f})) {
            project_settings_loaded_ = false;
            project_settings_operation_ = Result::success();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void build_import_options_modal() {
        if (import_options_pending_) {
            import_options_pending_ = false;
            set_text_buffer(import_source_buffer_, {});
            set_text_buffer(import_destination_buffer_, "Assets");
            import_mesh_scale_ = 1.0f;
            import_generate_normals_ = true;
            import_generate_tangents_ = true;
            import_generate_mipmaps_ = true;
            import_texture_srgb_ = true;
            import_texture_compression_ = 4;
            import_operation_ = Result::success();
            ImGui::OpenPopup("ImportOptions");
        }
        if (!ImGui::BeginPopupModal("ImportOptions", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
            return;
        }
        ImGui::TextUnformatted("Import Options");
        ImGui::TextDisabled("The importer copies the source into the project and refreshes the Asset Registry.");
        ImGui::InputText("Source file", import_source_buffer_.data(), import_source_buffer_.size());
        ImGui::InputText("Destination", import_destination_buffer_.data(), import_destination_buffer_.size());
        const std::filesystem::path source = import_source_buffer_.data();
        std::string extension = source.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        const bool mesh = extension == ".fbx" || extension == ".obj" || extension == ".gltf" || extension == ".glb";
        const bool texture = extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga";
        const bool audio = extension == ".wav" || extension == ".ogg";
        if (mesh) {
            if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat("Scale", &import_mesh_scale_, 0.01f, 0.001f, 100.0f, "%.3f");
                ImGui::Checkbox("Generate Normals", &import_generate_normals_);
                ImGui::Checkbox("Generate Tangents", &import_generate_tangents_);
            }
        } else if (texture) {
            if (ImGui::CollapsingHeader("Texture", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* compression[] = {"None", "BC1", "BC3", "BC5", "BC7"};
                ImGui::Combo("Compression", &import_texture_compression_, compression, IM_ARRAYSIZE(compression));
                ImGui::Checkbox("Generate Mipmaps", &import_generate_mipmaps_);
                ImGui::Checkbox("sRGB", &import_texture_srgb_);
            }
        } else if (audio) {
            ImGui::TextDisabled("Audio import uses the project audio defaults.");
        } else {
            ImGui::TextDisabled("Select an FBX, PNG, WAV, or another supported asset file.");
        }
        if (!import_operation_) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", import_operation_.message.c_str());
        }
        const bool can_import = scene_runtime_ != nullptr && scene_runtime_->project_loaded() &&
                                import_source_buffer_[0] != '\0' && import_destination_buffer_[0] != '\0';
        if (ImGui::Button("Import", ImVec2{90.0f, 0.0f}) && can_import) {
            EditorAssetImportRequest request{};
            request.source = import_source_buffer_.data();
            request.destination_directory = import_destination_buffer_.data();
            request.options.mesh_scale = import_mesh_scale_;
            request.options.generate_normals = import_generate_normals_;
            request.options.generate_tangents = import_generate_tangents_;
            request.options.generate_mipmaps = import_generate_mipmaps_;
            request.options.texture_srgb = import_texture_srgb_;
            request.options.texture_compression = static_cast<uint8_t>(std::clamp(import_texture_compression_, 0, 4));
            import_operation_ = scene_runtime_->import_external_asset(request);
            if (import_operation_) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{90.0f, 0.0f})) {
            import_operation_ = Result::success();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void build_registry_viewer_modal() {
        if (registry_viewer_pending_) {
            registry_viewer_pending_ = false;
            set_text_buffer(registry_search_buffer_, {});
            ImGui::OpenPopup("AssetRegistry");
        }
        if (!ImGui::BeginPopupModal("AssetRegistry", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
            return;
        }
        ImGui::TextUnformatted("Asset Registry");
        ImGui::TextDisabled("Runtime registry IDs are stable across refresh and rename operations.");
        ImGui::SetNextItemWidth(280.0f);
        ImGui::InputTextWithHint("##RegistrySearch", "Search path, name or ID...", registry_search_buffer_.data(),
                                 registry_search_buffer_.size());
        const std::string_view filter{registry_search_buffer_.data()};
        const auto records = scene_runtime_ != nullptr ? scene_runtime_->asset_records()
                                                        : std::span<const aeg::application::EditorAssetRecord>{};
        if (ImGui::BeginTable("AssetRegistryTable", 8,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2{760.0f, 320.0f})) {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Deps");
            ImGui::TableSetupColumn("Path");
            ImGui::TableSetupColumn("Actions");
            ImGui::TableHeadersRow();
            for (const aeg::application::EditorAssetRecord& record : records) {
                const std::string id = std::to_string(record.id);
                if (!filter.empty() && record.display_name.find(filter) == std::string::npos &&
                    record.relative_path.find(filter) == std::string::npos && id.find(filter) == std::string::npos) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%016llx", static_cast<unsigned long long>(record.id));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(record.display_name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(editor_asset_kind_name(record.kind).data());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(editor_asset_status_name(record.status).data());
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(record.file_size));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(record.dependencies.size()));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(record.relative_path.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(record.id & 0x7fffffff));
                if (ImGui::SmallButton("Copy ID")) {
                    ImGui::SetClipboardText(id.c_str());
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Reimport") && scene_runtime_ != nullptr) {
                    notify_runtime_result(scene_runtime_->reimport_asset(record.id), "reimport_asset_registry_row");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Refresh", ImVec2{90.0f, 0.0f}) && scene_runtime_ != nullptr) {
            notify_runtime_result(scene_runtime_->refresh_assets(), "refresh_asset_registry");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2{90.0f, 0.0f})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /** @brief Handles Edit history shortcuts and project hub/document dialogs. */
    void build_editor_menu() {
        if (scene_runtime_ == nullptr) {
            return;
        }

        build_project_hub();
        build_project_settings_modal();
        build_import_options_modal();
        build_registry_viewer_modal();

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

        build_document_dialogs();
    }

    /** @brief Routes one runtime failure to the editor notification or fatal fallback. */
    void notify_runtime_result(const Result& result, std::string_view operation) noexcept {
        if (result || result.code == ResultCode::kCancelled) {
            return;
        }
        const EditorErrorSeverity severity = aeg::editor::editor_error_severity(result.code);
        fatal_runtime_error_ = fatal_runtime_error_ || severity == EditorErrorSeverity::kFatal;
        static_cast<void>(aeg::editor::guard_editor_operation([&] {
            AEG_LOG_ERROR(aeg::logging::Category::kApplication, "Editor operation {} failed [{}]: {}", operation,
                          aeg::foundation::result_code_name(result.code), result.message);
            return Result::success();
        }));
        // Automated editor runs must fail deterministically instead of waiting
        // behind a native modal error dialog.
        if (regression_mode_) {
            fatal_runtime_error_ = true;
            return;
        }
        if (severity == EditorErrorSeverity::kFatal) {
            static_cast<void>(report_result(result, operation));
            fatal_runtime_error_ = true;
            return;
        }
        const Result notification_result = error_state_.report(operation, result, severity);
        if (!notification_result) {
            static_cast<void>(report_result(notification_result, "report_editor_error"));
            return;
        }
        if (!error_state_.consume_notification_request()) {
            return;
        }
        if (!error_dialogs_enabled || error_state_.pending() == nullptr) {
            return;
        }
        static_cast<void>(aeg::editor::guard_editor_operation([] {
            return aeg::application::show_error_dialog(
                "Aether Engine Error",
                "The editor could not complete the requested operation. Details are available in the log.");
        }));
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
        // Preferences owns its complete chrome in ImGui, just like the main
        // editor and detachable workspace panels.  Keep the native window
        // borderless so its client size matches the rendered surface.
        window_desc.client_decorated = false;
        window_desc.resizable = true;
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
        imgui_desc.font_catalog =
            preference_controller_ != nullptr ? &preference_controller_->catalog() : &font_catalog_;
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
        Result first_result = Result::success();
        const auto keep_first_error = [&first_result](const Result& result) noexcept {
            if (!result && first_result) {
                first_result = result;
            }
        };
        // Closing through the window manager must have the same semantics as
        // pressing Cancel: discard the draft and restore the applied editor
        // font before tearing down the preview window.
        if (preference_model_ != nullptr && preference_model_->state().dirty && preference_controller_ != nullptr)
            keep_first_error(preference_controller_->cancel(preference_model_->state()));
        if (preference_model_ != nullptr)
            preference_model_->discard();
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
            static_cast<void>(preferences_imgui_host_.abort_frame());
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

    /** @brief Creates the Application-owned Preferences controller from bundled and user fonts. */
    [[nodiscard]] bool initialize_preference_model() {
        try {
            const std::filesystem::path preference_root = editor_preference_root();
            if (preference_root.empty()) {
                static_cast<void>(
                    report_result(aeg::foundation::invalid_state("Editor preference directory cannot be resolved"),
                                  "resolve_editor_preference_directory"));
                return false;
            }
            preference_repository_ = std::make_unique<EditorFontRepository>();
            if (!report_result(preference_repository_->initialize(preference_root / "Fonts"),
                               "initialize_editor_font_repository")) {
                return false;
            }
            preference_store_ = std::make_unique<EditorPreferenceStore>(preference_root / "preferences.bin");
            preference_controller_ = std::make_unique<EditorPreferenceController>(EditorPreferenceControllerDesc{
                preference_repository_.get(), preference_store_.get(), &imgui_host_,
                [this](std::string_view key, float size) { return refresh_editor_preference_font(key, size); },
                [this]() -> aeg::foundation::ResultValue<std::optional<std::filesystem::path>> {
                    if (window_system_ == nullptr || window_ == nullptr) {
                        return {aeg::foundation::invalid_state("Editor font dialog owner is unavailable"), {}};
                    }
                    return aeg::application::open_font_file_dialog(*window_system_, window_->id());
                }});
            if (!report_result(preference_controller_->initialize(font_catalog_, default_font_key_,
                                                                  imgui_host_.active_font_size_pixels()),
                               "initialize_editor_preference_controller")) {
                return false;
            }
            preference_model_ = preference_controller_->model();
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

        if (preference_controller_ != nullptr && preference_model_ != nullptr) {
            const EditorPreferenceState state = preference_model_->state();
            Result controller_result = Result::success();
            switch (request.operation) {
                case EditorPreferenceActions::Operation::kPreview:
                    // Draft previews belong to the isolated Preferences host.
                    // Do not route them through the controller: that path also
                    // rebuilds and uploads the main editor atlas on every
                    // slider tick, which makes the UI block for hundreds of ms.
                    controller_result = preview_preference_font(state.active_font_key, state.font_size_pixels);
                    break;
                case EditorPreferenceActions::Operation::kApply:
                    controller_result = preference_controller_->apply(state);
                    break;
                case EditorPreferenceActions::Operation::kSave:
                    controller_result = preference_controller_->save(state);
                    break;
                case EditorPreferenceActions::Operation::kCancel:
                    // The Preferences preview lives in its own ImGui host. Cancel
                    // only discards the draft; restoring the main editor atlas here
                    // makes the window wait on a full font rebuild before closing.
                    controller_result = preference_controller_->discard_draft();
                    break;
                case EditorPreferenceActions::Operation::kImport:
                    controller_result = preference_controller_->request_import_font();
                    break;
                case EditorPreferenceActions::Operation::kRemove:
                    controller_result = preference_controller_->request_remove_font(request.font_key);
                    break;
            }
            if (!controller_result) {
                notify_runtime_result(controller_result, "process_editor_preference");
                static_cast<void>(preference_model_->set_error(
                    "The editor font change could not be completed. The previous font has been kept."));
                return !fatal_runtime_error_;
            }
            if (request.operation == EditorPreferenceActions::Operation::kPreview) {
                preferences_regression_preview_completed_ = preferences_regression_mode_;
                return true;
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

        if (request.operation == EditorPreferenceActions::Operation::kPreview) {
            const Result preview_result = preview_preference_font(request.font_key, request.font_size_pixels);
            if (!preview_result) {
                notify_runtime_result(preview_result, "preview_editor_font");
                if (preference_model_ != nullptr) {
                    static_cast<void>(preference_model_->set_error(
                        "This font could not be previewed. The previous font has been kept."));
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
            // The host commits only complete atlases; a rejected font leaves the
            // active font and GPU texture intact, so no recovery rebuild is needed.
            if (preference_model_ != nullptr) {
                static_cast<void>(preference_model_->set_error(
                    "The selected font could not be loaded. The current font has been kept."));
            }
            return !fatal_runtime_error_;
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

    /** @brief Refreshes the main and (when open) isolated Preferences font atlas. */
    [[nodiscard]] Result refresh_editor_preference_font(std::string_view font_key, float font_size_pixels) {
        if (const Result result = refresh_font_texture(); !result) {
            return result;
        }
        if (!preferences_imgui_host_.is_initialized() || preference_controller_ == nullptr) {
            return Result::success();
        }
        if (const Result result =
                preferences_imgui_host_.replace_font_catalog(preference_controller_->catalog(), font_key);
            !result) {
            return result;
        }
        if (const Result result = preferences_imgui_host_.set_preview_font(font_key, font_size_pixels); !result) {
            return result;
        }
        return refresh_preferences_font_texture();
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

    [[nodiscard]] ResultValue<NativeViewportRenderState*> ensure_native_viewport(WindowId window) {
        if (window_system_ == nullptr || device_ == nullptr || !window.is_valid()) {
            return {aeg::foundation::invalid_state("Native ImGui viewport requires initialized editor resources"),
                    nullptr};
        }
        if (const auto existing = native_viewports_.find(window); existing != native_viewports_.end()) {
            return {Result::success(), existing->second.get()};
        }
        const auto window_result = window_system_->find_window(window);
        if (!window_result) {
            return {window_result.result, nullptr};
        }
        const aeg::platform::WindowSurfaceBinding binding = window_result.value->surface_binding();
        if (!binding.is_valid()) {
            return {aeg::foundation::invalid_state("Native ImGui viewport has no surface binding"), nullptr};
        }
        auto state = std::make_unique<NativeViewportRenderState>();
        state->window = window_result.value;
        SurfaceDesc surface_desc{};
        surface_desc.native_display_handle = binding.display_handle;
        surface_desc.native_window_handle = binding.window_handle;
        surface_desc.name = aeg::rhi::DebugName{"Editor.NativeViewport.Surface"};
        const auto surface_result = device_->create_surface(surface_desc);
        if (!surface_result) {
            return {surface_result.result, nullptr};
        }
        state->surface = surface_result.value;
        const aeg::platform::WindowState window_state = state->window->state();
        SwapchainDesc swapchain_desc{};
        swapchain_desc.surface = state->surface;
        swapchain_desc.extent = {window_state.extent.width, window_state.extent.height};
        swapchain_desc.format = Format::kUnknown;
        swapchain_desc.present_mode = PresentMode::kVsync;
        swapchain_desc.min_image_count = 2;
        swapchain_desc.frames_in_flight = 2;
        swapchain_desc.image_capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        swapchain_desc.name = aeg::rhi::DebugName{"Editor.NativeViewport.Swapchain"};
        const auto swapchain_result = device_->create_swapchain(swapchain_desc);
        if (!swapchain_result) {
            static_cast<void>(device_->destroy_surface(state->surface));
            return {swapchain_result.result, nullptr};
        }
        state->swapchain = swapchain_result.value;
        const auto info_result = device_->get_swapchain_info(state->swapchain);
        if (!info_result || info_result.value.format == Format::kUnknown || info_result.value.extent.width == 0 ||
            info_result.value.extent.height == 0) {
            static_cast<void>(device_->destroy_swapchain(state->swapchain));
            static_cast<void>(device_->destroy_surface(state->surface));
            return {info_result ? aeg::foundation::invalid_state("Native ImGui viewport swapchain is invalid")
                                : info_result.result,
                    nullptr};
        }
        state->swapchain_info = info_result.value;
        try {
            state->frame_context = std::make_unique<FrameContext>(*device_);
            state->render_graph = std::make_unique<RenderGraph>(*device_);
            state->imgui_renderer = std::make_unique<ImGuiRenderer>(*device_, texture_registry_);
        } catch (const std::bad_alloc&) {
            static_cast<void>(device_->destroy_swapchain(state->swapchain));
            static_cast<void>(device_->destroy_surface(state->surface));
            return {aeg::foundation::out_of_memory("Native ImGui viewport rendering allocation failed"), nullptr};
        }
        Result result = state->imgui_renderer->initialize();
        if (result)
            result = aeg::renderer::register_render_pipeline(state->registry);
        if (result)
            result = aeg::editor::register_imgui_render_feature(state->registry, *state->imgui_renderer);
        if (result)
            result = state->frame_host.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline"));
        if (result) {
            state->orchestrator = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
                state->registry, state->scheduler, *state->frame_context, *state->render_graph);
            result = state->orchestrator->register_pipeline(state->frame_host);
        }
        if (result)
            result = state->orchestrator->initialize();
        if (!result) {
            if (state->orchestrator != nullptr)
                static_cast<void>(state->orchestrator->shutdown());
            static_cast<void>(aeg::editor::unregister_imgui_render_feature(state->registry));
            static_cast<void>(state->imgui_renderer->shutdown());
            static_cast<void>(device_->destroy_swapchain(state->swapchain));
            static_cast<void>(device_->destroy_surface(state->surface));
            return {result, nullptr};
        }
        NativeViewportRenderState* output = state.get();
        native_viewports_.emplace(window, std::move(state));
        return {Result::success(), output};
    }

    [[nodiscard]] Result recreate_native_viewport_swapchain(NativeViewportRenderState& state) {
        if (device_ == nullptr || state.window == nullptr || !state.swapchain.is_valid()) {
            return aeg::foundation::invalid_state("Native ImGui viewport swapchain is not initialized");
        }
        const aeg::platform::WindowState window_state = state.window->state();
        if (window_state.minimized || !window_state.extent.is_valid()) {
            state.resize_pending = false;
            return Result::success();
        }
        SwapchainDesc desc{};
        desc.surface = state.surface;
        desc.extent = {window_state.extent.width, window_state.extent.height};
        desc.present_mode = PresentMode::kVsync;
        desc.min_image_count = 2;
        desc.frames_in_flight = 2;
        desc.image_capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        if (const Result result = device_->recreate_swapchain(state.swapchain, desc); !result) {
            return result;
        }
        const auto info_result = device_->get_swapchain_info(state.swapchain);
        if (!info_result)
            return info_result.result;
        state.swapchain_info = info_result.value;
        state.render_graph->reset();
        state.resize_pending = false;
        return Result::success();
    }

    [[nodiscard]] Result run_native_viewport_frame(const ImGuiDrawPacket& draw_packet) {
        const auto state_result = ensure_native_viewport(draw_packet.window);
        if (!state_result)
            return state_result.result;
        NativeViewportRenderState& state = *state_result.value;
        if (state.resize_pending) {
            if (const Result result = recreate_native_viewport_swapchain(state); !result)
                return result;
        }
        if (state.window->state().minimized)
            return Result::success();
        const ImGuiRenderTargetDesc target{aeg::engine::kSwapchainImageResourceId, state.swapchain_info.format,
                                           state.swapchain_info.extent, ResourceUsage::kPresent};
        aeg::engine::EngineFrameDesc frame_desc{};
        frame_desc.fixed_tick = frame_count_;
        frame_desc.acquire_swapchain_image = true;
        frame_desc.swapchain = state.swapchain;
        frame_desc.swapchain_image_desc =
            make_swapchain_image_desc_for(state.swapchain_info, "Editor.NativeViewport.SwapchainImage");
        frame_desc.present_acquired_frame = true;
        frame_desc.render_feature_preparation = [&draw_packet, &state,
                                                 target](const aeg::engine::EngineFramePreparationContext& context) {
            if (context.acquired_frame == nullptr || !context.swapchain_resource.is_valid()) {
                return aeg::foundation::invalid_state("Native ImGui viewport requires an acquired swapchain image");
            }
            const auto clear_result = context.render_graph.add_pass(
                "Editor.NativeViewport.ClearSwapchain", QueueClass::kGraphics,
                [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                    return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kClear, aeg::rhi::StoreOp::kStore,
                                                        ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f},
                                                        ResourceUsage::kColorAttachmentWrite);
                },
                [](aeg::render_graph::PassExecutionContext& pass) {
                    if (const Result result = pass.begin_rendering(); !result)
                        return result;
                    return pass.end_rendering();
                });
            if (!clear_result)
                return clear_result.result;
            return state.imgui_renderer->prepare_frame(context.frame_context, context.render_graph, draw_packet,
                                                       target);
        };
        return state.orchestrator->run_frame(frame_desc);
    }

    void destroy_native_viewport(WindowId window) noexcept {
        const auto iterator = native_viewports_.find(window);
        if (iterator == native_viewports_.end())
            return;
        NativeViewportRenderState& state = *iterator->second;
        if (state.orchestrator != nullptr) {
            static_cast<void>(state.orchestrator->shutdown());
            // The destructor still borrows the frame context and render graph.
            // Release it before those services, just as for the main viewport.
            state.orchestrator.reset();
        }
        static_cast<void>(aeg::editor::unregister_imgui_render_feature(state.registry));
        if (state.imgui_renderer != nullptr) {
            static_cast<void>(state.imgui_renderer->release_prepared_frame());
            static_cast<void>(state.imgui_renderer->shutdown());
        }
        state.render_graph.reset();
        state.frame_context.reset();
        if (device_ != nullptr) {
            static_cast<void>(device_->wait_idle());
            if (state.swapchain.is_valid())
                static_cast<void>(device_->destroy_swapchain(state.swapchain));
            if (state.surface.is_valid())
                static_cast<void>(device_->destroy_surface(state.surface));
        }
        native_viewports_.erase(iterator);
    }

    void release_stale_native_viewports(const std::vector<ImGuiDrawPacket>& packets) noexcept {
        std::unordered_set<WindowId, WindowIdHash> active_windows;
        for (const ImGuiDrawPacket& packet : packets)
            active_windows.insert(packet.window);
        std::vector<WindowId> stale;
        for (const auto& [window, state] : native_viewports_) {
            if (!active_windows.contains(window))
                stale.push_back(window);
        }
        for (WindowId window : stale)
            destroy_native_viewport(window);
    }

    void release_all_native_viewports() noexcept {
        std::vector<WindowId> windows;
        windows.reserve(native_viewports_.size());
        for (const auto& [window, state] : native_viewports_)
            windows.push_back(window);
        for (WindowId window : windows)
            destroy_native_viewport(window);
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
        release_all_native_viewports();
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
    /** @brief Application-owned user font repository and versioned Preference store. */
    std::unique_ptr<EditorFontRepository> preference_repository_{};
    std::unique_ptr<EditorPreferenceStore> preference_store_{};
    std::unique_ptr<EditorPreferenceController> preference_controller_{};
    /** @brief Borrowed draft model owned by the Application Preference controller. */
    EditorPreferenceModel* preference_model_{nullptr};
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
    /** Engine render targets for ImGui-created native panel viewports. */
    std::unordered_map<WindowId, std::unique_ptr<NativeViewportRenderState>, WindowIdHash> native_viewports_{};
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
    bool project_hub_pending_{false};
    bool project_hub_close_requested_{false};
    bool project_hub_initialized_{false};
    std::filesystem::path project_hub_selected_root_{};
    std::array<char, 512> project_hub_open_path_buffer_{};
    std::array<char, 512> project_hub_path_buffer_{};
    std::array<char, 128> project_hub_name_buffer_{};
    int project_hub_template_index_{0};
    int project_hub_sdk_index_{0};
    int project_hub_pipeline_index_{0};
    Result project_hub_operation_{Result::success()};
    bool project_settings_pending_{false};
    bool project_settings_loaded_{false};
    ProjectConfig project_settings_draft_{};
    std::array<char, 128> project_settings_name_{};
    std::array<char, 64> project_settings_sdk_{};
    std::array<char, 128> project_settings_target_{};
    std::array<char, 256> project_settings_scene_{};
    std::array<char, 128> project_settings_pipeline_{};
    std::array<char, 128> project_settings_assets_{};
    std::array<char, 128> project_settings_source_{};
    std::array<char, 128> project_settings_cache_{};
    Result project_settings_operation_{Result::success()};
    bool import_options_pending_{false};
    std::array<char, 512> import_source_buffer_{};
    std::array<char, 128> import_destination_buffer_{};
    float import_mesh_scale_{1.0f};
    bool import_generate_normals_{true};
    bool import_generate_tangents_{true};
    bool import_generate_mipmaps_{true};
    bool import_texture_srgb_{true};
    int import_texture_compression_{4};
    Result import_operation_{Result::success()};
    bool registry_viewer_pending_{false};
    std::array<char, 256> registry_search_buffer_{};
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
    try {
        error_dialogs_enabled = parse_frame_limit(argc, argv) == 0;
#if defined(_MSC_VER) && defined(_DEBUG)
        if (!error_dialogs_enabled) {
            _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        }
#endif
        const auto console_interrupt_result = aeg::application::install_console_interrupt_controller();
        if (!console_interrupt_result || console_interrupt_result.value == nullptr) {
            return 1;
        }
        EditorApplication application;
        const std::filesystem::path resource_root =
            std::filesystem::absolute(std::filesystem::path{argv[0]}).parent_path();
        if (!application.initialize(resource_root, parse_path_argument(argc, argv, "--project"))) {
            return 1;
        }
        return application.run(parse_frame_limit(argc, argv), has_argument(argc, argv, "--stress-resize"),
                               has_argument(argc, argv, "--open-preferences"),
                               has_argument(argc, argv, "--stress-native-viewports"), *console_interrupt_result.value);
    } catch (...) {
        // Startup is also an application boundary: a malformed argument,
        // filesystem failure, or backend exception must have a controlled exit.
        return 1;
    }
}
