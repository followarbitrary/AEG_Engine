/**
 * @file application_platform_win32_smoke.cpp
 * @brief Verifies the Application-owned Win32 window adapter.
 */
#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../Application/Platform/private/win32/win32_clipboard_text.hpp"
#include "application_platform.hpp"
#include "window_event.hpp"

int main() {
    // Validate clipboard storage using owned buffers; the desktop clipboard is never changed.
    const wchar_t terminated_text[]{L'o', L'k', L'\0', L'x'};
    const auto text_view = aeg::application::win32::clipboard_text_view(terminated_text);
    if (!text_view || text_view.value != L"ok") {
        return 42;
    }
    const auto unterminated_text =
        aeg::application::win32::clipboard_text_view(std::span<const wchar_t>{terminated_text, 2});
    const auto missing_text = aeg::application::win32::clipboard_text_view({});
    const wchar_t empty_text[]{L'\0'};
    const auto empty_view = aeg::application::win32::clipboard_text_view(empty_text);
    if (unterminated_text || unterminated_text.result.code != aeg::foundation::ResultCode::kInvalidArgument ||
        missing_text || !empty_view || !empty_view.value.empty()) {
        return 43;
    }

    constexpr WORD kEditorIconResourceId = 101;
    constexpr WORD kGroupIconResourceType = 14;
    if (FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kEditorIconResourceId),
                      MAKEINTRESOURCEW(kGroupIconResourceType)) == nullptr) {
        return 27;
    }

    auto system_result = aeg::application::create_window_system();
    if (!system_result) {
        return 1;
    }
    std::unique_ptr<aeg::platform::IWindowSystem> system = std::move(system_result.value);

    const auto console_interrupt_result = aeg::application::install_console_interrupt_controller();
    if (!console_interrupt_result || console_interrupt_result.value == nullptr ||
        console_interrupt_result.value->interrupted()) {
        return 33;
    }

    const aeg::foundation::Result invalid_dialog = aeg::application::show_error_dialog("", "invalid");
    if (invalid_dialog || invalid_dialog.code != aeg::foundation::ResultCode::kInvalidArgument) {
        return 20;
    }

    const auto invalid_splash = aeg::application::show_startup_splash({});
    if (invalid_splash || invalid_splash.result.code != aeg::foundation::ResultCode::kInvalidArgument) {
        return 29;
    }
    aeg::application::StartupSplashDesc missing_splash_desc{};
    missing_splash_desc.image_path = std::filesystem::path{__FILE__}.parent_path() / "missing-startup-logo.png";
    const auto missing_splash = aeg::application::show_startup_splash(missing_splash_desc);
    if (missing_splash || missing_splash.result.code != aeg::foundation::ResultCode::kNotFound) {
        return 30;
    }
    aeg::application::StartupSplashDesc splash_desc{};
    splash_desc.image_path = std::filesystem::path{__FILE__}.parent_path().parent_path() / "Assets" / "Branding" /
                             "aether-engine-graph-mark.png";
    splash_desc.extent = {320, 320};
    splash_desc.accessible_name = "AEG transparent startup splash smoke";
    auto splash_result = aeg::application::show_startup_splash(splash_desc);
    if (!splash_result || splash_result.value == nullptr) {
        return 31;
    }
    const HWND splash_window = FindWindowW(nullptr, L"AEG transparent startup splash smoke");
    if (splash_window == nullptr ||
        (static_cast<DWORD>(GetWindowLongPtrW(splash_window, GWL_EXSTYLE)) & WS_EX_LAYERED) == 0) {
        return 32;
    }
    splash_result.value->close();

    const auto invalid_directory_dialog = aeg::application::select_directory({}, "", "C:\\");
    if (invalid_directory_dialog ||
        invalid_directory_dialog.result.code != aeg::foundation::ResultCode::kInvalidArgument) {
        return 26;
    }

    const auto font_result = aeg::application::enumerate_system_fonts();
    if (!font_result || font_result.value.empty()) {
        return 18;
    }
    for (const aeg::application::SystemFontRecord& font : font_result.value) {
        if (font.family_name.empty() || font.file_path.empty()) {
            return 19;
        }
    }

    aeg::platform::WindowDesc desc{};
    desc.title = "AEG Application Platform Smoke";
    desc.extent = {320, 200};
    desc.visible = false;
    desc.position = {128, 192};
    desc.position_specified = true;
    // Editor windows can own their title bar inside the client area.
    desc.client_decorated = false;
    if (desc.client_decorated) {
        return 99;
    }
    const auto borderless_result = system->create_window(desc);
    if (!borderless_result) {
        return 100;
    }
    const auto borderless_window = borderless_result.value;
    const HWND borderless_native = static_cast<HWND>(borderless_window->surface_binding().window_handle);
    POINT borderless_client_origin{0, 0};
    if (ClientToScreen(borderless_native, &borderless_client_origin) == FALSE ||
        borderless_client_origin.x != desc.position.x || borderless_client_origin.y != desc.position.y ||
        borderless_window->state().position != desc.position) {
        return 105;
    }
    const LONG_PTR borderless_style = GetWindowLongPtrW(borderless_native, GWL_STYLE);
    if ((static_cast<DWORD>(borderless_style) & WS_CAPTION) != 0 ||
        (static_cast<DWORD>(borderless_style) & WS_THICKFRAME) == 0) {
        return 101;
    }
    RECT borderless_rect{};
    if (GetWindowRect(borderless_native, &borderless_rect) == FALSE) {
        return 102;
    }
    const LRESULT resize_hit =
        SendMessageW(borderless_native, WM_NCHITTEST, 0, MAKELPARAM(borderless_rect.left + 2, borderless_rect.top + 2));
    if (resize_hit != HTTOPLEFT) {
        return 103;
    }
    const LRESULT chrome_close_hit = SendMessageW(borderless_native, WM_NCHITTEST, 0,
                                                  MAKELPARAM(borderless_rect.right - 20, borderless_rect.top + 4));
    if (chrome_close_hit != HTCLIENT) {
        return 106;
    }
    const LRESULT chrome_title_hit = SendMessageW(borderless_native, WM_NCHITTEST, 0,
                                                  MAKELPARAM(borderless_rect.left + 120, borderless_rect.top + 16));
    if (chrome_title_hit != HTCAPTION) {
        return 107;
    }
    const LRESULT chrome_content_hit = SendMessageW(borderless_native, WM_NCHITTEST, 0,
                                                    MAKELPARAM(borderless_rect.left + 120, borderless_rect.top + 120));
    if (chrome_content_hit != HTCLIENT) {
        return 108;
    }
    if (!borderless_window->set_visible(true)) {
        return 109;
    }
    MONITORINFO borderless_monitor{};
    borderless_monitor.cbSize = sizeof(borderless_monitor);
    if (GetMonitorInfoW(MonitorFromWindow(borderless_native, MONITOR_DEFAULTTONEAREST), &borderless_monitor) == FALSE) {
        return 113;
    }
    // A title double-click toggles the custom borderless window just like a
    // native caption double-click. The client area itself must not do so.
    SendMessageW(borderless_native, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(120, 16));
    if (!borderless_window->state().maximized) {
        return 110;
    }
    const RECT maximized_rect = [&] {
        RECT value{};
        GetWindowRect(borderless_native, &value);
        return value;
    }();
    // Custom chrome exposes the full client rectangle. Maximization must fit
    // the work area exactly, without hiding the title or covering the taskbar.
    const RECT work_rect = borderless_monitor.rcWork;
    if (EqualRect(&maximized_rect, &work_rect) == FALSE) {
        std::fprintf(stderr, "Borderless maximize bounds: got (%ld, %ld, %ld, %ld), work area (%ld, %ld, %ld, %ld)\n",
                     maximized_rect.left, maximized_rect.top, maximized_rect.right, maximized_rect.bottom,
                     work_rect.left, work_rect.top, work_rect.right, work_rect.bottom);
        return 114;
    }
    POINT maximized_client_origin{};
    RECT maximized_client_rect{};
    if (ClientToScreen(borderless_native, &maximized_client_origin) == FALSE ||
        GetClientRect(borderless_native, &maximized_client_rect) == FALSE ||
        maximized_client_origin.x != work_rect.left || maximized_client_origin.y != work_rect.top ||
        maximized_client_rect.right != work_rect.right - work_rect.left ||
        maximized_client_rect.bottom != work_rect.bottom - work_rect.top) {
        return 115;
    }
    const LRESULT maximized_title_hit = SendMessageW(
        borderless_native, WM_NCHITTEST, 0,
        MAKELPARAM(maximized_rect.left + 120, maximized_rect.top + 16));
    if (maximized_title_hit != HTCLIENT) {
        return 111;
    }
    const std::array<POINT, 8> maximized_edge_points{
        POINT{maximized_rect.left + 1, maximized_rect.top + 120},
        POINT{maximized_rect.right - 2, maximized_rect.top + 120},
        POINT{maximized_rect.left + 120, maximized_rect.top + 1},
        POINT{maximized_rect.left + 120, maximized_rect.bottom - 2},
        POINT{maximized_rect.left + 1, maximized_rect.top + 1},
        POINT{maximized_rect.right - 2, maximized_rect.top + 1},
        POINT{maximized_rect.left + 1, maximized_rect.bottom - 2},
        POINT{maximized_rect.right - 2, maximized_rect.bottom - 2},
    };
    for (const POINT point : maximized_edge_points) {
        if (SendMessageW(borderless_native, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) != HTCLIENT) {
            return 117;
        }
    }
    SendMessageW(borderless_native, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(120, 16));
    if (borderless_window->state().maximized) {
        return 112;
    }
    RECT restored_rect{};
    if (GetWindowRect(borderless_native, &restored_rect) == FALSE ||
        EqualRect(&restored_rect, &borderless_rect) == FALSE) {
        return 116;
    }
    if (!system->destroy_window(borderless_window->id())) {
        return 104;
    }
    // Keep the remaining platform smoke on the system-decorated path so its
    // native-frame extent assertions retain their original contract.
    desc.client_decorated = true;
    const auto window_result = system->create_window(desc);
    if (!window_result) {
        return 2;
    }
    const std::shared_ptr<aeg::platform::IWindow> window = window_result.value;
    if (!window->id().is_valid() || !window->surface_binding().is_valid() || window->state().extent != desc.extent) {
        return 3;
    }
    const HICON expected_small_icon =
        static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kEditorIconResourceId), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    const auto native_window = static_cast<HWND>(window->surface_binding().window_handle);
    const HICON actual_small_icon = reinterpret_cast<HICON>(GetClassLongPtrW(native_window, GCLP_HICONSM));
    if (expected_small_icon == nullptr || actual_small_icon != expected_small_icon) {
        return 28;
    }
    const aeg::platform::WindowPosition position{96, 144};
    if (!window->set_position(position) || window->state().position != position) {
        return 13;
    }
    if (!window->set_extent(aeg::platform::WindowExtent{360, 240}) ||
        window->state().extent != aeg::platform::WindowExtent{360, 240}) {
        return 14;
    }
    if (!window->set_visible(true) || !window->state().visible) {
        return 15;
    }
    if (!window->focus() || !window->state().focused) {
        return 16;
    }
    // A minimized surface has no drawable extent. Preserve zero in both the
    // native state sync and resize event so Runtime can suspend swapchain work.
    if (!system->poll_events([](const aeg::platform::WindowEvent&) {})) {
        return 45;
    }
    if (!window->set_minimized(true)) {
        return 46;
    }
    bool minimized_resize_seen = false;
    aeg::platform::WindowExtent minimized_extent{};
    const auto minimized_events = system->poll_events([&](const aeg::platform::WindowEvent& event) {
        if (event.window == window->id() && event.type == aeg::platform::WindowEventType::kWindowResized) {
            minimized_resize_seen = true;
            minimized_extent = event.extent;
        }
    });
    if (!minimized_events || !window->state().minimized ||
        window->state().extent != aeg::platform::WindowExtent{0, 0} || !minimized_resize_seen ||
        minimized_extent != aeg::platform::WindowExtent{0, 0}) {
        std::fprintf(stderr, "minimized drawable extent must be zero: state=%ux%u event=%ux%u seen=%d\n",
            window->state().extent.width, window->state().extent.height,
            minimized_extent.width, minimized_extent.height, minimized_resize_seen);
        return 47;
    }
    if (!window->set_minimized(false) || window->state().minimized ||
        window->state().extent != aeg::platform::WindowExtent{360, 240}) {
        return 48;
    }
    bool restored_resize_seen = false;
    aeg::platform::WindowExtent restored_extent{};
    const auto restored_events = system->poll_events([&](const aeg::platform::WindowEvent& event) {
        if (event.window == window->id() && event.type == aeg::platform::WindowEventType::kWindowResized) {
            restored_resize_seen = true;
            restored_extent = event.extent;
        }
    });
    if (!restored_events || !restored_resize_seen || restored_extent != window->state().extent) {
        return 49;
    }
    if (!window->set_visible(false) || window->state().visible) {
        return 17;
    }
    aeg::platform::IPlatformServices& platform_services = system->platform_services();
    const auto metrics_result = platform_services.query_window_metrics(window->id());
    if (!metrics_result || !metrics_result.value.is_valid()) {
        return 10;
    }
    const auto work_area_result = platform_services.query_display_work_area(window->id());
    if (!work_area_result || !work_area_result.value.is_valid()) {
        return 21;
    }
    if (!window->set_input_enabled(false) || window->state().input_enabled) {
        return 22;
    }
    if (!window->set_input_enabled(true) || !window->state().input_enabled) {
        return 23;
    }
    if (!window->set_maximized(true) || !window->state().maximized) {
        return 24;
    }
    if (!window->set_maximized(false) || window->state().maximized) {
        return 25;
    }
    if (!platform_services.set_cursor(window->id(), aeg::platform::CursorShape::kHand) ||
        !platform_services.set_ime_state(window->id(), aeg::platform::ImeState{true, true, 10.0f, 12.0f, 16.0f})) {
        return 11;
    }
    const auto found_result = system->find_window(window->id());
    if (!found_result || found_result.value->id() != window->id()) {
        return 4;
    }

    // A failing event consumer must leave the message pump available for later input.
    for (const bool standard_exception : {true, false}) {
        if (!window->request_close()) {
            return 34;
        }
        aeg::foundation::Result callback_result{};
        try {
            callback_result = system->poll_events([standard_exception](const aeg::platform::WindowEvent& event) {
                if (event.type != aeg::platform::WindowEventType::kWindowCloseRequested) {
                    return;
                }
                if (standard_exception) {
                    throw std::runtime_error("Recoverable window event consumer failure");
                }
                throw 7;
            });
        } catch (...) {
            return 35;
        }
        if (callback_result || callback_result.code != aeg::foundation::ResultCode::kUnknown ||
            !window->clear_close_request()) {
            return 36;
        }
        if (PostMessageW(native_window, WM_CHAR, L'R', 0) == FALSE) {
            return 37;
        }
        bool recovery_input_seen = false;
        if (!system->poll_events([&recovery_input_seen](const aeg::platform::WindowEvent& event) {
                recovery_input_seen |=
                    event.type == aeg::platform::WindowEventType::kTextInput && event.codepoint == U'R';
            }) ||
            !recovery_input_seen) {
            return 38;
        }
    }

    const auto oversized_extent = window->set_extent({std::numeric_limits<uint32_t>::max(), 240});
    if (oversized_extent || oversized_extent.code != aeg::foundation::ResultCode::kInvalidArgument) {
        return 39;
    }
    const auto oversized_frame = window->set_extent({static_cast<uint32_t>(std::numeric_limits<int32_t>::max()), 240});
    auto oversized_desc = desc;
    oversized_desc.extent.width = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    const auto oversized_window = system->create_window(oversized_desc);
    if (oversized_frame || oversized_frame.code != aeg::foundation::ResultCode::kInvalidArgument || oversized_window ||
        oversized_window.result.code != aeg::foundation::ResultCode::kInvalidArgument) {
        return 44;
    }

    bool close_event_seen = false;
    if (!window->request_close()) {
        return 5;
    }
    if (!system->poll_events([&close_event_seen](const aeg::platform::WindowEvent& event) {
            if (event.type == aeg::platform::WindowEventType::kWindowCloseRequested) {
                close_event_seen = true;
            }
        })) {
        return 6;
    }
    if (!close_event_seen || !window->state().close_requested) {
        return 7;
    }
    if (!system->destroy_window(window->id())) {
        return 8;
    }
    const auto stale_result = system->find_window(window->id());
    if (stale_result || stale_result.result.code != aeg::foundation::ResultCode::kNotFound) {
        return 9;
    }
    const auto stale_metrics = platform_services.query_window_metrics(window->id());
    if (stale_metrics || stale_metrics.result.code != aeg::foundation::ResultCode::kNotFound) {
        return 12;
    }
    if (window->surface_binding().display_handle != nullptr || window->surface_binding().window_handle != nullptr) {
        return 40;
    }
    system.reset();
    if (window->surface_binding().display_handle != nullptr || window->surface_binding().window_handle != nullptr) {
        return 41;
    }
    return 0;
}
