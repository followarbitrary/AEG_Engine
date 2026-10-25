/**
 * @file win32_console_interrupt.cpp
 * @brief Win32 console-control translation for the platform-neutral Application interface.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <new>

#include "application_platform.hpp"

namespace {
std::atomic_bool g_interrupt_requested{false};
std::atomic_bool g_interrupt_controller_installed{false};

BOOL WINAPI win32_console_control_handler(DWORD control) noexcept {
    if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    g_interrupt_requested.store(true, std::memory_order_relaxed);
    return TRUE;
}

class Win32ConsoleInterruptController final : public aeg::application::IConsoleInterruptController {
public:
    ~Win32ConsoleInterruptController() override {
        static_cast<void>(SetConsoleCtrlHandler(&win32_console_control_handler, FALSE));
        g_interrupt_requested.store(false, std::memory_order_relaxed);
        g_interrupt_controller_installed.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool interrupted() const noexcept override {
        return g_interrupt_requested.load(std::memory_order_relaxed);
    }
};
}  // namespace

namespace aeg::application {
foundation::ResultValue<std::unique_ptr<IConsoleInterruptController>> install_console_interrupt_controller() {
    bool expected = false;
    if (!g_interrupt_controller_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return {foundation::invalid_state("A console interrupt controller is already installed"), {}};
    }

    g_interrupt_requested.store(false, std::memory_order_relaxed);
    if (SetConsoleCtrlHandler(&win32_console_control_handler, TRUE) == FALSE) {
        g_interrupt_controller_installed.store(false, std::memory_order_release);
        return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                            "Platform console interrupt handler registration failed"),
                {}};
    }

    try {
        return {foundation::Result::success(), std::make_unique<Win32ConsoleInterruptController>()};
    } catch (const std::bad_alloc&) {
        static_cast<void>(SetConsoleCtrlHandler(&win32_console_control_handler, FALSE));
        g_interrupt_controller_installed.store(false, std::memory_order_release);
        return {foundation::out_of_memory("Console interrupt controller allocation failed"), {}};
    }
}
}  // namespace aeg::application
