#include "imgui_platform_bridge.hpp"

#include <imgui.h>

#include <cfloat>
#include <cmath>
#include <memory>
#include <new>
#include <utility>

namespace aeg::editor::detail {
namespace {
ImGuiKey translate_key(platform::KeyCode key) noexcept {
    using platform::KeyCode;
    switch (key) {
        case KeyCode::kTab:
            return ImGuiKey_Tab;
        case KeyCode::kLeft:
            return ImGuiKey_LeftArrow;
        case KeyCode::kRight:
            return ImGuiKey_RightArrow;
        case KeyCode::kUp:
            return ImGuiKey_UpArrow;
        case KeyCode::kDown:
            return ImGuiKey_DownArrow;
        case KeyCode::kPageUp:
            return ImGuiKey_PageUp;
        case KeyCode::kPageDown:
            return ImGuiKey_PageDown;
        case KeyCode::kHome:
            return ImGuiKey_Home;
        case KeyCode::kEnd:
            return ImGuiKey_End;
        case KeyCode::kInsert:
            return ImGuiKey_Insert;
        case KeyCode::kDelete:
            return ImGuiKey_Delete;
        case KeyCode::kBackspace:
            return ImGuiKey_Backspace;
        case KeyCode::kSpace:
            return ImGuiKey_Space;
        case KeyCode::kEnter:
            return ImGuiKey_Enter;
        case KeyCode::kEscape:
            return ImGuiKey_Escape;
        case KeyCode::kLeftCtrl:
            return ImGuiKey_LeftCtrl;
        case KeyCode::kLeftShift:
            return ImGuiKey_LeftShift;
        case KeyCode::kLeftAlt:
            return ImGuiKey_LeftAlt;
        case KeyCode::kLeftSuper:
            return ImGuiKey_LeftSuper;
        case KeyCode::kRightCtrl:
            return ImGuiKey_RightCtrl;
        case KeyCode::kRightShift:
            return ImGuiKey_RightShift;
        case KeyCode::kRightAlt:
            return ImGuiKey_RightAlt;
        case KeyCode::kRightSuper:
            return ImGuiKey_RightSuper;
        case KeyCode::kA:
            return ImGuiKey_A;
        case KeyCode::kB:
            return ImGuiKey_B;
        case KeyCode::kC:
            return ImGuiKey_C;
        case KeyCode::kD:
            return ImGuiKey_D;
        case KeyCode::kE:
            return ImGuiKey_E;
        case KeyCode::kF:
            return ImGuiKey_F;
        case KeyCode::kG:
            return ImGuiKey_G;
        case KeyCode::kH:
            return ImGuiKey_H;
        case KeyCode::kI:
            return ImGuiKey_I;
        case KeyCode::kJ:
            return ImGuiKey_J;
        case KeyCode::kK:
            return ImGuiKey_K;
        case KeyCode::kL:
            return ImGuiKey_L;
        case KeyCode::kM:
            return ImGuiKey_M;
        case KeyCode::kN:
            return ImGuiKey_N;
        case KeyCode::kO:
            return ImGuiKey_O;
        case KeyCode::kP:
            return ImGuiKey_P;
        case KeyCode::kQ:
            return ImGuiKey_Q;
        case KeyCode::kR:
            return ImGuiKey_R;
        case KeyCode::kS:
            return ImGuiKey_S;
        case KeyCode::kT:
            return ImGuiKey_T;
        case KeyCode::kU:
            return ImGuiKey_U;
        case KeyCode::kV:
            return ImGuiKey_V;
        case KeyCode::kW:
            return ImGuiKey_W;
        case KeyCode::kX:
            return ImGuiKey_X;
        case KeyCode::kY:
            return ImGuiKey_Y;
        case KeyCode::kZ:
            return ImGuiKey_Z;
        case KeyCode::kF1:
            return ImGuiKey_F1;
        case KeyCode::kF2:
            return ImGuiKey_F2;
        case KeyCode::kF3:
            return ImGuiKey_F3;
        case KeyCode::kF4:
            return ImGuiKey_F4;
        case KeyCode::kF5:
            return ImGuiKey_F5;
        case KeyCode::kF6:
            return ImGuiKey_F6;
        case KeyCode::kF7:
            return ImGuiKey_F7;
        case KeyCode::kF8:
            return ImGuiKey_F8;
        case KeyCode::kF9:
            return ImGuiKey_F9;
        case KeyCode::kF10:
            return ImGuiKey_F10;
        case KeyCode::kF11:
            return ImGuiKey_F11;
        case KeyCode::kF12:
            return ImGuiKey_F12;
        case KeyCode::kUnknown:
        case KeyCode::kCount:
            return ImGuiKey_None;
    }
    return ImGuiKey_None;
}

void submit_modifiers(ImGuiIO& io, platform::KeyModifiers modifiers) noexcept {
    io.AddKeyEvent(ImGuiMod_Ctrl, platform::has_modifier(modifiers, platform::KeyModifiers::kCtrl));
    io.AddKeyEvent(ImGuiMod_Shift, platform::has_modifier(modifiers, platform::KeyModifiers::kShift));
    io.AddKeyEvent(ImGuiMod_Alt, platform::has_modifier(modifiers, platform::KeyModifiers::kAlt));
    io.AddKeyEvent(ImGuiMod_Super, platform::has_modifier(modifiers, platform::KeyModifiers::kSuper));
}

ImGuiPlatformBridgeContext* current_bridge_context() noexcept {
    if (ImGui::GetCurrentContext() == nullptr) {
        return nullptr;
    }
    ImGuiIO& io = ImGui::GetIO();
    return static_cast<ImGuiPlatformBridgeContext*>(io.BackendPlatformUserData);
}

ImGuiPlatformViewportData* viewport_data(ImGuiViewport* viewport) noexcept {
    if (viewport == nullptr) {
        return nullptr;
    }
    return static_cast<ImGuiPlatformViewportData*>(viewport->PlatformUserData);
}

bool to_window_extent(const ImVec2& size, platform::WindowExtent& extent) noexcept {
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 1.0f || size.y < 1.0f ||
        size.x > static_cast<float>(UINT32_MAX) || size.y > static_cast<float>(UINT32_MAX)) {
        return false;
    }
    extent = platform::WindowExtent{static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)};
    return extent.is_valid();
}

