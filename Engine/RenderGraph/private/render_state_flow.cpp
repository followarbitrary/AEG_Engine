/**
 * @file render_state_flow.cpp
 * @brief Implements the editable RenderFeature state-flow graph.
 */
#include "../public/render_state_flow.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace aeg::render_graph {
namespace {
void append_unique(std::vector<uint32_t>& values, uint32_t value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

[[nodiscard]] bool contains_edge(const std::vector<std::pair<uint32_t, uint32_t>>& edges, uint32_t source,
                                 uint32_t destination) {
    return std::find(edges.begin(), edges.end(), std::pair<uint32_t, uint32_t>{source, destination}) != edges.end();
}
}  // namespace

RenderStateFlow build_render_state_flow(const pipeline::PipelinePlan& plan,
                                        std::span<const RenderStateLinkOverride> overrides) {
    RenderStateFlow result{};
    result.contributions.resize(plan.feature_contributions.size());

    std::vector<std::vector<uint32_t>> outgoing_nodes(plan.nodes.size());
    for (const pipeline::PipelineDependency& dependency : plan.dependencies) {
        if (dependency.source_node < plan.nodes.size() && dependency.destination_node < plan.nodes.size()) {
            append_unique(outgoing_nodes[dependency.source_node], dependency.destination_node);
        }
    }
    std::vector<std::vector<uint32_t>> node_contributions(plan.nodes.size());
    std::vector<std::vector<uint32_t>> contribution_nodes(plan.feature_contributions.size());
    for (uint32_t contribution_index = 0; contribution_index < plan.feature_contributions.size(); ++contribution_index) {
        const auto& contribution = plan.feature_contributions[contribution_index];
        if (contribution.domain != pipeline::FeatureDomain::kRender || contribution.stage_count == 0) continue;
        const uint64_t first_stage = contribution.first_stage_index;
        const uint64_t past_last_stage = first_stage + contribution.stage_count;
        for (uint32_t node_index = 0; node_index < plan.nodes.size(); ++node_index) {
            const uint64_t stage_index = plan.nodes[node_index].definition_stage_index;
            if (stage_index >= first_stage && stage_index < past_last_stage) {
                append_unique(node_contributions[node_index], contribution_index);
                append_unique(contribution_nodes[contribution_index], node_index);
            }
        }
    }
    for (const pipeline::PipelineNode& node : plan.nodes) {
        result.present_node_count += node.phase == pipeline::EnginePhase::kPresent ? 1u : 0u;
    }
    if (result.present_node_count == 0) {
        for (uint32_t index = 0; index < plan.feature_contributions.size(); ++index) {
            if (plan.feature_contributions[index].domain == pipeline::FeatureDomain::kRender) {
                result.contributions[index].reaches_present = true;
                result.contributions[index].layer_count = 1;
            }
        }
        return result;
    }

    // Reduce scheduler fan-out to State Links between render Features or Present.
    std::vector<std::pair<uint32_t, uint32_t>> state_edges{};
    std::vector<uint8_t> present_edges(plan.feature_contributions.size(), 0);
    for (uint32_t source = 0; source < plan.feature_contributions.size(); ++source) {
        if (plan.feature_contributions[source].domain != pipeline::FeatureDomain::kRender) continue;
        std::vector<uint8_t> visited(plan.nodes.size(), 0);
        std::vector<uint32_t> frontier = contribution_nodes[source];
        for (const uint32_t node : frontier) visited[node] = 1;
        for (std::size_t cursor = 0; cursor < frontier.size(); ++cursor) {
            for (const uint32_t successor : outgoing_nodes[frontier[cursor]]) {
                if (plan.nodes[successor].phase == pipeline::EnginePhase::kPresent) {
                    present_edges[source] = 1;
                    continue;
                }
                bool reached_other_contribution = false;
                for (const uint32_t destination : node_contributions[successor]) {
                    if (destination == source ||
                        plan.feature_contributions[destination].domain != pipeline::FeatureDomain::kRender) continue;
                    reached_other_contribution = true;
                    if (!contains_edge(state_edges, source, destination)) state_edges.emplace_back(source, destination);
                }
                if (!reached_other_contribution && visited[successor] == 0) {
                    visited[successor] = 1;
                    frontier.push_back(successor);
                }
            }
        }
    }

    // Keep a state contributor's direct Present link only when it does not already
    // feed a later contributor that reaches Present. This removes implicit phase
    // shortcuts such as Opaque -> Present beside Opaque -> Readback -> Present.
    std::vector<std::vector<uint32_t>> raw_incoming(plan.feature_contributions.size());
    for (const auto& [source, destination] : state_edges) raw_incoming[destination].push_back(source);
    std::vector<uint8_t> raw_reaches_present(plan.feature_contributions.size(), 0);
    std::vector<uint32_t> pending{};
    for (uint32_t index = 0; index < present_edges.size(); ++index) {
        if (present_edges[index] != 0) {
            raw_reaches_present[index] = 1;
            pending.push_back(index);
        }
    }
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (const uint32_t predecessor : raw_incoming[pending[cursor]]) {
            if (raw_reaches_present[predecessor] == 0) {
                raw_reaches_present[predecessor] = 1;
                pending.push_back(predecessor);
            }
        }
    }
    for (uint32_t source = 0; source < present_edges.size(); ++source) {
        if (present_edges[source] == 0) continue;
        for (const auto& [edge_source, destination] : state_edges) {
            if (edge_source == source && raw_reaches_present[destination] != 0) {
                present_edges[source] = 0;
                break;
            }
        }
    }

