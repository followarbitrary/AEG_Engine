#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aeg::application {
#ifndef AEG_EDITOR_ENABLE_BUILTIN_PIPELINE_DEBUG
#define AEG_EDITOR_ENABLE_BUILTIN_PIPELINE_DEBUG 0
#endif

/** Built-in editor-rendering graph edits require an explicit Debug-build opt-in. */
[[nodiscard]] inline constexpr bool builtin_pipeline_debug_enabled() noexcept {
    return AEG_EDITOR_ENABLE_BUILTIN_PIPELINE_DEBUG != 0;
}

/** Preferences is opened from the dedicated Settings rail, not the View menu. */
[[nodiscard]] inline constexpr bool preferences_action_shows_in_view_menu() noexcept {
    return false;
}

/** Compact settings dialog, with room for controls and a 1:1 font preview. */
struct PreferenceWindowWorkAreaScale final {
    uint32_t width_percent{48};
    uint32_t height_percent{54};
};

[[nodiscard]] inline constexpr PreferenceWindowWorkAreaScale preference_window_work_area_scale() noexcept {
    return {};
}

inline const char* asset_edit_block_reason(bool project_asset, bool project_loaded, bool edit_mode) {
    if (!project_asset)
        return "Built-in resource (read-only). Import an asset into a game project to edit it.";
    if (!project_loaded)
        return "Open a game project to edit its assets.";
    if (!edit_mode)
        return "Stop Play mode before editing assets.";
    return nullptr;
}
// Scope every asset row, including its popup, by the resource identity.
class AssetRowId final {
public:
    AssetRowId(uint32_t kind, uint64_t id) {
        ImGui::PushID(static_cast<int>(kind));
        ImGui::PushID(std::to_string(id).c_str());
    }
    ~AssetRowId() {
        ImGui::PopID();
        ImGui::PopID();
    }
    AssetRowId(const AssetRowId&) = delete;
    AssetRowId& operator=(const AssetRowId&) = delete;
};

class GraphPanGesture final {
public:
    ImVec2 update(bool hovered, bool empty, bool connecting, bool clicked, bool down, bool dragging, bool cancel,
                  ImVec2 delta) {
        if (cancel || !down) {
            active_ = false;
            return {};
        }
        if (clicked)
            active_ = hovered && empty && !connecting;
        return active_ && dragging ? delta : ImVec2{};
    }

private:
    bool active_{false};
};

/**
 * @brief Spacing and text sizing for Pipeline graph cards.
 *
 * Card rows use their own vertical rhythm so a user-selected editor font
 * cannot push the final type label into the description row.  Text is kept
 * at a readable screen size while the canvas is zoomed out.
 */
struct PipelineNodeCardLayout final {
    float title_y{12.0f};
    float subtitle_y{33.0f};
    float detail_y{53.0f};
    float divider_y{75.0f};
    float footer_y{82.0f};
    float height{109.0f};

    [[nodiscard]] static PipelineNodeCardLayout for_font_size(float font_size) noexcept {
        const float line_height = std::max(17.0f, std::ceil(font_size));
        PipelineNodeCardLayout layout{};
        layout.title_y = 12.0f;
        layout.subtitle_y = layout.title_y + line_height + 4.0f;
        layout.detail_y = layout.subtitle_y + line_height + 3.0f;
        layout.divider_y = layout.detail_y + line_height + 5.0f;
        layout.footer_y = layout.divider_y + 7.0f;
        layout.height = layout.footer_y + line_height + 10.0f;
        return layout;
    }

    [[nodiscard]] static float text_size(float font_size, float zoom) noexcept {
        return std::max(13.0f, font_size * zoom);
    }
};

[[nodiscard]] inline std::string pipeline_node_copy_text(std::string_view title, std::string_view subtitle,
                                                         std::string_view detail) {
    std::string text;
    text.reserve(title.size() + subtitle.size() + detail.size() + 2);
    text.append(title).append("\n").append(subtitle).append("\n").append(detail);
    return text;
}

