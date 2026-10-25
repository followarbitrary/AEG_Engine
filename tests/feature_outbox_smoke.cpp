/** @brief Exercises committed Feature outbox journals; World application is a separate boundary. */
#include "feature_clock_test_fixture.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "concurrency.hpp"
#include "feature_graph.hpp"
#include "feature_render_test_fixture.hpp"
#include "../Engine/Feature/private/feature_outbox.hpp"

namespace {
using namespace aeg::feature;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using Access = detail::FeatureOutboxAccess;
using Delivery = detail::OutboxDeliveryState;
constexpr SideEffectId kEffect{"Fixture.World.TransformCommand"};
constexpr SideEffectId kWrongKind{"Fixture.NotACommand"};
constexpr SideEffectId kWrongCommit{"Fixture.NotAnOutbox"};
constexpr ClockId kClock{"Fixture.WorldTick"};
constexpr FeatureDefinitionId kProducer{"Fixture.Outbox.Producer"};
constexpr ModuleIdentity kModule{"com.aeg.outbox.fixture"};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class T> void succeeds(const T& result, const char* message) {
    require(static_cast<bool>(result), message);
}

struct Script final {
    StateDelta delta{};
    std::vector<InvocationToken> calls{};
};

FeatureDefinition producer_definition(Script& script, bool process) {
    FeatureDefinition definition{};
    definition.id = kProducer;
    definition.version = {1, 0, 0};
    definition.owner_module = kModule;
    auto& contract = definition.contract;
    contract.definition_id = definition.id;
    contract.version = definition.version;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = process ? ExecutionKind::kProcess : ExecutionKind::kCall;
    contract.state = process ? StatePolicy::kProcessState : StatePolicy::kInstanceState;
    contract.has_continuation = process;
    contract.scope = {ScopeId("World")};
    contract.clock = aeg::feature::make_periodic_clock_contract(kClock, 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.state_payload = {{1, 0, 0}, 8};
    contract.ports = {{PortId("Output"), PortDirection::kOutput, ValueSchemaId("Fixture.Byte"), 1}};
    contract.side_effects = {
        {kEffect, 1, SideEffectKind::kCommand, CommitPolicy::kOutbox},
        {kWrongKind, 1, SideEffectKind::kStateDelta, CommitPolicy::kOutbox},
        {kWrongCommit, 1, SideEffectKind::kCommand, CommitPolicy::kOwnerBoundary}};
    definition.execute = [&script](const InvocationToken& token, const StateSnapshot&,
        std::span<const uint8_t>, std::span<const uint8_t>) -> aeg::foundation::ResultValue<StateDelta> {
        script.calls.push_back(token);
        auto delta = script.delta;
        delta.expected_version = token.state_version;
        return {Result::success(), std::move(delta)};
    };
    return definition;
}

struct Fixture final {
    Script script{};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry};
    FeatureDefinition definition{};
    FeatureGraphDocument graph{};
    CompiledFeatureGraph compiled{};
    FeatureInstanceId primary{}, other{}, sink{};

    explicit Fixture(bool process = false) {
        definition = producer_definition(script, process);
        succeeds(aeg::feature::test::register_clock_test_definition(registry, definition), "register actual producer");
        auto target = producer_definition(script, false);
        target.id = FeatureDefinitionId("Fixture.Outbox.Sink");
        target.contract.definition_id = target.id;
        target.contract.ports = {{PortId("Input"), PortDirection::kInput, ValueSchemaId("Fixture.Byte"), 1}};
        target.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                            std::span<const uint8_t>) -> aeg::foundation::ResultValue<StateDelta> {
            return {Result::success(), StateDelta{token.state_version}};
        };
        succeeds(aeg::feature::test::register_clock_test_definition(registry, std::move(target)), "register actual route target");
        graph.asset_uuid = "fixture://feature-outbox";
        graph.schema_version = 1;
        graph.graph_version = 1;
        graph.contract_digest = registry.contract_digest();
        graph.nodes = {
            {FeatureNodeId("Primary"), kProducer, {1, 0, 0}, OwnerPath{{FeatureNodeId("WorldRoot")}}, {}},
            {FeatureNodeId("Other"), kProducer, {1, 0, 0}, OwnerPath{{FeatureNodeId("OtherRoot")}}, {}},
            {FeatureNodeId("Sink"), FeatureDefinitionId("Fixture.Outbox.Sink"), {1, 0, 0}, {}, {}}};
        graph.edges = {{FeatureNodeId("Primary"), PortId("Output"), FeatureNodeId("Sink"), PortId("Input"),
                        EdgeKind::kQueue, kClock, kClock, 1, 8, 0}};
        if (process) {
            for (const auto node : {FeatureNodeId("Primary"), FeatureNodeId("Other")})
                graph.process_subgraphs.push_back({node, {"Idle", "Done"}, {"Idle->Done"}, 1});
        }
        auto result = FeatureGraphCompiler{}.compile(graph, registry);
        succeeds(result, "compile actual outbox graph");
        compiled = std::move(result.value);
        succeeds(runtime.publish_graph(graph, compiled), "publish actual outbox graph");
        for (const auto& row : compiled.provenance) {
            if (row.node_id == FeatureNodeId("Primary")) primary = row.instance_id;
            if (row.node_id == FeatureNodeId("Other")) other = row.instance_id;
            if (row.node_id == FeatureNodeId("Sink")) sink = row.instance_id;
        }
        require(primary.is_valid() && other.is_valid() && sink.is_valid() && primary != other,
                "publication did not produce independent real owners");
        script.delta.state_payload = std::vector<uint8_t>{0x51};
    }

