/** @file editor_process_graph_canvas.cpp */
#include "editor_process_graph_canvas.hpp"

#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace aeg::application {
namespace {
constexpr float kWidth = 150.0f;
constexpr float kHeight = 58.0f;
constexpr float kCanvasHeight = 360.0f;

feature::ProcessStateMachine canvas_machine(const feature::ProcessSubgraph& process) {
    feature::ProcessStateMachine machine;
    machine.initial_state = process.initial_state.empty() && !process.states.empty()
        ? process.states.front() : process.initial_state;
    for (const auto& encoded : process.transitions) {
        const auto separator = encoded.find("->");
        if (separator != std::string::npos) {
            feature::ProcessTransition transition;
            transition.from_state = encoded.substr(0, separator);
            transition.to_state = encoded.substr(separator + 2);
            machine.transitions.push_back(std::move(transition));
        }
    }
    for (const auto& name : process.states) {
        feature::ProcessStateNode state;
        state.name = name;
        state.is_entry = name == machine.initial_state;
        state.is_terminal = process.terminal_states.empty()
            ? std::none_of(machine.transitions.begin(), machine.transitions.end(), [&](const auto& edge) {
                return edge.from_state == name;
              })
            : std::find(process.terminal_states.begin(), process.terminal_states.end(), name) != process.terminal_states.end();
        machine.states.push_back(std::move(state));
    }
    return machine;
}

void arrow(ImDrawList& draw, ImVec2 from, ImVec2 to, float zoom) {
    const auto color = IM_COL32(155, 174, 199, 255);
    const float dx = to.x - from.x, dy = to.y - from.y;
    const float distance = std::hypot(dx, dy);
    if (distance < 1.0f) return;
    const float ux = dx / distance, uy = dy / distance;
    draw.AddLine(from, to, color, 2.0f);
    const float size = 8.0f * zoom;
    draw.AddTriangleFilled(to, {to.x - ux * size - uy * size * 0.5f, to.y - uy * size + ux * size * 0.5f},
                           {to.x - ux * size + uy * size * 0.5f, to.y - uy * size - ux * size * 0.5f}, color);
}
} // namespace

