/**
 * @file application_platform_win32_smoke.cpp
 * @brief Verifies the Application-owned Win32 window adapter.
 */
#include <memory>

#include "application_platform.hpp"
#include "window_event.hpp"

int main() {
    auto system_result = aeg::application::create_win32_window_system();
    if (!system_result) {
        return 1;
    }
    std::unique_ptr<aeg::platform::IWindowSystem> system = std::move(system_result.value);

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
    aeg::platform::IPlatformServices& platform_services = system->platform_services();
    const auto metrics_result = platform_services.query_window_metrics(window->id());
    if (!metrics_result || !metrics_result.value.is_valid()) {
        return 10;
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
