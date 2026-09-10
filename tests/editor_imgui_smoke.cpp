#include <imgui.h>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>

#include "editor_imgui.hpp"
#include "imgui_texture_registry.hpp"

namespace {
class TestPlatformServices final : public aeg::platform::IPlatformServices {
public:
    [[nodiscard]] aeg::foundation::ResultValue<std::string> get_clipboard_text() const override {
        return {aeg::foundation::Result::success(), clipboard_text};
    }

    [[nodiscard]] aeg::foundation::Result set_clipboard_text(std::string_view text) override {
        clipboard_text = text;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result set_cursor(aeg::platform::WindowId window,
                                                     aeg::platform::CursorShape shape) noexcept override {
        cursor_window = window;
        cursor_shape = shape;
        ++cursor_calls;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result set_ime_state(aeg::platform::WindowId window,
                                                        const aeg::platform::ImeState& state) noexcept override {
        ime_window = window;
        ime_state = state;
        ++ime_calls;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::platform::WindowMetrics> query_window_metrics(
        aeg::platform::WindowId) const override {
        return {aeg::foundation::Result::success(), aeg::platform::WindowMetrics{{1280, 720}, 1.0f, 1.0f}};
    }

    std::string clipboard_text{"initial"};
    aeg::platform::WindowId cursor_window{};
    aeg::platform::CursorShape cursor_shape{aeg::platform::CursorShape::kNone};
    uint32_t cursor_calls{0};
    aeg::platform::WindowId ime_window{};
    aeg::platform::ImeState ime_state{};
    uint32_t ime_calls{0};
};

class SmokePanel final : public aeg::editor::EditorPanel {
public:
    void build_ui() override {
        ImGui::SetClipboardText("bridge");
        const char* clipboard = ImGui::GetClipboardText();
        clipboard_roundtrip = clipboard != nullptr && std::string_view(clipboard) == "bridge";
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGuiPlatformImeData ime_data{};
        ime_data.WantVisible = true;
        ime_data.WantTextInput = true;
        ime_data.InputPos = ImVec2(24.0f, 32.0f);
        ime_data.InputLineHeight = 18.0f;
        if (const auto callback = ImGui::GetPlatformIO().Platform_SetImeDataFn; callback != nullptr) {
            callback(ImGui::GetCurrentContext(), nullptr, &ime_data);
        }
        ImGui::Begin("Smoke");
        ImGui::TextUnformatted("AEG");
        ImGui::End();
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(10.0f, 10.0f), ImVec2(100.0f, 100.0f), IM_COL32_WHITE);
        ++build_count;
    }

    uint32_t build_count{0};
    bool clipboard_roundtrip{false};
};
}  // namespace

int main() {
    using namespace aeg;

    editor::ImGuiTextureRegistry textures;
    const rhi::TextureViewHandle view{11};
    const rhi::SamplerHandle sampler{17};
    const foundation::ResultValue<editor::ImGuiTextureHandle> texture_result =
        textures.register_texture(editor::ImGuiTextureBinding{view, sampler});
    assert(texture_result);

    TestPlatformServices platform_services;
    editor::EditorImGuiHost host;
    editor::EditorImGuiDesc host_desc{};
    host_desc.platform_services = &platform_services;
    assert(host.initialize(host_desc));
    editor::ImGuiFontAtlasData font_atlas;
    assert(host.capture_font_atlas(font_atlas));
    assert(font_atlas.is_valid());
    assert(host.set_font_texture(texture_result.value));
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
    assert(panel_pointer->clipboard_roundtrip);
    assert(platform_services.clipboard_text == "bridge");
    assert(platform_services.cursor_window == window);
    assert(platform_services.cursor_shape == platform::CursorShape::kHand);
    assert(platform_services.cursor_calls != 0);
    assert(platform_services.ime_window == window);
    assert(platform_services.ime_state.want_visible);
    assert(platform_services.ime_state.want_text_input);
    assert(platform_services.ime_state.input_x == 24.0f);
    assert(platform_services.ime_state.input_y == 32.0f);
    assert(platform_services.ime_calls != 0);
    assert(host.draw_data() != nullptr);
    editor::ImGuiDrawPacket draw_packet;
    assert(host.capture_draw_packet(draw_packet));
    assert(!draw_packet.vertices.empty());
    assert(!draw_packet.indices.empty());
    assert(!draw_packet.commands.empty());

    assert(host.unregister_panel(panel_result.value));
    assert(host.panel_count() == 0);
    assert(host.shutdown());

    const uint64_t texture_id = editor::make_imgui_texture_id(texture_result.value);
    assert(editor::decode_imgui_texture_id(texture_id) == texture_result.value);
    assert(textures.resolve(texture_result.value));
    assert(textures.update_texture(texture_result.value,
                                   editor::ImGuiTextureBinding{rhi::TextureViewHandle{23}, rhi::SamplerHandle{29}}));
    assert(textures.unregister_texture(texture_result.value));
    assert(!textures.resolve(texture_result.value));
    return 0;
}
