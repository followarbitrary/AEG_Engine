/**
 * @file feature_resource_bus.hpp
 * @brief ResourceBus semantic transport for cross-domain Feature communication.
 *
 * Implements the full EdgeKind transport policies:
 * - Data: Direct value transfer with backpressure
 * - Snapshot: Immutable state capture with versioning
 * - Queue: Buffered message stream with ordering guarantees
 * - Event: Fire-and-forget notifications with sequence numbers
 * - Command: Request messages expecting Receipt responses
 * - Receipt: Response messages completing Command transactions
 */
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "feature_port_router.hpp"
#include "feature_types.hpp"
#include "result.hpp"

namespace aeg::feature {

/**
 * @brief Snapshot metadata for immutable state capture.
 *
 * Snapshots represent point-in-time state that can be safely shared
 * across Clock domains without synchronization.
 */
struct SnapshotMetadata final {
    /** Version number of the captured state. */
    StateVersion version{};

    /** Clock tick when snapshot was captured. */
    uint64_t capture_tick{0};

    /** Generation of the source instance (invalidates old snapshots on hot-reload). */
    RuntimeGeneration generation{};
};

/**
 * @brief Event metadata for fire-and-forget notifications.
 *
 * Events are delivered best-effort with sequence numbers for ordering
 * but no delivery guarantees. Duplicate detection via sequence numbers.
 */
struct EventMetadata final {
    /** Monotonic sequence number for ordering and deduplication. */
    uint64_t sequence{0};

    /** Clock tick when event was emitted. */
    uint64_t emit_tick{0};

    /** Causality token for distributed event ordering (0 = no causality tracking). */
    uint64_t causality_token{0};
};

/**
 * @brief Command metadata for request-response transactions.
 *
 * Commands expect a Receipt response with matching transaction ID.
 */
struct CommandMetadata final {
    /** Unique transaction ID for matching with Receipt. */
    uint64_t transaction_id{0};

    /** Timeout in clock ticks (0 = no timeout). */
    uint32_t timeout_ticks{0};

    /** Clock tick when command was issued. */
    uint64_t issue_tick{0};

    /** Endpoint identities used to reject receipts from unrelated routes. */
    FeatureInstanceId source_instance{};
    FeatureInstanceId target_instance{};
    PortId source_port{}, target_port{};
    TransportTiming timing{};
};

/**
 * @brief Receipt metadata for command responses.
 *
 * Receipts complete Command transactions and carry success/failure status.
 */
struct ReceiptMetadata final {
    /** Transaction ID matching the originating Command. */
    uint64_t transaction_id{0};

    /** Success status of the command execution. */
    bool success{false};

    /** Error code if success is false (0 = no error). */
    uint32_t error_code{0};
};

/**
 * @brief Queue policy for buffered message streams.
 *
 * Defines how Queue edges handle overflow, ordering, and backpressure.
 */
struct QueuePolicy final {
    /** Maximum queue depth (0 = unbounded, may cause memory exhaustion). */
    uint32_t capacity{0};

    /** Drop oldest message on overflow if true, reject new message if false. */
    bool drop_oldest_on_overflow{false};

    /** Merge duplicate messages based on payload hash if true. */
    bool merge_duplicates{false};

    /** Enforce FIFO ordering if true, allow reordering if false. */
    bool enforce_fifo{true};
};

/**
 * @brief ResourceBus transport manager for Feature edges.
 *
 * Implements EdgeKind-specific semantics on top of PortRouter:
 * - Validates transport policy constraints
 * - Attaches appropriate metadata to messages
 * - Enforces ordering and delivery guarantees
 * - Manages Command/Receipt transaction matching
 */
class ResourceBus final {
public:
    /** Register one compiled graph route as the canonical transport boundary. */
    [[nodiscard]] foundation::Result register_route(FeatureInstanceId source,
                                                    const PortRoute& route,
                                                    const QueuePolicy* queue_policy = nullptr);

    /**
     * @brief Register a Data edge (direct value transfer).
     *
     * Data edges provide immediate value transfer with backpressure.
     * Target receives the latest value; intermediate values may be dropped.
     */
    [[nodiscard]] foundation::Result register_data_edge(FeatureInstanceId source,
                                                        FeatureInstanceId target,
                                                        PortId source_port,
                                                        PortId target_port,
                                                        uint32_t capacity);

    /**
     * @brief Register a Snapshot edge (immutable state capture).
     *
     * Snapshot edges capture point-in-time state with version metadata.
     * Safe for cross-Clock domain sharing without synchronization.
     */
    [[nodiscard]] foundation::Result register_snapshot_edge(FeatureInstanceId source,
                                                            FeatureInstanceId target,
                                                            PortId source_port,
                                                            PortId target_port,
                                                            uint32_t max_snapshot_age_ticks,
                                                            uint32_t source_frequency_hz);

    /**
     * @brief Register a Queue edge (buffered message stream).
     *
     * Queue edges buffer messages with configurable capacity and overflow policy.
     * Provides ordering guarantees and backpressure control.
     */
    [[nodiscard]] foundation::Result register_queue_edge(FeatureInstanceId source,
                                                         FeatureInstanceId target,
                                                         PortId source_port,
                                                         PortId target_port,
                                                         const QueuePolicy& policy);

    /**
     * @brief Register an Event edge (fire-and-forget notification).
     *
     * Event edges deliver notifications best-effort with sequence numbers.
     * Supports deduplication and causality tracking.
     */
    [[nodiscard]] foundation::Result register_event_edge(FeatureInstanceId source,
                                                         FeatureInstanceId target,
                                                         PortId source_port,
                                                         PortId target_port,
                                                         bool enable_causality_tracking);