platform::WindowPosition to_window_position(const ImVec2& position) noexcept {
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
        return {};
    }
    return platform::WindowPosition{static_cast<int32_t>(position.x), static_cast<int32_t>(position.y)};
}

void update_monitors(ImGuiPlatformBridgeContext& context) noexcept {
    if (context.services == nullptr || !context.window.is_valid()) {
        return;
    }

    platform::WindowPosition position{};
    if (context.window_system != nullptr) {
        try {
            const foundation::ResultValue<std::shared_ptr<platform::IWindow>> window_result =
                context.window_system->find_window(context.window);
            if (window_result) {
                position = window_result.value->state().position;
            }
        } catch (...) {}
    }

    try {
        const foundation::ResultValue<platform::WindowMetrics> metrics_result =
            context.services->query_window_metrics(context.window);
        if (!metrics_result || !metrics_result.value.is_valid()) {
            return;
        }
        const platform::WindowMetrics& metrics = metrics_result.value;
        ImGuiPlatformMonitor monitor{};
        monitor.MainPos = ImVec2(static_cast<float>(position.x), static_cast<float>(position.y));
        monitor.MainSize = ImVec2(static_cast<float>(metrics.display_extent.width),
                                  static_cast<float>(metrics.display_extent.height));
        monitor.WorkPos = monitor.MainPos;
        monitor.WorkSize = monitor.MainSize;
        monitor.DpiScale = metrics.framebuffer_scale_x;

        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        if (platform_io.Monitors.Size == 0) {
            platform_io.Monitors.push_back(monitor);
        } else {
            platform_io.Monitors[0] = monitor;
            platform_io.Monitors.resize(1);
        }
    } catch (...) {}
}

