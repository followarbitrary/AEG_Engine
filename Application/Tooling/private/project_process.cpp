#include "project_process.hpp"

#include <system_error>

#include "concurrency.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace aeg::tooling::detail {
namespace {
#if defined(_WIN32)
std::wstring quote_argument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring::npos)
        return argument;
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            result.append(slashes * 2u + 1u, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2u, L'\\');
    result.push_back(L'\"');
    return result;
}
#endif
}  // namespace

foundation::ResultValue<ProjectProcessResult> run_project_process(const ProjectProcessRequest& request) {
    if (request.executable.empty())
        return {foundation::invalid_argument("Process executable is empty"), {}};
#if defined(_WIN32)
    std::wstring command = quote_argument(request.executable.wstring());
    for (const auto& argument : request.arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working =
        request.working_directory.empty() ? std::wstring{} : request.working_directory.wstring();
    if (!CreateProcessW(request.executable.wstring().c_str(), command_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, working.empty() ? nullptr : working.c_str(), &startup, &process)) {
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot start project process"), {}};
    }
    bool cancelled = false;
    for (;;) {
        const DWORD wait_result = WaitForSingleObject(process.hProcess, 50);
        if (wait_result == WAIT_OBJECT_0)
            break;
        if (wait_result == WAIT_FAILED) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return {
                foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot wait for project process"),
                {}};
        }
        if (request.cancellation_requested && request.cancellation_requested()) {
            cancelled = true;
            (void)TerminateProcess(process.hProcess, 0xC000013Au);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return {foundation::Result::success(), {static_cast<int>(exit_code), cancelled}};
#else
    std::vector<std::string> storage;
    storage.reserve(request.arguments.size() + 1);
    storage.push_back(request.executable.string());
    for (const auto& argument : request.arguments)
        storage.emplace_back(argument.begin(), argument.end());
    std::vector<char*> argv;
    for (auto& value : storage)
        argv.push_back(value.data());
    argv.push_back(nullptr);
    pid_t child = 0;
    const int spawn_result = posix_spawn(&child, storage.front().c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_result != 0)
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot start project process"), {}};
    int status = 0;
    bool cancelled = false;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0)
            return {
                foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot wait for project process"),
                {}};
        if (request.cancellation_requested && request.cancellation_requested()) {
            cancelled = true;
            (void)kill(child, SIGTERM);
            if (waitpid(child, &status, 0) < 0)
                return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                    "Cannot wait for project process"),
                        {}};
            break;
        }
        const foundation::Result delay = concurrency::sleep_for(50'000'000ull);
        if (!delay)
            return {delay, {}};
    }
    return {foundation::Result::success(), {WIFEXITED(status) ? WEXITSTATUS(status) : -1, cancelled}};
#endif
}
}  // namespace aeg::tooling::detail
