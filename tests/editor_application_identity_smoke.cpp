/**
 * @file editor_application_identity_smoke.cpp
 * @brief Verifies the editor's shipped executable name and native window title.
 */
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <concurrency.hpp>
#include <filesystem>
#include <string>

#ifndef AEG_EDITOR_APPLICATION_PATH
#error "AEG_EDITOR_APPLICATION_PATH must name the built editor executable"
#endif

namespace {
struct WindowSearch final {
    DWORD process_id{0};
    HWND matching_window{nullptr};
};

BOOL CALLBACK find_editor_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != search.process_id || IsWindowVisible(window) == FALSE ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    wchar_t title[256]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (std::wstring_view{title} == L"Aether Engine") {
        search.matching_window = window;
        return FALSE;
    }
    return TRUE;
}
bool verify_main_window_matching() {
    struct WindowCase final {
        DWORD extended_style;
        bool visible;
        bool expected_match;
    };
    // A startup splash and a hidden main window can share the shipped title.
    // Only a visible application window establishes the identity being tested.
    constexpr WindowCase cases[]{{0, false, false},
                                 {WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, true, false},
                                 {0, true, true}};
    for (const auto& test_case : cases) {
        HWND window = CreateWindowExW(test_case.extended_style, L"STATIC", L"Aether Engine",
                                      WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        if (window == nullptr) {
            std::fprintf(stderr, "Identity fixture window creation failed: %lu\n", GetLastError());
            return false;
        }
        if (test_case.visible) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
        }
        WindowSearch search{GetCurrentProcessId(), nullptr};
        EnumWindows(&find_editor_window, reinterpret_cast<LPARAM>(&search));
        const bool matched = search.matching_window == window;
        DestroyWindow(window);
        if (matched != test_case.expected_match) {
            std::fprintf(stderr, "Identity matched=%d for visible=%d, extended_style=%lu\n",
                         matched, test_case.visible, test_case.extended_style);
            return false;
        }
    }
    return true;
}
}  // namespace

int main() {
    if (!verify_main_window_matching()) {
        return 5;
    }
    const std::filesystem::path editor_path{AEG_EDITOR_APPLICATION_PATH};
    if (editor_path.filename() != L"AetherEngine.exe") {
        return 1;
    }

    std::wstring command_line = L"\"" + editor_path.wstring() + L"\" --frames 600";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(editor_path.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr,
                       editor_path.parent_path().c_str(), &startup, &process) == FALSE) {
        return 2;
    }
    CloseHandle(process.hThread);

    WindowSearch search{process.dwProcessId, nullptr};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    while (std::chrono::steady_clock::now() < deadline) {
        EnumWindows(&find_editor_window, reinterpret_cast<LPARAM>(&search));
        if (search.matching_window != nullptr) {
            break;
        }
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        static_cast<void>(aeg::concurrency::sleep_for(25'000'000ull));
    }

    if (search.matching_window != nullptr) {
        if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId) == FALSE) {
            std::fprintf(stderr, "Identity could not signal editor exit: %lu\n", GetLastError());
            TerminateProcess(process.hProcess, 4);
        }
    }
    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
        std::fprintf(stderr, "Identity editor did not exit within 5 seconds; visible main window=%d\n",
                     search.matching_window != nullptr);
        TerminateProcess(process.hProcess, 3);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (search.matching_window == nullptr) {
        std::fprintf(stderr, "Identity did not find a visible main window; editor exit code=%lu\n", exit_code);
        return 3;
    }
    if (exit_code != 0) {
        std::fprintf(stderr, "Identity editor exit code=%lu\n", exit_code);
        return 4;
    }
    return 0;
}
