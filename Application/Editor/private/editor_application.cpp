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
#include <functional>
#include <limits>
#include <memory>
#include <new>
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
#include "editor_workspace.hpp"
#include "engine_frame_orchestrator.hpp"
#include "font/editor_system_font_catalog.hpp"
#include "frame_context.hpp"
#include "imgui_renderer.hpp"
#include "imgui_texture_registry.hpp"
#include "input_system.hpp"
#include "logging.hpp"
#include "pipeline.hpp"
#include "render_graph.hpp"
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
        static_cast<void>(aeg::application::show_error_dialog("AEG Editor Error",
                                                              "The editor could not complete an initialization step."));
    } catch (...) {
        static_cast<void>(aeg::application::show_error_dialog("AEG Editor Error",
                                                              "The editor could not complete an initialization step."));
    }
    return false;
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
        ImGui::Begin("Scene Viewport");
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
        if (ImGui::Button("Play")) {
            last_operation_ = runtime_.play();
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause")) {
            last_operation_ = runtime_.pause();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
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
        ImGui::Separator();
        ImGui::TextUnformatted("Gizmo:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Move", gizmo_mode_ == ViewportGizmoMode::kTranslate)) {
            gizmo_mode_ = ViewportGizmoMode::kTranslate;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", gizmo_mode_ == ViewportGizmoMode::kRotate)) {
            gizmo_mode_ = ViewportGizmoMode::kRotate;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale", gizmo_mode_ == ViewportGizmoMode::kScale)) {
            gizmo_mode_ = ViewportGizmoMode::kScale;
        }
        ImGui::Separator();
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
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            draw_gizmo(image_min, extent, width, height);
            handle_input(image_min, extent, width, height, hovered);
        } else {
            ImGui::TextUnformatted("Scene target is preparing...");
        }
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
    Result last_operation_{Result::success()};
};

class HierarchyPanel final : public aeg::editor::EditorPanel {
public:
    explicit HierarchyPanel(EditorSceneRuntime& runtime) noexcept : runtime_(runtime) {}

    void build_ui() override {
        ImGui::Begin("Hierarchy");
        if (ImGui::Button("Expand All")) {
            expand_all_ = true;
            collapse_all_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Collapse All")) {
            collapse_all_ = true;
            expand_all_ = false;
        }
        if (!last_operation_) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", last_operation_.message.c_str());
        }
        const auto roots = runtime_.children({});
        if (roots) {
            for (std::size_t index = 0; index < roots.value.size(); ++index) {
                draw_drop_zone({}, index);
                draw_entity(roots.value[index]);
            }
            draw_drop_zone({}, roots.value.size());
        }
        ImGui::Dummy(ImVec2{0.0f, 8.0f});
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
        ImGui::PushID(static_cast<int>((sibling_index + parent.low()) & 0x7fffffff));
        ImGui::InvisibleButton("##HierarchyDropZone", ImVec2{-1.0f, 3.0f});
        if (ImGui::BeginDragDropTarget()) {
            accept_drop(parent, sibling_index, HierarchyTransformPolicy::kKeepWorld);
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
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
        const auto child_entities = runtime_.children(entity);
        const bool has_children = child_entities && !child_entities.value.empty();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!has_children) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (runtime_.is_selected(entity)) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (expand_all_) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        } else if (collapse_all_) {
            ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        }
        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity.low())), flags, "%s",
                                            record->name.c_str());
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
    }

    EditorSceneRuntime& runtime_;
    std::vector<scene::SceneEntityId> drag_entities_{};
    bool expand_all_{false};
    bool collapse_all_{false};
    Result last_operation_{Result::success()};
};

class InspectorPanel final : public aeg::editor::EditorPanel {
public:
    explicit InspectorPanel(EditorSceneRuntime& runtime) noexcept : runtime_(runtime) {}

