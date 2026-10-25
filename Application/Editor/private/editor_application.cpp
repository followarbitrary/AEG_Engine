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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "Components/component_ids.hpp"
#include "application_platform.hpp"
#include "concurrency.hpp"
#include "editor_error.hpp"
#include "editor_imgui.hpp"
#include "composition/editor_feature_graph_panel.hpp"
#include "composition/editor_render_graph_inspection.hpp"
#include "editor_preferences.hpp"
#include "editor_scene_runtime.hpp"
#include "editor_project_operations.hpp"
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
#include "editor_main_feature_workflow_probe.hpp"
#endif
#include "editor_toolbar.hpp"
#include "editor_ui_interaction.hpp"
#include "editor_workspace.hpp"
#include "engine_frame_orchestrator.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include "font/editor_system_font_catalog.hpp"
#include "frame_context.hpp"
#include "feature_graph_io.hpp"
#include "hierarchy_ui_layout.hpp"
#include "imgui_internal.h"
#include "imgui_renderer.hpp"
#include "imgui_render_execution.hpp"
#include "imgui_texture_registry.hpp"
#include "input_system.hpp"
#include "logging.hpp"
#include "pipeline.hpp"
#include "preferences/editor_preference_controller.hpp"
#include "project_context.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "renderer_variants_internal.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "texture_uploader.hpp"

struct ImGuiWindow;

namespace {
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
std::unique_ptr<aeg::editor_test::MainFeatureWorkflowProbe> feature_workflow_probe;
#endif
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
    // Keep spacing consistent and reserve accent colors for interaction states.
    style.WindowPadding = ImVec2{10.0f, 10.0f};
    style.FramePadding = ImVec2{7.0f, 5.0f};
    style.ItemSpacing = ImVec2{7.0f, 6.0f};
    style.ItemInnerSpacing = ImVec2{6.0f, 4.0f};
    style.ScrollbarSize = 12.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 3.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.DockingSeparatorSize = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4{0.91f, 0.95f, 0.96f, 1.0f};
    colors[ImGuiCol_MenuBarBg] = ImVec4{0.022f, 0.052f, 0.070f, 1.0f};
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_MenuBarBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_MenuBarBg];
    colors[ImGuiCol_Tab] = ImVec4{0.035f, 0.078f, 0.102f, 1.0f};
    colors[ImGuiCol_TabSelected] = ImVec4{0.052f, 0.122f, 0.142f, 1.0f};
    colors[ImGuiCol_TabDimmed] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TabDimmedSelected] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TabHovered] = ImVec4{0.075f, 0.18f, 0.20f, 1.0f};
    colors[ImGuiCol_TabSelectedOverline] = ImVec4{0.42f, 0.87f, 0.73f, 1.0f};
    colors[ImGuiCol_TabDimmed] = ImVec4{0.028f, 0.064f, 0.084f, 1.0f};
    colors[ImGuiCol_TabDimmedSelected] = ImVec4{0.045f, 0.105f, 0.125f, 1.0f};
    colors[ImGuiCol_DockingEmptyBg] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TextDisabled] = ImVec4{0.40f, 0.51f, 0.56f, 1.0f};
    colors[ImGuiCol_WindowBg] = ImVec4{0.020f, 0.047f, 0.063f, 1.0f};
    colors[ImGuiCol_ChildBg] = ImVec4{0.028f, 0.068f, 0.088f, 1.0f};
    colors[ImGuiCol_PopupBg] = ImVec4{0.045f, 0.094f, 0.118f, 0.98f};
    colors[ImGuiCol_Border] = ImVec4{0.105f, 0.205f, 0.245f, 1.0f};
    colors[ImGuiCol_FrameBg] = ImVec4{0.050f, 0.112f, 0.142f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = ImVec4{0.075f, 0.178f, 0.195f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = ImVec4{0.085f, 0.245f, 0.225f, 1.0f};
    colors[ImGuiCol_Button] = ImVec4{0.047f, 0.110f, 0.140f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = ImVec4{0.078f, 0.190f, 0.205f, 1.0f};
    colors[ImGuiCol_ButtonActive] = ImVec4{0.090f, 0.285f, 0.255f, 1.0f};
    colors[ImGuiCol_Header] = ImVec4{0.050f, 0.152f, 0.162f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = ImVec4{0.078f, 0.225f, 0.220f, 1.0f};
    colors[ImGuiCol_HeaderActive] = ImVec4{0.105f, 0.335f, 0.295f, 1.0f};
    colors[ImGuiCol_CheckMark] = ImVec4{0.42f, 0.87f, 0.73f, 1.0f};
    colors[ImGuiCol_SliderGrab] = ImVec4{0.33f, 0.68f, 0.61f, 1.0f};
    colors[ImGuiCol_SliderGrabActive] = ImVec4{0.42f, 0.87f, 0.73f, 1.0f};
    colors[ImGuiCol_Separator] = ImVec4{0.105f, 0.205f, 0.245f, 1.0f};
    colors[ImGuiCol_SeparatorHovered] = ImVec4{0.27f, 0.60f, 0.57f, 1.0f};
    colors[ImGuiCol_SeparatorActive] = ImVec4{0.42f, 0.87f, 0.73f, 1.0f};
    colors[ImGuiCol_ScrollbarBg] = ImVec4{0.016f, 0.038f, 0.050f, 1.0f};
    colors[ImGuiCol_ScrollbarGrab] = ImVec4{0.12f, 0.24f, 0.28f, 1.0f};
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4{0.18f, 0.36f, 0.39f, 1.0f};
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4{0.24f, 0.48f, 0.47f, 1.0f};
    colors[ImGuiCol_DockingPreview] = ImVec4{0.25f, 0.72f, 0.64f, 0.35f};
    colors[ImGuiCol_NavHighlight] = ImVec4{0.32f, 0.76f, 0.67f, 0.80f};
}

std::string property_display_name(std::string_view name) {
    if (const auto separator = name.find_last_of('.'); separator != std::string_view::npos)
        name.remove_prefix(separator + 1);
    std::string result;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto current = static_cast<unsigned char>(name[index]);
        if (current == '_') {
            if (!result.empty() && result.back() != ' ') result += ' ';
            continue;
        }
        if (index > 0 && std::isupper(current) &&
            (std::islower(static_cast<unsigned char>(name[index - 1])) ||
             (std::isupper(static_cast<unsigned char>(name[index - 1])) && index + 1 < name.size() &&
              std::islower(static_cast<unsigned char>(name[index + 1])))))
            result += ' ';
        result += result.empty() || result.back() == ' ' ? static_cast<char>(std::toupper(current)) : name[index];
    }
    return result;
}

void ellipsized_panel_text(std::string_view text, float width) {
    width = std::max(1.0f, width);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end{start.x + width, start.y + ImGui::GetTextLineHeight()};
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), start, end, end.x, text.data(),
                              text.data() + text.size(), nullptr);
    ImGui::Dummy(ImVec2{width, ImGui::GetTextLineHeight()});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), text.data());
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

std::string path_text(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

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

void begin_editor_panel_window(const char* title, const aeg::editor::EditorPanelBuildContext& context) {
    const ImGuiWindowFlags window_flags = context.native_window
                                              ? ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                                    ImGuiWindowFlags_NoScrollWithMouse
                                              : ImGuiWindowFlags_None;
    if (context.native_window)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2{aeg::editor::kEditorWindowContentPadding,
                                   aeg::editor::kEditorWindowContentPadding});
    ImGui::Begin(title, nullptr, window_flags);
    if (context.native_window)
        ImGui::PopStyleVar();
    if (context.native_window)
        aeg::editor::draw_editor_window_chrome(title, {}, context.window_actions);
}

class SceneViewportPanel final : public aeg::editor::EditorPanel {
public:
    SceneViewportPanel(EditorSceneRuntime& runtime, ImGuiTextureHandle& texture_handle,
                       EditorDocumentActionCallback document_action) noexcept
        : runtime_(runtime), texture_handle_(texture_handle), document_action_(std::move(document_action)) {}

    void build_ui() override { build_ui(aeg::editor::EditorPanelBuildContext{}); }

