/**
 * @file imgui_draw_packet.hpp
 * @brief Engine-owned copy of one Dear ImGui frame's draw data.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "editor_imgui_types.hpp"

namespace aeg::editor {
struct ImGuiDrawVertex final {
    float position_x{0.0f};
    float position_y{0.0f};
    float uv_x{0.0f};
    float uv_y{0.0f};
    uint32_t color{0};
};

struct ImGuiDrawCommand final {
    ImGuiTextureHandle texture{};
    float clip_min_x{0.0f};
    float clip_min_y{0.0f};
    float clip_max_x{0.0f};
    float clip_max_y{0.0f};
    uint32_t elem_count{0};
    uint32_t index_offset{0};
    int32_t vertex_offset{0};
};

struct ImGuiDrawPacket final {
    float display_pos_x{0.0f};
    float display_pos_y{0.0f};
    float display_width{0.0f};
    float display_height{0.0f};
    float framebuffer_scale_x{1.0f};
    float framebuffer_scale_y{1.0f};
    uint64_t frame_number{0};
    std::vector<ImGuiDrawVertex> vertices{};
    std::vector<uint32_t> indices{};
    std::vector<ImGuiDrawCommand> commands{};

    [[nodiscard]] bool empty() const noexcept { return commands.empty(); }
    void clear() noexcept {
        vertices.clear();
        indices.clear();
        commands.clear();
        frame_number = 0;
    }
};
}  // namespace aeg::editor
