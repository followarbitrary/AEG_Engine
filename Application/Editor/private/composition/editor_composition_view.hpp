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
    uint32_t max_connections{1};
    uint64_t variadic_group_id{};
    pipeline::PortValueKind value_kind{pipeline::PortValueKind::kArtifact};
    pipeline::InputSourceMask allowed_sources{};
    pipeline::DocumentEdgeKind edge_kind{pipeline::DocumentEdgeKind::kRenderResource};
    uint64_t binding_id{};
    /** Stable variadic slot identity; zero denotes a fixed input/output port. */
    uint64_t slot_id{};
};

struct EditorGraphNodeView final {
    std::string node_id{};
    EditorGraphNodeKind kind{EditorGraphNodeKind::kPipeline};
    bool fixed{};
    bool unknown{};
    std::vector<EditorGraphPortView> ports{};
    /** Human-facing title for structural nodes; authored nodes fall back to node_id. */
    std::string display_name{};
};

struct EditorGraphEdgeView final {
    std::string edge_id{};
    bool fixed{};
    pipeline::DocumentEdgeKind kind{pipeline::DocumentEdgeKind::kRenderResource};
    std::string source_node{};
    uint64_t source_port{};
    std::string destination_node{};
    uint64_t destination_port{};
    uint64_t source_slot{};
    uint64_t destination_slot{};
    /**
     * Document edges represented by this visual relation. Empty for a direct
     * authored edge; populated when a hidden Resource relay is collapsed into
     * a Pass-to-Pass line in the strict Pass graph.
     */
    std::vector<std::string> backing_edge_ids{};
};

struct EditorCompositionGraphView final {
    EditorGraphAddress address{};
    std::vector<EditorGraphNodeView> nodes{};
    std::vector<EditorGraphEdgeView> edges{};
};

/** Derived inspection data. It is never an editable authoring document. */
struct EditorRuntimeNodeView final {
    uint32_t index{};
    uint64_t stage_id{};
    uint64_t task_id{};
    pipeline::ExecutionDomain execution_domain{};
    uint32_t incoming_dependencies{};
    uint32_t outgoing_dependencies{};
    bool synthetic{};
    std::string author_node{};
    uint64_t feature_part{};
};

struct EditorRuntimeDependencyView final {
    uint32_t source{};
    uint32_t destination{};
    pipeline::PipelineDependencyKind kind{};
    uint64_t artifact{};
};

struct EditorRuntimePlanView final {
    std::string source{};
    uint64_t revision{};
    std::string graph_asset{};
    uint64_t graph_version{};
    std::string semantic_digest{};
    std::vector<EditorRuntimeNodeView> nodes{};
    std::vector<EditorRuntimeDependencyView> dependencies{};
};

/** Projects an active immutable plan; author_document must be the applied snapshot, never the editable draft. */
[[nodiscard]] foundation::ResultValue<EditorRuntimePlanView> project_runtime_plan(
    const pipeline::PipelinePlan&, std::string source, const pipeline::PipelineDocument* author_document = nullptr);

[[nodiscard]] foundation::ResultValue<EditorCompositionGraphView> project_composition_graph(
    const pipeline::PipelineDocument&, const pipeline::TypeCatalog&, const EditorGraphAddress&);

}  // namespace aeg::application