void create_platform_window(ImGuiViewport* viewport) noexcept {
    ImGuiPlatformBridgeContext* context = current_bridge_context();
    if (context == nullptr || context->window_system == nullptr || viewport == nullptr ||
        viewport->PlatformUserData != nullptr) {
        return;
    }

    platform::WindowExtent extent{};
    if (!to_window_extent(viewport->Size, extent)) {
        viewport->PlatformRequestClose = true;
        return;
    }

    auto* data = new (std::nothrow) ImGuiPlatformViewportData{};
    if (data == nullptr) {
        viewport->PlatformRequestClose = true;
        return;
    }
    data->services = context->services;
    data->window_system = context->window_system;

    platform::WindowDesc desc{};
    desc.title = "AEG Editor Viewport";
    desc.extent = extent;
    desc.position = to_window_position(viewport->Pos);
    desc.position_specified = true;
    desc.visible = false;
    try {
        const foundation::ResultValue<std::shared_ptr<platform::IWindow>> window_result =
            context->window_system->create_window(desc);
        if (!window_result) {
            delete data;
            viewport->PlatformRequestClose = true;
            return;
        }
        data->window = window_result.value->id();
        data->window_object = window_result.value;
        data->owns_window = true;
        viewport->PlatformUserData = data;
        viewport->PlatformHandle = data;
    } catch (...) {
        delete data;
        viewport->PlatformRequestClose = true;
    }
}

void destroy_platform_window(ImGuiViewport* viewport) noexcept {
    if (viewport == nullptr) {
        return;
    }
    auto* data = viewport_data(viewport);
    if (data == nullptr) {
        viewport->PlatformHandle = nullptr;
        return;
    }
    ImGuiPlatformBridgeContext* context = current_bridge_context();
    if (context != nullptr && data == &context->main_viewport) {
        context->main_viewport.window_object.reset();
        viewport->PlatformUserData = nullptr;
        viewport->PlatformHandle = nullptr;
        return;
    }
    if (data->owns_window && data->window_system != nullptr && data->window.is_valid()) {
        try {
            static_cast<void>(data->window_system->destroy_window(data->window));
        } catch (...) {}
    }
    delete data;
    viewport->PlatformUserData = nullptr;
    viewport->PlatformHandle = nullptr;
}

void show_platform_window(ImGuiViewport* viewport) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    if (data == nullptr || data->window_object == nullptr) {
        return;
    }
    try {
        static_cast<void>(data->window_object->set_visible(true));
    } catch (...) {}
}

void set_platform_window_position(ImGuiViewport* viewport, ImVec2 position) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    if (data == nullptr || data->window_object == nullptr) {
        return;
    }
    try {
        static_cast<void>(data->window_object->set_position(to_window_position(position)));
    } catch (...) {}
}

ImVec2 get_platform_window_position(ImGuiViewport* viewport) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    if (data == nullptr || data->window_object == nullptr) {
        return viewport != nullptr ? viewport->Pos : ImVec2{};
    }
    try {
        const platform::WindowPosition position = data->window_object->state().position;
        return ImVec2(static_cast<float>(position.x), static_cast<float>(position.y));
    } catch (...) {
        return viewport != nullptr ? viewport->Pos : ImVec2{};
    }
}

void set_platform_window_size(ImGuiViewport* viewport, ImVec2 size) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    if (data == nullptr || data->window_object == nullptr) {
        return;
    }
    platform::WindowExtent extent{};
    if (!to_window_extent(size, extent)) {
        return;
    }
    try {
        static_cast<void>(data->window_object->set_extent(extent));
    } catch (...) {}
}

void set_platform_window_title(ImGuiViewport*, const char*) noexcept {}

ImVec2 get_platform_window_size(ImGuiViewport* viewport) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    if (data == nullptr || data->window_object == nullptr) {
        return viewport != nullptr ? viewport->Size : ImVec2{};
    }
    try {
        const platform::WindowExtent extent = data->window_object->state().extent;
        return ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
    } catch (...) {
        return viewport != nullptr ? viewport->Size : ImVec2{};
    }
}

