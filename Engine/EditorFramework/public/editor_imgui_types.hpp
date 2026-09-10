/**
 * @file editor_imgui_types.hpp
 * @brief Backend-neutral configuration and identity types for Editor ImGui.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "platform_services.hpp"
#include "window_types.hpp"

namespace aeg::editor {
struct EditorImGuiDesc final {
    bool enable_keyboard_navigation{true};
    bool enable_gamepad_navigation{false};
    float font_global_scale{1.0f};
    platform::IPlatformServices* platform_services{nullptr};
};

struct EditorImGuiFrameDesc final {
    platform::WindowId window{};
    platform::WindowExtent display_extent{};
    float delta_seconds{1.0f / 60.0f};
    float framebuffer_scale_x{1.0f};
    float framebuffer_scale_y{1.0f};
};

/** @brief Engine-owned RGBA8 snapshot of the ImGui font atlas. */
struct ImGuiFontAtlasData final {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t row_pitch{0};
    std::vector<std::byte> rgba8_pixels{};

    [[nodiscard]] bool is_valid() const noexcept {
        const uint64_t required_row_pitch = static_cast<uint64_t>(width) * 4u;
        const uint64_t required_size = required_row_pitch * static_cast<uint64_t>(height);
        return width != 0 && height != 0 && row_pitch >= required_row_pitch &&
               static_cast<uint64_t>(rgba8_pixels.size()) >= static_cast<uint64_t>(row_pitch) * height &&
               required_size != 0;
    }

    void clear() noexcept {
        width = 0;
        height = 0;
        row_pitch = 0;
        rgba8_pixels.clear();
    }
};

struct EditorPanelId final {
    uint32_t index{UINT32_MAX};
    uint32_t generation{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != UINT32_MAX && generation != 0; }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(EditorPanelId, EditorPanelId) = default;
};

struct ImGuiTextureHandle final {
    uint32_t index{UINT32_MAX};
    uint32_t generation{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != UINT32_MAX && generation != 0; }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(ImGuiTextureHandle, ImGuiTextureHandle) = default;
};

[[nodiscard]] constexpr uint64_t make_imgui_texture_id(ImGuiTextureHandle texture) noexcept {
    if (!texture.is_valid()) {
        return 0;
    }
    return (static_cast<uint64_t>(texture.generation) << 32u) | (static_cast<uint64_t>(texture.index) + 1u);
}

[[nodiscard]] constexpr ImGuiTextureHandle decode_imgui_texture_id(uint64_t texture_id) noexcept {
    const uint32_t encoded_index = static_cast<uint32_t>(texture_id & 0xffffffffu);
    const uint32_t generation = static_cast<uint32_t>(texture_id >> 32u);
    if (encoded_index == 0 || generation == 0) {
        return {};
    }
    return ImGuiTextureHandle{encoded_index - 1u, generation};
}
}  // namespace aeg::editor
