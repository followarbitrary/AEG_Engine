#pragma once

#include <array>
#include <cstddef>

#include "window_event.hpp"

namespace aeg::input {
struct InputSnapshot final {
    std::array<bool, static_cast<std::size_t>(platform::KeyCode::kCount)> keys_down{};
    std::array<bool, static_cast<std::size_t>(platform::MouseButton::kCount)> mouse_buttons_down{};
    platform::KeyModifiers modifiers{platform::KeyModifiers::kNone};
    float mouse_x{0.0f};
    float mouse_y{0.0f};
    float wheel_x{0.0f};
    float wheel_y{0.0f};
    bool focused{false};

    [[nodiscard]] bool is_key_down(platform::KeyCode key) const noexcept {
        const std::size_t index = static_cast<std::size_t>(key);
        return index < keys_down.size() && keys_down[index];
    }

    [[nodiscard]] bool is_mouse_button_down(platform::MouseButton button) const noexcept {
        const std::size_t index = static_cast<std::size_t>(button);
        return index < mouse_buttons_down.size() && mouse_buttons_down[index];
    }
};
}  // namespace aeg::input
