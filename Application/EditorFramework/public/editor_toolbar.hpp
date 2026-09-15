#pragma once
#include <imgui.h>
#include <algorithm>

namespace aeg::editor {
enum class ToolbarIcon { None, Play, Pause, Stop };
inline float toolbar_height() { return std::max(32.0f, ImGui::GetFontSize() + 16.0f); }
inline bool toolbar_button(const char* label, const char* description, ToolbarIcon icon = ToolbarIcon::None,
                           bool active = false, bool dropdown = false) {
    const ImVec2 text = ImGui::CalcTextSize(label);
    const float h = toolbar_height();
    const float icon_space = icon == ToolbarIcon::None ? 0.0f : 22.0f;
    const ImVec2 size{24.0f + text.x + icon_space + (dropdown ? 20.0f : 0.0f), h};
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4{0.075f,0.24f,0.21f,1} : ImVec4{0.055f,0.10f,0.13f,1});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.10f,0.28f,0.25f,1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.12f,0.34f,0.29f,1});
    ImGui::PushStyleColor(ImGuiCol_Border, active ? ImVec4{0.22f,0.53f,0.45f,1} : ImVec4{0.16f,0.25f,0.29f,1});
    const bool pressed = ImGui::Button("##Action", size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(active ? ImVec4{0.65f,0.96f,0.85f,1} : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    const float x = p.x + 12, y = p.y + h * 0.5f;
    if (icon == ToolbarIcon::Play) draw->AddTriangleFilled({x,y-6},{x,y+6},{x+10,y},color);
    if (icon == ToolbarIcon::Stop) draw->AddRectFilled({x,y-5},{x+10,y+5},color,1);
    if (icon == ToolbarIcon::Pause) {
        draw->AddRectFilled({x,y-6},{x+3,y+6},color,1);
        draw->AddRectFilled({x+7,y-6},{x+10,y+6},color,1);
    }
    draw->AddText({x+icon_space,p.y+(h-text.y)*0.5f},color,label);
    if (dropdown) {
        const float cx = p.x+size.x-14;
        draw->AddLine({cx-4,y-2},{cx,y+2},color,1.3f);
        draw->AddLine({cx,y+2},{cx+4,y-2},color,1.3f);
    }
    if (description && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", description);
    ImGui::PopID();
    return pressed;
}
}