ImVec2 get_platform_window_framebuffer_scale(ImGuiViewport* viewport) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    if (data == nullptr || data->window_system == nullptr || data->services == nullptr || !data->window.is_valid()) {
        return ImVec2(1.0f, 1.0f);
    }
    try {
        const foundation::ResultValue<platform::WindowMetrics> metrics_result =
            data->services->query_window_metrics(data->window);
        if (!metrics_result || !metrics_result.value.is_valid()) {
            return ImVec2(1.0f, 1.0f);
        }
        return ImVec2(metrics_result.value.framebuffer_scale_x, metrics_result.value.framebuffer_scale_y);
    } catch (...) {
        return ImVec2(1.0f, 1.0f);
    }
}

void set_platform_window_focus(ImGuiViewport* viewport) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    if (data == nullptr || data->window_object == nullptr) {
        return;
    }
    try {
        static_cast<void>(data->window_object->focus());
    } catch (...) {}
}

bool get_platform_window_focus(ImGuiViewport* viewport) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    return data != nullptr && data->window_object != nullptr && data->window_object->state().focused;
}

bool get_platform_window_minimized(ImGuiViewport* viewport) noexcept {
    const ImGuiPlatformViewportData* data = viewport_data(viewport);
    return data != nullptr && data->window_object != nullptr && data->window_object->state().minimized;
}

const char* get_clipboard_text(void* user_data) noexcept {
    auto* context = static_cast<ImGuiPlatformBridgeContext*>(user_data);
    if (context == nullptr || context->services == nullptr) {
        return nullptr;
    }
    try {
        foundation::ResultValue<std::string> result = context->services->get_clipboard_text();
        if (!result) {
            context->clipboard_text.clear();
            return nullptr;
        }
        context->clipboard_text = std::move(result.value);
        return context->clipboard_text.c_str();
    } catch (...) {
        context->clipboard_text.clear();
        return nullptr;
    }
}

void set_clipboard_text(void* user_data, const char* text) noexcept {
    auto* context = static_cast<ImGuiPlatformBridgeContext*>(user_data);
    if (context == nullptr || context->services == nullptr || text == nullptr) {
        return;
    }
    try {
        static_cast<void>(context->services->set_clipboard_text(text));
    } catch (...) {}
}

const char* get_platform_clipboard_text(ImGuiContext*) noexcept {
    ImGuiPlatformBridgeContext* context = current_bridge_context();
    return get_clipboard_text(context);
}

void set_platform_clipboard_text(ImGuiContext*, const char* text) noexcept {
    ImGuiPlatformBridgeContext* context = current_bridge_context();
    set_clipboard_text(context, text);
}

void set_platform_ime_data(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data) noexcept {
    ImGuiPlatformBridgeContext* context = current_bridge_context();
    if (context == nullptr || context->services == nullptr || !context->window.is_valid() || data == nullptr) {
        return;
    }
    const platform::ImeState state{data->WantVisible, data->WantTextInput, data->InputPos.x, data->InputPos.y,
                                   data->InputLineHeight};
    try {
        static_cast<void>(context->services->set_ime_state(context->window, state));
    } catch (...) {}
}

platform::CursorShape translate_cursor(ImGuiMouseCursor cursor) noexcept {
    using platform::CursorShape;
    switch (cursor) {
        case ImGuiMouseCursor_Arrow:
            return CursorShape::kArrow;
        case ImGuiMouseCursor_TextInput:
            return CursorShape::kTextInput;
        case ImGuiMouseCursor_ResizeAll:
            return CursorShape::kResizeAll;
        case ImGuiMouseCursor_ResizeNS:
            return CursorShape::kResizeVertical;
        case ImGuiMouseCursor_ResizeEW:
            return CursorShape::kResizeHorizontal;
        case ImGuiMouseCursor_ResizeNESW:
            return CursorShape::kResizeDiagonalNortheastSouthwest;
        case ImGuiMouseCursor_ResizeNWSE:
            return CursorShape::kResizeDiagonalNorthwestSoutheast;
        case ImGuiMouseCursor_Hand:
            return CursorShape::kHand;
        case ImGuiMouseCursor_Wait:
            return CursorShape::kWait;
        case ImGuiMouseCursor_Progress:
            return CursorShape::kProgress;
        case ImGuiMouseCursor_NotAllowed:
            return CursorShape::kNotAllowed;
        case ImGuiMouseCursor_None:
        case ImGuiMouseCursor_COUNT:
            return CursorShape::kNone;
    }
    return CursorShape::kArrow;
}
}  // namespace

