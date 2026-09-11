/**
 * @file editor_layout.cpp
 * @brief Implements the backend-neutral editor layout model.
 */
#include "editor_layout.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <utility>

namespace aeg::editor {
namespace {
[[nodiscard]] bool is_valid_region(uint32_t value) noexcept {
    return value <= static_cast<uint32_t>(EditorDockRegion::kBottom);
}

[[nodiscard]] bool is_binary_flag(uint32_t value) noexcept {
    return value <= 1u;
}

[[nodiscard]] foundation::Result invalid_layout(std::string message) {
    return foundation::invalid_argument(std::move(message));
}
}  // namespace

foundation::ResultValue<EditorLayoutModel> EditorLayoutModel::create_default(
    std::span<const EditorPanelDescriptor> descriptors) {
    EditorLayoutModel model;
    try {
        for (const EditorPanelDescriptor& descriptor : descriptors) {
            if (const foundation::Result result = model.register_panel(descriptor); !result) {
                return {result, {}};
            }
        }
        return {foundation::Result::success(), std::move(model)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Editor default layout allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

foundation::ResultValue<EditorLayoutModel> EditorLayoutModel::deserialize(std::string_view serialized) {
    if (serialized.empty()) {
        return {invalid_layout("Editor layout serialization is empty"), {}};
    }

    try {
        std::istringstream stream{std::string{serialized}};
        std::string magic;
        uint32_t version = 0;
        if (!(stream >> magic >> version) || magic != "AEG_LAYOUT" || version != kCurrentVersion) {
            return {invalid_layout("Editor layout header or version is invalid"), {}};
        }

        std::string count_tag;
        uint64_t panel_count = 0;
        if (!(stream >> count_tag >> panel_count) || count_tag != "COUNT" ||
            panel_count > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return {invalid_layout("Editor layout panel count is invalid"), {}};
        }

        EditorLayoutModel model;
        model.version_ = version;
        model.panels_.reserve(static_cast<std::size_t>(panel_count));
        for (uint64_t index = 0; index < panel_count; ++index) {
            std::string tag;
            std::string panel_key;
            uint32_t region = 0;
            uint32_t visible = 0;
            uint32_t floating = 0;
            uint32_t closable = 0;
            uint32_t restorable = 0;
            uint64_t tab_order = 0;
            uint64_t window_index = 0;
            uint64_t window_generation = 0;
            int64_t position_x = 0;
            int64_t position_y = 0;
            uint64_t extent_width = 0;
            uint64_t extent_height = 0;
            if (!(stream >> tag >> std::quoted(panel_key) >> region >> visible >> floating >> closable >> restorable >>
                  tab_order >> window_index >> window_generation >> position_x >> position_y >> extent_width >>
                  extent_height) ||
                tag != "PANEL" || panel_key.empty() || !is_valid_region(region) || !is_binary_flag(visible) ||
                !is_binary_flag(floating) || !is_binary_flag(closable) || !is_binary_flag(restorable) ||
                tab_order > std::numeric_limits<uint32_t>::max() ||
                window_index > std::numeric_limits<uint32_t>::max() ||
                window_generation > std::numeric_limits<uint32_t>::max() ||
                position_x < std::numeric_limits<int32_t>::min() ||
                position_x > std::numeric_limits<int32_t>::max() || position_y < std::numeric_limits<int32_t>::min() ||
                position_y > std::numeric_limits<int32_t>::max() || extent_width > std::numeric_limits<uint32_t>::max() ||
                extent_height > std::numeric_limits<uint32_t>::max()) {
                return {invalid_layout("Editor layout panel record is invalid"), {}};
            }
            if (model.find_panel(panel_key) != nullptr) {
                return {invalid_layout("Editor layout contains duplicate panel keys"), {}};
            }

            EditorPanelLayoutState state{};
            state.panel_key = std::move(panel_key);
            state.region = static_cast<EditorDockRegion>(region);
            state.visible = visible != 0;
            state.floating = floating != 0;
            state.closable = closable != 0;
            state.restorable = restorable != 0;
            state.tab_order = static_cast<uint32_t>(tab_order);
            state.floating_window = platform::WindowId{static_cast<uint32_t>(window_index),
                                                       static_cast<uint32_t>(window_generation)};
            state.floating_position = platform::WindowPosition{static_cast<int32_t>(position_x),
                                                               static_cast<int32_t>(position_y)};
            state.floating_extent = platform::WindowExtent{static_cast<uint32_t>(extent_width),
                                                           static_cast<uint32_t>(extent_height)};
            if (state.floating && (!state.floating_window.is_valid() || !state.floating_extent.is_valid())) {
                return {invalid_layout("Editor floating panel state is incomplete"), {}};
            }
            if (!state.floating && state.floating_window.is_valid()) {
                return {invalid_layout("Docked panel cannot retain a floating window identity"), {}};
            }
            model.panels_.push_back(std::move(state));
        }

        stream >> std::ws;
        if (!stream.eof()) {
            return {invalid_layout("Editor layout contains trailing data"), {}};
        }
        return {foundation::Result::success(), std::move(model)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Editor layout deserialization allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

foundation::Result EditorLayoutModel::register_panel(const EditorPanelDescriptor& descriptor) {
    if (const foundation::Result result = validate_panel_descriptor(descriptor); !result) {
        return result;
    }
    if (find_panel(descriptor.key) != nullptr) {
        return foundation::invalid_argument("Editor panel key is already registered");
    }

    try {
        EditorPanelLayoutState state{};
        state.panel_key = descriptor.key;
        state.region = descriptor.default_region;
        state.visible = descriptor.visible_by_default;
        state.closable = descriptor.closable;
        state.restorable = descriptor.restorable;
        state.tab_order = static_cast<uint32_t>(next_tab_order(descriptor.default_region));
        panels_.push_back(std::move(state));
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor panel layout allocation failed");
    }
}

foundation::Result EditorLayoutModel::unregister_panel(std::string_view panel_key) {
    const auto iterator = std::find_if(panels_.begin(), panels_.end(), [panel_key](const auto& panel) {
        return panel.panel_key == panel_key;
    });
    if (iterator == panels_.end()) {
        return foundation::not_found("Editor panel key is not registered");
    }
    panels_.erase(iterator);
    return foundation::Result::success();
}

foundation::Result EditorLayoutModel::set_panel_region(std::string_view panel_key, EditorDockRegion region) {
    EditorPanelLayoutState* panel = const_cast<EditorPanelLayoutState*>(find_panel(panel_key));
    if (panel == nullptr) {
        return foundation::not_found("Editor panel key is not registered");
    }
    panel->region = region;
    panel->tab_order = static_cast<uint32_t>(next_tab_order(region));
    return foundation::Result::success();
}

foundation::Result EditorLayoutModel::set_panel_visible(std::string_view panel_key, bool visible) {
    EditorPanelLayoutState* panel = const_cast<EditorPanelLayoutState*>(find_panel(panel_key));
    if (panel == nullptr) {
        return foundation::not_found("Editor panel key is not registered");
    }
    panel->visible = visible;
    return foundation::Result::success();
}

foundation::Result EditorLayoutModel::detach_panel(std::string_view panel_key, platform::WindowId window,
                                                   platform::WindowPosition position, platform::WindowExtent extent) {
    EditorPanelLayoutState* panel = const_cast<EditorPanelLayoutState*>(find_panel(panel_key));
    if (panel == nullptr) {
        return foundation::not_found("Editor panel key is not registered");
    }
    if (!window.is_valid() || !extent.is_valid()) {
        return foundation::invalid_argument("Editor floating panel window state is invalid");
    }
    if (panel->floating) {
        return foundation::invalid_state("Editor panel is already floating");
    }
    panel->floating = true;
    panel->visible = true;
    panel->floating_window = window;
    panel->floating_position = position;
    panel->floating_extent = extent;
    return foundation::Result::success();
}

foundation::Result EditorLayoutModel::redock_panel(std::string_view panel_key, platform::WindowId window) {
    EditorPanelLayoutState* panel = const_cast<EditorPanelLayoutState*>(find_panel(panel_key));
    if (panel == nullptr) {
        return foundation::not_found("Editor panel key is not registered");
    }
    if (!panel->floating || panel->floating_window != window) {
        return foundation::not_found("Editor floating panel window identity is stale");
    }
    panel->floating = false;
    panel->floating_window = {};
    panel->floating_position = {};
    panel->floating_extent = {};
    panel->visible = true;
    panel->tab_order = static_cast<uint32_t>(next_tab_order(panel->region));
    return foundation::Result::success();
}

foundation::Result EditorLayoutModel::serialize(std::string& serialized) const {
    try {
        std::ostringstream stream;
        stream << "AEG_LAYOUT " << version_ << '\n';
        stream << "COUNT " << panels_.size() << '\n';
        for (const EditorPanelLayoutState& panel : panels_) {
            if (panel.panel_key.empty() || (panel.floating && (!panel.floating_window.is_valid() ||
                                                               !panel.floating_extent.is_valid())) ||
                (!panel.floating && panel.floating_window.is_valid())) {
                return foundation::invalid_state("Editor layout contains invalid panel state");
            }
            stream << "PANEL " << std::quoted(panel.panel_key) << ' ' << static_cast<uint32_t>(panel.region) << ' '
                   << (panel.visible ? 1 : 0) << ' ' << (panel.floating ? 1 : 0) << ' ' << (panel.closable ? 1 : 0)
                   << ' ' << (panel.restorable ? 1 : 0) << ' ' << panel.tab_order << ' '
                   << panel.floating_window.index << ' ' << panel.floating_window.generation << ' '
                   << panel.floating_position.x << ' ' << panel.floating_position.y << ' '
                   << panel.floating_extent.width << ' ' << panel.floating_extent.height << '\n';
        }
        serialized = stream.str();
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor layout serialization allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

const EditorPanelLayoutState* EditorLayoutModel::find_panel(std::string_view panel_key) const noexcept {
    const auto iterator = std::find_if(panels_.begin(), panels_.end(), [panel_key](const auto& panel) {
        return panel.panel_key == panel_key;
    });
    return iterator == panels_.end() ? nullptr : &*iterator;
}

std::size_t EditorLayoutModel::next_tab_order(EditorDockRegion region) const noexcept {
    std::size_t next = 0;
    for (const EditorPanelLayoutState& panel : panels_) {
        if (!panel.floating && panel.region == region) {
            next = std::max(next, static_cast<std::size_t>(panel.tab_order) + 1u);
        }
    }
    return next;
}

foundation::Result EditorLayoutModel::validate_panel_descriptor(const EditorPanelDescriptor& descriptor) const {
    if (!descriptor.is_valid()) {
        return foundation::invalid_argument("Editor panel descriptor is incomplete");
    }
    return foundation::Result::success();
}
}  // namespace aeg::editor
