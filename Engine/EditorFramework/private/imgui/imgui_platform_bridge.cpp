#include "imgui_platform_bridge.hpp"

#include <imgui.h>

#include <cfloat>
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
                                  platform::IPlatformServices* services) noexcept {
    context.services = services;
    context.window = {};
    context.clipboard_text.clear();
    io.BackendPlatformUserData = &context;

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (services == nullptr) {
        return;
    }

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

void ImGuiPlatformBridge::shutdown(ImGuiIO& io, ImGuiPlatformBridgeContext& context) noexcept {
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (platform_io.Platform_ClipboardUserData == &context) {
        platform_io.Platform_GetClipboardTextFn = nullptr;
        platform_io.Platform_SetClipboardTextFn = nullptr;
        platform_io.Platform_ClipboardUserData = nullptr;
    }
    if (platform_io.Platform_ImeUserData == &context) {
        platform_io.Platform_SetImeDataFn = nullptr;
        platform_io.Platform_ImeUserData = nullptr;
    }
    if (io.BackendPlatformUserData == &context) {
        io.BackendPlatformUserData = nullptr;
    }
    io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    if (io.ClipboardUserData == &context) {
        io.GetClipboardTextFn = nullptr;
        io.SetClipboardTextFn = nullptr;
        io.ClipboardUserData = nullptr;
    }
#endif
    context.services = nullptr;
    context.window = {};
    context.clipboard_text.clear();
}

void ImGuiPlatformBridge::set_window(ImGuiPlatformBridgeContext& context, platform::WindowId window) noexcept {
    context.window = window;
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

void ImGuiPlatformBridge::submit_event(ImGuiIO& io, const platform::WindowEvent& event) noexcept {
    using platform::WindowEventType;
    switch (event.type) {
        case WindowEventType::kWindowResized:
        case WindowEventType::kWindowCloseRequested:
            return;
        case WindowEventType::kWindowFocusChanged:
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
            io.AddMousePosEvent(event.mouse_x, event.mouse_y);
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
