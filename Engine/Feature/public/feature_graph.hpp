/**
 * @file feature_graph.hpp
 * @brief Feature-only authoring graph and clock-aware compiler.
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "feature_runtime.hpp"
#include "feature_catalog.hpp"
#include "feature_process.hpp"

namespace aeg::feature {

struct FeatureNode final {
    FeatureNodeId node_id{};
    FeatureDefinitionId definition_id{};
    Version definition_version{};
    OwnerPath owner_path{};
    std::vector<uint8_t> parameter_blob{};
};

struct FeatureEdge final {
    FeatureNodeId source_node{};
    PortId source_port{};
    FeatureNodeId target_node{};
    PortId target_port{};
    EdgeKind kind{EdgeKind::kData};
    ClockId source_clock{};
    ClockId target_clock{};
    uint32_t version{0};
    uint32_t capacity{0};
    uint32_t deadline_ticks{0};
};

struct ProcessSubgraph final {
    FeatureNodeId owner_node{};
    std::vector<std::string> states{};
    std::vector<std::string> transitions{};
    uint32_t state_schema_version{0};
    // Explicit state-machine entry. Empty is only an authoring convenience;
    // the compiler canonicalizes it to the first declared state.
    std::string initial_state{};
    // Explicit terminal states. When empty, the compiler derives sink states.
    std::vector<std::string> terminal_states{};
};

struct FeatureGraphDocument final {
    std::string asset_uuid{};
    uint32_t schema_version{0};
    uint64_t graph_version{0};
    uint64_t draft_revision{0};
    uint64_t layout_revision{0};
    std::vector<std::string> module_requirements{};
    std::vector<FeatureNode> nodes{};
    std::vector<FeatureEdge> edges{};
    std::vector<ProcessSubgraph> process_subgraphs{};
    std::vector<std::string> input_bindings{};
    std::vector<std::string> variadic_slots{};
    std::vector<std::string> extensions{};
    uint64_t contract_digest{0};
};

/** Canonical execution digest, independent of graph collection order and editor revisions.
 * Owner paths, parameter bytes, Process declaration lists and variadic slots retain order.
 * Reserved Process/Feature layout records are excluded; business extensions remain.
 * This is not structural validation: duplicate identities still require the compiler.
 */
[[nodiscard]] uint64_t graph_semantic_digest(const FeatureGraphDocument& graph);

struct FeatureGraphDiagnostic final {
    std::string message{};
    FeatureNodeId node_id{};
    PortId port_id{};
    OwnerPath owner_path{};
};

struct FeatureNodeProvenance final {
    FeatureDefinitionId definition_id{};
    FeatureNodeId node_id{};
    FeatureInstanceId instance_id{};
    ProcessId process_id{};
    InvocationId invocation_id{};
    OwnerPath owner_path{};
    ClockId clock{};
    // A Composite may expand to multiple leaf domains.  Keep the complete
    // set instead of projecting it to an arbitrary "primary" domain.
    FeatureDomainMask domains{0};
    /** Module provenance is retained for diagnostics and editor attribution. */
    ModuleIdentity owner_module{};
};

struct CompiledFeatureGraph final {
    std::vector<ExecutionPlan> plans{};
    std::vector<FeatureNodeProvenance> provenance{};
    std::vector<FeatureGraphDiagnostic> diagnostics{};
};

/** @brief Canonicalizes and validates one authored Process subgraph. */
[[nodiscard]] foundation::ResultValue<ProcessStateMachine> canonicalize_process_subgraph(
    const ProcessSubgraph& process, const FeatureContract& contract);

class FeatureGraphCompiler final {
public:
    [[nodiscard]] foundation::ResultValue<CompiledFeatureGraph> compile(
        const FeatureGraphDocument& graph, const FeatureRegistry& registry) const;
};

/** @brief Validates an editor edge against the immutable module catalog. */
[[nodiscard]] foundation::Result validate_feature_edge(const FeatureGraphDocument& graph,
                                                        const FeatureEdge& edge,
                                                        const FeatureCatalog& catalog);

}  // namespace aeg::feature