/** Editor-owned terminal card shown for every authored project Pipeline. */
struct RequiredPipelinePresentConsumer final {
    std::string_view node_id;
    std::string_view title;
    std::string_view subtitle;
    std::string_view detail;
};

[[nodiscard]] inline constexpr RequiredPipelinePresentConsumer required_pipeline_present_consumer() noexcept {
    return {"__aeg_present_consumer", "Present", "Display consumer", "No state link connected"};
}

inline bool remove_graph_edge(std::vector<std::pair<int, int>>& edges, int from, int to) {
    const auto found = std::find(edges.begin(), edges.end(), std::pair{from, to});
    if (found == edges.end())
        return false;
    edges.erase(found);
    return true;
}

struct PipelineOrderingDraft final {
    std::string source{};
    uint32_t source_node{0};
    uint32_t destination_node{0};
    bool add{true};
};

// Keeps pipeline edits local to the Editor until the scene is explicitly saved.
class PipelineOrderingDraftQueue final {
public:
    void stage_add(std::string source, uint32_t source_node, uint32_t destination_node) {
        stage(std::move(source), source_node, destination_node, true);
    }

    void stage_remove(std::string source, uint32_t source_node, uint32_t destination_node) {
        const auto found = find(source, source_node, destination_node);
        if (found != changes_.end() && found->add) {
            changes_.erase(found);
            return;
        }
        stage(std::move(source), source_node, destination_node, false);
    }

    [[nodiscard]] bool dirty() const noexcept { return !changes_.empty(); }
    [[nodiscard]] const std::vector<PipelineOrderingDraft>& changes() const noexcept { return changes_; }
    void clear() noexcept { changes_.clear(); }

private:
    [[nodiscard]] std::vector<PipelineOrderingDraft>::iterator find(const std::string& source, uint32_t source_node,
                                                                    uint32_t destination_node) {
        return std::find_if(changes_.begin(), changes_.end(), [&](const PipelineOrderingDraft& value) {
            return value.source == source && value.source_node == source_node &&
                   value.destination_node == destination_node;
        });
    }

    void stage(std::string source, uint32_t source_node, uint32_t destination_node, bool add) {
        const auto found = find(source, source_node, destination_node);
        if (found != changes_.end()) {
            found->add = add;
            return;
        }
        changes_.push_back(PipelineOrderingDraft{std::move(source), source_node, destination_node, add});
    }

    std::vector<PipelineOrderingDraft> changes_{};
};

/** @brief Stable identity of a compiled graph node persisted independently of its transient index. */
struct PipelineNodeIdentity final {
    uint64_t stage{0};
    uint64_t task{0};

    [[nodiscard]] friend bool operator==(const PipelineNodeIdentity&, const PipelineNodeIdentity&) = default;
};

/** @brief One saved graph connection or explicit removal of a compiled connection. */
struct PipelineSavedOrdering final {
    std::string source{};
    PipelineNodeIdentity source_node{};
    PipelineNodeIdentity destination_node{};
    bool add{true};
};

/**
 * @brief Maintains the durable, last-saved state of Editor-authored graph connections.
 *
 * Repeating a connection updates its operation instead of appending an ambiguous history entry.
 */
class PipelineOrderingSaveState final {
public:
    void stage(PipelineSavedOrdering change) {
        const auto found = std::find_if(changes_.begin(), changes_.end(), [&](const PipelineSavedOrdering& value) {
            return value.source == change.source && value.source_node == change.source_node &&
                   value.destination_node == change.destination_node;
        });
        if (found == changes_.end()) {
            changes_.push_back(std::move(change));
        } else {
            found->add = change.add;
        }
    }

    void clear() noexcept { changes_.clear(); }
    [[nodiscard]] const std::vector<PipelineSavedOrdering>& changes() const noexcept { return changes_; }

private:
    std::vector<PipelineSavedOrdering> changes_{};
};

