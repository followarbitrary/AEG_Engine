/**
 * @file feature_runtime.hpp
 * @brief Feature instance and Process lifecycle runtime.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "feature_registry.hpp"
#include "feature_resource_bus.hpp"

namespace aeg::feature {

struct FeatureGraphDocument;
struct CompiledFeatureGraph;
enum class EdgeKind : uint8_t;
struct FeatureInvocationWork;
class FeatureExecution;
namespace detail {
struct FeatureOutboxJournal;
struct FeatureOutboxAccess;
struct FeatureInputState;
struct FeatureInputAccess;
struct FeatureExecutionControl;
class FeatureTimeSource;
struct ClockWorkDemand;
}

struct StateStore final {
    StateVersion version{StateVersion(1)};
    uint64_t commit_count{0};
    uint64_t payload_revision{0};
    Version schema_version{1, 0, 0};
    std::vector<uint8_t> payload{};
};

enum class ProcessStatus : uint8_t { kReady, kSuspended, kCancelled, kCompleted };

struct ProcessInstance final {
    ProcessId id{};
    FeatureInstanceId owner{};
    /** Module provenance is snapshotted with the owning Process. */
    ModuleIdentity owner_module{};
    // A Continuous + ProcessState service may explicitly manage child
    // Process Features.  Child identities remain independent so each one
    // carries its own AttemptId/StateVersion and can reject stale results.
    ProcessId parent{};
    std::vector<ProcessId> children{};
    ProcessRevision revision{ProcessRevision(1)};
    StateStore state{};
    ProcessStatus status{ProcessStatus::kReady};
    std::vector<std::string> states{};
    std::vector<std::string> transitions{};
    uint32_t state_schema_version{0};
    std::string initial_state{};
    std::vector<std::string> terminal_states{};
    std::string current_state{};
};

/** Engine-only owning observations; issued/open does not imply callback entry. */
struct InvocationAttempt final {
    InvocationToken token{};
    ProcessId process{};
};

enum class InvocationFailureStage : uint8_t { kSubmission, kCallback, kCommit, kPublication };

struct InvocationFailure final {
    InvocationAttempt attempt{};
    InvocationFailureStage stage{};
    foundation::Result result{};
};

struct InvocationCommit final {
    InvocationAttempt attempt{};
    StateVersion state_version{};
    ProcessRevision process_revision{};
};

struct InvocationTimingObservation final {
    TransportTiming release{};
    std::optional<TransportTimePoint> callback_started{}, callback_finished{};
    bool deadline_exceeded{false}, budget_exceeded{false};
};

struct FeatureRuntimeOptions final {
    /** Thread safe monotonic nanoseconds; zero is a valid instant. */
    std::function<uint64_t()> now_ns{};
};

struct InvocationSnapshot final {
    struct OpenAttempt final {
        InvocationAttempt attempt{};
        /** Exact live token/contract/state/lifecycle checks, not a worker-running flag. */
        bool committable{false};
    };
    struct Retry final {
        InvocationToken origin{};
        uint32_t next_attempt{0};
        uint64_t due_tick{0};
    };
    std::vector<OpenAttempt> open_attempts{};
    /** History retains original generations when replacement or shutdown retires work. */
    std::optional<InvocationAttempt> last_issued{};
    std::optional<InvocationFailure> last_failed{};
    std::optional<InvocationCommit> last_committed{};
    std::optional<Retry> retry{};
    std::optional<InvocationTimingObservation> timing{};
};

