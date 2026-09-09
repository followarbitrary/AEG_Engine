#include <cassert>
#include <memory>

#include "editor_imgui.hpp"
#include "imgui_texture_registry.hpp"

namespace {
class SmokePanel final : public aeg::editor::EditorPanel {
public:
    void build_ui() override { ++build_count; }

    uint32_t build_count{0};
};
}  // namespace

int main() {
    using namespace aeg;

    editor::EditorImGuiHost host;
    assert(host.initialize());
    auto panel = std::make_unique<SmokePanel>();
    SmokePanel* panel_pointer = panel.get();
    const foundation::ResultValue<editor::EditorPanelId> panel_result = host.register_panel(std::move(panel));
    assert(panel_result);
    assert(host.panel_count() == 1);

    const platform::WindowId window{0, 1};
    platform::WindowEvent key_event{};
    key_event.type = platform::WindowEventType::kKey;
    key_event.window = window;
    key_event.key = platform::KeyCode::kA;
    key_event.key_action = platform::KeyAction::kPressed;
    assert(host.submit_event(key_event));

    editor::EditorImGuiFrameDesc frame_desc{};
    frame_desc.window = window;
    frame_desc.display_extent = platform::WindowExtent{1280, 720};
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(host.end_frame());
    assert(panel_pointer->build_count == 1);
    assert(host.draw_data() != nullptr);

    assert(host.unregister_panel(panel_result.value));
    assert(host.panel_count() == 0);
    assert(host.shutdown());

    editor::ImGuiTextureRegistry textures;
    const rhi::TextureViewHandle view{11};
    const rhi::SamplerHandle sampler{17};
    const foundation::ResultValue<editor::ImGuiTextureHandle> texture_result =
        textures.register_texture(editor::ImGuiTextureBinding{view, sampler});
    assert(texture_result);
    const uint64_t texture_id = editor::make_imgui_texture_id(texture_result.value);
    assert(editor::decode_imgui_texture_id(texture_id) == texture_result.value);
    assert(textures.resolve(texture_result.value));
    assert(textures.update_texture(texture_result.value,
                                   editor::ImGuiTextureBinding{rhi::TextureViewHandle{23}, rhi::SamplerHandle{29}}));
    assert(textures.unregister_texture(texture_result.value));
    assert(!textures.resolve(texture_result.value));
    return 0;
}