    void build_ui(const aeg::editor::EditorPanelBuildContext& context) override {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
        begin_editor_panel_window("Scene Viewport", context);
        ImGui::PopStyleVar();
        const ImVec2 viewport_origin = ImGui::GetCursorScreenPos();
        ImDrawList* viewport_draw = ImGui::GetWindowDrawList();
        viewport_draw->ChannelsSplit(2);
        viewport_draw->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2{viewport_origin.x + 10.0f, viewport_origin.y + 10.0f});
        bool toolbar_has_item = false;
        float toolbar_line_width = 0.0f;
        float toolbar_row_width = 0.0f;
        const auto place_toolbar_item = [&](float width) {
            if (!toolbar_has_item) {
                toolbar_has_item = true;
                toolbar_line_width = ImGui::GetContentRegionAvail().x;
                toolbar_row_width = width;
                return;
            }
            if (toolbar_row_width + ImGui::GetStyle().ItemSpacing.x + width <= toolbar_line_width) {
                ImGui::SameLine();
                toolbar_row_width += ImGui::GetStyle().ItemSpacing.x + width;
            } else {
                toolbar_row_width = width;
            }
        };
        const auto place_toolbar_separator = [&] {
            constexpr float separator_width = 14.0f;
            if (!toolbar_has_item) {
                toolbar_has_item = true;
                toolbar_line_width = ImGui::GetContentRegionAvail().x;
                toolbar_row_width = separator_width;
            } else if (toolbar_row_width + 8.0f + separator_width <= toolbar_line_width) {
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextDisabled("|");
                toolbar_row_width += 8.0f + separator_width;
            } else {
                toolbar_row_width = separator_width;
                ImGui::TextDisabled("|");
            }
        };
        place_toolbar_item(ImGui::CalcTextSize("Scene").x + 44.0f);
        if (aeg::editor::toolbar_button("Scene", "New, open, save or save the scene as a new file.",
                                        aeg::editor::ToolbarIcon::None, false, true))
            ImGui::OpenPopup("SceneActions");
        if (ImGui::BeginPopup("SceneActions")) {
            ImGui::TextDisabled("Scene file");
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
            if (!last_operation_) {
                ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.3f, 1.0f}, "%s", last_operation_.message.c_str());
            }
            if (!runtime_.diagnostic().empty()) {
                ImGui::TextColored(ImVec4{1.0f, 0.75f, 0.25f, 1.0f}, "%s", std::string(runtime_.diagnostic()).c_str());
            }
            ImGui::EndPopup();
        }
        place_toolbar_separator();
        place_toolbar_item(ImGui::CalcTextSize("Orthographic").x + 24.0f);
        if (aeg::editor::toolbar_button(viewport_projection_ == 0 ? "Perspective" : "Orthographic",
                                        "Projection selector: UI preview only; scene camera is unchanged.")) {
            viewport_projection_ = viewport_projection_ == 0 ? 1 : 0;
        }
        place_toolbar_item(ImGui::CalcTextSize("Unlit").x + 24.0f);
        if (aeg::editor::toolbar_button(viewport_lit_ ? "Lit" : "Unlit",
                                        "Shading selector: UI preview only; renderer shading is unchanged.")) {
            viewport_lit_ = !viewport_lit_;
        }
        place_toolbar_item(ImGui::CalcTextSize("Show").x + 24.0f);
        if (aeg::editor::toolbar_button("Show", "Show or hide viewport overlays and transform gizmos.",
                                        aeg::editor::ToolbarIcon::None, viewport_overlays_)) {
            viewport_overlays_ = !viewport_overlays_;
        }
        place_toolbar_separator();
        place_toolbar_item(ImGui::CalcTextSize("Move").x + 24.0f);
        if (aeg::editor::toolbar_button("Move", "Translate the selected object using its axis handles.",
                                        aeg::editor::ToolbarIcon::None, gizmo_mode_ == ViewportGizmoMode::kTranslate)) {
            gizmo_mode_ = ViewportGizmoMode::kTranslate;
        }
        place_toolbar_item(ImGui::CalcTextSize("Rotate").x + 24.0f);
        if (aeg::editor::toolbar_button("Rotate", "Rotate the selected object using the rotation gizmo.",
                                        aeg::editor::ToolbarIcon::None, gizmo_mode_ == ViewportGizmoMode::kRotate)) {
            gizmo_mode_ = ViewportGizmoMode::kRotate;
        }
        place_toolbar_item(ImGui::CalcTextSize("Scale").x + 24.0f);
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
            draw_gizmo(image_min, extent, width, height);
            handle_input(image_min, extent, width, height, hovered);
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

    void build_ui() override { build_ui(aeg::editor::EditorPanelBuildContext{}); }

    void build_ui(const aeg::editor::EditorPanelBuildContext& context) override {
        begin_editor_panel_window("Hierarchy", context);
        const float actions_width = ImGui::CalcTextSize("...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - actions_width -
                                                 ImGui::GetStyle().ItemSpacing.x));
        ImGui::InputTextWithHint("##entity-filter", "Search entities...", search_, sizeof(search_));
        ImGui::SameLine();
        if (ImGui::Button("..."))
            ImGui::OpenPopup("TreeActions");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Hierarchy actions");
        if (ImGui::BeginPopup("TreeActions")) {
            if (ImGui::MenuItem("Create Root Entity"))
                last_operation_ = runtime_.create_scene_entity({}, "Entity");
            ImGui::Separator();
            if (ImGui::MenuItem("Expand All")) {
                expand_all_ = true;
                collapse_all_ = false;
            }
            if (ImGui::MenuItem("Collapse All")) {
                collapse_all_ = true;
                expand_all_ = false;
            }
            ImGui::EndPopup();
        }
        ImGui::Spacing();
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
        const auto record = std::find_if(runtime_.entities().begin(), runtime_.entities().end(),
                                         [entity](const auto& value) { return value.id == entity; });
        if (record == runtime_.entities().end()) {
            return;
        }
        if (search_[0] && runtime_.hierarchy_path(entity).value.find(search_) == std::string::npos &&
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
        if (expand_all_ || search_[0]) {
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
    char search_[128]{};
    std::vector<scene::SceneEntityId> drag_entities_{};
    bool expand_all_{false};
    bool collapse_all_{false};
    Result last_operation_{Result::success()};
};

class InspectorPanel final : public aeg::editor::EditorPanel {
public:
    explicit InspectorPanel(EditorSceneRuntime& runtime) noexcept : runtime_(runtime) {}

    void build_ui() override { build_ui(aeg::editor::EditorPanelBuildContext{}); }

    void build_ui(const aeg::editor::EditorPanelBuildContext& context) override {
        begin_editor_panel_window("Inspector", context);
        ImGui::TextColored(ImVec4{0.45f, 0.90f, 0.76f, 1.0f}, "Properties");
        ImGui::Separator();
        const auto selected_entities = runtime_.selected_entities();
        if (selected_entities.empty()) {
            finish_component_transaction();
            ImGui::Spacing();
            ImGui::TextDisabled("Nothing selected");
            ImGui::TextWrapped("Select an entity in the scene or hierarchy to inspect its properties.");
            ImGui::End();
            return;
        }
        ImGui::TextColored(ImVec4{0.45f, 0.90f, 0.76f, 1.0f}, "Selected Entity");
        ImGui::SameLine();
        ImGui::TextDisabled("%llu%s", static_cast<unsigned long long>(selected_entities.size()),
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
        ImGui::TextDisabled("Identity");
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
        ImGui::TextUnformatted(property_display_name(field.name).c_str());
        ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - 55.0f));
        ImGui::Button((resource_label(expected_kind, *id) + "##resource").c_str(), ImVec2{-1.0f, 0.0f});
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
        const std::string component_name = property_display_name(schema.value->name);
        const bool open = ImGui::TreeNodeEx(component_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
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
                    const std::string field_name = property_display_name(field.name);
                    ImGui::TextUnformatted(field_name.c_str());
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
                            ImGui::Text("%s: unsupported field kind", field_name.c_str());
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

    void build_ui() override { build_ui(aeg::editor::EditorPanelBuildContext{}); }

    void build_ui(const aeg::editor::EditorPanelBuildContext& context) override {
        begin_editor_panel_window("Assets", context);
        ImGui::TextColored(ImVec4{0.45f, 0.90f, 0.76f, 1.0f}, "ASSETS");
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("Browser");
        const float refresh_width = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - refresh_width - 10.0f));
        if (ImGui::SmallButton("Refresh")) {
            last_operation_ = runtime_.refresh_assets();
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##asset-search", "Search assets...", search_buffer_.data(), search_buffer_.size());
        ImGui::SetNextItemWidth(128.0f);
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
        const bool asset_table = ImGui::BeginTable("AssetList", 3,
                                                   ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                       ImGuiTableFlags_SizingStretchProp);
        if (asset_table) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
        }
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
            if (asset_table)
                ImGui::TableNextRow();
            if (asset_table)
                ImGui::TableSetColumnIndex(0);
            const bool selected = selected_id_ == descriptor.id && selected_kind_ == descriptor.kind;
            const float row_height = ImGui::GetFrameHeight();
            const bool clicked = ImGui::Selectable("##AssetRow", selected,
                                                   ImGuiSelectableFlags_AllowDoubleClick |
                                                       ImGuiSelectableFlags_SpanAllColumns,
                                                   ImVec2{0.0f, row_height});
            const ImVec2 row_min = ImGui::GetItemRectMin();
            const ImVec2 row_max = ImGui::GetItemRectMax();
            const ImVec2 text_min{row_min.x + 8.0f,
                                  row_min.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f};
            const ImVec2 text_max{std::max(text_min.x + 1.0f, row_max.x - 6.0f), text_min.y + ImGui::GetTextLineHeight()};
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), text_min, text_max, text_max.x, name.c_str(),
                                      name.c_str() + name.size(), nullptr);
            if (clicked) {
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
            if (asset_table) {
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", kind);
                ImGui::TableSetColumnIndex(2);
                const std::string status = std::string(editor_asset_status_name(descriptor.status));
                ImGui::TextDisabled("%s", status.c_str());
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("%s #%llu [%s]", kind, static_cast<unsigned long long>(descriptor.id),
                                    editor_asset_status_name(descriptor.status).data());
            }
        }
        if (asset_table)
            ImGui::EndTable();
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

    void build_ui() override { build_ui(aeg::editor::EditorPanelBuildContext{}); }

    void build_ui(const aeg::editor::EditorPanelBuildContext& context) override {
        begin_editor_panel_window("Project", context);
        if (!runtime_.project_loaded()) {
            ImGui::TextDisabled("No project is open.");
            ImGui::TextWrapped("Create or import a project to browse its folders and assets.");
            ImGui::End();
            return;
        }

        const std::filesystem::path root = runtime_.project_root();
        const std::string root_name = path_text(root.filename().empty() ? root : root.filename());
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.040f, 0.102f, 0.134f, 1.0f});
        ImGui::BeginChild("ProjectHeader", ImVec2{0.0f, 42.0f}, true, ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4{0.42f, 0.91f, 0.77f, 1.0f}, "Project");
        ImGui::SameLine(0.0f, 8.0f);
        const float action_width = ImGui::GetFrameHeight();
        const float root_width = std::max(40.0f, ImGui::GetContentRegionAvail().x - action_width - 12.0f);
        ellipsized_panel_text(root_name, root_width);
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - action_width - 10.0f));
        if (ImGui::Button("...##ProjectActions", ImVec2{action_width, 0.0f}))
            ImGui::OpenPopup("ProjectActions");
        if (ImGui::BeginPopup("ProjectActions")) {
            if (ImGui::MenuItem("Sync Assets"))
                last_operation_ = runtime_.refresh_assets();
            if (ImGui::MenuItem("Close Project") && close_project_)
                close_project_();
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##project-tree-filter", "Search project...", filter_.data(), filter_.size());
        ImGui::Separator();

        std::string selected_extension = selected_path_.extension().string();
        std::transform(selected_extension.begin(), selected_extension.end(), selected_extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool retired_asset = selected_extension == ".aegpipe" || selected_extension == ".aegframe" ||
                                   selected_extension == ".aegpipeline" ||
                                   selected_extension == ".pipeline";
        const std::vector<EditorResourceDescriptor> descriptors = runtime_.resource_descriptors();
        const float details_height = selected_path_.empty() ? 0.0f : (retired_asset ? 112.0f : 74.0f);
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
            if (retired_asset) {
                ImGui::Separator();
                ImGui::TextWrapped("Unsupported retired author asset. Use a project FeatureGraph (.aegfeature).");
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
        } else if (extension == ".aegfeature") {
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
        if (extension == ".aegfeature")
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

    void build_ui() override { build_ui(aeg::editor::EditorPanelBuildContext{}); }

    void build_ui(const aeg::editor::EditorPanelBuildContext& context) override {
        begin_editor_panel_window("Console", context);
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

class FlowGraphWorkspacePanel final : public aeg::editor::EditorPanel {
public:
    using FeatureDocumentProvider = std::function<aeg::application::EditorFeatureGraphDocument*()>;
    using FeaturePathProvider = std::function<std::filesystem::path()>;
    using FeatureAuthoringProvider =
        std::function<std::span<const aeg::application::EditorProjectFeatureGraphDocument>()>;
    using FeatureCatalogProvider = std::function<const aeg::feature::FeatureCatalog*()>;
    using FeatureAuthorityProvider = std::function<std::span<const aeg::feature::FeatureDefinitionId>()>;
    using FeatureFocusReporter = std::function<void(bool)>;
    using FeatureActionsProvider = std::function<aeg::application::EditorFeatureGraphPanel::Actions()>;
    using FeatureSelectionAction = std::function<Result(const std::filesystem::path&)>;
    using FeatureRecoveryAction = std::function<Result(const std::filesystem::path&,
        const aeg::application::authoring::FeatureGraphFileRevision&,
        const aeg::application::authoring::FeatureGraphFileRevision&)>;
    using ProjectRootProvider = std::function<std::filesystem::path()>;
    using PreviewExecutionProvider = std::function<aeg::foundation::ResultValue<
        aeg::application::runtime::FeatureRuntimeExecutionSnapshot>()>;
    using RenderGraphResult = aeg::foundation::ResultValue<
        std::shared_ptr<const aeg::application::EditorRenderGraphSnapshot>>;
    using RenderGraphProvider = std::function<RenderGraphResult()>;
    FlowGraphWorkspacePanel(FeatureDocumentProvider feature_provider,
                            FeaturePathProvider feature_path_provider, FeatureAuthoringProvider feature_authoring_provider,
                            FeatureCatalogProvider feature_catalog_provider,
                            FeatureAuthorityProvider feature_authority_provider,
                            FeatureFocusReporter feature_focus_reporter,
                            ProjectRootProvider project_root_provider,
                            std::function<Result()> save_draft, FeatureActionsProvider feature_actions,
                            FeatureSelectionAction select_feature_graph,
                            FeatureRecoveryAction recover_feature_graph,
                            PreviewExecutionProvider preview_execution, RenderGraphProvider render_graph)
        : feature_provider_(std::move(feature_provider)),
          feature_document_path_provider_(std::move(feature_path_provider)),
          feature_authoring_provider_(std::move(feature_authoring_provider)),
          feature_catalog_provider_(std::move(feature_catalog_provider)),
          feature_authority_provider_(std::move(feature_authority_provider)),
          feature_focus_reporter_(std::move(feature_focus_reporter)),
          project_root_provider_(std::move(project_root_provider)),
          save_draft_(std::move(save_draft)), feature_actions_provider_(std::move(feature_actions)),
          select_feature_graph_(std::move(select_feature_graph)),
          recover_feature_graph_(std::move(recover_feature_graph)),
          preview_execution_(std::move(preview_execution)), render_graph_(std::move(render_graph)) {}

    void build_ui() override { build_ui(aeg::editor::EditorPanelBuildContext{}); }

    void build_ui(const aeg::editor::EditorPanelBuildContext& context) override {
        if (auto* feature_document = feature_provider_(); feature_document != nullptr) {
            const auto authored = feature_authoring_provider_ ? feature_authoring_provider_()
                                                              : std::span<const aeg::application::EditorProjectFeatureGraphDocument>{};
            auto actions = feature_actions_provider_ ? feature_actions_provider_()
                                                      : aeg::application::EditorFeatureGraphPanel::Actions{};
            std::filesystem::path requested_graph;
            actions.draw_graph_selector = [&] {
                draw_frame_render_inspection();
                draw_source_diagnostics(authored);
                if (authored.empty()) return;
                const auto current_path = feature_document_path_provider_ ? feature_document_path_provider_()
                                                                          : std::filesystem::path{};
                const std::string current_label = current_path.empty() ? "Select FeatureGraph"
                                                                       : current_path.filename().string();
                ImGui::BeginDisabled(!select_feature_graph_);
                if (ImGui::BeginCombo("Select FeatureGraph", current_label.c_str())) {
                    for (const auto& document : authored) {
                        if (!document.load_result)
                            continue;
                        const bool selected = document.path == current_path;
                        const std::string label = document.path.generic_string();
                        if (ImGui::Selectable(label.c_str(), selected) && select_feature_graph_)
                            requested_graph = document.path;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (!selection_operation_)
                    ImGui::TextWrapped("%s", selection_operation_.message.c_str());
            };
            actions.authored_feature_documents =
                authored;
            actions.save_draft = save_draft_;
            actions.report_focus = feature_focus_reporter_;
            actions.feature_catalog = feature_catalog_provider_ ? feature_catalog_provider_() : nullptr;
            actions.native_window = context.native_window;
            actions.window_actions = context.window_actions;
            actions.project_root = project_root_provider_ ? project_root_provider_() : std::filesystem::path{};
            actions.compiled_features = feature_authority_provider_ ? feature_authority_provider_()
                                                                   : std::span<const aeg::feature::FeatureDefinitionId>{};
            feature_panel_.draw(*feature_document, feature_document_path_provider_ ? feature_document_path_provider_()
                                                                                 : std::filesystem::path{}, actions);
            // The panel still borrows feature_document during draw. Replacing the
            // selected root here keeps that document alive until its last use.
            if (!requested_graph.empty() && select_feature_graph_)
                selection_operation_ = select_feature_graph_(requested_graph);
            restore_requested_source();
            return;
        }
        const ImGuiWindowFlags window_flags = context.native_window
                                                  ? ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                                        ImGuiWindowFlags_NoScrollWithMouse
                                                  : ImGuiWindowFlags_None;
        if (context.native_window)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
        ImGui::Begin("Flow Graph###FlowGraph", nullptr, window_flags);
        if (context.native_window)
            ImGui::PopStyleVar();
        if (context.native_window)
            aeg::editor::draw_editor_window_chrome("Flow Graph", "No authored graph", context.window_actions);
        if (feature_focus_reporter_)
            feature_focus_reporter_(false);
        const auto authored = feature_authoring_provider_ ? feature_authoring_provider_()
                                                          : std::span<const aeg::application::EditorProjectFeatureGraphDocument>{};
        const bool reported_diagnostic = draw_source_diagnostics(authored);
        if (!reported_diagnostic)
            ImGui::TextDisabled("Open a project with an authored .aegfeature document to edit its FlowGraph.");
        draw_frame_render_inspection();
        ImGui::End();
        restore_requested_source();
    }

private:
    struct RecoveryRequest final {
        std::filesystem::path path{};
        aeg::application::authoring::FeatureGraphFileRevision source{}, backup{};
    };
    bool draw_source_diagnostics(std::span<const aeg::application::EditorProjectFeatureGraphDocument> documents) {
        bool reported = false;
        for (const auto& document : documents) {
            if (document.load_result) continue;
            if (!reported) ImGui::TextUnformatted("FeatureGraph diagnostics");
            reported = true;
            const auto utf8 = document.path.generic_u8string();
            const std::string label(utf8.begin(), utf8.end());
            ImGui::PushID(label.c_str());
            ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.30f, 1.0f}, "%s", label.c_str());
            ImGui::TextWrapped("%s", document.load_result.message.c_str());
            ImGui::BeginDisabled(!recover_feature_graph_);
            if (ImGui::Button("Inspect recoverable backup")) {
                inspected_recovery_.reset();
                auto source = aeg::application::authoring::inspect_feature_graph_file_revision(document.path);
                auto backup_path = document.path; backup_path += ".bak";
                auto backup = aeg::application::authoring::load_feature_graph_source(backup_path);
                recovery_operation_ = !source ? source.result : backup.result;
                if (source && backup) {
                    recovery_version_ = backup.value.document.graph_version;
                    inspected_recovery_ = RecoveryRequest{document.path, std::move(source.value),
                                                         std::move(backup.value.revision)};
                }
            }
            ImGui::EndDisabled();
            if (inspected_recovery_ && inspected_recovery_->path == document.path) {
                ImGui::TextWrapped("Restore graph version %llu. The displaced source will be retained for recovery.",
                    static_cast<unsigned long long>(recovery_version_));
                ImGui::BeginDisabled(!recover_feature_graph_);
                if (ImGui::Button("Restore inspected backup")) requested_recovery_ = inspected_recovery_;
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        if (!recovery_operation_) ImGui::TextWrapped("%s", recovery_operation_.message.c_str());
        return reported;
    }
    void restore_requested_source() {
        if (!requested_recovery_) return;
        auto request = std::move(*requested_recovery_);
        requested_recovery_.reset();
        recovery_operation_ = recover_feature_graph_ ? recover_feature_graph_(request.path, request.source, request.backup)
            : aeg::foundation::invalid_state("FeatureGraph recovery is unavailable");
        if (recovery_operation_) inspected_recovery_.reset();
    }
    static void draw_execution_source(const aeg::application::runtime::FeatureRuntimeExecutionSnapshot& source) {
        ImGui::Text("Asset %s  generation %llu", source.asset_uuid.c_str(),
                    static_cast<unsigned long long>(source.generation.value()));
        ImGui::Text("Graph %llu  draft %llu  contract %llu", static_cast<unsigned long long>(source.graph_version),
                    static_cast<unsigned long long>(source.draft_revision),
                    static_cast<unsigned long long>(source.contract_digest));
        if (!source.source_path.empty())
            ImGui::TextWrapped("Source %s", source.source_path.generic_string().c_str());
        if (!ImGui::BeginTable("PublishedFeatureInstances", 7,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            return;
        for (const auto* label : {"Clock", "Order", "Definition", "Source node", "Instance", "Invocation", "Module"})
            ImGui::TableSetupColumn(label);
        ImGui::TableHeadersRow();
        for (const auto& plan : source.plans) {
            for (std::size_t index = 0; index < plan.instances.size(); ++index) {
                const auto row = std::find_if(source.provenance.begin(), source.provenance.end(),
                    [&](const auto& item) { return item.runtime.instance_id == plan.instances[index]; });
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                ImGui::Text("%llu", static_cast<unsigned long long>(plan.clock.value()));
                ImGui::TableSetColumnIndex(1); ImGui::Text("%zu", index + 1);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", static_cast<unsigned long long>(plan.instances[index].value()));
                if (row == source.provenance.end()) {
                    ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("No captured provenance");
                    continue;
                }
                const auto& origin = row->runtime;
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(origin.definition_id.value()));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", static_cast<unsigned long long>(row->authored_node_id.value()));
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%llu", static_cast<unsigned long long>(origin.invocation_id.value()));
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%llu", static_cast<unsigned long long>(origin.owner_module.value()));
            }
        }
        ImGui::EndTable();
    }

    void draw_frame_render_inspection() {
        ImGui::Separator();
        if (!ImGui::CollapsingHeader("Frame / Render (read-only)"))
            return;
        // Native author canvases fill the remaining window with their own
        // scrolling child. Keep this actual-source view above that canvas and
        // give it an independent scroll region in both presentations.
        const float height = std::clamp(ImGui::GetContentRegionAvail().y * 0.40f, 160.0f, 320.0f);
        ImGui::BeginChild("FrameRenderSources", ImVec2{0.0f, height}, true);
        draw_frame_render_sources();
        ImGui::EndChild();
    }

    void draw_frame_render_sources() {
        if (ImGui::TreeNodeEx("Published Preview ExecutionPlan", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto observed = preview_execution_();
            if (observed) draw_execution_source(observed.value);
            else ImGui::TextWrapped("%s", observed.result.message.c_str());
            ImGui::TreePop();
        }
        if (!ImGui::TreeNodeEx("Last successful RenderGraph", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        const auto observed = render_graph_();
        if (!observed || !observed.value) {
            ImGui::TextWrapped("%s", observed.result.message.c_str());
            ImGui::TreePop();
            return;
        }
        const auto& graph = *observed.value;
        ImGui::Text("Frame %llu  revision %llu", static_cast<unsigned long long>(graph.frame_index),
                    static_cast<unsigned long long>(graph.plan.revision));
        ImGui::Text("Resources %u  passes %u  culled %u  batches %u", graph.compilation.resource_count,
                    graph.compilation.pass_count, graph.compilation.culled_pass_count, graph.compilation.batch_count);
        if (ImGui::BeginTable("CapturedFeatureWork", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto* label : {"Order / pass", "Batch", "Live", "Source / definition", "Instance / invocation", "Operation"})
                ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            for (const auto& pass : graph.plan.pass_plans) {
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u / %u", pass.topological_index, pass.pass.index);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%u", pass.batch_index);
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(pass.live ? "yes" : "no");
                ImGui::TableSetColumnIndex(3);
                if (!pass.feature_work) { ImGui::TextDisabled("Frame infrastructure"); continue; }
                const auto& work = *pass.feature_work;
                const auto source = std::find_if(graph.sources.begin(), graph.sources.end(), [&](const auto& item) {
                    return item.generation == work.invocation.generation &&
                        std::any_of(item.provenance.begin(), item.provenance.end(), [&](const auto& row) {
                            return row.runtime.instance_id == work.instance_id && row.runtime.node_id == work.node_id &&
                                   row.runtime.definition_id == work.feature_definition_id &&
                                   row.runtime.owner_module == work.owner_module;
                        });
                });
                if (source != graph.sources.end()) ImGui::TextWrapped("%s", source->asset_uuid.c_str());
                else ImGui::TextDisabled("Unattributed Feature");
                ImGui::Text("Definition %llu", static_cast<unsigned long long>(work.feature_definition_id.value()));
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu / %llu", static_cast<unsigned long long>(work.instance_id.value()),
                            static_cast<unsigned long long>(work.invocation.invocation_id.value()));
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%u / %s", work.operation_index,
                            work.kind == aeg::render_graph::RenderWorkItemKind::kExecute ? "execute" : "prepare");
            }
            ImGui::EndTable();
        }
        if (ImGui::TreeNode("Captured source owners")) {
            for (std::size_t index = 0; index < graph.sources.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::TreeNode(graph.sources[index].asset_uuid.c_str())) {
                    draw_execution_source(graph.sources[index]);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Queue batches and dependencies")) {
            for (const auto& batch : graph.plan.batches)
                ImGui::BulletText("Batch %u  queue %u  passes %zu", batch.index,
                                  static_cast<unsigned>(batch.queue), batch.pass_indices.size());
            for (const auto& edge : graph.plan.queue_dependencies)
                ImGui::BulletText("Batch %u -> %u  hazards %u", edge.source_batch, edge.destination_batch,
                                  edge.resource_hazard_count);
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    FeatureDocumentProvider feature_provider_{};
    std::function<std::filesystem::path()> feature_document_path_provider_{};
    FeatureAuthoringProvider feature_authoring_provider_{};
    FeatureCatalogProvider feature_catalog_provider_{};
    FeatureAuthorityProvider feature_authority_provider_{};
    FeatureFocusReporter feature_focus_reporter_{};
    ProjectRootProvider project_root_provider_{};
    std::function<Result()> save_draft_{};
    FeatureActionsProvider feature_actions_provider_{};
    FeatureSelectionAction select_feature_graph_{};
    FeatureRecoveryAction recover_feature_graph_{};
    std::optional<RecoveryRequest> inspected_recovery_{}, requested_recovery_{};
    uint64_t recovery_version_{0};
    Result recovery_operation_{};
    PreviewExecutionProvider preview_execution_{};
    RenderGraphProvider render_graph_{};
    Result selection_operation_{Result::success()};
    aeg::application::EditorFeatureGraphPanel feature_panel_{};
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
        PipelineScheduler scheduler;
        aeg::engine::EngineFramePipelineHost pipeline_host{registry, scheduler};
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

    [[nodiscard]] Result launch_project_operation(aeg::tooling::ProjectOperationRequest request) {
        if (const auto synced = sync_project_operations(); !synced) return synced;
        if (scene_runtime_ == nullptr || !scene_runtime_->project_loaded() ||
            std::filesystem::weakly_canonical(request.project_file) != scene_runtime_->project_file())
            return aeg::foundation::invalid_state("Project operation requires the explicitly opened project");
        request.project_file = scene_runtime_->project_file();
        if (request.engine_directory.empty()) request.engine_directory = AEG_ENGINE_SOURCE_DIR;
        if (request.cmake_executable.empty()) request.cmake_executable = AEG_CMAKE_EXECUTABLE;
        if (request.target.empty()) request.target = project_operations_.request().target;
        if (const auto configured = project_operations_.set_request(request); !configured) return configured;
        if (const auto started = project_operations_.start(request.operation); !started) return started;
        project_operation_cli_ = true;
        project_operations_open_ = true;
        return Result::success();
    }

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
            const std::filesystem::path bundled_demo = resource_root_ / "DemoProject" / "Project.aegproj";
            if (std::filesystem::exists(bundled_demo)) {
                startup_project_ = bundled_demo;
            }
        }
        if (!startup_project_.empty()) {
            const auto project_result =
                aeg::editor::guard_editor_operation([&] { return scene_runtime_->import_project(startup_project_); });
            if (!project_result) {
                const std::string_view diagnostic{project_result.message};
                const bool retired_startup_input = project_result.code == ResultCode::kUnsupported &&
                    (diagnostic.starts_with("Retired Scene author sidecar is unsupported: ") ||
                     diagnostic.starts_with("Retired Frame/Pipeline author documents cannot be opened as a Scene") ||
                     diagnostic.starts_with("Removed project startup field: ") ||
                     diagnostic.starts_with("Removed startup asset extension in build.startup_feature_graphs: ") ||
                     diagnostic.starts_with("Unsupported project format_version") ||
                     diagnostic.starts_with("Unsupported retired project extension: ") ||
                     diagnostic.starts_with("Unsupported retired project header: "));
                // These inputs are rejected before replacing the initialized
                // Preview. Report that usable startup state without changing
                // device, backend, or Runtime failure classification.
                if (retired_startup_input && !scene_runtime_->project_loaded()) {
                    static_cast<void>(aeg::editor::guard_editor_operation([&] {
                        AEG_LOG_WARNING(aeg::logging::Category::kApplication,
                            "Editor operation open_startup_project rejected [{}]: {}",
                            aeg::foundation::result_code_name(project_result.code), project_result.message);
                        return Result::success();
                    }));
                } else {
                    notify_runtime_result(project_result, "open_startup_project");
                }
                if (fatal_runtime_error_)
                    return false;
            } else {
                sync_flowgraph_document();
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
        if (!report_result(frame_host_.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline")),
                           "bind_editor_render_pipeline")) {
            return false;
        }
        orchestrator_ = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
            pipeline_host_, *frame_context_, *render_graph_);
        if (!report_result(orchestrator_->register_pipeline(
                               aeg::engine::EngineFramePipelineRegistration{frame_host_}),
                           "register_editor_frame_pipeline")) {
            return false;
        }
        if (!report_result(orchestrator_->initialize(), "initialize_engine_frame_orchestrator")) {
            return false;
        }

        auto flowgraph_panel = std::make_unique<FlowGraphWorkspacePanel>(
            [this] { return feature_graph_document_.get(); },
            [this] { return feature_graph_document_path_; },
            [this] {
                return scene_runtime_ == nullptr
                           ? std::span<const aeg::application::EditorProjectFeatureGraphDocument>{}
                           : scene_runtime_->authored_feature_graphs();
            },
            [this] {
                return scene_runtime_ == nullptr ? nullptr : &scene_runtime_->authored_feature_catalog();
            },
            [this] { return scene_runtime_ == nullptr ? std::span<const aeg::feature::FeatureDefinitionId>{} : scene_runtime_->authored_compiled_features(); },
            [this](bool focused) { feature_graph_focused_ = focused; },
            [this] { return scene_runtime_ == nullptr ? std::filesystem::path{} : scene_runtime_->project_root(); },
            [this] { return save_feature_graph_document(); },
            [this] {
                aeg::application::EditorFeatureGraphPanel::Actions actions;
                if (scene_runtime_ == nullptr) return actions;
                actions.project_file = scene_runtime_->project_file();
                actions.asset_kind_resolver = aeg::application::make_editor_asset_kind_resolver(actions.project_file);
                actions.active_module_catalog = scene_runtime_->active_feature_module_catalog();
                actions.inspect_module_catalog = [this](const std::filesystem::path& manifest) {
                    return scene_runtime_->inspect_feature_module_catalog(manifest);
                };
                actions.select_module = [this](const std::filesystem::path& manifest, std::string_view configuration) {
                    return scene_runtime_->select_feature_module(manifest, configuration);
                };
                actions.select_saved_module = [this](const std::filesystem::path& manifest, std::string_view configuration) {
                    return scene_runtime_->select_feature_module(
                        manifest, configuration, EditorSceneRuntime::FeatureGraphLoadPolicy::kSavedStartup);
                };
                if (actions.active_module_catalog != nullptr) {
                    actions.apply_graph = [this](const aeg::feature::FeatureGraphDocument& graph) {
                        return scene_runtime_->queue_feature_graph(graph);
                    };
                }
                actions.feature_presentations = scene_runtime_->authored_feature_presentations();
                actions.inspect_node = [this](std::string_view asset, aeg::feature::FeatureNodeId node) {
                    auto result = scene_runtime_->inspect_feature_node(asset, node);
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
                    if (feature_workflow_probe) feature_workflow_probe->record_inspection(result);
#endif
                    return result;
                };
                actions.inspect_execution = [this](std::string_view asset) {
                    return scene_runtime_->inspect_feature_execution(asset);
                };
                actions.inspect_render_graph = [this] { return scene_runtime_->inspect_render_graph(); };
                actions.control_process = [this](const aeg::application::runtime::RuntimeProcessCommand& command) {
                    return scene_runtime_->control_feature_process(command);
                };
                actions.world_binding_editable =
                    scene_runtime_->mode() == EditorSceneRuntime::Mode::kEdit;
                actions.selected_world_target_label = [this] {
                    if (scene_runtime_ == nullptr) return std::string{};
                    const auto selected = scene_runtime_->selected_entity();
                    const auto entities = scene_runtime_->entities();
                    const auto found = std::find_if(entities.begin(), entities.end(), [&](const auto& entity) {
                        return entity.id == selected;
                    });
                    if (found == entities.end()) return std::string{};
                    return found->name + " (Scene entity " + std::to_string(selected.high()) + ":" +
                           std::to_string(selected.low()) + ")";
                };
                actions.world_binding_status = [this](std::string_view asset_uuid,
                                                       aeg::feature::FeatureNodeId node) {
                    if (scene_runtime_ == nullptr) return std::string{};
                    const auto& bindings = scene_runtime_->feature_world_bindings();
                    const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
                        return binding.graph_asset_uuid == asset_uuid && binding.node_id == node;
                    });
                    if (found == bindings.end()) return std::string{};
                    const auto entities = scene_runtime_->entities();
                    const auto target = std::find_if(entities.begin(), entities.end(), [&](const auto& entity) {
                        return entity.id == found->scene_entity;
                    });
                    const std::string label = target == entities.end() ? "missing Scene entity" : target->name;
                    return "Currently routed to " + label + " (" + std::to_string(found->scene_entity.high()) +
                           ":" + std::to_string(found->scene_entity.low()) + ").";
                };
                actions.bind_world_command = [this](std::string_view asset_uuid,
                                                    aeg::feature::FeatureNodeId node) {
                    if (scene_runtime_ == nullptr)
                        return aeg::foundation::invalid_state("Editor Scene runtime is unavailable");
                    return scene_runtime_->bind_feature_world_node(asset_uuid, node,
                                                                   scene_runtime_->selected_entity());
                };
                actions.clear_world_command = [this](std::string_view asset_uuid,
                                                     aeg::feature::FeatureNodeId node) {
                    if (scene_runtime_ == nullptr)
                        return aeg::foundation::invalid_state("Editor Scene runtime is unavailable");
                    return scene_runtime_->clear_feature_world_node_binding(asset_uuid, node);
                };
                actions.publication_pending = scene_runtime_->feature_publication_pending();
                return actions;
            },
            [this](const std::filesystem::path& path) { return select_feature_graph(path); },
            [this](const std::filesystem::path& path,
                   const aeg::application::authoring::FeatureGraphFileRevision& source,
                   const aeg::application::authoring::FeatureGraphFileRevision& backup) {
                if (scene_runtime_ == nullptr || scene_runtime_->mode() != EditorSceneRuntime::Mode::kEdit)
                    return aeg::foundation::invalid_state("Stop preview before restoring a FeatureGraph source");
                if (feature_graph_document_ && feature_graph_document_->dirty())
                    return aeg::foundation::invalid_state("Save the current draft before restoring a source");
                const auto restored = aeg::application::authoring::recover_feature_graph_document(path, source, &backup);
                if (!restored) return restored;
                if (const auto refreshed = scene_runtime_->refresh_assets(); !refreshed) return refreshed;
                feature_graph_document_.reset();
                feature_graph_document_path_.clear();
                return select_feature_graph(path);
            },
            [this] { return scene_runtime_->inspect_renderer_feature_execution(); },
            [this] { return scene_runtime_->inspect_render_graph(); });
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
                    EditorPanelDescriptor{"flowgraph", "Flow Graph###FlowGraph", EditorDockRegion::kBottom, true, true,
                                         true},
                    std::move(flowgraph_panel)),
                "register_flowgraph_panel") ||
            !report_result(workspace_.register_panel(EditorPanelDescriptor{"hierarchy", "Hierarchy",
                                                                           EditorDockRegion::kLeft, true, true, true},
                                                     std::make_unique<HierarchyPanel>(*scene_runtime_)),
                           "register_hierarchy_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"project", "Project", EditorDockRegion::kLeft, true, true, true},
                               std::make_unique<ProjectPanel>(
                                   *scene_runtime_,
                                   [this] {
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
                 {"save_scene", "Save", [this] { return scene_runtime_->save_scene(); }, false,
                  [this] { return scene_runtime_->mode() == EditorSceneRuntime::Mode::kEdit; }},
                 {"undo", "Undo", [this] {
                      return feature_graph_history_active()
                                 ? feature_graph_document_->undo()
                                 : scene_runtime_->undo();
                  }, false,
                  [this] {
                      return feature_graph_history_active()
                                 ? feature_graph_document_->can_undo()
                                 : scene_runtime_->can_undo();
                  }},
                 {"redo", "Redo", [this] {
                      return feature_graph_history_active()
                                 ? feature_graph_document_->redo()
                                 : scene_runtime_->redo();
                  }, false,
                  [this] {
                      return feature_graph_history_active()
                                 ? feature_graph_document_->can_redo()
                                 : scene_runtime_->can_redo();
                  }},
                 {"play", "Play", [this] { return scene_runtime_->play(); }, false,
                  [this] { return scene_runtime_->mode() != EditorSceneRuntime::Mode::kPlay; }},
                 {"pause", "Pause", [this] { return scene_runtime_->pause(); }, false,
                  [this] { return scene_runtime_->mode() == EditorSceneRuntime::Mode::kPlay; }},
                 {"stop", "Stop", [this] { return scene_runtime_->stop(); }, false,
                  [this] { return scene_runtime_->mode() != EditorSceneRuntime::Mode::kEdit; }},
                 {"refresh_assets", "Refresh Project Assets", [this] { return scene_runtime_->refresh_assets(); },
                  false, [this] { return scene_runtime_->project_loaded(); }},
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
                          const aeg::application::IConsoleInterruptController& console_interrupt,
                          bool verify_feature_preview, bool expect_project_rejected,
                          const std::filesystem::path& expected_scene) {
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
        uint32_t successful_scene_frames = 0;
        while (!close_requested() && !console_interrupt.interrupted()) {
            try {
                if (const auto polled = project_operations_.poll(); !polled) {
                    notify_runtime_result(polled, "poll_project_operation");
                    if (project_operation_cli_) return 1;
                }
                if (project_operation_cli_ && project_operations_.status().execution_complete
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
                    && (!feature_workflow_probe || feature_workflow_probe->complete())
#endif
                    )
                    break;
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
                    if (!report_result(workspace_.set_panel_presentation("flowgraph", presentation),
                                       "switch_flowgraph_presentation"))
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
                bool close_preferences_after_main_frame = false;
                bool closing_regression_preferences = false;
                if (preferences_close_requested_) {
                    closing_regression_preferences = preferences_regression_mode_ && preferences_window_ != nullptr;
                    // Keep the Settings window on top until the main editor has
                    // presented a complete frame. Destroying the foreground
                    // window before that presentation lets DWM expose the
                    // stale/transparent surface for one frame.
                    close_preferences_after_main_frame = preferences_window_ != nullptr &&
                                                         !window_->state().minimized;
                    if (!close_preferences_after_main_frame) {
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
                }
                if (preferences_open_requested_ && !close_preferences_after_main_frame) {
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
                        static_cast<void>(aeg::concurrency::sleep_for(16'000'000ull));
                        continue;
                    }
                }

                const Result begin_input_result = input_system_.begin_frame();
                if (!begin_input_result) {
                    notify_runtime_result(begin_input_result, "begin_input_frame");
                    if (fatal_runtime_error_) {
                        return 1;
                    }
                    static_cast<void>(aeg::concurrency::sleep_for(16'000'000ull));
                    continue;
                }
                const auto finish_input_frame = [this]() noexcept { input_system_.end_frame(); };

                if (window_->state().minimized) {
                    finish_input_frame();
                    static_cast<void>(aeg::concurrency::sleep_for(16'000'000ull));
                    continue;
                }

                const auto now = std::chrono::steady_clock::now();
                const double feature_delta_seconds = std::chrono::duration<double>(now - last_frame_time_).count();
                float delta_seconds = static_cast<float>(feature_delta_seconds);
                last_frame_time_ = now;
                if (!(delta_seconds > 0.0f) || delta_seconds > 0.25f) {
                    delta_seconds = 1.0f / 60.0f;
                }

                if (scene_runtime_ != nullptr) {
                    Result scene_frame_result = Result::success();
                    if (scene_runtime_->viewport_resize_pending()) {
                        // Detached UI packets may also sample the old SceneColor image.
                        scene_frame_result = release_shared_ui_texture_frames();
                        if (!scene_frame_result) {
                            notify_runtime_result(scene_frame_result, "release_scene_viewport_frame");
                        }
                        if (scene_frame_result && render_graph_ != nullptr) {
                            // The editor graph also caches logical imports of SceneColor. Rebuild
                            // its orchestrator after dropping those records so a resized target is
                            // observed as a fresh imported resource with its current RHI state.
                            orchestrator_.reset();
                            render_graph_->reset();
                            try {
                                orchestrator_ = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
                                    pipeline_host_, *frame_context_, *render_graph_);
                            } catch (const std::bad_alloc&) {
                                scene_frame_result = aeg::foundation::out_of_memory(
                                    "Editor frame orchestrator recreation allocation failed");
                            }
                            if (scene_frame_result) {
                                scene_frame_result = orchestrator_->register_pipeline(
                                    aeg::engine::EngineFramePipelineRegistration{frame_host_});
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
                        scene_frame_result = scene_runtime_->render_frame(frame_count_, feature_delta_seconds);
                        if (verify_feature_preview && scene_frame_result)
                            ++successful_scene_frames;
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
                if (project_operations_open_) {
                    if (const auto synced = sync_project_operations(); !synced)
                        notify_runtime_result(synced, "select_project_operation_project");
                    if (ImGui::Begin("Project Operations", &project_operations_open_))
                        project_operations_.draw();
                    ImGui::End();
                }
                sync_flowgraph_document();
                workspace_.set_preferences_open(preferences_window_ != nullptr || preferences_open_requested_);
                workspace_.set_playing(scene_runtime_->mode() != EditorSceneRuntime::Mode::kEdit);
                workspace_.set_paused(scene_runtime_->mode() == EditorSceneRuntime::Mode::kPause);
                workspace_.set_game_draw_calls(scene_runtime_ != nullptr ? scene_runtime_->draw_call_count() : 0);
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
                if (feature_workflow_probe) {
                    if (const auto result = feature_workflow_probe->before_ui(feature_workflow_context()); !result) {
                        std::fprintf(stderr, "Main Feature workflow: %s\n", result.message.c_str());
                        return 73;
                    }
                }
#endif
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
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
                if (feature_workflow_probe) {
                    if (const auto result = feature_workflow_probe->after_ui(feature_workflow_context()); !result) {
                        std::fprintf(stderr, "Main Feature workflow: %s\n", result.message.c_str());
                        return 73;
                    }
                }
#endif
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
                if (close_preferences_after_main_frame) {
                    const Result close_result = close_preferences();
                    if (!close_result) {
                        notify_runtime_result(close_result, "close_preferences");
                        if (regression_mode_) {
                            finish_input_frame();
                            return 1;
                        }
                        if (fatal_runtime_error_) {
                            finish_input_frame();
                            return 1;
                        }
                    }
                    if (closing_regression_preferences) {
                        if (preferences_window_ != nullptr || !main_window_input_enabled_) {
                            finish_input_frame();
                            return 1;
                        }
                        preferences_regression_closed_ = true;
                    }
                }
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
                                                 "FlowGraph viewport presentation did not change at frame " +
                                                 std::to_string(attempted_frames) +
                                                 "; native windows: " + std::to_string(native_viewports_.size())),
                                       "verify_flowgraph_presentation"))
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
                static_cast<void>(aeg::concurrency::sleep_for(16'000'000ull));
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
        if (verify_feature_preview) {
            const auto verified = verify_preview_source(expect_project_rejected, expected_scene, successful_scene_frames);
            if (!verified) {
                std::fprintf(stderr, "Feature Preview verification failed: %s\n", verified.message.c_str());
                return 1;
            }
            std::printf("FEATURE_PREVIEW_VERIFIED %s\n", expect_project_rejected ? "rejected" : "recovered");
        }
        if (project_operation_cli_) {
            const auto& operation = project_operations_.status();
            if (!operation.execution_complete || operation.state != aeg::tooling::ProjectOperationState::kSucceeded) {
                std::fprintf(stderr, "Project operation failed or did not finish: %s\n", operation.result.message.c_str());
                return 1;
            }
            std::printf("PROJECT_OPERATION_COMPLETE commits=%zu manifest=%s\n",
                        operation.feature_stages.size(), operation.game_manifest.string().c_str());
        }
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
        if (feature_workflow_probe) {
            if (const auto result = feature_workflow_probe->finish(); !result) {
                std::fprintf(stderr, "Main Feature workflow: %s\n", result.message.c_str());
                return 73;
            }
        }
#endif
        return 0;
    }

private:
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
    [[nodiscard]] aeg::editor_test::FeatureWorkflowContext feature_workflow_context() {
        return {*scene_runtime_, feature_graph_document_.get(), feature_graph_document_path_,
                project_operations_.request(), project_operations_.status(),
                [this](std::string_view action) { return workspace_.invoke_view_action(action); }};
    }
#endif
    [[nodiscard]] Result sync_project_operations() {
        const auto file = scene_runtime_ == nullptr ? std::filesystem::path{} : scene_runtime_->project_file();
        if (file == project_operations_.request().project_file) return Result::success();
        if (const auto selected = project_operations_.set_project(file); !selected) return selected;
        if (file.empty()) return Result::success();
        auto request = project_operations_.request();
        request.engine_directory = AEG_ENGINE_SOURCE_DIR;
        request.cmake_executable = AEG_CMAKE_EXECUTABLE;
        return project_operations_.set_request(request);
    }
    [[nodiscard]] Result verify_preview_source(bool expect_rejected, const std::filesystem::path& expected_scene,
                                               uint32_t successful_scene_frames) const {
        if (scene_runtime_ == nullptr || frame_count_ == 0 || successful_scene_frames != frame_count_ ||
            !scene_runtime_->viewport_ready() || scene_runtime_->draw_call_count() == 0)
            return aeg::foundation::invalid_state("Scene Preview did not complete every presented test frame");
        if (expect_rejected) {
            if (scene_runtime_->project_loaded())
                return aeg::foundation::invalid_state("Rejected project became active");
        } else {
            if (!scene_runtime_->project_loaded())
                return aeg::foundation::invalid_state("Supported recovery project was not loaded");
            std::error_code error;
            const auto root = startup_project_.has_extension() ? startup_project_.parent_path() : startup_project_;
            if (!std::filesystem::equivalent(root, scene_runtime_->project_root(), error) || error)
                return aeg::foundation::invalid_state("Recovered Preview belongs to another project");
            if (!std::filesystem::equivalent(expected_scene, scene_runtime_->scene_path(), error) || error)
                return aeg::foundation::invalid_state("Recovered Preview did not open the real expected Scene");
        }
        const auto execution = scene_runtime_->inspect_renderer_feature_execution();
        const auto captured = scene_runtime_->inspect_render_graph();
        if (!execution || !captured || !captured.value || execution.value.asset_uuid != "renderer-scene-opaque" ||
            !execution.value.generation.is_valid() || execution.value.contract_digest == 0)
            return aeg::foundation::invalid_state("Actual Opaque Preview publication or successful RenderGraph is absent");
        const auto& published = execution.value;
        const auto& graph = *captured.value;
        const auto source = std::find_if(graph.sources.begin(), graph.sources.end(), [&](const auto& candidate) {
            return candidate.asset_uuid == published.asset_uuid && candidate.generation == published.generation &&
                   candidate.contract_digest == published.contract_digest && candidate.graph_version == published.graph_version;
        });
        if (source == graph.sources.end())
            return aeg::foundation::invalid_state("Successful RenderGraph does not retain the actual Preview source owner");
        const auto definition = aeg::feature::FeatureDefinitionId{"Renderer.Scene.Opaque"};
        const auto node = aeg::feature::FeatureNodeId{"Renderer.Scene.Root"};
        const auto module = aeg::feature::ModuleIdentity{"com.aeg.renderer"};
        const auto clock = aeg::feature::ClockId{"Frame"};
        const auto origin = std::find_if(source->provenance.begin(), source->provenance.end(), [&](const auto& row) {
            return row.authored_node_id == node && row.runtime.definition_id == definition &&
                   row.runtime.owner_module == module && row.runtime.clock == clock &&
                   row.runtime.instance_id.is_valid() && row.runtime.invocation_id.is_valid();
        });
        if (origin == source->provenance.end())
            return aeg::foundation::invalid_state("Captured Preview has no actual Opaque Frame owner provenance");
        const auto contains_owner = [&](const auto& snapshot) {
            return std::any_of(snapshot.plans.begin(), snapshot.plans.end(), [&](const auto& plan) {
                return plan.clock == clock && std::find(plan.instances.begin(), plan.instances.end(),
                    origin->runtime.instance_id) != plan.instances.end();
            });
        };
        if (!contains_owner(published) || !contains_owner(*source))
            return aeg::foundation::invalid_state("Opaque owner is absent from the actual published Frame plan");
        const auto live_origin = std::find_if(published.provenance.begin(), published.provenance.end(), [&](const auto& row) {
            return row.runtime.instance_id == origin->runtime.instance_id && row.runtime.node_id == origin->runtime.node_id &&
                   row.runtime.definition_id == definition && row.runtime.owner_module == module && row.runtime.clock == clock &&
                   row.runtime.invocation_id == origin->runtime.invocation_id;
        });
        if (live_origin == published.provenance.end())
            return aeg::foundation::invalid_state("Captured Preview owner differs from the live publication");
        const bool live_work = std::any_of(graph.plan.pass_plans.begin(), graph.plan.pass_plans.end(), [&](const auto& pass) {
            if (!pass.live || !pass.feature_work) return false;
            const auto& work = *pass.feature_work;
            return work.kind == aeg::render_graph::RenderWorkItemKind::kExecute && work.id.is_valid() &&
                   work.domain == aeg::feature::FeatureDomain::kRender && work.feature_definition_id == definition &&
                   work.node_id == origin->runtime.node_id && work.instance_id == origin->runtime.instance_id &&
                   work.owner_module == module && work.invocation.generation == source->generation &&
                   work.invocation.invocation_id == origin->runtime.invocation_id && work.invocation.attempt_id.is_valid();
        });
        return live_work ? Result::success() :
            aeg::foundation::invalid_state("Successful RenderGraph has no live work from the actual Opaque invocation token");
    }

    enum class SceneDialogMode : uint8_t {
        kOpen,
        kSaveAs,
    };

    [[nodiscard]] bool feature_graph_history_active() const noexcept {
        if (feature_graph_document_ == nullptr || !feature_graph_focused_)
            return false;
        const auto* layout = workspace_.layout();
        const auto* panel = layout == nullptr ? nullptr : layout->find_panel("flowgraph");
        return panel != nullptr && panel->visible;
    }

    [[nodiscard]] Result save_scene_and_feature_graph() {
        if (const Result result = scene_runtime_->save_scene(); !result)
            return result;
        return save_feature_graph_document();
    }

    [[nodiscard]] Result save_scene_as_and_feature_graph(const std::filesystem::path& path) {
        if (const Result result = scene_runtime_->save_scene_as(path); !result)
            return result;
        return save_feature_graph_document();
    }

    [[nodiscard]] Result save_feature_graph_document() {
        if (feature_graph_document_ == nullptr || feature_graph_document_path_.empty())
            return Result::success();
        const auto resolver = aeg::application::make_editor_asset_kind_resolver(
            scene_runtime_ != nullptr ? scene_runtime_->project_file() : std::filesystem::path{});
        return feature_graph_document_->save(feature_graph_document_path_, resolver);
    }

    [[nodiscard]] Result select_feature_graph(const std::filesystem::path& path) {
        if (scene_runtime_ == nullptr)
            return aeg::foundation::invalid_state("No project is open");
        const auto normalize = [this](std::filesystem::path candidate) {
            if (candidate.is_relative())
                candidate = scene_runtime_->project_root() / candidate;
            return candidate.lexically_normal();
        };
        const auto requested = normalize(path);
        const auto authored = scene_runtime_->authored_feature_graphs();
        const auto target = std::find_if(authored.begin(), authored.end(), [&](const auto& document) {
            return document.load_result && normalize(document.path) == requested;
        });
        if (target == authored.end())
            return aeg::foundation::not_found("FeatureGraph document is not available for editing");
        if (feature_graph_document_ != nullptr && feature_graph_document_->dirty() &&
            normalize(feature_graph_document_path_) != requested)
            return aeg::foundation::invalid_state("Save the current FeatureGraph before switching documents");
        if (feature_graph_document_ != nullptr && normalize(feature_graph_document_path_) == requested)
            return Result::success();
        auto source = aeg::application::authoring::load_feature_graph_source(target->path);
        if (!source) return source.result;
        feature_graph_document_ = std::make_unique<aeg::application::EditorFeatureGraphDocument>(
            std::move(source.value.document), scene_runtime_->authored_feature_catalog());
        feature_graph_document_->bind_source_file(target->path, std::move(source.value.revision));
        feature_graph_document_path_ = target->path;
        return Result::success();
    }

    void sync_flowgraph_document() {
        if (scene_runtime_ == nullptr) {
            feature_graph_document_.reset();
            feature_graph_document_path_.clear();
            return;
        }
        const auto feature_graphs = scene_runtime_->authored_feature_graphs();
        const auto normalize = [this](std::filesystem::path candidate) {
            if (candidate.is_relative())
                candidate = scene_runtime_->project_root() / candidate;
            return candidate.lexically_normal();
        };
        auto selected_document = std::find_if(feature_graphs.begin(), feature_graphs.end(), [&](const auto& document) {
            return document.load_result && !feature_graph_document_path_.empty() &&
                   normalize(document.path) == normalize(feature_graph_document_path_);
        });
        if (selected_document == feature_graphs.end()) {
            selected_document = std::find_if(feature_graphs.begin(), feature_graphs.end(),
                                             [](const auto& document) { return static_cast<bool>(document.load_result); });
        }
        if (selected_document != feature_graphs.end()) {
            if (feature_graph_document_ != nullptr &&
                normalize(feature_graph_document_path_) == normalize(selected_document->path))
                return;
            auto source = aeg::application::authoring::load_feature_graph_source(selected_document->path);
            if (!source) {
                feature_graph_source_operation_ = source.result;
                return;
            }
            feature_graph_document_ = std::make_unique<aeg::application::EditorFeatureGraphDocument>(
                std::move(source.value.document), scene_runtime_->authored_feature_catalog());
            feature_graph_document_->bind_source_file(selected_document->path, std::move(source.value.revision));
            feature_graph_source_operation_ = Result::success();
            feature_graph_document_path_ = selected_document->path;
            return;
        }
        feature_graph_document_.reset();
        feature_graph_document_path_.clear();
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
        if (destructive && (scene_runtime_->dirty() ||
                            (feature_graph_document_ != nullptr && feature_graph_document_->dirty()))) {
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
                break;
            case EditorDocumentActionType::kOpenScene:
                result = scene_runtime_->open_scene(action.path);
                break;
            case EditorDocumentActionType::kSaveScene:
                result = save_scene_and_feature_graph();
                break;
            case EditorDocumentActionType::kSaveAsScene:
                result = save_scene_as_and_feature_graph(action.path);
                break;
            case EditorDocumentActionType::kNewProject:
                result = action.project_options.has_value()
                             ? scene_runtime_->new_project(*action.project_options)
                             : scene_runtime_->new_project(action.path, action.name);
                if (result) {
                    feature_graph_document_.reset();
                    feature_graph_document_path_.clear();
                    project_hub_operation_ = Result::success();
                }
                break;
            case EditorDocumentActionType::kImportProject:
                result = scene_runtime_->import_project(action.path);
                if (result) {
                    feature_graph_document_.reset();
                    feature_graph_document_path_.clear();
                    project_hub_operation_ = Result::success();
                }
                break;
            case EditorDocumentActionType::kCloseProject:
                result = scene_runtime_->close_project();
                if (result) {
                    feature_graph_document_.reset();
                    feature_graph_document_path_.clear();
                    project_hub_pending_ = false;
                }
                break;
            case EditorDocumentActionType::kCloseEditor:
                close_confirmation_requested_ = false;
                force_shutdown_ = true;
                return;
        }
        if (result && (action.type == EditorDocumentActionType::kNewProject ||
                       action.type == EditorDocumentActionType::kImportProject ||
                       action.type == EditorDocumentActionType::kCloseProject))
            result = sync_project_operations();
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
                unsaved_operation_ = save_scene_and_feature_graph();
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
        if (ImGui::BeginMenu("Build")) {
            if (ImGui::MenuItem("Build / Cook...", nullptr, false, has_project)) {
                const auto selected = sync_project_operations();
                if (selected) project_operations_open_ = true;
                else notify_runtime_result(selected, "select_project_operation_project");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false,
                                feature_graph_history_active()
                                    ? feature_graph_document_->can_undo()
                                    : scene_runtime_->can_undo()))
                report_history_result(feature_graph_history_active()
                                          ? feature_graph_document_->undo()
                                          : scene_runtime_->undo(),
                                      "undo_editor_menu");
            if (ImGui::MenuItem("Redo", "Ctrl+Y / Ctrl+Shift+Z", false,
                                feature_graph_history_active()
                                    ? feature_graph_document_->can_redo()
                                    : scene_runtime_->can_redo()))
                report_history_result(feature_graph_history_active()
                                          ? feature_graph_document_->redo()
                                          : scene_runtime_->redo(),
                                      "redo_editor_menu");
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
                ImGui::TextDisabled("Name your project.");
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
            ImGui::TextUnformatted("Template: FeatureGraph starter");
            ImGui::TextDisabled("Includes a Logic / Process example. Open Flow Graph to validate, compile and edit it.");
            ImGui::Dummy(ImVec2{0.0f, 14.0f});
            const char* engine_sdks[] = {"1.0", "2.0"};
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0f, 8.0f});
            if (ImGui::BeginTable("ProjectHubEngine", 1, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Engine Version");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::Combo("##ProjectHubSdk", &project_hub_sdk_index_, engine_sdks, IM_ARRAYSIZE(engine_sdks));
                ImGui::TextDisabled("Saved as this project's version.");
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
                options.template_id = "empty";
                options.engine_sdk = project_hub_sdk_index_ == 1 ? "2.0" : "1.0";
                options.startup_feature_graph = "Assets/Features/Default.aegfeature";
                EditorDocumentAction action{};
                action.type = EditorDocumentActionType::kNewProject;
                action.project_options = std::move(options);
                ImGui::CloseCurrentPopup();
                request_confirmed_document_action(std::move(action));
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();

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
                const auto project_source = path_text(recent.project_file);
                ImGui::PushID(project_source.c_str());
                ImGui::InvisibleButton("RecentProjectCard", ImVec2{-1.0f, 60.0f});
                const bool hovered = ImGui::IsItemHovered();
                if (hovered) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s", project_source.c_str());
                }
                if (ImGui::IsItemClicked()) {
                    open_project_from_path(recent.project_file);
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
                const std::string display_path = fit_project_hub_text(project_source, available_text_width, true);
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
        project_settings_feature_graphs_.clear();
        project_settings_feature_graphs_.reserve(project_settings_draft_.startup_feature_graphs.size());
        for (const auto& path : project_settings_draft_.startup_feature_graphs) {
            std::array<char, 256> buffer{};
            set_text_buffer(buffer, path.generic_string());
            project_settings_feature_graphs_.push_back(std::move(buffer));
        }
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
                    ImGui::TextUnformatted("Startup FeatureGraphs");
                    std::optional<std::size_t> remove_feature_graph;
                    for (std::size_t index = 0; index < project_settings_feature_graphs_.size(); ++index) {
                        auto& buffer = project_settings_feature_graphs_[index];
                        ImGui::PushID(static_cast<int>(index));
                        const std::string label = "Startup FeatureGraph " + std::to_string(index + 1);
                        ImGui::InputText(label.c_str(), buffer.data(), buffer.size());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove"))
                            remove_feature_graph = index;
                        ImGui::PopID();
                    }
                    if (remove_feature_graph)
                        project_settings_feature_graphs_.erase(project_settings_feature_graphs_.begin() +
                                                              static_cast<std::ptrdiff_t>(*remove_feature_graph));
                    if (ImGui::Button("Add Startup FeatureGraph"))
                        project_settings_feature_graphs_.emplace_back();
                    if (project_settings_feature_graphs_.empty())
                        ImGui::TextDisabled("No startup FeatureGraphs configured.");
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
            project_settings_draft_.startup_feature_graphs.clear();
            for (const auto& buffer : project_settings_feature_graphs_) {
                if (buffer[0] != '\0')
                    project_settings_draft_.startup_feature_graphs.emplace_back(buffer.data());
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
                const Result result = feature_graph_history_active()
                                          ? feature_graph_document_->redo()
                                          : scene_runtime_->redo();
                report_history_result(result, "redo_editor_shortcut");
            } else {
                const Result result = feature_graph_history_active()
                                          ? feature_graph_document_->undo()
                                          : scene_runtime_->undo();
                report_history_result(result, "undo_editor_shortcut");
            }
        } else if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            const Result result = feature_graph_history_active()
                                      ? feature_graph_document_->redo()
                                      : scene_runtime_->redo();
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
        imgui_desc.native_window = true;
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
        if (!(result = preferences_frame_host_.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline")))) {
            static_cast<void>(close_preferences());
            return result;
        }
        preferences_orchestrator_ = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
            preferences_pipeline_host_, *preferences_frame_context_, *preferences_render_graph_);
        if (!(result = preferences_orchestrator_->register_pipeline(
                  aeg::engine::EngineFramePipelineRegistration{preferences_frame_host_}))) {
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
        if (const Result result = release_shared_ui_texture_frames(); !result) {
            keep_first_error(result);
            return first_result;
        }
        if (preferences_orchestrator_ != nullptr) {
            keep_first_error(preferences_orchestrator_->shutdown());
            preferences_orchestrator_.reset();
        }
        const Result unregister_pipeline_result =
            preferences_registry_.unregister_pipeline(aeg::pipeline::PipelineId("RenderPipeline"));
        if (unregister_pipeline_result.code != ResultCode::kNotFound)
            keep_first_error(unregister_pipeline_result);
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
            if (const Result result = aeg::editor::detail::ImGuiRenderExecution::build_render_graph(
                    *preferences_imgui_renderer_, context.render_graph, context.swapchain_resource);
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
        if (const Result verified = validate_ui_framebuffer(preferences_imgui_host_, preferences_font_texture_handle_,
                draw_packet, target, pixels, preferences_orchestrator_->draw_call_count()); !verified)
            return verified;
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

    /** @brief Retires every UI consumer before changing a borrowed shared texture binding. */
    [[nodiscard]] Result release_shared_ui_texture_frames() noexcept {
        struct Host final {
            FrameContext* frame{nullptr};
            RenderGraph* graph{nullptr};
            ImGuiRenderer* renderer{nullptr};
        };
        try {
            std::vector<Host> hosts;
            hosts.reserve(native_viewports_.size() + 2);
            hosts.push_back({frame_context_.get(), render_graph_.get(), imgui_renderer_.get()});
            hosts.push_back({preferences_frame_context_.get(), preferences_render_graph_.get(),
                             preferences_imgui_renderer_.get()});
            for (const auto& [window, state] : native_viewports_) {
                if (state != nullptr)
                    hosts.push_back({state->frame_context.get(), state->render_graph.get(), state->imgui_renderer.get()});
            }
            std::vector<ImGuiRenderer*> renderers;
            renderers.reserve(hosts.size());
            for (const Host& host : hosts) {
                if (host.frame != nullptr && host.frame->is_frame_active())
                    return aeg::foundation::invalid_state("Cannot replace a shared UI texture during an active frame");
                if (host.renderer != nullptr)
                    renderers.push_back(host.renderer);
            }
            // Drop submitted command references across all hosts before freeing any descriptors.
            for (const Host& host : hosts) {
                if (host.frame != nullptr) {
                    if (const Result result = host.frame->drain(); !result)
                        return result;
                }
            }
            for (const Host& host : hosts) {
                if (host.graph != nullptr)
                    host.graph->reset();
            }
            return aeg::editor::detail::ImGuiRenderExecution::release_prepared_frames(
                std::span<ImGuiRenderer* const>{renderers.data(), renderers.size()});
        } catch (const std::bad_alloc&) {
            return aeg::foundation::out_of_memory("Editor UI texture consumer collection allocation failed");
        } catch (const std::exception& exception) {
            return aeg::foundation::unknown_exception(exception);
        } catch (...) {
            return Result::failure(ResultCode::kUnknown, "Editor UI texture consumer release failed");
        }
    }

    /** @brief Rebuilds and replaces the engine-owned GPU font texture for the current CPU atlas. */
    [[nodiscard]] Result refresh_font_texture() {
        const GpuTexture previous_texture = font_texture_;
        const TextureDesc previous_desc = font_texture_desc_;
        const ImGuiTextureHandle previous_handle = font_texture_handle_;

        if (previous_handle.is_valid()) {
            if (const Result result = release_shared_ui_texture_frames(); !result) {
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
        if (const Result result = release_shared_ui_texture_frames(); !result) {
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
            result = state->frame_host.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline"));
        if (result) {
            state->orchestrator = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
                state->pipeline_host, *state->frame_context, *state->render_graph);
            result = state->orchestrator->register_pipeline(
                aeg::engine::EngineFramePipelineRegistration{state->frame_host});
        }
        if (result)
            result = state->orchestrator->initialize();
        if (!result) {
            if (state->orchestrator != nullptr)
                static_cast<void>(state->orchestrator->shutdown());
            static_cast<void>(state->registry.unregister_pipeline(aeg::pipeline::PipelineId("RenderPipeline")));
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
            if (const Result result = state.imgui_renderer->prepare_frame(
                    context.frame_context, context.render_graph, draw_packet, target); !result)
                return result;
            return aeg::editor::detail::ImGuiRenderExecution::build_render_graph(
                *state.imgui_renderer, context.render_graph, context.swapchain_resource);
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
        static_cast<void>(state.registry.unregister_pipeline(aeg::pipeline::PipelineId("RenderPipeline")));
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

    /** @brief Checks current UI pixels against the captured packet, including genuinely uncovered clear pixels. */
    [[nodiscard]] Result validate_ui_framebuffer(const EditorImGuiHost& host, ImGuiTextureHandle font,
        const ImGuiDrawPacket& packet, const ImGuiRenderTargetDesc& target, std::span<const std::byte> pixels,
        uint64_t gpu_draws) const {
        constexpr uint32_t kProbeExtent = 8;
        if (target.extent.width < kProbeExtent * 2 || target.extent.height < kProbeExtent * 2 ||
            pixels.size() != uint64_t(target.extent.width) * target.extent.height * 4 ||
            packet.display_width <= 0 || packet.display_height <= 0 || packet.empty() ||
            packet.vertices.empty() || packet.indices.empty() || gpu_draws == 0)
            return aeg::foundation::invalid_state("UI framebuffer regression requires actual draw geometry and pixels");
        if (target.format != Format::kBgra8Unorm && target.format != Format::kRgba8Unorm)
            return aeg::foundation::unsupported("UI framebuffer regression requires an RGBA8/BGRA8 Unorm target");
        ImGuiFontAtlasData atlas;
        if (const Result result = host.capture_font_atlas(atlas); !result)
            return result;
        if (!atlas.is_valid())
            return aeg::foundation::invalid_state("UI framebuffer regression has no actual font atlas");
        struct Probe final {
            uint32_t x{0}, y{0}, corner{0};
            std::array<unsigned, 4> rgba{0u, 0u, 0u, 255u};
            size_t command{SIZE_MAX};
            double u{0}, v{0}, source_alpha{0};
            bool painted{false};
        };
        std::array<Probe, 4 * kProbeExtent * kProbeExtent> probes{};
        size_t next = 0;
        for (uint32_t corner = 0; corner < 4; ++corner)
            for (uint32_t y = 0; y < kProbeExtent; ++y)
                for (uint32_t x = 0; x < kProbeExtent; ++x)
                    probes[next++] = {(corner & 1u) ? target.extent.width - 1u - x : x,
                        (corner & 2u) ? target.extent.height - 1u - y : y, corner};
        const auto sample_font = [&atlas](double u, double v) {
            // Match the actual RGBA8 Unorm font texture and its linear, clamp-to-edge sampler.
            const double x = u * atlas.width - .5, y = v * atlas.height - .5;
            const int64_t ix = static_cast<int64_t>(std::floor(x));
            const int64_t iy = static_cast<int64_t>(std::floor(y));
            const double fx = x - std::floor(x), fy = y - std::floor(y);
            std::array<double, 4> sampled{};
            for (int64_t dy = 0; dy < 2; ++dy)
                for (int64_t dx = 0; dx < 2; ++dx) {
                    const auto sx = static_cast<uint32_t>(std::clamp<int64_t>(ix + dx, 0, atlas.width - 1));
                    const auto sy = static_cast<uint32_t>(std::clamp<int64_t>(iy + dy, 0, atlas.height - 1));
                    const size_t offset = size_t(sy) * atlas.row_pitch + size_t(sx) * 4;
                    const double weight = (dx == 0 ? 1.0 - fx : fx) * (dy == 0 ? 1.0 - fy : fy);
                    for (size_t channel = 0; channel < 4; ++channel)
                        sampled[channel] += std::to_integer<unsigned>(atlas.rgba8_pixels[offset + channel]) * weight / 255.0;
                }
            return sampled;
        };
        struct Point final { double x, y; };
        const auto screen = [&](const auto& vertex) {
            return Point{(double(vertex.position_x) - packet.display_pos_x) * target.extent.width / packet.display_width,
                (double(vertex.position_y) - packet.display_pos_y) * target.extent.height / packet.display_height};
        };
        const auto edge = [](Point a, Point b, Point p) {
            return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        };
        const auto top_left = [](Point a, Point b) {
            return b.y < a.y || (b.y == a.y && b.x > a.x);
        };
        uint64_t visible_commands = 0;
        for (size_t command_index = 0; command_index < packet.commands.size(); ++command_index) {
            const auto& command = packet.commands[command_index];
            if (command.elem_count == 0) continue;
            const auto clip = [&](float coordinate, float origin, float scale, uint32_t limit, bool maximum) {
                const float value = (coordinate - origin) * scale;
                return std::clamp(maximum ? std::ceil(value) : std::floor(value), 0.f, static_cast<float>(limit));
            };
            const double left = clip(command.clip_min_x, packet.display_pos_x, packet.framebuffer_scale_x, target.extent.width, false);
            const double right = clip(command.clip_max_x, packet.display_pos_x, packet.framebuffer_scale_x, target.extent.width, true);
            const double top = clip(command.clip_min_y, packet.display_pos_y, packet.framebuffer_scale_y, target.extent.height, false);
            const double bottom = clip(command.clip_max_y, packet.display_pos_y, packet.framebuffer_scale_y, target.extent.height, true);
            if (left >= right || top >= bottom) continue;
            ++visible_commands;
            const uint64_t end = uint64_t(command.index_offset) + command.elem_count;
            if (end > packet.indices.size() || command.elem_count % 3 != 0)
                return aeg::foundation::invalid_state("UI framebuffer regression has an invalid actual triangle range");
            for (uint64_t index = command.index_offset; index < end; index += 3) {
                std::array<const aeg::editor::ImGuiDrawVertex*, 3> vertices{};
                for (size_t vertex = 0; vertex < 3; ++vertex) {
                    const int64_t selected = int64_t(packet.indices[size_t(index) + vertex]) + command.vertex_offset;
                    if (selected < 0 || uint64_t(selected) >= packet.vertices.size())
                        return aeg::foundation::invalid_state("UI framebuffer regression has an invalid actual vertex range");
                    vertices[vertex] = &packet.vertices[size_t(selected)];
                }
                Point a = screen(*vertices[0]), b = screen(*vertices[1]), c = screen(*vertices[2]);
                double area = edge(a, b, c);
                if (area == 0) continue;
                if (area < 0) {
                    std::swap(b, c);
                    std::swap(vertices[1], vertices[2]);
                    area = -area;
                }
                for (Probe& probe : probes) {
                    const Point point{probe.x + .5, probe.y + .5};
                    if (point.x < left || point.x >= right || point.y < top || point.y >= bottom) continue;
                    const auto inside = [&](Point p, Point q) {
                        const double value = edge(p, q, point);
                        return value > 0 || (value == 0 && top_left(p, q));
                    };
                    if (!inside(a, b) || !inside(b, c) || !inside(c, a)) continue;
                    if (command.texture != font) {
                        AEG_LOG_ERROR(aeg::logging::Category::kApplication,
                            "UI framebuffer probe ({},{}) covered by a non-font texture at command={} triangle={} commands={} GPUdraws={}",
                            probe.x, probe.y, command_index, index, packet.commands.size(), gpu_draws);
                        return aeg::foundation::invalid_state("UI framebuffer regression cannot predict a non-font texture at a probe");
                    }
                    const std::array<double, 3> weights{edge(b, c, point) / area,
                        edge(c, a, point) / area, edge(a, b, point) / area};
                    std::array<double, 4> source{};
                    double u = 0, v = 0;
                    for (size_t vertex = 0; vertex < 3; ++vertex) {
                        u += vertices[vertex]->uv_x * weights[vertex];
                        v += vertices[vertex]->uv_y * weights[vertex];
                        for (size_t channel = 0; channel < 4; ++channel)
                            source[channel] += ((vertices[vertex]->color >> (channel * 8u)) & 255u) * weights[vertex] / 255.0;
                    }
                    const auto sampled = sample_font(u, v);
                    for (size_t channel = 0; channel < 4; ++channel)
                        source[channel] = std::clamp(source[channel] * sampled[channel], 0.0, 1.0);
                    // Match DrawMesh: SrcAlpha/OneMinusSrcAlpha RGB, One/OneMinusSrcAlpha alpha, quantized per fragment.
                    for (size_t channel = 0; channel < 4; ++channel) {
                        const double value = source[channel] * (channel == 3 ? 1.0 : source[3]) +
                            probe.rgba[channel] / 255.0 * (1.0 - source[3]);
                        probe.rgba[channel] = static_cast<unsigned>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
                    }
                    probe.command = command_index;
                    probe.u = u;
                    probe.v = v;
                    probe.source_alpha = source[3];
                    probe.painted = probe.painted || source[3] > 0.0;
                }
            }
        }
        if (visible_commands != gpu_draws)
            return aeg::foundation::invalid_state("UI framebuffer regression GPU draw count differs from its actual visible commands");
        std::array<uint32_t, 4> painted_counts{};
        for (const Probe& probe : probes) {
            auto expected = probe.rgba;
            if (target.format == Format::kBgra8Unorm) std::swap(expected[0], expected[2]);
            const size_t offset = (size_t(probe.y) * target.extent.width + probe.x) * 4;
            bool matches = true;
            for (size_t channel = 0; channel < 4; ++channel) {
                const int actual = std::to_integer<unsigned>(pixels[offset + channel]);
                const int tolerance = probe.painted && channel < 3 ? 1 : 0;
                matches = matches && std::abs(actual - int(expected[channel])) <= tolerance;
            }
            if (!matches) {
                AEG_LOG_ERROR(aeg::logging::Category::kApplication,
                    "UI framebuffer probe ({},{}) command={} expected={}/{}/{}/{} actual={}/{}/{}/{} uv={}/{} sourceAlpha={} commands={} GPUdraws={} format={}",
                    probe.x, probe.y, probe.command, expected[0], expected[1], expected[2], expected[3],
                    std::to_integer<unsigned>(pixels[offset]), std::to_integer<unsigned>(pixels[offset + 1]),
                    std::to_integer<unsigned>(pixels[offset + 2]), std::to_integer<unsigned>(pixels[offset + 3]),
                    probe.u, probe.v, probe.source_alpha, visible_commands, gpu_draws, static_cast<unsigned>(target.format));
                return aeg::foundation::invalid_state("UI framebuffer regression pixel differs from its actual font draw or uncovered clear");
            }
            if (probe.painted) ++painted_counts[probe.corner];
        }
        if (std::any_of(painted_counts.begin(), painted_counts.end(), [](auto count) { return count == 0; }))
            return aeg::foundation::invalid_state("UI framebuffer regression has no actual font UI proof at a viewport corner");
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
            if (const Result result = aeg::editor::detail::ImGuiRenderExecution::build_render_graph(
                    *imgui_renderer_, context.render_graph, context.swapchain_resource);
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
        if (const Result verified = validate_ui_framebuffer(imgui_host_, font_texture_handle_, draw_packet, target,
                pixels, orchestrator_->draw_call_count()); !verified)
            return verified;
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
        project_operations_.shutdown();
        if (startup_splash_ != nullptr) {
            startup_splash_->close();
            startup_splash_.reset();
        }
        if (const Result result = release_shared_ui_texture_frames(); !result) {
            static_cast<void>(report_result(result, "release_editor_ui_texture_frames"));
            return;
        }
        static_cast<void>(close_preferences());
        release_all_native_viewports();
        if (viewport_texture_handle_.is_valid()) {
            static_cast<void>(texture_registry_.unregister_texture(viewport_texture_handle_));
            viewport_texture_handle_ = {};
        }
        if (scene_runtime_ != nullptr) {
            static_cast<void>(scene_runtime_->shutdown());
            scene_runtime_.reset();
        }
        if (orchestrator_ != nullptr) {
            static_cast<void>(orchestrator_->shutdown());
            orchestrator_.reset();
        }
        static_cast<void>(registry_.unregister_pipeline(aeg::pipeline::PipelineId("RenderPipeline")));
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
    PipelineScheduler preferences_scheduler_;
    aeg::engine::EngineFramePipelineHost preferences_pipeline_host_{preferences_registry_, preferences_scheduler_};
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
    /** Canonical project FeatureGraph document edited by the main FlowGraph panel. */
    std::unique_ptr<aeg::application::EditorFeatureGraphDocument> feature_graph_document_{};
    std::filesystem::path feature_graph_document_path_{};
    Result feature_graph_source_operation_{};
    /** @brief Focus state sampled by the FeatureGraph panel for shared history commands. */
    bool feature_graph_focused_{false};
    std::unique_ptr<ImGuiRenderer> imgui_renderer_{};
    /** Engine render targets for ImGui-created native panel viewports. */
    std::unordered_map<WindowId, std::unique_ptr<NativeViewportRenderState>, WindowIdHash> native_viewports_{};
    PipelineRegistry registry_{};
    PipelineScheduler scheduler_;
    aeg::engine::EngineFramePipelineHost pipeline_host_{registry_, scheduler_};
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
    aeg::application::EditorProjectOperations project_operations_{};
    bool project_operations_open_{false};
    bool project_operation_cli_{false};
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
    std::array<char, 512> project_hub_open_path_buffer_{};
    std::array<char, 512> project_hub_path_buffer_{};
    std::array<char, 128> project_hub_name_buffer_{};
    int project_hub_sdk_index_{0};
    Result project_hub_operation_{Result::success()};
    bool project_settings_pending_{false};
    bool project_settings_loaded_{false};
    ProjectConfig project_settings_draft_{};
    std::array<char, 128> project_settings_name_{};
    std::array<char, 64> project_settings_sdk_{};
    std::array<char, 128> project_settings_target_{};
    std::array<char, 256> project_settings_scene_{};
    std::vector<std::array<char, 256>> project_settings_feature_graphs_{};
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
        const bool verify_feature_preview = has_argument(argc, argv, "--verify-feature-preview");
        const bool expect_project_rejected = has_argument(argc, argv, "--expect-project-rejected");
        const auto expected_scene = parse_path_argument(argc, argv, "--expected-scene");
        const auto startup_project = parse_path_argument(argc, argv, "--project");
        const bool run_project_operation = has_argument(argc, argv, "--project-operation");
#if defined(AEG_EDITOR_WORKFLOW_TESTS)
        if (has_argument(argc, argv, "--verify-feature-workflow")) {
            if (!run_project_operation || startup_project.empty() || parse_frame_limit(argc, argv) == 0) {
                std::fprintf(stderr, "Main Feature workflow verification requires an operation, project and bounded frames\n");
                return 1;
            }
            feature_workflow_probe = std::make_unique<aeg::editor_test::MainFeatureWorkflowProbe>();
        }
#endif
        aeg::tooling::ProjectOperationRequest project_operation{};
        if (run_project_operation) {
            const auto kind = parse_path_argument(argc, argv, "--project-operation").string();
            if (startup_project.empty() || (kind != "build" && kind != "cook" && kind != "build-and-cook")) {
                std::fprintf(stderr, "--project-operation requires --project and build, cook or build-and-cook\n");
                return 1;
            }
            project_operation.project_file = startup_project;
            project_operation.operation = kind == "build" ? aeg::tooling::ProjectOperation::kBuild
                : kind == "cook" ? aeg::tooling::ProjectOperation::kCook : aeg::tooling::ProjectOperation::kBuildAndCook;
            project_operation.engine_directory = parse_path_argument(argc, argv, "--engine-dir");
            project_operation.cmake_executable = parse_path_argument(argc, argv, "--cmake");
            project_operation.module_manifest = parse_path_argument(argc, argv, "--module-manifest");
            project_operation.output_directory = parse_path_argument(argc, argv, "--output-directory");
            project_operation.target = parse_path_argument(argc, argv, "--target").string();
            const auto configuration = parse_path_argument(argc, argv, "--configuration").string();
            if (!configuration.empty()) project_operation.configuration = configuration;
        }
        if (verify_feature_preview && (parse_frame_limit(argc, argv) == 0 || startup_project.empty() ||
            expect_project_rejected == !expected_scene.empty())) {
            std::fprintf(stderr, "Feature Preview verification requires bounded frames, a project, and one expected outcome\n");
            return 1;
        }
        const std::filesystem::path resource_root =
            std::filesystem::absolute(std::filesystem::path{argv[0]}).parent_path();
        if (!application.initialize(resource_root, startup_project)) {
            return 1;
        }
        if (run_project_operation) {
            if (const auto launched = application.launch_project_operation(std::move(project_operation)); !launched) {
                std::fprintf(stderr, "Cannot start project operation: %s\n", launched.message.c_str());
                return 1;
            }
        }
        return application.run(parse_frame_limit(argc, argv), has_argument(argc, argv, "--stress-resize"),
                               has_argument(argc, argv, "--open-preferences"),
                               has_argument(argc, argv, "--stress-native-viewports"), *console_interrupt_result.value,
                               verify_feature_preview, expect_project_rejected, expected_scene);
    } catch (...) {
        // Startup is also an application boundary: a malformed argument,
        // filesystem failure, or backend exception must have a controlled exit.
        return 1;
    }
}
