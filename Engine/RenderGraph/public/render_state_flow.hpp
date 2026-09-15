/**
 * @file render_state_flow.hpp
 * @brief Resolves RenderFeature state layers from a compiled Pipeline graph.
 */
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pipeline.hpp"

namespace aeg::render_graph {
/**
 * @brief Editor-authored state-flow override for a compiled Pipeline node edge.
 *
 * This is intentionally separate from Pipeline scheduling: removing a State
 * Link changes state propagation and Present visibility without removing a
 * mandatory engine dependency from the compiled task graph.
 */
struct RenderStateLinkOverride final {
    uint32_t source_node{pipeline::kInvalidPipelineHandleIndex};
    uint32_t destination_node{pipeline::kInvalidPipelineHandleIndex};
    bool connected{true};
};

/** @brief Editor-authored blend selection for the RenderFeature owning a plan node. */
struct RenderStateBlendOverride final {
    uint32_t node{pipeline::kInvalidPipelineHandleIndex};
    pipeline::RenderStateBlendMode blend_mode{pipeline::RenderStateBlendMode::kReplace};
};

/** @brief State visibility and composition data for one Pipeline feature contribution. */
struct RenderStateContribution final {
    bool reaches_present{false};
    /** This contribution has an explicit effective State Link to Present. */
    bool connects_to_present{false};
    uint32_t input_count{0};
    uint32_t layer_count{0};
    /** Immediate render-feature predecessors after non-feature stages are collapsed. */
    std::vector<uint32_t> input_contributions{};
    /** Immediate render-feature successors after non-feature stages are collapsed. */
    std::vector<uint32_t> output_contributions{};
};

/** @brief State-flow summary indexed by `PipelinePlan::feature_contributions`. */
struct RenderStateFlow final {
    uint32_t present_node_count{0};
    /** True when the effective state-link graph is cyclic and cannot be layered. */
    bool has_cycle{false};
    std::vector<RenderStateContribution> contributions{};
};

/**
 * @brief Builds the render-state flow induced by a compiled Pipeline plan.
 *
 * Only render-domain feature contributions participate. Contributions that
 * cannot reach a Present node are intentionally excluded from layer counts.
 * A plan without Present remains an offscreen plan and therefore retains all
 * render contributions as one independent layer each.
 */
[[nodiscard]] RenderStateFlow build_render_state_flow(
    const pipeline::PipelinePlan& plan, std::span<const RenderStateLinkOverride> overrides = {});
}  // namespace aeg::render_graph