void ImGuiPlatformBridge::install(ImGuiIO& io, ImGuiPlatformBridgeContext& context,
                                  platform::IPlatformServices* services, platform::IWindowSystem* window_system,
                                  bool enable_viewports) noexcept {
    context.services = services;
    context.window_system = window_system;
    context.window = {};
    context.main_viewport = {};
    context.viewports_enabled = enable_viewports && window_system != nullptr;
    context.clipboard_text.clear();
    io.BackendPlatformUserData = &context;

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (services != nullptr) {
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        platform_io.Platform_GetClipboardTextFn = &get_platform_clipboard_text;
        platform_io.Platform_SetClipboardTextFn = &set_platform_clipboard_text;
        platform_io.Platform_ClipboardUserData = &context;
        platform_io.Platform_SetImeDataFn = &set_platform_ime_data;
        platform_io.Platform_ImeUserData = &context;
#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS
        io.GetClipboardTextFn = &get_clipboard_text;
        io.SetClipboardTextFn = &set_clipboard_text;
        io.ClipboardUserData = &context;
#endif
    }
    if (context.viewports_enabled) {
        io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports;
        platform_io.Platform_CreateWindow = &create_platform_window;
        platform_io.Platform_DestroyWindow = &destroy_platform_window;
        platform_io.Platform_ShowWindow = &show_platform_window;
        platform_io.Platform_SetWindowPos = &set_platform_window_position;
        platform_io.Platform_GetWindowPos = &get_platform_window_position;
        platform_io.Platform_SetWindowSize = &set_platform_window_size;
        platform_io.Platform_GetWindowSize = &get_platform_window_size;
        platform_io.Platform_GetWindowFramebufferScale = &get_platform_window_framebuffer_scale;
        platform_io.Platform_SetWindowFocus = &set_platform_window_focus;
        platform_io.Platform_GetWindowFocus = &get_platform_window_focus;
        platform_io.Platform_GetWindowMinimized = &get_platform_window_minimized;
        platform_io.Platform_SetWindowTitle = &set_platform_window_title;
    }
}

void ImGuiPlatformBridge::shutdown(ImGuiIO& io, ImGuiPlatformBridgeContext& context) noexcept {
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (ImGui::GetMainViewport() != nullptr &&
        ImGui::GetMainViewport()->PlatformUserData == &context.main_viewport) {
        ImGui::GetMainViewport()->PlatformUserData = nullptr;
        ImGui::GetMainViewport()->PlatformHandle = nullptr;
    }
    if (platform_io.Platform_ClipboardUserData == &context) {
        platform_io.Platform_GetClipboardTextFn = nullptr;
        platform_io.Platform_SetClipboardTextFn = nullptr;
        platform_io.Platform_ClipboardUserData = nullptr;
    }
    if (platform_io.Platform_ImeUserData == &context) {
        platform_io.Platform_SetImeDataFn = nullptr;
        platform_io.Platform_ImeUserData = nullptr;
    }
    platform_io.Platform_CreateWindow = nullptr;
    platform_io.Platform_DestroyWindow = nullptr;
    platform_io.Platform_ShowWindow = nullptr;
    platform_io.Platform_SetWindowPos = nullptr;
    platform_io.Platform_GetWindowPos = nullptr;
    platform_io.Platform_SetWindowSize = nullptr;
    platform_io.Platform_GetWindowSize = nullptr;
    platform_io.Platform_GetWindowFramebufferScale = nullptr;
    platform_io.Platform_SetWindowFocus = nullptr;
    platform_io.Platform_GetWindowFocus = nullptr;
    platform_io.Platform_GetWindowMinimized = nullptr;
    platform_io.Platform_SetWindowTitle = nullptr;
    if (io.BackendPlatformUserData == &context) {
        io.BackendPlatformUserData = nullptr;
    }
    io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags &= ~(ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports);
#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    if (io.ClipboardUserData == &context) {
        io.GetClipboardTextFn = nullptr;
        io.SetClipboardTextFn = nullptr;
        io.ClipboardUserData = nullptr;
    }
