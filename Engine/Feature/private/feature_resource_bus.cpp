/**
 * @file feature_resource_bus.cpp
 * @brief Implementation of ResourceBus EdgeKind transport semantics.
 */
#include "feature_resource_bus.hpp"

#include <algorithm>
#include <exception>
#include <new>

namespace aeg::feature {

namespace {

foundation::Result invalid_argument(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
}

foundation::Result invalid_state(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidState, std::move(message));
}

}  // namespace

foundation::Result ResourceBus::register_route(FeatureInstanceId source, const PortRoute& route,
                                               const QueuePolicy* queue_policy) {
    PortRoute normalized = route;
    // Snapshot is a latest-value transport even when the graph edge omits an
    // explicit capacity.  Normalize once at the transport boundary so all
    // callers (including compiled graph publication) share that policy.
    if ((normalized.kind == EdgeKind::kSnapshot || normalized.kind == EdgeKind::kCommand) && normalized.capacity == 0)
        normalized.capacity = 1;
    if (const auto result = router_.register_route(source, normalized); !result) return result;
    if (normalized.kind == EdgeKind::kQueue) {
        QueuePolicy policy{};
        policy.capacity = normalized.capacity;
        if (queue_policy != nullptr) policy = *queue_policy;
        queue_policies_[RouteKey{source, normalized.source_port, normalized.target_instance,
                                 normalized.target_port}] = policy;
    }
    if (normalized.kind == EdgeKind::kEvent) {
        event_causality_[RouteKey{source, normalized.source_port, normalized.target_instance,
                                  normalized.target_port}] = normalized.event_causality_tracking;
    }
    return foundation::Result::success();
}

foundation::Result ResourceBus::register_data_edge(FeatureInstanceId source,
                                                   FeatureInstanceId target,
                                                   PortId source_port,
                                                   PortId target_port,
                                                   uint32_t capacity) {
    PortRoute route{};
    route.target_instance = target;
    route.source_port = source_port;
    route.target_port = target_port;
    route.kind = EdgeKind::kData;
    route.capacity = capacity;

    return register_route(source, route);
}

foundation::Result ResourceBus::register_snapshot_edge(FeatureInstanceId source,
                                                       FeatureInstanceId target,
                                                       PortId source_port,
                                                       PortId target_port,
                                                       uint32_t max_snapshot_age_ticks,
                                                       uint32_t source_frequency_hz) {
    PortRoute route{};
    route.target_instance = target;
    route.source_port = source_port;
    route.target_port = target_port;
    route.kind = EdgeKind::kSnapshot;
    route.deadline_ticks = max_snapshot_age_ticks;
    route.source_frequency_hz = source_frequency_hz;
    route.capacity = 1;  // Snapshots only keep latest version

    return register_route(source, route);
}

foundation::Result ResourceBus::register_queue_edge(FeatureInstanceId source,
                                                    FeatureInstanceId target,
                                                    PortId source_port,
                                                    PortId target_port,
                                                    const QueuePolicy& policy) {
    PortRoute route{};
    route.target_instance = target;
    route.source_port = source_port;
    route.target_port = target_port;
    route.kind = EdgeKind::kQueue;
    route.capacity = policy.capacity;

    // Store queue policy for overflow handling
    RouteKey key{source, source_port, target, target_port};
    queue_policies_[key] = policy;

    return register_route(source, route, &policy);
}

foundation::Result ResourceBus::register_event_edge(FeatureInstanceId source,
                                                    FeatureInstanceId target,
                                                    PortId source_port,
                                                    PortId target_port,
                                                    bool enable_causality_tracking) {
    PortRoute route{};
    route.target_instance = target;
    route.source_port = source_port;
    route.target_port = target_port;
    route.kind = EdgeKind::kEvent;
    route.capacity = 0;  // Events are unbounded by default
    route.event_causality_tracking = enable_causality_tracking;

    // Initialize event sequence counter
    if (event_sequences_.find(source) == event_sequences_.end()) {
        event_sequences_[source] = 0;
    }

    if (const auto result = register_route(source, route); !result) return result;
    return foundation::Result::success();
}

foundation::Result ResourceBus::register_command_edge(FeatureInstanceId source,
                                                      FeatureInstanceId target,
                                                      PortId source_port,
                                                      PortId target_port,
                                                      uint32_t timeout_ticks,
                                                      uint32_t source_frequency_hz) {
    PortRoute route{};
    route.target_instance = target;
    route.source_port = source_port;
    route.target_port = target_port;
    route.kind = EdgeKind::kCommand;
    route.deadline_ticks = timeout_ticks;
    route.source_frequency_hz = source_frequency_hz;
    route.capacity = 1;  // Commands typically one-at-a-time

    return register_route(source, route);
}

