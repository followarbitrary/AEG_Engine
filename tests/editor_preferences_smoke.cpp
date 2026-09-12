/**
 * @file editor_preferences_smoke.cpp
 * @brief Verifies the editor preference draft model and panel contract.
 */
#include <imgui.h>

#include <cassert>
#include <memory>
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
    assert(host.initialize());
    EditorPreferenceModel panel_model(make_initial_state());
    TestActions panel_actions;
    assert(host.register_panel(std::make_unique<EditorPreferencePanel>(panel_model, panel_actions)));
    EditorImGuiFrameDesc frame_desc{};
    frame_desc.window = WindowId{0, 1};
    frame_desc.display_extent = WindowExtent{1280, 720};
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(host.end_frame());
    assert(host.draw_data() != nullptr);
    assert(host.shutdown());
    return 0;
}
