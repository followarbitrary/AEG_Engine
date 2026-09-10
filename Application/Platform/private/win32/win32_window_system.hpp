/**
 * @file win32_window_system.hpp
 * @brief Win32 window-system implementation owned by Application.
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "window.hpp"

namespace aeg::application::win32 {
class Win32Window;

class Win32WindowSystem final : public platform::IWindowSystem, public platform::IPlatformServices {
public:
    Win32WindowSystem();
    ~Win32WindowSystem() override;

    Win32WindowSystem(const Win32WindowSystem&) = delete;
    Win32WindowSystem& operator=(const Win32WindowSystem&) = delete;
    Win32WindowSystem(Win32WindowSystem&&) = delete;
    Win32WindowSystem& operator=(Win32WindowSystem&&) = delete;

    [[nodiscard]] foundation::ResultValue<std::shared_ptr<platform::IWindow>> create_window(
        const platform::WindowDesc& desc) override;
    [[nodiscard]] foundation::Result destroy_window(platform::WindowId window) override;
    [[nodiscard]] foundation::Result poll_events(const platform::WindowEventSink& sink) override;
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<platform::IWindow>> find_window(
        platform::WindowId window) const override;
    [[nodiscard]] platform::IPlatformServices& platform_services() noexcept override { return *this; }

    [[nodiscard]] foundation::ResultValue<std::string> get_clipboard_text() const override;
    [[nodiscard]] foundation::Result set_clipboard_text(std::string_view text) override;
    [[nodiscard]] foundation::Result set_cursor(platform::WindowId window,
                                                platform::CursorShape shape) noexcept override;
    [[nodiscard]] foundation::Result set_ime_state(platform::WindowId window,
                                                   const platform::ImeState& state) noexcept override;
    [[nodiscard]] foundation::ResultValue<platform::WindowMetrics> query_window_metrics(
        platform::WindowId window) const override;

private:
    friend class Win32Window;

    struct WindowSlot final {
        uint32_t generation{1};
        std::shared_ptr<Win32Window> window{};
    };

    [[nodiscard]] foundation::Result register_window_class();
    [[nodiscard]] foundation::ResultValue<std::wstring> to_wide_title(const platform::WindowDesc& desc) const;
    [[nodiscard]] foundation::ResultValue<std::wstring> to_wide_string(std::string_view value) const;
    [[nodiscard]] std::size_t find_free_slot() const noexcept;
    [[nodiscard]] uint64_t next_sequence() noexcept;
    [[nodiscard]] double timestamp_seconds() const noexcept;
    [[nodiscard]] HINSTANCE instance() const noexcept { return instance_; }
    void enqueue_event(platform::WindowEvent event) noexcept;

    HINSTANCE instance_{nullptr};
    std::chrono::steady_clock::time_point start_time_{};
    std::vector<WindowSlot> windows_{};
    std::deque<platform::WindowEvent> pending_events_{};
    uint64_t next_sequence_{1};
    bool class_registered_{false};
    bool event_allocation_failed_{false};
};
}  // namespace aeg::application::win32
