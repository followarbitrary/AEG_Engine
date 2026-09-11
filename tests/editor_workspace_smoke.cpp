/**
 * @file editor_workspace_smoke.cpp
 * @brief Verifies the customizable editor workspace orchestration.
 */
#include <imgui.h>

#include <cassert>
#include <memory>
#include <string>

#include "editor_imgui.hpp"
#include "editor_workspace.hpp"

namespace {
class TestPanel final : public aeg::editor::EditorPanel {
public:
    explicit TestPanel(uint32_t& build_count) noexcept : build_count_(build_count) {}

    void build_ui() override {
        ImGui::Begin("Workspace Test");
        ImGui::TextUnformatted("AEG");
        ImGui::End();
        ++build_count_;
    }

private:
    uint32_t& build_count_;
};
}  // namespace

int main() {
    aeg::editor::EditorImGuiHost host;
    assert(host.initialize());

    aeg::editor::EditorWorkspace workspace(host);
    uint32_t build_count = 0;
    assert(workspace.register_panel(
        aeg::editor::EditorPanelDescriptor{"scene_viewport", "Workspace Test", aeg::editor::EditorDockRegion::kCenter,
                                           true, true, true},
        std::make_unique<TestPanel>(build_count)));
    assert(workspace.panel_count() == 1);
    assert(workspace.layout() != nullptr);

    const aeg::platform::WindowId window{0, 1};
    aeg::editor::EditorImGuiFrameDesc frame_desc{};
    frame_desc.window = window;
    frame_desc.display_extent = {1280, 720};
    assert(host.begin_frame(frame_desc));
    assert(workspace.build_ui());
    assert(host.end_frame());
    assert(build_count == 1);

    assert(workspace.set_panel_visible("scene_viewport", false));
    assert(host.begin_frame(frame_desc));
    assert(workspace.build_ui());
    assert(host.end_frame());
    assert(build_count == 1);

    std::string serialized;
    assert(workspace.serialize_layout(serialized));
    assert(workspace.deserialize_layout(serialized));
    assert(host.shutdown());
    return 0;
}