struct FeatureInstance final {
    FeatureInstanceId id{};
    FeatureDefinitionId definition_id{};
    /** Module provenance is snapshotted when the instance is activated. */
    ModuleIdentity owner_module{};
    ScopeId scope{};
    OwnerPath owner_path{};
    StateStore state{};
    std::vector<ProcessId> processes{};
    std::vector<uint8_t> parameters{};
    // Inputs delivered by completed upstream Features.  The runtime keeps
    // them opaque; the target Feature validates its own schema.
    std::vector<uint8_t> pending_input{};
    uint32_t pending_input_messages{0};
    std::vector<PortMessage> pending_messages{};
    // Retry state is retained on the instance so a nonzero backoff can be
    // scheduled by the owning clock instead of being silently dropped.
    std::vector<uint8_t> retry_input{};
    std::vector<uint8_t> retry_parameters{};
    std::vector<PortMessage> retry_messages{};
    std::optional<StateSnapshot> retry_state{};
    InvocationToken retry_origin{};
    uint32_t retry_attempts{0};
    uint64_t retry_due_tick{0};
    bool retry_pending{false};
    std::vector<FeatureInstanceId> children{};
    std::optional<InvocationAttempt> last_invocation_issued{};
    std::optional<InvocationFailure> last_invocation_failed{};
    std::optional<InvocationCommit> last_invocation_committed{};
    std::optional<InvocationTimingObservation> last_execution_timing{};
    /** Derived binding to the declaration accepted by activation/migration. */
    uint64_t definition_digest{0};
    /** Allocation lifetime; ordinary commits and unrelated migrations do not change it. */
    uint64_t render_resource_epoch{1};
    struct CommittedRenderIntent final {
        InvocationToken invocation{};
        RenderIntent intent{};
        /** Exact consumed graph deliveries, captured before callback/retry. */
        std::vector<PortMessage> input_messages{};
        /** Process revision after this owner commit; tokens retain their invocation provenance. */
        ProcessRevision process_revision{};
    };
    std::optional<CommittedRenderIntent> render_output{};
};

struct RenderResourceOwner final {
    FeatureInstanceId instance_id{};
    FeatureNodeId node_id{};
    uint64_t definition_digest{0};
    uint64_t resource_epoch{0};
    friend bool operator==(const RenderResourceOwner&, const RenderResourceOwner&) = default;
};

/** Frame-captured values remain owned after Runtime publication or module retirement. */
struct RenderFeatureOutput final {
    FeatureContract contract{};
    ModuleIdentity owner_module{};
    FeatureNodeId node_id{};
    FeatureInstanceId instance_id{};
    OwnerPath owner_path{};
    InvocationToken invocation{};
    RenderIntent intent{};
    RenderResourceOwner resource_owner{};
    /** Engine evidence; never supplied by StateDelta or explicit caller bytes. */
    std::vector<PortMessage> input_messages{};
    /** Optional stable RenderView scope; zero keeps legacy single-view behavior. */
    uint64_t view_key{0};
};

struct ExecutionPlan final {
    ClockId clock{};
    std::vector<FeatureInstanceId> instances{};
};

class FeatureRuntime final {
public:
    explicit FeatureRuntime(FeatureRegistry& registry, AssetKindResolver asset_kind_resolver = {},
                            FeatureRuntimeOptions options = {});
    ~FeatureRuntime();
    FeatureRuntime(const FeatureRuntime&) = delete;
    FeatureRuntime& operator=(const FeatureRuntime&) = delete;
    /** Empty replacement retaining this host's time source, origin and domain. */
    [[nodiscard]] std::unique_ptr<FeatureRuntime> make_reset_runtime() const;
    [[nodiscard]] std::unique_ptr<FeatureRuntime> make_reset_runtime(
        FeatureRegistry& registry, AssetKindResolver asset_kind_resolver) const;

    /** Engine-owned policy; does not add executor types to author contracts. */
    [[nodiscard]] foundation::Result enable_async_execution();
    [[nodiscard]] foundation::Result pump_completions();
    [[nodiscard]] foundation::Result shutdown_execution();
    [[nodiscard]] size_t pending_execution_count() const noexcept;
    [[nodiscard]] foundation::ResultValue<std::vector<RenderFeatureOutput>> capture_render_outputs(
        const CompiledFeatureGraph& graph) const;
    /** Engine diagnostic: waiting preserves the preceding complete CPU cohort. */
    [[nodiscard]] foundation::Result render_publication_status(ClockId clock) const;
    /** Captures live Atomic Render owners independently of whether they emitted an intent. */
    [[nodiscard]] foundation::ResultValue<std::vector<RenderResourceOwner>> capture_render_resource_owners(
        const CompiledFeatureGraph& graph) const;

