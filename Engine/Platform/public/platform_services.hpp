/**
 * @file platform_services.hpp
 * @brief Backend-neutral application platform services.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "result.hpp"
#include "window_types.hpp"

namespace aeg::platform {
/** @brief Cursor shapes understood by the engine platform contract. */
enum class CursorShape : uint8_t {
    kNone,
    kArrow,
    kTextInput,
    kResizeAll,
    kResizeVertical,
    kResizeHorizontal,
    kResizeDiagonalNortheastSouthwest,
    kResizeDiagonalNorthwestSoutheast,
    kHand,
    kWait,
    kProgress,
    kNotAllowed,
    kCount,
};

/** @brief Window-relative text-input position requested by an IME. */
struct ImeState final {
    bool want_visible{false};
    bool want_text_input{false};
    float input_x{0.0f};
    float input_y{0.0f};
    float input_line_height{0.0f};
};

/** @brief Display metrics used to initialize an ImGui frame. */
struct WindowMetrics final {
    WindowExtent display_extent{};
    float framebuffer_scale_x{1.0f};
    float framebuffer_scale_y{1.0f};

    [[nodiscard]] bool is_valid() const noexcept {
        return display_extent.is_valid() && framebuffer_scale_x > 0.0f && framebuffer_scale_y > 0.0f;
    }
};

/** @brief OS services exposed to Engine without exposing native platform types. */
class IPlatformServices {
public:
    virtual ~IPlatformServices() = default;

    [[nodiscard]] virtual foundation::ResultValue<std::string> get_clipboard_text() const = 0;
    [[nodiscard]] virtual foundation::Result set_clipboard_text(std::string_view text) = 0;
    [[nodiscard]] virtual foundation::Result set_cursor(WindowId window, CursorShape shape) noexcept = 0;
    [[nodiscard]] virtual foundation::Result set_ime_state(WindowId window, const ImeState& state) noexcept = 0;
    [[nodiscard]] virtual foundation::ResultValue<WindowMetrics> query_window_metrics(WindowId window) const = 0;
    /** @brief Returns the usable work area of the display containing a window. */
    [[nodiscard]] virtual foundation::ResultValue<DisplayWorkArea> query_display_work_area(WindowId window) const {
        static_cast<void>(window);
        return {foundation::unsupported("Display work-area queries are not supported by this platform adapter"), {}};
    }
};
}  // namespace aeg::platform
