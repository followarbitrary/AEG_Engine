#pragma once

#include "window_event.hpp"

struct ImGuiIO;

namespace aeg::editor::detail {
class ImGuiPlatformBridge final {
public:
    static void submit_event(ImGuiIO& io, const platform::WindowEvent& event) noexcept;
};
}  // namespace aeg::editor::detail
