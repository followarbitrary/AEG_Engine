/**
 * @file win32_window.cpp
 * @brief Win32 message dispatch and semantic event conversion.
 */
#include "win32_window.hpp"

#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "win32_window_system.hpp"

namespace aeg::application::win32 {
namespace {
using aeg::platform::KeyAction;
using aeg::platform::KeyCode;
using aeg::platform::KeyModifiers;
using aeg::platform::MouseButton;
using aeg::platform::WindowEvent;
using aeg::platform::WindowEventType;

constexpr bool is_high_surrogate(wchar_t value) noexcept {
    return value >= 0xd800 && value <= 0xdbff;
}

constexpr bool is_low_surrogate(wchar_t value) noexcept {
    return value >= 0xdc00 && value <= 0xdfff;
}

constexpr char32_t combine_surrogates(wchar_t high, wchar_t low) noexcept {
    return 0x10000u + ((static_cast<char32_t>(high) - 0xd800u) << 10u) + (static_cast<char32_t>(low) - 0xdc00u);
}

KeyCode translate_extended_key(UINT virtual_key, LPARAM l_param) noexcept {
    const UINT scan_code = (static_cast<uint64_t>(l_param) >> 16u) & 0xffu;
    const bool extended = (static_cast<uint64_t>(l_param) & (1ull << 24u)) != 0;
    switch (virtual_key) {
        case VK_SHIFT:
            return scan_code == MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC) ? KeyCode::kRightShift : KeyCode::kLeftShift;
        case VK_CONTROL:
            return extended ? KeyCode::kRightCtrl : KeyCode::kLeftCtrl;
        case VK_MENU:
            return extended ? KeyCode::kRightAlt : KeyCode::kLeftAlt;
        case VK_LWIN:
            return KeyCode::kLeftSuper;
        case VK_RWIN:
            return KeyCode::kRightSuper;
        default:
            return KeyCode::kUnknown;
    }
}

KeyCode translate_virtual_key(WPARAM w_param, LPARAM l_param) noexcept {
    const UINT virtual_key = static_cast<UINT>(w_param);
    if (virtual_key == VK_SHIFT || virtual_key == VK_CONTROL || virtual_key == VK_MENU || virtual_key == VK_LWIN ||
        virtual_key == VK_RWIN) {
        return translate_extended_key(virtual_key, l_param);
    }
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        return static_cast<KeyCode>(static_cast<uint16_t>(KeyCode::kA) + (virtual_key - 'A'));
    }
    if (virtual_key >= VK_F1 && virtual_key <= VK_F12) {
        return static_cast<KeyCode>(static_cast<uint16_t>(KeyCode::kF1) + (virtual_key - VK_F1));
    }
    switch (virtual_key) {
        case VK_TAB:
            return KeyCode::kTab;
        case VK_LEFT:
            return KeyCode::kLeft;
        case VK_RIGHT:
            return KeyCode::kRight;
        case VK_UP:
            return KeyCode::kUp;
        case VK_DOWN:
            return KeyCode::kDown;
        case VK_PRIOR:
            return KeyCode::kPageUp;
        case VK_NEXT:
            return KeyCode::kPageDown;
        case VK_HOME:
            return KeyCode::kHome;
        case VK_END:
            return KeyCode::kEnd;
        case VK_INSERT:
            return KeyCode::kInsert;
        case VK_DELETE:
            return KeyCode::kDelete;
        case VK_BACK:
            return KeyCode::kBackspace;
        case VK_SPACE:
            return KeyCode::kSpace;
        case VK_RETURN:
            return KeyCode::kEnter;
        case VK_ESCAPE:
            return KeyCode::kEscape;
        default:
            return KeyCode::kUnknown;
    }
}

KeyModifiers modifiers_from_key_state(WPARAM w_param) noexcept {
    KeyModifiers modifiers = KeyModifiers::kNone;
    const auto is_down = [w_param](int virtual_key, WPARAM message_mask) noexcept {
        return (GetKeyState(virtual_key) & 0x8000) != 0 || (message_mask != 0 && (w_param & message_mask) != 0);
    };
    if (is_down(VK_CONTROL, MK_CONTROL)) {
        modifiers = modifiers | KeyModifiers::kCtrl;
    }
    if (is_down(VK_SHIFT, MK_SHIFT)) {
        modifiers = modifiers | KeyModifiers::kShift;
    }
    if (is_down(VK_MENU, 0)) {
        modifiers = modifiers | KeyModifiers::kAlt;
    }
    if (is_down(VK_LWIN, 0) || is_down(VK_RWIN, 0)) {
        modifiers = modifiers | KeyModifiers::kSuper;
    }
    return modifiers;
}

