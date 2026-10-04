/**
 * @file editor_workspace.hpp
 * @brief Application-owned customizable editor workspace orchestration.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "editor_imgui.hpp"
#include "editor_layout.hpp"
#include "editor_panel_descriptor.hpp"
#include "result.hpp"

namespace aeg::editor {
/** @brief Describes one application command that may be exposed in the workspace View menu. */
struct EditorWorkspaceActionDescriptor final {
    /** @brief Stable machine-readable action key. */
    std::string key{};
    /** @brief User-visible menu title. */
    std::string title{};
    /** @brief Application callback invoked after the menu item is selected. */
    std::function<foundation::Result()> callback{};
    /** @brief Whether this command is listed under View rather than invoked by another control. */
    bool show_in_view_menu{true};
    /** @brief Optional live availability check; unavailable commands remain discoverable but disabled. */
    std::function<bool()> enabled{};

    /** @brief Returns whether the action can be registered. */
    [[nodiscard]] bool is_valid() const noexcept { return !key.empty() && !title.empty() && callback != nullptr; }
};

/** @brief Application callbacks used by the client-drawn window controls. */
struct EditorWindowControls final {
    std::function<foundation::Result()> minimize{};
    std::function<foundation::Result()> toggle_maximize{};
    std::function<foundation::Result()> close{};
};

/** @brief Coordinates panel registration, semantic layout state, and ImGui docking. */
class EditorWorkspace final {
public:
    /** @brief Chooses whether a panel remains in the editor dockspace or owns a native platform viewport. */
    enum class PanelPresentation : uint8_t {
        kDockWindow,
        kNativeWindow,
    };
    /** @brief Creates a workspace that borrows one initialized ImGui host. */
    explicit EditorWorkspace(EditorImGuiHost& imgui_host) noexcept;

    EditorWorkspace(const EditorWorkspace&) = delete;
    EditorWorkspace& operator=(const EditorWorkspace&) = delete;
    EditorWorkspace(EditorWorkspace&&) = delete;
    EditorWorkspace& operator=(EditorWorkspace&&) = delete;

    /** @brief Registers one panel and adds its descriptor to layout persistence. */
    [[nodiscard]] foundation::Result register_panel(EditorPanelDescriptor descriptor,
                                                    std::unique_ptr<EditorPanel> panel,
                                                    std::function<void(bool)> sync_visibility = {});
    /** @brief Removes one panel from the host and semantic layout. */
    [[nodiscard]] foundation::Result unregister_panel(std::string_view panel_key);
    /** @brief Changes visibility while preserving the panel's dock or floating placement. */
    [[nodiscard]] foundation::Result set_panel_visible(std::string_view panel_key, bool visible);
    /** @brief Changes a user-rearrangeable panel between the default dockspace and a native viewport. */
    [[nodiscard]] foundation::Result set_panel_presentation(std::string_view panel_key, PanelPresentation presentation);
    /** @brief Returns the selected presentation; panels start in the dockspace. */
    [[nodiscard]] PanelPresentation panel_presentation(std::string_view panel_key) const noexcept;
    /** @brief Registers one application command without adding a workspace panel. */
    [[nodiscard]] foundation::Result register_view_action(EditorWorkspaceActionDescriptor descriptor);
    /** @brief Installs application-owned File/Edit menu content in the workspace navigation row. */
    void set_main_menu_content(std::function<void()> content) { main_menu_content_ = std::move(content); }
    /** @brief Installs application-owned callbacks for the custom title-bar controls. */
    void set_window_controls(EditorWindowControls controls) { window_controls_ = std::move(controls); }
    /** @brief Invokes one registered View-menu command by stable key. */
    [[nodiscard]] foundation::Result invoke_view_action(std::string_view action_key);
    /** @brief Builds the dockspace, workspace menu, and currently visible panels. */
    [[nodiscard]] foundation::Result build_ui();
    /** @brief Vertical space reserved by the brand, navigation, and command rows. */
    [[nodiscard]] static constexpr float chrome_height() noexcept { return 104.0f; }
    /** @brief Space reserved at the right edge for client-drawn window controls. */
    [[nodiscard]] static constexpr float title_control_width() noexcept { return 126.0f; }
    void set_preferences_open(bool open) noexcept { preferences_open_ = open; }
    [[nodiscard]] bool preferences_open() const noexcept { return preferences_open_; }
    void set_playing(bool playing) noexcept { playing_ = playing; }
    void set_paused(bool paused) noexcept { paused_ = paused; }
    /** @brief Sets a user-sized navigation rail in pixels; zero restores automatic sizing. */
    void set_navigation_width(float width) noexcept { navigation_width_ = width > 0.0f ? width : 0.0f; }
    /** @brief Supplies the latest draw-call count from the open game project viewport. */
    void set_game_draw_calls(uint32_t draw_calls) noexcept { game_draw_calls_ = draw_calls; }
    /** @brief Serializes the current semantic layout for an Application-owned store. */
    [[nodiscard]] foundation::Result serialize_layout(std::string& serialized) const;
    /** @brief Replaces the semantic layout from a previously serialized snapshot. */
    [[nodiscard]] foundation::Result deserialize_layout(std::string_view serialized);

