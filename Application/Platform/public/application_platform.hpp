/**
 * @file application_platform.hpp
 * @brief Application-owned platform factories.
 */
#pragma once

#include <memory>

#include "window.hpp"

namespace aeg::application {
/** @brief Creates the Win32 window system without exposing Win32 types. */
[[nodiscard]] foundation::ResultValue<std::unique_ptr<platform::IWindowSystem>> create_win32_window_system();
}  // namespace aeg::application
