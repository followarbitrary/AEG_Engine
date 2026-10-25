#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aeg::application {
/** Preferences is opened from the dedicated Settings rail, not the View menu. */
[[nodiscard]] inline constexpr bool preferences_action_shows_in_view_menu() noexcept {
    return false;
}

/** Compact settings dialog, with room for controls and a 1:1 font preview. */
struct PreferenceWindowWorkAreaScale final {
    uint32_t width_percent{48};
    uint32_t height_percent{54};
};

[[nodiscard]] inline constexpr PreferenceWindowWorkAreaScale preference_window_work_area_scale() noexcept {
    return {};
}

inline const char* asset_edit_block_reason(bool project_asset, bool project_loaded, bool edit_mode) {
    if (!project_asset)
        return "Built-in resource (read-only). Import an asset into a game project to edit it.";
    if (!project_loaded)
        return "Open a game project to edit its assets.";
    if (!edit_mode)
        return "Stop Play mode before editing assets.";
    return nullptr;
}
// Scope every asset row, including its popup, by the resource identity.
class AssetRowId final {
public:
    AssetRowId(uint32_t kind, uint64_t id) {
        ImGui::PushID(static_cast<int>(kind));
        ImGui::PushID(std::to_string(id).c_str());
    }
    ~AssetRowId() {
        ImGui::PopID();
        ImGui::PopID();
    }
    AssetRowId(const AssetRowId&) = delete;
    AssetRowId& operator=(const AssetRowId&) = delete;
};

class GraphPanGesture final {
public:
    ImVec2 update(bool hovered, bool empty, bool connecting, bool clicked, bool down, bool dragging, bool cancel,
                  ImVec2 delta) {
        if (cancel || !down) {
            active_ = false;
            return {};
        }
        if (clicked)
            active_ = hovered && empty && !connecting;
        return active_ && dragging ? delta : ImVec2{};
    }

private:
    bool active_{false};
};

inline bool remove_graph_edge(std::vector<std::pair<int, int>>& edges, int from, int to) {
    const auto found = std::find(edges.begin(), edges.end(), std::pair{from, to});
    if (found == edges.end())
        return false;
    edges.erase(found);
    return true;
}

inline bool graph_link_hit(ImVec2 from, ImVec2 to, ImVec2 mouse, float zoom) {
    const float bend = std::max(50.0f * zoom, std::abs(to.x - from.x) * 0.45f);
    const ImVec2 control_a{from.x + bend, from.y};
    const ImVec2 control_b{to.x - bend, to.y};
    const auto point = [&](float t) {
        const float inverse = 1.0f - t;
        return ImVec2{
            inverse * inverse * inverse * from.x + 3.0f * inverse * inverse * t * control_a.x +
                3.0f * inverse * t * t * control_b.x + t * t * t * to.x,
            inverse * inverse * inverse * from.y + 3.0f * inverse * inverse * t * control_a.y +
                3.0f * inverse * t * t * control_b.y + t * t * t * to.y,
        };
    };
    constexpr int segments = 24;
    // A curve is a thin visual target; keep the click target comfortably larger
    // than the rendered stroke so double-click removal is reliable.
    const float hit_radius = std::max(9.0f, 10.0f * zoom);
    ImVec2 previous = from;
    for (int index = 1; index <= segments; ++index) {
        const ImVec2 current = point(static_cast<float>(index) / segments);
        const float dx = current.x - previous.x;
        const float dy = current.y - previous.y;
        const float length_squared = dx * dx + dy * dy;
        const float fraction =
            length_squared > 0.0f
                ? std::clamp(((mouse.x - previous.x) * dx + (mouse.y - previous.y) * dy) / length_squared, 0.0f, 1.0f)
                : 0.0f;
        const float near_x = previous.x + dx * fraction;
        const float near_y = previous.y + dy * fraction;
        const float distance_x = mouse.x - near_x;
        const float distance_y = mouse.y - near_y;
        if (distance_x * distance_x + distance_y * distance_y <= hit_radius * hit_radius)
            return true;
        previous = current;
    }
    return false;
}

}  // namespace aeg::application
