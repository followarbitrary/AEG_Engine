/**
 * @file win32_platform_helpers.hpp
 * @brief Shared private helpers for Win32 Application platform implementations.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <exception>
#include <memory>

#include "result.hpp"

namespace aeg::application::win32::detail {
[[nodiscard]] inline foundation::Result exception_result(const std::exception& exception) noexcept {
    try {
        return foundation::unknown_exception(exception);
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown);
    }
}

struct ComApartmentCloser final {
    ~ComApartmentCloser() { CoUninitialize(); }
};

struct ComMemoryDeleter final {
    void operator()(wchar_t* memory) const noexcept { CoTaskMemFree(memory); }
};
}  // namespace aeg::application::win32::detail
