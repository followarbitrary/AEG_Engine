#pragma once

#include <functional>
#include <memory>

#include "platform_services.hpp"
#include "result.hpp"
#include "window_event.hpp"
#include "window_types.hpp"

namespace aeg::platform {
/** @brief Backend-neutral lifetime and state interface for one platform window. */
class IWindow {
public:
    virtual ~IWindow() = default;

    /** @brief Returns the generation-checked identity of this window. */
    [[nodiscard]] virtual WindowId id() const noexcept = 0;
    /** @brief Returns the immutable creation description. */
    [[nodiscard]] virtual const WindowDesc& desc() const noexcept = 0;
    /** @brief Returns the latest semantic window state snapshot. */
    [[nodiscard]] virtual WindowState state() const noexcept = 0;
    /** @brief Returns opaque handles for Application-owned RHI Surface creation. */
    [[nodiscard]] virtual WindowSurfaceBinding surface_binding() const noexcept = 0;
    /** @brief Moves the window client area to a screen-space position. */
    [[nodiscard]] virtual foundation::Result set_position(WindowPosition position) noexcept = 0;
    /** @brief Resizes the window client area. */
    [[nodiscard]] virtual foundation::Result set_extent(WindowExtent extent) noexcept = 0;
    /** @brief Shows or hides the platform window. */
    [[nodiscard]] virtual foundation::Result set_visible(bool visible) noexcept = 0;
    /** @brief Requests platform focus for the window. */
    [[nodiscard]] virtual foundation::Result focus() noexcept = 0;
    /** @brief Posts a semantic close request to the platform window. */
    [[nodiscard]] virtual foundation::Result request_close() noexcept = 0;
};

/** @brief Callback invoked for each semantic platform event during event polling. */
using WindowEventSink = std::function<void(const WindowEvent&)>;

/** @brief Creates, finds, pumps, and destroys Application-owned platform windows. */
class IWindowSystem {
public:
    virtual ~IWindowSystem() = default;

    /** @brief Creates one window and transfers shared ownership to the caller. */
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<IWindow>> create_window(const WindowDesc& desc) = 0;
    /** @brief Destroys a live window after its generation-checked identity is validated. */
    [[nodiscard]] virtual foundation::Result destroy_window(WindowId window) = 0;
    /** @brief Pumps native messages and emits backend-neutral events to the sink. */
    [[nodiscard]] virtual foundation::Result poll_events(const WindowEventSink& sink) = 0;
    /** @brief Finds a live window by semantic identity. */
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<IWindow>> find_window(WindowId window) const = 0;
    /** @brief Returns backend-neutral clipboard, cursor, IME, and display services. */
    [[nodiscard]] virtual IPlatformServices& platform_services() noexcept = 0;
};
}  // namespace aeg::platform
