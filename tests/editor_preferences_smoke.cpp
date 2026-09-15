/**
 * @file editor_preferences_smoke.cpp
 * @brief Verifies the editor preference draft model and panel contract.
 */
#include <imgui.h>
#include <imgui_internal.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "editor_imgui.hpp"
#include "editor_preferences.hpp"

namespace {
class TestActions final : public aeg::editor::IEditorPreferenceActions {
public:
    aeg::foundation::Result preview(const aeg::editor::EditorPreferenceState& state) override {
        ++preview_count;
        previewed_key = state.active_font_key;
        previewed_size = state.font_size_pixels;
        return fail_preview ? aeg::foundation::invalid_state("test preview failure")
                            : aeg::foundation::Result::success();
    }

    aeg::foundation::Result apply(const aeg::editor::EditorPreferenceState& state) override {
        ++apply_count;
        if (throw_apply) throw std::runtime_error("recoverable preferences action failure");
        applied_key = state.active_font_key;
        applied_size = state.font_size_pixels;
        return fail_apply ? aeg::foundation::invalid_state("test apply failure") : aeg::foundation::Result::success();
    }

    aeg::foundation::Result save(const aeg::editor::EditorPreferenceState& state) override {
        ++save_count;
        saved_key = state.active_font_key;
        saved_size = state.font_size_pixels;
        return fail_save ? aeg::foundation::invalid_state("test save failure") : aeg::foundation::Result::success();
    }

    aeg::foundation::Result cancel(const aeg::editor::EditorPreferenceState& state) override {
        ++cancel_count;
        cancelled_key = state.active_font_key;
        cancelled_size = state.font_size_pixels;
        return aeg::foundation::Result::success();
    }

    uint32_t preview_count{0};
    uint32_t apply_count{0};
    uint32_t save_count{0};
    uint32_t cancel_count{0};
    bool fail_preview{false};
    bool fail_apply{false};
    bool throw_apply{false};
    bool fail_save{false};
    std::string previewed_key{};
    float previewed_size{0.0f};
    std::string applied_key{};
    float applied_size{0.0f};
    std::string saved_key{};
    float saved_size{0.0f};
    std::string cancelled_key{};
    float cancelled_size{0.0f};
};

[[nodiscard]] aeg::editor::EditorPreferenceState make_initial_state() {
    aeg::editor::EditorPreferenceState state{};
    state.fonts = {
        aeg::editor::EditorFontDescriptor{"noto_sans_sc", "Noto Sans SC", aeg::editor::EditorFontKind::kInterface,
                                          true, false, true, true},
        aeg::editor::EditorFontDescriptor{"inter", "Inter", aeg::editor::EditorFontKind::kInterface, false, false,
                                          true, true},
    };
    state.active_font_key = "noto_sans_sc";
    state.font_size_pixels = 18.0f;
    return state;
}
}  // namespace

