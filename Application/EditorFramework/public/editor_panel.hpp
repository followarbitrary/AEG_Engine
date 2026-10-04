/**
 * @file editor_panel.hpp
 * @brief Extension point for editor-owned ImGui panels.
 */
#pragma once

#include "editor_window_chrome.hpp"
#include "editor_imgui_types.hpp"

namespace aeg::editor {
struct EditorPanelBuildContext final {
    /** @brief True when the panel is rendered in an independent native window. */
    bool native_window{false};
    /** @brief Native operations for the independent window's title bar. */
    EditorWindowChromeActions window_actions{};
};

class EditorPanel {
public:
    virtual ~EditorPanel() = default;

    /** @brief Builds the panel contents while the host owns the active ImGui frame. */
    virtual void build_ui() = 0;
    /** @brief Builds the panel with native-window context when it is detached. */
    virtual void build_ui(const EditorPanelBuildContext& context) {
        static_cast<void>(context);
        build_ui();
    }
};
}  // namespace aeg::editor
