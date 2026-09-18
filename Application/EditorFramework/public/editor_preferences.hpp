/**
 * @file editor_preferences.hpp
 * @brief Backend-neutral editor preference state and Preferences panel.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor_font.hpp"
#include "editor_panel.hpp"
#include "result.hpp"

namespace aeg::editor {
class EditorImGuiHost;
/** @brief Draft and applied settings exposed by the editor Preferences panel. */
struct EditorPreferenceState final {
    /** @brief Fonts currently available to the editor. */
    std::vector<EditorFontDescriptor> fonts{};
    /** @brief Stable key of the selected editor font. */
    std::string active_font_key{};
    /** @brief Global editor font size in pixels. */
    float font_size_pixels{16.0f};
    /** @brief Whether the draft differs from the last accepted settings. */
    bool dirty{false};
    /** @brief Last user-visible operation error. */
    std::string error_message{};
};

/** @brief Transactional draft model for editor-only Preferences changes. */
class EditorPreferenceModel final {
public:
    /** @brief Creates a model whose initial state is both applied and selected as the reset default. */
    explicit EditorPreferenceModel(EditorPreferenceState applied);

    /** @brief Returns the current draft state without transferring ownership. */
    [[nodiscard]] const EditorPreferenceState& state() const noexcept;
    /** @brief Returns the last accepted state used as the Cancel target. */
    [[nodiscard]] const EditorPreferenceState& applied_state() const noexcept;
    /** @brief Selects one registered font in the draft state. */
    [[nodiscard]] foundation::Result select_font(std::string_view font_key);
    /** @brief Updates the global editor font size in the draft state. */
    [[nodiscard]] foundation::Result set_font_size(float size_pixels);
    /** @brief Restores the initial font and size values in the draft state. */
    [[nodiscard]] foundation::Result reset_defaults();
    /** @brief Accepts the draft as the new applied state. */
    void accept();
    /** @brief Discards the draft and restores the last applied state. */
    void discard();
    /** @brief Stores a diagnostic for display in the Preferences panel. */
    [[nodiscard]] foundation::Result set_error(std::string_view message);
    /** @brief Clears the current diagnostic. */
    void clear_error() noexcept;
    /** @brief Replaces the draft font descriptors while preserving its selected key. */
    [[nodiscard]] foundation::Result set_draft_fonts(std::span<const EditorFontDescriptor> fonts);

private:
    void refresh_dirty() noexcept;

    /** @brief Last accepted preference values used by Cancel. */
    EditorPreferenceState applied_{};
    /** @brief Mutable values edited by the Preferences panel. */
    EditorPreferenceState draft_{};
    /** @brief Initial font key used by Reset Defaults. */
    std::string default_font_key_{};
    /** @brief Initial font size used by Reset Defaults. */
    float default_font_size_pixels_{16.0f};
};

/** @brief Application-owned actions invoked by the backend-neutral Preferences panel. */
class IEditorPreferenceActions {
public:
    virtual ~IEditorPreferenceActions() = default;

    /** @brief Requests the Application-owned font file picker and staged import. */
    [[nodiscard]] virtual foundation::Result request_import_font() {
        return foundation::invalid_state("Editor font import is unavailable");
    }
    /** @brief Requests removal of one user font after the panel confirms it. */
    [[nodiscard]] virtual foundation::Result request_remove_font(std::string_view) {
        return foundation::invalid_state("Editor font removal is unavailable");
    }

    /** @brief Requests a transient preview in the independent Preferences window without applying the editor state. */
    [[nodiscard]] virtual foundation::Result preview(const EditorPreferenceState& state) = 0;
    /** @brief Requests that the Application apply the draft during a safe frame and keep Preferences open. */
    [[nodiscard]] virtual foundation::Result apply(const EditorPreferenceState& state) = 0;
    /** @brief Requests that the Application save the draft during a safe frame and close Preferences. */
    [[nodiscard]] virtual foundation::Result save(const EditorPreferenceState& state) = 0;
    /** @brief Requests that the Application restore the last applied editor font and close Preferences. */
    [[nodiscard]] virtual foundation::Result cancel(const EditorPreferenceState& state) = 0;
};

/** @brief Dockable and floatable editor Preferences panel. */
class EditorPreferencePanel final : public EditorPanel {
public:
    /** @brief Creates a panel borrowing a model, action bridge, and preview font host. */
    EditorPreferencePanel(EditorPreferenceModel& model, IEditorPreferenceActions& actions,
                          EditorImGuiHost* preview_host = nullptr) noexcept;

    /** @brief Builds category navigation and the Fonts preference page. */
    void build_ui() override;

private:
    enum class Page : uint8_t {
        kGeneral,
        kFonts,
        kInput,
    };

    /** @brief Borrowed transactional state model. */
    EditorPreferenceModel* model_{nullptr};
    /** @brief Borrowed Application-side command bridge. */
    IEditorPreferenceActions* actions_{nullptr};
    /** @brief Borrowed ImGui host that owns the isolated preview font. */
    EditorImGuiHost* preview_host_{nullptr};
    /** @brief Currently selected Preferences category. */
    Page page_{Page::kFonts};
};
}  // namespace aeg::editor