void EditorProcessGraphCanvas::draw(EditorFeatureGraphDocument& document, feature::FeatureNodeId owner,
                                   std::string& feedback) {
    const auto snapshot = document.snapshot_owner();
    const auto found = std::find_if(snapshot->process_subgraphs.begin(), snapshot->process_subgraphs.end(),
                                  [&](const auto& process) { return process.owner_node == owner; });
    if (found == snapshot->process_subgraphs.end()) {
        snapshot_.reset(); selection_snapshot_.reset(); owner_ = {}; selected_state_.clear(); dragging_state_.clear(); linking_from_.clear(); panning_ = false;
        ImGui::TextDisabled("Add a state to this Process owner to open its state graph.");
        return;
    }
    if (snapshot_ != snapshot || owner_ != owner) {
        if (owner_ != owner || selection_snapshot_ != snapshot) selected_state_.clear();
        selection_snapshot_.reset();
        dragging_state_.clear(); linking_from_.clear(); panning_ = false;
        const auto loaded = read_process_editor_layout(*snapshot, owner);
        if (!loaded) { feedback = loaded.result.message; snapshot_.reset(); return; }
        layout_ = loaded.value;
        snapshot_ = snapshot; owner_ = owner;
    }
    const auto machine = canvas_machine(*found); // Incomplete authoring drafts are still visible.
    if (std::none_of(machine.states.begin(), machine.states.end(), [&](const auto& state) { return state.name == selected_state_; }))
        selected_state_.clear();
    for (std::size_t index = 0; index < machine.states.size(); ++index)
        layout_.state_positions.try_emplace(machine.states[index].name, 40.0f + static_cast<float>(index % 4) * 210.0f,
                                           45.0f + static_cast<float>(index / 4) * 110.0f);

    bool save_layout = false;
    if (ImGui::Button("Auto layout states")) {
        auto_layout_state_machine(machine, layout_, std::max(180.0f, ImGui::GetContentRegionAvail().x - kWidth - 40.0f),
                                  kCanvasHeight - kHeight - 40.0f);
        save_layout = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Zoom -")) { layout_.zoom = std::max(0.5f, layout_.zoom - 0.1f); save_layout = true; }
    ImGui::SameLine();
    if (ImGui::Button("Zoom +")) { layout_.zoom = std::min(2.0f, layout_.zoom + 0.1f); save_layout = true; }
    ImGui::SameLine();
    if (ImGui::Button("Reset view")) { layout_.zoom = 1.0f; layout_.pan_offset = {}; save_layout = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Snap to grid", &layout_.grid_snap)) save_layout = true;
    ImGui::TextDisabled("Drag states to arrange. Drag an output dot to another state to connect. Middle-drag to pan; Escape cancels.");

    const auto owner_id = std::to_string(owner.value());
    ImGui::PushID(owner_id.c_str());
    ImGui::BeginChild("ProcessCanvas", {0, kCanvasHeight}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto origin = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail();
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, {origin.x + available.x, origin.y + available.y}, true);
    const float zoom = layout_.zoom;
    const auto point = [&](std::pair<float, float> position) {
        return ImVec2{origin.x + layout_.pan_offset.first + position.first * zoom,
                      origin.y + layout_.pan_offset.second + position.second * zoom};
    };
    const float spacing = std::max(8.0f, layout_.grid_size * zoom);
    for (float x = std::fmod(layout_.pan_offset.first, spacing); x < available.x; x += spacing)
        draw->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + available.y}, IM_COL32(70, 76, 85, 70));
    for (float y = std::fmod(layout_.pan_offset.second, spacing); y < available.y; y += spacing)
        draw->AddLine({origin.x, origin.y + y}, {origin.x + available.x, origin.y + y}, IM_COL32(70, 76, 85, 70));
    for (const auto& transition : machine.transitions) {
        const auto from = layout_.state_positions.find(transition.from_state);
        const auto to = layout_.state_positions.find(transition.to_state);
        if (from == layout_.state_positions.end() || to == layout_.state_positions.end()) continue;
        const auto a = point(from->second), b = point(to->second);
        if (from == to) {
            const ImVec2 start{a.x + kWidth * zoom, a.y + kHeight * 0.5f * zoom};
            const ImVec2 end{a.x + kWidth * 0.5f * zoom, a.y};
            draw->AddBezierCubic(start, {start.x + 60 * zoom, start.y}, {end.x, end.y - 60 * zoom}, end,
                                 IM_COL32(155, 174, 199, 255), 2.0f);
            arrow(*draw, {end.x, end.y - 8 * zoom}, end, zoom);
        } else arrow(*draw, {a.x + kWidth * zoom, a.y + kHeight * 0.5f * zoom},
                           {b.x, b.y + kHeight * 0.5f * zoom}, zoom);
    }

    std::string link_target;
    bool hover_state = false;
    const bool canvas_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    // Hit the topmost card first; paint cards in the opposite order below.
    for (std::size_t remaining = machine.states.size(); remaining > 0; --remaining) {
        const auto state_index = remaining - 1;
        const auto& state = machine.states[state_index];
        auto& position = layout_.state_positions.at(state.name);
        const auto top_left = point(position);
        const ImVec2 bottom_right{top_left.x + kWidth * zoom, top_left.y + kHeight * zoom};
        const bool hovered = ImGui::IsMouseHoveringRect(top_left, bottom_right);
        hover_state |= hovered;
        if (link_target.empty() && canvas_hovered && hovered && !linking_from_.empty() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) link_target = state.name;
        // State names are literal data; ImGui's string-ID syntax reserves "###".
        ImGui::PushID(static_cast<int>(state_index));
        ImGui::SetCursorScreenPos({bottom_right.x - 19 * zoom, top_left.y + (kHeight * 0.5f - 9) * zoom});
        ImGui::InvisibleButton("output", {18 * zoom, 18 * zoom});
        if (ImGui::IsItemActivated()) { linking_from_ = state.name; selected_state_ = state.name; }
        ImGui::SetCursorScreenPos(top_left);
        ImGui::InvisibleButton("state", {kWidth * zoom, kHeight * zoom});
        if (ImGui::IsItemActivated()) {
            selected_state_ = state.name; dragging_state_ = state.name; drag_start_ = position;
        }
        if (dragging_state_ == state.name && ImGui::IsItemActive() && !ImGui::IsItemActivated()) {
            const auto delta = ImGui::GetIO().MouseDelta;
            position.first += delta.x / zoom; position.second += delta.y / zoom;
        }
        if (dragging_state_ == state.name && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (layout_.grid_snap && position != drag_start_) {
                position.first = std::round(position.first / layout_.grid_size) * layout_.grid_size;
                position.second = std::round(position.second / layout_.grid_size) * layout_.grid_size;
            }
            save_layout |= position != drag_start_;
            dragging_state_.clear();
        }
        ImGui::PopID();
    }
    for (const auto& state : machine.states) {
        const auto top_left = point(layout_.state_positions.at(state.name));
        const ImVec2 bottom_right{top_left.x + kWidth * zoom, top_left.y + kHeight * zoom};
        draw->AddRectFilled(top_left, bottom_right, selected_state_ == state.name ? IM_COL32(56, 90, 130, 255) : IM_COL32(45, 52, 66, 255), 5);
        draw->AddRect(top_left, bottom_right, state.is_terminal ? IM_COL32(115, 192, 140, 255) : IM_COL32(128, 151, 184, 255), 5, 0, state.is_entry ? 3.0f : 1.0f);
        draw->PushClipRect(top_left, bottom_right, true);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * zoom, {top_left.x + 8 * zoom, top_left.y + 8 * zoom}, IM_COL32_WHITE, state.name.c_str());
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * zoom, {top_left.x + 8 * zoom, top_left.y + 30 * zoom}, IM_COL32(169, 199, 216, 255),
                      state.is_entry && state.is_terminal ? "Entry / Terminal" : state.is_entry ? "Entry" : state.is_terminal ? "Terminal" : "State");
        draw->PopClipRect();
        draw->AddCircleFilled({bottom_right.x - 10 * zoom, top_left.y + kHeight * 0.5f * zoom}, 5 * zoom, IM_COL32(190, 208, 228, 255));
    }
    if (!linking_from_.empty()) {
        const auto a = point(layout_.state_positions.at(linking_from_));
        arrow(*draw, {a.x + kWidth * zoom, a.y + kHeight * 0.5f * zoom}, ImGui::GetIO().MousePos, zoom);
    }
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        panning_ = true; pan_start_ = layout_.pan_offset;
    }
    if (panning_) {
        const auto delta = ImGui::GetIO().MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            layout_.pan_offset.first += delta.x; layout_.pan_offset.second += delta.y;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) { save_layout |= layout_.pan_offset != pan_start_; panning_ = false; }
    }
    const bool cancel = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (cancel) { snapshot_.reset(); dragging_state_.clear(); linking_from_.clear(); panning_ = false; save_layout = false; link_target.clear(); }
    else if (ImGui::IsWindowHovered() && !hover_state && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected_state_.clear();
    draw->PopClipRect();
    ImGui::EndChild();
    ImGui::PopID();

    if (!link_target.empty()) {
        const auto result = document.add_process_transition(owner, linking_from_, link_target);
        feedback = result ? "Process transition added" : result.message;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) linking_from_.clear();
    if (save_layout) {
        const auto result = document.set_process_layout(owner, layout_);
        feedback = result ? "Process layout updated" : result.message;
        if (!result) snapshot_.reset();
    }
    if (!selected_state_.empty()) {
        ImGui::Text("Selected state: %s", selected_state_.c_str());
        if (ImGui::Button("Make selected entry")) {
            const auto result = document.set_process_initial_state(owner, selected_state_);
            feedback = result ? "Process entry updated" : result.message;
        }
        ImGui::SameLine();
        bool terminal = std::find(found->terminal_states.begin(), found->terminal_states.end(), selected_state_) != found->terminal_states.end();
        if (ImGui::Checkbox("Explicit terminal", &terminal)) {
            const auto result = document.set_process_terminal_state(owner, selected_state_, terminal);
            feedback = result ? "Process terminal updated" : result.message;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete selected state")) {
            const auto result = document.erase_process_state(owner, selected_state_);
            feedback = result ? "Process state deleted" : result.message;
            if (result) selected_state_.clear();
        }
    }
    if (document.snapshot_owner() != snapshot) selection_snapshot_ = document.snapshot_owner();
}
} // namespace aeg::application