    [[nodiscard]] foundation::ResultValue<FeatureInstanceId> activate(
        const ExpandedFeature& feature, ScopeId scope);
    [[nodiscard]] foundation::ResultValue<InvocationToken> invoke(FeatureInstanceId instance);
    [[nodiscard]] foundation::Result dispatch_call(FeatureInstanceId instance,
                                                   std::span<const uint8_t> input = {});
    [[nodiscard]] foundation::Result dispatch_process(FeatureInstanceId instance,
                                                      std::span<const uint8_t> input = {});
    [[nodiscard]] foundation::Result tick(ClockId clock);
    [[nodiscard]] foundation::Result advance(double delta_seconds);
    [[nodiscard]] foundation::Result publish_graph(const FeatureGraphDocument& graph,
                                                   CompiledFeatureGraph& compiled);
    [[nodiscard]] std::vector<ClockId> clocks() const;
    [[nodiscard]] foundation::Result suspend(ProcessId process);
    [[nodiscard]] foundation::Result resume(ProcessId process);
    [[nodiscard]] foundation::Result cancel(ProcessId process);
    [[nodiscard]] foundation::Result complete(const InvocationToken& token,
                                               const StateDelta& delta);
    [[nodiscard]] foundation::Result transition(ProcessId process, std::string_view next_state);
    [[nodiscard]] foundation::ResultValue<InvocationToken> retry(ProcessId process);
    [[nodiscard]] foundation::Result replace_definition(const FeatureDefinition& definition,
                                                        const StateMigration& migration);

    [[nodiscard]] foundation::ResultValue<ProcessId> process(FeatureInstanceId instance) const;
    [[nodiscard]] const FeatureInstance* find_instance(FeatureInstanceId id) const noexcept;
    [[nodiscard]] const ProcessInstance* find_process(ProcessId id) const noexcept;
    [[nodiscard]] RuntimeGeneration generation() const noexcept;
    /** Owner-thread query, including history after execution has closed. */
    [[nodiscard]] foundation::ResultValue<InvocationSnapshot> inspect_invocations(FeatureInstanceId instance) const;

private:
    friend struct detail::FeatureOutboxAccess;
    friend struct detail::FeatureInputAccess;
    friend struct detail::FeatureExecutionControl;
    struct EdgeRoute final {
        PortId source_port{};
        PortId target_port{};
        ValueSchemaId schema{};
        uint32_t schema_version{0};
        FeatureInstanceId target{};
        EdgeKind kind{};
        uint32_t capacity{0};
        uint32_t deadline_ticks{0};
    };

    struct InvocationRecord final {
        InvocationToken token{};
        FeatureInstanceId instance{};
        ProcessId process{};
        // Consume exactly the deliveries captured by this invocation. A
        // Snapshot replacement arriving during backoff has a different id.
        std::vector<PortMessage> consumed_messages{};
        TransportTiming timing{};
    };

    [[nodiscard]] foundation::ResultValue<FeatureInstanceId> activate_expanded(
        const ExpandedFeature& feature, ScopeId scope);
    [[nodiscard]] foundation::Result collect_clock_workload(
        std::vector<detail::ClockWorkDemand>& demands, const FeatureDefinition* replacement = nullptr) const;
    [[nodiscard]] foundation::ResultValue<InvocationToken> invoke_internal(
        FeatureInstance& instance, ProcessInstance* process, StatePolicy state_policy);
    [[nodiscard]] foundation::Result invoke_and_complete(FeatureInstanceId instance,
                                                          std::span<const uint8_t> input,
                                                          bool replay_retry = false,
                                                          bool clock_scheduled = false);
    [[nodiscard]] foundation::Result finish_invocation(const FeatureInvocationWork& work);
    [[nodiscard]] foundation::Result validate_invocation(const InvocationRecord& record,
                                                         const InvocationToken& token) const;
    [[nodiscard]] foundation::Result commit_invocation(const InvocationToken& token, const StateDelta& delta);
    void record_invocation_failure(FeatureInstanceId instance, const InvocationAttempt& attempt,
                                   InvocationFailureStage stage, const foundation::Result& result);
    [[nodiscard]] bool on_owner_thread() const noexcept;
    [[nodiscard]] bool prerequisites_busy(FeatureInstanceId instance) const;
    [[nodiscard]] bool plan_busy(ClockId clock) const;
    [[nodiscard]] foundation::Result schedule_plan(ClockId clock, TransportTimePoint released);
    [[nodiscard]] foundation::Result release_clock(ClockId clock, TransportTimePoint released);
    [[nodiscard]] foundation::Result check_timing(const InvocationRecord& record);
    [[nodiscard]] foundation::Result check_timing(const InvocationRecord& record, TransportTimePoint now);
    void retire_expired_messages(FeatureInstanceId instance, TransportTimePoint now);
    [[nodiscard]] foundation::Result pump_completions_impl(bool admit_queued_work);
    [[nodiscard]] foundation::Result settle_render_publications();
    void discard_dependents(FeatureInstanceId source);
    [[nodiscard]] ProcessInstance* mutable_process(ProcessId id) noexcept;
    [[nodiscard]] bool children_completed(ProcessId id) const noexcept;
    void cascade_process_status(ProcessId id, ProcessStatus status) noexcept;
    void clear_process_retry(ProcessId id) noexcept;
    [[nodiscard]] foundation::Result validate_parameter_assets(
        std::span<const uint8_t> parameters, std::span<const ParameterContract> schema);

