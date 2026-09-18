#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "editor_composition_catalog.hpp"

namespace aeg::application {

enum class EditorGraphNodeKind : uint8_t { kPipeline, kStage, kFeaturePoint, kFeature, kPart, kPass, kResource };

struct EditorGraphPortView final {
    uint64_t port_id{};
    std::string label{};
    bool input{};
    bool required{};
    pipeline::DocumentEdgeKind edge_kind{pipeline::DocumentEdgeKind::kRenderResource};
};

struct EditorGraphNodeView final {
    std::string node_id{};
    EditorGraphNodeKind kind{EditorGraphNodeKind::kPipeline};
    bool fixed{};
    bool unknown{};
    std::vector<EditorGraphPortView> ports{};
};

struct EditorGraphEdgeView final {
    std::string edge_id{};
    bool fixed{};
    pipeline::DocumentEdgeKind kind{pipeline::DocumentEdgeKind::kRenderResource};
    std::string source_node{};
    uint64_t source_port{};
    std::string destination_node{};
    uint64_t destination_port{};
};

struct EditorCompositionGraphView final {
    EditorGraphAddress address{};
    std::vector<EditorGraphNodeView> nodes{};
    std::vector<EditorGraphEdgeView> edges{};
};

[[nodiscard]] foundation::ResultValue<EditorCompositionGraphView> project_composition_graph(
    const pipeline::PipelineDocument&, const pipeline::TypeCatalog&, const EditorGraphAddress&);

}  // namespace aeg::application
