/** @brief Test-only pure scalar Features and authenticated owner-result projection.
 * This is not a production World/ECS outbox or projection bridge.
 */
#pragma once

#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <vector>

#include "feature_graph.hpp"

namespace aeg::tests::logic {
using namespace feature;

inline constexpr ClockId kClock{"Fixture.LogicTick"};
inline constexpr ModuleIdentity kModule{"com.aeg.pipeline.fixture"};
inline constexpr ValueSchemaId kScalarSchema{"Fixture.Int32.v1"};

inline std::vector<uint8_t> encode(int32_t value) {
    const auto bits = std::bit_cast<uint32_t>(value);
    return {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
        static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24)};
}

inline foundation::ResultValue<int32_t> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() != 4) return {foundation::invalid_argument("Fixture Int32 schema requires four bytes"), 0};
    uint32_t bits = 0;
    for (unsigned index = 0; index != 4; ++index) bits |= uint32_t{bytes[index]} << (index * 8);
    return {foundation::Result::success(), std::bit_cast<int32_t>(bits)};
}

struct CallbackObservation final {
    FeatureDefinitionId definition{};
    InvocationToken token{};
    int32_t input{0}, output{0};
};

struct ScalarOperation final {
    int32_t value{0};
    int32_t increment{0};
    std::optional<int32_t> expected_input{};
};

inline FeatureDefinition definition(const char* id, ScalarOperation operation,
    std::vector<CallbackObservation>& observations) {
    FeatureDefinition result{};
    result.id = FeatureDefinitionId(id); result.version = {1, 0, 0}; result.owner_module = kModule;
    auto& contract = result.contract;
    contract.definition_id = result.id; contract.version = result.version;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kCall; contract.state = StatePolicy::kInstanceState;
    contract.scope = {ScopeId("World")}; contract.clock = aeg::feature::make_periodic_clock_contract(kClock, 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.state_payload = {{1, 0, 0}, 4};
    contract.ports = {{PortId("In"), PortDirection::kInput, kScalarSchema, 1},
        {PortId("Out"), PortDirection::kOutput, kScalarSchema, 1}};
    contract.side_effects = {{SideEffectId("Fixture.ScalarDelta"), 1,
        SideEffectKind::kStateDelta, CommitPolicy::kOwnerBoundary}};
    result.validate_state_payload = [](std::span<const uint8_t> bytes) { return decode(bytes).result; };
    result.execute = [id = result.id, operation, &observations](const InvocationToken& token,
        const StateSnapshot& state, std::span<const uint8_t> input, std::span<const uint8_t>)
        -> foundation::ResultValue<StateDelta> {
        if (state.version != token.state_version || state.schema_version != Version{1, 0, 0})
            return {foundation::invalid_state("Fixture callback has a stale owner snapshot"), {}};
        const auto scalar = input.empty() ? foundation::ResultValue<int32_t>{foundation::Result::success(), 0} : decode(input);
        if (!scalar) return {scalar.result, {}};
        // This valid request deliberately makes the callback submit an invalid
        // delta, so schema rejection is tested at the real Runtime commit.
        const bool bad_schema = !input.empty() && scalar.value == -999;
        if (!bad_schema && operation.expected_input && (input.empty() || scalar.value != *operation.expected_input))
            return {foundation::invalid_argument("Fixture DataEdge supplied the wrong scalar input"), {}};
        const int32_t output = operation.expected_input ? scalar.value + operation.increment : operation.value;
        observations.push_back({id, token, scalar.value, output});
        StateDelta delta{token.state_version};
        delta.state_payload = encode(output); delta.output_payload = encode(output);
        if (bad_schema) delta.state_payload->pop_back();
        return {foundation::Result::success(), std::move(delta)};
    };
    return result;
}

inline FeatureGraphDocument graph(const char* uuid, const FeatureRegistry& registry) {
    FeatureGraphDocument result{};
    result.asset_uuid = uuid; result.schema_version = 1; result.graph_version = 1;
    result.contract_digest = registry.contract_digest();
    return result;
}

inline foundation::ResultValue<CompiledFeatureGraph> publish(FeatureRuntime& runtime,
    const FeatureRegistry& registry, const FeatureGraphDocument& document) {
    auto compiled = FeatureGraphCompiler{}.compile(document, registry);
    if (!compiled) return compiled;
    if (const auto result = runtime.publish_graph(document, compiled.value); !result) return {result, {}};
    return compiled;
}

inline const FeatureNodeProvenance* owner(const CompiledFeatureGraph& graph, FeatureNodeId node) {
    const auto row = std::find_if(graph.provenance.begin(), graph.provenance.end(),
        [node](const auto& value) { return value.node_id == node; });
    return row == graph.provenance.end() ? nullptr : &*row;
}

inline foundation::ResultValue<InvocationToken> committed_token(const FeatureRuntime& runtime, FeatureInstanceId owner) {
    const auto snapshot = runtime.inspect_invocations(owner);
    if (!snapshot) return {snapshot.result, {}};
    if (!snapshot.value.last_committed) return {foundation::invalid_state("Fixture owner has no committed invocation"), {}};
    return {foundation::Result::success(), snapshot.value.last_committed->attempt.token};
}

/** Authenticates current Runtime state before a test-specific artifact owner applies it. */
class ScalarProjection final {
public:
    explicit ScalarProjection(FeatureNodeProvenance binding) : binding_(std::move(binding)) {}

    foundation::ResultValue<int32_t> read(const FeatureRegistry& registry, const FeatureRuntime& runtime,
        const CompiledFeatureGraph& compiled, FeatureInstanceId candidate, const InvocationToken& token) const {
        const auto* row = owner(compiled, binding_.node_id);
        const auto* live = runtime.find_instance(candidate);
        const auto* declaration = live ? registry.find_definition(live->definition_id) : nullptr;
        const auto history = runtime.inspect_invocations(candidate);
        auto expected_path = binding_.owner_path.nodes; expected_path.push_back(binding_.node_id);
        const bool scheduled = std::any_of(compiled.plans.begin(), compiled.plans.end(), [&](const auto& plan) {
            return plan.clock == binding_.clock &&
                std::find(plan.instances.begin(), plan.instances.end(), candidate) != plan.instances.end();
        });
        if (candidate != binding_.instance_id || row == nullptr || row->instance_id != candidate ||
            row->definition_id != binding_.definition_id || row->owner_module != binding_.owner_module ||
            row->owner_path.nodes != binding_.owner_path.nodes || row->clock != binding_.clock || !scheduled ||
            live == nullptr || declaration == nullptr || live->definition_id != binding_.definition_id ||
            live->owner_module != binding_.owner_module || live->owner_module != kModule ||
            live->owner_path.nodes != expected_path || live->scope != ScopeId("World") ||
            live->definition_digest != feature_contract_digest(*declaration) ||
            declaration->contract.state_payload.schema_version != Version{1, 0, 0} ||
            live->state.schema_version != declaration->contract.state_payload.schema_version ||
            token.generation != runtime.generation() || !history || !history.value.last_committed ||
            history.value.last_committed->attempt.token != token ||
            history.value.last_committed->state_version != live->state.version ||
            live->state.version != StateVersion(token.state_version.value() + 1) ||
            live->state.commit_count != token.input_version + 1 || applied_ == token.invocation_id)
            return {foundation::invalid_state("Fixture scalar has no current unconsumed bound Feature commit"), 0};
        return decode(live->state.payload);
    }

    void applied(const InvocationToken& token) noexcept { applied_ = token.invocation_id; }

private:
    FeatureNodeProvenance binding_{};
    InvocationId applied_{};
};
}  // namespace aeg::tests::logic
