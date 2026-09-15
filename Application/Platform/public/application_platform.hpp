/**
 * @file application_platform.hpp
 * @brief Application-owned platform factories.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "window.hpp"
#include "result.hpp"

namespace aeg::application {
/** @brief Creates the Win32 window system without exposing Win32 types. */
[[nodiscard]] foundation::ResultValue<std::unique_ptr<platform::IWindowSystem>> create_win32_window_system();

/** @brief Shows an Application-owned native error dialog without exposing Win32 types. */
[[nodiscard]] foundation::Result show_error_dialog(std::string_view title, std::string_view message);

/**
 * @brief Opens the native folder picker and returns the selected UTF-8 directory.
 *
 * Closing the picker without selecting a directory returns kCancelled. The
 * initial directory may name a not-yet-created project; its nearest existing
 * parent is used as the picker's starting location.
 */
[[nodiscard]] foundation::ResultValue<std::string> select_directory(
    platform::WindowSurfaceBinding owner, std::string_view title, std::string_view initial_directory = {});

/** @brief Describes one installed system font face without exposing platform API types. */
struct SystemFontRecord final {
    /** @brief User-facing family name selected from the system font collection. */
    std::string family_name{};
    /** @brief UTF-8 path to the installed font file. */
    std::string file_path{};
    /** @brief Face index used when the file contains multiple faces. */
    uint32_t face_index{0};
    /** @brief Whether the face contains common simplified Chinese glyphs. */
    bool supports_chinese{false};
    /** @brief Whether the face contains common programming ligatures. */
    bool supports_ligatures{false};
};

/** @brief Enumerates readable system font faces for Application-owned editor services. */
[[nodiscard]] foundation::ResultValue<std::vector<SystemFontRecord>> enumerate_system_fonts();
}  // namespace aeg::application
