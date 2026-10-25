#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <variant>

#include "../Engine/RenderGraph/private/render_feature_projector.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"

namespace render_feature_test {

/**
 * Checks actual live geometry records against work projected from committed captures.
 * Callers supply outputs scoped to their actual extracted View; native prepared-source
 * checks establish that View association because RenderWorkItem itself has no View field.
 * Readback, upload and Present records never count as Scene geometry.
 */
inline aeg::foundation::ResultValue<size_t> check_scene_draw_provenance(
    const aeg::render_graph::RenderGraph& graph,
    std::span<const aeg::feature::RenderFeatureOutput> captured_outputs, uint64_t frame) {
    using namespace aeg;
    constexpr std::array names{std::string_view("DrawOpaque"), std::string_view("Renderer.ForwardOpaquePass"),
        std::string_view("Renderer.GBufferPass"), std::string_view("Renderer.GBufferScene")};
    const auto expected = render_graph::RenderFeatureProjector{}.project(captured_outputs, frame);
    if (!expected) return {expected.result, 0};
    size_t geometry = 0;
    for (const auto& pass : render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
        if (!pass.live) continue;
        const auto actual = render_graph::detail::RenderGraphExecution::inspect_pass(graph, pass.pass);
        if (!actual) return {actual.result, 0};
        if (std::find(names.begin(), names.end(), actual.value.name) == names.end()) continue;
        ++geometry;
        if (!pass.feature_work)
            return {foundation::invalid_state("Live SceneDraw has no committed Feature work"), geometry};
        const auto& work = *pass.feature_work;
        const auto source = std::find_if(captured_outputs.begin(), captured_outputs.end(), [&](const auto& output) {
            return output.instance_id == work.instance_id && output.invocation == work.invocation;
        });
        if (source == captured_outputs.end() || work.operation_index >= source->intent.operations.size())
            return {foundation::invalid_state("Live SceneDraw does not belong to its committed capture"), geometry};
        const auto& operation = source->intent.operations[work.operation_index];
        if (!std::holds_alternative<feature::RenderDrawScene>(operation) &&
            !std::holds_alternative<feature::RenderDrawGBufferScene>(operation) &&
            !std::holds_alternative<feature::RenderDrawClusteredScene>(operation))
            return {foundation::invalid_state("Live SceneDraw was attributed to another operation"), geometry};
        const auto projected = std::find_if(expected.value.begin(), expected.value.end(), [&](const auto& item) {
            return item.kind == render_graph::RenderWorkItemKind::kExecute && item.id == work.id &&
                item.domain == work.domain && item.feature_definition_id == work.feature_definition_id &&
                item.owner_module == work.owner_module && item.node_id == work.node_id &&
                item.instance_id == work.instance_id && item.frame_index == work.frame_index &&
                item.invocation == work.invocation && item.operation_index == work.operation_index;
        });
        if (projected == expected.value.end())
            return {foundation::invalid_state("Live SceneDraw differs from its actual projected work"), geometry};
    }
    if (geometry == 0) return {foundation::invalid_state("No live Scene geometry was observed"), 0};
    return {foundation::Result::success(), geometry};
}

} // namespace render_feature_test
