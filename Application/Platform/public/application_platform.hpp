/**
 * @file application_platform.hpp
 * @brief Application-owned platform factories.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "result.hpp"
#include "window.hpp"

namespace aeg::application {
/** @brief Parameters for an Application-owned native startup splash. */
struct StartupSplashDesc final {
    /** @brief Image displayed inside the splash window. */
    std::filesystem::path image_path{};
    /** @brief Splash client-area extent in physical pixels. */
    platform::WindowExtent extent{560, 560};
    /** @brief Native accessibility and diagnostics name for the splash window. */
    std::string accessible_name{"Aether Engine"};
};

/** @brief Owns one native startup splash without exposing platform handles. */
class IStartupSplash {
public:
    virtual ~IStartupSplash() = default;

    /** @brief Closes the splash immediately; repeated calls are harmless. */
    virtual void close() noexcept = 0;
};

/** @brief Creates the active host platform window system without exposing native types. */
[[nodiscard]] foundation::ResultValue<std::unique_ptr<platform::IWindowSystem>> create_window_system();

/** @brief Receives process-interrupt requests from the active host platform. */
class IConsoleInterruptController {
public:
    virtual ~IConsoleInterruptController() = default;

    /** @brief Returns true after the host has requested orderly process termination. */
    [[nodiscard]] virtual bool interrupted() const noexcept = 0;
};

/**
 * @brief Installs one process-wide, platform-owned interrupt controller.
 *
 * The controller is removed when the returned object is destroyed. A second
 * active controller is rejected so the host owns signal registration in one
 * place.
 */
[[nodiscard]] foundation::ResultValue<std::unique_ptr<IConsoleInterruptController>>
install_console_interrupt_controller();

/** @brief Shows a centered transparent native startup splash loaded from an image file. */
[[nodiscard]] foundation::ResultValue<std::unique_ptr<IStartupSplash>> show_startup_splash(
    const StartupSplashDesc& desc);

/** @brief Shows an Application-owned native error dialog without exposing native types. */
[[nodiscard]] foundation::Result show_error_dialog(std::string_view title, std::string_view message);

/**
 * @brief Opens the native folder picker and returns the selected UTF-8 directory.
 *
 * Closing the picker without selecting a directory returns kCancelled. The
 * initial directory may name a not-yet-created project; its nearest existing
 * parent is used as the picker's starting location.
 */
[[nodiscard]] foundation::ResultValue<std::string> select_directory(platform::WindowSurfaceBinding owner,
                                                                    std::string_view title,
                                                                    std::string_view initial_directory = {});

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
