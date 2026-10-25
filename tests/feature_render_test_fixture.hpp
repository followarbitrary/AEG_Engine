#pragma once

#include <array>
#include <stdexcept>
#include <vector>

#include "feature_graph.hpp"

namespace render_feature_test {
using namespace aeg;
using namespace aeg::feature;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline FeatureDefinition definition() {
    FeatureDefinition result{};
    result.id = FeatureDefinitionId("Render.IntentSmoke");
    result.version = {1, 0, 0};
    result.owner_module = ModuleIdentity("Render.Smoke");
    auto& contract = result.contract;
    contract.definition_id = result.id;
    contract.version = result.version;
    contract.domains = to_mask(FeatureDomain::kRender);
    contract.execution = ExecutionKind::kContinuous;
    contract.state = StatePolicy::kInstanceState;
    contract.scope = {ScopeId("Frame")};
    contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("Frame"), 60, 4, LateDataPolicy::kDrop,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions = {kAllRenderIntentCapabilities, 1};
    contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    contract.parameters = {{ParameterId("Channel"), 1, 1, true}};
    result.execute = [schema = contract.parameters](const InvocationToken& token, const StateSnapshot& state,
                                                   std::span<const uint8_t>, std::span<const uint8_t> parameters) {
        const auto decoded = decode_feature_parameters(parameters, schema);
        if (!decoded) return foundation::ResultValue<StateDelta>{decoded.result, {}};
        const auto channel = decoded.value.front().payload.front();
        const bool scene = channel == 3;
        const bool red = channel == 1 || channel == 4 || scene;
        const RenderTextureReference local{RenderResourceId("Scratch"), 1, false};
        const RenderTextureReference target{RenderResourceId(scene ? "Renderer.SceneColor" : red ? "Target.Red" : "Target.Blue"), 1, true};
        RenderIntent intent{};
        intent.textures = {{local.id, 1, 4, 4}};
        intent.operations = {RenderClearColor{local, red ? std::array{1.f, 0.f, 0.f, 1.f}
                                                       : std::array{0.f, 0.f, 1.f, 1.f}},
                             RenderCopyTexture{local, target, 4, 4}};
        if (scene) {
            intent.textures.front().lifetime = RenderTextureLifetime::kHistory;
            // Later real DLL commits copy existing history without another clear.
            if (!state.payload.empty()) intent.operations.erase(intent.operations.begin());
        }
        intent.final_output = target;
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{state.payload.empty() ? uint8_t{1} : uint8_t(state.payload[0] + 1)};
        delta.render_intent = std::move(intent);
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    return result;
}

inline FeatureGraphDocument document(const FeatureDefinition& source) {
    FeatureGraphDocument result{};
    result.asset_uuid = "asset://render-intent-smoke";
    result.schema_version = 1;
    result.graph_version = 1;
    for (uint8_t channel : {uint8_t{1}, uint8_t{2}}) {
        const std::vector<FeatureParameterRecord> records{{ParameterId("Channel"), 1, {channel}}};
        const auto parameters = encode_feature_parameters(records, source.contract.parameters);
        require(static_cast<bool>(parameters), "parameter encoding failed");
        result.nodes.push_back({FeatureNodeId(channel), source.id, source.version, {}, parameters.value});
    }
    return result;
}

}  // namespace render_feature_test
