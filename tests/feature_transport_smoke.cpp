#include <feature_port_router.hpp>
#include <feature_resource_bus.hpp>
#include <feature_contract_metadata.hpp>

#include <iostream>
#include <cassert>
#include <unordered_map>

using namespace aeg::feature;

// These transport-only fixtures use one nanosecond per authored source period.
constexpr TransportTimePoint at(uint64_t nanoseconds) { return {RuntimeGeneration(1), nanoseconds}; }

using Queues = std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                                  StableIdHash<FeatureInstanceIdTag>>;

void test_router_rejects_expired_message_without_mutation() {
    PortRouter router;
    PortRoute route{};
    route.target_instance = FeatureInstanceId("Target");
    route.source_port = PortId("Out");
    route.target_port = PortId("In");
    route.deadline_ticks = 5;
    route.source_frequency_hz = 1'000'000'000;
    assert(router.register_route(FeatureInstanceId("Source"), route));

    PortMessage message{};
    message.source_port = PortId("Out");
    message.timestamp_ticks = 10;
    message.source.timing = TransportTiming{at(10), {}};
    Queues queues;
    assert(router.route_message(FeatureInstanceId("Source"), message, queues, at(16)));
    assert(queues.empty());
}

void test_router_schema_and_capacity_validation_is_atomic() {
    PortRouter router;
    const auto source = FeatureInstanceId("Source");
    PortRoute first{};
    first.target_instance = FeatureInstanceId("A");
    first.source_port = PortId("Out");
    first.target_port = PortId("InA");
    first.capacity = 2;
    first.target_schema = ValueSchemaId("Data.v1");
    const auto second = PortRoute{FeatureInstanceId("B"), PortId("Out"), PortId("InB"),
                                  EdgeKind::kData, 1, 0, 0};
    assert(router.register_route(source, first));
    assert(router.register_route(source, second));

    Queues queues;
    queues[FeatureInstanceId("B")].push_back(PortMessage{});
    PortMessage message{};
    message.source_port = PortId("Out");
    message.schema = ValueSchemaId("Data.v1");
    assert(!router.route_message(source, message, queues, at(0)));
    assert(queues[FeatureInstanceId("A")].empty());

    queues.clear();
    message.schema = ValueSchemaId("Data.v2");
    assert(!router.route_message(source, message, queues, at(0)));
    assert(queues.empty());
}

void test_resource_bus_command_receipt_and_timeout() {
    ResourceBus bus;
    const auto caller = FeatureInstanceId("Caller");
    const auto worker = FeatureInstanceId("Worker");
    assert(bus.register_command_edge(caller, worker, PortId("Command"), PortId("CommandIn"), 3, 1'000'000'000));
    assert(bus.register_receipt_edge(worker, caller, PortId("Receipt"), PortId("ReceiptIn")));

    Queues queues;
    assert(bus.send_message(caller, PortId("Command"), {1}, at(10), queues));
    const auto transaction = queues[worker].front().transaction_id;
    assert(transaction != 0);
    const auto receipt_result = bus.send_receipt(worker, PortId("Receipt"), transaction, {2}, at(12), queues);
    if (!receipt_result) std::cerr << "receipt failed: " << receipt_result.message << "\n";
    assert(receipt_result);
    assert(queues[caller].back().transaction_id == transaction);
    queues[worker].clear(); // worker consumed the command before the next request

    assert(bus.send_message(caller, PortId("Command"), {3}, at(20), queues));
    const auto expired = queues[worker].back().transaction_id;
    assert(expired != transaction);
    Queues late_receipt;
    const auto late = bus.send_receipt(worker, PortId("Receipt"), expired, {4}, at(24), late_receipt);
    assert(!late && late.code == aeg::foundation::ResultCode::kTimeout);
    Queues repeated_late_receipt;
    const auto repeated = bus.send_receipt(worker, PortId("Receipt"), expired, {4}, at(24), repeated_late_receipt);
    assert(!repeated && repeated.code == aeg::foundation::ResultCode::kInvalidState);

    // Rejection of the stale Receipt must retire the expired transaction.  A
    // route with capacity one must be immediately reusable rather than pinned
    // by an unbounded expired-command tombstone.
    Queues recovered;
    assert(bus.send_message(caller, PortId("Command"), {5}, at(25), recovered));
}

void test_queue_and_event_sequences_are_independent() {
    ResourceBus bus;
    const auto source = FeatureInstanceId("SequenceSource");
    const auto target = FeatureInstanceId("SequenceTarget");
    assert(bus.register_queue_edge(source, target, PortId("Queue"), PortId("QueueIn"), QueuePolicy{}));
    assert(bus.register_event_edge(source, target, PortId("Event"), PortId("EventIn"), false));
    Queues queues;
    assert(bus.send_message(source, PortId("Queue"), {1}, at(1), queues));
    assert(bus.send_message(source, PortId("Event"), {2}, at(1), queues));
    assert(queues[target].size() == 2);
    assert(queues[target][0].sequence == 1 && queues[target][1].sequence == 1);
}

void test_snapshot_host_origin_regression_preserves_exact_latest_delivery() {
    ResourceBus bus;
    const auto source = FeatureInstanceId("SnapshotSource");
    const auto target = FeatureInstanceId("SnapshotTarget");
    assert(bus.register_snapshot_edge(source, target, PortId("Out"), PortId("In"), 100, 1'000'000'000));
    Queues queues;
    PortMessage latest{};
    latest.source_port = PortId("Out"); latest.timestamp_ticks = 10; latest.payload = {10, 0, 0xFF};
    latest.source.timing = TransportTiming{at(10), {}};
    assert(bus.send_message(source, latest, at(10), queues));
    const auto saved = queues[target].front();
    auto old = latest;
    old.source.timing = TransportTiming{at(9), {}};
    old.timestamp_ticks = 9; old.payload = {9};
    assert(!bus.send_message(source, old, at(11), queues));
    assert(queues[target].size() == 1 && queues[target].front().delivery_id == saved.delivery_id &&
           queues[target].front().payload == saved.payload && queues[target].front().source.timing == saved.source.timing);
}

int main() {
    test_router_rejects_expired_message_without_mutation();
    test_router_schema_and_capacity_validation_is_atomic();
    test_resource_bus_command_receipt_and_timeout();
    test_queue_and_event_sequences_are_independent();
    test_snapshot_host_origin_regression_preserves_exact_latest_delivery();
    std::cout << "feature transport smoke passed\n";
}
