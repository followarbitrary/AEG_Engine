#pragma once

#include <imgui.h>

#include <cstdint>

namespace aeg::application {
struct HierarchyTreeLayout final {
    ImVec2 item_spacing{7.0f, 5.0f};
    ImVec2 frame_padding{7.0f, 4.0f};
    float drop_zone_height{3.0f};
    float footer_gap{8.0f};
};

inline HierarchyTreeLayout compact_hierarchy_tree_layout() {
    return {{4.0f, 1.0f}, {4.0f, 2.0f}, 1.0f, 4.0f};
}

/** @brief Pushes all four 32-bit words of a stable 128-bit hierarchy identity. */
inline void push_hierarchy_item_id(uint64_t high, uint64_t low) {
    ImGui::PushID(static_cast<int>(static_cast<uint32_t>(high >> 32u)));
    ImGui::PushID(static_cast<int>(static_cast<uint32_t>(high)));
    ImGui::PushID(static_cast<int>(static_cast<uint32_t>(low >> 32u)));
    ImGui::PushID(static_cast<int>(static_cast<uint32_t>(low)));
}

inline void pop_hierarchy_item_id() {
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}
}  // namespace aeg::application
