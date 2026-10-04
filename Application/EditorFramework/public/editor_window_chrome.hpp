/**
 * @file editor_window_chrome.hpp
 * @brief Shared client-drawn title bar for independent editor windows.
 */
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>

#include <imgui.h>

#include "result.hpp"

namespace aeg::editor {

/** @brief Consistent inset between an independent window's client border and its content. */
inline constexpr float kEditorWindowContentPadding = 12.0f;

/** @brief Operations exposed by the native window hosting one editor panel. */
struct EditorWindowChromeActions final {
    std::function<foundation::Result()> minimize{};
    std::function<foundation::Result()> maximize{};
    std::function<foundation::Result()> close{};
};

/**
 * @brief Draws the common title bar used by every independent editor window.
 *
 * The caller owns the surrounding ImGui window and invokes this immediately
 * after Begin().  The title is always left aligned, optional status text is
 * centered, and the three native window controls are always right aligned.
 */
inline void draw_editor_window_chrome(std::string_view title, std::string_view status,
                                      const EditorWindowChromeActions& actions) {
    constexpr float kHeaderHeight = 32.0f;
    constexpr ImGuiWindowFlags kHeaderFlags = ImGuiWindowFlags_NoScrollbar |
                                               ImGuiWindowFlags_NoScrollWithMouse |
                                               ImGuiWindowFlags_NoSavedSettings;

    const ImVec2 parent_window_pos = ImGui::GetWindowPos();
    const float parent_window_width = ImGui::GetWindowWidth();
    // The chrome spans the whole client area. The content cursor is restored
    // below it with the shared window inset after the child is closed.
    ImGui::SetCursorScreenPos(parent_window_pos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{8.0f, 4.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.025f, 0.055f, 0.070f, 1.0f});
    if (ImGui::BeginChild("##editor-window-chrome", ImVec2{parent_window_width, kHeaderHeight}, false, kHeaderFlags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 header_min = ImGui::GetWindowPos();
        const float header_width = ImGui::GetWindowWidth();
        const ImVec2 header_max{header_min.x + header_width, header_min.y + kHeaderHeight};
        draw->AddRectFilled(header_min, header_max, IM_COL32(5, 20, 28, 255));
        draw->AddLine(ImVec2{header_min.x, header_max.y - 1.0f}, ImVec2{header_max.x, header_max.y - 1.0f},
                      IM_COL32(27, 55, 67, 255), 1.0f);

        // Keep title and status text optically centered with a stable top
        // breathing space even when the native window is maximized against
        // the monitor work-area edge.
        const float content_y = header_min.y + 8.0f;
        ImGui::SetCursorScreenPos(ImVec2{header_min.x + 8.0f, content_y});
        ImGui::TextUnformatted(title.data(), title.data() + title.size());

        const float controls_width = 36.0f * 3.0f;
        // Window controls belong to the native title-bar edge. They are
        // intentionally exempt from the shared content inset and touch the
        // right border so the hit targets remain predictable at every size.
        const float controls_start = header_min.x + header_width - controls_width;
        if (!status.empty()) {
            const float status_width = ImGui::CalcTextSize(std::string{status}.c_str()).x;
            const float title_end = header_min.x + 8.0f + ImGui::CalcTextSize(std::string{title}.c_str()).x;
            const float status_min = title_end + 24.0f;
            const float status_max = std::max(status_min, controls_start - status_width - 16.0f);
            const float status_start = std::clamp(header_min.x + (header_width - status_width) * 0.5f,
                                                  status_min, status_max);
            ImGui::SetCursorScreenPos(ImVec2{status_start, content_y});
            ImGui::TextDisabled("%.*s", static_cast<int>(status.size()), status.data());
        }

        const auto title_control = [&](const char* id, int kind,
                                       const std::function<foundation::Result()>& callback) {
            constexpr ImVec2 size{36.0f, 32.0f};
            const int index = kind;
            const ImVec2 position{controls_start + static_cast<float>(index) * size.x, header_min.y};
            ImGui::SetCursorScreenPos(position);
            ImGui::InvisibleButton(id, size);
            const bool hovered = ImGui::IsItemHovered();
            const ImU32 background = hovered
                                         ? (kind == 2 ? IM_COL32(174, 54, 58, 255) : IM_COL32(20, 58, 66, 255))
                                         : IM_COL32(4, 25, 32, 255);
            const ImU32 foreground = IM_COL32(174, 207, 211, 255);
            draw->AddRectFilled(position, ImVec2{position.x + size.x, position.y + size.y}, background, 3.0f);
            const ImVec2 center{position.x + size.x * 0.5f, position.y + size.y * 0.5f};
            if (kind == 0) {
                draw->AddLine(ImVec2{center.x - 5.0f, center.y}, ImVec2{center.x + 5.0f, center.y}, foreground,
                              1.2f);
            } else if (kind == 1) {
                draw->AddRect(ImVec2{center.x - 4.0f, center.y - 4.0f}, ImVec2{center.x + 4.0f, center.y + 4.0f},
                              foreground, 0.0f, 0, 1.1f);
            } else {
                draw->AddLine(ImVec2{center.x - 4.0f, center.y - 4.0f},
                              ImVec2{center.x + 4.0f, center.y + 4.0f}, foreground, 1.2f);
                draw->AddLine(ImVec2{center.x + 4.0f, center.y - 4.0f},
                              ImVec2{center.x - 4.0f, center.y + 4.0f}, foreground, 1.2f);
            }
            if (ImGui::IsItemClicked() && callback)
                static_cast<void>(callback());
        };

        title_control("##editor-window-minimize", 0, actions.minimize);
        title_control("##editor-window-maximize", 1, actions.maximize);
        title_control("##editor-window-close", 2, actions.close);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2{parent_window_pos.x + kEditorWindowContentPadding,
                                    parent_window_pos.y + kHeaderHeight + kEditorWindowContentPadding});
}

}  // namespace aeg::editor
