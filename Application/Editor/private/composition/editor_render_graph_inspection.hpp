/** @brief Owning diagnostic values captured after a successful Scene frame. */
#pragma once
#include <memory>
#include "feature_graph_runtime_host.hpp"
#include "../../../../Engine/RenderGraph/public/render_graph.hpp"

namespace aeg::application {
struct EditorRenderGraphSnapshot final {
    uint64_t frame_index{0};
    /** Common generation when every source shares one Runtime publication; empty for independent sources. */
    feature::RuntimeGeneration generation{};
    render_graph::RenderGraphCompileInfo compilation{};
    render_graph::RenderGraphPlan plan{};
    std::vector<runtime::FeatureRuntimeExecutionSnapshot> sources{};
};

/** Caller establishes successful execution; this copies metadata without backend ownership. */
[[nodiscard]] foundation::ResultValue<std::shared_ptr<const EditorRenderGraphSnapshot>> capture_editor_render_graph(
    const render_graph::RenderGraph& graph, uint64_t frame_index,
    std::vector<runtime::FeatureRuntimeExecutionSnapshot> sources);
} // namespace aeg::application
