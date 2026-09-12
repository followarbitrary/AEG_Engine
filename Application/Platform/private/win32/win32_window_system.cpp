/**
 * @file win32_window_system.cpp
 * @brief Win32 window creation, ownership, and message pumping.
 */
#include "win32_window_system.hpp"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
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

foundation::ResultValue<std::wstring> convert_utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {foundation::Result::success(), {}};
    }
    if (value.find('\0') != std::string_view::npos ||
        value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {foundation::invalid_argument("UTF-8 platform string is invalid"), {}};
    }
    const int source_length = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, nullptr, 0);
    if (required <= 0) {
        return {backend_error("UTF-8 platform string conversion failed"), {}};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, result.data(), required) !=
        required) {
        return {backend_error("UTF-8 platform string conversion failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}

foundation::ResultValue<std::string> convert_wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {foundation::Result::success(), {}};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {foundation::invalid_argument("UTF-16 clipboard text is too large"), {}};
    }
    const int source_length = static_cast<int>(value.size());
    const int required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), source_length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {backend_error("UTF-16 clipboard text conversion failed"), {}};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), source_length, result.data(), required,
                            nullptr, nullptr) != required) {
        return {backend_error("UTF-16 clipboard text conversion failed"), {}};
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

UINT dpi_for_window(HWND window) noexcept {
    using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
    static const auto get_dpi_for_window =
        std::bit_cast<GetDpiForWindowFunction>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (get_dpi_for_window != nullptr) {
        if (const UINT dpi = get_dpi_for_window(window); dpi != 0) {
            return dpi;
        }
    }

    HDC device_context = GetDC(window);
    if (device_context == nullptr) {
        return USER_DEFAULT_SCREEN_DPI;
    }
    const int dpi = GetDeviceCaps(device_context, LOGPIXELSX);
    ReleaseDC(window, device_context);
    return dpi > 0 ? static_cast<UINT>(dpi) : USER_DEFAULT_SCREEN_DPI;
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
        const int x = desc.position_specified ? desc.position.x : CW_USEDEFAULT;
        const int y = desc.position_specified ? desc.position.y : CW_USEDEFAULT;
        HWND handle = CreateWindowExW(0, kWindowClassName, title_result.value.c_str(), window_style(desc), x, y,
                                      client_rect.right - client_rect.left,
                                      client_rect.bottom - client_rect.top, nullptr, nullptr, instance_, window.get());
        if (handle == nullptr) {
            return {backend_error("CreateWindowExW failed"), {}};
        }
        window->attach(handle);
        window->sync_state_from_native();
        if (desc.visible) {
            ShowWindow(handle, desc.start_maximized ? SW_MAXIMIZE : SW_SHOWNORMAL);
            UpdateWindow(handle);
            window->sync_state_from_native();
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

foundation::ResultValue<std::string> Win32WindowSystem::get_clipboard_text() const {
    if (OpenClipboard(nullptr) == FALSE) {
        return {backend_error("OpenClipboard failed"), {}};
    }

    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        CloseClipboard();
        return {foundation::Result::success(), {}};
    }

    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text == nullptr) {
        CloseClipboard();
        return {backend_error("GlobalLock for clipboard text failed"), {}};
    }
    const std::wstring_view value{text, static_cast<std::size_t>(lstrlenW(text))};
    const foundation::ResultValue<std::string> result = convert_wide_to_utf8(value);
    GlobalUnlock(data);
    CloseClipboard();
    return result;
}

foundation::Result Win32WindowSystem::set_clipboard_text(std::string_view text) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return foundation::invalid_argument("Clipboard text is too large");
    }
    const foundation::ResultValue<std::wstring> wide_result = convert_utf8_to_wide(text);
    if (!wide_result) {
        return wide_result.result;
    }
    const std::wstring& wide_text = wide_result.value;
    if (wide_text.size() > (std::numeric_limits<SIZE_T>::max() / sizeof(wchar_t)) - 1u) {
        return foundation::out_of_memory("Clipboard text allocation size overflowed");
    }

    if (OpenClipboard(nullptr) == FALSE) {
        return backend_error("OpenClipboard failed");
    }
    if (EmptyClipboard() == FALSE) {
        CloseClipboard();
        return backend_error("EmptyClipboard failed");
    }

    const SIZE_T allocation_size = (wide_text.size() + 1u) * sizeof(wchar_t);
    HGLOBAL allocation = GlobalAlloc(GMEM_MOVEABLE, allocation_size);
    if (allocation == nullptr) {
        CloseClipboard();
        return foundation::out_of_memory("Clipboard text allocation failed");
    }
    void* destination = GlobalLock(allocation);
    if (destination == nullptr) {
        GlobalFree(allocation);
        CloseClipboard();
        return backend_error("GlobalLock for clipboard text failed");
    }
    std::memcpy(destination, wide_text.c_str(), allocation_size);
    GlobalUnlock(allocation);

    if (SetClipboardData(CF_UNICODETEXT, allocation) == nullptr) {
        GlobalFree(allocation);
        CloseClipboard();
        return backend_error("SetClipboardData failed");
    }
    CloseClipboard();
    return foundation::Result::success();
}

