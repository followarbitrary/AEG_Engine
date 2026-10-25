/**
 * @file feature_port_router.cpp
 * @brief Implementation of type-safe port-based routing.
 */
#include "feature_port_router.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace aeg::feature {

namespace {

foundation::Result invalid_argument(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
}

foundation::Result invalid_state(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidState, std::move(message));
}

/**
 * @brief Check if two schema IDs are compatible for routing.
 *
 * Currently exact match required. Future: support schema evolution/coercion.
 */
bool schemas_compatible(ValueSchemaId source, ValueSchemaId target) {
    return source == target;
}

/**
 * @brief Check if EdgeKind is appropriate for the given port types.
 *
 * Data edges: direct value transfer
 * Snapshot edges: immutable state capture
 * Queue edges: buffered message stream
 * Event edges: fire-and-forget notifications
 * Command/Receipt edges: request-response patterns
 */
bool edge_kind_valid_for_ports(EdgeKind kind, const PortContract& source, const PortContract& target) {
    (void)source;
    (void)target;
    // All edge kinds currently supported for all port types
    // Future: add semantic restrictions (e.g., Command must have Receipt response)
    return kind == EdgeKind::kData ||
           kind == EdgeKind::kSnapshot ||
           kind == EdgeKind::kQueue ||
           kind == EdgeKind::kEvent ||
           kind == EdgeKind::kCommand ||
           kind == EdgeKind::kReceipt;
}

}  // namespace

foundation::Result validate_port_route_compatibility(const PortContract& source_port,
                                                     const PortContract& target_port,
                                                     EdgeKind edge_kind) {
    // Validate port directions
    if (source_port.direction != PortDirection::kOutput) {
        return invalid_argument("source port must be an output port");
    }
    if (target_port.direction != PortDirection::kInput) {
        return invalid_argument("target port must be an input port");
    }

    // Validate schema compatibility
    if (!schemas_compatible(source_port.schema, target_port.schema)) {
        return invalid_argument("source and target port schemas are incompatible");
    }

    // Validate EdgeKind is appropriate for port types
    if (!edge_kind_valid_for_ports(edge_kind, source_port, target_port)) {
        return invalid_argument("edge kind is not valid for the given port types");
    }

    return foundation::Result::success();
}

foundation::Result PortRouter::register_route(FeatureInstanceId source_instance, const PortRoute& route) {
    // Validate route configuration
    if (!source_instance.is_valid()) {
        return invalid_argument("source instance ID is invalid");
    }
    if (!route.target_instance.is_valid()) {
        return invalid_argument("target instance ID is invalid");
    }
    if (!route.source_port.is_valid()) {
        return invalid_argument("source port ID is invalid");
    }
    if (!route.target_port.is_valid()) {
        return invalid_argument("target port ID is invalid");
    }
    if (route.source_schema.is_valid() && route.target_schema.is_valid() &&
        route.source_schema != route.target_schema) {
        return invalid_argument("source and target route schemas are incompatible");
    }
    if (route.deadline_ticks != 0 && route.source_frequency_hz == 0)
        return invalid_argument("deadline route requires the author source Clock frequency");
    if (route.kind == EdgeKind::kSnapshot && route.deadline_ticks == 0)
        return invalid_argument("Snapshot route requires a positive source-period deadline");

    // Check for duplicate routes
    auto& routes = routes_[source_instance];
    const auto duplicate = std::find_if(routes.begin(), routes.end(), [&](const PortRoute& existing) {
        return existing.target_instance == route.target_instance &&
               existing.source_port == route.source_port &&
               existing.target_port == route.target_port &&
               existing.kind == route.kind;
    });

    if (duplicate != routes.end()) {
        return invalid_state("route already registered");
    }

    // Register the route
    routes.push_back(route);
    return foundation::Result::success();
}

void PortRouter::unregister_routes(FeatureInstanceId source_instance) {
    routes_.erase(source_instance);
}