foundation::Result ResourceBus::register_receipt_edge(FeatureInstanceId source,
                                                      FeatureInstanceId target,
                                                      PortId source_port,
                                                      PortId target_port) {
    PortRoute route{};
    route.target_instance = target;
    route.source_port = source_port;
    route.target_port = target_port;
    route.kind = EdgeKind::kReceipt;
    route.capacity = 0;  // Receipts are unbounded

    return register_route(source, route);
}

foundation::Result ResourceBus::send_message(
    FeatureInstanceId source_instance,
    PortId source_port,
    std::vector<uint8_t> payload,
    TransportTimePoint now,
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>>& pending_queues,
    uint64_t transaction_id, std::optional<PortId> target_port) {
    PortMessage message{};
    message.source_port = source_port;
    message.payload = std::move(payload);
    message.timestamp_ticks = now.nanoseconds;
    message.transaction_id = transaction_id;
    if (target_port.has_value()) message.target_port = *target_port;
    return send_message(source_instance, std::move(message), now, pending_queues);
}

foundation::Result ResourceBus::send_message(
    FeatureInstanceId source_instance, PortMessage message, TransportTimePoint now,
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>>& pending_queues) {
    try {
        // Keep all bus bookkeeping transactional. A failed schema, deadline,
        // capacity, or Receipt validation must not consume sequence numbers,
        // command transactions, or Snapshot history.  The staging copies are
        // part of that transaction too: allocation failure must be reported at
        // this Result boundary while leaving the live bus and queues untouched.
        const bool is_receipt = message.kind == EdgeKind::kReceipt;
        const uint64_t transaction_id = message.transaction_id;
        auto staged_bus = *this;
        auto staged_queues = pending_queues;
        if (const auto result = staged_bus.send_message_impl(source_instance, std::move(message),
                                                             now, staged_queues);
            !result) {
            // The staged copy may have retired an expired command before
            // rejecting its Receipt. Preserve that retirement on the live bus
            // while keeping every other failed transaction mutation atomic.
            if (is_receipt && result.code == foundation::ResultCode::kTimeout)
                pending_commands_.erase(transaction_id);
            return result;
        }
        pending_queues.swap(staged_queues);
        *this = std::move(staged_bus);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ResourceBus transaction allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::Result ResourceBus::send_message_impl(
    FeatureInstanceId source_instance, PortMessage message, TransportTimePoint now,
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>>& pending_queues) {
    if (!now.domain.is_valid() || (last_now_ && (last_now_->domain != now.domain ||
        now.nanoseconds < last_now_->nanoseconds)))
        return invalid_argument("Bus time is missing, backwards or belongs to another domain");
    if (!message.source.timing) message.source.timing = TransportTiming{now, {}};
    if (const auto valid = validate_transport_timing(*message.source.timing, now); !valid) return valid;
    const auto existing_command = pending_commands_.find(message.transaction_id);
    const auto registered = router_.get_routes(source_instance);
    const bool receipt_route = std::any_of(registered.begin(), registered.end(), [&](const PortRoute& route) {
        return route.kind == EdgeKind::kReceipt && route.source_port == message.source_port &&
            (!message.target_port.is_valid() || route.target_port == message.target_port);
    });
    if (receipt_route && existing_command != pending_commands_.end()) {
        if (const auto valid = validate_transport_timing(existing_command->second.timing, now); !valid) {
            // Retire an expired command at the same boundary that rejects its
            // stale Receipt. This preserves the timeout result expected by
            // callers while preventing expired transactions from accumulating
            // as unbounded tombstones.
            if (valid.code == foundation::ResultCode::kTimeout)
                pending_commands_.erase(existing_command);
            return valid;
        }
    }
    for (auto it = pending_commands_.begin(); it != pending_commands_.end();) {
        const auto valid = validate_transport_timing(it->second.timing, now);
        if (!valid && valid.code == foundation::ResultCode::kTimeout) it = pending_commands_.erase(it);
        else if (!valid) return valid;
        else ++it;
    }
    const auto routes = router_.get_routes(source_instance);
    std::vector<const PortRoute*> matching_routes;
    for (const auto& route : routes) {
        if (route.source_port == message.source_port &&
            (!message.target_port.is_valid() || route.target_port == message.target_port)) {
            matching_routes.push_back(&route);
        }
    }
    if (matching_routes.empty()) return foundation::Result::success();

    const auto event_it = event_sequences_.find(source_instance);
    uint64_t next_event_sequence = event_it == event_sequences_.end() ? 0 : event_it->second;
    const auto queue_it = queue_sequences_.find(source_instance);
    uint64_t next_queue_sequence = queue_it == queue_sequences_.end() ? 0 : queue_it->second;
    const uint64_t next_command_transaction = next_transaction_id_ + 1;
    uint64_t command_transaction = message.transaction_id;
    std::vector<uint64_t> completed_transactions;
    std::vector<CommandMetadata> new_commands;

    for (const PortRoute* route : matching_routes) {
        PortMessage routed_message = message;
        routed_message.target_port = route->target_port;
        routed_message.kind = route->kind;
        routed_message.deadline_ticks = route->deadline_ticks;
        if (route->deadline_ticks != 0 && route->kind != EdgeKind::kReceipt) {
            const auto duration = source_period_duration(route->deadline_ticks, route->source_frequency_hz);
            if (!duration) return duration.result;
            const auto expiry = transport_add(routed_message.source.timing->origin, duration.value);
            if (!expiry) return expiry.result;
            routed_message.source.timing->expires_at = expiry.value;
            if (const auto valid = validate_transport_timing(*routed_message.source.timing, now); !valid) return valid;
        }
        // An omitted schema/version can be filled from the compiled route for
        // legacy callers. Explicit values are retained and validated below.
        if (!routed_message.schema.is_valid()) routed_message.schema = route->source_schema;
        if (routed_message.schema_version == 0)
            routed_message.schema_version = route->target_schema_version;

        switch (route->kind) {
            case EdgeKind::kData:
                routed_message.sequence = 0;
                break;
            case EdgeKind::kSnapshot: {
                if (routed_message.sequence == 0) routed_message.sequence = routed_message.timestamp_ticks;
                const RouteKey key{source_instance, route->source_port, route->target_instance,
                                   route->target_port};
                const auto previous = snapshot_timestamps_.find(key);
                if (previous != snapshot_timestamps_.end() &&
                    routed_message.timestamp_ticks < previous->second) {
                    return invalid_state("snapshot timestamp regressed");
                }
                break;
            }
            case EdgeKind::kQueue: {
                if (routed_message.sequence == 0) routed_message.sequence = ++next_queue_sequence;
                else next_queue_sequence = std::max(next_queue_sequence, routed_message.sequence);
                const RouteKey key{source_instance, route->source_port, route->target_instance,
                                   route->target_port};
                const auto policy_it = queue_policies_.find(key);
                if (policy_it != queue_policies_.end()) {
                    const auto& policy = policy_it->second;
                    auto& target_queue = pending_queues[route->target_instance];
                    if (policy.capacity > 0 && target_queue.size() >= policy.capacity) {
                        if (policy.drop_oldest_on_overflow) target_queue.erase(target_queue.begin());
                        else return invalid_state("queue capacity exhausted");
                    }
                    if (policy.merge_duplicates) {
                        const auto duplicate = std::find_if(
                            target_queue.begin(), target_queue.end(), [&](const PortMessage& existing) {
                                return existing.payload == routed_message.payload;
                            });
                        if (duplicate != target_queue.end()) continue;
                    }
                }
                break;
            }
            case EdgeKind::kEvent:
                if (routed_message.sequence == 0) routed_message.sequence = ++next_event_sequence;
                else next_event_sequence = std::max(next_event_sequence, routed_message.sequence);
                {
                    const RouteKey key{source_instance, route->source_port,
                                       route->target_instance, route->target_port};
                    const auto causal = event_causality_.find(key);
                    if (causal != event_causality_.end() && causal->second) {
                        routed_message.causality_token = routed_message.sequence;
                        const auto previous = event_causality_tokens_.find(key);
                        if (previous != event_causality_tokens_.end() &&
                            routed_message.causality_token <= previous->second) {
                            // A replayed or out-of-order causal sequence has
                            // already been accepted for this route.
                            continue;
                        }
                    }
                }
                break;
            case EdgeKind::kCommand: {
                if (command_transaction == 0 && next_command_transaction == 0)
                    return invalid_state("Command transaction identity space exhausted");
                if (command_transaction == 0) command_transaction = next_command_transaction;
                routed_message.transaction_id = command_transaction;
                if (routed_message.sequence == 0) routed_message.sequence = command_transaction;
                if (pending_commands_.contains(command_transaction))
                    return invalid_state("Command transaction ID is already pending");
                const auto count = std::count_if(pending_commands_.begin(), pending_commands_.end(), [&](const auto& entry) {
                    const auto& existing = entry.second;
                    return existing.source_instance == source_instance && existing.target_instance == route->target_instance &&
                        existing.source_port == route->source_port && existing.target_port == route->target_port;
                });
                if (static_cast<size_t>(count) >= route->capacity)
                    return invalid_state("pending Command route capacity exhausted");
                const CommandMetadata command{command_transaction, route->deadline_ticks,
                                              routed_message.timestamp_ticks, source_instance,
                                              route->target_instance, route->source_port, route->target_port,
                                              *routed_message.source.timing};
                new_commands.push_back(command);
                break;
            }
            case EdgeKind::kReceipt: {
                if (routed_message.transaction_id == 0)
                    return invalid_argument("receipt requires an explicit transaction ID");
                if (routed_message.sequence == 0) routed_message.sequence = routed_message.transaction_id;
                const auto cmd_it = pending_commands_.find(routed_message.transaction_id);
                if (cmd_it == pending_commands_.end())
                    return invalid_state("receipt does not match any pending command");
                if (cmd_it->second.target_instance != source_instance ||
                    cmd_it->second.source_instance != route->target_instance)
                    return invalid_state("receipt endpoint does not match command transaction");
                if (const auto valid = validate_transport_timing(cmd_it->second.timing, now); !valid) return valid;
                routed_message.source.timing->expires_at = cmd_it->second.timing.expires_at;
                completed_transactions.push_back(routed_message.transaction_id);
                break;
            }
        }

        if ((route->kind == EdgeKind::kSnapshot || route->kind == EdgeKind::kData) &&
            route->capacity == 1) {
            auto target_it = pending_queues.find(route->target_instance);
            if (target_it != pending_queues.end()) {
                auto& target_queue = target_it->second;
                target_queue.erase(std::remove_if(target_queue.begin(), target_queue.end(),
                                                  [&](const PortMessage& existing) {
                                                      return existing.kind == route->kind &&
                                                             existing.target_port == route->target_port;
                                                  }),
                                   target_queue.end());
            }
        }
        if (const auto result = router_.route_message(source_instance, routed_message, pending_queues,
                                                      now, route->target_instance);
            !result) {
            return result;
        }
        if (route->kind == EdgeKind::kEvent && routed_message.causality_token != 0) {
            event_causality_tokens_[RouteKey{source_instance, route->source_port,
                                             route->target_instance, route->target_port}] =
                routed_message.causality_token;
        }
        if (route->kind == EdgeKind::kSnapshot) {
            snapshot_timestamps_[RouteKey{source_instance, route->source_port,
                                          route->target_instance, route->target_port}] =
                routed_message.timestamp_ticks;
        }
    }

    event_sequences_[source_instance] = next_event_sequence;
    queue_sequences_[source_instance] = next_queue_sequence;
    if (!new_commands.empty()) {
        next_transaction_id_ = std::max(next_transaction_id_, command_transaction);
        for (const auto& command : new_commands) pending_commands_[command.transaction_id] = command;
    }
    for (const uint64_t completed : completed_transactions) pending_commands_.erase(completed);
    last_now_ = now;
    return foundation::Result::success();
}

foundation::Result ResourceBus::send_receipt(
    FeatureInstanceId source_instance,
    PortId source_port,
    uint64_t transaction_id,
    std::vector<uint8_t> payload,
    TransportTimePoint now,
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>>& pending_queues,
    std::optional<PortId> target_port) {
    PortMessage message{};
    message.source_port = source_port;
    message.kind = EdgeKind::kReceipt;
    message.transaction_id = transaction_id;
    message.timestamp_ticks = now.nanoseconds;
    message.payload = std::move(payload);
    if (target_port.has_value()) message.target_port = *target_port;
    return send_message(source_instance, std::move(message), now, pending_queues);
}

foundation::Result ResourceBus::send_receipt(
    FeatureInstanceId source_instance, PortMessage message, TransportTimePoint now,
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>>& pending_queues) {
    message.kind = EdgeKind::kReceipt;
    return send_message(source_instance, std::move(message), now, pending_queues);
}

foundation::Result ResourceBus::route_message(
    FeatureInstanceId source_instance, const PortMessage& message,
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>>& pending_queues,
    TransportTimePoint now) {
    try {
        return router_.route_message(source_instance, message, pending_queues, now);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ResourceBus route allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

}  // namespace aeg::feature
