#pragma once

#include <cstdint>
#include <span>

#include "input_types.hpp"
#include "window_event.hpp"

namespace aeg::application::runtime {

/** Borrowed data describing one runtime frame. Pointers and spans live only during the callback. */
struct RuntimeFrameContext final {
    uint64_t frame_number{0};
    uint64_t fixed_tick{0};
    float real_delta_seconds{0.0f};
    float delta_seconds{0.0f};
    float fixed_delta_seconds{1.0f / 60.0f};
    float interpolation_alpha{0.0f};
    uint32_t fixed_steps{0};
    bool paused{false};
    bool quit_requested{false};
    const input::InputSnapshot* input{nullptr};
    std::span<const platform::WindowEvent> events{};
};

struct FixedUpdateContext final {
    const RuntimeFrameContext* frame{nullptr};
    float delta_seconds{0.0f};
};

struct UpdateContext final {
    const RuntimeFrameContext* frame{nullptr};
    float delta_seconds{0.0f};
};
}  // namespace aeg::application::runtime
