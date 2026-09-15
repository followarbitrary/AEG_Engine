/**
 * @file application_platform_win32_smoke.cpp
 * @brief Verifies the Application-owned Win32 window adapter.
 */
#include <filesystem>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "application_platform.hpp"
#include "window_event.hpp"

int main() {
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
    return 0;
}
