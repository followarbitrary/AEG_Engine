/**
 * @file editor_panel.hpp
 * @brief Extension point for editor-owned ImGui panels.
 */
#pragma once

#include "editor_imgui_types.hpp"

namespace aeg::editor {
class EditorPanel {
public:
    virtual ~EditorPanel() = default;

    /** @brief Builds the panel contents while the host owns the active ImGui frame. */
    virtual void build_ui() = 0;
};
}  // namespace aeg::editor