    void build_ui() override {
        ImGui::Begin("Inspector");
        const auto selected_entities = runtime_.selected_entities();
        if (selected_entities.empty()) {
            finish_component_transaction();
            ImGui::TextUnformatted("No entity selected.");
            ImGui::End();
            return;
        }
        ImGui::Text("Entities: %llu%s", static_cast<unsigned long long>(selected_entities.size()),
                    selected_entities.size() > 1 ? " (multi-edit)" : "");
        if (selected_entities.size() == 1) {
            const auto path = runtime_.hierarchy_path(selected_entities.front());
            if (path) {
                ImGui::Text("Path: %s", path.value.c_str());
            }
            draw_entity_metadata(selected_entities.front());
        }
        ImGui::Separator();

        const auto common_components = runtime_.selected_component_types();
        if (common_components.empty()) {
            ImGui::TextUnformatted("No common editable components.");
        }
        if (ImGui::Button("Add Component")) {
            ImGui::OpenPopup("InspectorAddComponent");
        }
        if (ImGui::BeginPopup("InspectorAddComponent")) {
            const bool single_selection = selected_entities.size() == 1;
            const auto has_common = [&common_components](ComponentTypeId type) {
                return std::find(common_components.begin(), common_components.end(), type) != common_components.end();
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
        ImGui::Text("Entity ID: %016llx:%016llx", static_cast<unsigned long long>(entity.high()),
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
        ImGui::SameLine();
        ImGui::Button(resource_label(expected_kind, *id).c_str(), ImVec2{150.0f, 0.0f});
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
                            changed = ImGui::DragFloat(field.name.c_str(), static_cast<float*>(field_value), 0.01f,
                                                       minimum, maximum, "%.4f", flags);
                            break;
                        }
                        case aeg::reflection::FieldValueKind::kUInt32:
                            changed = ImGui::InputScalar(field.name.c_str(), ImGuiDataType_U32, field_value);
                            break;
                        case aeg::reflection::FieldValueKind::kUInt64:
                            changed = ImGui::InputScalar(field.name.c_str(), ImGuiDataType_U64, field_value);
                            break;
                        case aeg::reflection::FieldValueKind::kBool:
                            changed = ImGui::Checkbox(field.name.c_str(), static_cast<bool*>(field_value));
                            break;
                        case aeg::reflection::FieldValueKind::kVec3: {
                            auto* vector = static_cast<aeg::math::Vec3*>(field_value);
                            if (normalized_color) {
                                changed = ImGui::ColorEdit3(field.name.c_str(), &vector->x);
                                if (component == ComponentTypeId(aeg::engine::components::kMeshRendererName) &&
                                    field.name == "tint") {
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip(
                                            "Tint multiplies the sampled material texture; it is not a final-color "
                                            "override.");
                                    }
                                    const bool tint_item_deactivated = ImGui::IsItemDeactivatedAfterEdit();
                                    ImGui::SameLine();
                                    ImGui::TextDisabled("texture x tint x light");
                                    if (tint_item_deactivated) {
                                        deactivated = true;
                                    }
                                }
                            } else {
                                changed = ImGui::DragFloat3(field.name.c_str(), &vector->x, 0.01f);
                            }
                            break;
                        }
                        case aeg::reflection::FieldValueKind::kQuat: {
                            auto* quaternion = static_cast<aeg::math::Quat*>(field_value);
                            changed = ImGui::DragFloat4(field.name.c_str(), &quaternion->x, 0.01f);
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
            ImGui::PushID(
                static_cast<int>((descriptor.id ^ (static_cast<uint64_t>(descriptor.kind) << 32u)) & 0x7fffffff));
            const char* kind = resource_kind_name(descriptor.kind);
            const bool project_asset = !descriptor.path.empty();
            if (ImGui::Selectable(name.c_str(), selected_id_ == descriptor.id, ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_id_ = descriptor.id;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    preview_id_ = descriptor.id;
                    open_preview = true;
                }
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                const ResourceDragPayload payload{resource_kind_value(descriptor.kind), descriptor.id};
                ImGui::SetDragDropPayload(kResourcePayload, &payload, sizeof(payload));
                ImGui::Text("%s: %s", kind, name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s #%llu [%s]", kind, static_cast<unsigned long long>(descriptor.id),
                                editor_asset_status_name(descriptor.status).data());
            if (ImGui::BeginPopupContextItem("AssetContext")) {
                if (ImGui::MenuItem("Reimport", nullptr, false, project_asset)) {
                    last_operation_ = runtime_.reimport_asset(descriptor.id);
                }
                if (ImGui::MenuItem("Rename", nullptr, false, project_asset)) {
                    rename_id_ = descriptor.id;
                    std::fill(rename_buffer_.begin(), rename_buffer_.end(), '\0');
                    std::memcpy(rename_buffer_.data(), name.data(), std::min(name.size(), rename_buffer_.size() - 1u));
                    open_rename = true;
                }
                if (ImGui::MenuItem("Delete", nullptr, false, project_asset)) {
                    pending_delete = descriptor.id;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
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
                return descriptor.id == selected_id_;
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
            const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                            [this](const auto& descriptor) { return descriptor.id == preview_id_; });
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
        float font_size_pixels{18.0f};
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
    float pending_font_size_pixels_{18.0f};
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
                "AEG Editor Startup Failure",
                "The editor could not create its log directory. Check the user data directory."));
            return false;
        }
        aeg::logging::Config logging_config{};
        logging_config.file_enabled = true;
        logging_config.file_path = log_path;
        const aeg::logging::Status logging_status = aeg::logging::initialize(logging_config);
        if (!logging_status) {
            static_cast<void>(aeg::application::show_error_dialog(
                "AEG Editor Startup Failure",
                "The editor could not initialize its log file. Check the user data directory."));
            return false;
        }
        auto window_system_result = aeg::application::create_win32_window_system();
        if (!report_result(window_system_result, "create_win32_window_system")) {
            return false;
        }
        window_system_ = std::move(window_system_result.value);

        aeg::platform::WindowDesc window_desc{};
        window_desc.title = "AEG Engine Editor";
        window_desc.extent = {1, 1};
        window_desc.visible = false;
        const auto window_result = window_system_->create_window(window_desc);
        if (!report_result(window_result, "create_editor_window")) {
            return false;
        }
        window_ = window_result.value;
        if (!report_result(configure_window_for_work_area(*window_, 80, 80, true), "configure_editor_window")) {
            return false;
        }

        const DeviceCreateDesc device_desc{
            aeg::rhi::BackendType::kVulkan, "AEG Editor", std::numeric_limits<uint32_t>::max(), true, true,
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
        imgui_desc.font_size_pixels = 18.0f;
        if (!report_result(imgui_host_.initialize(imgui_desc), "initialize_editor_imgui")) {
            return false;
        }
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
            !report_result(workspace_.register_panel(EditorPanelDescriptor{"hierarchy", "Hierarchy",
                                                                           EditorDockRegion::kLeft, true, true, true},
                                                     std::make_unique<HierarchyPanel>(*scene_runtime_)),
                           "register_hierarchy_panel") ||
            !report_result(workspace_.register_panel(EditorPanelDescriptor{"inspector", "Inspector",
                                                                           EditorDockRegion::kRight, true, true, true},
                                                     std::make_unique<InspectorPanel>(*scene_runtime_)),
                           "register_inspector_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"assets", "Assets", EditorDockRegion::kBottom, true, true, true},
                               std::make_unique<AssetsPanel>(*scene_runtime_)),
                           "register_assets_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"console", "Console", EditorDockRegion::kBottom, true, true, true},
                               std::make_unique<ConsolePanel>(frame_count_)),
                           "register_console_panel") ||
            !report_result(workspace_.register_view_action(
                               aeg::editor::EditorWorkspaceActionDescriptor{"preferences", "Preferences",
                                                                            [this]() {
                                                                                preferences_open_requested_ = true;
                                                                                return Result::success();
                                                                            }}),
                           "register_preferences_action")) {
            return false;
        }
        last_frame_time_ = std::chrono::steady_clock::now();
        initialized_ = true;
        return true;
    }

    [[nodiscard]] int run(uint32_t frame_limit, bool resize_regression, bool open_preferences_regression) {
        regression_mode_ = resize_regression || open_preferences_regression;
        resize_regression_ = resize_regression;
        preferences_regression_mode_ = open_preferences_regression;
        preferences_regression_opened_ = false;
        preferences_regression_closed_ = false;
        preferences_regression_reopened_ = false;
        preferences_regression_preview_requested_ = false;
        preferences_regression_preview_completed_ = false;
        preferences_open_requested_ = open_preferences_regression;
        while (!close_requested()) {
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
                if (scene_runtime_->viewport_resize_pending() && imgui_renderer_ != nullptr &&
                    imgui_renderer_->has_prepared_frame()) {
                    scene_frame_result = imgui_renderer_->release_prepared_frame();
                    if (!scene_frame_result) {
                        notify_runtime_result(scene_frame_result, "release_scene_viewport_frame");
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
        if (destructive && scene_runtime_->dirty()) {
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
                result = scene_runtime_->save_scene();
                break;
            case EditorDocumentActionType::kSaveAsScene:
                result = scene_runtime_->save_scene_as(action.path);
                break;
            case EditorDocumentActionType::kNewProject:
                result = scene_runtime_->new_project(action.path, action.name);
                break;
            case EditorDocumentActionType::kImportProject:
                result = scene_runtime_->import_project(action.path);
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
                unsaved_operation_ = scene_runtime_->save_scene();
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

        if (!ImGui::BeginMainMenuBar()) {
            return;
        }
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
        ImGui::SameLine();
        if (scene_runtime_->project_loaded()) {
            ImGui::Text("Project: %s | Scene %s", std::string(scene_runtime_->project_name()).c_str(),
                        scene_runtime_->dirty() ? "*" : "clean");
        } else {
            ImGui::Text("No Game Project | Scene %s", scene_runtime_->dirty() ? "*" : "clean");
        }
        ImGui::EndMainMenuBar();

        if (project_dialog_pending_) {
            ImGui::OpenPopup("ProjectDialog");
            project_dialog_pending_ = false;
        }
        if (ImGui::BeginPopupModal("ProjectDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool creating = project_dialog_mode_ == ProjectDialogMode::kNew;
            ImGui::TextUnformatted(creating ? "Create a game project" : "Import a game project");
            ImGui::InputText("Project directory", project_path_buffer_.data(), project_path_buffer_.size());
            if (creating) {
                ImGui::InputText("Project name", project_name_buffer_.data(), project_name_buffer_.size());
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
            static_cast<void>(aeg::application::show_error_dialog("AEG Editor Error", message));
        } catch (...) {
            static_cast<void>(aeg::application::show_error_dialog(
                "AEG Editor Error", "The editor could not display a runtime error notification."));
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
        window_desc.title = "AEG Editor Preferences";
        window_desc.extent = {1, 1};
        window_desc.visible = false;
        const auto window_result = window_system_->create_window(window_desc);
        if (!window_result) {
            return window_result.result;
        }
        preferences_window_ = window_result.value;

        Result result = configure_window_for_work_area(*preferences_window_, 48, 54, false);
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
                                            aeg::rhi::ShaderStage::kAll, ResourceUsage::kCopySource);
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
            state.font_size_pixels = 18.0f;
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
                                            aeg::rhi::ShaderStage::kAll, ResourceUsage::kCopySource);
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
    EditorApplication application;
    const std::filesystem::path resource_root = std::filesystem::absolute(std::filesystem::path{argv[0]}).parent_path();
    if (!application.initialize(resource_root, parse_path_argument(argc, argv, "--project"))) {
        return 1;
    }
    return application.run(parse_frame_limit(argc, argv), has_argument(argc, argv, "--stress-resize"),
                           has_argument(argc, argv, "--open-preferences"));
}
