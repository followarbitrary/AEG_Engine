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
    [[nodiscard]] foundation::Result set_position(platform::WindowPosition position) noexcept override;
    [[nodiscard]] foundation::Result set_extent(platform::WindowExtent extent) noexcept override;
    [[nodiscard]] foundation::Result set_visible(bool visible) noexcept override;
    [[nodiscard]] foundation::Result focus() noexcept override;
    [[nodiscard]] foundation::Result set_maximized(bool maximized) noexcept override;
    [[nodiscard]] foundation::Result set_input_enabled(bool enabled) noexcept override;
    [[nodiscard]] foundation::Result request_close() noexcept override;

    void attach(HWND handle) noexcept { hwnd_ = handle; }
    void detach_owner() noexcept { owner_ = nullptr; }
    void destroy_native() noexcept;
    [[nodiscard]] foundation::Result set_cursor(platform::CursorShape shape) noexcept;
    [[nodiscard]] foundation::Result set_ime_state(const platform::ImeState& state) noexcept;

private:
    friend class Win32WindowSystem;

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) noexcept;

    [[nodiscard]] LRESULT handle_message(UINT message, WPARAM w_param, LPARAM l_param) noexcept;
    void sync_state_from_native() noexcept;
    void emit_event(platform::WindowEvent event) noexcept;
    void emit_text_codepoint(char32_t codepoint) noexcept;

    [[nodiscard]] static platform::KeyCode translate_key(WPARAM w_param, LPARAM l_param) noexcept;
    [[nodiscard]] static platform::KeyModifiers current_modifiers(WPARAM w_param = 0) noexcept;
    [[nodiscard]] static platform::MouseButton translate_mouse_button(UINT message, WPARAM w_param) noexcept;
    [[nodiscard]] static HCURSOR cursor_handle(platform::CursorShape shape) noexcept;

    /** @brief Generation-checked identity exposed through the platform contract. */
    platform::WindowId id_{};
    /** @brief Non-owning owner used for event delivery and slot lifecycle. */
    Win32WindowSystem* owner_{nullptr};
    /** @brief Native window handle kept private to the Application adapter. */
    HWND hwnd_{nullptr};
    /** @brief Immutable semantic creation description. */
    platform::WindowDesc desc_{};
    /** @brief Cached semantic state synchronized from Win32 messages. */
    platform::WindowState state_{};
    /** @brief Cursor shape requested through the backend-neutral service. */
    platform::CursorShape cursor_shape_{platform::CursorShape::kArrow};
    /** @brief Last IME state requested by the editor bridge. */
    platform::ImeState ime_state_{};
    /** @brief Pending high UTF-16 surrogate awaiting a matching low surrogate. */
    wchar_t pending_high_surrogate_{0};
};
}  // namespace aeg::application::win32
