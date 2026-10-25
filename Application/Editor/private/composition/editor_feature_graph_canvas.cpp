#include "editor_feature_graph_canvas.hpp"
#include "editor_feature_presentation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <imgui.h>

namespace aeg::application {
namespace {
constexpr float kNodeWidth = 260.0f;
constexpr float kCanvasHeight = 390.0f;
constexpr float kPortTop = 70.0f;
constexpr float kPortSpacing = 26.0f;

const feature::FeatureCatalogEntry* entry_for(const feature::FeatureCatalog* catalog, const feature::FeatureNode& node) {
    if (!catalog) return nullptr;
    const auto found = std::find_if(catalog->entries().begin(), catalog->entries().end(), [&](const auto& entry) {
        return entry.definition_id == node.definition_id && entry.version == node.definition_version;
    });
    return found == catalog->entries().end() ? nullptr : &*found;
}

float height_for(const feature::FeatureCatalogEntry* entry) {
    if (!entry) return 115.0f;
    std::size_t inputs = 0, outputs = 0;
    for (const auto& port : entry->ports)
        (port.direction == feature::PortDirection::kInput ? inputs : outputs)++;
    return std::max(115.0f, kPortTop + kPortSpacing * static_cast<float>(std::max(inputs, outputs)));
}

std::optional<std::pair<float, float>> port_position(const feature::FeatureCatalogEntry* entry,
    feature::PortId id, feature::PortDirection direction) {
    if (!entry) return {};
    std::size_t row = 0;
    for (const auto& port : entry->ports) {
        if (port.direction != direction) continue;
        if (port.id == id) return std::pair{direction == feature::PortDirection::kInput ? 10.0f : kNodeWidth - 10.0f,
                                         kPortTop + static_cast<float>(row) * kPortSpacing};
        ++row;
    }
    return {};
}

void wire(ImDrawList& draw, ImVec2 from, ImVec2 to, ImU32 color, float thickness = 2.0f) {
    const float bend = std::max(45.0f, std::abs(to.x - from.x) * 0.45f);
    draw.AddBezierCubic(from, {from.x + bend, from.y}, {to.x - bend, to.y}, to, color, thickness);
}

void visible_text(ImDrawList& draw, ImVec2 at, const std::string& text, float scale, ImU32 color) {
    draw.AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, at, color, text.c_str());
    ImGui::LogText("%s\n", text.c_str());
}
} // namespace