    /**
     * @brief Register a Command edge (request message).
     *
     * Command edges send requests that expect Receipt responses.
     * Supports timeout and transaction tracking.
     */
    [[nodiscard]] foundation::Result register_command_edge(FeatureInstanceId source,
                                                           FeatureInstanceId target,
                                                           PortId source_port,
                                                           PortId target_port,
                                                           uint32_t timeout_ticks,
                                                           uint32_t source_frequency_hz);

    /**
     * @brief Register a Receipt edge (response message).
     *
     * Receipt edges carry responses to Command transactions.
     * Must reference a registered Command edge.
     */
    [[nodiscard]] foundation::Result register_receipt_edge(FeatureInstanceId source,
                                                           FeatureInstanceId target,
                                                           PortId source_port,
                                                           PortId target_port);

    /**
     * @brief Send a message through the ResourceBus.
     *
     * Attaches appropriate metadata based on EdgeKind and routes to targets.
     *
     * @param source_instance Source feature instance
     * @param source_port Output port producing the message
     * @param payload Message payload bytes
     * @param now Current clock tick for timestamping
     * @param pending_queues Output: messages queued for target instances
     * @return Success or transport policy violation error
     */
    [[nodiscard]] foundation::Result send_message(
        FeatureInstanceId source_instance,
        PortId source_port,
        std::vector<uint8_t> payload,
        TransportTimePoint now,
        std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                           StableIdHash<FeatureInstanceIdTag>>& pending_queues,
        uint64_t transaction_id = 0,
        std::optional<PortId> target_port = std::nullopt);

    /**
     * Route a fully formed output envelope without discarding its metadata.
     *
     * Feature execution uses this overload so schema identifiers, schema
     * versions, timestamps, sequence numbers, and transaction IDs remain
     * producer-owned values and are validated by the compiled route.
     */
    [[nodiscard]] foundation::Result send_message(
        FeatureInstanceId source_instance, PortMessage message, TransportTimePoint now,
        std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                           StableIdHash<FeatureInstanceIdTag>>& pending_queues);

    /** Send a Receipt while explicitly supplying the Command transaction ID. */
    [[nodiscard]] foundation::Result send_receipt(
        FeatureInstanceId source_instance,
        PortId source_port,
        uint64_t transaction_id,
        std::vector<uint8_t> payload,
        TransportTimePoint now,
        std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                           StableIdHash<FeatureInstanceIdTag>>& pending_queues,
        std::optional<PortId> target_port = std::nullopt);

    /** Send a fully formed Receipt envelope while preserving its metadata. */
    [[nodiscard]] foundation::Result send_receipt(
        FeatureInstanceId source_instance, PortMessage message, TransportTimePoint now,
        std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                           StableIdHash<FeatureInstanceIdTag>>& pending_queues);

    /** Route a fully formed message while retaining the bus as the ownership boundary. */
    [[nodiscard]] foundation::Result route_message(
        FeatureInstanceId source_instance, const PortMessage& message,
        std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                           StableIdHash<FeatureInstanceIdTag>>& pending_queues,
        TransportTimePoint now);

    /**
     * @brief Access the underlying PortRouter for capacity checks.
     */
    [[nodiscard]] const PortRouter& router() const { return router_; }
    [[nodiscard]] PortRouter& router() { return router_; }

private:
    [[nodiscard]] foundation::Result send_message_impl(
        FeatureInstanceId source_instance, PortMessage message, TransportTimePoint now,
        std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                           StableIdHash<FeatureInstanceIdTag>>& pending_queues);

    /** Port-level routing infrastructure. */
    PortRouter router_{};

    /** Next sequence number for Event edges (per source instance). */
    std::unordered_map<FeatureInstanceId, uint64_t, StableIdHash<FeatureInstanceIdTag>> event_sequences_{};

    /** Next sequence number for Queue edges (kept independent from Events). */
    std::unordered_map<FeatureInstanceId, uint64_t, StableIdHash<FeatureInstanceIdTag>> queue_sequences_{};

    /** Next transaction ID. IDs are bus-wide so independent sources cannot collide. */
    uint64_t next_transaction_id_{0};
    std::optional<TransportTimePoint> last_now_{};

    /** Pending Command transactions awaiting Receipt (transaction_id → CommandMetadata). */
    std::unordered_map<uint64_t, CommandMetadata> pending_commands_{};

    /** Queue policies indexed by route (source_instance + source_port + target_instance). */
    struct RouteKey {
        FeatureInstanceId source;
        PortId source_port;
        FeatureInstanceId target;
        PortId target_port;

        bool operator==(const RouteKey&) const = default;
    };
    struct RouteKeyHash {
        std::size_t operator()(const RouteKey& key) const noexcept {
            std::size_t h1 = std::hash<uint64_t>{}(key.source.value());
            std::size_t h2 = std::hash<uint64_t>{}(key.source_port.value());
            std::size_t h3 = std::hash<uint64_t>{}(key.target.value());
            std::size_t h4 = std::hash<uint64_t>{}(key.target_port.value());
            return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
        }
    };
    std::unordered_map<RouteKey, QueuePolicy, RouteKeyHash> queue_policies_{};

    /** Event routes that opt into causal sequence/deduplication semantics. */
    std::unordered_map<RouteKey, bool, RouteKeyHash> event_causality_{};

    /** Last accepted causal token for each causality-enabled event route. */
    std::unordered_map<RouteKey, uint64_t, RouteKeyHash> event_causality_tokens_{};

    /** Highest accepted timestamp for each Snapshot route. */
    std::unordered_map<RouteKey, uint64_t, RouteKeyHash> snapshot_timestamps_{};
};

}  // namespace aeg::feature
