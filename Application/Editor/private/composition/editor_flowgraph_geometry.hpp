#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <string>
#include <string_view>

namespace aeg::application::flowgraph {

struct Point final {
    float x{};
    float y{};
};

struct ColumnWidths final {
    float palette{};
    float canvas{};
    float inspector{};
};

// Shared card metrics keep the hit targets, labels and wire anchors on the
// same rows.  The renderer uses these values for both the visual card and its
// invisible ImGui interaction widgets, so zooming cannot move the pointer
// away from the port it is targeting.
struct NodeLayout final {
    float width{220.0f};
    float header_height{32.0f};
    float row_height{21.0f};
    float padding{10.0f};
    std::size_t input_count{};
    std::size_t output_count{};

    [[nodiscard]] float height() const noexcept {
        // Empty nodes still need a body row for their status text.  Without
        // this minimum the "No declared ports" label is drawn below the card
        // and appears to belong to the canvas.
        const std::size_t rows = std::max<std::size_t>(1u, std::max(input_count, output_count));
        return header_height + padding * 2.0f + static_cast<float>(rows) * row_height;
    }

    [[nodiscard]] Point input_anchor(std::size_t row) const noexcept {
        return {0.0f, header_height + padding + (static_cast<float>(row) + 0.5f) * row_height};
    }

    [[nodiscard]] Point output_anchor(std::size_t row) const noexcept {
        return {width, header_height + padding + (static_cast<float>(row) + 0.5f) * row_height};
    }
};

inline NodeLayout node_layout(std::size_t input_count, std::size_t output_count,
                              float width = 220.0f) noexcept {
    NodeLayout result;
    result.width = std::max(160.0f, width);
    result.input_count = input_count;
    result.output_count = output_count;
    return result;
}

// A resource can use its ResourceId as both its input and output PortId.
// Direction therefore belongs in every interaction and anchor identity.
inline std::string port_anchor_key(std::string_view node_id, bool input, uint64_t port_id,
                                   uint64_t slot_id) {
    return std::string(node_id) + (input ? ":in:" : ":out:") + std::to_string(port_id) + ":" +
           std::to_string(slot_id);
}

inline Point drag_node_top_left(Point mouse) noexcept {
    return mouse;
}

inline Point graph_position_from_mouse(Point mouse, Point origin, float zoom) noexcept {
    const float safe_zoom = std::max(0.001f, zoom);
    return {(mouse.x - origin.x) / safe_zoom, (mouse.y - origin.y) / safe_zoom};
}

// Returns the first visible grid line relative to a fixed content rectangle.
// Keeping this calculation in graph space avoids using a large screen-space
// origin as the phase source, which loses precision after a long canvas pan.
inline float first_grid_line_offset(float scroll, float grid) noexcept {
    if (!(grid > 0.0f) || !std::isfinite(scroll) || !std::isfinite(grid))
        return 0.0f;
    const float remainder = std::fmod(scroll, grid);
    if (remainder == 0.0f)
        return 0.0f;
    return remainder > 0.0f ? grid - remainder : -remainder;
}

// Face labels are intentionally semantic and compact.  The complete authored
// identifier remains available from the hover tooltip and Inspector, while a
// card can reserve separate input/output lanes without text collisions.
inline std::string compact_label(std::string_view value) {
    const auto last_separator = value.find_last_of("./:");
    std::string source{last_separator == std::string_view::npos ? value : value.substr(last_separator + 1)};
    if (source.rfind("Renderer_", 0u) == 0u)
        source.erase(0u, 9u);
    std::string result;
    result.reserve(source.size() + 8u);
    for (std::size_t index = 0; index < source.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(source[index]);
        if (character == '_' || character == '-') {
            if (!result.empty() && result.back() != ' ')
                result.push_back(' ');
            continue;
        }
        const bool uppercase = std::isupper(character) != 0;
        const bool previous_lower = index > 0u && std::islower(static_cast<unsigned char>(source[index - 1])) != 0;
        const bool acronym_boundary = uppercase && index + 1u < source.size() &&
                                      std::islower(static_cast<unsigned char>(source[index + 1])) != 0 &&
                                      index > 0u && std::isupper(static_cast<unsigned char>(source[index - 1])) != 0;
        if (uppercase && (previous_lower || acronym_boundary) && !result.empty() && result.back() != ' ')
            result.push_back(' ');
        result.push_back(static_cast<char>(character));
    }
    return result.empty() ? std::string{"Node"} : result;
}

inline ColumnWidths distribute_columns(float available_width, float palette_width,
                                       float inspector_width) noexcept {
    const float palette = std::clamp(palette_width, 180.0f, 380.0f);
    const float inspector = std::max(240.0f, inspector_width);
    const float canvas = std::max(240.0f, available_width - palette - inspector);
    return {palette, canvas, inspector};
}

}  // namespace aeg::application::flowgraph