MouseButton mouse_button_from_message(UINT message, WPARAM w_param) noexcept {
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return MouseButton::kLeft;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return MouseButton::kRight;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return MouseButton::kMiddle;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            return GET_XBUTTON_WPARAM(w_param) == XBUTTON1 ? MouseButton::kX1 : MouseButton::kX2;
        default:
            return MouseButton::kLeft;
    }
}
}  // namespace

Win32Window::Win32Window(platform::WindowId id, Win32WindowSystem* owner, platform::WindowDesc desc) noexcept
    : id_(id), owner_(owner), desc_(std::move(desc)), state_{desc_.position, desc_.extent, desc_.visible, false,
                                                             false,          false,        true,          false} {}

Win32Window::~Win32Window() {
    destroy_native();
}

platform::WindowSurfaceBinding Win32Window::surface_binding() const noexcept {
    return platform::WindowSurfaceBinding{
        owner_ != nullptr ? reinterpret_cast<void*>(owner_->instance()) : nullptr,
        reinterpret_cast<void*>(hwnd_),
    };
}

foundation::Result Win32Window::set_position(platform::WindowPosition position) noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    if (SetWindowPos(hwnd_, nullptr, position.x, position.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ==
        FALSE) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "Win32 window position update failed");
    }
    state_.position = position;
    return foundation::Result::success();
}

foundation::Result Win32Window::set_extent(platform::WindowExtent extent) noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    if (!extent.is_valid() || extent.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        extent.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return foundation::invalid_argument("Win32 window extent is outside native limits");
    }

    RECT window_rect{};
    RECT client_rect{};
    if (GetWindowRect(hwnd_, &window_rect) == FALSE || GetClientRect(hwnd_, &client_rect) == FALSE) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError, "Win32 window extent query failed");
    }
    const int64_t frame_width = (static_cast<int64_t>(window_rect.right) - window_rect.left) -
                                (static_cast<int64_t>(client_rect.right) - client_rect.left);
    const int64_t frame_height = (static_cast<int64_t>(window_rect.bottom) - window_rect.top) -
                                 (static_cast<int64_t>(client_rect.bottom) - client_rect.top);
    const int64_t width = static_cast<int64_t>(extent.width) + frame_width;
    const int64_t height = static_cast<int64_t>(extent.height) + frame_height;
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int32_t>::max() ||
        height > std::numeric_limits<int32_t>::max()) {
        return foundation::invalid_argument("Win32 window extent exceeds native limits");
    }
    if (SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) == FALSE) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError, "Win32 window extent update failed");
    }
    sync_state_from_native();
    return foundation::Result::success();
}

foundation::Result Win32Window::set_visible(bool visible) noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    state_.visible = visible;
    if (visible) {
        UpdateWindow(hwnd_);
    }
    return foundation::Result::success();
}

foundation::Result Win32Window::focus() noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }

    const DWORD current_thread = GetCurrentThreadId();
    const HWND foreground_window = GetForegroundWindow();
    const DWORD foreground_thread =
        foreground_window == nullptr ? 0 : GetWindowThreadProcessId(foreground_window, nullptr);
    const bool attached_to_foreground = foreground_thread != 0 && foreground_thread != current_thread &&
                                        AttachThreadInput(foreground_thread, current_thread, TRUE) != FALSE;

    SetActiveWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);

    if (attached_to_foreground) {
        AttachThreadInput(foreground_thread, current_thread, FALSE);
    }

    state_.focused = GetForegroundWindow() == hwnd_ || GetActiveWindow() == hwnd_ || GetFocus() == hwnd_;
    if (!state_.focused) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError, "Win32 window focus update failed");
    }
    return foundation::Result::success();
}

