#include "dynamic_library.hpp"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace aeg::module::detail {
namespace {
foundation::Result os_error(std::string_view action, const std::filesystem::path& path) {
#if defined(_WIN32)
    return foundation::Result::failure(
        foundation::ResultCode::kNotFound,
        std::string(action) + " failed for " + path.string() + " (win32=" + std::to_string(GetLastError()) + ")");
#else
    return foundation::unsupported(std::string(action) + " is unsupported on this platform: " + path.string());
#endif
}
}  // namespace

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : native_handle_(other.detach()) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        native_handle_ = other.detach();
    }
    return *this;
}

DynamicLibrary::~DynamicLibrary() {
    static_cast<void>(close());
}

foundation::ResultValue<DynamicLibrary> DynamicLibrary::open(const std::filesystem::path& path) {
    if (path.empty())
        return {foundation::invalid_argument("dynamic library path is empty"), {}};
#if defined(_WIN32)
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error)
        return {foundation::invalid_argument("dynamic library path could not be canonicalized"), {}};
    const HMODULE module = LoadLibraryExW(canonical.wstring().c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr)
        return {os_error("LoadLibraryExW", canonical), {}};
    return {foundation::Result::success(), DynamicLibrary(static_cast<void*>(module))};
#else
    return {os_error("dynamic library loading", path), {}};
#endif
}

DynamicLibrary DynamicLibrary::adopt(void* native_handle) noexcept {
    return DynamicLibrary(native_handle);
}

foundation::ResultValue<void*> DynamicLibrary::symbol(std::string_view name) const {
    if (!valid() || name.empty())
        return {foundation::invalid_argument("dynamic library symbol request is invalid"), {}};
#if defined(_WIN32)
    std::string symbol_name(name);
    const auto address = GetProcAddress(static_cast<HMODULE>(native_handle_), symbol_name.c_str());
    if (address == nullptr)
        return {foundation::not_found("dynamic library symbol is missing: " + symbol_name), {}};
    return {foundation::Result::success(), reinterpret_cast<void*>(address)};
#else
    return {foundation::unsupported("dynamic library symbol lookup is unsupported on this platform"), {}};
#endif
}

foundation::Result DynamicLibrary::close() noexcept {
    if (native_handle_ == nullptr)
        return foundation::Result::success();
#if defined(_WIN32)
    if (FreeLibrary(static_cast<HMODULE>(native_handle_)) == FALSE) {
        native_handle_ = nullptr;
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "FreeLibrary failed");
    }
#else
    return foundation::unsupported("dynamic library close is unsupported on this platform");
#endif
    native_handle_ = nullptr;
    return foundation::Result::success();
}

void* DynamicLibrary::detach() noexcept {
    void* result = native_handle_;
    native_handle_ = nullptr;
    return result;
}
}  // namespace aeg::module::detail