#endif
    context.services = nullptr;
    context.window_system = nullptr;
    context.window = {};
    context.main_viewport = {};
    context.viewports_enabled = false;
    context.clipboard_text.clear();
}

void ImGuiPlatformBridge::set_window(ImGuiPlatformBridgeContext& context, platform::WindowId window) noexcept {
    context.window = window;
    context.main_viewport.window_system = context.window_system;
    context.main_viewport.services = context.services;
    context.main_viewport.window = window;
    context.main_viewport.owns_window = false;
    context.main_viewport.window_object.reset();
    if (context.window_system != nullptr && window.is_valid()) {
        try {
            const foundation::ResultValue<std::shared_ptr<platform::IWindow>> window_result =
                context.window_system->find_window(window);
            if (window_result) {
                context.main_viewport.window_object = window_result.value;
            }
        } catch (...) {}
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        if (main_viewport != nullptr) {
            main_viewport->PlatformUserData = &context.main_viewport;
            main_viewport->PlatformHandle = &context.main_viewport;
        }
        if (context.viewports_enabled) {
            update_monitors(context);
        }
    }
}

void ImGuiPlatformBridge::update_cursor(ImGuiIO& io, const ImGuiPlatformBridgeContext& context) noexcept {
    if (context.services == nullptr || !context.window.is_valid() ||
        (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0) {
        return;
    }
    const platform::CursorShape shape =
        io.MouseDrawCursor ? platform::CursorShape::kNone : translate_cursor(ImGui::GetMouseCursor());
    try {
        static_cast<void>(context.services->set_cursor(context.window, shape));
    } catch (...) {}
}

void ImGuiPlatformBridge::submit_event(ImGuiIO& io, const platform::WindowEvent& event,
                                       const ImGuiPlatformBridgeContext& context) noexcept {
    using platform::WindowEventType;
    switch (event.type) {
        case WindowEventType::kWindowResized:
        case WindowEventType::kWindowCloseRequested:
            return;
        case WindowEventType::kWindowFocusChanged:
            io.AddFocusEvent(event.focused);
            if (!event.focused) {
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            }
            return;
        case WindowEventType::kKey: {
            submit_modifiers(io, event.modifiers);
            const ImGuiKey key = translate_key(event.key);
            if (key != ImGuiKey_None) {
                io.AddKeyEvent(key, event.key_action != platform::KeyAction::kReleased);
            }
            return;
        }
        case WindowEventType::kTextInput:
            if (event.codepoint != 0) {
                io.AddInputCharacter(static_cast<unsigned int>(event.codepoint));
            }
            return;
        case WindowEventType::kMouseMoved:
            if (!context.viewports_enabled || context.window_system == nullptr) {
                io.AddMousePosEvent(event.mouse_x, event.mouse_y);
                return;
            }
            try {
                const foundation::ResultValue<std::shared_ptr<platform::IWindow>> window_result =
                    context.window_system->find_window(event.window);
                if (!window_result) {
                    return;
                }
                const platform::WindowPosition position = window_result.value->state().position;
                io.AddMousePosEvent(event.mouse_x + static_cast<float>(position.x),
                                    event.mouse_y + static_cast<float>(position.y));
            } catch (...) {}
            return;
        case WindowEventType::kMouseButton:
            submit_modifiers(io, event.modifiers);
            io.AddMouseButtonEvent(static_cast<int>(event.mouse_button), event.mouse_button_down);
            return;
        case WindowEventType::kMouseWheel:
            io.AddMouseWheelEvent(event.wheel_x, event.wheel_y);
            return;
    }
}
}  // namespace aeg::editor::detail
