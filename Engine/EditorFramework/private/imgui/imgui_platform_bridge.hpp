#pragma once

#include <string>

#include "platform_services.hpp"
#include "window_event.hpp"

struct ImGuiIO;

namespace aeg::editor::detail {
struct ImGuiPlatformBridgeContext final {
    platform::IPlatformServices* services{nullptr};
    platform::WindowId window{};
    std::string clipboard_text{};
};

class ImGuiPlatformBridge final {
public:
    static void install(ImGuiIO& io, ImGuiPlatformBridgeContext& context,
                        platform::IPlatformServices* services) noexcept;
    static void shutdown(ImGuiIO& io, ImGuiPlatformBridgeContext& context) noexcept;
    static void set_window(ImGuiPlatformBridgeContext& context, platform::WindowId window) noexcept;
    static void update_cursor(ImGuiIO& io, const ImGuiPlatformBridgeContext& context) noexcept;
    static void submit_event(ImGuiIO& io, const platform::WindowEvent& event) noexcept;
};
}  // namespace aeg::editor::detail
