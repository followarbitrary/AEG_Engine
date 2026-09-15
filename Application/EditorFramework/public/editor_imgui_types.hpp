/**
 * @file editor_imgui_types.hpp
 * @brief Backend-neutral configuration and identity types for Editor ImGui.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "editor_font.hpp"
#include "platform_services.hpp"
#include "window_types.hpp"

namespace aeg::platform {
class IWindowSystem;
}

namespace aeg::editor {
/**
 * @brief Configuration for one engine-owned Dear ImGui context.
 *
 * The context is thread-confined to the editor thread. Platform and viewport
 * services are borrowed and must outlive the host while it is initialized.
 */
struct EditorImGuiDesc final {
    /** @brief Enables Dear ImGui keyboard navigation. */
    bool enable_keyboard_navigation{true};
    /** @brief Enables Dear ImGui gamepad navigation. */
    bool enable_gamepad_navigation{false};
    /** @brief Global scale applied by Dear ImGui to its fonts and widgets. */
    float font_global_scale{1.0f};
    /** @brief Borrowed platform services used for clipboard, cursor, and IME integration. */
    platform::IPlatformServices* platform_services{nullptr};
    /** @brief Borrowed window system used to create engine-owned ImGui platform viewports. */
    platform::IWindowSystem* window_system{nullptr};
    /** @brief Enables docking platform viewports when a window system is available. */
    bool enable_viewports{false};
    /** @brief Borrowed font catalog copied while the ImGui host initializes. */
    const EditorFontCatalog* font_catalog{nullptr};
    /** @brief Stable key of the initial font selected from the catalog. */
    std::string_view default_font_key{};
    /** @brief Base font size in pixels used to build the initial atlas. */
    float font_size_pixels{16.0f};
    /** @brief Pre-rasterize the fixed Settings chrome independently of the editable preview. */
    bool enable_preference_fonts{false};
};

/** @brief Engine-owned timing and display metrics for one ImGui frame. */
struct EditorImGuiFrameDesc final {
    /** @brief Semantic window identity receiving the frame's primary input. */
    platform::WindowId window{};
    /** @brief Logical client-area extent in display coordinates. */
    platform::WindowExtent display_extent{};
    /** @brief Positive elapsed time since the previous frame. */
    float delta_seconds{1.0f / 60.0f};
    /** @brief Horizontal framebuffer density used by Dear ImGui. */
    float framebuffer_scale_x{1.0f};
    /** @brief Vertical framebuffer density used by Dear ImGui. */
    float framebuffer_scale_y{1.0f};
};

/** @brief Engine-owned RGBA8 snapshot of the ImGui font atlas. */
struct ImGuiFontAtlasData final {
    /** @brief Atlas width in pixels. */
    uint32_t width{0};
    /** @brief Atlas height in pixels. */
    uint32_t height{0};
    /** @brief Number of bytes between adjacent atlas rows. */
    uint32_t row_pitch{0};
    /** @brief RGBA8 pixel bytes borrowed by the caller after capture. */
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
