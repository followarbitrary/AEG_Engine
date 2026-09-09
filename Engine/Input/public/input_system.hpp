#pragma once

#include <mutex>
#include <span>
#include <vector>

#include "input_types.hpp"
#include "result.hpp"

namespace aeg::input {
class InputSystem final {
public:
    InputSystem() = default;
    ~InputSystem() = default;

    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;
    InputSystem(InputSystem&&) = delete;
    InputSystem& operator=(InputSystem&&) = delete;

    [[nodiscard]] foundation::Result submit_event(const platform::WindowEvent& event);
    [[nodiscard]] foundation::Result begin_frame();
    void end_frame() noexcept;

    [[nodiscard]] const InputSnapshot& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] std::span<const platform::WindowEvent> frame_events() const noexcept { return frame_events_; }

private:
    void apply_event(const platform::WindowEvent& event) noexcept;

    mutable std::mutex mutex_{};
    std::vector<platform::WindowEvent> pending_events_{};
    std::vector<platform::WindowEvent> frame_events_{};
    InputSnapshot snapshot_{};
};
}  // namespace aeg::input
