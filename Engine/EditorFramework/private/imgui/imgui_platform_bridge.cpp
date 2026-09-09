#include "imgui_platform_bridge.hpp"

#include <imgui.h>

#include <cfloat>

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
}  // namespace

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
