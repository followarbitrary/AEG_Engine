/**
 * @file editor_layout.hpp
 * @brief Serializable semantic state for the customizable editor workspace.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor_panel_descriptor.hpp"
#include "result.hpp"
#include "window_types.hpp"

namespace aeg::editor {
/** @brief Serializable state for one registered editor panel. */
struct EditorPanelLayoutState final {
    /** @brief Stable panel key. */
    std::string panel_key{};
    /** @brief Current dock region when the panel is not floating. */
    EditorDockRegion region{EditorDockRegion::kCenter};
    /** @brief Whether the panel is currently visible. */
    bool visible{true};
    /** @brief Whether the panel is hosted by a floating platform window. */
    bool floating{false};
    /** @brief Whether the panel can be closed according to its descriptor. */
    bool closable{true};
    /** @brief Whether the panel can be restored according to its descriptor. */
    bool restorable{true};
    /** @brief Stable order inside the region's tab stack. */
    uint32_t tab_order{0};
    /** @brief Platform identity of the floating window when floating is true. */
    platform::WindowId floating_window{};
    /** @brief Saved screen-space position of the floating window. */
    platform::WindowPosition floating_position{};
    /** @brief Saved client-area extent of the floating window. */
    platform::WindowExtent floating_extent{};
};

/** @brief Thread-confined, backend-neutral editor layout model. */
class EditorLayoutModel final {
public:
    /** @brief Current serialized layout format version. */
    static constexpr uint32_t kCurrentVersion = 1;

    /** @brief Builds the default resource-production layout from panel descriptors. */
    [[nodiscard]] static foundation::ResultValue<EditorLayoutModel> create_default(
        std::span<const EditorPanelDescriptor> descriptors);
    /** @brief Parses a versioned serialized layout without applying filesystem policy. */
    [[nodiscard]] static foundation::ResultValue<EditorLayoutModel> deserialize(std::string_view serialized);

    /** @brief Registers one panel key without partially changing existing layout state. */
    [[nodiscard]] foundation::Result register_panel(const EditorPanelDescriptor& descriptor);
    /** @brief Removes one panel and its floating-window state. */
    [[nodiscard]] foundation::Result unregister_panel(std::string_view panel_key);
    /** @brief Moves a docked panel to another semantic region. */
    [[nodiscard]] foundation::Result set_panel_region(std::string_view panel_key, EditorDockRegion region);
    /** @brief Changes panel visibility while preserving its placement. */
    [[nodiscard]] foundation::Result set_panel_visible(std::string_view panel_key, bool visible);
    /** @brief Detaches a panel into an engine-owned floating window identity. */
    [[nodiscard]] foundation::Result detach_panel(std::string_view panel_key, platform::WindowId window,
                                                  platform::WindowPosition position, platform::WindowExtent extent);
    /** @brief Re-docks a panel only when the supplied floating identity is still current. */
    [[nodiscard]] foundation::Result redock_panel(std::string_view panel_key, platform::WindowId window);
    /** @brief Serializes layout state deterministically for an Application-owned store. */
    [[nodiscard]] foundation::Result serialize(std::string& serialized) const;

    /** @brief Finds a panel state without transferring ownership. */
    [[nodiscard]] const EditorPanelLayoutState* find_panel(std::string_view panel_key) const noexcept;
    /** @brief Returns the number of registered panel states. */
    [[nodiscard]] std::size_t panel_count() const noexcept { return panels_.size(); }
    /** @brief Returns the serialized layout version represented by this model. */
    [[nodiscard]] uint32_t version() const noexcept { return version_; }

private:
    [[nodiscard]] std::size_t next_tab_order(EditorDockRegion region) const noexcept;
    [[nodiscard]] foundation::Result validate_panel_descriptor(const EditorPanelDescriptor& descriptor) const;

    uint32_t version_{kCurrentVersion};
    std::vector<EditorPanelLayoutState> panels_{};
};
}  // namespace aeg::editor
