#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aeg::platform {
/** @brief Generation-checked identity for one engine window. */
struct WindowId final {
    /** @brief Slot index assigned by the owning window system. */
    uint32_t index{UINT32_MAX};
    /** @brief Slot generation used to reject stale identities. */
    uint32_t generation{0};

    /** @brief Returns whether the identity can refer to a live window. */
    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != UINT32_MAX && generation != 0; }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(WindowId, WindowId) = default;
};

/** @brief Hash function for generation-checked window identities. */
struct WindowIdHash final {
    [[nodiscard]] std::size_t operator()(WindowId value) const noexcept {
        const std::size_t index = static_cast<std::size_t>(value.index);
        const std::size_t generation = static_cast<std::size_t>(value.generation);
        return index ^ (generation + static_cast<std::size_t>(0x9e3779b9u) + (index << 6u) + (index >> 2u));
    }
};

/** @brief Unsigned client-area extent in display pixels. */
struct WindowExtent final {
    /** @brief Client-area width in pixels. */
    uint32_t width{1};
    /** @brief Client-area height in pixels. */
    uint32_t height{1};

    /** @brief Returns whether both dimensions are non-zero. */
    [[nodiscard]] constexpr bool is_valid() const noexcept { return width != 0 && height != 0; }
    friend constexpr bool operator==(WindowExtent, WindowExtent) = default;
};

/** @brief Signed screen-space position of a window client area. */
struct WindowPosition final {
    /** @brief Horizontal screen coordinate of the client-area origin. */
    int32_t x{0};
    /** @brief Vertical screen coordinate of the client-area origin. */
    int32_t y{0};

    friend constexpr bool operator==(WindowPosition, WindowPosition) = default;
};

/** @brief Work area of the display containing a platform window. */
struct DisplayWorkArea final {
    /** @brief Screen-space origin of the usable display area. */
    WindowPosition position{};
    /** @brief Usable display extent excluding taskbars and reserved edges. */
    WindowExtent extent{};

    /** @brief Returns whether the display work area contains usable dimensions. */
    [[nodiscard]] constexpr bool is_valid() const noexcept { return extent.is_valid(); }
};

/** @brief Opaque display and window handles used only by Application RHI setup. */
struct WindowSurfaceBinding final {
    /** @brief Opaque display or instance handle supplied by the platform adapter. */
    void* display_handle{nullptr};
    /** @brief Opaque window handle supplied by the platform adapter. */
    void* window_handle{nullptr};

    /** @brief Returns whether both opaque handles are available. */
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return display_handle != nullptr && window_handle != nullptr;
    }
};

/** @brief Semantic parameters used to create one platform window. */
struct WindowDesc final {
    /** @brief UTF-8 title requested by the caller. */
    std::string title{"AEG Engine"};
    /** @brief Initial client-area extent. */
    WindowExtent extent{};
    /** @brief Initial client-area position when position_specified is true. */
    WindowPosition position{};
    /** @brief Selects whether the requested initial position is applied. */
    bool position_specified{false};
    /** @brief Enables user resizing when supported by the platform. */
    bool resizable{true};
    /** @brief Shows the window after native creation. */
    bool visible{true};
    /** @brief Requests platform-managed high-DPI behavior when supported. */
    bool high_dpi{true};
    /** @brief Requests that the platform starts the window maximized when visible. */
    bool start_maximized{false};
};

/** @brief Observable semantic state of a live platform window. */
struct WindowState final {
    /** @brief Current client-area screen position. */
    WindowPosition position{};
    /** @brief Current client-area extent. */
    WindowExtent extent{};
    /** @brief Whether the platform window is currently visible. */
    bool visible{false};
    /** @brief Whether the platform window currently owns input focus. */
    bool focused{false};
    /** @brief Whether the platform reports the window as minimized. */
    bool minimized{false};
    /** @brief Whether the platform reports the window as maximized. */
    bool maximized{false};
    /** @brief Whether user input is currently enabled for the window. */
    bool input_enabled{true};
    /** @brief Whether a close request has been received for the window. */
    bool close_requested{false};
};
}  // namespace aeg::platform
