/**
 * @file editor_imgui.hpp
 * @brief Engine-owned Dear ImGui lifecycle and panel host.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "editor_imgui_types.hpp"
#include "editor_panel.hpp"
#include "imgui_draw_packet.hpp"
#include "result.hpp"
#include "window_event.hpp"

namespace aeg::editor {
/** @brief Thread-confined owner of one Dear ImGui context and its editor panels. */
class EditorImGuiHost final {
public:
    EditorImGuiHost();
    ~EditorImGuiHost();

    EditorImGuiHost(const EditorImGuiHost&) = delete;
    EditorImGuiHost& operator=(const EditorImGuiHost&) = delete;
    EditorImGuiHost(EditorImGuiHost&&) = delete;
    EditorImGuiHost& operator=(EditorImGuiHost&&) = delete;

    /** @brief Creates the engine-owned ImGui context and disables implicit settings I/O. */
    [[nodiscard]] foundation::Result initialize(const EditorImGuiDesc& desc = {});
    /** @brief Destroys the ImGui context after the current frame has ended. */
    [[nodiscard]] foundation::Result shutdown();

    /** @brief Queues one engine event for translation at the next frame boundary. */
    [[nodiscard]] foundation::Result submit_event(const platform::WindowEvent& event);
    /** @brief Associates the uploaded font atlas with an engine-managed texture identity. */
    [[nodiscard]] foundation::Result set_font_texture(ImGuiTextureHandle texture);
    /** @brief Copies the engine-owned RGBA8 font atlas for upload through Resource/RHI APIs. */
    [[nodiscard]] foundation::Result capture_font_atlas(ImGuiFontAtlasData& atlas) const;
    /** @brief Rebuilds the CPU font atlas for a registered font before the next frame. */
    [[nodiscard]] foundation::Result set_font(std::string_view font_key, float size_pixels);
    /** @brief Rebuilds the atlas while keeping the active UI font and changing only the preview font. */
    [[nodiscard]] foundation::Result set_preview_font(std::string_view font_key, float size_pixels);
    /** @brief Returns the stable key of the currently selected font. */
    [[nodiscard]] std::string_view active_font_key() const noexcept;
    /** @brief Returns the base pixel size used by the currently selected font. */
    [[nodiscard]] float active_font_size_pixels() const noexcept;
    /** @brief Returns the stable key of the font used by the preview scope. */
    [[nodiscard]] std::string_view preview_font_key() const noexcept;
    /** @brief Returns the base pixel size used by the preview font. */
    [[nodiscard]] float preview_font_size_pixels() const noexcept;
    /** @brief Returns an opaque ImGui font pointer for the current preview scope. */
    [[nodiscard]] const void* preview_font() const noexcept;

    /** @brief Starts one ImGui frame using engine-owned display timing. */
    [[nodiscard]] foundation::Result begin_frame(const EditorImGuiFrameDesc& desc);
    /** @brief Invokes all registered panels during the active ImGui frame. */
    [[nodiscard]] foundation::Result build_panels();
    /** @brief Finalizes the current frame and makes its draw data available internally. */
    [[nodiscard]] foundation::Result end_frame();
    /** @brief Copies the finalized ImGui draw lists into engine-owned storage. */
    [[nodiscard]] foundation::Result capture_draw_packet(ImGuiDrawPacket& packet) const;
    /** @brief Copies every active ImGui viewport into engine-owned draw packets. */
    [[nodiscard]] foundation::Result capture_draw_packets(std::vector<ImGuiDrawPacket>& packets) const;

    /** @brief Takes ownership of one panel and returns its generation-checked identity. */
    [[nodiscard]] foundation::ResultValue<EditorPanelId> register_panel(std::unique_ptr<EditorPanel> panel);
    /** @brief Removes a previously registered panel. */
    [[nodiscard]] foundation::Result unregister_panel(EditorPanelId panel);
    /** @brief Enables or suppresses one registered panel without releasing its ownership. */
    [[nodiscard]] foundation::Result set_panel_visible(EditorPanelId panel, bool visible);

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] bool is_frame_active() const noexcept;
    [[nodiscard]] std::size_t panel_count() const noexcept;
    /** @brief Returns opaque ImDrawData valid until the next begin_frame call. */
    [[nodiscard]] const void* draw_data() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace aeg::editor
