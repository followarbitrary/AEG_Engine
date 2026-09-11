/**
 * @file editor_layout_smoke.cpp
 * @brief Verifies the semantic editor layout model.
 */
#include <cassert>
#include <span>
#include <string>
#include <vector>

#include "editor_layout.hpp"
#include "editor_panel_descriptor.hpp"

int main() {
    using aeg::editor::EditorDockRegion;
    using aeg::editor::EditorLayoutModel;
    using aeg::editor::EditorPanelDescriptor;
    using aeg::foundation::ResultCode;

    const std::vector<EditorPanelDescriptor> descriptors{
        EditorPanelDescriptor{"scene_viewport", "Scene Viewport", EditorDockRegion::kCenter, true, true, true},
        EditorPanelDescriptor{"hierarchy", "Hierarchy", EditorDockRegion::kLeft, true, true, true},
        EditorPanelDescriptor{"inspector", "Inspector", EditorDockRegion::kRight, true, true, true},
        EditorPanelDescriptor{"assets", "Assets", EditorDockRegion::kBottom, true, true, true},
        EditorPanelDescriptor{"console", "Console", EditorDockRegion::kBottom, true, true, true},
    };

    const auto layout_result = EditorLayoutModel::create_default(std::span<const EditorPanelDescriptor>{descriptors});
    assert(layout_result);
    EditorLayoutModel layout = layout_result.value;
    assert(layout.panel_count() == descriptors.size());
    assert(layout.find_panel("scene_viewport")->region == EditorDockRegion::kCenter);
    assert(layout.find_panel("scene_viewport")->visible);
    assert(layout.find_panel("console")->tab_order == 1);

    assert(!layout.register_panel(descriptors.front()));
    assert(layout.register_panel(EditorPanelDescriptor{"log", "Log", EditorDockRegion::kBottom, true, true, true}));
    assert(layout.set_panel_region("log", EditorDockRegion::kRight));
    assert(layout.set_panel_visible("log", false));
    assert(!layout.set_panel_region("missing", EditorDockRegion::kLeft));

    const aeg::platform::WindowId floating_window{7, 3};
    assert(layout.detach_panel("inspector", floating_window, aeg::platform::WindowPosition{40, 60},
                               aeg::platform::WindowExtent{640, 480}));
    const auto* floating_panel = layout.find_panel("inspector");
    assert(floating_panel != nullptr);
    assert(floating_panel->floating);
    assert(floating_panel->floating_window == floating_window);
    assert(layout.redock_panel("inspector", floating_window));
    assert(!layout.redock_panel("inspector", floating_window));

    std::string serialized;
    assert(layout.serialize(serialized));
    const auto round_trip_result = EditorLayoutModel::deserialize(serialized);
    assert(round_trip_result);
    std::string round_trip_serialized;
    assert(round_trip_result.value.serialize(round_trip_serialized));
    assert(serialized == round_trip_serialized);

    const auto malformed_result = EditorLayoutModel::deserialize("AEG_LAYOUT 999\n");
    assert(!malformed_result);
    assert(malformed_result.result.code == ResultCode::kInvalidArgument);
    return 0;
}
