/**
 * @file feature_types.hpp
 * @brief Stable identities and orthogonal dimensions for Feature contracts.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "stable_id.hpp"

namespace aeg::feature {

template <typename Tag>
using StableId = foundation::StableId<Tag>;

template <typename Tag>
using StableIdHash = foundation::StableIdHash<Tag>;

struct FeatureIdTag;
struct FeatureDefinitionIdTag;
/** Stable identity of the module that owns a Feature definition. */
struct ModuleIdentityTag;
struct FeatureNodeIdTag;
struct FeatureInstanceIdTag;
struct ProcessIdTag;
struct InvocationIdTag;
struct ScopeIdTag;
struct ClockIdTag;
struct OwnerPathIdTag;
struct PortIdTag;
struct ParameterIdTag;
struct MessageDeliveryIdTag;
struct ValueSchemaIdTag;
struct SideEffectIdTag;
struct ProcessRevisionTag;
struct AttemptIdTag;
struct StateVersionTag;
struct RuntimeGenerationTag;
struct RenderResourceIdTag;

using FeatureId = StableId<FeatureIdTag>;
using FeatureDefinitionId = StableId<FeatureDefinitionIdTag>;
using ModuleIdentity = StableId<ModuleIdentityTag>;
using ModuleIdentityHash = StableIdHash<ModuleIdentityTag>;
using FeatureNodeId = StableId<FeatureNodeIdTag>;
using FeatureInstanceId = StableId<FeatureInstanceIdTag>;
using ProcessId = StableId<ProcessIdTag>;
using InvocationId = StableId<InvocationIdTag>;
using ScopeId = StableId<ScopeIdTag>;
using ClockId = StableId<ClockIdTag>;
using OwnerPathId = StableId<OwnerPathIdTag>;
using PortId = StableId<PortIdTag>;
using ParameterId = StableId<ParameterIdTag>;
using MessageDeliveryId = StableId<MessageDeliveryIdTag>;
using ValueSchemaId = StableId<ValueSchemaIdTag>;
using SideEffectId = StableId<SideEffectIdTag>;
using ProcessRevision = StableId<ProcessRevisionTag>;
using AttemptId = StableId<AttemptIdTag>;
using StateVersion = StableId<StateVersionTag>;
using RuntimeGeneration = StableId<RuntimeGenerationTag>;
/** Shared identity used by Feature intents and the internal RenderGraph adapter. */
using RenderResourceId = StableId<RenderResourceIdTag>;

struct InvocationToken final {
    InvocationId invocation_id{};
    AttemptId attempt_id{};
    RuntimeGeneration generation{};
    uint64_t input_version{0};
    ProcessRevision process_revision{};
    StateVersion state_version{};
    friend bool operator==(const InvocationToken&, const InvocationToken&) = default;
};

struct Version final {
    uint16_t major{0};
    uint16_t minor{0};
    uint16_t patch{0};
    friend constexpr bool operator==(Version, Version) = default;
    friend constexpr auto operator<=>(Version, Version) = default;
};

enum class FeatureDomain : uint8_t {
    kLogic = 0,
    kPhysics = 1,
    kRender = 2,
    kAudio = 3,
    kStream = 4,
};

using FeatureDomainMask = uint8_t;

[[nodiscard]] constexpr FeatureDomainMask to_mask(FeatureDomain domain) noexcept {
    return static_cast<FeatureDomainMask>(1u << static_cast<uint8_t>(domain));
}

inline constexpr FeatureDomainMask kAllFeatureDomains =
    to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kPhysics) |
    to_mask(FeatureDomain::kRender) | to_mask(FeatureDomain::kAudio) |
    to_mask(FeatureDomain::kStream);

enum class FeatureStructure : uint8_t { kAtomic, kComposite };
enum class ExecutionKind : uint8_t { kCall, kProcess, kContinuous };
enum class StatePolicy : uint8_t {
    kStateless,
    kInstanceState,
    kProcessState,
    kExternalSnapshot,
};

[[nodiscard]] constexpr bool owns_state_payload(StatePolicy policy) noexcept {
    return policy == StatePolicy::kInstanceState || policy == StatePolicy::kProcessState;
}
enum class PortDirection : uint8_t { kInput, kOutput };
/** Transport policy used by a Feature graph edge. */
enum class EdgeKind : uint8_t { kData, kSnapshot, kQueue, kEvent, kCommand, kReceipt };
enum class LateDataPolicy : uint8_t { kReject, kUseLatest, kQueue, kDrop };
enum class SideEffectKind : uint8_t {
    kStateDelta,
    kIntent,
    kEvent,
    kCommand,
    kReceipt,
    kExternalIo,
};
enum class CommitPolicy : uint8_t { kOwnerBoundary, kOutbox, kExternalOwner };
enum class FailurePolicy : uint8_t { kStop, kRetry, kCompensate };

struct PermissionContract final {
    uint64_t capability_mask{0};
    uint32_t version{0};
};

struct RetryPolicy final {
    uint32_t max_attempts{1};
    uint32_t backoff_ticks{0};
};

struct ScopeContract final {
    ScopeId id{};
};

enum class DeadlinePolicy : uint8_t { kUnspecified = 0, kObserve, kRejectCommit };

struct ClockTiming final {
    uint64_t phase_ns{0};
    uint64_t relative_deadline_ns{0};
    DeadlinePolicy deadline_policy{DeadlinePolicy::kUnspecified};
    friend constexpr bool operator==(const ClockTiming&, const ClockTiming&) = default;
};

struct ClockContract final {
    ClockId id{};
    uint32_t frequency_hz{0};
    uint32_t max_backlog{0};
    LateDataPolicy late_data_policy{LateDataPolicy::kReject};
    Version capability_version{};
    std::optional<ClockTiming> timing{};
    uint64_t work_budget_ns{0};
};

/** Explicit author request; invalid frequency/policy/budget remain invalid. */
[[nodiscard]] inline ClockContract make_periodic_clock_contract(
    ClockId id, uint32_t frequency_hz, uint32_t max_backlog, LateDataPolicy late_data_policy,
    DeadlinePolicy deadline_policy, uint64_t work_budget_ns, uint64_t phase_ns) {
    const uint64_t deadline = frequency_hz == 0 ? 0
        : 1'000'000'000ull / frequency_hz + (1'000'000'000ull % frequency_hz != 0);
    return {id, frequency_hz, max_backlog, late_data_policy, {1, 0, 0},
            ClockTiming{phase_ns, deadline, deadline_policy}, work_budget_ns};
}

/** A shared Clock has one release schedule; each leaf declares its own budget. */
[[nodiscard]] inline bool same_clock_schedule(const ClockContract& left, const ClockContract& right) {
    return left.id == right.id && left.frequency_hz == right.frequency_hz &&
        left.max_backlog == right.max_backlog && left.late_data_policy == right.late_data_policy &&
        left.capability_version == right.capability_version && left.timing == right.timing;
}

struct PortContract final {
    PortId id{};
    PortDirection direction{PortDirection::kInput};
    ValueSchemaId schema{};
    uint32_t version{0};
    friend constexpr bool operator==(const PortContract&, const PortContract&) = default;
};

struct SideEffectContract final {
    SideEffectId id{};
    uint32_t version{0};
    SideEffectKind kind{SideEffectKind::kStateDelta};
    CommitPolicy commit{CommitPolicy::kOwnerBoundary};
};

}  // namespace aeg::feature
