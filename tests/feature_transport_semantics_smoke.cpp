/**
 * @file feature_transport_semantics_smoke.cpp
 * @brief Verifies explicit Event causality and duplicate suppression.
 */
#include <cassert>
#include <iostream>
#include <unordered_map>
#include <vector>

#include <feature_resource_bus.hpp>

namespace {
using namespace aeg::feature;

// These transport-only fixtures use one nanosecond per authored source period.
constexpr TransportTimePoint at(uint64_t nanoseconds) { return {RuntimeGeneration(1), nanoseconds}; }

void test_event_causality_token_and_deduplication() {
    ResourceBus bus;
    const FeatureInstanceId source("Logic.Source");
    const FeatureInstanceId target("Audio.Target");
    PortRoute route{};
    route.target_instance = target;
    route.source_port = PortId("Event");
    route.target_port = PortId("Input");
    route.kind = EdgeKind::kEvent;
    assert(bus.register_route(source, route));

    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>> queues;
    PortMessage first{};
    first.source_port = PortId("Event");
    first.sequence = 7;
    first.payload = {1, 2, 3};
    assert(bus.send_message(source, first, at(4), queues));
    assert(queues[target].size() == 1);
    assert(queues[target].front().causality_token != 0);

    // Replaying the same causal sequence must not deliver a duplicate event.
    assert(bus.send_message(source, first, at(4), queues));
    assert(queues[target].size() == 1);
}

void test_event_causality_can_be_disabled_by_explicit_helper() {
    ResourceBus bus;
    const FeatureInstanceId source("Logic.BestEffort");
    const FeatureInstanceId target("Audio.BestEffort");
    assert(bus.register_event_edge(source, target, PortId("Event"), PortId("Input"), false));

    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>> queues;
    PortMessage replay{};
    replay.source_port = PortId("Event");
    replay.sequence = 3;
    assert(bus.send_message(source, replay, at(1), queues));
    assert(bus.send_message(source, replay, at(1), queues));
    assert(queues[target].size() == 2);
}

}  // namespace

int main() {
    test_event_causality_token_and_deduplication();
    test_event_causality_can_be_disabled_by_explicit_helper();
    std::cout << "feature transport semantics smoke passed\n";
    return 0;
}