foundation::Result PortRouter::route_message(
    FeatureInstanceId source_instance,
    const PortMessage& message,
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>>& pending_queues,
    TransportTimePoint now,
    FeatureInstanceId target_instance) {
    if (!now.domain.is_valid() || (last_now_ && (last_now_->domain != now.domain ||
        now.nanoseconds < last_now_->nanoseconds)))
        return invalid_argument("router time is missing, backwards or belongs to another domain");

    const auto routes_it = routes_.find(source_instance);
    if (routes_it == routes_.end()) {
        // No routes registered - message goes nowhere (valid for optional outputs)
        return foundation::Result::success();
    }

    // Find routes matching the source port. A valid target_port on the input
    // is an optional selector used by ResourceBus to process one fan-out leg
    // at a time while preserving per-route metadata.
    std::vector<const PortRoute*> matching_routes;
    for (const auto& route : routes_it->second) {
        if (route.source_port == message.source_port &&
            (!message.target_port.is_valid() || route.target_port == message.target_port) &&
            (!target_instance.is_valid() || route.target_instance == target_instance)) {
            matching_routes.push_back(&route);
        }
    }

    if (matching_routes.empty()) {
        // No routes for this specific port - valid if port is optional
        return foundation::Result::success();
    }

    // Build all deliveries off to the side. Validation and capacity checks must
    // complete before touching the caller's queues (atomic fan-out).
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>> staged;
    std::unordered_map<FeatureInstanceId, uint32_t,
                       StableIdHash<FeatureInstanceIdTag>> capacities;

    auto last_delivery_id = last_delivery_id_;
    for (const PortRoute* route : matching_routes) {
        // Validate schema version compatibility
        if (route->target_schema_version != 0 && message.schema_version != route->target_schema_version) {
            return invalid_state("message schema version does not match target port schema version");
        }
        if (route->source_schema.is_valid() && message.schema != route->source_schema) {
            return invalid_state("message schema does not match source port schema");
        }
        if (route->target_schema.is_valid() && message.schema != route->target_schema) {
            return invalid_state("message schema does not match target port schema");
        }

        // Create routed message with target port information
        PortMessage routed_message = message;
        routed_message.source.instance = source_instance;
        routed_message.target_port = route->target_port;
        routed_message.kind = route->kind;
        routed_message.deadline_ticks = route->deadline_ticks;

        if (!routed_message.source.timing) routed_message.source.timing = TransportTiming{now, {}};
        if (route->deadline_ticks != 0 && route->kind != EdgeKind::kReceipt) {
            const auto duration = source_period_duration(route->deadline_ticks, route->source_frequency_hz);
            if (!duration) return duration.result;
            const auto expiry = transport_add(routed_message.source.timing->origin, duration.value);
            if (!expiry) return expiry.result;
            routed_message.source.timing->expires_at = expiry.value;
        }
        if (const auto valid = validate_transport_timing(*routed_message.source.timing, now); !valid) {
            if (valid.code == foundation::ResultCode::kTimeout) continue;
            return valid;
        }

        if (last_delivery_id == (std::numeric_limits<uint64_t>::max)())
            return invalid_state("message delivery identity space exhausted");
        routed_message.delivery_id = MessageDeliveryId(++last_delivery_id);
        staged[route->target_instance].push_back(std::move(routed_message));
        if (route->capacity != 0) {
            auto& capacity = capacities[route->target_instance];
            capacity = capacity == 0 ? route->capacity : std::min(capacity, route->capacity);
        }
    }

    for (const auto& [target, deliveries] : staged) {
        const auto capacity_it = capacities.find(target);
        if (capacity_it == capacities.end()) {
            continue;
        }
        const auto current_it = pending_queues.find(target);
        const size_t current_depth = current_it == pending_queues.end() ? 0 : current_it->second.size();
        if (current_depth + deliveries.size() > capacity_it->second) {
            return invalid_state("target queue capacity exhausted");
        }
    }

    for (auto& [target, deliveries] : staged) {
        auto& destination = pending_queues[target];
        destination.insert(destination.end(),
                           std::make_move_iterator(deliveries.begin()),
                           std::make_move_iterator(deliveries.end()));
    }

    last_delivery_id_ = last_delivery_id;
    last_now_ = now;
    return foundation::Result::success();
}

foundation::Result PortRouter::check_capacity(
    FeatureInstanceId source_instance,
    uint32_t message_count,
    const std::unordered_map<FeatureInstanceId, uint32_t,
                             StableIdHash<FeatureInstanceIdTag>>& current_queue_depths) const {

    const auto routes_it = routes_.find(source_instance);
    if (routes_it == routes_.end()) {
        return foundation::Result::success();
    }

    // Group routes by target to check capacity once per target
    std::unordered_map<FeatureInstanceId, uint32_t, StableIdHash<FeatureInstanceIdTag>> min_capacity_per_target;

    for (const auto& route : routes_it->second) {
        if (route.capacity == 0) {
            // Unbounded capacity - skip check
            continue;
        }

        auto& min_capacity = min_capacity_per_target[route.target_instance];
        if (min_capacity == 0 || route.capacity < min_capacity) {
            min_capacity = route.capacity;
        }
    }

    // Check capacity for each target
    for (const auto& [target_id, min_capacity] : min_capacity_per_target) {
        const uint32_t current_depth = [&]() {
            const auto it = current_queue_depths.find(target_id);
            return it != current_queue_depths.end() ? it->second : 0;
        }();

        const uint32_t available = min_capacity > current_depth ? min_capacity - current_depth : 0;

        if (message_count > available) {
            return invalid_state("target queue capacity exhausted");
        }
    }

    return foundation::Result::success();
}

std::span<const PortRoute> PortRouter::get_routes(FeatureInstanceId source_instance) const {
    const auto it = routes_.find(source_instance);
    if (it == routes_.end()) {
        return {};
    }
    return it->second;
}

}  // namespace aeg::feature