foundation::Result Win32WindowSystem::set_cursor(platform::WindowId window, platform::CursorShape shape) noexcept {
    if (!window.is_valid() || window.index >= windows_.size()) {
        return foundation::not_found("Win32 window identity is not registered");
    }
    WindowSlot& slot = windows_[window.index];
    if (slot.window == nullptr || slot.generation != window.generation) {
        return foundation::not_found("Win32 window identity is stale");
    }
    return slot.window->set_cursor(shape);
}

foundation::Result Win32WindowSystem::set_ime_state(platform::WindowId window,
                                                    const platform::ImeState& state) noexcept {
    if (!window.is_valid() || window.index >= windows_.size()) {
        return foundation::not_found("Win32 window identity is not registered");
    }
    WindowSlot& slot = windows_[window.index];
    if (slot.window == nullptr || slot.generation != window.generation) {
        return foundation::not_found("Win32 window identity is stale");
    }
    return slot.window->set_ime_state(state);
}

foundation::ResultValue<platform::WindowMetrics> Win32WindowSystem::query_window_metrics(
    platform::WindowId window) const {
    if (!window.is_valid() || window.index >= windows_.size()) {
        return {foundation::not_found("Win32 window identity is not registered"), {}};
    }
    const WindowSlot& slot = windows_[window.index];
    if (slot.window == nullptr || slot.generation != window.generation) {
        return {foundation::not_found("Win32 window identity is stale"), {}};
    }

    RECT client_rect{};
    if (GetClientRect(slot.window->hwnd_, &client_rect) == FALSE) {
        return {backend_error("GetClientRect failed"), {}};
    }
    const LONG width = std::max<LONG>(client_rect.right - client_rect.left, 1);
    const LONG height = std::max<LONG>(client_rect.bottom - client_rect.top, 1);
    const UINT dpi = dpi_for_window(slot.window->hwnd_);
    const float scale = static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
    return {foundation::Result::success(),
            platform::WindowMetrics{platform::WindowExtent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                                    scale, scale}};
}

foundation::ResultValue<platform::DisplayWorkArea> Win32WindowSystem::query_display_work_area(
    platform::WindowId window) const {
    if (!window.is_valid() || window.index >= windows_.size()) {
        return {foundation::not_found("Win32 window identity is not registered"), {}};
    }
    const WindowSlot& slot = windows_[window.index];
    if (slot.window == nullptr || slot.generation != window.generation || slot.window->hwnd_ == nullptr) {
        return {foundation::not_found("Win32 window identity is stale"), {}};
    }

    const HMONITOR monitor = MonitorFromWindow(slot.window->hwnd_, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return {backend_error("MonitorFromWindow failed"), {}};
    }
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (GetMonitorInfoW(monitor, &monitor_info) == FALSE) {
        return {backend_error("GetMonitorInfoW failed"), {}};
    }
    const RECT& work = monitor_info.rcWork;
    const LONG width = std::max<LONG>(work.right - work.left, 1);
    const LONG height = std::max<LONG>(work.bottom - work.top, 1);
    return {foundation::Result::success(),
            platform::DisplayWorkArea{platform::WindowPosition{work.left, work.top},
                                      platform::WindowExtent{static_cast<uint32_t>(width),
                                                             static_cast<uint32_t>(height)}}};
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
    if (desc.title.empty()) {
        return {foundation::invalid_argument("Window title must not be empty"), {}};
    }
    return to_wide_string(desc.title);
}

foundation::ResultValue<std::wstring> Win32WindowSystem::to_wide_string(std::string_view value) const {
    return convert_utf8_to_wide(value);
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

foundation::Result show_error_dialog(std::string_view title, std::string_view message) {
    if (title.empty() || message.empty() || title.find('\0') != std::string_view::npos ||
        message.find('\0') != std::string_view::npos ||
        title.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        message.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument);
    }

    try {
        const auto to_wide = [](std::string_view value) -> foundation::ResultValue<std::wstring> {
            const int source_length = static_cast<int>(value.size());
            const int required =
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, nullptr, 0);
            if (required <= 0) {
                return {foundation::Result::failure(foundation::ResultCode::kBackendError), {}};
            }
            std::wstring converted(static_cast<std::size_t>(required), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, converted.data(),
                                    required) != required) {
                return {foundation::Result::failure(foundation::ResultCode::kBackendError), {}};
            }
            return {foundation::Result::success(), std::move(converted)};
        };

        const foundation::ResultValue<std::wstring> title_result = to_wide(title);
        if (!title_result) {
            return title_result.result;
        }
        const foundation::ResultValue<std::wstring> message_result = to_wide(message);
        if (!message_result) {
            return message_result.result;
        }
        if (MessageBoxW(nullptr, message_result.value.c_str(), title_result.value.c_str(),
                        MB_OK | MB_ICONERROR | MB_TASKMODAL) == 0) {
            return foundation::Result::failure(foundation::ResultCode::kBackendError);
        }
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory);
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown);
    }
}
}  // namespace aeg::application
