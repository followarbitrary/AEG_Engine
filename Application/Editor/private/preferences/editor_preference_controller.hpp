#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "editor_preference_store.hpp"
#include "editor_preferences.hpp"
#include "font/editor_font_repository.hpp"

namespace aeg::application {

/** @brief Application callbacks used to refresh the Engine-owned font texture. */
struct EditorPreferenceControllerDesc final {
    EditorFontRepository* repository{nullptr};
    EditorPreferenceStore* store{nullptr};
    editor::EditorImGuiHost* imgui_host{nullptr};
    std::function<foundation::Result(std::string_view, float)> refresh_font_texture{};
    std::function<foundation::ResultValue<std::optional<std::filesystem::path>>()> open_font_dialog{};
};

/** @brief Coordinates user font transactions with the Framework preference draft. */
class EditorPreferenceController final : public editor::IEditorPreferenceActions {
public:
    explicit EditorPreferenceController(EditorPreferenceControllerDesc desc) noexcept : desc_(std::move(desc)) {}

    [[nodiscard]] foundation::Result initialize(const editor::EditorFontCatalog& bundled_catalog,
                                                std::string_view default_font_key, float default_font_size = 18.0f);
    [[nodiscard]] foundation::Result request_import_font();
    [[nodiscard]] foundation::Result request_remove_font(std::string_view font_key);
    [[nodiscard]] foundation::Result reset_defaults();

    [[nodiscard]] const editor::EditorPreferenceModel* model() const noexcept { return model_.get(); }
    [[nodiscard]] editor::EditorPreferenceModel* model() noexcept { return model_.get(); }
    [[nodiscard]] const editor::EditorFontCatalog& catalog() const noexcept { return working_catalog_; }

    [[nodiscard]] foundation::Result preview(const editor::EditorPreferenceState& state) override;
    [[nodiscard]] foundation::Result apply(const editor::EditorPreferenceState& state) override;
    [[nodiscard]] foundation::Result save(const editor::EditorPreferenceState& state) override;
    [[nodiscard]] foundation::Result cancel(const editor::EditorPreferenceState& state) override;

private:
    [[nodiscard]] foundation::Result rebuild_working_catalog();
    [[nodiscard]] foundation::Result refresh_font_texture(std::string_view key, float size) const;
    [[nodiscard]] foundation::Result restore_applied_state();
    [[nodiscard]] std::vector<editor::EditorFontDescriptor> descriptors(const editor::EditorFontCatalog& catalog) const;

    EditorPreferenceControllerDesc desc_{};
    editor::EditorFontCatalog bundled_catalog_{};
    editor::EditorFontCatalog working_catalog_{};
    editor::EditorFontCatalog applied_catalog_{};
    std::vector<EditorFontFileRecord> user_records_{};
    std::vector<EditorFontFileRecord> applied_user_records_{};
    std::unique_ptr<editor::EditorPreferenceModel> model_{};
    std::string default_font_key_{};
    float default_font_size_{18.0f};
    bool initialized_{false};
};

}  // namespace aeg::application
