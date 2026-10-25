#include "render_feature_projector.hpp"
#include "../../Feature/private/render_intent_planner.hpp"

#include <utility>
#include <algorithm>
#include <unordered_map>

namespace aeg::render_graph {

namespace {

[[nodiscard]] RenderWorkItemId make_work_item_id(const feature::FeatureNodeProvenance& provenance,
                                                 uint64_t frame_index,
                                                 RenderWorkItemKind kind, uint64_t generation = 0,
                                                 uint32_t operation_index = 0) noexcept {
    // Keep the identity deterministic across runs while separating items from
    // different instances, frames, and operation kinds. This is an internal
    // identity, not an authoring identifier or a render-pass name.
    uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](uint64_t value) {
        for (uint8_t shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    mix(provenance.definition_id.value());
    mix(provenance.owner_module.value());
    mix(provenance.node_id.value());
    mix(provenance.instance_id.value());
    mix(frame_index);
    mix(static_cast<uint64_t>(kind));
    mix(provenance.invocation_id.value());
    mix(generation);
    mix(operation_index);
    return RenderWorkItemId(hash == 0 ? 1 : hash);
}

}  // namespace

foundation::ResultValue<feature::RenderIntentExecutionPlan> RenderFeatureProjector::plan(
    std::span<const feature::RenderFeatureOutput> outputs) const {
    return feature::RenderIntentPlanner{}.plan(outputs);
}

foundation::ResultValue<std::vector<RenderWorkItem>> RenderFeatureProjector::project(
    const feature::CompiledFeatureGraph& graph, uint64_t frame_index) const {
    std::vector<RenderWorkItem> work;
    work.reserve(graph.provenance.size() * 2);
    for (const auto& provenance : graph.provenance) {
        if ((provenance.domains & feature::to_mask(feature::FeatureDomain::kRender)) == 0) {
            // Render projection consumes only Render leaves. Other domains
            // remain scheduled by their own clocks.
            continue;
        }
        work.push_back({feature::FeatureDomain::kRender,
                        provenance.definition_id,
                        provenance.owner_module,
                        provenance.node_id,
                        provenance.instance_id,
                        frame_index,
                        make_work_item_id(provenance, frame_index, RenderWorkItemKind::kPrepare),
                        RenderWorkItemKind::kPrepare});
        work.push_back({feature::FeatureDomain::kRender,
                        provenance.definition_id,
                        provenance.owner_module,
                        provenance.node_id,
                        provenance.instance_id,
                        frame_index,
                        make_work_item_id(provenance, frame_index, RenderWorkItemKind::kExecute),
                        RenderWorkItemKind::kExecute});
    }
    return {foundation::Result::success(), std::move(work)};
}

foundation::ResultValue<std::vector<RenderWorkItem>> RenderFeatureProjector::project(
    std::span<const feature::RenderFeatureOutput> outputs, uint64_t frame_index) const {
    std::vector<RenderWorkItem> work;
    for (const auto& output : outputs) {
        if (!output.instance_id.is_valid() || !output.node_id.is_valid() ||
            !output.invocation.invocation_id.is_valid() || !output.invocation.generation.is_valid())
            return {foundation::invalid_argument("Render work requires committed Feature provenance"), {}};
        if (const auto result = feature::validate_render_intent(output.intent, output.contract); !result) return {result, {}};
        feature::FeatureNodeProvenance provenance{};
        provenance.definition_id = output.contract.definition_id;
        provenance.owner_module = output.owner_module;
        provenance.node_id = output.node_id;
        provenance.instance_id = output.instance_id;
        provenance.invocation_id = output.invocation.invocation_id;
        for (uint32_t index = 0; index < output.intent.operations.size(); ++index)
            work.push_back({feature::FeatureDomain::kRender, provenance.definition_id, provenance.owner_module,
                            provenance.node_id, provenance.instance_id, frame_index,
                            make_work_item_id(provenance, frame_index, RenderWorkItemKind::kExecute,
                                              output.invocation.generation.value(), index),
                            RenderWorkItemKind::kExecute, output.invocation, index});
    }
    return {foundation::Result::success(), std::move(work)};
}
}  // namespace aeg::render_graph
