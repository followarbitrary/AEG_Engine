/**
 * @file project_dialog_ui.hpp
 * @brief Small ImGui layout helpers shared by project-dialog rendering and tests.
 */
#pragma once

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace aeg::application {
struct ProjectDialogFieldMetrics final {
    float label_left{0.0f};
    float label_right{0.0f};
    float input_left{0.0f};
    float input_right{0.0f};
};

/** @brief Draws a project field with its label in the leading column. */
inline ProjectDialogFieldMetrics draw_project_dialog_field(std::string_view label, const char* input_id,
                                                           char* buffer, std::size_t buffer_size,
                                                           float input_width = 360.0f,
                                                           float input_column_x = 130.0f) {
    const float row_start_x = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ProjectDialogFieldMetrics metrics{};
    metrics.label_left = ImGui::GetItemRectMin().x;
    metrics.label_right = ImGui::GetItemRectMax().x;

    const float minimum_input_offset = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x +
                                       ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(row_start_x + std::max(input_column_x, minimum_input_offset));
    ImGui::SetNextItemWidth(input_width);
    static_cast<void>(ImGui::InputText(input_id, buffer, buffer_size));
    metrics.input_left = ImGui::GetItemRectMin().x;
    metrics.input_right = ImGui::GetItemRectMax().x;
    return metrics;
}
}  // namespace aeg::application
