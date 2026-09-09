/**
 * @file editor_imgui.hpp
 * @brief Engine-owned Dear ImGui lifecycle and panel host.
 */
#pragma once

#include <cstddef>
#include <memory>

#include "editor_imgui_types.hpp"
#include "editor_panel.hpp"
#include "result.hpp"
#include "window_event.hpp"

namespace aeg::editor {
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

    /** @brief Starts one ImGui frame using engine-owned display timing. */
    [[nodiscard]] foundation::Result begin_frame(const EditorImGuiFrameDesc& desc);
    /** @brief Invokes all registered panels during the active ImGui frame. */
    [[nodiscard]] foundation::Result build_panels();
    /** @brief Finalizes the current frame and makes its draw data available internally. */
    [[nodiscard]] foundation::Result end_frame();

    /** @brief Takes ownership of one panel and returns its generation-checked identity. */
    [[nodiscard]] foundation::ResultValue<EditorPanelId> register_panel(std::unique_ptr<EditorPanel> panel);
    /** @brief Removes a previously registered panel. */
    [[nodiscard]] foundation::Result unregister_panel(EditorPanelId panel);

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