void EditorFeatureGraphCanvas::draw(EditorFeatureGraphDocument& document, const feature::FeatureCatalog* catalog,
                                   std::string& selected_node, std::string& feedback,
                                   std::span<const NodeDefinitionDraft> presentations) {
    const auto snapshot = document.snapshot_owner();
    bool reset_selection = false;
    if (document_ != &document || snapshot_ != snapshot) {
        reset_selection = document_ != &document || selection_snapshot_ != snapshot;
        if (reset_selection) selected_.clear();
        dragging_ = {}; linking_ = {}; panning_ = false;
        const auto loaded = read_feature_editor_layout(*snapshot);
        if (!loaded) {
            ImGui::TextColored({1.0f, 0.5f, 0.3f, 1.0f}, "Feature canvas: %s", loaded.result.message.c_str());
            feedback = loaded.result.message; snapshot_.reset(); return;
        }
        layout_ = loaded.value; snapshot_ = snapshot; document_ = &document;
    }
    const auto& graph = *snapshot;
    if (reset_selection || selected_node != primary_selection_) {
        // External selection is one authored node, even when it has several
        // domain projections. Do not collapse a canvas Ctrl multi-selection
        // merely because its unchanged primary identity is passed back in.
        selected_.clear();
        const auto selected = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) {
            return std::to_string(node.node_id.value()) == selected_node;
        });
        if (selected != graph.nodes.end()) selected_.insert(selected->node_id.value());
        else selected_node.clear();
        if (dragging_.is_valid() || panning_) layout_ = drag_start_;
        dragging_ = {}; linking_ = {}; panning_ = false;
    }
    for (std::size_t index = 0; index < graph.nodes.size(); ++index)
        layout_.node_positions.try_emplace(graph.nodes[index].node_id.value(), default_feature_node_position(index));
    std::erase_if(selected_, [&](uint64_t id) {
        return std::none_of(graph.nodes.begin(), graph.nodes.end(), [id](const auto& node) { return node.node_id.value() == id; });
    });

    bool save_layout = false, copy = false, erase = false, frame_all = false, focus_selected = false;
    ImGui::PushID("Main Feature canvas tools");
    ImGui::TextUnformatted("Feature graph canvas");
    if (ImGui::Button("Layout nodes")) {
        for (std::size_t index = 0; index < graph.nodes.size(); ++index)
            layout_.node_positions[graph.nodes[index].node_id.value()] = default_feature_node_position(index);
        save_layout = true;
    }
    ImGui::SameLine(); if (ImGui::Button("Frame all (A)")) frame_all = true;
    ImGui::SameLine(); if (ImGui::Button("Focus selected (F)")) focus_selected = true;
    ImGui::SameLine(); if (ImGui::Button("Zoom -")) { layout_.zoom = std::max(0.2f, layout_.zoom / 1.2f); save_layout = true; }
    ImGui::SameLine(); if (ImGui::Button("Zoom +")) { layout_.zoom = std::min(3.0f, layout_.zoom * 1.2f); save_layout = true; }
    ImGui::SameLine(); if (ImGui::Button("Reset view")) { layout_.zoom = 1.0f; layout_.pan_offset = {}; save_layout = true; }
    const char* kinds[] = {"Data", "Snapshot", "Queue", "Event", "Command", "Receipt"};
    ImGui::SetNextItemWidth(140); ImGui::Combo("Connection kind", &edge_kind_, kinds, IM_ARRAYSIZE(kinds));
    if (edge_kind_ == static_cast<int>(feature::EdgeKind::kQueue)) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::InputInt("Capacity", &edge_capacity_);
    }
    if (edge_kind_ == static_cast<int>(feature::EdgeKind::kSnapshot) || edge_kind_ == static_cast<int>(feature::EdgeKind::kCommand)) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::InputInt("Deadline ticks", &edge_deadline_);
        ImGui::SameLine(); ImGui::TextDisabled("Deadline unit: source Clock periods");
    }
    ImGui::TextDisabled("Drag headers to move; output to input connects. Ctrl+click selects multiple; Ctrl+D copies; Delete removes. Middle/Space drag pans; Ctrl+wheel zooms; Esc cancels.");
    ImGui::PopID();

    ImGui::BeginChild("FeatureGraphCanvas", {0, kCanvasHeight}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto origin = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail();
    auto* draw = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool focused = ImGui::IsWindowFocused();
    auto& io = ImGui::GetIO();
    const bool shortcuts = focused && !io.WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    const auto point = [&](std::pair<float, float> position) {
        return ImVec2{origin.x + layout_.pan_offset.first + position.first * layout_.zoom,
                      origin.y + layout_.pan_offset.second + position.second * layout_.zoom};
    };
    const auto fit = [&](bool only_selected) {
        float left = std::numeric_limits<float>::max(), top = left;
        float right = std::numeric_limits<float>::lowest(), bottom = right;
        for (const auto& node : graph.nodes) {
            if (only_selected && !selected_.contains(node.node_id.value())) continue;
            const auto position = layout_.node_positions.at(node.node_id.value());
            left = std::min(left, position.first); top = std::min(top, position.second);
            right = std::max(right, position.first + kNodeWidth);
            bottom = std::max(bottom, position.second + height_for(entry_for(catalog, node)));
        }
        if (right < left || bottom < top) return;
        layout_.zoom = std::clamp(std::min(std::max(1.0f, available.x - 60) / std::max(1.0f, right - left),
                                          std::max(1.0f, available.y - 60) / std::max(1.0f, bottom - top)), 0.2f, 3.0f);
        layout_.pan_offset = {available.x * 0.5f - (left + right) * 0.5f * layout_.zoom,
                              available.y * 0.5f - (top + bottom) * 0.5f * layout_.zoom};
        save_layout = true;
    };
    if (frame_all || (shortcuts && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))) fit(false);
    if (focus_selected || (shortcuts && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F))) fit(true);
    if (hovered && io.KeyCtrl && io.MouseWheel != 0) {
        const auto before = layout_.zoom;
        layout_.zoom = std::clamp(before * std::pow(1.2f, io.MouseWheel), 0.2f, 3.0f);
        const float ratio = layout_.zoom / before;
        layout_.pan_offset = {io.MousePos.x - origin.x - (io.MousePos.x - origin.x - layout_.pan_offset.first) * ratio,
                              io.MousePos.y - origin.y - (io.MousePos.y - origin.y - layout_.pan_offset.second) * ratio};
        save_layout = layout_.zoom != before;
    }

    draw->PushClipRect(origin, {origin.x + available.x, origin.y + available.y}, true);
    const float spacing = 24.0f * layout_.zoom;
    for (float x = std::fmod(layout_.pan_offset.first, spacing); x < available.x; x += spacing)
        draw->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + available.y}, IM_COL32(65, 72, 84, 70));
    for (float y = std::fmod(layout_.pan_offset.second, spacing); y < available.y; y += spacing)
        draw->AddLine({origin.x, origin.y + y}, {origin.x + available.x, origin.y + y}, IM_COL32(65, 72, 84, 70));
    for (const auto& edge : graph.edges) {
        const auto source = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) { return node.node_id == edge.source_node; });
        const auto target = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) { return node.node_id == edge.target_node; });
        if (source == graph.nodes.end() || target == graph.nodes.end()) continue;
        const auto from_port = port_position(entry_for(catalog, *source), edge.source_port, feature::PortDirection::kOutput);
        const auto to_port = port_position(entry_for(catalog, *target), edge.target_port, feature::PortDirection::kInput);
        if (!from_port || !to_port) continue;
        const auto from = layout_.node_positions.at(source->node_id.value()), to = layout_.node_positions.at(target->node_id.value());
        wire(*draw, point({from.first + from_port->first, from.second + from_port->second}),
             point({to.first + to_port->first, to.second + to_port->second}), IM_COL32(130, 190, 220, 255));
    }

    std::optional<PortSelection> dropped;
    bool hover_node = false;
    // Hit topmost cards first. All IDs include the complete authored identity.
    for (std::size_t remaining = graph.nodes.size(); remaining > 0; --remaining) {
        const auto& node = graph.nodes[remaining - 1];
        const auto* entry = entry_for(catalog, node);
        const auto top_left = point(layout_.node_positions.at(node.node_id.value()));
        const float zoom = layout_.zoom;
        const bool inside = hovered && ImGui::IsMouseHoveringRect(top_left,
            {top_left.x + kNodeWidth * zoom, top_left.y + height_for(entry) * zoom});
        hover_node |= inside;
        const auto node_text = std::to_string(node.node_id.value());
        ImGui::PushID(node_text.c_str());
        ImGui::SetCursorScreenPos(top_left);
        ImGui::InvisibleButton("node", {kNodeWidth * zoom, 38.0f * zoom});
        if (ImGui::IsItemActivated() && !ImGui::IsKeyDown(ImGuiKey_Space)) {
            if (io.KeyCtrl) {
                if (!selected_.erase(node.node_id.value())) selected_.insert(node.node_id.value());
            } else if (!selected_.contains(node.node_id.value())) {
                selected_.clear(); selected_.insert(node.node_id.value());
            }
            selected_node = selected_.contains(node.node_id.value()) ? node_text :
                            (selected_.empty() ? std::string{} : std::to_string(*selected_.begin()));
            if (selected_.contains(node.node_id.value())) { dragging_ = node.node_id; drag_start_ = layout_; }
        }
        if (dragging_ == node.node_id && ImGui::IsItemActive() && !ImGui::IsItemActivated()) {
            for (const auto id : selected_) {
                auto& position = layout_.node_positions.at(id);
                position.first += io.MouseDelta.x / zoom; position.second += io.MouseDelta.y / zoom;
            }
        }
        if (entry) for (const auto& port : entry->ports) {
            const auto offset = *port_position(entry, port.id, port.direction);
            const auto center = point({layout_.node_positions.at(node.node_id.value()).first + offset.first,
                                        layout_.node_positions.at(node.node_id.value()).second + offset.second});
            const auto port_text = std::to_string(port.id.value());
            ImGui::PushID(port_text.c_str());
            ImGui::SetCursorScreenPos({center.x - 9 * zoom, center.y - 9 * zoom});
            ImGui::InvisibleButton("port", {18 * zoom, 18 * zoom});
            if (ImGui::IsItemActivated() && !ImGui::IsKeyDown(ImGuiKey_Space)) {
                linking_ = {node.node_id, port}; selected_node = node_text;
                if (!selected_.contains(node.node_id.value())) {
                    selected_.clear(); selected_.insert(node.node_id.value());
                }
            }
            if (linking_.node.is_valid() && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) dropped = PortSelection{node.node_id, port};
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip(); ImGui::Text("Port %llu | schema %llu | version %u",
                    static_cast<unsigned long long>(port.id.value()), static_cast<unsigned long long>(port.schema.value()), port.version);
                ImGui::Text("Clock %llu | %u Hz", static_cast<unsigned long long>(entry->clock.value()), entry->contract.clock.frequency_hz);
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    for (const auto& node : graph.nodes) {
        const auto* entry = entry_for(catalog, node);
        const auto top_left = point(layout_.node_positions.at(node.node_id.value()));
        const float zoom = layout_.zoom;
        const ImVec2 bottom{top_left.x + kNodeWidth * zoom, top_left.y + height_for(entry) * zoom};
        draw->AddRectFilled(top_left, bottom, selected_.contains(node.node_id.value()) ? IM_COL32(48, 78, 112, 255) : IM_COL32(39, 47, 61, 255), 5);
        draw->AddRect(top_left, bottom, entry ? IM_COL32(125, 154, 190, 255) : IM_COL32(230, 130, 90, 255), 5);
        draw->PushClipRect(top_left, bottom, true);
        const bool named = entry && feature_presentation(*entry, presentations);
        draw->PushClipRect(top_left, {top_left.x + kNodeWidth * zoom, top_left.y + 65 * zoom}, true);
        visible_text(*draw, {top_left.x + 8 * zoom, top_left.y + 7 * zoom},
            named ? feature_presentation_name(*entry, presentations) : "Node " + std::to_string(node.node_id.value()),
            zoom, IM_COL32_WHITE);
        if (named) visible_text(*draw, {top_left.x + 8 * zoom, top_left.y + 27 * zoom},
            "Node " + std::to_string(node.node_id.value()), zoom * 0.8f, IM_COL32(175, 198, 224, 255));
        visible_text(*draw, {top_left.x + 8 * zoom, top_left.y + (named ? 45 : 29) * zoom},
            "Definition " + std::to_string(node.definition_id.value()), zoom * 0.8f, IM_COL32(175, 198, 224, 255));
        draw->PopClipRect();
        if (!entry) visible_text(*draw, {top_left.x + 8 * zoom, top_left.y + 65 * zoom}, "Contract missing or stale", zoom, IM_COL32(240, 160, 100, 255));
        else for (const auto& port : entry->ports) {
            const auto offset = *port_position(entry, port.id, port.direction);
            const auto center = point({layout_.node_positions.at(node.node_id.value()).first + offset.first,
                                        layout_.node_positions.at(node.node_id.value()).second + offset.second});
            draw->AddCircleFilled(center, 5.0f * zoom, port.direction == feature::PortDirection::kInput ? IM_COL32(140, 200, 170, 255) : IM_COL32(150, 180, 235, 255));
            const auto text = std::to_string(port.id.value());
            const float size = ImGui::CalcTextSize(text.c_str()).x * zoom * 0.65f;
            visible_text(*draw, {port.direction == feature::PortDirection::kInput ? center.x + 9 * zoom : center.x - size - 9 * zoom,
                                center.y - ImGui::GetFontSize() * zoom * 0.35f}, text, zoom * 0.65f, IM_COL32(204, 215, 227, 255));
        }
        draw->PopClipRect();
    }
    if (linking_.node.is_valid()) {
        const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& value) { return value.node_id == linking_.node; });
        if (node != graph.nodes.end()) {
            const auto offset = port_position(entry_for(catalog, *node), linking_.port.id, linking_.port.direction);
            if (offset) {
                const auto position = layout_.node_positions.at(node->node_id.value());
                const auto start = point({position.first + offset->first, position.second + offset->second});
                if (linking_.port.direction == feature::PortDirection::kOutput) wire(*draw, start, io.MousePos, IM_COL32(220, 210, 140, 255));
                else wire(*draw, io.MousePos, start, IM_COL32(220, 210, 140, 255));
            }
        }
    }
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
        (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantTextInput))) {
        panning_ = true; drag_start_ = layout_; dragging_ = {}; linking_ = {};
    }
    if (panning_) {
        if ((ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) &&
            !ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            layout_.pan_offset.first += io.MouseDelta.x; layout_.pan_offset.second += io.MouseDelta.y;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            save_layout |= layout_ != drag_start_; panning_ = false;
        }
    }
    if (dragging_.is_valid() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        save_layout |= layout_ != drag_start_; dragging_ = {};
    }
    if (hovered && !hover_node && !panning_ && !linking_.node.is_valid() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyCtrl) {
        selected_.clear(); selected_node.clear();
    }
    if (shortcuts && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (dragging_.is_valid() || panning_) layout_ = drag_start_;
        dragging_ = {}; linking_ = {}; panning_ = false; dropped.reset(); save_layout = false;
    }
    if (shortcuts && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) copy = !selected_.empty();
    if (shortcuts && ImGui::IsKeyPressed(ImGuiKey_Delete)) erase = !selected_.empty();
    draw->PopClipRect();
    ImGui::SetCursorScreenPos(origin); ImGui::Dummy(available);
    ImGui::EndChild();

    if (!selected_.empty()) {
        ImGui::PushID("Main Feature canvas selection");
        ImGui::Text("Selected nodes: %zu", selected_.size());
        ImGui::SameLine(); copy |= ImGui::SmallButton("Copy selected");
        ImGui::SameLine(); erase |= ImGui::SmallButton("Delete selected");
        ImGui::PopID();
    }
    if (dropped) {
        auto source = linking_, target = *dropped;
        if (source.port.direction == feature::PortDirection::kInput) std::swap(source, target);
        if (source.port.direction != feature::PortDirection::kOutput || target.port.direction != feature::PortDirection::kInput || !catalog)
            feedback = "Connect an output to a compatible input";
        else {
            const auto source_node = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) { return node.node_id == source.node; });
            const auto target_node = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) { return node.node_id == target.node; });
            const auto* source_entry = entry_for(catalog, *source_node);
            const auto* target_entry = entry_for(catalog, *target_node);
            const auto kind = static_cast<feature::EdgeKind>(edge_kind_);
            // Capacity and deadline belong to their respective transport
            // contracts. Never serialize stale values from hidden controls.
            const auto capacity = kind == feature::EdgeKind::kQueue
                ? static_cast<uint32_t>(std::max(0, edge_capacity_)) : 0u;
            const auto deadline = (kind == feature::EdgeKind::kSnapshot || kind == feature::EdgeKind::kCommand)
                ? static_cast<uint32_t>(std::max(0, edge_deadline_)) : 0u;
            feature::FeatureEdge edge{source.node, source.port.id, target.node, target.port.id,
                kind, source_entry->clock, target_entry->clock, source.port.version, capacity, deadline};
            const auto result = document.add_edge(edge, *catalog);
            feedback = result ? "Feature edge added" : result.message;
            save_layout = false; // Connecting never publishes the transient view defaults.
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) linking_ = {};
    if (copy || erase) {
        std::vector<feature::FeatureNodeId> ids;
        for (const auto id : selected_) ids.emplace_back(id);
        const auto result = erase ? document.erase_nodes(ids) : document.clone_nodes(ids);
        feedback = result ? (erase ? "Feature nodes deleted" : "Feature nodes copied") : result.message;
        if (result && erase) { selected_.clear(); selected_node.clear(); }
        if (result && copy) {
            selected_.clear();
            for (const auto& node : document.snapshot().nodes)
                if (std::none_of(graph.nodes.begin(), graph.nodes.end(), [&](const auto& old) { return old.node_id == node.node_id; })) selected_.insert(node.node_id.value());
            if (!selected_.empty()) selected_node = std::to_string(*selected_.begin());
        }
        save_layout = false;
    }
    if (save_layout) {
        const auto result = document.set_feature_layout(layout_);
        feedback = result ? "Feature layout updated" : result.message;
        if (!result) snapshot_.reset();
    }
    if (document.snapshot_owner() != snapshot) selection_snapshot_ = document.snapshot_owner();
    primary_selection_ = selected_node;
}
} // namespace aeg::application
