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
    /** @brief Vertex position in the viewport's logical display space. */
    float position_x{0.0f};
    /** @brief Vertex position in the viewport's logical display space. */
    float position_y{0.0f};
    /** @brief Vertex texture coordinate on the horizontal axis. */
    float uv_x{0.0f};
    /** @brief Vertex texture coordinate on the vertical axis. */
    float uv_y{0.0f};
    /** @brief Packed RGBA8 vertex color produced by Dear ImGui. */
    uint32_t color{0};
};

struct ImGuiDrawCommand final {
    /** @brief Generation-checked engine texture identity used by the command. */
    ImGuiTextureHandle texture{};
    /** @brief Minimum clip rectangle coordinate in display space. */
    float clip_min_x{0.0f};
    /** @brief Minimum clip rectangle coordinate in display space. */
    float clip_min_y{0.0f};
    /** @brief Maximum clip rectangle coordinate in display space. */
    float clip_max_x{0.0f};
    /** @brief Maximum clip rectangle coordinate in display space. */
    float clip_max_y{0.0f};
    /** @brief Number of indices emitted by the draw command. */
    uint32_t elem_count{0};
    /** @brief Index-buffer offset for the draw command. */
    uint32_t index_offset{0};
    /** @brief Vertex-buffer offset for the draw command. */
    int32_t vertex_offset{0};
};

struct ImGuiDrawPacket final {
    /** @brief Semantic platform window that owns this viewport's draw data. */
    platform::WindowId window{};
    /** @brief Display-space origin supplied by Dear ImGui. */
    float display_pos_x{0.0f};
    /** @brief Display-space origin supplied by Dear ImGui. */
    float display_pos_y{0.0f};
    /** @brief Logical display width of the viewport. */
    float display_width{0.0f};
    /** @brief Logical display height of the viewport. */
    float display_height{0.0f};
    /** @brief Horizontal framebuffer density of the viewport. */
    float framebuffer_scale_x{1.0f};
    /** @brief Vertical framebuffer density of the viewport. */
    float framebuffer_scale_y{1.0f};
    /** @brief Monotonic editor frame number that produced the packet. */
    uint64_t frame_number{0};
    /** @brief Engine-owned copy of Dear ImGui vertices. */
    std::vector<ImGuiDrawVertex> vertices{};
    /** @brief Engine-owned copy of Dear ImGui indices. */
    std::vector<uint32_t> indices{};
    /** @brief Engine-owned copy of Dear ImGui draw commands. */
    std::vector<ImGuiDrawCommand> commands{};

    [[nodiscard]] bool empty() const noexcept { return commands.empty(); }
    void clear() noexcept {
        window = {};
        vertices.clear();
        indices.clear();
        commands.clear();
        frame_number = 0;
    }
};
}  // namespace aeg::editor
