/** @brief Source-only layout of the main Feature graph; never an execution contract. */
#pragma once

#include <cstdint>
#include <map>
#include <utility>

#include "feature_graph.hpp"

namespace aeg::application {
struct FeatureEditorLayout final {
    float zoom{1.0f};
    std::pair<float, float> pan_offset{};
    std::map<uint64_t, std::pair<float, float>> node_positions{};
    friend bool operator==(const FeatureEditorLayout&, const FeatureEditorLayout&) = default;
};

[[nodiscard]] foundation::ResultValue<FeatureEditorLayout> read_feature_editor_layout(
    const feature::FeatureGraphDocument& graph);
/** Replaces just the exact Feature v1 layout record. The caller owns revisions/history. */
[[nodiscard]] foundation::Result write_feature_editor_layout(
    feature::FeatureGraphDocument& graph, const FeatureEditorLayout& layout);
[[nodiscard]] std::pair<float, float> default_feature_node_position(std::size_t index) noexcept;
} // namespace aeg::application
