#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aeg::platform {
struct WindowId final {
    uint32_t index{UINT32_MAX};
    uint32_t generation{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != UINT32_MAX && generation != 0; }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(WindowId, WindowId) = default;
};

struct WindowIdHash final {
    [[nodiscard]] std::size_t operator()(WindowId value) const noexcept {
        const std::size_t index = static_cast<std::size_t>(value.index);
        const std::size_t generation = static_cast<std::size_t>(value.generation);
        return index ^ (generation + static_cast<std::size_t>(0x9e3779b9u) + (index << 6u) + (index >> 2u));
    }
};

struct WindowExtent final {
    uint32_t width{1};
    uint32_t height{1};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return width != 0 && height != 0; }
    friend constexpr bool operator==(WindowExtent, WindowExtent) = default;
};

struct WindowSurfaceBinding final {
    void* display_handle{nullptr};
    void* window_handle{nullptr};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return display_handle != nullptr && window_handle != nullptr;
    }
};

struct WindowDesc final {
    std::string title{"AEG Engine"};
    WindowExtent extent{};
    bool resizable{true};
    bool visible{true};
    bool high_dpi{true};
};

struct WindowState final {
    WindowExtent extent{};
    bool focused{false};
    bool minimized{false};
    bool close_requested{false};
};
}  // namespace aeg::platform
