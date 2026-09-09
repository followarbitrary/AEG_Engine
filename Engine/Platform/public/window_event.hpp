#pragma once

#include <cstdint>

#include "window_types.hpp"

namespace aeg::platform {
enum class WindowEventType : uint8_t {
    kWindowResized,
    kWindowFocusChanged,
    kWindowCloseRequested,
    kKey,
    kTextInput,
    kMouseMoved,
    kMouseButton,
    kMouseWheel,
};

enum class KeyCode : uint16_t {
    kUnknown,
    kTab,
    kLeft,
    kRight,
    kUp,
    kDown,
    kPageUp,
    kPageDown,
    kHome,
    kEnd,
    kInsert,
    kDelete,
    kBackspace,
    kSpace,
    kEnter,
    kEscape,
    kA,
    kB,
    kC,
    kD,
    kE,
    kF,
    kG,
    kH,
    kI,
    kJ,
    kK,
    kL,
    kM,
    kN,
    kO,
    kP,
    kQ,
    kR,
    kS,
    kT,
    kU,
    kV,
    kW,
    kX,
    kY,
    kZ,
    kF1,
    kF2,
    kF3,
    kF4,
    kF5,
    kF6,
    kF7,
    kF8,
    kF9,
    kF10,
    kF11,
    kF12,
    kLeftCtrl,
    kLeftShift,
    kLeftAlt,
    kLeftSuper,
    kRightCtrl,
    kRightShift,
    kRightAlt,
    kRightSuper,
    kCount,
};

enum class KeyAction : uint8_t { kReleased, kPressed, kRepeated };
enum class MouseButton : uint8_t { kLeft, kRight, kMiddle, kX1, kX2, kCount };

enum class KeyModifiers : uint8_t {
    kNone = 0,
    kCtrl = 1u << 0,
    kShift = 1u << 1,
    kAlt = 1u << 2,
    kSuper = 1u << 3,
};

[[nodiscard]] constexpr KeyModifiers operator|(KeyModifiers left, KeyModifiers right) noexcept {
    return static_cast<KeyModifiers>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

[[nodiscard]] constexpr bool has_modifier(KeyModifiers value, KeyModifiers modifier) noexcept {
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(modifier)) != 0;
}

struct WindowEvent final {
    WindowEventType type{WindowEventType::kWindowResized};
    WindowId window{};
    uint64_t sequence{0};
    double timestamp_seconds{0.0};

    WindowExtent extent{};
    bool focused{false};
    KeyCode key{KeyCode::kUnknown};
    KeyAction key_action{KeyAction::kReleased};
    KeyModifiers modifiers{KeyModifiers::kNone};
    char32_t codepoint{0};
    float mouse_x{0.0f};
    float mouse_y{0.0f};
    MouseButton mouse_button{MouseButton::kLeft};
    bool mouse_button_down{false};
    float wheel_x{0.0f};
    float wheel_y{0.0f};
};
}  // namespace aeg::platform