    void dispatch(bool process = false) {
        succeeds(process ? runtime.dispatch_process(primary) : runtime.dispatch_call(primary), "actual producer dispatch");
    }

    auto bind(FeatureInstanceId owner) { return Access::bind(runtime, owner, kEffect, 1); }
};

struct Applied final {
    uint32_t calls{0};
    std::size_t bytes{0};
    uint8_t first{0};
};

void apply(void* opaque, const OutboxIntent& intent) noexcept {
    auto& observed = *static_cast<Applied*>(opaque);
    ++observed.calls;
    observed.bytes = intent.payload.size();
    observed.first = intent.payload.empty() ? 0 : intent.payload.front();
}

PortMessage routed_byte(uint8_t value) {
    PortMessage message{};
    message.source_port = PortId("Output");
    message.schema = ValueSchemaId("Fixture.Byte");
    message.schema_version = 1;
    message.payload = {value};
    return message;
}

void test_ordered_owning_entries_and_canonical_authority() {
    Fixture fixture;
    const auto generation = fixture.runtime.generation();
    const auto digest = fixture.runtime.find_instance(fixture.primary)->definition_digest;
    // Binding has no caller-owned graph argument. Altering the exported diagnostic
    // value cannot move a real owner into a forged author node/module/path/clock.
    for (auto& row : fixture.compiled.provenance) {
        row.node_id = FeatureNodeId("Forged"); row.owner_module = ModuleIdentity("forged.module");
        row.owner_path = {{FeatureNodeId("ForgedRoot")}}; row.clock = ClockId("ForgedClock");
    }
    const auto binding = fixture.bind(fixture.primary);
    const auto other_binding = fixture.bind(fixture.other);
    succeeds(binding, "bind canonical producer"); succeeds(other_binding, "bind other actual owner");
    const auto* source = binding.value.source();
    require(source && source->definition_id == kProducer && source->node_id == FeatureNodeId("Primary") &&
        source->instance_id == fixture.primary && source->owner_module == kModule && source->clock == kClock &&
        source->domains == to_mask(FeatureDomain::kLogic) &&
        source->owner_path.nodes == std::vector<FeatureNodeId>{FeatureNodeId("WorldRoot")} &&
        binding.value.generation() == generation && binding.value.definition_digest() == digest &&
        binding.value.effect() == kEffect && binding.value.version() == 1,
        "binding trusted a caller-edited graph instead of actual published provenance");
    fixture.script.delta.outbox = {{kEffect, 1, {0x11}}, {kEffect, 1, {0x12}}};
    fixture.dispatch();
    fixture.script.delta.outbox = {{kEffect, 1, {0x21}}};
    fixture.dispatch();
    require(fixture.script.calls.size() == 2 && fixture.script.calls[0] != fixture.script.calls[1],
            "two real callback commits did not produce distinct full tokens");
    const auto manual_token = fixture.runtime.invoke(fixture.primary);
    succeeds(manual_token, "invoke copy-mutation commit");
    StateDelta submitted{manual_token.value.state_version};
    submitted.outbox = {{kEffect, 1, {0x31, 0x32}}};
    succeeds(fixture.runtime.complete(manual_token.value, submitted), "commit caller-owned Delta");
    submitted.outbox.front() = {SideEffectId("ForgedEffect"), 99, {0xee}};
    submitted.outbox.clear();
    const auto captured = Access::capture(fixture.runtime, binding.value);
    succeeds(captured, "capture all pending commits");
    require(captured.value.size() == 4, "journal overwrote a preceding commit or lost an entry");
    const std::array<std::vector<uint8_t>, 4> payloads{{{0x11}, {0x12}, {0x21}, {0x31, 0x32}}};
    const std::array<InvocationToken, 4> tokens{{fixture.script.calls[0], fixture.script.calls[0],
                                               fixture.script.calls[1], manual_token.value}};
    const std::array<uint32_t, 4> indices{{0, 1, 0, 0}};
    for (std::size_t index = 0; index < captured.value.size(); ++index) {
        const auto& handle = captured.value[index];
        const auto receipt = handle.receipt();
        require(handle.intent() && handle.intent()->effect == kEffect && handle.intent()->version == 1 &&
            handle.intent()->payload == payloads[index] && handle.invocation() && *handle.invocation() == tokens[index] &&
            handle.entry_index() == indices[index] && receipt.invocation == tokens[index] &&
            receipt.entry_index == indices[index] && receipt.state == Delivery::kPending &&
            receipt.error_code == ResultCode::kSuccess, "capture lost immutable bytes, full token or entry identity");
    }
    Applied observed;
    Fixture foreign;
    require(!Access::consume(fixture.runtime, other_binding.value, captured.value[0], apply, &observed) &&
        !Access::consume(foreign.runtime, binding.value, captured.value[0], apply, &observed) &&
        !Access::capture(foreign.runtime, binding.value) &&
        !Access::consume(fixture.runtime, binding.value, detail::FeatureOutboxHandle{}, apply, &observed) &&
        !Access::consume(fixture.runtime, binding.value, captured.value[0], nullptr, &observed),
        "foreign owner/Runtime, invalid handle or null apply acquired consumption authority");
    Result foreign_thread;
    const auto owner_thread = aeg::concurrency::current_thread_id();
    aeg::concurrency::ThreadId probe_thread{};
    aeg::concurrency::WorkerExecutor worker({aeg::concurrency::ExecutorDomain::kCpuWorker, 1, false});
    aeg::concurrency::TaskDesc probe{};
    probe.domain = aeg::concurrency::ExecutorDomain::kCpuWorker;
    probe.execute = [&](const aeg::concurrency::TaskContext&) {
        probe_thread = aeg::concurrency::current_thread_id();
        foreign_thread = Access::consume(fixture.runtime, binding.value, captured.value[0], apply, &observed);
        return Result::success();
    };
    const auto probe_submission = worker.submit(std::move(probe));
    succeeds(probe_submission, "submit foreign owner-thread probe");
    succeeds(probe_submission.value.wait(2'000'000'000), "complete foreign owner-thread probe");
    require(probe_thread.is_valid() && probe_thread != owner_thread && !foreign_thread &&
            observed.calls == 0 && captured.value[0].receipt().state == Delivery::kPending,
            "rejected consumption called apply or lost the pending entry");
    require(!Access::reject(fixture.runtime, binding.value, captured.value[0], ResultCode::kSuccess) &&
        !Access::reject(fixture.runtime, binding.value, captured.value[0], static_cast<ResultCode>(255)),
        "terminal rejection accepted a successful or invalid result code");
    succeeds(Access::consume(fixture.runtime, binding.value, captured.value[0], apply, &observed), "retry after failed prechecks");
    require(observed.calls == 1 && observed.first == 0x11 && observed.bytes == 1 &&
        captured.value[0].receipt().state == Delivery::kApplied, "successful consumption did not apply once");
    require(!Access::consume(fixture.runtime, binding.value, captured.value[0], apply, &observed) && observed.calls == 1,
            "applied handle replayed its side effect");
    succeeds(Access::reject(fixture.runtime, binding.value, captured.value[1], ResultCode::kUnsupported), "terminal rejection");
    require(captured.value[1].receipt().state == Delivery::kRejected &&
        captured.value[1].receipt().error_code == ResultCode::kUnsupported &&
        !Access::reject(fixture.runtime, binding.value, captured.value[1], ResultCode::kCancelled) &&
        !Access::consume(fixture.runtime, binding.value, captured.value[1], apply, &observed), "rejected entry remained consumable");
    for (std::size_t index : {std::size_t{2}, std::size_t{3}})
        succeeds(Access::consume(fixture.runtime, binding.value, captured.value[index], apply, &observed), "consume older committed entry");
    require(observed.calls == 3 && observed.first == 0x31 && observed.bytes == 2,
            "pending commands depended on the owner's latest token");
    const auto empty = Access::capture(fixture.runtime, binding.value);
    succeeds(empty, "capture drained journal"); require(empty.value.empty(), "terminal entries remained pending");
}

void test_declared_effects_and_commit_atomicity() {
    Fixture fixture;
    const auto binding = fixture.bind(fixture.primary); succeeds(binding, "bind atomicity owner");
    require(!Access::bind(fixture.runtime, fixture.primary, SideEffectId("Missing"), 1) &&
        !Access::bind(fixture.runtime, fixture.primary, kEffect, 2) &&
        !Access::bind(fixture.runtime, fixture.primary, kWrongKind, 1) &&
        !Access::bind(fixture.runtime, fixture.primary, kWrongCommit, 1), "bind accepted an undeclared outbox protocol");
    const auto expanded = fixture.registry.expand(kProducer, OwnerPath{{FeatureNodeId("RawActivation")}});
    succeeds(expanded, "expand raw activation fixture");
    const auto raw = fixture.runtime.activate(expanded.value, ScopeId("World")); succeeds(raw, "actual unbound activation");
    require(!Access::bind(fixture.runtime, raw.value, kEffect, 1), "an owner outside real graph publication acquired binding");
    fixture.script.delta.state_payload = std::vector<uint8_t>{0x31};
    fixture.script.delta.output_messages = {routed_byte(0x41)};
    fixture.script.delta.outbox = {{kEffect, 1, {0xa1}}}; fixture.dispatch();
    const auto first = Access::capture(fixture.runtime, binding.value); succeeds(first, "capture baseline commit");
    require(first.value.size() == 1, "baseline entry missing");
    const auto version = fixture.runtime.find_instance(fixture.primary)->state.version;
    const auto delivery = fixture.runtime.find_instance(fixture.sink)->pending_messages.front().delivery_id;
    const auto unchanged = [&] {
        const auto* owner = fixture.runtime.find_instance(fixture.primary);
        const auto* target = fixture.runtime.find_instance(fixture.sink);
        const auto pending = Access::capture(fixture.runtime, binding.value);
        require(owner && owner->state.version == version && owner->state.commit_count == 1 &&
            owner->state.payload == std::vector<uint8_t>{0x31} && target && target->pending_messages.size() == 1 &&
            target->pending_messages[0].delivery_id == delivery && target->pending_messages[0].payload == std::vector<uint8_t>{0x41} &&
            pending && pending.value.size() == 1 && pending.value[0].intent()->payload == std::vector<uint8_t>{0xa1},
            "rejected outbox partially committed Feature state, routes or journal");
    };
    const std::array<OutboxIntent, 4> invalid{{
        {SideEffectId("Undeclared"), 1, {0xb1}}, {kEffect, 2, {0xb2}},
        {kWrongKind, 1, {0xb3}}, {kWrongCommit, 1, {0xb4}}}};
    for (const auto& bad : invalid) {
        fixture.script.delta.state_payload = std::vector<uint8_t>{0x52};
        fixture.script.delta.output_messages = {routed_byte(0x62)};
        fixture.script.delta.outbox = {{kEffect, 1, {0xb0}}, bad};
        require(!fixture.runtime.dispatch_call(fixture.primary), "invalid later outbox entry committed"); unchanged();
    }
    fixture.script.delta.outbox = {{kEffect, 1, {0xb0}}};
    fixture.script.delta.output_messages[0].schema = ValueSchemaId("WrongSchema");
    require(!fixture.runtime.dispatch_call(fixture.primary), "invalid routed schema committed"); unchanged();
    fixture.script.delta.output_messages = {routed_byte(0x62)};
    fixture.script.delta.state_payload = std::vector<uint8_t>(9, 0x52);
    require(!fixture.runtime.dispatch_call(fixture.primary), "invalid state schema committed outbox"); unchanged();
    fixture.script.delta.state_payload = std::vector<uint8_t>{0x52};
    fixture.script.delta.outbox.assign(1025, OutboxIntent{kEffect, 1, {0xb0}});
    require(!fixture.runtime.dispatch_call(fixture.primary), "per-commit entry limit accepted 1025"); unchanged();
    fixture.script.delta.outbox = {{kEffect, 1, std::vector<uint8_t>(1024 * 1024 + 1, 0xb0)}};
    require(!fixture.runtime.dispatch_call(fixture.primary), "per-intent byte limit accepted 1 MiB plus one"); unchanged();
    fixture.script.delta.outbox = {{kEffect, 1, {0xb1}}}; fixture.dispatch();
    const auto accepted = Access::capture(fixture.runtime, binding.value);
    require(accepted && accepted.value.size() == 2 &&
        fixture.runtime.find_instance(fixture.primary)->state.commit_count == 2 &&
        fixture.runtime.find_instance(fixture.sink)->pending_messages.size() == 2,
        "valid retry after rejected commit lost existing or new output");
}

void test_composite_descendants_keep_distinct_canonical_sources() {
    Script script;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    succeeds(aeg::feature::test::register_clock_test_definition(registry, producer_definition(script, false)), "register actual Composite leaf");
    FeatureDefinition composite{};
    composite.id = FeatureDefinitionId("Fixture.Outbox.Composite");
    composite.version = {1, 0, 0};
    composite.owner_module = ModuleIdentity("com.aeg.outbox.composite");
    auto& contract = composite.contract;
    contract.definition_id = composite.id; contract.version = composite.version;
    contract.structure = FeatureStructure::kComposite; contract.clock.work_budget_ns = 0;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kCall; contract.state = StatePolicy::kStateless;
    contract.scope = {ScopeId("World")};
    contract.clock = aeg::feature::make_periodic_clock_contract(kClock, 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.children = {kProducer};
    succeeds(aeg::feature::test::register_clock_test_definition(registry, composite), "register actual Composite");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "fixture://composite-outbox"; graph.schema_version = 1; graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    graph.nodes = {
        {FeatureNodeId("RootA"), composite.id, composite.version, OwnerPath{{FeatureNodeId("WorldA")}}, {}},
        {FeatureNodeId("RootB"), composite.id, composite.version, OwnerPath{{FeatureNodeId("WorldB")}}, {}}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry); succeeds(compiled, "compile Composite graph");
    FeatureRuntime runtime(registry);
    succeeds(runtime.publish_graph(graph, compiled.value), "publish Composite graph");
    std::array<FeatureInstanceId, 2> leaves{};
    for (const auto& row : compiled.value.provenance) {
        const auto* root = runtime.find_instance(row.instance_id);
        require(root && root->definition_id == composite.id && root->children.size() == 1,
                "actual Composite did not expand one real leaf");
        if (row.node_id == FeatureNodeId("RootA")) leaves[0] = root->children.front();
        if (row.node_id == FeatureNodeId("RootB")) leaves[1] = root->children.front();
    }
    require(leaves[0].is_valid() && leaves[1].is_valid() && leaves[0] != leaves[1],
            "two Composite roots aliased the same leaf instance");
    const auto first = Access::bind(runtime, leaves[0], kEffect, 1);
    const auto second = Access::bind(runtime, leaves[1], kEffect, 1);
    succeeds(first, "bind first actual descendant"); succeeds(second, "bind second actual descendant");
    const std::array<const detail::FeatureOutboxBinding*, 2> bindings{{&first.value, &second.value}};
    const std::array<FeatureNodeId, 2> nodes{{FeatureNodeId("RootA"), FeatureNodeId("RootB")}};
    const std::array<std::vector<FeatureNodeId>, 2> paths{{
        {FeatureNodeId("WorldA"), FeatureNodeId("RootA"), FeatureNodeId("Fixture.Outbox.Producer")},
        {FeatureNodeId("WorldB"), FeatureNodeId("RootB"), FeatureNodeId("Fixture.Outbox.Producer")}}};
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const auto* source = bindings[index]->source();
        const auto* leaf = runtime.find_instance(leaves[index]);
        require(source && leaf && source->definition_id == kProducer && source->node_id == nodes[index] &&
            source->instance_id == leaves[index] && source->owner_module == kModule && source->clock == kClock &&
            source->domains == to_mask(FeatureDomain::kLogic) && source->owner_path.nodes == paths[index] &&
            leaf->owner_path.nodes == paths[index] && leaf->owner_module == kModule,
            "descendant source borrowed another root, module, path or instance");
    }
    script.delta.outbox = {{kEffect, 1, {0xd1}}};
    succeeds(runtime.dispatch_call(leaves[0]), "dispatch first real leaf");
    script.delta.outbox = {{kEffect, 1, {0xd2}}};
    succeeds(runtime.dispatch_call(leaves[1]), "dispatch second real leaf");
    const auto first_output = Access::capture(runtime, first.value);
    const auto second_output = Access::capture(runtime, second.value);
    require(first_output && second_output && first_output.value.size() == 1 && second_output.value.size() == 1 &&
        first_output.value[0].intent()->payload == std::vector<uint8_t>{0xd1} &&
        second_output.value[0].intent()->payload == std::vector<uint8_t>{0xd2} && script.calls.size() == 2 &&
        *first_output.value[0].invocation() == script.calls[0] &&
        *second_output.value[0].invocation() == script.calls[1], "Composite leaf journals or tokens were mixed");
    Applied observed;
    require(!Access::consume(runtime, first.value, second_output.value[0], apply, &observed) &&
        !Access::consume(runtime, second.value, first_output.value[0], apply, &observed) && observed.calls == 0 &&
        first_output.value[0].receipt().state == Delivery::kPending &&
        second_output.value[0].receipt().state == Delivery::kPending,
        "a sibling Composite binding consumed another actual leaf's command");
    succeeds(Access::consume(runtime, first.value, first_output.value[0], apply, &observed), "consume first leaf command");
    require(observed.calls == 1 && observed.first == 0xd1, "first leaf applied different bytes");
    succeeds(Access::consume(runtime, second.value, second_output.value[0], apply, &observed), "consume second leaf command");
    require(observed.calls == 2 && observed.first == 0xd2, "second leaf applied different bytes");
}

void test_accepted_command_survives_definition_registry_removal() {
    Fixture fixture;
    const auto binding = fixture.bind(fixture.primary); succeeds(binding, "bind before definition retirement");
    fixture.script.delta.outbox = {{kEffect, 1, {0xe1}}}; fixture.dispatch();
    const auto captured = Access::capture(fixture.runtime, binding.value);
    require(captured && captured.value.size() == 1 && fixture.script.calls.size() == 1, "capture accepted command before retirement");
    const auto token = *captured.value[0].invocation();
    succeeds(fixture.registry.unregister_definition(kProducer), "unregister committed producer definition");
    succeeds(fixture.registry.unregister_definition(FeatureDefinitionId("Fixture.Outbox.Sink")), "empty the real definition registry");
    require(fixture.registry.find_definition(kProducer) == nullptr &&
        fixture.registry.find_definition(FeatureDefinitionId("Fixture.Outbox.Sink")) == nullptr &&
        !fixture.runtime.dispatch_call(fixture.primary) && fixture.script.calls.size() == 1,
        "retired definition executed another callback");
    const auto still_pending = Access::capture(fixture.runtime, binding.value);
    require(still_pending && still_pending.value.size() == 1 && *still_pending.value[0].invocation() == token &&
        still_pending.value[0].intent()->payload == std::vector<uint8_t>{0xe1},
        "latest registry lookup discarded an already accepted command");
    Applied observed;
    succeeds(Access::consume(fixture.runtime, binding.value, captured.value[0], apply, &observed), "consume after registry retirement");
    require(observed.calls == 1 && observed.first == 0xe1 && fixture.script.calls.size() == 1 &&
        captured.value[0].receipt().state == Delivery::kApplied &&
        !Access::consume(fixture.runtime, binding.value, still_pending.value[0], apply, &observed) && observed.calls == 1,
        "accepted command was lost, replayed or re-executed after definition retirement");
}

void test_async_completion_pump_appends_one_delivery_once() {
    using aeg::concurrency::CompletionSource;
    CompletionSource entered, release;
    Script script;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    struct ReleaseOnExit final {
        CompletionSource& source;
        ~ReleaseOnExit() { static_cast<void>(source.complete()); }
    } release_on_exit{release};
    auto definition = producer_definition(script, false);
    aeg::concurrency::ThreadId callback_thread{};
    definition.execute = [execute = definition.execute, &entered, &release, &callback_thread](
        const InvocationToken& token, const StateSnapshot& state, std::span<const uint8_t> input,
        std::span<const uint8_t> parameters) -> aeg::foundation::ResultValue<StateDelta> {
        callback_thread = aeg::concurrency::current_thread_id();
        static_cast<void>(entered.complete());
        const auto released = release.completion().wait();
        if (!released) return {released, {}};
        return execute(token, state, input, parameters);
    };
    succeeds(aeg::feature::test::register_clock_test_definition(registry, definition), "register gated actual async producer");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "fixture://async-outbox"; graph.schema_version = 1; graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    graph.nodes = {{FeatureNodeId("Async"), definition.id, definition.version, {}, {}}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry); succeeds(compiled, "compile actual async graph");
    succeeds(runtime.publish_graph(graph, compiled.value), "publish actual async graph");
    require(compiled.value.provenance.size() == 1, "async graph did not publish its real owner");
    const auto owner = compiled.value.provenance[0].instance_id;
    const auto binding = Access::bind(runtime, owner, kEffect, 1); succeeds(binding, "bind actual async owner");
    script.delta.outbox = {{kEffect, 1, {0xf1}}};
    succeeds(runtime.enable_async_execution(), "enable actual Feature executor");
    succeeds(runtime.dispatch_call(owner), "dispatch actual async producer");
    succeeds(entered.completion().wait(2'000'000'000ull), "async producer did not enter its real worker");
    const auto before_pump = Access::capture(runtime, binding.value);
    require(callback_thread != aeg::concurrency::current_thread_id() && before_pump && before_pump.value.empty() &&
        runtime.pending_execution_count() == 1 && runtime.find_instance(owner)->state.commit_count == 0,
        "worker published an outbox entry before owner completion");
    succeeds(release.complete(), "release actual async producer");
    for (unsigned spin = 0; spin < 2000 && runtime.pending_execution_count() != 0; ++spin) {
        succeeds(runtime.pump_completions(), "actual owner completion pump");
        static_cast<void>(aeg::concurrency::sleep_for(1'000'000));
    }
    require(runtime.pending_execution_count() == 0 && script.calls.size() == 1 &&
        runtime.find_instance(owner)->state.commit_count == 1, "async owner completion did not settle exactly once");
    const auto first = Access::capture(runtime, binding.value);
    require(first && first.value.size() == 1 && first.value[0].intent()->payload == std::vector<uint8_t>{0xf1} &&
        *first.value[0].invocation() == script.calls[0] && first.value[0].entry_index() == 0, "async commit lost actual command/token");
    succeeds(runtime.pump_completions(), "repeat owner completion pump");
    succeeds(runtime.pump_completions(), "repeat empty owner completion pump");
    const auto repeated = Access::capture(runtime, binding.value);
    require(repeated && repeated.value.size() == 1 && *repeated.value[0].invocation() == script.calls[0] &&
        script.calls.size() == 1 && runtime.find_instance(owner)->state.commit_count == 1,
        "repeated completion pumps appended the same accepted output twice");
    Applied observed;
    succeeds(Access::consume(runtime, binding.value, first.value[0], apply, &observed), "consume actual asynchronous command");
    require(!Access::consume(runtime, binding.value, repeated.value[0], apply, &observed) && observed.calls == 1,
            "two captures of one async delivery replayed its effect");
}

void test_committed_outbox_survives_render_publication_failure() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    auto definition = render_feature_test::definition();
    definition.id = FeatureDefinitionId("Fixture.Outbox.RenderPublication");
    definition.contract.definition_id = definition.id;
    definition.contract.execution = ExecutionKind::kCall;
    definition.contract.parameters.clear();
    definition.contract.side_effects.push_back({kEffect, 1, SideEffectKind::kCommand, CommitPolicy::kOutbox});
    InvocationToken entered{};
    std::optional<RenderIntent> returned_intent;
    uint32_t callback_calls{0};
    definition.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                             std::span<const uint8_t>) -> aeg::foundation::ResultValue<StateDelta> {
        entered = token; ++callback_calls;
        RenderDrawClusteredScene draw{};
        draw.scene = {RenderResourceId("Scene"), 1};
        // This is the actual callback's token, but no Cull operation produced
        // the requested list. The value is well formed; cohort planning fails.
        draw.light_list = {RenderResourceId("Unproduced.List"), 1, token.invocation_id, token.generation};
        draw.hdr.target = {RenderResourceId("HDR"), 1, true};
        draw.depth = {{RenderResourceId("Depth"), 1, true}, RenderAttachmentLoad::kLoad, 1.f};
        returned_intent = RenderIntent{};
        returned_intent->operations = {draw};
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{0x61};
        delta.outbox = {{kEffect, 1, {0xf2}}};
        delta.render_intent = *returned_intent;
        return {Result::success(), std::move(delta)};
    };
    succeeds(aeg::feature::test::register_clock_test_definition(registry, definition), "register real publication-failure producer");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "fixture://outbox-render-publication"; graph.schema_version = 1; graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    graph.nodes = {{FeatureNodeId("Render"), definition.id, definition.version, {}, {}}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry); succeeds(compiled, "compile publication-failure graph");
    succeeds(runtime.publish_graph(graph, compiled.value), "publish publication-failure graph");
    require(compiled.value.provenance.size() == 1, "publication fixture did not resolve its actual owner");
    const auto owner = compiled.value.provenance[0].instance_id;
    const auto binding = Access::bind(runtime, owner, kEffect, 1); succeeds(binding, "bind actual Render outbox owner");
    const auto result = runtime.dispatch_call(owner);
    require(!result && result.code == ResultCode::kInvalidArgument && callback_calls == 1 && returned_intent &&
        validate_render_intent(*returned_intent, definition.contract),
        "expected a genuine post-commit planner failure, not callback or intent rejection");
    const auto history = runtime.inspect_invocations(owner);
    const auto* instance = runtime.find_instance(owner);
    require(history && history.value.last_committed && history.value.last_failed && history.value.open_attempts.empty() &&
        history.value.last_committed->attempt.token == entered &&
        history.value.last_failed->attempt.token == entered &&
        history.value.last_failed->stage == InvocationFailureStage::kPublication &&
        history.value.last_failed->result.code == ResultCode::kInvalidArgument && instance &&
        instance->state.commit_count == 1 && instance->state.payload == std::vector<uint8_t>{0x61} &&
        instance->state.version == history.value.last_committed->state_version &&
        runtime.render_publication_status(definition.contract.clock.id).code == ResultCode::kInvalidArgument,
        "publication error erased or misclassified the actual accepted owner commit");
    const auto commands = Access::capture(runtime, binding.value);
    require(commands && commands.value.size() == 1 && *commands.value[0].invocation() == entered &&
        commands.value[0].entry_index() == 0 && commands.value[0].intent()->payload == std::vector<uint8_t>{0xf2} &&
        commands.value[0].receipt().state == Delivery::kPending,
        "failed Render publication discarded an already committed outbox command");
    Applied observed;
    succeeds(Access::consume(runtime, binding.value, commands.value[0], apply, &observed), "consume command after publication error");
    succeeds(runtime.pump_completions(), "pump after reported publication failure");
    const auto after = Access::capture(runtime, binding.value);
    require(observed.calls == 1 && observed.first == 0xf2 && commands.value[0].receipt().state == Delivery::kApplied &&
        after && after.value.empty() && callback_calls == 1 && runtime.find_instance(owner)->state.commit_count == 1 &&
        !Access::consume(runtime, binding.value, commands.value[0], apply, &observed) && observed.calls == 1,
        "publication failure caused a committed command to be lost, appended twice or replayed");
}

void test_pending_budgets_are_released_by_terminal_delivery() {
    {
        Fixture fixture;
        const auto binding = fixture.bind(fixture.primary); succeeds(binding, "bind entry budget");
        fixture.script.delta.outbox.assign(1024, OutboxIntent{kEffect, 1, {0x71}}); fixture.dispatch();
        const auto full = Access::capture(fixture.runtime, binding.value);
        require(full && full.value.size() == 1024, "exact pending entry budget was rejected");
        fixture.script.delta.outbox = {{kEffect, 1, {0x72}}};
        require(!fixture.runtime.dispatch_call(fixture.primary) &&
            fixture.runtime.find_instance(fixture.primary)->state.commit_count == 1, "entry budget overflow committed state");
        Applied observed;
        succeeds(Access::consume(fixture.runtime, binding.value, full.value[0], apply, &observed), "release one pending entry");
        fixture.dispatch();
        const auto reused = Access::capture(fixture.runtime, binding.value);
        require(reused && reused.value.size() == 1024 && reused.value.back().intent()->payload == std::vector<uint8_t>{0x72} &&
            full.value[0].receipt().state == Delivery::kApplied, "applied retained handle still consumed pending budget");
    }
    {
        Fixture fixture;
        const auto binding = fixture.bind(fixture.primary); succeeds(binding, "bind byte budget");
        fixture.script.delta.outbox.assign(16, OutboxIntent{kEffect, 1, std::vector<uint8_t>(1024 * 1024, 0x81)});
        fixture.dispatch();
        const auto full = Access::capture(fixture.runtime, binding.value);
        require(full && full.value.size() == 16 && full.value.front().intent()->payload.size() == 1024 * 1024,
                "exact 1 MiB intent / 16 MiB pending budget was rejected");
        fixture.script.delta.outbox = {{kEffect, 1, {0x82}}};
        require(!fixture.runtime.dispatch_call(fixture.primary) &&
            fixture.runtime.find_instance(fixture.primary)->state.commit_count == 1, "pending byte overflow committed state");
        succeeds(Access::reject(fixture.runtime, binding.value, full.value[0], ResultCode::kUnsupported), "release rejected payload budget");
        fixture.dispatch();
        const auto reused = Access::capture(fixture.runtime, binding.value);
        require(reused && reused.value.size() == 16 && reused.value.back().intent()->payload == std::vector<uint8_t>{0x82},
                "rejected retained handle still consumed pending bytes");
    }
}

void test_pending_blocks_replacement_and_reentrant_retirement() {
    Fixture fixture;
    const auto binding = fixture.bind(fixture.primary); succeeds(binding, "bind publication guard");
    fixture.script.delta.outbox = {{kEffect, 1, {0x91}}}; fixture.dispatch();
    const auto pending = Access::capture(fixture.runtime, binding.value); succeeds(pending, "capture publication guard");
    auto replacement = fixture.definition;
    replacement.version = {2, 0, 0}; replacement.contract.version = replacement.version;
    const StateMigration migration{{1, 0, 0}, {2, 0, 0}, {1, 0, 0}, {1, 0, 0}, {}};
    const auto generation = fixture.runtime.generation();
    require(fixture.runtime.publish_graph(fixture.graph, fixture.compiled).code == ResultCode::kNotReady &&
        fixture.runtime.replace_definition(replacement, migration).code == ResultCode::kNotReady &&
        fixture.runtime.generation() == generation && fixture.runtime.find_instance(fixture.primary) &&
        fixture.registry.find_definition(kProducer)->version == Version{1, 0, 0},
        "pending commands were silently retired by publication/replacement");
    struct Reentry final {
        Fixture* fixture;
        const detail::FeatureOutboxBinding* binding;
        const detail::FeatureOutboxHandle* handle;
        Result shutdown{}, publication{}, replay{};
        Applied nested{};
        uint32_t calls{0};
        bool threw{false};
    } reentry{&fixture, &binding.value, &pending.value[0]};
    const auto reenter = [](void* opaque, const OutboxIntent&) noexcept {
        auto& state = *static_cast<Reentry*>(opaque); ++state.calls;
        try {
            state.shutdown = state.fixture->runtime.shutdown_execution();
            state.publication = state.fixture->runtime.publish_graph(state.fixture->graph, state.fixture->compiled);
            state.replay = Access::consume(state.fixture->runtime, *state.binding, *state.handle, apply, &state.nested);
        } catch (...) { state.threw = true; }
    };
    succeeds(Access::consume(fixture.runtime, binding.value, pending.value[0], reenter, &reentry), "consume around reentrant attempts");
    require(reentry.calls == 1 && !reentry.threw && !reentry.shutdown && !reentry.publication && !reentry.replay &&
        reentry.nested.calls == 0 && fixture.runtime.generation() == generation &&
        pending.value[0].receipt().state == Delivery::kApplied, "reentrant apply invalidated its consumption authority");
    succeeds(fixture.runtime.replace_definition(replacement, migration), "replacement after journal drains");
    require(fixture.runtime.generation() != generation && !Access::capture(fixture.runtime, binding.value),
            "replacement retained an earlier binding generation");
    for (auto& node : fixture.graph.nodes) if (node.definition_id == kProducer) node.definition_version = {2, 0, 0};
    fixture.graph.contract_digest = fixture.registry.contract_digest();
    auto compiled = FeatureGraphCompiler{}.compile(fixture.graph, fixture.registry); succeeds(compiled, "compile replacement graph");
    succeeds(fixture.runtime.publish_graph(fixture.graph, compiled.value), "publication after journal drains");
}

void test_process_completion_cancellation_and_runtime_retirement() {
    for (const bool complete : {true, false}) {
        Fixture fixture(true);
        const auto binding = fixture.bind(fixture.primary); succeeds(binding, "bind actual Process owner");
        fixture.script.delta.outbox = {{kEffect, 1, {0xa1}}};
        fixture.script.delta.process_completed = complete;
        if (complete) fixture.script.delta.next_state = "Done";
        fixture.dispatch(true);
        const auto process = fixture.runtime.process(fixture.primary); succeeds(process, "find actual Process");
        if (!complete) succeeds(fixture.runtime.cancel(process.value), "cancel Process after committed command");
        require(fixture.runtime.find_process(process.value)->status ==
                    (complete ? ProcessStatus::kCompleted : ProcessStatus::kCancelled), "Process lifecycle differs");
        const auto captured = Access::capture(fixture.runtime, binding.value);
        require(captured && captured.value.size() == 1 &&
            *captured.value[0].invocation() == fixture.script.calls[0], "Process lifecycle lost an already committed command");
        Applied observed;
        succeeds(Access::consume(fixture.runtime, binding.value, captured.value[0], apply, &observed), "consume committed Process command");
        require(observed.calls == 1, "terminal Process command was not applied exactly once");
    }
    Fixture fixture;
    const auto binding = fixture.bind(fixture.primary); succeeds(binding, "bind shutdown fixture");
    fixture.script.delta.outbox = {{kEffect, 1, {0xb1}}, {kEffect, 1, {0xb2}}}; fixture.dispatch();
    const auto captured = Access::capture(fixture.runtime, binding.value); require(captured && captured.value.size() == 2, "capture shutdown fixture");
    Applied observed;
    succeeds(Access::consume(fixture.runtime, binding.value, captured.value[0], apply, &observed), "apply before shutdown");
    succeeds(fixture.runtime.shutdown_execution(), "shutdown cancels remaining journal");
    require(captured.value[0].receipt().state == Delivery::kApplied && captured.value[1].receipt().state == Delivery::kCancelled &&
        captured.value[1].receipt().error_code == ResultCode::kCancelled &&
        captured.value[1].intent()->payload == std::vector<uint8_t>{0xb2} &&
        !Access::consume(fixture.runtime, binding.value, captured.value[1], apply, &observed) && observed.calls == 1,
        "shutdown erased, applied or replayed a retained pending command");
    std::optional<detail::FeatureOutboxHandle> retained;
    {
        Fixture dying;
        const auto owner = dying.bind(dying.primary); succeeds(owner, "bind destructor fixture");
        dying.script.delta.outbox = {{kEffect, 1, {0xc1}}}; dying.dispatch();
        const auto outputs = Access::capture(dying.runtime, owner.value); require(outputs && outputs.value.size() == 1, "capture destructor fixture");
        retained = outputs.value[0];
    }
    require(retained->receipt().state == Delivery::kCancelled && retained->receipt().error_code == ResultCode::kCancelled &&
        retained->intent()->payload == std::vector<uint8_t>{0xc1}, "Runtime destruction silently lost retained command status");
}
} // namespace

int main() try {
    test_ordered_owning_entries_and_canonical_authority();
    test_declared_effects_and_commit_atomicity();
    test_composite_descendants_keep_distinct_canonical_sources();
    test_accepted_command_survives_definition_registry_removal();
    test_async_completion_pump_appends_one_delivery_once();
    test_committed_outbox_survives_render_publication_failure();
    test_pending_budgets_are_released_by_terminal_delivery();
    test_pending_blocks_replacement_and_reentrant_retirement();
    test_process_completion_cancellation_and_runtime_retirement();
    std::cout << "Feature outbox committed journal, authority, budgets and retirement passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Feature outbox: " << error.what() << '\n';
    return 73;
}