foundation::Result Win32Window::set_maximized(bool maximized) noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    if (!desc_.resizable) {
        return foundation::unsupported("Win32 window is not resizable");
    }
    ShowWindow(hwnd_, maximized ? SW_MAXIMIZE : SW_RESTORE);
    sync_state_from_native();
    if (state_.maximized != maximized) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "Win32 window maximization update failed");
    }
    return foundation::Result::success();
}

foundation::Result Win32Window::set_minimized(bool minimized) noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    ShowWindow(hwnd_, minimized ? SW_MINIMIZE : SW_RESTORE);
    sync_state_from_native();
    if (state_.minimized != minimized) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "Win32 window minimization update failed");
    }
    return foundation::Result::success();
}

foundation::Result Win32Window::set_input_enabled(bool enabled) noexcept {
    if (hwnd_ == nullptr || IsWindow(hwnd_) == FALSE) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    EnableWindow(hwnd_, enabled ? TRUE : FALSE);
    state_.input_enabled = enabled;
    if (!enabled) {
        state_.focused = false;
    }
    return foundation::Result::success();
}

foundation::Result Win32Window::request_close() noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    if (PostMessageW(hwnd_, WM_CLOSE, 0, 0) == FALSE) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError);
    }
    return foundation::Result::success();
}

foundation::Result Win32Window::clear_close_request() noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    state_.close_requested = false;
    return foundation::Result::success();
}

void Win32Window::destroy_native() noexcept {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    state_.visible = false;
    state_.focused = false;
}

void Win32Window::sync_state_from_native() noexcept {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT window_rect{};
    RECT client_rect{};
    if (GetWindowRect(hwnd_, &window_rect) != FALSE) {
        state_.position = platform::WindowPosition{window_rect.left, window_rect.top};
    }
    if (GetClientRect(hwnd_, &client_rect) != FALSE) {
        const LONG width = std::max<LONG>(client_rect.right - client_rect.left, 0);
        const LONG height = std::max<LONG>(client_rect.bottom - client_rect.top, 0);
        state_.extent = platform::WindowExtent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }
    state_.visible = IsWindowVisible(hwnd_) != FALSE;
    state_.maximized = IsZoomed(hwnd_) != FALSE;
    state_.input_enabled = IsWindowEnabled(hwnd_) != FALSE;
}

foundation::Result Win32Window::set_cursor(platform::CursorShape shape) noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    if (shape >= platform::CursorShape::kCount) {
        return foundation::invalid_argument("Win32 cursor shape is invalid");
    }
    cursor_shape_ = shape;
    SetCursor(cursor_handle(cursor_shape_));
    return foundation::Result::success();
}

foundation::Result Win32Window::set_ime_state(const platform::ImeState& state) noexcept {
    if (hwnd_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    }
    ime_state_ = state;

    HIMC context = ImmGetContext(hwnd_);
    if (context == nullptr) {
        return foundation::Result::success();
    }

    ImmSetOpenStatus(context, state.want_visible ? TRUE : FALSE);
    if (state.want_visible || state.want_text_input) {
        const POINT position{static_cast<LONG>(std::lround(state.input_x)),
                             static_cast<LONG>(std::lround(state.input_y))};

        COMPOSITIONFORM composition{};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = position;
        ImmSetCompositionWindow(context, &composition);

        CANDIDATEFORM candidate{};
        candidate.dwIndex = 0;
        candidate.dwStyle = CFS_CANDIDATEPOS;
        candidate.ptCurrentPos = position;
        ImmSetCandidateWindow(context, &candidate);
    }
    ImmReleaseContext(hwnd_, context);
    return foundation::Result::success();
}

LRESULT CALLBACK Win32Window::window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) noexcept {
    auto* instance = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        instance = static_cast<Win32Window*>(create->lpCreateParams);
        if (instance != nullptr) {
            instance->attach(window);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
        }
    }
    if (instance == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    const LRESULT result = instance->handle_message(message, w_param, l_param);
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return result;
}

