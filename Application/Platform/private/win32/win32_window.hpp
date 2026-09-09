/**
 * @file win32_window.hpp
 * @brief Win32 implementation of the backend-neutral application window.
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "window.hpp"

namespace aeg::application::win32 {
class Win32WindowSystem;

class Win32Window final : public platform::IWindow {
public:
    Win32Window(platform::WindowId id, Win32WindowSystem* owner, platform::WindowDesc desc) noexcept;
    ~Win32Window() override;

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;
    Win32Window(Win32Window&&) = delete;
    Win32Window& operator=(Win32Window&&) = delete;

    [[nodiscard]] platform::WindowId id() const noexcept override { return id_; }
    [[nodiscard]] const platform::WindowDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] platform::WindowState state() const noexcept override { return state_; }
    [[nodiscard]] platform::WindowSurfaceBinding surface_binding() const noexcept override;
    [[nodiscard]] foundation::Result request_close() noexcept override;

    void attach(HWND handle) noexcept { hwnd_ = handle; }
    void detach_owner() noexcept { owner_ = nullptr; }
    void destroy_native() noexcept;

private:
    friend class Win32WindowSystem;

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) noexcept;

    [[nodiscard]] LRESULT handle_message(UINT message, WPARAM w_param, LPARAM l_param) noexcept;
    void emit_event(platform::WindowEvent event) noexcept;
    void emit_text_codepoint(char32_t codepoint) noexcept;

    [[nodiscard]] static platform::KeyCode translate_key(WPARAM w_param, LPARAM l_param) noexcept;
    [[nodiscard]] static platform::KeyModifiers current_modifiers(WPARAM w_param = 0) noexcept;
    [[nodiscard]] static platform::MouseButton translate_mouse_button(UINT message, WPARAM w_param) noexcept;

    platform::WindowId id_{};
    Win32WindowSystem* owner_{nullptr};
    HWND hwnd_{nullptr};
    platform::WindowDesc desc_{};
    platform::WindowState state_{};
    wchar_t pending_high_surrogate_{0};
};
}  // namespace aeg::application::win32
