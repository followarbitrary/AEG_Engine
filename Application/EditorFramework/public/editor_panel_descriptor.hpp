/**
 * @file editor_panel_descriptor.hpp
 * @brief Stable semantic metadata for one editor panel.
 */
#pragma once

#include <cstdint>
#include <string>

namespace aeg::editor {
/** @brief Default semantic region used by the resource-production workspace. */
enum class EditorDockRegion : uint8_t {
    kCenter,
    kLeft,
    kRight,
    kBottom,
};

/** @brief Stable metadata used to register a panel with the editor workspace. */
struct EditorPanelDescriptor final {
    /** @brief Stable machine-readable panel key used by layout persistence. */
    std::string key{};
    /** @brief User-visible panel title. */
    std::string title{};
    /** @brief Region used by the built-in default layout. */
    EditorDockRegion default_region{EditorDockRegion::kCenter};
    /** @brief Whether the panel is visible in a newly created workspace. */
    bool visible_by_default{true};
    /** @brief Whether the user may close the panel. */
    bool closable{true};
    /** @brief Whether the panel may be restored by layout recovery. */
    bool restorable{true};

    bool visible{true};

    /** @brief Returns whether the descriptor contains the required stable metadata. */
    [[nodiscard]] bool is_valid() const noexcept { return !key.empty() && !title.empty(); }
};
}  // namespace aeg::editor
