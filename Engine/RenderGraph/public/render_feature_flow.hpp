/**
 * @file render_feature_flow.hpp
 * @brief Maps compiled Pipeline feature stages to their Present reachability.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "pipeline.hpp"

namespace aeg::render_graph {
/**
 * @brief Returns whether a render contribution can reach a Present stage.
 *
 * A plan without a Present stage is treated as an offscreen/standalone plan,
 * so its render contributions remain enabled. Once a Present stage exists,
 * a contribution must have at least one declared stage with a dependency path
 * to it. Invalid dependency indices are ignored here; normal plan validation
 * owns reporting malformed plans.
 */
[[nodiscard]] inline bool render_feature_contribution_reaches_present(
    const pipeline::PipelinePlan& plan, const pipeline::PipelineFeatureContributionRecord& contribution) {
    bool has_present = false;
    for (const pipeline::PipelineNode& node : plan.nodes) {
        if (node.phase == pipeline::EnginePhase::kPresent) {
            has_present = true;
            break;
        }
    }
    if (!has_present) {
        return true;
    }
    if (contribution.stage_count == 0) {
        return false;
    }

    const uint64_t first_stage = contribution.first_stage_index;
    const uint64_t past_last_stage = first_stage + contribution.stage_count;
    std::vector<std::vector<uint32_t>> successors(plan.nodes.size());
    for (const pipeline::PipelineDependency& dependency : plan.dependencies) {
        if (dependency.source_node < plan.nodes.size() && dependency.destination_node < plan.nodes.size()) {
            successors[dependency.source_node].push_back(dependency.destination_node);
        }
    }

    std::vector<uint8_t> visited(plan.nodes.size(), 0);
    std::vector<uint32_t> pending{};
    pending.reserve(plan.nodes.size());
    for (uint32_t node_index = 0; node_index < plan.nodes.size(); ++node_index) {
        const uint64_t definition_stage_index = plan.nodes[node_index].definition_stage_index;
        if (definition_stage_index >= first_stage && definition_stage_index < past_last_stage) {
            visited[node_index] = 1;
            pending.push_back(node_index);
        }
    }
    while (!pending.empty()) {
        const uint32_t node_index = pending.back();
        pending.pop_back();
        if (plan.nodes[node_index].phase == pipeline::EnginePhase::kPresent) {
            return true;
        }
        for (const uint32_t successor : successors[node_index]) {
            if (visited[successor] == 0) {
                visited[successor] = 1;
                pending.push_back(successor);
            }
        }
    }
    return false;
}
}  // namespace aeg::render_graph