    /** @brief Returns the current semantic layout when at least one panel is registered. */
    [[nodiscard]] const EditorLayoutModel* layout() const noexcept;
    /** @brief Returns the number of registered panels. */
    [[nodiscard]] std::size_t panel_count() const noexcept;
    /** @brief Returns the number of registered View-menu commands. */
    [[nodiscard]] std::size_t view_action_count() const noexcept;
    /** @brief Returns the number of registered commands visible in the View menu. */
    [[nodiscard]] std::size_t view_menu_action_count() const noexcept;

private:
    struct PanelEntry final {
        /** @brief Stable semantic metadata used to map the panel to an ImGui window. */
        EditorPanelDescriptor descriptor{};
        /** @brief Generation-checked identity owned by the ImGui host. */
        EditorPanelId panel_id{};
        /** @brief Mirrors workspace visibility into panels with local visibility state. */
        std::function<void(bool)> sync_visibility{};
    };

    [[nodiscard]] PanelEntry* find_panel(std::string_view panel_key) noexcept;
    [[nodiscard]] const PanelEntry* find_panel(std::string_view panel_key) const noexcept;
    [[nodiscard]] EditorWorkspaceActionDescriptor* find_view_action(std::string_view action_key) noexcept;
    [[nodiscard]] const EditorWorkspaceActionDescriptor* find_view_action(std::string_view action_key) const noexcept;
    [[nodiscard]] foundation::Result initialize_dockspace();
    [[nodiscard]] foundation::Result sync_host_visibility();
    [[nodiscard]] bool is_native_panel(std::string_view panel_key) const noexcept;

    /** @brief Borrowed ImGui host that owns panel instances and the active context. */
    EditorImGuiHost* imgui_host_{nullptr};
    /** @brief Application-owned semantic layout state. */
    std::unique_ptr<EditorLayoutModel> layout_{};
    /** @brief Registered panels in deterministic application order. */
    std::vector<PanelEntry> panels_{};
    /** @brief Panels currently requested as independent native viewports. */
    std::vector<std::string> native_panel_keys_{};
    /** @brief Application commands shown in the workspace View menu. */
    std::vector<EditorWorkspaceActionDescriptor> view_actions_{};
    std::function<void()> main_menu_content_{};
    EditorWindowControls window_controls_{};
    /** @brief True after the current layout has been applied to the ImGui dockspace. */
    bool dockspace_initialized_{false};
    bool building_ui_{false};
    bool preferences_open_{false};
    bool playing_{false};
    bool paused_{false};
    float navigation_width_{0.0f};
    uint32_t game_draw_calls_{0};
    std::string active_rail_{"Scene"};
    std::string pending_panel_focus_{};
};
}  // namespace aeg::editor
