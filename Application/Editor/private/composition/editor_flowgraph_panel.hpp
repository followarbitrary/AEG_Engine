#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "editor_composition_commands.hpp"
#include "editor_composition_view.hpp"
#include "editor_node_definition_service.hpp"
#include "editor_window_chrome.hpp"

namespace aeg::application {

/**
 * @brief Immediate-mode FlowGraph canvas used by the editor application.
 *
 * The panel is deliberately backed by EditorCompositionDocument snapshots and
 * EditorCompositionCommands. It never edits a PipelineDocument directly, so
 * moving a node is a layout-only interaction while graph edits remain undoable
 * semantic transactions.
 */
class EditorFlowGraphPanel final {
public:
    struct Actions final {
        std::function<foundation::Result()> save_draft{};
        std::function<foundation::Result()> apply_to_viewport{};
        std::function<foundation::Result()> restore_default_viewport{};
        std::function<std::string()> runtime_status{};
        std::function<std::vector<EditorRuntimePlanView>()> runtime_inspect{};
        bool native_window{false};
        aeg::editor::EditorWindowChromeActions window_actions{};
    };

    void draw(EditorCompositionDocument&, const EditorGraphAddress&,
              const std::filesystem::path& project_root = {});
    void draw(EditorCompositionDocument&, const EditorGraphAddress&, const std::filesystem::path& project_root,
              const Actions& actions);
    [[nodiscard]] std::string_view last_action() const noexcept { return last_action_; }

private:
    struct Position final {
        ImVec2 value{};
        bool initialized{false};
    };

    std::unordered_map<std::string, Position> positions_{};
    EditorGraphAddress address_{};
    // Keep the last child graph so returning from the Pipeline tab restores
    // the exact Feature/Pass canvas instead of silently opening an empty
    // parent view.
    EditorGraphAddress last_child_address_{};
    bool has_last_child_address_{false};
    // Selecting Pipeline from a child graph is a temporary parent detour.  A
    // following selection of that same Feature row should return to the exact
    // child graph (including its Pass part) instead of opening the empty
    // Feature container.
    bool restore_child_after_pipeline_switch_{false};
    bool address_initialized_{false};
    const EditorCompositionDocument* document_identity_{nullptr};
    std::string position_scope_{};
    // A stale full-graph layout is migrated once per graph scope. Keeping
    // this marker prevents a deliberate user move outside the first viewport
    // from being mistaken for the old Resource-relay layout on every frame.
    std::string migrated_pass_layout_scope_{};
    std::string selected_node_{};
    std::string connect_source_node_{};
    uint64_t connect_source_port_{};
    uint64_t connect_source_slot_{};
    std::string selected_edge_{};
    std::string last_action_{};
    std::string definition_node_{};
    std::string pending_layout_node_{};
    bool definition_popup_requested_{false};
    bool add_node_popup_requested_{false};
    bool empty_node_popup_requested_{false};
    std::array<char, 160> custom_type_name_{};
    std::array<char, 128> palette_search_{};
    std::unordered_map<std::string, std::array<char, 128>> input_buffers_{};
    bool custom_type_name_initialized_{false};
    float zoom_{1.0f};
    // The palette is an authoring surface, so its width is intentionally
    // persisted in the panel instead of being hard-coded into the table.
    // Keep the range comfortable for both short and long node names.
    float palette_width_{220.0f};
    std::filesystem::path project_root_{};
    std::string scaffold_status_{};
    std::filesystem::path scaffold_implementation_{};
    bool show_runtime_inspect_{false};
    EditorNodeDefinitionService node_definition_service_{};
    // FlowGraph uses a signed viewport offset rather than ImGui child-window
    // scrolling. A small graph still needs to pan when its content does not
    // exceed the child window's ScrollMax.
    ImVec2 canvas_pan_{};
    bool canvas_pan_active_{false};
    bool canvas_pan_candidate_{false};
    ImVec2 canvas_pan_press_position_{};
    ImGuiMouseButton canvas_pan_button_{ImGuiMouseButton_Middle};
    NodeDefinitionDraft definition_draft_{};
    bool definition_window_open_{false};
    bool definition_window_minimized_{false};
    bool definition_window_maximized_{false};
    bool definition_read_only_{true};
    bool definition_disconnect_confirmed_{false};
    std::string definition_status_{};
    std::unordered_map<uint64_t, std::array<char, 160>> definition_name_buffers_{};
    std::unordered_map<uint64_t, std::array<char, 160>> definition_display_buffers_{};
    std::unordered_map<uint64_t, std::array<char, 160>> definition_parameter_buffers_{};
    std::unordered_map<uint64_t, std::array<char, 160>> definition_parameter_display_buffers_{};
    std::unordered_map<uint64_t, std::array<char, 160>> definition_parameter_default_buffers_{};

    void draw_node_definition_window(EditorCompositionDocument&, const EditorCompositionGraphView&,
                                     const std::filesystem::path&);
};

}  // namespace aeg::application
