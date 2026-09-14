/**
 * @file input_system_smoke.cpp
 * @brief Verifies input state reset at an editor world-mode boundary.
 */
#include <iostream>

#include "input_system.hpp"

namespace {
bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}  // namespace

int main() {
    aeg::input::InputSystem input;
    aeg::platform::WindowEvent key{};
    key.type = aeg::platform::WindowEventType::kKey;
    key.window = aeg::platform::WindowId{1, 1};
    key.key = aeg::platform::KeyCode::kW;
    key.key_action = aeg::platform::KeyAction::kPressed;
    if (!require(static_cast<bool>(input.submit_event(key)), "input smoke key submission failed") ||
        !require(static_cast<bool>(input.begin_frame()), "input smoke frame begin failed") ||
        !require(input.snapshot().is_key_down(aeg::platform::KeyCode::kW),
                 "input smoke pressed key was not observed")) {
        return 1;
    }
    input.reset();
    if (!require(!input.snapshot().is_key_down(aeg::platform::KeyCode::kW), "input smoke reset left a key held") ||
        !require(input.frame_events().empty(), "input smoke reset left frame events")) {
        return 2;
    }
    std::cout << "Input system smoke passed\n";
    return 0;
}