    FeatureRegistry& registry_;
    AssetKindResolver asset_kind_resolver_{};
    std::shared_ptr<detail::FeatureTimeSource> time_source_{};
    std::unordered_map<FeatureInstanceId, FeatureInstance,
                       StableIdHash<FeatureInstanceIdTag>> instances_{};
    std::unordered_map<ProcessId, ProcessInstance, StableIdHash<ProcessIdTag>> processes_{};
    std::unordered_map<InvocationId, InvocationRecord, StableIdHash<InvocationIdTag>> invocations_{};
    std::unordered_map<ClockId, ExecutionPlan, StableIdHash<ClockIdTag>> plans_{};
    std::unordered_map<FeatureInstanceId, std::vector<EdgeRoute>,
                       StableIdHash<FeatureInstanceIdTag>> routes_{};
    ResourceBus resource_bus_{};
    struct ClockSchedule final { uint64_t elapsed_ns{0}, next_period{1}; };
    std::unordered_map<ClockId, ClockSchedule, StableIdHash<ClockIdTag>> clock_elapsed_{};
    long double delta_remainder_ns_{0};
    std::unordered_map<ClockId, uint64_t, StableIdHash<ClockIdTag>> clock_ticks_{};
    uint64_t next_instance_{1};
    uint64_t next_process_{1};
    uint64_t next_invocation_{1};
    uint64_t next_attempt_{1};
    RuntimeGeneration generation_{};
    bool state_callback_active_{false};
    std::unique_ptr<detail::FeatureOutboxJournal> outbox_{};
    std::unique_ptr<detail::FeatureInputState> input_{};
    std::unique_ptr<FeatureExecution> execution_{};
    bool execution_closed_{false};
    bool async_execution_{false};
    std::vector<FeatureInstanceId> deferred_{};
    std::unordered_map<ClockId, std::deque<TransportTimePoint>, StableIdHash<ClockIdTag>> execution_backlog_{};
    std::unordered_map<FeatureInstanceId, TransportTiming, StableIdHash<FeatureInstanceIdTag>> release_timings_{};
    struct RenderOwnerState final {
        RenderResourceOwner resource{};
        ProcessRevision process_revision{};
    };
    struct RenderPublication final {
        std::vector<RenderFeatureOutput> outputs{};
        std::vector<RenderOwnerState> owners{};
        foundation::Result readiness{foundation::not_ready("Render Clock has no complete publication")};
    };
    [[nodiscard]] bool render_publication_current(const RenderPublication& publication) const;
    std::unordered_map<FeatureInstanceId, FeatureNodeId, StableIdHash<FeatureInstanceIdTag>> render_nodes_{};
    std::unordered_map<ClockId, RenderPublication, StableIdHash<ClockIdTag>> render_publications_{};
    std::unordered_set<ClockId, StableIdHash<ClockIdTag>> render_dirty_{};
    bool render_boundary_active_{false};
};

}  // namespace aeg::feature