    for (const RenderStateLinkOverride& override : overrides) {
        if (override.source_node >= node_contributions.size() || override.destination_node >= plan.nodes.size()) continue;
        for (const uint32_t source : node_contributions[override.source_node]) {
            if (plan.nodes[override.destination_node].phase == pipeline::EnginePhase::kPresent) {
                present_edges[source] = override.connected ? 1u : 0u;
                continue;
            }
            for (const uint32_t destination : node_contributions[override.destination_node]) {
                if (source == destination) continue;
                const std::pair<uint32_t, uint32_t> edge{source, destination};
                if (override.connected) {
                    if (!contains_edge(state_edges, source, destination)) state_edges.push_back(edge);
                } else {
                    state_edges.erase(std::remove(state_edges.begin(), state_edges.end(), edge), state_edges.end());
                }
            }
        }
    }

    std::vector<std::vector<uint32_t>> incoming(plan.feature_contributions.size());
    std::vector<std::vector<uint32_t>> outgoing(plan.feature_contributions.size());
    for (const auto& [source, destination] : state_edges) {
        if (source < outgoing.size() && destination < incoming.size()) {
            append_unique(outgoing[source], destination);
            append_unique(incoming[destination], source);
        }
    }
    std::vector<uint32_t> reachable_pending{};
    for (uint32_t index = 0; index < present_edges.size(); ++index) {
        if (present_edges[index] != 0) {
            result.contributions[index].reaches_present = true;
            result.contributions[index].connects_to_present = true;
            reachable_pending.push_back(index);
        }
    }
    for (std::size_t cursor = 0; cursor < reachable_pending.size(); ++cursor) {
        for (const uint32_t predecessor : incoming[reachable_pending[cursor]]) {
            if (!result.contributions[predecessor].reaches_present) {
                result.contributions[predecessor].reaches_present = true;
                reachable_pending.push_back(predecessor);
            }
        }
    }

    std::vector<uint32_t> indegree(plan.feature_contributions.size(), 0);
    std::vector<std::vector<uint32_t>> layers(plan.feature_contributions.size());
    std::vector<uint32_t> ready{};
    uint32_t active_contributions = 0;
    for (uint32_t index = 0; index < plan.feature_contributions.size(); ++index) {
        RenderStateContribution& contribution = result.contributions[index];
        if (!contribution.reaches_present) continue;
        ++active_contributions;
        for (const uint32_t predecessor : incoming[index]) {
            if (result.contributions[predecessor].reaches_present) {
                contribution.input_contributions.push_back(predecessor);
                ++indegree[index];
            }
        }
        contribution.input_count = indegree[index];
        for (const uint32_t successor : outgoing[index]) {
            if (result.contributions[successor].reaches_present) contribution.output_contributions.push_back(successor);
        }
        if (indegree[index] == 0) {
            layers[index].push_back(index);
            ready.push_back(index);
        }
    }
    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
        const uint32_t source = ready[cursor];
        result.contributions[source].layer_count = static_cast<uint32_t>(layers[source].size());
        for (const uint32_t destination : result.contributions[source].output_contributions) {
            for (const uint32_t layer : layers[source]) append_unique(layers[destination], layer);
            if (--indegree[destination] == 0) ready.push_back(destination);
        }
    }
    result.has_cycle = ready.size() != active_contributions;
    return result;
}
}  // namespace aeg::render_graph
