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
    if (model_ == nullptr || actions_ == nullptr) return;

    // Settings has its own ImGui context. Keep this theme scoped, including the
    // zero-padding root; unbordered children only honor padding with the flag below.
    struct ScopedTheme {
        ImGuiStyle saved{ImGui::GetStyle()};
        ScopedTheme() {
            auto& style = ImGui::GetStyle();
            style.WindowPadding = ImVec2(0, 0);
            style.WindowBorderSize = 0;
            style.WindowRounding = 0;
            style.ChildBorderSize = 1;
            style.ChildRounding = 4;
            style.FramePadding = ImVec2(8, 4);
            style.FrameRounding = 5;
            style.FrameBorderSize = 1;
            style.PopupRounding = 5;
            style.ItemSpacing = ImVec2(8, 4);
            style.ItemInnerSpacing = ImVec2(8, 6);
            style.CellPadding = ImVec2(0, 0);
            style.ScrollbarSize = 10;
            style.ScrollbarRounding = 5;
            style.GrabRounding = 4;
            style.DisabledAlpha = 0.48f;
            style.SelectableTextAlign = ImVec2(0, 0.5f);
            const auto color = [](int r, int g, int b) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1); };
            style.Colors[ImGuiCol_WindowBg] = color(14, 22, 29);
            style.Colors[ImGuiCol_ChildBg] = color(14, 22, 29);
            style.Colors[ImGuiCol_PopupBg] = color(20, 32, 42);
            style.Colors[ImGuiCol_Text] = color(234, 242, 246);
            style.Colors[ImGuiCol_TextDisabled] = color(153, 179, 194);
            style.Colors[ImGuiCol_Border] = color(47, 67, 80);
            style.Colors[ImGuiCol_Separator] = color(47, 67, 80);
            style.Colors[ImGuiCol_FrameBg] = color(24, 37, 47);
            style.Colors[ImGuiCol_FrameBgHovered] = color(32, 51, 65);
            style.Colors[ImGuiCol_FrameBgActive] = color(37, 62, 79);
            style.Colors[ImGuiCol_Button] = color(21, 33, 43);
            style.Colors[ImGuiCol_ButtonHovered] = color(31, 53, 68);
            style.Colors[ImGuiCol_ButtonActive] = color(26, 66, 86);
            style.Colors[ImGuiCol_Header] = color(24, 56, 79);
            style.Colors[ImGuiCol_HeaderHovered] = color(28, 65, 89);
            style.Colors[ImGuiCol_HeaderActive] = color(31, 75, 99);
            style.Colors[ImGuiCol_CheckMark] = color(15, 198, 220);
            style.Colors[ImGuiCol_ScrollbarBg] = color(14, 22, 29);
            style.Colors[ImGuiCol_ScrollbarGrab] = color(49, 69, 82);
        }
        ~ScopedTheme() { ImGui::GetStyle() = saved; }
    } theme;
    const auto font = [this](float size) {
        const void* face = preview_host_ != nullptr ? preview_host_->preference_font(size) : nullptr;
        return face != nullptr ? const_cast<ImFont*>(static_cast<const ImFont*>(face)) : ImGui::GetFont();
    };
    const auto text = [&font](float size, const char* value, bool muted = false) {
        ImGui::PushFont(font(size), font(size)->LegacySize);
        if (muted) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", value);
        if (muted) ImGui::PopStyleColor();
        ImGui::PopFont();
    };
    const auto gap = [](float pixels) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pixels);
    };
    const auto preview_draft = [this]() {
        if (!actions_->preview(model_->state()))
            static_cast<void>(model_->set_error("The font preview could not be updated. Please try again."));
    };
    const auto update_font_size = [this, &preview_draft](float size) {
        if (!model_->set_font_size(size))
            static_cast<void>(model_->set_error("Font size must be between 10 and 48 pixels."));
        else
            preview_draft();
    };

    // Reserve space from the rasterized draft font, not the previous main font.
    const float unit = std::max(1.0f, font(20)->LegacySize / 16.0f);
    const float control_height = font(20)->LegacySize + 8;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags fixed_window = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    constexpr ImGuiChildFlags padded = ImGuiChildFlags_AlwaysUseWindowPadding;
    ImGui::PushFont(font(20), font(20)->LegacySize);
    if (ImGui::Begin("Preferences", nullptr, fixed_window)) {
        const auto& state = model_->state();
        const bool compact_footer = viewport->WorkSize.x < 560 * unit;
        const float footer_height = compact_footer ? control_height * 2 + 40 : control_height + 24;
        const float content_height = std::max(100.0f, ImGui::GetContentRegionAvail().y - footer_height);
        const float navigation_width = std::min(viewport->WorkSize.x * 0.24f,
                                                176 * std::min(unit, 1.35f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        if (ImGui::BeginChild("PreferenceContent", ImVec2(0, content_height), 0, ImGuiWindowFlags_NoScrollbar)) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(11 / 255.0f, 20 / 255.0f, 27 / 255.0f, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 14));
            if (ImGui::BeginChild("PreferenceNavigation", ImVec2(navigation_width, 0), padded)) {
                const auto category = [this, &font, control_height](const char* label, const char* icon, Page page) {
                    const bool selected = page_ == page;
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const ImVec2 size(ImGui::GetContentRegionAvail().x + 10, control_height + 8);
                    ImGui::PushID(label);
                    if (ImGui::Selectable("##Category", selected, 0, size))
                        page_ = page;
                    auto* draw = ImGui::GetWindowDrawList();
                    if (selected)
                        draw->AddRectFilled(p, ImVec2(p.x + 3, p.y + size.y), IM_COL32(15, 204, 223, 255));
                    const void* icons = preview_host_ != nullptr ? preview_host_->preference_icon_font() : nullptr;
                    if (icons != nullptr)
                        draw->AddText(const_cast<ImFont*>(static_cast<const ImFont*>(icons)), 20,
                                      ImVec2(p.x + 8, p.y + (size.y - 20) / 2), IM_COL32(184, 214, 230, 255), icon);
                    draw->AddText(font(20), font(20)->LegacySize, ImVec2(p.x + 36, p.y + (size.y - font(20)->LegacySize) / 2),
                                  ImGui::GetColorU32(ImGuiCol_Text), label);
                    ImGui::PopID();
                };
                category("General", "\xEE\x9C\x93", Page::kGeneral);
                category("Fonts", "\xEE\xA3\x92", Page::kFonts);
                category("Input", "\xEE\x9D\xA5", Page::kInput);
                const auto p = ImGui::GetWindowPos();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + navigation_width - 1, p.y),
                    ImVec2(p.x + navigation_width - 1, p.y + ImGui::GetWindowHeight()),
                    ImGui::GetColorU32(ImGuiCol_Border));
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
            if (ImGui::BeginChild("PreferenceDetails", ImVec2(0, 0), padded)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
                const float available_width = ImGui::GetContentRegionAvail().x;
                // Font previews stay beside the form at normal dialog widths.
                // Draft font size must not change this structural breakpoint.
                const bool stacked = available_width < 720;
                const float settings_ratio = unit > 1.5f ? 0.48f : 0.435f;
                const float settings_width = stacked ? available_width : (available_width - 24) * settings_ratio;
                const float body_height = ImGui::GetContentRegionAvail().y;
                const ImVec2 origin = ImGui::GetCursorPos();
                const float stacked_settings_height = 320 * unit;
                if (ImGui::BeginChild("FontSettings", ImVec2(settings_width, stacked ? stacked_settings_height : body_height), 0)) {
                    gap(8);
                    text(32, page_ == Page::kFonts ? "Fonts" : page_ == Page::kGeneral ? "General" : "Input");
                    text(16, page_ == Page::kFonts ? "Editor UI typography. Game runtime fonts are independent."
                        : page_ == Page::kGeneral ? "Editor-wide behaviour and application defaults."
                                                : "Keyboard, mouse, and viewport interaction settings.", true);
                    gap(2);
                    ImGui::Separator();
                    gap(12);
                    if (page_ == Page::kFonts) {
                        const auto* selected_font = find_font(state, state.active_font_key);
                        const char* name = selected_font != nullptr ? selected_font->display_name.c_str() : "Unavailable";
                        const float label_width = std::min(100 * unit, settings_width * 0.32f);
                        if (ImGui::BeginTable("FontControls", 2, ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, label_width);
                            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableNextRow(0, control_height + 8);
                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted("Font family");
                            ImGui::TableSetColumnIndex(1);
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::BeginCombo("##EditorFont", name)) {
                                for (const auto& entry : state.fonts) {
                                    ImGui::PushID(entry.key.c_str());
                                    const bool selected = entry.key == state.active_font_key;
                                    if (ImGui::Selectable(entry.display_name.c_str(), selected)) {
                                        if (model_->select_font(entry.key)) preview_draft();
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                    ImGui::PopID();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::TableNextRow(0, control_height);
                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted("Font size");
                            ImGui::TableSetColumnIndex(1);
                            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
                            const float step_width = control_height;
                            const float value_width = std::clamp(ImGui::GetContentRegionAvail().x - 2 * step_width - 36 * unit, 48 * unit, 72 * unit);
                            if (ImGui::Button("-", ImVec2(step_width, control_height)))
                                update_font_size(std::max(kMinimumEditorFontSizePixels, state.font_size_pixels - 1));
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(value_width);
                            float size = state.font_size_pixels;
                            if (ImGui::DragFloat("##FontSize", &size, 0.1f, kMinimumEditorFontSizePixels,
                                                kMaximumEditorFontSizePixels, "%.1f", ImGuiSliderFlags_AlwaysClamp))
                                update_font_size(size);
                            ImGui::SameLine();
                            if (ImGui::Button("+", ImVec2(step_width, control_height)))
                                update_font_size(std::min(kMaximumEditorFontSizePixels, state.font_size_pixels + 1));
                            ImGui::SameLine(0, 16);
                            ImGui::AlignTextToFramePadding();
                            text(16, "px", true);
                            ImGui::PopStyleVar();
                            ImGui::EndTable();
                        }
                        gap(10);
                        if (selected_font != nullptr && ImGui::BeginTable("FontMetadata", 2)) {
                            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, label_width);
                            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                            const auto metadata = [control_height](const char* label, const char* value) {
                                ImGui::TableNextRow(0, control_height);
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextDisabled("%s", label);
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextWrapped("%s", value);
                            };
                            metadata("Family:", selected_font->display_name.c_str());
                            metadata("Category:", font_kind_name(selected_font->kind));
                            metadata("Source:", selected_font->is_default ? "System default" : "System font");
                            metadata("Scope:", "Editor UI only");
                            ImGui::EndTable();
                        }
                        gap(8);
                        ImGui::Separator();
                        gap(4);
                        const bool expanded = ImGui::TreeNodeEx("Advanced Typography Settings", ImGuiTreeNodeFlags_SpanAvailWidth);
                        ImGui::Indent(expanded ? 20 : 32);
                        text(16, "Fine-tune editor text rendering and layout behaviour.", true);
                        if (expanded) {
                            gap(8);
                            text(16, "Size is measured in pixels (10-48). Preview changes are isolated until Apply or Save.", true);
                        }
                        ImGui::Unindent(expanded ? 20 : 32);
                        if (expanded) ImGui::TreePop();
                    } else {
                        text(20, page_ == Page::kGeneral ? "No additional general preferences are available yet."
                                                       : "No editable input bindings are available yet.", true);
                    }
                    if (!state.error_message.empty()) {
                        gap(16);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.48f, 0.40f, 1));
                        text(16, state.error_message.c_str());
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::EndChild();
                if (page_ == Page::kFonts) {
                    if (stacked) ImGui::SetCursorPos(ImVec2(origin.x, origin.y + stacked_settings_height + 16));
                    else ImGui::SetCursorPos(ImVec2(origin.x + settings_width + 24, origin.y));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
                    if (ImGui::BeginChild("FontPreviewSurface", ImVec2(stacked ? available_width : 0, stacked ? 520 : body_height),
                                          ImGuiChildFlags_Borders | padded)) {
                        const char* caption = "Actual size / 100%";
                        const float heading_y = ImGui::GetCursorPosY();
                        text(24, "Preview");
                        const float caption_width = font(16)->CalcTextSizeA(font(16)->LegacySize, 10000, 0, caption).x;
                        if (ImGui::GetContentRegionAvail().x > caption_width + 120) {
                            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - caption_width, heading_y + 6));
                            ImGui::PushFont(font(16), font(16)->LegacySize);
                            ImGui::TextDisabled("%s", caption);
                            ImGui::PopFont();
                        }
                        ImGui::SetCursorPosY(heading_y + font(24)->LegacySize + 12);
                        ImGui::Separator();
                        gap(12);
                        text(16, "Editor UI Text", true);
                        const void* face = preview_host_ != nullptr ? preview_host_->preview_font() : nullptr;
                        ImFont* preview = face != nullptr ? const_cast<ImFont*>(static_cast<const ImFont*>(face)) : font(20);
                        const float sample_size = preview->LegacySize;
                        const auto sample = [preview, sample_size](const char* value, bool muted = false) {
                            ImGui::PushFont(preview, sample_size);
                            if (muted) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                            ImGui::TextWrapped("%s", value);
                            if (muted) ImGui::PopStyleColor();
                            ImGui::PopFont();
                        };
                        gap(2);
                        const float paragraph_top = ImGui::GetCursorPosY();
                        sample("Clarity and rhythm matter when reading scene data, component values, and code inside the editor.");
                        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 12, paragraph_top + 56));
                        ImGui::Separator();
                        gap(12);
                        text(16, "Multilingual Support", true);
                        gap(4);
                        const float multilingual_top = ImGui::GetCursorPosY();
                        sample("中文字符预览：场景 组件 属性 资源");
                        sample("多语言文本在编辑器界面中应清晰易读。", true);
                        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 12, multilingual_top + 60));
                        ImGui::Separator();
                        gap(12);
                        text(16, "Code Sample", true);
                        gap(2);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 8));
                        // Include three lines, row gaps, padding and a possible
                        // horizontal scrollbar. A large font must not add a
                        // redundant vertical scrollbar inside this code block.
                        const float first_line_width = preview->CalcTextSizeA(sample_size, 10000, 0,
                            "for (const auto& entity : world.entities()) {").x;
                        const bool needs_horizontal_scroll = first_line_width + 90 > ImGui::GetContentRegionAvail().x;
                        const float code_height = std::max(104.0f, sample_size * 3 + 48 +
                            (needs_horizontal_scroll ? ImGui::GetStyle().ScrollbarSize + 6 : 0));
                        if (ImGui::BeginChild("FontPreviewCode", ImVec2(0, code_height), ImGuiChildFlags_Borders | padded,
                                              ImGuiWindowFlags_HorizontalScrollbar)) {
                            ImGui::PushFont(preview, sample_size);
                            const auto code_line = [](const char* number, const char* prefix, const char* keyword, const char* suffix) {
                                ImGui::TextDisabled("%s", number);
                                ImGui::SameLine(66);
                                ImGui::TextUnformatted(prefix);
                                ImGui::SameLine(0, 0);
                                ImGui::TextColored(ImVec4(0.32f, 0.68f, 0.96f, 1), "%s", keyword);
                                ImGui::SameLine(0, 0);
                                ImGui::TextUnformatted(suffix);
                            };
                            code_line("1", "", "for", " (const auto& entity : world.entities()) {");
                            code_line("2", "    world.", "destroy", "(entity);");
                            code_line("3", "}", "", "");
                            ImGui::PopFont();
                            const ImVec2 p = ImGui::GetWindowPos();
                            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 50, p.y + 14),
                                ImVec2(p.x + 50, p.y + code_height - 14), ImGui::GetColorU32(ImGuiCol_Border));
                        }
                        ImGui::EndChild();
                        ImGui::PopStyleVar(2);
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                }
                ImGui::PopStyleVar();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 0));
        if (ImGui::BeginChild("PreferenceActionBar", ImVec2(0, footer_height), padded, ImGuiWindowFlags_NoScrollbar)) {
            const ImVec2 p = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetWindowWidth(), p.y), ImGui::GetColorU32(ImGuiCol_Border));
            ImGui::PushFont(font(16), font(16)->LegacySize);
            if (ImGui::Button("Reset to Defaults", ImVec2(128 * unit, control_height))) {
                if (model_->reset_defaults()) preview_draft();
            }
            const float button_width = 72 * unit;
            const float actions_x = ImGui::GetWindowContentRegionMax().x - (button_width * 3 + 24);
            if (compact_footer) ImGui::SetCursorPos(ImVec2(actions_x, control_height + 24));
            else ImGui::SameLine(actions_x);
            if (ImGui::Button("Cancel", ImVec2(button_width, control_height))) {
                if (!actions_->cancel(model_->applied_state()))
                    static_cast<void>(model_->set_error("The preferences could not be cancelled. Please try again."));
            }
            ImGui::SameLine();
            const bool disabled = !state.dirty;
            ImGui::BeginDisabled(disabled);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.075f, 0.21f, 0.32f, 1));
            if (ImGui::Button("Apply", ImVec2(button_width, control_height))) {
                if (!actions_->apply(state))
                    static_cast<void>(model_->set_error("The preferences could not be applied. Please try again."));
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.77f, 0.86f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.86f, 0.94f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.03f, 0.62f, 0.72f, 1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.01f, 0.08f, 0.11f, 1));
            if (ImGui::Button("Save", ImVec2(button_width, control_height))) {
                if (!actions_->save(state))
                    static_cast<void>(model_->set_error("The preferences could not be saved. Please try again."));
            }
            ImGui::PopStyleColor(4);
            ImGui::EndDisabled();
            ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
    }
    ImGui::End();
    ImGui::PopFont();
}
}  // namespace aeg::editor
