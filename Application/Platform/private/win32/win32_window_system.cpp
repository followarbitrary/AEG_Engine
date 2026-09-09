/**
 * @file win32_window_system.cpp
 * @brief Win32 window creation, ownership, and message pumping.
 */
#include "win32_window_system.hpp"

#include <windows.h>

#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "application_platform.hpp"
#include "win32_window.hpp"

namespace aeg::application::win32 {
namespace {
constexpr wchar_t kWindowClassName[] = L"AEG.Engine.Application.Win32Window";

foundation::Result backend_error(const char* message) {
    return foundation::Result::failure(foundation::ResultCode::kBackendError, message);
}

foundation::ResultValue<std::wstring> convert_title(std::string_view title) {
    if (title.empty()) {
        return {foundation::invalid_argument("Window title must not be empty"), {}};
    }
    if (title.find('\0') != std::string_view::npos ||
        title.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {foundation::invalid_argument("Window title is invalid"), {}};
    }
    const int source_length = static_cast<int>(title.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.data(), source_length, nullptr, 0);
    if (required <= 0) {
        return {backend_error("UTF-8 window title conversion failed"), {}};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.data(), source_length, result.data(), required) !=
        required) {
        return {backend_error("UTF-8 window title conversion failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}

DWORD window_style(const platform::WindowDesc& desc) noexcept {
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!desc.resizable) {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    return style;
}
}  // namespace

Win32WindowSystem::Win32WindowSystem()
    : instance_(GetModuleHandleW(nullptr)), start_time_(std::chrono::steady_clock::now()) {}

Win32WindowSystem::~Win32WindowSystem() {
    for (WindowSlot& slot : windows_) {
        if (slot.window == nullptr) {
            continue;
        }
        slot.window->detach_owner();
        slot.window->destroy_native();
        slot.window.reset();
    }
}

foundation::ResultValue<std::shared_ptr<platform::IWindow>> Win32WindowSystem::create_window(
    const platform::WindowDesc& desc) {
    if (!desc.extent.is_valid()) {
        return {foundation::invalid_argument("Window extent must be non-zero"), {}};
    }
    if (desc.extent.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        desc.extent.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return {foundation::invalid_argument("Window extent exceeds Win32 limits"), {}};
    }

    try {
        if (const foundation::Result result = register_window_class(); !result) {
            return {result, {}};
        }
        const foundation::ResultValue<std::wstring> title_result = to_wide_title(desc);
        if (!title_result) {
            return {title_result.result, {}};
        }

        const std::size_t index = find_free_slot();
        if (index > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            return {foundation::out_of_memory("Win32 window identity space is exhausted"), {}};
        }
        if (index == windows_.size()) {
            windows_.push_back(WindowSlot{});
        }
        WindowSlot& slot = windows_[index];
        const platform::WindowId id{static_cast<uint32_t>(index), slot.generation};
        auto window = std::make_shared<Win32Window>(id, this, desc);

        RECT client_rect{0, 0, static_cast<LONG>(desc.extent.width), static_cast<LONG>(desc.extent.height)};
        if (AdjustWindowRectEx(&client_rect, window_style(desc), FALSE, 0) == FALSE) {
            return {backend_error("AdjustWindowRectEx failed"), {}};
        }
        HWND handle = CreateWindowExW(0, kWindowClassName, title_result.value.c_str(), window_style(desc),
                                      CW_USEDEFAULT, CW_USEDEFAULT, client_rect.right - client_rect.left,
                                      client_rect.bottom - client_rect.top, nullptr, nullptr, instance_, window.get());
        if (handle == nullptr) {
            return {backend_error("CreateWindowExW failed"), {}};
        }
        window->attach(handle);
        if (desc.visible) {
            ShowWindow(handle, SW_SHOWNORMAL);
            UpdateWindow(handle);
        }
        slot.window = std::move(window);
        return {foundation::Result::success(), std::static_pointer_cast<platform::IWindow>(slot.window)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Win32 window allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

foundation::Result Win32WindowSystem::destroy_window(platform::WindowId window) {
    if (!window.is_valid() || window.index >= windows_.size()) {
        return foundation::not_found("Win32 window identity is not registered");
    }
    WindowSlot& slot = windows_[window.index];
    if (slot.window == nullptr || slot.generation != window.generation) {
        return foundation::not_found("Win32 window identity is stale");
    }
    slot.window->destroy_native();
    slot.window.reset();
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    return foundation::Result::success();
}

foundation::Result Win32WindowSystem::poll_events(const platform::WindowEventSink& sink) {
    if (!sink) {
        return foundation::invalid_argument("Window event sink must not be empty");
    }
    try {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            if (message.message == WM_QUIT) {
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (event_allocation_failed_) {
            event_allocation_failed_ = false;
            pending_events_.clear();
            return foundation::out_of_memory("Win32 window event queue allocation failed");
        }
        while (!pending_events_.empty()) {
            platform::WindowEvent event = std::move(pending_events_.front());
            pending_events_.pop_front();
            sink(event);
        }
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Win32 window event polling allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::ResultValue<std::shared_ptr<platform::IWindow>> Win32WindowSystem::find_window(
    platform::WindowId window) const {
    if (!window.is_valid() || window.index >= windows_.size()) {
        return {foundation::not_found("Win32 window identity is not registered"), {}};
    }
    const WindowSlot& slot = windows_[window.index];
    if (slot.window == nullptr || slot.generation != window.generation) {
        return {foundation::not_found("Win32 window identity is stale"), {}};
    }
    try {
        return {foundation::Result::success(), std::static_pointer_cast<platform::IWindow>(slot.window)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Win32 window lookup allocation failed"), {}};
    }
}

foundation::Result Win32WindowSystem::register_window_class() {
    if (class_registered_) {
        return foundation::Result::success();
    }
    if (instance_ == nullptr) {
        return foundation::invalid_state("Win32 application module handle is unavailable");
    }
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &Win32Window::window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return backend_error("RegisterClassExW failed");
    }
    class_registered_ = true;
    return foundation::Result::success();
}

foundation::ResultValue<std::wstring> Win32WindowSystem::to_wide_title(const platform::WindowDesc& desc) const {
    return convert_title(desc.title);
}

std::size_t Win32WindowSystem::find_free_slot() const noexcept {
    for (std::size_t index = 0; index < windows_.size(); ++index) {
        if (windows_[index].window == nullptr) {
            return index;
        }
    }
    return windows_.size();
}

uint64_t Win32WindowSystem::next_sequence() noexcept {
    const uint64_t sequence = next_sequence_;
    ++next_sequence_;
    if (next_sequence_ == 0) {
        next_sequence_ = 1;
    }
    return sequence;
}

double Win32WindowSystem::timestamp_seconds() const noexcept {
    const auto elapsed = std::chrono::steady_clock::now() - start_time_;
    return std::chrono::duration<double>(elapsed).count();
}

void Win32WindowSystem::enqueue_event(platform::WindowEvent event) noexcept {
    try {
        pending_events_.push_back(std::move(event));
    } catch (...) {
        event_allocation_failed_ = true;
    }
}
}  // namespace aeg::application::win32

namespace aeg::application {
foundation::ResultValue<std::unique_ptr<platform::IWindowSystem>> create_win32_window_system() {
    try {
        return {foundation::Result::success(), std::make_unique<win32::Win32WindowSystem>()};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Win32 window system allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}
}  // namespace aeg::application
