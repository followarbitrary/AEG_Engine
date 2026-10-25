/**
 * @file feature_process_editor.hpp
 * @brief Editor-only layout state for Process state machines.
 *
 * Process semantics live in Engine/Feature. This type stores canvas state and
 * is intentionally kept out of runtime Feature contracts and cooked assets.
 */
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "feature_process.hpp"
#include "feature_graph.hpp"

namespace aeg::application {

struct ProcessEditorLayout final {
    float zoom{1.0f};
    std::pair<float, float> pan_offset{0.0f, 0.0f};
    bool grid_snap{true};
    float grid_size{20.0f};
    bool show_descriptions{true};
    bool show_conditions{true};

    std::unordered_map<std::string, std::pair<float, float>> state_positions{};
    std::unordered_map<std::string, std::string> state_colors{};
    std::vector<std::vector<std::pair<float, float>>> transition_control_points{};

    friend bool operator==(const ProcessEditorLayout&, const ProcessEditorLayout&) = default;
};

/** @brief Reads validated author-only canvas metadata, or defaults when absent. */
[[nodiscard]] foundation::ResultValue<ProcessEditorLayout> read_process_editor_layout(
    const feature::FeatureGraphDocument& graph, feature::FeatureNodeId owner_node);

/** @brief Replaces only one owner's canvas extension; revisions are caller-owned. */
[[nodiscard]] foundation::Result write_process_editor_layout(
    feature::FeatureGraphDocument& graph, feature::FeatureNodeId owner_node,
    const ProcessEditorLayout& layout);

/** @brief Removes one owner's validated canvas extension before semantic deletion. */
[[nodiscard]] foundation::Result erase_process_editor_layout(
    feature::FeatureGraphDocument& graph, feature::FeatureNodeId owner_node);

/** @brief Computes a deterministic force-directed canvas layout for an editor view. */
void auto_layout_state_machine(const aeg::feature::ProcessStateMachine& machine,
                               ProcessEditorLayout& layout,
                               float canvas_width,
                               float canvas_height);

}  // namespace aeg::application
