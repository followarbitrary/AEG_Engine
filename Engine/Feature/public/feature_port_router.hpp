/**
 * @file feature_port_router.hpp
 * @brief Type-safe port-based routing for Feature graph edges.
 *
 * Replaces opaque payload broadcasting with schema-validated port-to-port routing.
 * Each edge connects a source output port to a target input port with compatible schemas.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "feature_types.hpp"
#include "feature_time.hpp"
#include "result.hpp"

namespace aeg::feature {
namespace detail { struct RenderLightListEvidence; }

/**
 * @brief Single-edge routing entry mapping source port to target port.
 *
 * Validates schema compatibility at registration time.
 */
struct PortRoute final {
    /** Target instance receiving the data. */
    FeatureInstanceId target_instance{};

    /** Output port on source instance. */
    PortId source_port{};

    /** Input port on target instance. */
    PortId target_port{};

    /** Edge transport policy (Data/Snapshot/Queue/Event/Command/Receipt). */
    EdgeKind kind{EdgeKind::kData};

    /** Maximum queued messages (0 = unbounded). */
    uint32_t capacity{0};

    /** Deadline in clock ticks (0 = no deadline). */
    uint32_t deadline_ticks{0};

    /** Schema version expected by target port. */
    uint32_t target_schema_version{0};

    /** Schema identifier produced by the source port (invalid means unchecked). */
    ValueSchemaId source_schema{};

    /** Schema identifier expected by target port (invalid means unchecked). */
    ValueSchemaId target_schema{};

    /** Event causal sequence tracking is enabled by default for graph routes. */
    bool event_causality_tracking{true};
    /** Author edge's source Clock frequency; Composite projection preserves this unit. */
    uint32_t source_frequency_hz{0};
};

/**
 * @brief Message envelope carrying port metadata with payload.
 *
 * Replaces opaque byte vectors with structured messages that preserve
 * source/target port information for validation and routing.
 */
/** Runtime-stamped source of a committed delivery; author values are overwritten. */
struct MessageSource final {
    FeatureInstanceId instance{};
    InvocationToken invocation{};
    uint64_t definition_digest{0};
    uint64_t resource_epoch{0};
    ClockId clock{};
    /** Host provenance only; excluded from author callback codecs. */
    std::optional<TransportTiming> timing{};
    friend bool operator==(const MessageSource&, const MessageSource&) = default;
};

struct PortMessage final {
    /** Source port that produced this message. */
    PortId source_port{};

    /** Target port that should receive this message. */
    PortId target_port{};

    /** Schema version of the payload. */
    uint32_t schema_version{0};

    /** Sequence number for ordering (Event/Queue edges). */
    uint64_t sequence{0};

    /**
     * Causality token for Event delivery when the route enables causal
     * tracking.  The token is stable for a replayed sequence and therefore
     * can be used to suppress duplicate notifications at the target.
     */
    uint64_t causality_token{0};

    /** Clock tick when message was produced. */
    uint64_t timestamp_ticks{0};

    /** Serialized payload data. */
    std::vector<uint8_t> payload{};

    /** Schema identifier of the payload (invalid means unchecked). */
    ValueSchemaId schema{};

    /** Edge transport policy selected for this delivery. */
    EdgeKind kind{EdgeKind::kData};

    /** Command/Receipt correlation identifier (zero for other edges). */
    uint64_t transaction_id{0};

    /** Deadline origin-relative limit copied from the route. */
    uint32_t deadline_ticks{0};
    /** Runtime-local identity assigned by the router, never by the producer. */
    MessageDeliveryId delivery_id{};
    MessageSource source{};
    /** Opaque host-owned declaration; Runtime overwrites author-provided evidence. */
    std::shared_ptr<const detail::RenderLightListEvidence> render_source{};
};

/**
 * @brief Validates port route compatibility at registration time.
 *
 * Checks:
 * - Port directions match (output → input)
 * - Schema IDs are compatible
 * - EdgeKind is appropriate for port types
 *
 * @param source_port Source port contract
 * @param target_port Target port contract
 * @param edge_kind Requested edge transport policy
 * @return Success if compatible, error with diagnostic message otherwise
 */
[[nodiscard]] foundation::Result validate_port_route_compatibility(const PortContract& source_port,
                                                                   const PortContract& target_port,
                                                                   EdgeKind edge_kind);

/**
 * @brief Port-based message router for Feature instances.
 *
 * Replaces the simple routes_ map in FeatureRuntime with schema-aware routing
 * that validates types and enforces capacity/deadline constraints.
 */
class PortRouter final {
public:
    /**
     * @brief Register a port-to-port route.
     *
     * @param source_instance Source feature instance
     * @param route Route configuration with port IDs and transport policy
     * @return Success or validation error
     */
    [[nodiscard]] foundation::Result register_route(FeatureInstanceId source_instance, const PortRoute& route);

    /**
     * @brief Remove all routes from a source instance.
     *
     * Used during instance deactivation.
     */
    void unregister_routes(FeatureInstanceId source_instance);

    /**
     * @brief Route a message from source to targets.
     *
     * Validates schema versions, enforces capacity limits, and queues messages
     * at target instances according to EdgeKind policy.
     *
     * @param source_instance Source feature instance
     * @param message Message with port metadata and payload
     * @param pending_queues Output: target queues to append to
     * @param current_tick Current transport clock for deadline checks
     * @param target_instance Optional exact route target selected by ResourceBus
     * @return Success if all routes accept the message, capacity error otherwise
     */
    [[nodiscard]] foundation::Result route_message(
        FeatureInstanceId source_instance,
        const PortMessage& message,
        std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                           StableIdHash<FeatureInstanceIdTag>>& pending_queues,
        TransportTimePoint now,
        FeatureInstanceId target_instance = {});

    /**
     * @brief Check capacity before committing state.
     *
     * Pre-validates that all target queues have space for the message.
     * Prevents partial commits where some targets accept and others reject.
     *
     * @param source_instance Source feature instance
     * @param message_count Number of messages being sent
     * @param current_queue_depths Current depth of each target queue
     * @return Success if capacity available, error with diagnostic otherwise
     */
    [[nodiscard]] foundation::Result check_capacity(
        FeatureInstanceId source_instance,
        uint32_t message_count,
        const std::unordered_map<FeatureInstanceId, uint32_t,
                                 StableIdHash<FeatureInstanceIdTag>>& current_queue_depths) const;

    /**
     * @brief Get all routes from a source instance.
     *
     * Used for introspection and diagnostics.
     */
    [[nodiscard]] std::span<const PortRoute> get_routes(FeatureInstanceId source_instance) const;

private:
    /** Routes indexed by source instance. Each source can have multiple output routes. */
    std::unordered_map<FeatureInstanceId, std::vector<PortRoute>,
                       StableIdHash<FeatureInstanceIdTag>> routes_{};
    uint64_t last_delivery_id_{0};
    std::optional<TransportTimePoint> last_now_{};
};

}  // namespace aeg::feature
