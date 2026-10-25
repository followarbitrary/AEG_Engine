/** @brief Main Feature node/port canvas, independent of the Process state canvas. */
#pragma once

#include <memory>
#include <set>
#include <string>

#include "editor_feature_graph_document.hpp"
#include "editor_feature_graph_layout.hpp"
#include "editor_node_definition.hpp"

namespace aeg::application {
class EditorFeatureGraphCanvas final {
public:
    void draw(EditorFeatureGraphDocument&, const feature::FeatureCatalog*,
              std::string& selected_node, std::string& feedback,
              std::span<const NodeDefinitionDraft> presentations = {});

private:
    struct PortSelection final {
        feature::FeatureNodeId node{};
        feature::PortContract port{};
    };
    const EditorFeatureGraphDocument* document_{};
    std::shared_ptr<const feature::FeatureGraphDocument> snapshot_{};
    std::shared_ptr<const feature::FeatureGraphDocument> selection_snapshot_{};
    FeatureEditorLayout layout_{};
    FeatureEditorLayout drag_start_{};
    std::set<uint64_t> selected_{};
    // Last primary selection emitted by the canvas. A different caller value
    // comes from a lane, Inspector navigation, or the node table.
    std::string primary_selection_{};
    feature::FeatureNodeId dragging_{};
    PortSelection linking_{};
    bool panning_{false};
    int edge_kind_{0};
    // Transport-specific settings are opt-in.  A freshly authored Data edge
    // must not inherit a queue capacity or a one-tick deadline from the
    // transient controls.
    int edge_capacity_{0};
    int edge_deadline_{0};
};
} // namespace aeg::application