/** @brief Saved free-form position for one compiled Pipeline node. */
struct PipelineNodeLayout final {
    std::string source{};
    PipelineNodeIdentity node{};
    float x{0.0f};
    float y{0.0f};
};

/** @brief Durable free-form node layout, keyed by source and stable stage/task identity. */
class PipelineLayoutSaveState final {
public:
    void stage(PipelineNodeLayout layout) {
        const auto found = std::find_if(layouts_.begin(), layouts_.end(), [&](const PipelineNodeLayout& value) {
            return value.source == layout.source && value.node == layout.node;
        });
        if (found == layouts_.end())
            layouts_.push_back(std::move(layout));
        else
            *found = std::move(layout);
    }
    [[nodiscard]] std::optional<PipelineNodeLayout> find(std::string_view source, PipelineNodeIdentity node) const {
        const auto found = std::find_if(layouts_.begin(), layouts_.end(), [&](const PipelineNodeLayout& value) {
            return value.source == source && value.node == node;
        });
        return found == layouts_.end() ? std::nullopt : std::optional<PipelineNodeLayout>{*found};
    }
    void clear() noexcept { layouts_.clear(); }
    [[nodiscard]] const std::vector<PipelineNodeLayout>& layouts() const noexcept { return layouts_; }

private:
    std::vector<PipelineNodeLayout> layouts_{};
};

/** @brief One persisted node-level Render State blend choice. */
struct PipelineSavedBlend final {
    std::string source{};
    PipelineNodeIdentity node{};
    uint8_t blend_mode{0};
};

class PipelineBlendSaveState final {
public:
    void stage(PipelineSavedBlend blend) {
        const auto found = std::find_if(blends_.begin(), blends_.end(), [&](const PipelineSavedBlend& value) {
            return value.source == blend.source && value.node == blend.node;
        });
        if (found == blends_.end())
            blends_.push_back(std::move(blend));
        else
            *found = std::move(blend);
    }
    void clear() noexcept { blends_.clear(); }
    [[nodiscard]] const std::vector<PipelineSavedBlend>& blends() const noexcept { return blends_; }

private:
    std::vector<PipelineSavedBlend> blends_{};
};

inline bool graph_link_hit(ImVec2 from, ImVec2 to, ImVec2 mouse, float zoom) {
    const float bend = std::max(50.0f * zoom, std::abs(to.x - from.x) * 0.45f);
    const ImVec2 control_a{from.x + bend, from.y};
    const ImVec2 control_b{to.x - bend, to.y};
    const auto point = [&](float t) {
        const float inverse = 1.0f - t;
        return ImVec2{
            inverse * inverse * inverse * from.x + 3.0f * inverse * inverse * t * control_a.x +
                3.0f * inverse * t * t * control_b.x + t * t * t * to.x,
            inverse * inverse * inverse * from.y + 3.0f * inverse * inverse * t * control_a.y +
                3.0f * inverse * t * t * control_b.y + t * t * t * to.y,
        };
    };
    constexpr int segments = 24;
    // A curve is a thin visual target; keep the click target comfortably larger
    // than the rendered stroke so double-click removal is reliable.
    const float hit_radius = std::max(9.0f, 10.0f * zoom);
    ImVec2 previous = from;
    for (int index = 1; index <= segments; ++index) {
        const ImVec2 current = point(static_cast<float>(index) / segments);
        const float dx = current.x - previous.x;
        const float dy = current.y - previous.y;
        const float length_squared = dx * dx + dy * dy;
        const float fraction =
            length_squared > 0.0f
                ? std::clamp(((mouse.x - previous.x) * dx + (mouse.y - previous.y) * dy) / length_squared, 0.0f, 1.0f)
                : 0.0f;
        const float near_x = previous.x + dx * fraction;
        const float near_y = previous.y + dy * fraction;
        const float distance_x = mouse.x - near_x;
        const float distance_y = mouse.y - near_y;
        if (distance_x * distance_x + distance_y * distance_y <= hit_radius * hit_radius)
            return true;
        previous = current;
    }
    return false;
}

