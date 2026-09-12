/**
 * @file editor_application.cpp
 * @brief Minimal editor application assembled from the public engine contracts.
 */
#include <imgui.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "application_platform.hpp"
#include "editor_error.hpp"
#include "editor_imgui.hpp"
#include "editor_preferences.hpp"
#include "font/editor_system_font_catalog.hpp"
#include "editor_workspace.hpp"
#include "engine_frame_orchestrator.hpp"
#include "frame_context.hpp"
#include "imgui_renderer.hpp"
#include "imgui_texture_registry.hpp"
#include "input_system.hpp"
#include "pipeline.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "texture_uploader.hpp"

namespace {
using aeg::editor::EditorImGuiFrameDesc;
using aeg::editor::EditorErrorNotice;
using aeg::editor::EditorErrorSeverity;
using aeg::editor::EditorErrorState;
using aeg::editor::EditorFontCatalog;
using aeg::editor::EditorPreferenceModel;
using aeg::editor::EditorPreferencePanel;
using aeg::editor::EditorPreferenceState;
using aeg::editor::IEditorPreferenceActions;
using aeg::editor::EditorImGuiHost;
using aeg::editor::EditorDockRegion;
using aeg::editor::EditorPanelDescriptor;
using aeg::editor::ImGuiDrawPacket;
using aeg::editor::ImGuiFontAtlasData;
using aeg::editor::ImGuiRenderTargetDesc;
using aeg::editor::ImGuiRenderer;
using aeg::editor::ImGuiTextureBinding;
using aeg::editor::ImGuiTextureHandle;
using aeg::editor::ImGuiTextureRegistry;
using aeg::editor::EditorWorkspace;
using aeg::application::EditorSystemFontCatalog;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::input::InputSystem;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::PipelineScheduler;
using aeg::pipeline::PipelineScope;
using aeg::pipeline::PipelineScopeHost;
using aeg::platform::IWindow;
using aeg::platform::IWindowSystem;
using aeg::platform::DisplayWorkArea;
using aeg::platform::WindowEvent;
using aeg::platform::WindowEventType;
using aeg::render_graph::FrameAllocation;
using aeg::render_graph::FrameContext;
using aeg::render_graph::RenderGraph;
using aeg::resource::GpuTexture;
using aeg::resource::TextureData;
using aeg::resource::TextureSubresourceData;
using aeg::resource::TextureUploader;
using aeg::rhi::Device;
using aeg::rhi::DeviceCreateDesc;
using aeg::rhi::ClearColorValue;
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
    try {
        std::string message;
        message.reserve(operation.size() + result.message.size() + 32u);
        message.append(operation);
        message.append(" failed [");
        message.append(aeg::foundation::result_code_name(result.code));
        message.append("]: ");
        message.append(result.message.empty() ? "No diagnostic was provided." : result.message);
        static_cast<void>(aeg::application::show_error_dialog("AEG Editor Error", message));
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

class SceneViewportPanel final : public aeg::editor::EditorPanel {
public:
    void build_ui() override {
        ImGui::Begin("Scene Viewport");
        ImGui::TextUnformatted("Central workspace for the active scene and engine-rendered preview.");
        ImGui::Separator();
        ImGui::TextUnformatted("The viewport is reserved for Renderer and RenderGraph output.");
        ImGui::End();
    }
};

class HierarchyPanel final : public aeg::editor::EditorPanel {
public:
    void build_ui() override {
        ImGui::Begin("Hierarchy");
        ImGui::TextUnformatted("Scene entities and parent-child relationships.");
        ImGui::Separator();
        ImGui::TextUnformatted("Selection changes will feed the editor selection bridge.");
        ImGui::End();
    }
};

class InspectorPanel final : public aeg::editor::EditorPanel {
public:
    void build_ui() override {
        ImGui::Begin("Inspector");
        ImGui::TextUnformatted("Properties and components for the current selection.");
        ImGui::Separator();
        ImGui::TextUnformatted("Component editing belongs to Application and Reflection bridges.");
        ImGui::End();
    }
};

class AssetsPanel final : public aeg::editor::EditorPanel {
public:
    void build_ui() override {
        ImGui::Begin("Assets");
        ImGui::TextUnformatted("Project assets, source files, and import status.");
        ImGui::Separator();
        ImGui::TextUnformatted("Resource lifetime remains owned by Engine Resource APIs.");
        ImGui::End();
    }
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

    [[nodiscard]] bool initialize() {
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
        if (!report_result(configure_window_for_work_area(*window_, 80, 80, true),
                           "configure_editor_window")) {
            return false;
        }

        const DeviceCreateDesc device_desc{
            aeg::rhi::BackendType::kVulkan,
            "AEG Editor",
            std::numeric_limits<uint32_t>::max(),
            true,
            true,
        };
        auto device_result = aeg::rhi::create_device(device_desc);
        if (!report_result(device_result, "create_device")) {
            return false;
        }
        device_ = std::move(device_result.value);

        const aeg::platform::WindowSurfaceBinding surface_binding = window_->surface_binding();
        if (!surface_binding.is_valid()) {
            static_cast<void>(report_result(
                aeg::foundation::invalid_state("Editor window has no valid surface binding"),
                "create_editor_surface"));
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
        orchestrator_ = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
            registry_, scheduler_, *frame_context_, *render_graph_);
        if (!report_result(orchestrator_->register_pipeline(frame_host_), "register_editor_frame_pipeline")) {
            return false;
        }
        if (!report_result(orchestrator_->initialize(), "initialize_engine_frame_orchestrator")) {
            return false;
        }

        if (!report_result(workspace_.register_panel(
                              EditorPanelDescriptor{"scene_viewport", "Scene Viewport", EditorDockRegion::kCenter,
                                                    true, false, true},
                              std::make_unique<SceneViewportPanel>()),
                          "register_scene_viewport_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"hierarchy", "Hierarchy", EditorDockRegion::kLeft, true, true,
                                                     true},
                               std::make_unique<HierarchyPanel>()),
                           "register_hierarchy_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"inspector", "Inspector", EditorDockRegion::kRight, true, true,
                                                     true},
                               std::make_unique<InspectorPanel>()),
                           "register_inspector_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"assets", "Assets", EditorDockRegion::kBottom, true, true, true},
                               std::make_unique<AssetsPanel>()),
                           "register_assets_panel") ||
            !report_result(workspace_.register_panel(
                               EditorPanelDescriptor{"console", "Console", EditorDockRegion::kBottom, true, true, true},
                               std::make_unique<ConsolePanel>(frame_count_)),
                           "register_console_panel") ||
            !report_result(workspace_.register_view_action(
                               aeg::editor::EditorWorkspaceActionDescriptor{
                                   "preferences",
                                   "Preferences",
                                   [this]() {
                                       preferences_open_requested_ = true;
                                       return Result::success();
                                   } }),
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

            if (resize_regression_ &&
                (frame_count_ == 10 || frame_count_ == 30 || frame_count_ == 50)) {
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

            const aeg::platform::WindowState window_state = window_->state();
            const EditorImGuiFrameDesc imgui_frame_desc{
                window_->id(),
                window_state.extent,
                delta_seconds,
                1.0f,
                1.0f,
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
                if (!preference_model_->select_font(preview_font_key) ||
                    !preference_model_->set_font_size(20.0f) ||
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
             preferences_window_ != nullptr ||
             !main_window_input_enabled_ || !preferences_regression_preview_requested_ ||
             !preferences_regression_preview_completed_ || preference_model_ == nullptr ||
             imgui_host_.active_font_key() != preference_model_->applied_state().active_font_key ||
             imgui_host_.active_font_size_pixels() != preference_model_->applied_state().font_size_pixels)) {
            return 1;
        }
        return 0;
    }

private:
    /** @brief Routes one runtime failure to the editor notification or fatal fallback. */
    void notify_runtime_result(const Result& result, std::string_view operation) noexcept {
        if (result) {
            return;
        }
        const EditorErrorSeverity severity = is_fatal_runtime_error(result.code)
                                                 ? EditorErrorSeverity::kFatal
                                                 : EditorErrorSeverity::kRecoverable;
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
            message.append(notice->operation);
            message.append(" failed [");
            message.append(aeg::foundation::result_code_name(notice->code));
            message.append("]: ");
            message.append(notice->message.empty() ? "No diagnostic was provided." : notice->message);
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
        if (preferences_swapchain_info_.format == Format::kUnknown ||
            preferences_swapchain_info_.extent.width == 0 || preferences_swapchain_info_.extent.height == 0) {
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
        if (!(result = aeg::editor::register_imgui_render_feature(preferences_registry_,
                                                                    *preferences_imgui_renderer_))) {
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
            keep_first_error(window_->focus());
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
        const EditorImGuiFrameDesc frame_desc{
            preferences_window_->id(), window_state.extent, delta_seconds, 1.0f, 1.0f};
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
        const ImGuiRenderTargetDesc target{
            aeg::engine::kSwapchainImageResourceId,
            preferences_swapchain_info_.format,
            preferences_swapchain_info_.extent,
            regression_mode_ ? ResourceUsage::kCopySource : ResourceUsage::kPresent};
        aeg::engine::EngineFrameDesc engine_frame_desc{};
        engine_frame_desc.fixed_tick = frame_count_;
        engine_frame_desc.acquire_swapchain_image = true;
        engine_frame_desc.swapchain = preferences_swapchain_;
        engine_frame_desc.swapchain_image_desc = make_swapchain_image_desc_for(
            preferences_swapchain_info_, "Editor.Preferences.SwapchainImage");
        engine_frame_desc.present_acquired_frame = true;
        FrameAllocation readback_allocation{};
        bool has_readback = false;
        engine_frame_desc.render_feature_preparation =
            [this, &draw_packet, target, &readback_allocation, &has_readback](
                    const aeg::engine::EngineFramePreparationContext& context) {
            if (context.acquired_frame == nullptr || !context.swapchain_resource.is_valid()) {
                return aeg::foundation::invalid_state("Preferences requires an acquired swapchain image");
            }
            const auto clear_result = context.render_graph.add_pass(
                "Editor.Preferences.ClearSwapchain", QueueClass::kGraphics,
                [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                    return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kClear,
                                                        aeg::rhi::StoreOp::kStore,
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
                        preferences_imgui_renderer_->prepare_frame(context.frame_context, context.render_graph,
                                                                   draw_packet, target);
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
                                         aeg::rhi::BufferCapability::kVertex |
                                         aeg::rhi::BufferCapability::kIndex |
                                         aeg::rhi::BufferCapability::kUniform;
            readback_desc.memory = aeg::rhi::MemoryClass::kReadback;
            readback_desc.initial_usage = ResourceUsage::kHostRead;
            readback_desc.initial_stages = PipelineStage::kHost;
            readback_desc.initial_queue = QueueClass::kCopy;
            readback_desc.name = aeg::rhi::DebugName{"Editor.Preferences.FramebufferReadback"};
            const auto readback_resource_result = context.render_graph.import_buffer(
                readback_allocation.buffer, readback_desc,
                ResourceState{ResourceUsage::kHostRead, aeg::rhi::ShaderStage::kAll, QueueClass::kCopy, false,
                              PipelineStage::kHost});
            if (!readback_resource_result) {
                return readback_resource_result.result;
            }
            const auto readback_pass_result = context.render_graph.add_pass(
                "Editor.Preferences.FramebufferReadback", QueueClass::kCopy,
                [swapchain_resource = context.swapchain_resource,
                 readback_resource = readback_resource_result.value,
                 readback_allocation, pixel_count](aeg::render_graph::PassBuilder& builder) {
                    if (const Result result = builder.read_to(
                            swapchain_resource, ResourceUsage::kCopySource, PipelineStage::kCopy,
                            aeg::rhi::ShaderStage::kAll, ResourceUsage::kCopySource);
                        !result) {
                        return result;
                    }
                    if (const Result result = builder.write_to(
                            readback_resource, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                            aeg::rhi::ShaderStage::kAll, ResourceUsage::kHostRead,
                            aeg::render_graph::RenderGraphBufferRange{readback_allocation.offset,
                                                                       pixel_count * 4u},
                            true);
                        !result) {
                        return result;
                    }
                    builder.set_side_effects();
                    return Result::success();
                },
                [swapchain_resource = context.swapchain_resource,
                 readback_resource = readback_resource_result.value, readback_allocation, target](
                        aeg::render_graph::PassExecutionContext& pass) {
                    return pass.copy_texture_to_buffer(
                        swapchain_resource,
                        aeg::rhi::TextureSubresourceLayers{aeg::rhi::Aspect::kColor, 0, 0, 1},
                        aeg::rhi::Offset3D{0, 0, 0},
                        aeg::rhi::Extent3D{target.extent.width, target.extent.height, 1}, readback_resource,
                        readback_allocation.offset, 0, 0);
                });
            if (!readback_pass_result) {
                return readback_pass_result.result;
            }
            const auto present_pass_result = context.render_graph.add_pass(
                "Editor.Preferences.PresentSwapchain", QueueClass::kGraphics,
                [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                    return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kLoad,
                                                        aeg::rhi::StoreOp::kStore, ClearColorValue{},
                                                        ResourceUsage::kPresent);
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
                clear_probes_are_clean = clear_probes_are_clean &&
                                         pixel_is_clear(x, y) &&
                                         pixel_is_clear(target.extent.width - 1u - x, y) &&
                                         pixel_is_clear(x, target.extent.height - 1u - y) &&
                                         pixel_is_clear(target.extent.width - 1u - x,
                                                        target.extent.height - 1u - y);
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
                    static_cast<void>(preference_model_->set_error(preview_result.message));
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
            notify_runtime_result(
                aeg::foundation::Result::failure(aeg::foundation::ResultCode::kOutOfMemory),
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
                static_cast<void>(preference_model_->set_error(set_font_result.message));
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
                static_cast<void>(preference_model_->set_error(refresh_result.message));
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
        return window_ == nullptr || window_->state().close_requested;
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
        const auto upload_result = texture_uploader_->upload_texture(
            frame_context, upload_graph, texture_data, texture_name);
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
            const Result wait_result = device_->wait(execution_result.value.completion, std::numeric_limits<uint64_t>::max());
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
            texture.sampled_view,
            font_sampler_,
            texture.texture,
            texture_desc,
            upload_state,
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
        if (const Result result = upload_font_texture(preferences_imgui_host_, *preferences_frame_context_,
                                                      "Editor.Preferences.ImGui.FontTexture", replacement_desc,
                                                      replacement_texture);
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
            const Result restore_result = preferences_imgui_host_.set_preview_font(previous_font_key, previous_font_size);
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
                        return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kClear,
                                                            aeg::rhi::StoreOp::kStore,
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
                                         aeg::rhi::BufferCapability::kVertex |
                                         aeg::rhi::BufferCapability::kIndex |
                                         aeg::rhi::BufferCapability::kUniform;
            readback_desc.memory = aeg::rhi::MemoryClass::kReadback;
            readback_desc.initial_usage = ResourceUsage::kHostRead;
            readback_desc.initial_stages = PipelineStage::kHost;
            readback_desc.initial_queue = QueueClass::kCopy;
            readback_desc.name = aeg::rhi::DebugName{"Editor.FramebufferReadback"};
            const auto readback_resource_result = context.render_graph.import_buffer(
                    readback_allocation.buffer, readback_desc,
                    ResourceState{ResourceUsage::kHostRead, aeg::rhi::ShaderStage::kAll, QueueClass::kCopy, false,
                                  PipelineStage::kHost});
            if (!readback_resource_result) {
                return readback_resource_result.result;
            }
            const auto readback_pass_result = context.render_graph.add_pass(
                    "Editor.FramebufferReadback", QueueClass::kCopy,
                    [swapchain_resource = context.swapchain_resource,
                     readback_resource = readback_resource_result.value,
                     readback_allocation, pixel_count](aeg::render_graph::PassBuilder& builder) {
                        if (const Result result = builder.read_to(
                                    swapchain_resource, ResourceUsage::kCopySource, PipelineStage::kCopy,
                                    aeg::rhi::ShaderStage::kAll, ResourceUsage::kCopySource);
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.write_to(
                                    readback_resource, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                                    aeg::rhi::ShaderStage::kAll,
                                    ResourceUsage::kHostRead,
                                    aeg::render_graph::RenderGraphBufferRange{readback_allocation.offset,
                                                                              pixel_count * 4u},
                                    true);
                            !result) {
                            return result;
                        }
                        builder.set_side_effects();
                        return Result::success();
                    },
                    [swapchain_resource = context.swapchain_resource,
                     readback_resource = readback_resource_result.value, readback_allocation, target](
                            aeg::render_graph::PassExecutionContext& pass) {
                        return pass.copy_texture_to_buffer(
                                swapchain_resource,
                                aeg::rhi::TextureSubresourceLayers{aeg::rhi::Aspect::kColor, 0, 0, 1},
                                aeg::rhi::Offset3D{0, 0, 0},
                                aeg::rhi::Extent3D{target.extent.width, target.extent.height, 1}, readback_resource,
                                readback_allocation.offset, 0, 0);
                    });
            if (!readback_pass_result) {
                return readback_pass_result.result;
            }
            const auto present_pass_result = context.render_graph.add_pass(
                    "Editor.PresentSwapchain", QueueClass::kGraphics,
                    [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                        return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kLoad,
                                                            aeg::rhi::StoreOp::kStore, ClearColorValue{},
                                                            ResourceUsage::kPresent);
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
                clear_probes_are_clean = clear_probes_are_clean &&
                                         pixel_is_clear(x, y) &&
                                         pixel_is_clear(target.extent.width - 1u - x, y) &&
                                         pixel_is_clear(x, target.extent.height - 1u - y) &&
                                         pixel_is_clear(target.extent.width - 1u - x,
                                                        target.extent.height - 1u - y);
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
    EditorImGuiHost imgui_host_{};
    /** @brief Application-owned five-panel editor workspace. */
    EditorWorkspace workspace_;
    std::unique_ptr<ImGuiRenderer> imgui_renderer_{};
    PipelineRegistry registry_{};
    PipelineScheduler scheduler_{{1, 1, 1, 1}};
    PipelineScopeHost frame_host_{PipelineScope::kFrame,
                                  aeg::pipeline::PipelineInstanceId("Editor.Frame")};
    std::unique_ptr<aeg::engine::EngineFrameOrchestrator> orchestrator_{};
    InputSystem input_system_{};
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
    if (!application.initialize()) {
        return 1;
    }
    return application.run(parse_frame_limit(argc, argv), has_argument(argc, argv, "--stress-resize"),
                           has_argument(argc, argv, "--open-preferences"));
}
