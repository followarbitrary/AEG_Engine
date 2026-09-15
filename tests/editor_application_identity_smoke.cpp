/**
 * @file editor_application_identity_smoke.cpp
 * @brief Verifies the editor's shipped executable name and native window title.
 */
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

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
    if (process_id != search.process_id) {
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
}  // namespace

int main() {
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
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }

    if (search.matching_window != nullptr) {
        if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId) == FALSE) {
            TerminateProcess(process.hProcess, 4);
        }
    }
    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 3);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (search.matching_window == nullptr) {
        return 3;
    }
    return exit_code == 0 ? 0 : 4;
}
