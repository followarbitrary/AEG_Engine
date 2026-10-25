/**
 * @file render_feature_projector.hpp
 * @brief RenderGraph-private typed RenderFeature lowering adapter.
 */
#pragma once

#include "../public/render_feature_projection.hpp"

namespace aeg::render_graph {

class RenderFeatureProjector final {
public:
    [[nodiscard]] foundation::ResultValue<feature::RenderIntentExecutionPlan> plan(
        std::span<const feature::RenderFeatureOutput> outputs) const;
    [[nodiscard]] foundation::ResultValue<std::vector<RenderWorkItem>> project(
        const feature::CompiledFeatureGraph& graph, uint64_t frame_index) const;
    [[nodiscard]] foundation::ResultValue<std::vector<RenderWorkItem>> project(
        std::span<const feature::RenderFeatureOutput> outputs, uint64_t frame_index) const;
};

}  // namespace aeg::render_graph