int main() {
    using aeg::editor::EditorImGuiFrameDesc;
    using aeg::editor::EditorImGuiHost;
    using aeg::editor::EditorPreferenceModel;
    using aeg::editor::EditorPreferencePanel;
    using aeg::platform::WindowExtent;
    using aeg::platform::WindowId;

    const aeg::editor::EditorPreferenceState default_preferences{};
    assert(default_preferences.font_size_pixels == 16.0f);

    EditorPreferenceModel model(make_initial_state());
    assert(!model.state().dirty);
    assert(model.select_font("inter"));
    assert(model.state().active_font_key == "inter");
    assert(model.state().dirty);
    assert(!model.select_font("missing"));
    assert(model.state().active_font_key == "inter");
    assert(!model.set_font_size(0.0f));
    assert(model.state().font_size_pixels == 18.0f);
    assert(model.set_font_size(22.0f));

    TestActions actions;
    assert(actions.preview(model.state()));
    assert(actions.preview_count == 1);
    assert(actions.previewed_key == "inter");
    assert(actions.previewed_size == 22.0f);
    assert(actions.apply(model.state()));
    model.accept();
    assert(!model.state().dirty);
    assert(actions.applied_key == "inter");
    assert(actions.applied_size == 22.0f);
    assert(model.applied_state().active_font_key == "inter");

    assert(model.select_font("noto_sans_sc"));
    assert(model.set_font_size(20.0f));
    assert(actions.save(model.state()));
    assert(actions.saved_key == "noto_sans_sc");
    assert(actions.saved_size == 20.0f);
    model.accept();
    assert(!model.state().dirty);
    assert(model.applied_state().active_font_key == "noto_sans_sc");
    assert(model.applied_state().font_size_pixels == 20.0f);

    assert(model.select_font("inter"));
    assert(actions.cancel(model.applied_state()));
    assert(actions.cancelled_key == "noto_sans_sc");
    assert(actions.cancelled_size == 20.0f);
    model.discard();
    assert(!model.state().dirty);
    assert(model.state().active_font_key == "noto_sans_sc");
    assert(model.reset_defaults());
    assert(model.state().active_font_key == "noto_sans_sc");
    assert(model.state().font_size_pixels == 18.0f);

    assert(model.select_font("noto_sans_sc"));
    actions.fail_apply = true;
    assert(!actions.apply(model.state()));
    assert(model.state().dirty);
    assert(model.state().active_font_key == "noto_sans_sc");

    actions.fail_save = true;
    assert(!actions.save(model.state()));
    assert(model.state().dirty);

    EditorImGuiHost host;
    aeg::editor::EditorImGuiDesc host_desc{};
    host_desc.enable_preference_fonts = true;
    assert(host.initialize(host_desc));
    // Display copy must come from a separately rasterized atlas font rather
    // than scaling the 16px interface bitmap until it blurs.
    assert(host.display_font() != nullptr);
    assert(host.display_font_size_pixels() == 32.0f);
    EditorPreferenceModel panel_model(make_initial_state());
    TestActions panel_actions;
    assert(host.register_panel(std::make_unique<EditorPreferencePanel>(panel_model, panel_actions, &host)));
    EditorImGuiFrameDesc frame_desc{};
    frame_desc.window = WindowId{0, 1};
    frame_desc.display_extent = WindowExtent{1578, 954};
    const ImGuiStyle original_style = ImGui::GetStyle();
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    // The selected settings design separates navigation, the editable font
    // controls, and the preview surface into stable visual regions.
    const auto contains_window = [](const char* fragment) {
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
            if (std::strstr(window->Name, fragment) != nullptr) {
                return true;
            }
        }
        return false;
    };
    const auto find_window = [](const char* fragment) -> ImGuiWindow* {
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
            if (std::strstr(window->Name, fragment) != nullptr) {
                return window;
            }
        }
        return nullptr;
    };
    assert(contains_window("PreferenceNavigation"));
    assert(contains_window("FontPreviewSurface"));
    assert(contains_window("PreferenceActionBar"));
    assert(find_window("PreferenceNavigation")->Size.x <= 200.0f);
    assert(find_window("PreferenceActionBar")->Size.y < 68.0f);
    // The reference design starts the preview card above the left form's
    // controls instead of letting the Fonts heading push both columns down.
    const auto* settings = find_window("FontSettings");
    const auto* preview = find_window("FontPreviewSurface");
    assert(settings->Pos.x + settings->Size.x < preview->Pos.x);
    assert(preview->Pos.y == settings->Pos.y);
    assert(preview->Pos.y + preview->Size.y <= find_window("PreferenceActionBar")->Pos.y);
    assert(ImGui::GetStyle().FrameRounding == original_style.FrameRounding);
    assert(ImGui::GetStyle().WindowPadding.x == original_style.WindowPadding.x);
    assert(ImGui::GetCurrentContext()->ColorStack.Size == 0);
    assert(ImGui::GetCurrentContext()->StyleVarStack.Size == 0);
    assert(host.end_frame());
    assert(host.draw_data() != nullptr);
    assert(panel_model.select_font("inter"));
    panel_actions.throw_apply = true;
    ImGui::ActivateItemByID(find_window("PreferenceActionBar")->GetID("Apply"));
    const auto apply_count_before_failure = panel_actions.apply_count;
    for (int frame = 0; frame < 3; ++frame) {
        assert(host.begin_frame(frame_desc));
        assert(host.build_panels());
        assert(ImGui::GetCurrentContext()->ColorStack.Size == 0);
        assert(ImGui::GetCurrentContext()->StyleVarStack.Size == 0);
        assert(host.end_frame());
    }
    assert(panel_actions.apply_count == apply_count_before_failure + 1);
    assert(!panel_model.state().error_message.empty());
    assert(panel_model.state().dirty);
    panel_actions.throw_apply = false;
    // An applied accessibility-sized font may enlarge text, but must not let
    // navigation consume the workspace or move Preview below the form.
    assert(host.set_font("", 40));
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(find_window("PreferenceNavigation")->Size.x <= 240.0f);
    assert(find_window("FontSettings")->Pos.x + find_window("FontSettings")->Size.x <
           find_window("FontPreviewSurface")->Pos.x);
    assert(find_window("FontPreviewSurface")->Pos.y == find_window("FontSettings")->Pos.y);
    assert(host.end_frame());
    assert(host.set_font("", 16));
    // A normal-width dialog keeps Preview on the right even when the draft
    // font is large. Stacking is reserved for genuinely narrow windows.
    frame_desc.display_extent = WindowExtent{980, 720};
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(find_window("FontSettings")->Pos.x + find_window("FontSettings")->Size.x <
           find_window("FontPreviewSurface")->Pos.x);
    assert(find_window("FontPreviewSurface")->Pos.y == find_window("FontSettings")->Pos.y);
    assert(find_window("PreferenceActionBar")->Pos.y < 720);
    assert(ImGui::GetCurrentContext()->ColorStack.Size == 0);
    assert(ImGui::GetCurrentContext()->StyleVarStack.Size == 0);
    assert(host.end_frame());
    frame_desc.display_extent = WindowExtent{760, 720};
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(find_window("FontSettings")->Pos.y + find_window("FontSettings")->Size.y <
           find_window("FontPreviewSurface")->Pos.y);
    assert(host.end_frame());
    frame_desc.display_extent = WindowExtent{500, 720};
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(find_window("PreferenceActionBar")->Size.y >= 80);
    assert(find_window("PreferenceContent")->Pos.y + find_window("PreferenceContent")->Size.y <=
           find_window("PreferenceActionBar")->Pos.y);
    assert(host.end_frame());
    assert(host.shutdown());
    return 0;
}
