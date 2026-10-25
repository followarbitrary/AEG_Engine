#include "editor_render_graph_inspection.hpp"
#include <new>

namespace aeg::application {
foundation::ResultValue<std::shared_ptr<const EditorRenderGraphSnapshot>> capture_editor_render_graph(
    const render_graph::RenderGraph& graph, uint64_t frame_index,
    std::vector<runtime::FeatureRuntimeExecutionSnapshot> sources) {
    auto inspection = graph.inspect();
    if (!inspection) return {std::move(inspection.result), {}};
    try {
        auto snapshot = std::make_shared<EditorRenderGraphSnapshot>();
        snapshot->frame_index = frame_index;
        snapshot->compilation = inspection.value.compilation;
        snapshot->plan = std::move(inspection.value.plan);
        bool common_generation = !sources.empty();
        for (const auto& source : sources) {
            if (!source.generation.is_valid())
                return {foundation::invalid_argument("RenderGraph source requires a published generation"), {}};
            if (source.generation != sources.front().generation) common_generation = false;
        }
        if (common_generation) snapshot->generation = sources.front().generation;
        snapshot->sources = std::move(sources);
        return {foundation::Result::success(), std::move(snapshot)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("RenderGraph inspection snapshot allocation failed"), {}};
    }
}
} // namespace aeg::application
