#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "editor_imgui.hpp"
#include "hierarchy_ui_layout.hpp"
#include "imgui_texture_registry.hpp"
#include "project_dialog_ui.hpp"

namespace {
class ThrowingPanel final : public aeg::editor::EditorPanel {
public:
    void build_ui() override {
        ImGui::Begin("Throwing panel");
        ImGui::BeginChild("Unfinished child");
        ImGui::BeginTable("Unfinished table", 2);
        ImGui::TableNextColumn();
        ImGui::PushFont(ImGui::GetFont());
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7, 7));
        ImGui::BeginDisabled();
        throw std::runtime_error("recoverable panel callback failure");
    }
};

class MutatingPanel final : public aeg::editor::EditorPanel {
public:
    aeg::editor::EditorImGuiHost* host{};
    aeg::editor::EditorPanelId other_panel{};
    void build_ui() override {
        assert(!host->unregister_panel(other_panel));
        assert(!host->register_panel(std::make_unique<ThrowingPanel>()));
        assert(!host->build_panels());
        assert(!host->end_frame());
        assert(!host->abort_frame());
    }
};

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
        if (throw_metrics)
            throw std::runtime_error("recoverable metrics failure");
        return {aeg::foundation::Result::success(), aeg::platform::WindowMetrics{{1280, 720}, 1.0f, 1.0f}};
    }

    bool throw_metrics{false};
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
        project_field_metrics = aeg::application::draw_project_dialog_field(
            "Project directory", "##SmokeProjectDirectory", project_path.data(), project_path.size());
        aeg::application::push_hierarchy_item_id(1, 1);
        first_hierarchy_item_id = ImGui::GetID("##HierarchyEntity");
        aeg::application::pop_hierarchy_item_id();
        aeg::application::push_hierarchy_item_id(2, 1);
        second_hierarchy_item_id = ImGui::GetID("##HierarchyEntity");
        aeg::application::pop_hierarchy_item_id();
        ImGui::End();
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(10.0f, 10.0f), ImVec2(100.0f, 100.0f), IM_COL32_WHITE);
        ++build_count;
    }

    uint32_t build_count{0};
    bool clipboard_roundtrip{false};
    std::array<char, 64> project_path{};
    aeg::application::ProjectDialogFieldMetrics project_field_metrics{};
    ImGuiID first_hierarchy_item_id{0};
    ImGuiID second_hierarchy_item_id{0};
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
    editor::EditorFontCatalog replacement_catalog;
    assert(host.replace_font_catalog(replacement_catalog));
    assert(host.set_font_texture(texture_result.value));
    editor::ImGuiFontAtlasData replacement_atlas;
    assert(host.capture_font_atlas(replacement_atlas));
    assert(replacement_atlas.is_valid());
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
    assert(!host.replace_font_catalog(replacement_catalog));
    assert(host.build_panels());
    assert(host.end_frame());

    assert(panel_pointer->build_count == 1);
    assert(panel_pointer->clipboard_roundtrip);
    assert(panel_pointer->project_field_metrics.label_right <= panel_pointer->project_field_metrics.input_left);
    assert(panel_pointer->first_hierarchy_item_id != panel_pointer->second_hierarchy_item_id);
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

    const auto throwing_panel = host.register_panel(std::make_unique<ThrowingPanel>());
    assert(throwing_panel);
    assert(host.begin_frame(frame_desc));
    bool callback_escaped = false;
    foundation::Result callback_result;
    try {
        callback_result = host.build_panels();
    } catch (...) {
        callback_escaped = true;
    }
    assert(!callback_escaped);
    assert(!callback_result);
    assert(callback_result.message.find("panel callback failure") != std::string::npos);
    assert(ImGui::GetCurrentContext()->CurrentWindowStack.Size == 1);
    assert(ImGui::GetCurrentContext()->ColorStack.Size == 0);
    assert(ImGui::GetCurrentContext()->StyleVarStack.Size == 0);
    assert(ImGui::GetCurrentContext()->DisabledStackSize == 0);
    assert(host.end_frame());
    assert(host.unregister_panel(throwing_panel.value));
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(host.end_frame());

    // The application can discard a failed menu frame without presenting any
    // partial draw data, then begin another frame with an unlocked atlas.
    assert(host.begin_frame(frame_desc));
    ImGui::Begin("Interrupted menu operation");
    ImGui::BeginChild("Interrupted child");
    ImGui::PushFont(ImGui::GetFont());
    ImGui::BeginDisabled();
    assert(host.abort_frame());
    assert(!host.is_frame_active());
    assert(!ImGui::GetIO().Fonts->Locked);
    assert(host.draw_data() == nullptr);
    assert(!host.capture_draw_packet(draw_packet));
    assert(host.abort_frame());
    auto mutation = std::make_unique<MutatingPanel>();
    mutation->host = &host;
    const auto mutating_panel = host.register_panel(std::move(mutation));
    assert(mutating_panel);
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(host.end_frame());
    assert(host.unregister_panel(mutating_panel.value));

    platform_services.throw_metrics = true;
    bool metrics_escaped = false;
    foundation::Result metrics_result;
    try {
        metrics_result = host.begin_frame(frame_desc);
    } catch (...) {
        metrics_escaped = true;
    }
    assert(!metrics_escaped);
    assert(!metrics_result);
    assert(!host.is_frame_active());
    platform_services.throw_metrics = false;
    assert(host.begin_frame(frame_desc));
    assert(host.end_frame());
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
