/**
 * @file editor_preferences.cpp
 * @brief Implements the editor Preferences draft model and panel.
 */
#include "editor_preferences.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <new>
#include <string>
#include <utility>

#include "editor_imgui.hpp"

namespace aeg::editor {
namespace {
constexpr float kMinimumEditorFontSizePixels = 10.0f;
constexpr float kMaximumEditorFontSizePixels = 48.0f;

[[nodiscard]] const char* font_kind_name(EditorFontKind kind) noexcept {
    return kind == EditorFontKind::kMonospace ? "Monospace" : "Interface";
}

[[nodiscard]] const EditorFontDescriptor* find_font(const EditorPreferenceState& state,
                                                      std::string_view font_key) noexcept {
    const auto iterator = std::find_if(state.fonts.begin(), state.fonts.end(), [font_key](const auto& font) {
        return font.key == font_key;
    });
    return iterator == state.fonts.end() ? nullptr : &*iterator;
}

}  // namespace

EditorPreferenceModel::EditorPreferenceModel(EditorPreferenceState applied) : applied_(std::move(applied)), draft_(applied_) {
    default_font_key_ = applied_.active_font_key;
    default_font_size_pixels_ = applied_.font_size_pixels;
    draft_.dirty = false;
    applied_.dirty = false;
}

const EditorPreferenceState& EditorPreferenceModel::state() const noexcept {
    return draft_;
}

const EditorPreferenceState& EditorPreferenceModel::applied_state() const noexcept {
    return applied_;
}

foundation::Result EditorPreferenceModel::select_font(std::string_view font_key) {
    if (font_key.empty()) {
        return foundation::invalid_argument("Editor preference font key is empty");
    }
    if (find_font(draft_, font_key) == nullptr) {
        return foundation::not_found("Editor preference font key is not available");
    }
    draft_.active_font_key = font_key;
    draft_.error_message.clear();
    refresh_dirty();
    return foundation::Result::success();
}

foundation::Result EditorPreferenceModel::set_font_size(float size_pixels) {
    if (!std::isfinite(size_pixels) || size_pixels < kMinimumEditorFontSizePixels ||
        size_pixels > kMaximumEditorFontSizePixels) {
        return foundation::invalid_argument("Editor preference font size must be between 10 and 48 pixels");
    }
    draft_.font_size_pixels = size_pixels;
    draft_.error_message.clear();
    refresh_dirty();
    return foundation::Result::success();
}

foundation::Result EditorPreferenceModel::reset_defaults() {
    if (draft_.fonts.empty()) {
        return foundation::invalid_state("Editor Preferences has no available fonts");
    }
    if (find_font(draft_, default_font_key_) == nullptr) {
        return foundation::not_found("Editor preference default font is not available");
    }
    draft_.active_font_key = default_font_key_;
    draft_.font_size_pixels = default_font_size_pixels_;
    draft_.error_message.clear();
    refresh_dirty();
    return foundation::Result::success();
}

void EditorPreferenceModel::accept() {
    applied_.active_font_key = draft_.active_font_key;
    applied_.font_size_pixels = draft_.font_size_pixels;
    applied_.error_message.clear();
    applied_.dirty = false;
    draft_.error_message.clear();
    draft_.dirty = false;
}

void EditorPreferenceModel::discard() {
    draft_.active_font_key = applied_.active_font_key;
    draft_.font_size_pixels = applied_.font_size_pixels;
    draft_.error_message.clear();
    draft_.dirty = false;
}

foundation::Result EditorPreferenceModel::set_error(std::string_view message) {
    try {
        draft_.error_message.assign(message);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor preference error message allocation failed");
    }
}

void EditorPreferenceModel::clear_error() noexcept {
    draft_.error_message.clear();
}

void EditorPreferenceModel::refresh_dirty() noexcept {
    draft_.dirty = draft_.active_font_key != applied_.active_font_key ||
                   draft_.font_size_pixels != applied_.font_size_pixels;
}

EditorPreferencePanel::EditorPreferencePanel(EditorPreferenceModel& model, IEditorPreferenceActions& actions,
                                             EditorImGuiHost* preview_host) noexcept
    : model_(&model), actions_(&actions), preview_host_(preview_host) {}

void EditorPreferencePanel::build_ui() {
    if (model_ == nullptr || actions_ == nullptr) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport != nullptr) {
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    }
    constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                               ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("Preferences", nullptr, window_flags)) {
        ImGui::End();
        return;
    }
    const EditorPreferenceState& state = model_->state();
    const float footer_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;

    if (ImGui::BeginChild("PreferenceContent", ImVec2(0.0f, -footer_height), false)) {
        if (ImGui::BeginChild("PreferenceCategories", ImVec2(180.0f, 0.0f), true)) {
            ImGui::TextUnformatted("Preferences");
            ImGui::Separator();
            if (ImGui::Selectable("General", page_ == Page::kGeneral)) {
                page_ = Page::kGeneral;
            }
            if (ImGui::Selectable("Fonts", page_ == Page::kFonts)) {
                page_ = Page::kFonts;
            }
            if (ImGui::Selectable("Input", page_ == Page::kInput)) {
                page_ = Page::kInput;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        if (ImGui::BeginChild("PreferenceDetails", ImVec2(0.0f, 0.0f), false)) {
            const char* page_title = page_ == Page::kGeneral ? "General" : page_ == Page::kFonts ? "Fonts" : "Input";
            ImGui::Text("%s", page_title);
            ImGui::Separator();
            switch (page_) {
                case Page::kGeneral:
                    ImGui::TextWrapped("Editor-wide behavior and application defaults.");
                    break;
                case Page::kFonts:
                    ImGui::TextWrapped("Typography used by the editor UI only. Game runtime fonts are independent.");
                    break;
                case Page::kInput:
                    ImGui::TextWrapped("Keyboard, mouse, and viewport interaction settings.");
                    break;
            }

            if (page_ == Page::kFonts) {
                const float content_width = ImGui::GetContentRegionAvail().x;
                const float settings_width = std::clamp(content_width * 0.42f, 300.0f, 430.0f);
                if (ImGui::BeginChild("FontSettings", ImVec2(settings_width, 0.0f), false)) {
                    ImGui::TextDisabled("Editor");
                    ImGui::SameLine();
                    ImGui::TextDisabled(">");
                    ImGui::SameLine();
                    ImGui::TextUnformatted("Font");
                    ImGui::Separator();

                    const EditorFontDescriptor* selected_font = find_font(state, state.active_font_key);
                    const char* selected_name = selected_font != nullptr ? selected_font->display_name.c_str() : "Unavailable";
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Font");
                    ImGui::SameLine(92.0f);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##EditorFont", selected_name)) {
                        for (const EditorFontDescriptor& font : state.fonts) {
                            const bool selected = font.key == state.active_font_key;
                            if (ImGui::Selectable(font.display_name.c_str(), selected)) {
                                if (const foundation::Result result = model_->select_font(font.key); !result) {
                                    static_cast<void>(model_->set_error(result.message));
                                } else if (const foundation::Result preview_result = actions_->preview(model_->state());
                                           !preview_result) {
                                    static_cast<void>(model_->set_error(preview_result.message));
                                }
                            }
                            if (selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Size");
                    ImGui::SameLine(92.0f);
                    ImGui::SetNextItemWidth(92.0f);
                    float font_size = state.font_size_pixels;
                    if (ImGui::DragFloat("##FontSize", &font_size, 0.1f, kMinimumEditorFontSizePixels,
                                         kMaximumEditorFontSizePixels, "%.1f")) {
                        if (const foundation::Result result = model_->set_font_size(font_size); !result) {
                            static_cast<void>(model_->set_error(result.message));
                        } else if (const foundation::Result preview_result = actions_->preview(model_->state());
                                   !preview_result) {
                            static_cast<void>(model_->set_error(preview_result.message));
                        }
                    }

                    ImGui::Spacing();
                    selected_font = find_font(model_->state(), model_->state().active_font_key);
                    if (selected_font != nullptr) {
                        ImGui::Text("Family: %s", selected_font->display_name.c_str());
                        ImGui::Text("Category: %s", font_kind_name(selected_font->kind));
                        ImGui::TextUnformatted(selected_font->is_default ? "Source: System default" : "Source: System font");
                        ImGui::TextUnformatted("Scope: Editor UI only");
                    }

                    ImGui::Spacing();
                    if (ImGui::CollapsingHeader("Typography Settings")) {
                        ImGui::TextWrapped("Additional editor typography options will be added here.");
                    }
                }
                ImGui::EndChild();
                ImGui::SameLine();

                if (ImGui::BeginChild("FontPreview", ImVec2(0.0f, 0.0f), true)) {
                    ImGui::TextUnformatted("Preview");
                    ImGui::Separator();
                    const void* preview_font = preview_host_ != nullptr ? preview_host_->preview_font() : nullptr;
                    if (preview_font != nullptr) {
                        ImGui::PushFont(const_cast<ImFont*>(static_cast<const ImFont*>(preview_font)));
                    }
                    ImGui::TextUnformatted("AEG Engine Editor");
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Aa Bb Cc 0123456789 !@#$%^&*()");
                    ImGui::TextUnformatted("中文字体预览：场景 组件 属性 资源");
                    ImGui::Spacing();
                    ImGui::TextUnformatted("for (const auto& entity : world.entities()) {");
                    ImGui::TextUnformatted("    world.destroy(entity);");
                    ImGui::TextUnformatted("}");
                    ImGui::Spacing();
                    ImGui::TextWrapped(
                        "Clarity and rhythm matter when reading scene data, component values, and code inside the editor.");
                    if (preview_font != nullptr) {
                        ImGui::PopFont();
                    }
                }
                ImGui::EndChild();
            } else if (page_ == Page::kGeneral) {
                ImGui::TextUnformatted("Project and editor-wide defaults will live here.");
            } else {
                ImGui::TextUnformatted("Input bindings and viewport interaction will live here.");
            }
        }
        ImGui::EndChild();

        if (!state.error_message.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.25f, 1.0f), "%s", state.error_message.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float reset_button_width = ImGui::CalcTextSize("Reset Defaults").x + style.FramePadding.x * 2.0f;
    const float cancel_button_width = ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
    const float apply_button_width = ImGui::CalcTextSize("Apply").x + style.FramePadding.x * 2.0f;
    const float save_button_width = ImGui::CalcTextSize("Save").x + style.FramePadding.x * 2.0f;
    const float footer_button_spacing = style.ItemSpacing.x;
    const float footer_buttons_width = reset_button_width + cancel_button_width + apply_button_width + save_button_width +
                                       footer_button_spacing * 3.0f;
    const float footer_start_x = std::max(ImGui::GetCursorPosX(),
                                          ImGui::GetWindowContentRegionMax().x - footer_buttons_width);
    ImGui::SameLine(footer_start_x);
    if (ImGui::Button("Reset Defaults")) {
        if (const foundation::Result result = model_->reset_defaults(); !result) {
            static_cast<void>(model_->set_error(result.message));
        } else if (const foundation::Result preview_result = actions_->preview(model_->state()); !preview_result) {
            static_cast<void>(model_->set_error(preview_result.message));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        if (const foundation::Result result = actions_->cancel(model_->applied_state()); !result) {
            static_cast<void>(model_->set_error(result.message));
        }
    }
    ImGui::SameLine();
    if (!state.dirty) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Apply")) {
        if (const foundation::Result result = actions_->apply(state); !result) {
            static_cast<void>(model_->set_error(result.message));
        }
    }
    if (!state.dirty) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (!state.dirty) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save")) {
        if (const foundation::Result result = actions_->save(state); !result) {
            static_cast<void>(model_->set_error(result.message));
        }
    }
    if (!state.dirty) {
        ImGui::EndDisabled();
    }
    ImGui::End();
}
}  // namespace aeg::editor
