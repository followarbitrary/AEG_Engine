#include "input_system.hpp"

namespace aeg::input {
foundation::Result InputSystem::submit_event(const platform::WindowEvent& event) {
    if (!event.window.is_valid()) {
        return foundation::invalid_argument("Input event has an invalid window identity");
    }

    std::scoped_lock lock(mutex_);
    pending_events_.push_back(event);
    return foundation::Result::success();
}

foundation::Result InputSystem::begin_frame() {
    {
        std::scoped_lock lock(mutex_);
        frame_events_.clear();
        frame_events_.swap(pending_events_);
    }

    snapshot_.wheel_x = 0.0f;
    snapshot_.wheel_y = 0.0f;
    for (const platform::WindowEvent& event : frame_events_) {
        apply_event(event);
    }
    return foundation::Result::success();
}

void InputSystem::end_frame() noexcept {
    frame_events_.clear();
}

void InputSystem::apply_event(const platform::WindowEvent& event) noexcept {
    switch (event.type) {
        case platform::WindowEventType::kWindowResized:
        case platform::WindowEventType::kWindowCloseRequested:
        case platform::WindowEventType::kTextInput:
            break;
        case platform::WindowEventType::kWindowFocusChanged:
            snapshot_.focused = event.focused;
            break;
        case platform::WindowEventType::kKey: {
            const std::size_t index = static_cast<std::size_t>(event.key);
            if (index < snapshot_.keys_down.size()) {
                snapshot_.keys_down[index] = event.key_action != platform::KeyAction::kReleased;
            }
            snapshot_.modifiers = event.modifiers;
            break;
        }
        case platform::WindowEventType::kMouseMoved:
            snapshot_.mouse_x = event.mouse_x;
            snapshot_.mouse_y = event.mouse_y;
            break;
        case platform::WindowEventType::kMouseButton: {
            const std::size_t index = static_cast<std::size_t>(event.mouse_button);
            if (index < snapshot_.mouse_buttons_down.size()) {
                snapshot_.mouse_buttons_down[index] = event.mouse_button_down;
            }
            snapshot_.modifiers = event.modifiers;
            break;
        }
        case platform::WindowEventType::kMouseWheel:
            snapshot_.wheel_x += event.wheel_x;
            snapshot_.wheel_y += event.wheel_y;
            break;
    }
}
}  // namespace aeg::input