/**
 * @brief True when a RenderFeature may originate a State Link.
 *
 * A state writer always originates a link. A read-only feature is a legal
 * pass-through origin once an incoming State Link exists; it must not rely on
 * reachability-derived input counts, because those stay zero until this link
 * is created.
 */
[[nodiscard]] inline bool can_start_render_state_link(bool writes_render_state, bool has_state_input) noexcept {
    return writes_render_state || has_state_input;
}

/** @brief Visualized data state accumulated by one Pipeline graph node. */
struct PipelineGraphState final {
    bool reaches_output{false};
    uint32_t input_count{0};
    uint32_t layer_count{0};
};

/**
 * @brief Propagates source-state layers through only the branches that can reach an output node.
 *
 * A layer is a distinct root state.  Merging two upstream roots produces two layers, while a
 * serial stage preserves the existing layer count.  This mirrors the compositing semantics used
 * by the Pipeline canvas without tying the UI helper to a particular PipelinePlan type.
 */
inline std::vector<PipelineGraphState> pipeline_graph_state_overlay(std::size_t node_count,
                                                                    const std::vector<std::pair<int, int>>& edges,
                                                                    const std::vector<int>& output_nodes) {
    std::vector<PipelineGraphState> states(node_count);
    std::vector<std::vector<int>> incoming(node_count), outgoing(node_count);
    for (const auto& [source, destination] : edges) {
        if (source < 0 || destination < 0 || static_cast<std::size_t>(source) >= node_count ||
            static_cast<std::size_t>(destination) >= node_count) {
            continue;
        }
        outgoing[static_cast<std::size_t>(source)].push_back(destination);
        incoming[static_cast<std::size_t>(destination)].push_back(source);
    }

    std::vector<int> pending_outputs;
    for (const int output : output_nodes) {
        if (output >= 0 && static_cast<std::size_t>(output) < node_count) {
            pending_outputs.push_back(output);
        }
    }
    for (std::size_t index = 0; index < pending_outputs.size(); ++index) {
        const int current = pending_outputs[index];
        PipelineGraphState& state = states[static_cast<std::size_t>(current)];
        if (state.reaches_output) {
            continue;
        }
        state.reaches_output = true;
        for (const int source : incoming[static_cast<std::size_t>(current)]) {
            pending_outputs.push_back(source);
        }
    }

    std::vector<uint32_t> pending_inputs(node_count);
    std::vector<std::vector<int>> layers(node_count);
    std::vector<int> ready;
    for (std::size_t node = 0; node < node_count; ++node) {
        if (!states[node].reaches_output) {
            continue;
        }
        for (const int source : incoming[node]) {
            if (states[static_cast<std::size_t>(source)].reaches_output) {
                ++pending_inputs[node];
                ++states[node].input_count;
            }
        }
        if (pending_inputs[node] == 0) {
            layers[node].push_back(static_cast<int>(node));
            ready.push_back(static_cast<int>(node));
        }
    }
    for (std::size_t index = 0; index < ready.size(); ++index) {
        const int current = ready[index];
        const std::size_t current_index = static_cast<std::size_t>(current);
        states[current_index].layer_count = static_cast<uint32_t>(layers[current_index].size());
        for (const int destination : outgoing[current_index]) {
            const std::size_t destination_index = static_cast<std::size_t>(destination);
            if (!states[destination_index].reaches_output) {
                continue;
            }
            for (const int layer : layers[current_index]) {
                if (std::find(layers[destination_index].begin(), layers[destination_index].end(), layer) ==
                    layers[destination_index].end()) {
                    layers[destination_index].push_back(layer);
                }
            }
            if (--pending_inputs[destination_index] == 0) {
                ready.push_back(destination);
            }
        }
    }
    return states;
}
}  // namespace aeg::application