LRESULT Win32Window::handle_message(UINT message, WPARAM w_param, LPARAM l_param) noexcept {
    switch (message) {
        case WM_NCCALCSIZE:
            if (!desc_.client_decorated) {
                // Preserve the full client area for editor-owned chrome while
                // retaining WS_THICKFRAME for native resize handling.
                return 0;
            }
            return DefWindowProcW(hwnd_, message, w_param, l_param);
        case WM_GETMINMAXINFO:
            if (!desc_.client_decorated && l_param != 0) {
                // Borderless editor windows still use WS_THICKFRAME for
                // resizing, but their maximized bounds must follow the
                // monitor work area rather than the full monitor rectangle.
                // This preserves the Windows taskbar and avoids the default
                // frame compensation extending the client area past it.
                const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
                MONITORINFO monitor_info{};
                monitor_info.cbSize = sizeof(monitor_info);
                if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                    auto* limits = reinterpret_cast<MINMAXINFO*>(l_param);
                    const RECT& work = monitor_info.rcWork;
                    limits->ptMaxPosition = POINT{work.left, work.top};
                    limits->ptMaxSize = POINT{work.right - work.left, work.bottom - work.top};
                    return 0;
                }
            }
            return DefWindowProcW(hwnd_, message, w_param, l_param);
        case WM_NCHITTEST:
            if (!desc_.client_decorated) {
                POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                ScreenToClient(hwnd_, &point);
                RECT client{};
                GetClientRect(hwnd_, &client);
                constexpr LONG resize_border = 7;
                constexpr LONG title_height = 32;
                // Inline Aeg Chrome owns the close control in the upper-right
                // client area. Keep that rectangle out of the native resize
                // hit-test so its ImGui button receives the complete click
                // region, including the pixels adjacent to the top/right edge.
                const LONG close_left = client.right - 36;
                const LONG close_right = client.right;
                const LONG close_top = 0;
                const LONG close_bottom = title_height;
                if (point.x >= close_left && point.x < close_right && point.y >= close_top && point.y < close_bottom) {
                    return HTCLIENT;
                }
                // The complete three-button cluster is client-owned. Keep it
                // out of the top/right resize strips so every control remains
                // clickable when it touches the native border.
                const LONG controls_left = client.right - 108;
                if (point.x >= controls_left && point.x < client.right && point.y >= 0 && point.y < title_height) {
                    return HTCLIENT;
                }
                // A maximized editor window has no resize affordance at all.
                // Return a client hit before evaluating the border strips so
                // the native frame cannot restore-and-resize it from an edge.
                if (state_.maximized) {
                    return HTCLIENT;
                }
                // The workspace selector, status, and client controls are real
                // ImGui controls, not a draggable caption region. Reserve the
                // right-side control cluster even for compact native panels.
                const LONG draggable_title_width = std::max<LONG>(0, client.right - 108);
                if (desc_.resizable) {
                    const bool left = point.x < resize_border;
                    const bool right = point.x >= client.right - resize_border;
                    const bool top = point.y < resize_border;
                    const bool bottom = point.y >= client.bottom - resize_border;
                    if (top && left)
                        return HTTOPLEFT;
                    if (top && right)
                        return HTTOPRIGHT;
                    if (bottom && left)
                        return HTBOTTOMLEFT;
                    if (bottom && right)
                        return HTBOTTOMRIGHT;
                    if (left)
                        return HTLEFT;
                    if (right)
                        return HTRIGHT;
                    if (top)
                        return HTTOP;
                    if (bottom)
                        return HTBOTTOM;
                }
                // A maximized borderless window must remain stationary. Keep
                // the title area client-owned so a drag cannot invoke the
                // operating system's restore-and-move behavior. Double-click
                // is handled below and still toggles the maximized state.
                if (point.y >= 0 && point.y < title_height && point.x < draggable_title_width)
                    return HTCAPTION;
                return HTCLIENT;
            }
            return DefWindowProcW(hwnd_, message, w_param, l_param);
        case WM_NCLBUTTONDBLCLK:
            if (!desc_.client_decorated && !state_.minimized && static_cast<UINT>(w_param) == HTCAPTION) {
                // The window owns its title bar instead of using the native
                // frame, so toggle explicitly on a caption double-click.
                static_cast<void>(set_maximized(!state_.maximized));
                return 0;
            }
            return DefWindowProcW(hwnd_, message, w_param, l_param);
        case WM_LBUTTONDBLCLK:
            if (!desc_.client_decorated && !state_.minimized) {
                POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                constexpr LONG title_height = 32;
                constexpr LONG control_reserve = 108;
                RECT client{};
                GetClientRect(hwnd_, &client);
                if (point.y >= 0 && point.y < title_height && point.x >= 0 &&
                    point.x < client.right - control_reserve) {
                    static_cast<void>(set_maximized(!state_.maximized));
                    return 0;
                }
            }
            return DefWindowProcW(hwnd_, message, w_param, l_param);
        case WM_SETCURSOR:
            if (!desc_.client_decorated) {
                switch (LOWORD(l_param)) {
                    case HTTOP:
                    case HTBOTTOM:
                        SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                        return TRUE;
                    case HTLEFT:
                    case HTRIGHT:
                        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                        return TRUE;
                    case HTTOPLEFT:
                    case HTBOTTOMRIGHT:
                        SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                        return TRUE;
                    case HTTOPRIGHT:
                    case HTBOTTOMLEFT:
                        SetCursor(LoadCursorW(nullptr, IDC_SIZENESW));
                        return TRUE;
                    case HTCAPTION:
                        SetCursor(cursor_handle(cursor_shape_));
                        return TRUE;
                    default:
                        break;
                }
            }
            if (LOWORD(l_param) == HTCLIENT) {
                SetCursor(cursor_handle(cursor_shape_));
                return TRUE;
            }
            return DefWindowProcW(hwnd_, message, w_param, l_param);
        case WM_CLOSE:
            state_.close_requested = true;
            emit_event(WindowEvent{WindowEventType::kWindowCloseRequested});
            return 0;
        case WM_DESTROY:
            state_.close_requested = true;
            state_.visible = false;
            return 0;
        case WM_NCDESTROY: {
            const HWND current_window = hwnd_;
            hwnd_ = nullptr;
            return DefWindowProcW(current_window, message, w_param, l_param);
        }
        case WM_SIZE: {
            const uint32_t width = static_cast<uint32_t>(LOWORD(l_param));
            const uint32_t height = static_cast<uint32_t>(HIWORD(l_param));
            state_.minimized = w_param == SIZE_MINIMIZED;
            state_.maximized = w_param == SIZE_MAXIMIZED;
            // A zero client extent suspends drawing until a restore/resize event.
            state_.extent = platform::WindowExtent{width, height};
            WindowEvent event{WindowEventType::kWindowResized};
            event.extent = state_.extent;
            emit_event(event);
            return 0;
        }
        case WM_MOVE:
            sync_state_from_native();
            return 0;
        case WM_SHOWWINDOW:
            state_.visible = w_param != FALSE;
            return 0;
        case WM_ENABLE:
            state_.input_enabled = w_param != FALSE;
            if (!state_.input_enabled) {
                state_.focused = false;
            }
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            state_.focused = message == WM_SETFOCUS;
            WindowEvent event{WindowEventType::kWindowFocusChanged};
            event.focused = state_.focused;
            emit_event(event);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            WindowEvent event{WindowEventType::kKey};
            event.key = translate_key(w_param, l_param);
            event.key_action =
                (static_cast<uint64_t>(l_param) & (1ull << 30u)) != 0 ? KeyAction::kRepeated : KeyAction::kPressed;
            event.modifiers = current_modifiers();
            emit_event(event);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            WindowEvent event{WindowEventType::kKey};
            event.key = translate_key(w_param, l_param);
            event.key_action = KeyAction::kReleased;
            event.modifiers = current_modifiers();
            emit_event(event);
            return 0;
        }
        case WM_CHAR:
        case WM_SYSCHAR: {
            const wchar_t code_unit = static_cast<wchar_t>(w_param);
            if (is_high_surrogate(code_unit)) {
                pending_high_surrogate_ = code_unit;
            } else if (is_low_surrogate(code_unit) && pending_high_surrogate_ != 0) {
                emit_text_codepoint(combine_surrogates(pending_high_surrogate_, code_unit));
                pending_high_surrogate_ = 0;
            } else if (!is_low_surrogate(code_unit)) {
                pending_high_surrogate_ = 0;
                emit_text_codepoint(static_cast<char32_t>(code_unit));
            }
            return 0;
        }
        case WM_UNICHAR:
            if (w_param == UNICODE_NOCHAR) {
                return TRUE;
            }
            emit_text_codepoint(static_cast<char32_t>(w_param));
            return 0;
        case WM_MOUSEMOVE: {
            WindowEvent event{WindowEventType::kMouseMoved};
            event.mouse_x = static_cast<float>(GET_X_LPARAM(l_param));
            event.mouse_y = static_cast<float>(GET_Y_LPARAM(l_param));
            emit_event(event);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            SetCapture(hwnd_);
            WindowEvent event{WindowEventType::kMouseButton};
            event.mouse_button = translate_mouse_button(message, w_param);
            event.mouse_button_down = true;
            event.modifiers = current_modifiers(w_param);
            emit_event(event);
            return message == WM_XBUTTONDOWN ? TRUE : 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            if (GetCapture() == hwnd_) {
                ReleaseCapture();
            }
            WindowEvent event{WindowEventType::kMouseButton};
            event.mouse_button = translate_mouse_button(message, w_param);
            event.mouse_button_down = false;
            event.modifiers = current_modifiers(w_param);
            emit_event(event);
            return message == WM_XBUTTONUP ? TRUE : 0;
        }
        case WM_MOUSEWHEEL: {
            WindowEvent event{WindowEventType::kMouseWheel};
            event.wheel_y = static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / static_cast<float>(WHEEL_DELTA);
            event.modifiers = current_modifiers(GET_KEYSTATE_WPARAM(w_param));
            emit_event(event);
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            WindowEvent event{WindowEventType::kMouseWheel};
            event.wheel_x = static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / static_cast<float>(WHEEL_DELTA);
            event.modifiers = current_modifiers(GET_KEYSTATE_WPARAM(w_param));
            emit_event(event);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(hwnd_, message, w_param, l_param);
    }
}

