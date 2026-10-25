/**
 * @file feature_contract.hpp
 * @brief Author-facing Feature contract and validation rules.
 */
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <span>
#include <optional>

#include "feature_types.hpp"
#include "feature_parameters.hpp"
#include "feature_port_router.hpp"
#include "feature_render_intent.hpp"
#include "result.hpp"

namespace aeg::feature {

struct OwnerPath final {
    std::vector<FeatureNodeId> nodes{};

    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }
};

/** Owned immutable input to an invocation; no spans alias the Runtime store. */
struct StateSnapshot final {
    StateVersion version{};
    Version schema_version{};
    std::vector<uint8_t> payload{};
    std::string current_state{};
};

struct StatePayloadContract final {
    Version schema_version{1, 0, 0};
    uint32_t max_size{1024 * 1024};
    friend bool operator==(const StatePayloadContract&, const StatePayloadContract&) = default;
};

inline constexpr uint32_t kMaxOutboxIntentsPerCommit = 1024;
inline constexpr uint32_t kMaxOutboxIntentBytes = 1024 * 1024;

/** Versioned command bytes; only the Runtime supplies committed delivery provenance. */
struct OutboxIntent final {
    SideEffectId effect{};
    uint32_t version{0};
    std::vector<uint8_t> payload{};
};

struct StateDelta final {
    StateVersion expected_version{};
    uint64_t payload_revision{0};
    std::string next_state{};
    bool process_completed{false};
    /**
     * Payload emitted at the commit boundary.  The graph runtime routes this
     * opaque, versioned byte sequence through declared edges; a Feature owns
     * its schema and the orchestrator never interprets the bytes.
     */
    std::vector<uint8_t> output_payload{};
    /** Port-preserving output envelopes.  Each envelope selects one source port. */
    std::vector<PortMessage> output_messages{};
    /** Missing preserves state; an engaged empty vector explicitly clears it. */
    std::optional<std::vector<uint8_t>> state_payload{};
    /** Missing retains accepted presentation; an engaged empty intent retires its work. */
    std::optional<RenderIntent> render_intent{};
    /** Commands append at the same owner commit boundary; empty preserves pending deliveries. */
    std::vector<OutboxIntent> outbox{};
};

struct StateMigration final {
    Version from_definition{};
    Version to_definition{};
    Version from_schema{};
    Version to_schema{};
    /** Pure transformation of each owner's snapshot, staged before publication. */
    std::function<foundation::ResultValue<std::vector<uint8_t>>(const StateSnapshot&)> migrate_payload{};
};

/**
 * @brief Explicit boundary mapping for a Composite Feature port.
 *
 * Each mapping connects a parent port to one child definition port. Multiple
 * mappings on the same parent port form an explicit fan-out; runtime routing
 * never broadcasts implicitly to undeclared children.
 */
struct CompositePortMapping final {
    PortId parent_port{};
    FeatureDefinitionId child_definition{};
    PortId child_port{};
    PortDirection direction{PortDirection::kInput};

    friend constexpr bool operator==(const CompositePortMapping&, const CompositePortMapping&) = default;
};

struct FeatureContract final {
    FeatureDefinitionId definition_id{};
    Version version{1, 0, 0};
    FeatureStructure structure{FeatureStructure::kAtomic};
    FeatureDomainMask domains{0};
    ExecutionKind execution{ExecutionKind::kCall};
    StatePolicy state{StatePolicy::kStateless};
    ScopeContract scope{};
    ClockContract clock{};
    std::vector<PortContract> ports{};
    std::vector<ParameterContract> parameters{};
    PermissionContract permissions{};
    std::vector<SideEffectContract> side_effects{};
    std::vector<CompositePortMapping> child_port_mappings{};
    FailurePolicy failure{FailurePolicy::kStop};
    RetryPolicy retry{};
    std::vector<FeatureDefinitionId> children{};
    bool has_continuation{false};
    bool allows_child_process{false};
    StatePayloadContract state_payload{};
};

struct FeatureDefinition final {
    FeatureDefinitionId id{};
    Version version{};
    /** Stable module identity that owns the executable callback and contract. */
    ModuleIdentity owner_module{};
    FeatureContract contract{};
    // Executable implementation is module-owned and never serialized into an asset.
    // State, input messages and the complete parameter envelope are read-only
    // snapshots, valid for the duration of the callback. Copy retained data.
    std::function<foundation::ResultValue<StateDelta>(const InvocationToken&,
                                                     const StateSnapshot&,
                                                     std::span<const uint8_t>,
                                                     std::span<const uint8_t>)> execute{};
    /** Optional pure schema validation, executed by the state owner before commit. */
    std::function<foundation::Result(std::span<const uint8_t>)> validate_state_payload{};
};

/** @brief Stable digest of one complete Feature contract and definition identity. */
[[nodiscard]] uint64_t feature_contract_digest(const FeatureDefinition& definition);

[[nodiscard]] foundation::Result validate_feature_contract(const FeatureContract& contract);
/** Structural declaration validation only; host approval is a separate gate. */
[[nodiscard]] foundation::Result validate_clock_contract(const ClockContract& clock, FeatureStructure structure);
/** Validates an editor draft; a Composite may still be awaiting its children. */
[[nodiscard]] foundation::Result validate_feature_contract_draft(const FeatureContract& contract);

}  // namespace aeg::feature
