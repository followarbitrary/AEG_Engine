#include "render_intent_planner.hpp"
#include "feature_graph.hpp"
#include "render_intent_evidence.hpp"

#include <algorithm>
#include <unordered_map>

namespace aeg::feature {

foundation::ResultValue<RenderIntentExecutionPlan> RenderIntentPlanner::plan(
    std::span<const RenderFeatureOutput> outputs,
    std::span<const RenderFeatureOutput> accepted) const {
    return plan_with_retired_sources(outputs, accepted, {});
}

foundation::ResultValue<RenderIntentExecutionPlan> RenderIntentPlanner::plan_with_retired_sources(
    std::span<const RenderFeatureOutput> outputs, std::span<const RenderFeatureOutput> accepted,
    std::span<const std::shared_ptr<const detail::RenderLightListEvidence>> retired) const {
    const auto failure = [](const char* message) {
        return foundation::ResultValue<RenderIntentExecutionPlan>{foundation::invalid_argument(message), {}};
    };
    struct Producer {
        size_t output;
        uint32_t operation;
        const RenderLightCullingScene* declaration;
        const RenderFeatureOutput* source;
        bool retained{false};
        bool retired{false};
    };
    std::vector<Producer> producers;
    std::unordered_map<FeatureInstanceId, size_t, StableIdHash<FeatureInstanceIdTag>> owners;
    std::vector<std::vector<size_t>> dependents(outputs.size());
    std::vector<size_t> indegree(outputs.size());
    const auto edge = [&](size_t producer, size_t consumer) {
        if (producer == consumer) return;
        auto& targets = dependents[producer];
        if (std::find(targets.begin(), targets.end(), consumer) == targets.end()) {
            targets.push_back(consumer);
            ++indegree[consumer];
        }
    };
    for (size_t index = 0; index < outputs.size(); ++index) {
        const auto& output = outputs[index];
        if (!output.instance_id.is_valid() || !output.node_id.is_valid() ||
            !output.invocation.invocation_id.is_valid() || !output.invocation.generation.is_valid() ||
            !owners.emplace(output.instance_id, index).second)
            return failure("Render outputs have invalid or duplicate committed provenance");
        if (const auto result = validate_render_intent(output.intent, output.contract); !result) return {result, {}};
        const bool owns_light_list = std::any_of(output.intent.operations.begin(), output.intent.operations.end(), [](const auto& op) {
            return std::holds_alternative<RenderLightCullingScene>(op) || std::holds_alternative<RenderDrawClusteredScene>(op);
        });
        // Every light-list owner must be Runtime captured; other existing intent
        // paths still perform their renderer/history owner checks at lowering.
        if (owns_light_list && (output.resource_owner.instance_id != output.instance_id ||
            output.resource_owner.node_id != output.node_id || output.resource_owner.definition_digest == 0 ||
            output.resource_owner.resource_epoch == 0))
            return failure("light-list output owner provenance is inconsistent");
        for (uint32_t operation = 0; operation < output.intent.operations.size(); ++operation)
            if (const auto* cull = std::get_if<RenderLightCullingScene>(&output.intent.operations[operation]))
                producers.push_back({index, operation, cull, &output});
    }
    // Only an already admitted CPU cohort can establish normal readiness.
    // Equal bytes or an arbitrary old token alone cannot turn an error into waiting.
    for (const auto& output : accepted) {
        const auto current = owners.find(output.instance_id);
        if (current == owners.end()) continue;
        const auto& live = outputs[current->second];
        if (live.resource_owner != output.resource_owner || live.contract.clock.id != output.contract.clock.id ||
            live.invocation.generation != output.invocation.generation || live.invocation == output.invocation) continue;
        for (uint32_t operation = 0; operation < output.intent.operations.size(); ++operation)
            if (const auto* cull = std::get_if<RenderLightCullingScene>(&output.intent.operations[operation]))
                producers.push_back({current->second, operation, cull, &output, true});
    }
    // Runtime alone supplies lifecycle-retired, immutable export declarations.
    // Keep the ordinary receipt and scene checks below; waiting is not authorization.
    for (const auto& evidence : retired) {
        const auto& source = evidence->source;
        for (uint32_t operation = 0; operation < source.intent.operations.size(); ++operation)
            if (const auto* cull = std::get_if<RenderLightCullingScene>(&source.intent.operations[operation]))
                producers.push_back({outputs.size(), operation, cull, &source, true, true});
    }
    RenderIntentExecutionPlan plan;
    bool waiting = false;
    for (size_t consumer = 0; consumer < outputs.size(); ++consumer) {
        const auto& output = outputs[consumer];
        // Preserve all real same-clock DataEdge dependencies, including camera
        // depth/HDR producers, rather than sorting just light-list references.
        for (const auto& message : output.input_messages) {
            if (message.kind != EdgeKind::kData || message.source.clock != output.contract.clock.id) continue;
            const auto source = owners.find(message.source.instance);
            if (source != owners.end()) edge(source->second, consumer);
        }
        for (uint32_t operation = 0; operation < output.intent.operations.size(); ++operation) {
            const auto* draw = std::get_if<RenderDrawClusteredScene>(&output.intent.operations[operation]);
            if (!draw) continue;
            const Producer* selected = nullptr;
            for (const auto& producer : producers) {
                const auto& source = *producer.source;
                const auto& cull = *producer.declaration;
                const auto& reference = draw->light_list;
                if (cull.output.id != reference.id || cull.output.version != reference.version ||
                    source.invocation.invocation_id != reference.producer_invocation ||
                    source.invocation.generation != reference.producer_generation) continue;
                bool authorized = !producer.retained && producer.output == consumer && producer.operation < operation;
                if (producer.output != consumer && cull.output.export_port.is_valid()) {
                    for (const auto& message : output.input_messages) {
                        if (message.kind != EdgeKind::kData || !message.delivery_id.is_valid() ||
                            message.schema != kRenderLightListSchema || message.schema_version != 1 ||
                            message.source_port != cull.output.export_port ||
                            message.source.instance != source.instance_id || message.source.invocation != source.invocation ||
                            message.source.definition_digest != source.resource_owner.definition_digest ||
                            message.source.resource_epoch != source.resource_owner.resource_epoch ||
                            message.source.clock != source.contract.clock.id || source.contract.clock.id != output.contract.clock.id)
                            continue;
                        const auto port = std::find_if(output.contract.ports.begin(), output.contract.ports.end(), [&](const auto& port) {
                            return port.id == message.target_port && port.direction == PortDirection::kInput &&
                                   port.schema == kRenderLightListSchema && port.version == 1;
                        });
                        if (port == output.contract.ports.end()) continue;
                        const auto decoded = decode_render_light_list_reference(message.payload);
                        if (decoded && decoded.value == reference) authorized = true;
                    }
                }
                if (!authorized) continue;
                if (selected) return failure("light-list reference selects more than one producer");
                selected = &producer;
            }
            if (!selected) return failure("light-list reference is missing, stale or has no consumed DataEdge authorization");
            const auto& cull = *selected->declaration;
            const auto same_texture = [](const auto& first, const auto& second) {
                return first.id == second.id && first.version == second.version && first.external == second.external;
            };
            if (cull.scene.id != draw->scene.id || cull.scene.version != draw->scene.version ||
                !same_texture(cull.camera_depth, draw->depth.target) ||
                cull.directional_shadow.has_value() != draw->directional_shadow.has_value() ||
                (cull.directional_shadow && !same_texture(*cull.directional_shadow, *draw->directional_shadow)))
                return failure("clustered scene/depth/shadow differs from its light-list producer");
            waiting = waiting || selected->retained;
            if (!selected->retired) {
                plan.light_list_uses.push_back({consumer, operation, selected->output, selected->operation});
                edge(selected->output, consumer);
            }
        }
    }
    // Stable Kahn ordering keeps unrelated outputs in their supplied order.
    std::vector<bool> emitted(outputs.size());
    while (plan.output_order.size() != outputs.size()) {
        size_t next = outputs.size();
        for (size_t index = 0; index < outputs.size(); ++index)
            if (!emitted[index] && indegree[index] == 0) { next = index; break; }
        if (next == outputs.size()) return failure("Render input dependencies contain a cycle");
        emitted[next] = true;
        plan.output_order.push_back(next);
        for (const auto target : dependents[next]) --indegree[target];
    }
    if (waiting) return {foundation::not_ready("Render consumers await a current light-list producer"), {}};
    return {foundation::Result::success(), std::move(plan)};
}

foundation::ResultValue<const RenderLightCullingScene*> detail::light_list_export_declaration(
    const RenderIntent* intent, const PortMessage& message, const InvocationToken& token) {
    if (message.schema != kRenderLightListSchema) return {foundation::Result::success(), nullptr};
    const auto decoded = decode_render_light_list_reference(message.payload);
    if (!decoded) return {decoded.result, nullptr};
    if (message.schema_version != 1 || intent == nullptr || decoded.value.producer_invocation != token.invocation_id ||
        decoded.value.producer_generation != token.generation)
        return {foundation::invalid_argument("light-list export requires the current invocation declaration"), nullptr};
    for (const auto& operation : intent->operations)
        if (const auto* cull = std::get_if<RenderLightCullingScene>(&operation); cull &&
            cull->output.export_port == message.source_port && cull->output.id == decoded.value.id &&
            cull->output.version == decoded.value.version) return {foundation::Result::success(), cull};
    return {foundation::invalid_argument("light-list export has no matching producer declaration"), nullptr};
}

} // namespace aeg::feature
