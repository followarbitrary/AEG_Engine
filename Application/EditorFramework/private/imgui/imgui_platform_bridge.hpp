#pragma once

#include <memory>
#include <string>

#include "platform_services.hpp"
#include "window.hpp"
#include "window_event.hpp"

struct ImGuiIO;
struct ImGuiViewport;

namespace aeg::editor::detail {
/** @brief Semantic state retained by one engine-owned ImGui platform viewport. */
struct ImGuiPlatformViewportData final {
    /** @brief Borrowed display-metrics service used for framebuffer scale queries. */
    platform::IPlatformServices* services{nullptr};
    /** @brief Window service used to create and update the platform window. */
    platform::IWindowSystem* window_system{nullptr};
    /** @brief Generation-checked identity of the platform window. */
    platform::WindowId window{};
    /** @brief Strong reference that keeps the abstract window alive until destruction. */
    std::shared_ptr<platform::IWindow> window_object{};
    /** @brief True when this bridge entry owns the corresponding window-system slot. */
    bool owns_window{false};
};

/** @brief State shared by the ImGui platform callbacks and editor host. */
struct ImGuiPlatformBridgeContext final {
    /** @brief Borrowed clipboard, cursor, IME, and display-metrics service. */
    platform::IPlatformServices* services{nullptr};
    /** @brief Borrowed engine window service used for multi-viewport windows. */
    platform::IWindowSystem* window_system{nullptr};
    /** @brief Main editor window identity currently driving the frame. */
    platform::WindowId window{};
    /** @brief Main viewport bridge data required by ImGui's viewport contract. */
    ImGuiPlatformViewportData main_viewport{};
    /** @brief Whether absolute mouse coordinates and platform viewport callbacks are active. */
    bool viewports_enabled{false};
    /** @brief Temporary storage backing the clipboard callback result. */
    std::string clipboard_text{};
};

class ImGuiPlatformBridge final {
public:
    static void install(ImGuiIO& io, ImGuiPlatformBridgeContext& context,
                        platform::IPlatformServices* services, platform::IWindowSystem* window_system,
                        bool enable_viewports) noexcept;
    static void shutdown(ImGuiIO& io, ImGuiPlatformBridgeContext& context) noexcept;
    static void set_window(ImGuiPlatformBridgeContext& context, platform::WindowId window) noexcept;
    static void update_cursor(ImGuiIO& io, const ImGuiPlatformBridgeContext& context) noexcept;
    static void submit_event(ImGuiIO& io, const platform::WindowEvent& event,
                             const ImGuiPlatformBridgeContext& context) noexcept;
};
}  // namespace aeg::editor::detail
