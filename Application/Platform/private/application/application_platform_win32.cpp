/**
 * @file application_platform_win32.cpp
 * @brief Win32 implementations of the Application platform facade.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "application_platform.hpp"
#include "../win32/win32_platform_helpers.hpp"
#include "../win32/win32_window_system.hpp"

namespace aeg::application {
foundation::ResultValue<std::unique_ptr<platform::IWindowSystem>> create_window_system() {
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

foundation::ResultValue<std::string> select_directory(platform::WindowSurfaceBinding owner, std::string_view title,
                                                      std::string_view initial_directory) try {
    if (title.empty() || title.find('\0') != std::string_view::npos ||
        initial_directory.find('\0') != std::string_view::npos ||
        title.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        initial_directory.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {foundation::invalid_argument("Directory picker parameters are invalid"), {}};
    }

    const auto backend_failure = [](const char* message) {
        return foundation::ResultValue<std::string>{
            foundation::Result::failure(foundation::ResultCode::kBackendError, message), {}};
    };
    const auto to_wide = [](std::string_view value) -> foundation::ResultValue<std::wstring> {
        if (value.empty())
            return {foundation::Result::success(), {}};
        const int source_length = static_cast<int>(value.size());
        const int required =
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, nullptr, 0);
        if (required <= 0) {
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                "Directory picker UTF-8 conversion failed"),
                    {}};
        }
        std::wstring converted(static_cast<std::size_t>(required), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, converted.data(),
                                required) != required) {
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                "Directory picker UTF-8 conversion failed"),
                    {}};
        }
        return {foundation::Result::success(), std::move(converted)};
    };
    const auto to_utf8 = [](std::wstring_view value) -> foundation::ResultValue<std::string> {
        if (value.empty())
            return {foundation::Result::success(), {}};
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return {foundation::invalid_argument("Selected directory is too long"), {}};
        }
        const int source_length = static_cast<int>(value.size());
        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), source_length, nullptr, 0,
                                                 nullptr, nullptr);
        if (required <= 0) {
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                "Directory picker UTF-16 conversion failed"),
                    {}};
        }
        std::string converted(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), source_length, converted.data(), required,
                                nullptr, nullptr) != required) {
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                "Directory picker UTF-16 conversion failed"),
                    {}};
        }
        return {foundation::Result::success(), std::move(converted)};
    };

    const auto title_result = to_wide(title);
    if (!title_result)
        return {title_result.result, {}};
    const auto initial_result = to_wide(initial_directory);
    if (!initial_result)
        return {initial_result.result, {}};

    const HRESULT initialize_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialize_result)) {
        return backend_failure("Directory picker COM initialization failed");
    }
    const win32::detail::ComApartmentCloser apartment_closer{};

    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                      reinterpret_cast<void**>(dialog.GetAddressOf()));
    if (FAILED(result) || dialog == nullptr) {
        return backend_failure("Directory picker creation failed");
    }

    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    if (SUCCEEDED(result))
        result = dialog->SetTitle(title_result.value.c_str());

    if (SUCCEEDED(result) && !initial_result.value.empty()) {
        std::filesystem::path initial_path(initial_result.value);
        std::error_code error;
        while (!initial_path.empty() && (!std::filesystem::exists(initial_path, error) || error ||
                                         !std::filesystem::is_directory(initial_path, error) || error)) {
            error.clear();
            const std::filesystem::path parent = initial_path.parent_path();
            if (parent == initial_path)
                break;
            initial_path = parent;
        }
        if (!initial_path.empty()) {
            Microsoft::WRL::ComPtr<IShellItem> initial_item;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial_path.c_str(), nullptr, IID_IShellItem,
                                                      reinterpret_cast<void**>(initial_item.GetAddressOf()))) &&
                initial_item != nullptr) {
                static_cast<void>(dialog->SetFolder(initial_item.Get()));
            }
        }
    }

    if (SUCCEEDED(result)) {
        result = dialog->Show(static_cast<HWND>(owner.window_handle));
    }
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {foundation::Result::failure(foundation::ResultCode::kCancelled, "Directory selection was cancelled"),
                {}};
    }
    if (FAILED(result)) {
        return backend_failure("Directory picker could not be shown");
    }

    Microsoft::WRL::ComPtr<IShellItem> selected_item;
    result = dialog->GetResult(selected_item.GetAddressOf());
    PWSTR selected_path = nullptr;
    if (SUCCEEDED(result) && selected_item != nullptr) {
        result = selected_item->GetDisplayName(SIGDN_FILESYSPATH, &selected_path);
    }
    const std::unique_ptr<wchar_t, win32::detail::ComMemoryDeleter> selected_path_owner{selected_path};

    return FAILED(result) || selected_path == nullptr
               ? backend_failure("Directory picker did not return a filesystem path")
               : to_utf8(selected_path);
} catch (const std::bad_alloc&) {
    return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory), {}};
} catch (const std::exception& exception) {
    return {win32::detail::exception_result(exception), {}};
} catch (...) {
    return {foundation::Result::failure(foundation::ResultCode::kUnknown), {}};
}

foundation::ResultValue<std::optional<std::filesystem::path>> open_font_file_dialog(
    platform::IWindowSystem& window_system, platform::WindowId owner_window) try {
    if (!owner_window.is_valid()) {
        return {foundation::invalid_argument("Font picker owner window is invalid"), {}};
    }
    const auto owner_result = window_system.find_window(owner_window);
    if (!owner_result)
        return {owner_result.result, {}};
    const auto binding = owner_result.value->surface_binding();
    if (!binding.is_valid()) {
        return {foundation::invalid_state("Font picker owner window has no native binding"), {}};
    }

    const HRESULT initialize_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialize_result)) {
        return {
            foundation::Result::failure(foundation::ResultCode::kBackendError, "Font picker COM initialization failed"),
            {}};
    }
    const win32::detail::ComApartmentCloser apartment_closer{};
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                      reinterpret_cast<void**>(dialog.GetAddressOf()));
    if (FAILED(result) || dialog == nullptr) {
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "Font picker creation failed"), {}};
    }
    static constexpr COMDLG_FILTERSPEC filters[] = {
        {L"Font files", L"*.ttf;*.otf"},
        {L"TrueType fonts", L"*.ttf"},
        {L"OpenType fonts", L"*.otf"},
    };
    result = dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    if (SUCCEEDED(result))
        result = dialog->SetDefaultExtension(L"ttf");
    if (SUCCEEDED(result)) {
        DWORD options = 0;
        result = dialog->GetOptions(&options);
        if (SUCCEEDED(result)) {
            result = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        }
    }
    if (SUCCEEDED(result))
        result = dialog->SetTitle(L"Import editor font");
    if (SUCCEEDED(result))
        result = dialog->Show(static_cast<HWND>(binding.window_handle));
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {foundation::Result::success(), std::nullopt};
    }
    if (FAILED(result)) {
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "Font picker could not be shown"),
                {}};
    }
    Microsoft::WRL::ComPtr<IShellItem> selected_item;
    PWSTR selected_path = nullptr;
    result = dialog->GetResult(selected_item.GetAddressOf());
    if (SUCCEEDED(result) && selected_item != nullptr) {
        result = selected_item->GetDisplayName(SIGDN_FILESYSPATH, &selected_path);
    }
    const std::unique_ptr<wchar_t, win32::detail::ComMemoryDeleter> selected_path_owner{selected_path};
    if (FAILED(result) || selected_path == nullptr) {
        return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                            "Font picker did not return a filesystem path"),
                {}};
    }
    return {foundation::Result::success(), std::filesystem::path(selected_path)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("Font picker allocation failed"), {}};
} catch (const std::exception& exception) {
    return {win32::detail::exception_result(exception), {}};
} catch (...) {
    return {foundation::Result::failure(foundation::ResultCode::kUnknown), {}};
}
}  // namespace aeg::application