void Win32Window::emit_event(platform::WindowEvent event) noexcept {
    if (owner_ == nullptr) {
        return;
    }
    event.window = id_;
    event.timestamp_seconds = owner_->timestamp_seconds();
    event.sequence = owner_->next_sequence();
    owner_->enqueue_event(std::move(event));
}

void Win32Window::emit_text_codepoint(char32_t codepoint) noexcept {
    if (codepoint == 0) {
        return;
    }
    WindowEvent event{WindowEventType::kTextInput};
    event.codepoint = codepoint;
    emit_event(event);
}

platform::KeyCode Win32Window::translate_key(WPARAM w_param, LPARAM l_param) noexcept {
    return translate_virtual_key(w_param, l_param);
}

platform::KeyModifiers Win32Window::current_modifiers(WPARAM w_param) noexcept {
    return modifiers_from_key_state(w_param);
}

platform::MouseButton Win32Window::translate_mouse_button(UINT message, WPARAM w_param) noexcept {
    return mouse_button_from_message(message, w_param);
}

HCURSOR Win32Window::cursor_handle(platform::CursorShape shape) noexcept {
    LPCWSTR resource = nullptr;
    switch (shape) {
        case platform::CursorShape::kNone:
            return nullptr;
        case platform::CursorShape::kArrow:
            resource = IDC_ARROW;
            break;
        case platform::CursorShape::kTextInput:
            resource = IDC_IBEAM;
            break;
        case platform::CursorShape::kResizeAll:
            resource = IDC_SIZEALL;
            break;
        case platform::CursorShape::kResizeVertical:
            resource = IDC_SIZENS;
            break;
        case platform::CursorShape::kResizeHorizontal:
            resource = IDC_SIZEWE;
            break;
        case platform::CursorShape::kResizeDiagonalNortheastSouthwest:
            resource = IDC_SIZENESW;
            break;
        case platform::CursorShape::kResizeDiagonalNorthwestSoutheast:
            resource = IDC_SIZENWSE;
            break;
        case platform::CursorShape::kHand:
            resource = IDC_HAND;
            break;
        case platform::CursorShape::kWait:
            resource = IDC_WAIT;
            break;
        case platform::CursorShape::kProgress:
            resource = IDC_APPSTARTING;
            break;
        case platform::CursorShape::kNotAllowed:
            resource = IDC_NO;
            break;
        case platform::CursorShape::kCount:
            return nullptr;
    }
    return LoadCursorW(nullptr, resource);
}
}  // namespace aeg::application::win32
