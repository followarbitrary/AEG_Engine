/** Actual owner workload admission must preserve the live Runtime on rejection. */
#include <feature_graph_runtime_host.hpp>
#include <feature_clock_admission.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace aeg::feature;
using aeg::application::runtime::FeatureGraphRuntimeHost;
namespace admission = aeg::feature::detail;

template<class T> void require(const T& result, const char* message) {
    if (!static_cast<bool>(result)) throw std::runtime_error(message);
}

admission::ClockHostContextPtr host_context() {
    const auto result = admission::make_clock_host_context({});
    require(result, "normal host context failed");
    return result.value;
}

FeatureDefinition definition(std::string_view name, uint64_t budget = 6'000'000,
                             ExecutionKind execution = ExecutionKind::kProcess) {
    FeatureDefinition value{};
    value.id = FeatureDefinitionId(name);
    value.version = {1, 0, 0};
    auto& contract = value.contract;
    contract.definition_id = value.id;
    contract.version = value.version;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = execution;
    contract.state = StatePolicy::kInstanceState;
    contract.scope = {ScopeId("World")};
    contract.clock = make_periodic_clock_contract(ClockId("RuntimeAdmission.Clock"), 60, 4,
        LateDataPolicy::kReject, DeadlinePolicy::kObserve, budget, 0);
    // A 100ms relative deadline isolates utilization from the minimum-deadline sum.
    contract.clock.timing->relative_deadline_ns = 100'000'000;
    contract.permissions.version = 1;
    contract.has_continuation = execution == ExecutionKind::kProcess;
    contract.ports = {{PortId("Input"), PortDirection::kInput, ValueSchemaId("Bytes"), 1},
                      {PortId("Output"), PortDirection::kOutput, ValueSchemaId("Bytes"), 1}};
    value.execute = [](const InvocationToken& token, const StateSnapshot& state,
                       std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{token.state_version, state.version.value()};
        delta.state_payload = std::vector<uint8_t>{static_cast<uint8_t>(state.version.value())};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    return value;
}

void register_feature(FeatureRegistry& registry, const FeatureDefinition& value) {
    require(admission::admit_clock_request(registry, value.contract), "Clock approval failed");
    require(registry.register_definition(value), "Feature registration failed");
}

FeatureGraphDocument graph(std::string asset, const FeatureDefinition& feature,
                           const FeatureRegistry& registry) {
    FeatureGraphDocument value{};
    value.asset_uuid = std::move(asset);
    value.schema_version = 1;
    value.graph_version = 1;
    value.contract_digest = registry.contract_digest();
    value.nodes = {{FeatureNodeId("Owner"), feature.id, feature.version}};
    return value;
}

bool same_state(const StateStore& a, const StateStore& b) {
    return a.version == b.version && a.commit_count == b.commit_count &&
           a.payload_revision == b.payload_revision && a.schema_version == b.schema_version &&
           a.payload == b.payload;
}

void actual_repeated_owners_and_suspended_reservation() {
    FeatureRegistry registry(host_context());
    const auto value = definition("RuntimeAdmission.Repeated");
    register_feature(registry, value);
    FeatureRuntime runtime(registry);
    auto expansion = registry.expand(value.id, OwnerPath{{FeatureNodeId("First")}});
    require(expansion, "first expansion failed");
    const auto first = runtime.activate(expansion.value, ScopeId("World"));
    require(first, "first 360ms/s owner rejected");
    expansion = registry.expand(value.id, OwnerPath{{FeatureNodeId("Second")}});
    const auto second = runtime.activate(expansion.value, ScopeId("World"));
    require(second, "second 360ms/s owner rejected");
    require(runtime.tick(value.contract.clock.id), "initial owner tick failed");
    const auto first_state = runtime.find_instance(first.value)->state;
    const auto second_state = runtime.find_instance(second.value)->state;
    const auto process = runtime.process(first.value);
    require(process, "Process owner absent");
    require(runtime.suspend(process.value), "suspend failed");
    const auto generation = runtime.generation();
    expansion = registry.expand(value.id, OwnerPath{{FeatureNodeId("Third")}});
    require(!runtime.activate(expansion.value, ScopeId("World")),
            "third repeated owner exceeded 1s/s; suspension must retain its reservation");
    require(runtime.generation() == generation &&
            same_state(first_state, runtime.find_instance(first.value)->state) &&
            same_state(second_state, runtime.find_instance(second.value)->state),
            "rejected activation changed live generation or state");
    require(runtime.resume(process.value), "existing suspended Process did not survive rejection");
    require(runtime.tick(value.contract.clock.id), "existing plan did not recover after rejection");
    require(runtime.find_instance(first.value)->state.commit_count == first_state.commit_count + 1 &&
            runtime.find_instance(second.value)->state.commit_count == second_state.commit_count + 1,
            "existing owners did not commit exactly once after rejection");
}

void composite_duplicate_children_are_actual_instances() {
    FeatureRegistry registry(host_context());
    const auto leaf = definition("RuntimeAdmission.CompositeLeaf");
    register_feature(registry, leaf);
    auto parent = definition("RuntimeAdmission.Composite");
    parent.contract.structure = FeatureStructure::kComposite;
    parent.contract.execution = ExecutionKind::kCall;
    parent.contract.state = StatePolicy::kStateless;
    parent.contract.has_continuation = false;
    parent.contract.clock.work_budget_ns = 0;
    parent.contract.children.clear();
    parent.contract.ports.clear();
    parent.execute = {};
    // Child declarations are unique by Definition. Three distinct structural
    // wrappers each instantiate the same leaf at a different actual owner path.
    for (uint64_t i = 0; i < 3; ++i) {
        auto wrapper = parent;
        wrapper.id = wrapper.contract.definition_id = FeatureDefinitionId("RuntimeAdmission.Wrapper" + std::to_string(i));
        wrapper.contract.children = {leaf.id};
        register_feature(registry, wrapper);
        parent.contract.children.push_back(wrapper.id);
    }
    register_feature(registry, parent);
    const auto expansion = registry.expand(parent.id, {});
    require(expansion, "duplicate-child Composite expansion failed");
    FeatureRuntime runtime(registry);
    const auto generation = runtime.generation();
    require(!runtime.activate(expansion.value, ScopeId("World")),
            "Composite duplicate children were deduplicated by Definition instead of counted as owners");
    require(runtime.generation() == generation && runtime.clocks().empty(),
            "rejected Composite activation left partial plans");
    const auto leaf_expansion = registry.expand(leaf.id, {});
    require(runtime.activate(leaf_expansion.value, ScopeId("World")),
            "rejected Composite activation leaked owner reservations");
}

void explicit_calls_do_not_reserve_an_authored_periodic_arrival_rate() {
    FeatureRegistry registry(host_context());
    const auto call = definition("RuntimeAdmission.Call", 90'000'000, ExecutionKind::kCall);
    register_feature(registry, call);
    FeatureRuntime runtime(registry);
    FeatureInstanceId last{};
    for (uint64_t i = 0; i < 24; ++i) {
        const auto expansion = registry.expand(call.id, OwnerPath{{FeatureNodeId(i + 1)}});
        require(expansion, "Call expansion failed");
        const auto instance = runtime.activate(expansion.value, ScopeId("World"));
        require(instance, "Call was incorrectly counted as 60Hz automatic workload");
        last = instance.value;
    }
    require(runtime.tick(call.contract.clock.id), "Call-only Clock tick failed");
    require(runtime.find_instance(last)->state.commit_count == 0, "Clock automatically invoked a Call");
    require(runtime.dispatch_call(last), "explicit admitted Call did not execute");
    require(runtime.find_instance(last)->state.commit_count == 1, "explicit Call did not commit");
}

void combined_roots_and_replacement_preserve_live_state_input_and_tokens() {
    FeatureRegistry registry(host_context());
    const auto owner = definition("RuntimeAdmission.BatchOwner");
    auto source = definition("RuntimeAdmission.Source", 250'000, ExecutionKind::kCall);
    source.execute = [](const InvocationToken& token, const StateSnapshot&,
                        std::span<const uint8_t>, std::span<const uint8_t>) {
        StateDelta delta{token.state_version, 1};
        delta.output_payload = {0x42, 0, 0xFE};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    register_feature(registry, owner);
    register_feature(registry, source);
    auto a = graph("admission://a", owner, registry);
    a.nodes.push_back({FeatureNodeId("Source"), source.id, source.version});
    a.edges = {{FeatureNodeId("Source"), PortId("Output"), FeatureNodeId("Owner"), PortId("Input"),
                EdgeKind::kQueue, owner.contract.clock.id, owner.contract.clock.id, 1, 4, 0}};
    const auto b = graph("admission://b", owner, registry);
    const auto c = graph("admission://c", owner, registry);
    FeatureRuntime runtime(registry);
    FeatureGraphRuntimeHost host(runtime, registry);
    require(host.stage(a) && host.stage(b) && host.apply_pending(), "valid two-root publication failed");
    require(runtime.tick(owner.contract.clock.id), "initial batch tick failed");
    const auto a_view = host.inspect_node(a.asset_uuid, FeatureNodeId("Owner"));
    const auto b_view = host.inspect_node(b.asset_uuid, FeatureNodeId("Owner"));
    const auto source_view = host.inspect_node(a.asset_uuid, FeatureNodeId("Source"));
    require(a_view && b_view && source_view, "published owners absent");
    const auto a_id = a_view.value.owners.front().owner.instance_id;
    const auto b_id = b_view.value.owners.front().owner.instance_id;
    const auto source_id = source_view.value.owners.front().owner.instance_id;
    require(runtime.dispatch_call(source_id), "source did not publish pending Queue input");
    const auto a_before = *runtime.find_instance(a_id);
    const auto b_before = *runtime.find_instance(b_id);
    require(a_before.pending_messages.size() == 1 && a_before.pending_input == std::vector<uint8_t>({0x42, 0, 0xFE}),
            "pending Queue fixture was not created");
    const auto token = runtime.invoke(b_id);
    require(token, "open owner token fixture failed");
    const auto generation = runtime.generation();
    const auto digest = registry.contract_digest();
    const auto check_live = [&] {
        const auto* live_a = runtime.find_instance(a_id);
        const auto* live_b = runtime.find_instance(b_id);
        require(runtime.generation() == generation && host.generation() == generation &&
                registry.contract_digest() == digest && host.active_documents().size() == 2,
                "rejected workload changed active generation, Registry, or roots");
        require(live_a && live_b && same_state(live_a->state, a_before.state) &&
                same_state(live_b->state, b_before.state) &&
                live_a->pending_input == a_before.pending_input && live_a->pending_messages.size() == 1 &&
                live_a->pending_messages.front().delivery_id == a_before.pending_messages.front().delivery_id,
                "rejected workload changed owner state or exact pending delivery");
        const auto snapshot = runtime.inspect_invocations(b_id);
        require(snapshot && snapshot.value.open_attempts.size() == 1 &&
                snapshot.value.open_attempts.front().committable &&
                snapshot.value.open_attempts.front().attempt.token.invocation_id == token.value.invocation_id,
                "rejected workload retired an existing open token");
    };
    require(host.stage(a) && host.stage(b) && host.stage(c), "individually valid candidate roots did not stage");
    require(!host.apply_pending(), "three roots exceeded the combined domain utilization but published");
    check_live();
    auto replacement = owner;
    replacement.version = replacement.contract.version = {2, 0, 0};
    replacement.contract.clock.work_budget_ns = 9'000'000;
    require(!runtime.replace_definition(replacement,
                {owner.version, replacement.version, {1, 0, 0}, {1, 0, 0}}),
            "replacement counted one Definition instead of both actual owners");
    check_live();
    require(runtime.complete(token.value, StateDelta{token.value.state_version, 9}),
            "original open token no longer commits after rejected publication/replacement");
    require(runtime.tick(owner.contract.clock.id), "last-good batch did not recover");
    require(runtime.find_instance(a_id)->pending_messages.empty() &&
            runtime.find_instance(a_id)->state.commit_count == a_before.state.commit_count + 1,
            "last-good owner did not consume its original pending delivery exactly once");
    // Failed publication retains its reviewable candidate batch. The existing
    // duplicate-stage rejection discards that batch before authoring a new one.
    if (host.has_pending()) require(!host.stage(a), "duplicate stage did not discard the rejected candidate batch");
    require(host.stage(a) && host.stage(b) && host.apply_pending(), "valid replacement batch did not recover");
}

void stale_registry_owner_cannot_be_repriced(bool replace_peer) {
    FeatureRegistry registry(host_context());
    const auto first = definition("RuntimeAdmission.StaleOwner");
    const auto peer = definition("RuntimeAdmission.StalePeer", 1'000'000);
    register_feature(registry, first);
    register_feature(registry, peer);
    FeatureRuntime runtime(registry);
    const auto first_expansion = registry.expand(first.id, {});
    const auto peer_expansion = registry.expand(peer.id, {});
    require(first_expansion && peer_expansion, "stale-owner fixture expansion failed");
    const auto first_owner = runtime.activate(first_expansion.value, ScopeId("World"));
    const auto peer_owner = runtime.activate(peer_expansion.value, ScopeId("World"));
    require(first_owner && peer_owner, "stale-owner fixture activation failed");
    require(runtime.tick(first.contract.clock.id), "stale-owner fixture tick failed");
    const auto original_state = runtime.find_instance(first_owner.value)->state;
    const auto peer_state = runtime.find_instance(peer_owner.value)->state;
    const auto token = runtime.invoke(peer_owner.value);
    require(token, "stale-owner peer open token failed");
    auto changed = first;
    changed.version = changed.contract.version = {2, 0, 0};
    changed.contract.clock.work_budget_ns = 1;
    // Registry mutation is deliberately performed without Runtime state migration.
    require(registry.replace_definition(changed), "could not create an actual stale Runtime binding");
    const auto digest = registry.contract_digest();
    const auto generation = runtime.generation();
    if (replace_peer) {
        auto replacement = peer;
        replacement.version = replacement.contract.version = {2, 0, 0};
        replacement.contract.clock.work_budget_ns = 2'000'000;
        const auto result = runtime.replace_definition(replacement,
            {peer.version, replacement.version, {1, 0, 0}, {1, 0, 0}});
        require(!result && result.code == aeg::foundation::ResultCode::kInvalidState,
                "peer replacement repriced an existing stale owner from its changed Registry contract");
    } else {
        const auto result = runtime.activate(peer_expansion.value, ScopeId("World"));
        require(!result && result.result.code == aeg::foundation::ResultCode::kInvalidState,
                "activation repriced an existing stale owner from its changed Registry contract");
    }
    require(runtime.generation() == generation && registry.contract_digest() == digest &&
            registry.find_definition(peer.id)->version == peer.version &&
            same_state(runtime.find_instance(first_owner.value)->state, original_state) &&
            same_state(runtime.find_instance(peer_owner.value)->state, peer_state),
            "stale-owner rejection mutated another live owner or generation");
    const auto open = runtime.inspect_invocations(peer_owner.value);
    require(open && open.value.open_attempts.size() == 1 && open.value.open_attempts.front().committable,
            "stale-owner rejection retired the unrelated peer token");
    require(runtime.complete(token.value, StateDelta{token.value.state_version, 17}),
            "unrelated peer token could not commit after stale-owner admission rejection");
    require(registry.replace_definition(first), "could not restore exact fixture Registry binding");
    require(runtime.tick(first.contract.clock.id), "restored exact owner binding did not recover");
}

void stale_registry_activation() { stale_registry_owner_cannot_be_repriced(false); }
void stale_registry_peer_replacement() { stale_registry_owner_cannot_be_repriced(true); }
} // namespace

int main() {
    int failures = 0;
    const struct { const char* name; void (*run)(); } cases[] = {
        {"actual-repeated-and-suspended", actual_repeated_owners_and_suspended_reservation},
        {"composite-duplicate-owners", composite_duplicate_children_are_actual_instances},
        {"explicit-call-rate", explicit_calls_do_not_reserve_an_authored_periodic_arrival_rate},
        {"combined-roots-replacement-last-good", combined_roots_and_replacement_preserve_live_state_input_and_tokens},
        {"stale-owner-activation", stale_registry_activation},
        {"stale-owner-peer-replacement", stale_registry_peer_replacement},
    };
    for (const auto& item : cases) {
        try { item.run(); std::cout << "Clock Runtime admission " << item.name << " passed\n"; }
        catch (const std::exception& error) { ++failures; std::cerr << item.name << ": " << error.what() << '\n'; }
    }
    return failures == 0 ? 0 : 1;
}
