#pragma once

#include <functional>
#include <memory>

#include "platform_services.hpp"
#include "result.hpp"
#include "window_event.hpp"
#include "window_types.hpp"

namespace aeg::platform {
class IWindow {
public:
    virtual ~IWindow() = default;

    [[nodiscard]] virtual WindowId id() const noexcept = 0;
    [[nodiscard]] virtual const WindowDesc& desc() const noexcept = 0;
    [[nodiscard]] virtual WindowState state() const noexcept = 0;
    [[nodiscard]] virtual WindowSurfaceBinding surface_binding() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result set_extent(WindowExtent extent) noexcept = 0;
    [[nodiscard]] virtual foundation::Result request_close() noexcept = 0;
};

using WindowEventSink = std::function<void(const WindowEvent&)>;

class IWindowSystem {
public:
    virtual ~IWindowSystem() = default;

    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<IWindow>> create_window(const WindowDesc& desc) = 0;
    [[nodiscard]] virtual foundation::Result destroy_window(WindowId window) = 0;
    [[nodiscard]] virtual foundation::Result poll_events(const WindowEventSink& sink) = 0;
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<IWindow>> find_window(WindowId window) const = 0;
    [[nodiscard]] virtual IPlatformServices& platform_services() noexcept = 0;
};
}  // namespace aeg::platform
