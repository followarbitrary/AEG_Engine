/**
 * @file win32_clipboard_text.hpp
 * @brief Bounds clipboard text reads by the memory allocation supplied by Win32.
 */
#pragma once

#include <algorithm>
#include <span>
#include <string_view>

#include "result.hpp"

namespace aeg::application::win32 {
[[nodiscard]] inline foundation::ResultValue<std::wstring_view> clipboard_text_view(
    std::span<const wchar_t> storage) noexcept {
    const auto terminator = std::find(storage.begin(), storage.end(), L'\0');
    if (terminator == storage.end()) {
        return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument), {}};
    }
    return {foundation::Result::success(),
            std::wstring_view{storage.data(), static_cast<std::size_t>(terminator - storage.begin())}};
}
}  // namespace aeg::application::win32
