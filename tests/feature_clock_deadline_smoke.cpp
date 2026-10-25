/** @brief Real Runtime cross-Clock freshness and Command/Receipt deadline regressions. */
#include "concurrency.hpp"
#include "feature_contract_json.hpp"
#include "feature_graph.hpp"
#include "feature_clock_test_fixture.hpp"
#include "../Engine/Feature/private/feature_input.hpp"
#include "../Engine/Feature/private/feature_execution_control.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <limits>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace aeg;
using namespace aeg::feature;
using SteadyClock = std::chrono::steady_clock;
using Instant = SteadyClock::time_point;

constexpr ClockId kFast{"Deadline.Fast1000"}, kSlow{"Deadline.Slow10"};
constexpr FeatureDefinitionId kSender{"Deadline.Sender"}, kReceiver{"Deadline.Receiver"};
constexpr FeatureNodeId kSenderNode{"Deadline.Sender.Node"}, kReceiverNode{"Deadline.Receiver.Node"};
constexpr PortId kOutput{"Deadline.Output"}, kInput{"Deadline.Input"};
constexpr PortId kReceiptOut{"Deadline.Receipt.Out"}, kReceiptIn{"Deadline.Receipt.In"};
constexpr PortId kProbeOut{"Deadline.Probe.Out"}, kProbeIn{"Deadline.Probe.In"};
constexpr ValueSchemaId kBytes{"Deadline.Bytes.v1"};
constexpr uint64_t kShortBudgetNs = 100'000'000; // 100 Fast source periods.
constexpr uint64_t kLongBudgetNs = 1'000'000'000; // 10 Slow source periods.
constexpr uint64_t kLateWaitNs = 300'000'000;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void succeeds(const foundation::Result& result, std::string_view operation) {
    require(bool(result), std::string(operation) + ": " + result.message);
}

uint64_t elapsed_ns(Instant start, Instant end = SteadyClock::now()) {
    require(end >= start, "fixture steady time moved backwards");
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

void wait_past(Instant published) {
    succeeds(concurrency::sleep_for(kLateWaitNs), "wait beyond source-period transport deadline");
    require(elapsed_ns(published) >= kLateWaitNs,
            "fixture wait did not establish the measured late-delivery precondition");
}

std::vector<uint8_t> encode_transaction(uint64_t transaction) {
    std::vector<uint8_t> bytes(8);
    for (unsigned index = 0; index != 8; ++index)
        bytes[index] = static_cast<uint8_t>(transaction >> (index * 8));
    return bytes;
}

uint64_t decode_transaction(std::span<const uint8_t> bytes) {
    require(bytes.size() >= 8, "fixture Command correlation was not present in actual callback input/state");
    uint64_t transaction = 0;
    for (unsigned index = 0; index != 8; ++index)
        transaction |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    require(transaction != 0, "fixture Command correlation was zero");
    return transaction;
}

bool unchanged(const StateStore& before, const StateStore& after) {
    return before.version == after.version && before.commit_count == after.commit_count &&
        before.payload_revision == after.payload_revision && before.schema_version == after.schema_version &&
        before.payload == after.payload;
}

struct Results final {
    unsigned observed{0}, failed{0}, fixture_errors{0};

    void observe(std::string_view name, bool passed, const foundation::Result& result,
                 uint64_t age_ns, size_t callback_delta, uint64_t commit_delta, size_t delivered) {
        ++observed;
        if (!passed) ++failed;
        std::cout << "Clock business probe=" << name << " passed=" << passed
            << " elapsed-ns=" << age_ns << " callback-delta=" << callback_delta
            << " commit-delta=" << commit_delta << " delivered=" << delivered
            << " result-code=" << static_cast<unsigned>(result.code) << " result=" << result.message << '\n';
    }

    template <typename Function> void run(std::string_view name, Function&& function) {
        try {
            function();
        } catch (const std::exception& error) {
            ++fixture_errors;
            std::cerr << "Clock fixture failure=" << name << ": " << error.what() << '\n';
        }
    }
};

enum class SenderMode { kIdle, kProbe, kSend };
enum class ReceiverMode { kIdle, kProbe, kConsume, kReceipt };

FeatureDefinition definition(FeatureDefinitionId id, ClockId clock, FeatureDomain domain) {
    FeatureDefinition result;
    result.id = id;
    result.version = {1, 0, 0};
    result.owner_module = ModuleIdentity{"Deadline.Runtime.Fixture"};
    auto& contract = result.contract;
    contract.definition_id = id;
    contract.version = result.version;
    contract.domains = to_mask(domain);
    contract.execution = ExecutionKind::kCall;
    contract.state = StatePolicy::kInstanceState;
    contract.scope = {ScopeId{"World"}};
    contract.clock = make_periodic_clock_contract(clock, clock == kFast ? 1000u : 10u, 256,
        LateDataPolicy::kQueue, DeadlinePolicy::kObserve, 250'000, 0);
    contract.permissions.version = 1;
    return result;
}

struct Command final {
    uint64_t transaction{};
    MessageDeliveryId delivery{};
    std::vector<uint8_t> payload{};
    Instant accepted_before{}, published_after{};
};

struct Fixture final {
    FeatureRegistry registry{test::clock_test_host()};
    FeatureRuntime runtime;
    FeatureGraphDocument graph;
    CompiledFeatureGraph compiled;
    FeatureInstanceId sender{}, receiver{};
    ClockId sender_clock{}, receiver_clock{};
    EdgeKind kind{};
    SenderMode sender_mode{SenderMode::kIdle};
    ReceiverMode receiver_mode{ReceiverMode::kIdle};
    std::vector<uint8_t> outgoing;
    std::vector<std::vector<uint8_t>> consumed_inputs, receipt_inputs;
    uint64_t next_transaction{700001};
    uint64_t expected_sender_tick{0}, expected_receiver_tick{0};
    std::function<void()> on_sender_send{}, on_receiver_consume{}, on_receiver_receipt{};
    std::optional<TransportTiming> author_timing{};
    size_t sender_callbacks{0};

    Fixture(EdgeKind transport, bool fast_sender, uint32_t deadline_ticks, FeatureRuntimeOptions options = {},
            uint32_t receipt_deadline_ticks = 0, uint32_t main_capacity = 4)
        : runtime(registry, {}, std::move(options)), sender_clock(fast_sender ? kFast : kSlow),
          receiver_clock(fast_sender ? kSlow : kFast), kind(transport) {
        auto source = definition(kSender, sender_clock, FeatureDomain::kLogic);
        source.contract.ports = {
            {kOutput, PortDirection::kOutput, kBytes, 1},
            {kReceiptIn, PortDirection::kInput, kBytes, 1},
            {kProbeOut, PortDirection::kOutput, kBytes, 1},
            {kProbeIn, PortDirection::kInput, kBytes, 1}};
        source.execute = [this](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                               std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
            StateDelta delta{token.state_version};
            ++sender_callbacks;
            if (sender_mode != SenderMode::kIdle) {
                if (sender_mode == SenderMode::kSend && on_sender_send) on_sender_send();
                PortMessage message;
                message.source_port = sender_mode == SenderMode::kProbe ? kProbeOut : kOutput;
                message.schema = kBytes;
                message.schema_version = 1;
                message.source.timing = author_timing;
                message.payload = sender_mode == SenderMode::kProbe ? std::vector<uint8_t>{0xf1} : outgoing;
                if (sender_mode == SenderMode::kSend && kind == EdgeKind::kCommand) {
                    // An explicit unique correlation is part of the declared payload, so the
                    // worker recovers it from actual callback bytes rather than a Runtime side channel.
                    message.transaction_id = next_transaction++;
                    message.payload = encode_transaction(message.transaction_id);
                    message.payload.push_back(0xc4);
                }
                delta.output_messages.push_back(std::move(message));
            }
            return {foundation::Result::success(), std::move(delta)};
        };

        auto target = definition(kReceiver, receiver_clock, FeatureDomain::kStream);
        target.contract.ports = {
            {kInput, PortDirection::kInput, kBytes, 1},
            {kReceiptOut, PortDirection::kOutput, kBytes, 1},
            {kProbeOut, PortDirection::kOutput, kBytes, 1},
            {kProbeIn, PortDirection::kInput, kBytes, 1}};
        target.execute = [this](const InvocationToken& token, const StateSnapshot& state,
                               std::span<const uint8_t> input,
                               std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
            StateDelta delta{token.state_version};
            if (receiver_mode == ReceiverMode::kProbe) {
                PortMessage message;
                message.source_port = kProbeOut;
                message.schema = kBytes;
                message.schema_version = 1;
                message.payload = {0xf2};
                delta.output_messages.push_back(std::move(message));
            } else if (receiver_mode == ReceiverMode::kConsume) {
                if (on_receiver_consume) on_receiver_consume();
                consumed_inputs.emplace_back(input.begin(), input.end());
                delta.state_payload = kind == EdgeKind::kCommand
                    ? encode_transaction(decode_transaction(input)) : std::vector<uint8_t>{input.begin(), input.end()};
            } else if (receiver_mode == ReceiverMode::kReceipt) {
                if (on_receiver_receipt) on_receiver_receipt();
                receipt_inputs.emplace_back(input.begin(), input.end());
                PortMessage receipt;
                receipt.source_port = kReceiptOut;
                receipt.schema = kBytes;
                receipt.schema_version = 1;
                receipt.kind = EdgeKind::kReceipt;
                receipt.transaction_id = decode_transaction(state.payload);
                receipt.payload = encode_transaction(receipt.transaction_id);
                delta.output_messages.push_back(std::move(receipt));
                delta.state_payload = state.payload;
                delta.state_payload->push_back(0xa5); // Proves Receipt failure cannot partially commit owner state.
            }
            return {foundation::Result::success(), std::move(delta)};
        };
        succeeds(test::register_clock_test_definition(registry, std::move(source)), "register actual sender");
        succeeds(test::register_clock_test_definition(registry, std::move(target)), "register actual receiver");
        graph.asset_uuid = "fixture://clock-deadline/" + std::to_string(static_cast<unsigned>(kind));
        graph.schema_version = 1;
        graph.graph_version = 1;
        graph.contract_digest = registry.contract_digest();
        graph.nodes = {{kSenderNode, kSender, {1, 0, 0}, {}, {}},
                       {kReceiverNode, kReceiver, {1, 0, 0}, {}, {}}};
        graph.edges = {
            {kSenderNode, kOutput, kReceiverNode, kInput, kind, sender_clock, receiver_clock, 1, main_capacity, deadline_ticks},
            {kSenderNode, kProbeOut, kReceiverNode, kProbeIn, EdgeKind::kEvent, sender_clock, receiver_clock, 1, 4, 0},
            {kReceiverNode, kProbeOut, kSenderNode, kProbeIn, EdgeKind::kEvent, receiver_clock, sender_clock, 1, 4, 0}};
        if (kind == EdgeKind::kCommand)
            graph.edges.push_back({kReceiverNode, kReceiptOut, kSenderNode, kReceiptIn, EdgeKind::kReceipt,
                                   receiver_clock, sender_clock, 1, 4, receipt_deadline_ticks});
        auto candidate = FeatureGraphCompiler{}.compile(graph, registry);
        succeeds(candidate.result, "compile actual cross-Clock graph");
        succeeds(runtime.publish_graph(graph, candidate.value), "publish actual cross-Clock graph");
        compiled = std::move(candidate.value);
        for (const auto& row : compiled.provenance) {
            if (row.node_id == kSenderNode) sender = row.instance_id;
            if (row.node_id == kReceiverNode) receiver = row.instance_id;
        }
        require(sender.is_valid() && receiver.is_valid() && sender != receiver,
                "fixture did not materialize two distinct actual owners");
    }

    ~Fixture() { static_cast<void>(runtime.shutdown_execution()); }

    const FeatureInstance& owner(FeatureInstanceId id) const {
        const auto* value = runtime.find_instance(id);
        require(value != nullptr, "fixture owner disappeared");
        return *value;
    }

    void clear_caller_input() {
        sender_mode = SenderMode::kIdle;
        succeeds(runtime.dispatch_call(sender), "consume caller's actual probe/Receipt delivery");
        require(owner(sender).pending_messages.empty(), "caller did not consume exact delivered input");
    }

    void warm() {
        succeeds(runtime.advance(0.1), "advance the actual 1000 Hz and 10 Hz Clock plans");
        expected_sender_tick = sender_clock == kFast ? 100 : 1;
        expected_receiver_tick = receiver_clock == kFast ? 100 : 1;
        sender_mode = SenderMode::kProbe;
        succeeds(runtime.dispatch_call(sender), "publish sender local Clock proof");
        sender_mode = SenderMode::kIdle;
        const auto& at_receiver = owner(receiver).pending_messages;
        require(at_receiver.size() == 1 && at_receiver.front().target_port == kProbeIn &&
                    at_receiver.front().source.clock == sender_clock &&
                    at_receiver.front().timestamp_ticks == expected_sender_tick,
                "actual sender local Clock counter/frequency precondition failed");
        receiver_mode = ReceiverMode::kProbe;
        succeeds(runtime.dispatch_call(receiver), "publish receiver local Clock proof");
        receiver_mode = ReceiverMode::kIdle;
        const auto& at_sender = owner(sender).pending_messages;
        require(at_sender.size() == 1 && at_sender.front().target_port == kProbeIn &&
                    at_sender.front().source.clock == receiver_clock &&
                    at_sender.front().timestamp_ticks == expected_receiver_tick,
                "actual receiver local Clock counter/frequency precondition failed");
        clear_caller_input();
        require(owner(receiver).pending_messages.empty(), "Clock probes left old target input");
        std::cout << "Clock counters source=" << expected_sender_tick << " receiver=" << expected_receiver_tick << '\n';
    }

    foundation::Result send(std::vector<uint8_t> bytes = {}) {
        outgoing = std::move(bytes);
        sender_mode = SenderMode::kSend;
        const auto result = runtime.dispatch_call(sender);
        sender_mode = SenderMode::kIdle;
        return result;
    }

    foundation::Result consume() {
        receiver_mode = ReceiverMode::kConsume;
        const auto result = runtime.dispatch_call(receiver);
        receiver_mode = ReceiverMode::kIdle;
        return result;
    }

    foundation::Result receipt() {
        receiver_mode = ReceiverMode::kReceipt;
        const auto result = runtime.dispatch_call(receiver);
        receiver_mode = ReceiverMode::kIdle;
        return result;
    }

    Command issue_and_consume() {
        Command command;
        command.accepted_before = SteadyClock::now();
        succeeds(send(), "publish actual Command callback output");
        command.published_after = SteadyClock::now();
        const auto& pending = owner(receiver).pending_messages;
        require(pending.size() == 1 && pending.front().kind == EdgeKind::kCommand &&
                    pending.front().target_port == kInput && pending.front().source.instance == sender &&
                    pending.front().source.clock == sender_clock &&
                    pending.front().timestamp_ticks == expected_sender_tick && pending.front().delivery_id.is_valid(),
                "actual Command delivery/provenance precondition failed");
        command.transaction = pending.front().transaction_id;
        command.delivery = pending.front().delivery_id;
        command.payload = pending.front().payload;
        require(decode_transaction(command.payload) == command.transaction,
                "Bus Command transaction differed from the actual payload correlation");
        const auto count = consumed_inputs.size();
        succeeds(consume(), "consume fresh Command in the first worker invocation");
        require(consumed_inputs.size() == count + 1 && consumed_inputs.back() == command.payload &&
                    decode_transaction(owner(receiver).state.payload) == command.transaction &&
                    owner(receiver).pending_messages.empty(),
                "worker did not commit correlation from the actual Command bytes before Receipt invocation");
        return command;
    }

    void expect_receipt(uint64_t transaction) const {
        const auto& pending = owner(sender).pending_messages;
        require(pending.size() == 1 && pending.front().kind == EdgeKind::kReceipt &&
                    pending.front().target_port == kReceiptIn && pending.front().transaction_id == transaction &&
                    decode_transaction(pending.front().payload) == transaction &&
                    pending.front().source.instance == receiver && pending.front().source.clock == receiver_clock &&
                    pending.front().timestamp_ticks == expected_receiver_tick && pending.front().delivery_id.is_valid(),
                "timely Receipt did not preserve the actual transaction, endpoints and Clock provenance");
    }

    void timely_roundtrip(uint64_t budget_ns) {
        const auto command = issue_and_consume();
        const auto before = owner(receiver).state;
        const auto callbacks = receipt_inputs.size();
        const auto result = receipt();
        const auto age = elapsed_ns(command.accepted_before);
        require(age < budget_ns / 2, "timely baseline/recovery exceeded its measured timing margin");
        succeeds(result, "publish timely actual Receipt baseline/recovery");
        require(receipt_inputs.size() == callbacks + 1 && receipt_inputs.back().empty() &&
                    owner(receiver).state.commit_count == before.commit_count + 1,
                "timely Receipt did not use a distinct no-old-input invocation and one actual owner commit");
        expect_receipt(command.transaction);
        clear_caller_input();
    }
};

void delayed_capture(Results& results, EdgeKind kind, std::string_view name) {
    Fixture fixture(kind, true, 100);
    fixture.warm();
    const std::vector<uint8_t> immediate{0x11, 0x22};
    const auto immediate_before = fixture.owner(fixture.receiver).state;
    const auto immediate_start = SteadyClock::now();
    succeeds(fixture.send(immediate), "publish immediate freshness baseline");
    succeeds(fixture.consume(), "consume immediate freshness baseline");
    require(elapsed_ns(immediate_start) < kShortBudgetNs / 2 && fixture.consumed_inputs.size() == 1 &&
                fixture.consumed_inputs.back() == immediate && fixture.owner(fixture.receiver).pending_messages.empty() &&
                fixture.owner(fixture.receiver).state.payload == immediate &&
                fixture.owner(fixture.receiver).state.commit_count == immediate_before.commit_count + 1,
            "fresh cross-Clock input baseline was not valid");

    succeeds(fixture.runtime.tick(kFast), "advance source tick before the next Snapshot/Event");
    ++fixture.expected_sender_tick;
    const std::vector<uint8_t> stale{0x33, 0x44};
    succeeds(fixture.send(stale), "publish delayed actual Snapshot/Event");
    const auto published = SteadyClock::now();
    require(fixture.owner(fixture.receiver).pending_messages.size() == 1,
            "delayed callback output did not produce one actual delivery");
    const auto delivery = fixture.owner(fixture.receiver).pending_messages.front().delivery_id;
    require(fixture.owner(fixture.receiver).pending_messages.size() == 1 && delivery.is_valid() &&
                fixture.owner(fixture.receiver).pending_messages.front().source.clock == kFast &&
                fixture.owner(fixture.receiver).pending_messages.front().timestamp_ticks == fixture.expected_sender_tick,
            "delayed message did not originate from the actual Fast owner");
    wait_past(published);
    const auto before = fixture.owner(fixture.receiver).state;
    const auto sender_before = fixture.owner(fixture.sender).state;
    const auto callbacks = fixture.consumed_inputs.size();
    const auto rejected = fixture.consume();
    const auto& after = fixture.owner(fixture.receiver);
    const auto age = elapsed_ns(published);
    const bool retired = std::none_of(after.pending_messages.begin(), after.pending_messages.end(),
        [&](const auto& message) { return message.delivery_id == delivery; });
    const bool passed = !rejected && rejected.code == foundation::ResultCode::kTimeout &&
        fixture.consumed_inputs.size() == callbacks && unchanged(before, after.state) &&
        unchanged(sender_before, fixture.owner(fixture.sender).state) && retired;
    results.observe(name, passed, rejected, age, fixture.consumed_inputs.size() - callbacks,
                    after.state.commit_count - before.commit_count, after.pending_messages.size());

    succeeds(fixture.runtime.tick(kFast), "advance source tick for fresh input recovery");
    ++fixture.expected_sender_tick;
    const std::vector<uint8_t> recovered{0x55, 0x66};
    const auto recovery_start = SteadyClock::now();
    succeeds(fixture.send(recovered), "publish fresh replacement after delayed input");
    const auto recovery_before = fixture.owner(fixture.receiver).state;
    const auto recovery_callbacks = fixture.consumed_inputs.size();
    succeeds(fixture.consume(), "consume actual fresh input recovery");
    require(elapsed_ns(recovery_start) < kShortBudgetNs / 2 &&
                fixture.consumed_inputs.size() == recovery_callbacks + 1 && fixture.consumed_inputs.back() == recovered &&
                fixture.owner(fixture.receiver).pending_messages.empty() &&
                fixture.owner(fixture.receiver).state.payload == recovered &&
                fixture.owner(fixture.receiver).state.commit_count == recovery_before.commit_count + 1,
            "fresh cross-Clock recovery did not consume the actual new delivery");
}

void late_receipt(Results& results) {
    Fixture fixture(EdgeKind::kCommand, true, 100);
    fixture.warm();
    fixture.timely_roundtrip(kShortBudgetNs);
    const auto command = fixture.issue_and_consume();
    require(elapsed_ns(command.accepted_before) < kShortBudgetNs / 2,
            "Command was already old when the first worker invocation consumed it");
    wait_past(command.published_after);
    require(fixture.owner(fixture.receiver).pending_messages.empty(),
            "late Receipt oracle was contaminated by old pending Command input");
    const auto before = fixture.owner(fixture.receiver).state;
    const auto caller_before = fixture.owner(fixture.sender).state;
    const auto callbacks = fixture.receipt_inputs.size();
    const auto rejected = fixture.receipt();
    const auto age = elapsed_ns(command.published_after);
    const auto& after = fixture.owner(fixture.receiver).state;
    const auto delivered = fixture.owner(fixture.sender).pending_messages.size();
    // Expiry cleanup may have removed the transaction already; either rejection must
    // preserve owner state and must not form a successful Receipt delivery.
    const bool expiry_rejection = !rejected && (rejected.code == foundation::ResultCode::kTimeout ||
        rejected.code == foundation::ResultCode::kNotFound || rejected.code == foundation::ResultCode::kInvalidState);
    const bool passed = expiry_rejection && fixture.receipt_inputs.size() == callbacks + 1 &&
        fixture.receipt_inputs.back().empty() && unchanged(before, after) &&
        unchanged(caller_before, fixture.owner(fixture.sender).state) && delivered == 0;
    results.observe("late-receipt-fast100-to-slow1", passed, rejected, age,
                    fixture.receipt_inputs.size() - callbacks, after.commit_count - before.commit_count, delivered);
    // Consume an incorrectly accepted old Receipt through the real caller too, so
    // the independent recovery oracle and the remaining business cases still run on RED.
    fixture.clear_caller_input();
    fixture.timely_roundtrip(kShortBudgetNs);
}

void timely_receipt_with_larger_receiver_tick(Results& results) {
    Fixture fixture(EdgeKind::kCommand, false, 10);
    // An actual successful pair before warming is the positive baseline for these endpoints.
    fixture.timely_roundtrip(kLongBudgetNs);
    fixture.warm();
    const auto command = fixture.issue_and_consume();
    const auto before = fixture.owner(fixture.receiver).state;
    const auto caller_before = fixture.owner(fixture.sender).state;
    const auto callbacks = fixture.receipt_inputs.size();
    const auto accepted = fixture.receipt();
    const auto age = elapsed_ns(command.accepted_before);
    require(age < kLongBudgetNs / 2, "timely Receipt did not meet its measured one-second source-period budget");
    const auto& after = fixture.owner(fixture.receiver).state;
    const auto delivered = fixture.owner(fixture.sender).pending_messages.size();
    bool matching_receipt = false;
    if (accepted) {
        fixture.expect_receipt(command.transaction);
        matching_receipt = true;
    }
    const bool passed = accepted && matching_receipt && fixture.receipt_inputs.size() == callbacks + 1 &&
        fixture.receipt_inputs.back().empty() && after.commit_count == before.commit_count + 1 &&
        after.version.value() == before.version.value() + 1 &&
        unchanged(caller_before, fixture.owner(fixture.sender).state);
    results.observe("timely-receipt-slow1-to-fast100", passed, accepted, age,
                    fixture.receipt_inputs.size() - callbacks, after.commit_count - before.commit_count, delivered);
    fixture.clear_caller_input();
    // Both clocks remain real Runtime clocks. Align the diagnostic counters for a
    // new transaction so the old bug cannot hide the normal recovery path on RED.
    for (unsigned tick = 1; tick != 100; ++tick)
        succeeds(fixture.runtime.tick(kSlow), "advance actual Slow Clock for recovery");
    fixture.expected_sender_tick = 100;
    fixture.timely_roundtrip(kLongBudgetNs);
}

void continuous_phase_from_contract_json(Results& results) {
    constexpr ClockId phase_clock{"Deadline.Phase60"};
    constexpr FeatureDefinitionId phase_definition{"Deadline.Phase.Continuous"};
    constexpr FeatureNodeId phase_node{"Deadline.Phase.Node"};
    FeatureRegistry registry{test::clock_test_host()};
    auto feature = definition(phase_definition, phase_clock, FeatureDomain::kLogic);
    feature.contract.execution = ExecutionKind::kContinuous;
    feature.contract.clock.frequency_hz = 60;
    const auto encoded = encode_feature_contract_json(feature.contract);
    succeeds(encoded.result, "encode phase fixture with the one contract JSON codec");
    auto json = nlohmann::json::parse(encoded.value);
    auto& clock = json.at("clock");
    require(clock.is_object(), "phase fixture's canonical Clock JSON was not an object");
    // Object assignment replaces each key exactly once; it does not append a
    // duplicate Clock/key that an old or future decoder could interpret differently.
    clock["capability_version"] = {{"major", 1}, {"minor", 0}, {"patch", 0}};
    clock["timing"] = {{"phase_ns", 5'000'000}, {"relative_deadline_ns", 16'666'667},
                       {"deadline_policy", 1}}; // Formal DeadlinePolicy::kObserve.
    clock["work_budget_ns"] = 250'000;
    const auto decoded = decode_feature_contract_json(json.dump());
    succeeds(decoded.result, "decode formal phase declaration through the existing contract codec");
    require(decoded.value.definition_id == phase_definition && decoded.value.clock.id == phase_clock &&
                decoded.value.clock.frequency_hz == 60 && decoded.value.execution == ExecutionKind::kContinuous,
            "phase JSON roundtrip changed the actual Continuous owner identity/Clock");
    feature.contract = decoded.value;
    size_t callbacks = 0;
    feature.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                          std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
        ++callbacks;
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{0x60, 0x05};
        return {foundation::Result::success(), std::move(delta)};
    };
    succeeds(test::register_clock_test_definition(registry, std::move(feature)), "register actual phase Continuous owner");
    FeatureGraphDocument graph;
    graph.asset_uuid = "fixture://clock-deadline/phase";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    graph.nodes = {{phase_node, phase_definition, {1, 0, 0}, {}, {}}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    succeeds(compiled.result, "compile actual phase graph");
    FeatureRuntime runtime(registry);
    succeeds(runtime.publish_graph(graph, compiled.value), "publish actual phase graph");
    require(compiled.value.provenance.size() == 1, "phase graph did not materialize exactly one owner");
    const auto owner_id = compiled.value.provenance.front().instance_id;
    const auto* owner = runtime.find_instance(owner_id);
    require(owner != nullptr, "phase graph owner was not live");
    const auto before = owner->state;
    succeeds(runtime.advance(0.020), "advance phase Clock to 20 ms before its first release");
    owner = runtime.find_instance(owner_id);
    require(owner != nullptr, "phase owner disappeared before first release");
    const auto callbacks_at_20ms = callbacks;
    const auto state_at_20ms = owner->state;
    // First release is ceil(1 second / 60) + 5 ms = 21,666,667 ns.
    const auto release = runtime.advance(0.001666667);
    succeeds(release, "advance phase Clock to its actual first period plus phase");
    owner = runtime.find_instance(owner_id);
    require(owner != nullptr, "phase owner disappeared at first release");
    const bool passed = callbacks_at_20ms == 0 && unchanged(before, state_at_20ms) && callbacks == 1 &&
        owner->state.commit_count == before.commit_count + 1 &&
        owner->state.payload == std::vector<uint8_t>{0x60, 0x05};
    std::cout << "Clock phase logical-ns=21666667 callbacks-at-20ms=" << callbacks_at_20ms
        << " callbacks-at-release=" << callbacks << " phase-ns=5000000 frequency-hz=60\n";
    results.observe("continuous-phase-first-period-plus-5ms", passed, release, 21'666'667,
                    callbacks_at_20ms, state_at_20ms.commit_count - before.commit_count, 0);
    succeeds(runtime.advance(0.016666667), "advance one more actual phase Clock period");
    owner = runtime.find_instance(owner_id);
    require(owner != nullptr && callbacks == 2 && owner->state.commit_count == before.commit_count + 2,
            "phase Clock did not preserve one callback/commit per subsequent real period");
}

FeatureDefinition timed_definition(std::string_view name, ExecutionKind execution, DeadlinePolicy policy) {
    auto value = definition(FeatureDefinitionId(name), kFast, FeatureDomain::kLogic);
    value.contract.execution = execution;
    value.contract.clock.id = ClockId(value.id.value());
    value.contract.clock.timing->relative_deadline_ns = 100;
    value.contract.clock.timing->deadline_policy = policy;
    value.contract.clock.work_budget_ns = 25;
    value.contract.ports = {{kInput, PortDirection::kInput, kBytes, 1}};
    return value;
}

struct VirtualHost final {
    FeatureRegistry registry{test::clock_test_host()};
    FeatureRuntime runtime;
    FeatureInstanceId owner{};
    ClockId clock{};
    VirtualHost(std::atomic<uint64_t>& now, FeatureDefinition value)
        : runtime(registry, {}, FeatureRuntimeOptions{[&now] { return now.load(); }}), clock(value.contract.clock.id) {
        const auto id = value.id;
        succeeds(test::register_clock_test_definition(registry, std::move(value)), "register virtual-time owner");
        const auto expanded = registry.expand(id, {});
        succeeds(expanded.result, "expand virtual-time owner");
        const auto active = runtime.activate(expanded.value, ScopeId{"World"});
        succeeds(active.result, "activate virtual-time owner");
        owner = active.value;
    }
    const StateStore& state() const {
        const auto* value = runtime.find_instance(owner);
        require(value != nullptr, "virtual-time owner disappeared");
        return value->state;
    }
};

foundation::ResultValue<StateDelta> state_delta(const InvocationToken& token) {
    StateDelta delta{token.state_version};
    delta.state_payload = std::vector<uint8_t>{0x31};
    return {foundation::Result::success(), std::move(delta)};
}

void virtual_execution_boundaries() {
    for (const auto policy : {DeadlinePolicy::kRejectCommit, DeadlinePolicy::kObserve}) {
        for (const uint64_t finish : {100u, 101u}) {
            std::atomic<uint64_t> now{0};
            auto feature = timed_definition("Deadline.Virtual.Boundary", ExecutionKind::kCall, policy);
            feature.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
                now.store(finish);
                return state_delta(token);
            };
            VirtualHost host(now, std::move(feature));
            const auto before = host.state();
            const auto result = host.runtime.dispatch_call(host.owner);
            const bool accepted = finish == 100 || policy == DeadlinePolicy::kObserve;
            require(bool(result) == accepted && (accepted || result.code == foundation::ResultCode::kTimeout),
                    "virtual exact/overdue execution deadline boundary was wrong");
            require(accepted ? host.state().commit_count == before.commit_count + 1 : unchanged(before, host.state()),
                    "virtual deadline rejection partially committed state");
            const auto observation = host.runtime.inspect_invocations(host.owner);
            succeeds(observation.result, "inspect virtual execution timing");
            require(observation.value.timing && observation.value.timing->release.origin.nanoseconds == 0 &&
                        observation.value.timing->callback_started && observation.value.timing->callback_finished &&
                        observation.value.timing->callback_started->nanoseconds == 0 &&
                        observation.value.timing->callback_finished->nanoseconds == finish &&
                        observation.value.timing->deadline_exceeded == (finish > 100) &&
                        observation.value.timing->budget_exceeded,
                    "virtual callback interval/budget/deadline observations were not recorded");
        }
    }
    // Schema validation runs after callback completion and must not bypass the final gate.
    std::atomic<uint64_t> now{0};
    auto feature = timed_definition("Deadline.Virtual.Staging", ExecutionKind::kCall, DeadlinePolicy::kRejectCommit);
    feature.execute = [](const InvocationToken& token, const StateSnapshot&, auto, auto) { return state_delta(token); };
    feature.validate_state_payload = [&](auto) { now.store(101); return foundation::Result::success(); };
    VirtualHost host(now, std::move(feature));
    const auto before = host.state();
    const auto rejected = host.runtime.dispatch_call(host.owner);
    require(!rejected && rejected.code == foundation::ResultCode::kTimeout && unchanged(before, host.state()),
            "schema/staging delay escaped the final owner deadline gate");
}

void virtual_transport_boundaries() {
    std::atomic<uint64_t> now{0};
    Fixture fixture(EdgeKind::kSnapshot, true, 1, FeatureRuntimeOptions{[&] { return now.load(); }});
    succeeds(fixture.send({1}), "publish zero-origin virtual Snapshot");
    require(fixture.owner(fixture.receiver).pending_messages.size() == 1, "zero-origin Snapshot was not delivered");
    const auto original = fixture.owner(fixture.receiver).pending_messages.front();
    require(original.source.timing && original.source.timing->expires_at && original.source.timing->origin.nanoseconds == 0 &&
                original.source.timing->expires_at->nanoseconds == 1'000'000,
            "zero instant or Fast source-period expiry was lost");
    now.store(1'000'000);
    succeeds(fixture.consume(), "consume at exact virtual transport expiry");
    succeeds(fixture.runtime.tick(kFast), "advance diagnostic source counter for replacement");
    succeeds(fixture.send({2}), "publish virtual expired Snapshot");
    const auto before = fixture.owner(fixture.receiver).state;
    const auto callbacks = fixture.consumed_inputs.size();
    now.store(2'000'001);
    const auto rejected = fixture.consume();
    require(!rejected && rejected.code == foundation::ResultCode::kTimeout &&
                fixture.consumed_inputs.size() == callbacks && unchanged(before, fixture.owner(fixture.receiver).state) &&
                fixture.owner(fixture.receiver).pending_messages.empty(),
            "virtual expired Snapshot entered callback, committed, or retained its exact delivery");
    succeeds(fixture.runtime.tick(kFast), "advance diagnostic source for virtual recovery");
    succeeds(fixture.send({3}), "publish virtual fresh recovery");
    succeeds(fixture.consume(), "consume virtual fresh recovery");
    require(fixture.consumed_inputs.back() == std::vector<uint8_t>{3}, "virtual Snapshot recovery lost fresh bytes");

    const auto domain = original.source.timing->origin.domain;
    require(!transport_add({domain, (std::numeric_limits<uint64_t>::max)()}, {1}),
            "transport expiry addition overflowed silently");
    require(!validate_transport_timing({{RuntimeGeneration(domain.value() + 1), 0}, {}}, {domain, 1}) &&
                !validate_transport_timing({{domain, 2}, {}}, {domain, 1}),
            "cross-domain or future transport provenance was accepted");
    now.store(1); // A host fault must not mutate the live owner/deliveries.
    const auto sender_before = fixture.owner(fixture.sender).state;
    const auto backwards = fixture.send({4});
    require(!backwards && backwards.code == foundation::ResultCode::kInvalidState &&
                unchanged(sender_before, fixture.owner(fixture.sender).state),
            "backwards host time was accepted or partially committed");
}

void virtual_defer_and_retry() {
    std::atomic<uint64_t> now{0};
    size_t callbacks = 0;
    auto feature = timed_definition("Deadline.Virtual.Defer", ExecutionKind::kContinuous, DeadlinePolicy::kRejectCommit);
    feature.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
        ++callbacks; return state_delta(token);
    };
    VirtualHost host(now, std::move(feature));
    bool ready = false;
    auto binding = detail::FeatureInputAccess::bind(host.runtime, host.owner, kInput, kBytes, 1,
        [&](const detail::FeatureInputContext&) -> foundation::ResultValue<detail::FeatureInputProjection> {
            return {foundation::Result::success(), {ready, {0x51}}};
        });
    succeeds(binding.result, "bind actual deferred readiness projector");
    succeeds(host.runtime.tick(host.clock), "release not-ready owner at virtual zero");
    now.store(101);
    ready = true;
    const auto expired = host.runtime.pump_completions();
    require(!expired && expired.code == foundation::ResultCode::kTimeout && callbacks == 0 &&
                host.state().commit_count == 0, "ready-deferred work renewed its original release deadline");
    succeeds(host.runtime.tick(host.clock), "fresh release after deferred expiry");
    require(callbacks == 1 && host.state().commit_count == 1, "deferred expiry prevented a fresh release recovery");

    for (const uint32_t backoff : {0u, 1u}) {
        now.store(0);
        size_t attempts = 0;
        auto retry = timed_definition("Deadline.Virtual.Retry", ExecutionKind::kCall, DeadlinePolicy::kRejectCommit);
        retry.contract.failure = FailurePolicy::kRetry;
        retry.contract.retry.max_attempts = 3;
        retry.contract.retry.backoff_ticks = backoff;
        retry.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
            if (++attempts == 1) {
                now.store(101);
                return foundation::ResultValue<StateDelta>{foundation::Result::failure(
                    foundation::ResultCode::kUnknown, "deliberate retry fixture failure"), {}};
            }
            return state_delta(token);
        };
        VirtualHost retry_host(now, std::move(retry));
        const auto first = retry_host.runtime.dispatch_call(retry_host.owner);
        const auto result = backoff == 0 ? first : retry_host.runtime.tick(retry_host.clock);
        if (backoff != 0) succeeds(first, "retain clock-owned backoff retry");
        require(!result && result.code == foundation::ResultCode::kTimeout && attempts == 1 &&
                    retry_host.state().commit_count == 0 && !retry_host.runtime.find_instance(retry_host.owner)->retry_pending,
                "retry/backoff renewed an expired release or entered its stale callback");
        succeeds(retry_host.runtime.dispatch_call(retry_host.owner), "fresh invocation after retry expiry");
        require(attempts == 2 && retry_host.state().commit_count == 1, "retry expiry prevented fresh recovery");
    }
}

struct CompleteOnExit final {
    concurrency::CompletionSource& release;
    ~CompleteOnExit() { static_cast<void>(release.complete()); }
};

foundation::Result drain_virtual(FeatureRuntime& runtime) {
    auto result = foundation::Result::success();
    for (unsigned spin = 0; spin != 5000 && runtime.pending_execution_count() != 0; ++spin) {
        const auto pumped = runtime.pump_completions();
        if (!pumped && result) result = pumped;
        succeeds(concurrency::sleep_for(100'000), "yield to actual virtual-time worker");
    }
    require(runtime.pending_execution_count() == 0, "virtual-time worker did not retire");
    return result;
}

void virtual_async_queue_and_backlog() {
    std::atomic<uint64_t> now{0};
    concurrency::CompletionSource entered, release;
    auto first = timed_definition("Deadline.Virtual.Blocker", ExecutionKind::kCall, DeadlinePolicy::kObserve);
    first.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
        static_cast<void>(entered.complete());
        succeeds(release.completion().wait(2'000'000'000), "release blocking virtual-time callback");
        return state_delta(token);
    };
    VirtualHost host(now, std::move(first));
    CompleteOnExit release_on_exit{release};
    size_t queued_callbacks = 0;
    auto queued = timed_definition("Deadline.Virtual.Queued", ExecutionKind::kCall, DeadlinePolicy::kRejectCommit);
    const auto queued_id = queued.id;
    queued.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
        ++queued_callbacks; return state_delta(token);
    };
    succeeds(test::register_clock_test_definition(host.registry, std::move(queued)), "register actual queued worker owner");
    const auto expanded = host.registry.expand(queued_id, {});
    succeeds(expanded.result, "expand queued owner");
    const auto active = host.runtime.activate(expanded.value, ScopeId{"World"});
    succeeds(active.result, "activate queued owner");
    succeeds(host.runtime.enable_async_execution(), "enable virtual-time actual async workers");
    succeeds(host.runtime.dispatch_call(host.owner), "submit blocking worker");
    succeeds(entered.completion().wait(2'000'000'000), "wait for actual first worker entry");
    succeeds(host.runtime.dispatch_call(active.value), "submit actual queued second owner");
    require(host.runtime.pending_execution_count() == 2, "queued start fixture did not own two actual issued tasks");
    now.store(101);
    static_cast<void>(release.complete());
    const auto expired = drain_virtual(host.runtime);
    require(!expired && expired.code == foundation::ResultCode::kTimeout && queued_callbacks == 0 &&
                host.runtime.find_instance(active.value)->state.commit_count == 0 && host.state().commit_count == 1,
            "worker start did not reject queued overdue work or Observe blocker failed to commit");

    std::atomic<uint64_t> backlog_now{0};
    concurrency::CompletionSource backlog_entered, backlog_release;
    std::atomic<unsigned> callbacks{0};
    auto automatic = timed_definition("Deadline.Virtual.Backlog", ExecutionKind::kContinuous, DeadlinePolicy::kObserve);
    automatic.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
        if (++callbacks == 1) {
            static_cast<void>(backlog_entered.complete());
            succeeds(backlog_release.completion().wait(2'000'000'000), "release backlog fixture");
        }
        return state_delta(token);
    };
    VirtualHost backlog_host(backlog_now, std::move(automatic));
    CompleteOnExit backlog_exit{backlog_release};
    succeeds(backlog_host.runtime.enable_async_execution(), "enable backlog workers");
    succeeds(backlog_host.runtime.tick(backlog_host.clock), "issue first automatic release");
    succeeds(backlog_entered.completion().wait(2'000'000'000), "wait first backlog callback");
    backlog_now.store(10);
    succeeds(backlog_host.runtime.tick(backlog_host.clock), "queue real busy Clock release at virtual 10");
    backlog_now.store(200);
    static_cast<void>(backlog_release.complete());
    succeeds(drain_virtual(backlog_host.runtime), "drain original backlog release under Observe");
    const auto timing = backlog_host.runtime.inspect_invocations(backlog_host.owner);
    succeeds(timing.result, "inspect backlog original release");
    require(callbacks == 2 && backlog_host.state().commit_count == 2 && timing.value.timing &&
                timing.value.timing->release.origin.nanoseconds == 10 && timing.value.timing->deadline_exceeded &&
                timing.value.timing->callback_started &&
                timing.value.timing->callback_started->nanoseconds == 200,
            "busy Queue refreshed its captured release or confused queue wait with callback time");
}

void virtual_output_origin_and_command_expiry() {
    std::atomic<uint64_t> now{0};
    Fixture origin(EdgeKind::kSnapshot, true, 1, FeatureRuntimeOptions{[&] { return now.load(); }});
    origin.author_timing = TransportTiming{{RuntimeGeneration(123456), (std::numeric_limits<uint64_t>::max)()}, {}};
    succeeds(origin.send({0x41}), "publish with an author's forged authority stamp");
    require(origin.owner(origin.receiver).pending_messages.size() == 1, "forgery baseline was not actually delivered");
    const auto& stamp = origin.owner(origin.receiver).pending_messages.front().source;
    require(stamp.timing && stamp.timing->expires_at && stamp.timing->origin.nanoseconds == 0 &&
                stamp.timing->origin.domain != RuntimeGeneration(123456) &&
                stamp.timing->expires_at->nanoseconds == 1'000'000,
            "author supplied time was retained as Runtime authority");
    succeeds(origin.consume(), "consume Runtime-stamped origin baseline");
    now.store(1'000'000);
    const auto before = origin.owner(origin.sender).state;
    origin.on_sender_send = [&] { now.store(2'000'001); };
    const auto expired = origin.send({0x42});
    require(!expired && expired.code == foundation::ResultCode::kTimeout &&
                unchanged(before, origin.owner(origin.sender).state) && origin.owner(origin.receiver).pending_messages.empty(),
            "long callback output refreshed origin or partially published state/transport");

    std::atomic<uint64_t> command_now{0};
    Fixture command(EdgeKind::kCommand, true, 100, FeatureRuntimeOptions{[&] { return command_now.load(); }});
    const auto exact = command.issue_and_consume();
    command_now.store(100'000'000);
    succeeds(command.receipt(), "accept Receipt at original Command exact expiry");
    command.expect_receipt(exact.transaction);
    command.clear_caller_input();
    const auto late = command.issue_and_consume();
    const auto worker_before = command.owner(command.receiver).state;
    command_now.store(200'000'001);
    const auto refused = command.receipt();
    require(!refused && refused.code == foundation::ResultCode::kTimeout &&
                unchanged(worker_before, command.owner(command.receiver).state) &&
                command.owner(command.sender).pending_messages.empty() && late.transaction != exact.transaction,
            "Receipt did not use the original same-domain Command expiry");
    // Capacity bounds pending transactions independently of already consumed input queues.
    std::atomic<uint64_t> capacity_now{0};
    Fixture bounded(EdgeKind::kCommand, true, 100, FeatureRuntimeOptions{[&] { return capacity_now.load(); }});
    for (unsigned index = 0; index != 4; ++index) static_cast<void>(bounded.issue_and_consume());
    const auto bounded_before = bounded.owner(bounded.sender).state;
    require(!bounded.send() && unchanged(bounded_before, bounded.owner(bounded.sender).state) &&
                bounded.owner(bounded.receiver).pending_messages.empty(),
            "consuming Command input bypassed the pending transaction route capacity");
    capacity_now.store(100'000'001);
    const auto recovery = bounded.issue_and_consume();
    succeeds(bounded.receipt(), "expired transaction cleanup permits a fresh Command/Receipt");
    bounded.expect_receipt(recovery.transaction);
}

void virtual_inflight_snapshot_preserves_replacement() {
    std::atomic<uint64_t> now{0};
    concurrency::CompletionSource entered, release;
    std::atomic<unsigned> fresh_producer_entries{0};
    Fixture fixture(EdgeKind::kSnapshot, true, 1, FeatureRuntimeOptions{[&] { return now.load(); }}, 0, 1);
    require(fixture.graph.edges.front().kind == EdgeKind::kSnapshot && fixture.graph.edges.front().capacity == 1,
            "in-flight fixture did not publish an actual latest-value Snapshot route");
    succeeds(fixture.send({0x61}), "publish old Snapshot before async capture");
    require(fixture.owner(fixture.receiver).pending_messages.size() == 1, "old in-flight Snapshot was not delivered");
    const auto old_delivery = fixture.owner(fixture.receiver).pending_messages.front().delivery_id;
    fixture.on_receiver_consume = [&] {
        static_cast<void>(entered.complete());
        succeeds(release.completion().wait(2'000'000'000), "release old Snapshot callback");
    };
    succeeds(fixture.runtime.enable_async_execution(), "enable actual async Snapshot consumer");
    CompleteOnExit release_on_exit{release};
    const auto before = fixture.owner(fixture.receiver).state;
    fixture.receiver_mode = ReceiverMode::kConsume;
    succeeds(fixture.runtime.dispatch_call(fixture.receiver), "submit actual old Snapshot consumer");
    succeeds(entered.completion().wait(2'000'000'000), "wait for actual captured Snapshot callback");
    now.store(2'000'000);
    fixture.on_sender_send = [&] { ++fresh_producer_entries; };
    fixture.sender_mode = SenderMode::kSend;
    fixture.outgoing = {0x62};
    succeeds(fixture.runtime.dispatch_call(fixture.sender), "submit fresh Snapshot while old consumer is in flight");
    for (unsigned spin = 0; spin != 5000; ++spin) {
        const auto& pending = fixture.owner(fixture.receiver).pending_messages;
        if (pending.size() != 1 || pending.front().delivery_id != old_delivery) break;
        succeeds(detail::FeatureExecutionControl::drain_issued_completions(fixture.runtime), "commit fresh producer independently");
        succeeds(concurrency::sleep_for(100'000), "yield for fresh producer callback");
    }
    const auto replacement = fixture.owner(fixture.receiver).pending_messages;
    const auto producer = fixture.runtime.inspect_invocations(fixture.sender);
    succeeds(producer.result, "inspect actual fresh Snapshot producer attempt");
    std::cout << "Clock in-flight Snapshot old-delivery=" << old_delivery.value()
        << " producer-entries=" << fresh_producer_entries.load()
        << " producer-commits=" << fixture.owner(fixture.sender).state.commit_count
        << " producer-open=" << producer.value.open_attempts.size()
        << " producer-failed=" << bool(producer.value.last_failed)
        << " producer-failure-code=" << (producer.value.last_failed ? static_cast<unsigned>(producer.value.last_failed->result.code) : 0)
        << " target-pending=" << replacement.size()
        << " target-delivery=" << (replacement.empty() ? 0 : replacement.front().delivery_id.value())
        << " target-first-byte=" << (replacement.empty() || replacement.front().payload.empty() ? 0 : replacement.front().payload.front())
        << " worker-pending=" << fixture.runtime.pending_execution_count() << '\n';
    require(replacement.size() == 1 && replacement.front().delivery_id != old_delivery &&
                replacement.front().payload == std::vector<uint8_t>{0x62},
            "actual in-flight fixture did not receive a fresh Snapshot replacement");
    static_cast<void>(release.complete());
    const auto rejected = drain_virtual(fixture.runtime);
    fixture.sender_mode = SenderMode::kIdle;
    fixture.receiver_mode = ReceiverMode::kIdle;
    fixture.on_receiver_consume = {};
    require(!rejected && rejected.code == foundation::ResultCode::kTimeout &&
                unchanged(before, fixture.owner(fixture.receiver).state) &&
                fixture.owner(fixture.receiver).pending_messages.size() == 1 &&
                fixture.owner(fixture.receiver).pending_messages.front().delivery_id == replacement.front().delivery_id,
            "stale in-flight consumer committed or retired the newer exact Snapshot delivery");
    fixture.receiver_mode = ReceiverMode::kConsume;
    succeeds(fixture.runtime.dispatch_call(fixture.receiver), "submit actual fresh replacement consumer");
    succeeds(drain_virtual(fixture.runtime), "commit fresh replacement consumer recovery");
    fixture.receiver_mode = ReceiverMode::kIdle;
    require(fixture.owner(fixture.receiver).state.payload == std::vector<uint8_t>{0x62} &&
                fixture.owner(fixture.receiver).state.commit_count == before.commit_count + 1 &&
                fixture.owner(fixture.receiver).pending_messages.empty(),
            "new Snapshot delivery could not recover after stale in-flight retirement");
}

void virtual_time_faults_and_reset() {
    std::atomic<uint64_t> now{7};
    auto feature = timed_definition("Deadline.Virtual.Reset", ExecutionKind::kCall, DeadlinePolicy::kObserve);
    const auto feature_id = feature.id;
    feature.execute = [](const InvocationToken& token, const StateSnapshot&, auto, auto) { return state_delta(token); };
    VirtualHost host(now, feature);
    succeeds(host.runtime.dispatch_call(host.owner), "accept original host time before reset");
    const auto original = host.runtime.inspect_invocations(host.owner);
    succeeds(original.result, "inspect original host time domain");
    require(original.value.timing.has_value(), "original callback had no time provenance");
    auto reset = host.runtime.make_reset_runtime();
    const auto expanded = host.registry.expand(feature_id, {});
    succeeds(expanded.result, "expand owner into a reset Runtime");
    const auto active = reset->activate(expanded.value, ScopeId{"World"});
    succeeds(active.result, "activate reset Runtime owner");
    succeeds(reset->dispatch_call(active.value), "accept inherited reset source");
    const auto inherited = reset->inspect_invocations(active.value);
    succeeds(inherited.result, "inspect reset source domain");
    require(inherited.value.timing && inherited.value.timing->release.origin == original.value.timing->release.origin,
            "reset Runtime silently replaced the host time source, domain or origin");
    now.store(6);
    const auto backwards = reset->dispatch_call(active.value);
    require(!backwards && backwards.code == foundation::ResultCode::kInvalidState &&
                reset->find_instance(active.value)->state.commit_count == 1,
            "reset Runtime discarded the shared monotonic watermark");
    now.store(8);
    succeeds(reset->dispatch_call(active.value), "recover shared source after a rejected regression");

    std::atomic<uint64_t> overflowing_now{(std::numeric_limits<uint64_t>::max)() - 50};
    VirtualHost overflowing(overflowing_now, feature);
    const auto overflow = overflowing.runtime.dispatch_call(overflowing.owner);
    require(!overflow && overflow.code == foundation::ResultCode::kInvalidArgument && overflowing.state().commit_count == 0,
            "execution deadline overflow issued or committed work");

    FeatureRegistry registry{test::clock_test_host()};
    succeeds(test::register_clock_test_definition(registry, feature), "register throwing-clock fixture");
    bool throw_time = true;
    FeatureRuntime throwing(registry, {}, FeatureRuntimeOptions{[&]() -> uint64_t {
        if (throw_time) throw std::runtime_error("deliberate host time failure");
        return 0;
    }});
    const auto tree = registry.expand(feature_id, {});
    succeeds(tree.result, "expand throwing-clock fixture");
    const auto owner = throwing.activate(tree.value, ScopeId{"World"});
    succeeds(owner.result, "activate throwing-clock fixture");
    const auto exception = throwing.dispatch_call(owner.value);
    require(!exception && exception.code == foundation::ResultCode::kUnknown &&
                throwing.find_instance(owner.value)->state.commit_count == 0,
            "time-source exception escaped or partially committed work");
    throw_time = false;
    succeeds(throwing.dispatch_call(owner.value), "recover from a typed source exception at legitimate zero time");
}

void virtual_composite_author_period() {
    std::atomic<uint64_t> now{0};
    FeatureRegistry registry{test::clock_test_host()};
    auto child = definition(FeatureDefinitionId{"Deadline.Composite.Child"}, kFast, FeatureDomain::kLogic);
    child.contract.ports = {{kOutput, PortDirection::kOutput, kBytes, 1}};
    child.execute = [](const InvocationToken& token, const StateSnapshot&, auto, auto) {
        StateDelta delta{token.state_version};
        PortMessage message;
        message.source_port = kOutput; message.schema = kBytes; message.schema_version = 1; message.payload = {0x73};
        delta.output_messages.push_back(std::move(message));
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    auto parent = definition(FeatureDefinitionId{"Deadline.Composite.Parent"}, ClockId{"Deadline.Composite60"}, FeatureDomain::kLogic);
    parent.contract.structure = FeatureStructure::kComposite;
    parent.contract.state = StatePolicy::kStateless;
    parent.contract.clock = make_periodic_clock_contract(parent.contract.clock.id, 60, 256,
        LateDataPolicy::kQueue, DeadlinePolicy::kObserve, 0, 0);
    parent.contract.ports = child.contract.ports;
    parent.contract.children = {child.id};
    parent.contract.child_port_mappings = {{kOutput, child.id, kOutput, PortDirection::kOutput}};
    auto target = definition(FeatureDefinitionId{"Deadline.Composite.Target"}, kSlow, FeatureDomain::kStream);
    target.contract.ports = {{kInput, PortDirection::kInput, kBytes, 1}};
    size_t callbacks = 0;
    target.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, auto) {
        ++callbacks;
        require(std::vector<uint8_t>(input.begin(), input.end()) == std::vector<uint8_t>{0x73},
                "Composite actual projected bytes were wrong");
        return state_delta(token);
    };
    for (const auto& item : {child, parent, target})
        succeeds(test::register_clock_test_definition(registry, item), "register Composite period fixture");
    FeatureGraphDocument graph;
    graph.asset_uuid = "fixture://clock-deadline/composite-source-unit";
    graph.schema_version = 1; graph.graph_version = 1; graph.contract_digest = registry.contract_digest();
    const FeatureNodeId parent_node{"Deadline.Composite.Parent.Node"}, target_node{"Deadline.Composite.Target.Node"};
    graph.nodes = {{parent_node, parent.id, parent.version}, {target_node, target.id, target.version}};
    graph.edges = {{parent_node, kOutput, target_node, kInput, EdgeKind::kSnapshot,
                    parent.contract.clock.id, target.contract.clock.id, 1, 1, 1}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    succeeds(compiled.result, "compile authored Composite source Clock edge");
    FeatureRuntime runtime(registry, {}, FeatureRuntimeOptions{[&] { return now.load(); }});
    succeeds(runtime.publish_graph(graph, compiled.value), "publish projected Composite transport");
    FeatureInstanceId parent_owner{}, child_owner{}, target_owner{};
    for (const auto& row : compiled.value.provenance) {
        if (row.definition_id == parent.id) parent_owner = row.instance_id;
        if (row.definition_id == target.id) target_owner = row.instance_id;
    }
    const auto* parent_instance = runtime.find_instance(parent_owner);
    require(parent_instance && parent_instance->children.size() == 1, "Composite fixture did not expand one actual leaf");
    child_owner = parent_instance->children.front();
    require(runtime.find_instance(child_owner) && runtime.find_instance(child_owner)->definition_id == child.id &&
                target_owner.is_valid(), "Composite fixture did not expose real leaf owners");
    succeeds(runtime.dispatch_call(child_owner), "publish real Fast child output through Composite boundary");
    const auto* owner = runtime.find_instance(target_owner);
    require(owner && owner->pending_messages.size() == 1 && owner->pending_messages.front().source.clock == kFast &&
                owner->pending_messages.front().source.timing && owner->pending_messages.front().source.timing->expires_at &&
                owner->pending_messages.front().source.timing->expires_at->nanoseconds == 16'666'667,
            "Composite projection replaced author 60 Hz deadline units with Fast leaf units");
    now.store(10'000'000);
    succeeds(runtime.dispatch_call(target_owner), "consume beyond one leaf period but within authored Composite period");
    require(callbacks == 1, "Composite source-period positive callback was missing");
    succeeds(runtime.dispatch_call(child_owner), "publish next Composite delivery");
    const auto before = runtime.find_instance(target_owner)->state;
    now.store(26'666'668);
    const auto expired = runtime.dispatch_call(target_owner);
    require(!expired && expired.code == foundation::ResultCode::kTimeout && callbacks == 1 &&
                unchanged(before, runtime.find_instance(target_owner)->state),
            "Composite transport did not expire in the author's declared period unit");
}

void virtual_published_source_period_snapshot() {
    std::atomic<uint64_t> now{0};
    Fixture fixture(EdgeKind::kSnapshot, true, 1,
        FeatureRuntimeOptions{[&] { return now.load(); }});
    succeeds(fixture.runtime.advance(0.002), "establish two old 1000 Hz source Clock periods");
    succeeds(fixture.send({0x81}), "publish old author-period baseline");
    const auto baseline = fixture.owner(fixture.receiver).pending_messages;
    require(baseline.size() == 1 && baseline.front().source.clock == kFast &&
                baseline.front().timestamp_ticks == 2 && baseline.front().source.timing &&
                baseline.front().source.timing->origin.nanoseconds == 0 &&
                baseline.front().source.timing->expires_at &&
                baseline.front().source.timing->expires_at->nanoseconds == 1'000'000,
            "published 1000 Hz source did not establish its actual one-millisecond edge unit");
    now.store(500'000);
    succeeds(fixture.consume(), "consume the old author-period timely baseline");
    require(fixture.consumed_inputs.size() == 1 && fixture.consumed_inputs.back() == std::vector<uint8_t>{0x81} &&
                fixture.owner(fixture.receiver).state.commit_count == 1 &&
                fixture.owner(fixture.receiver).pending_messages.empty(),
            "published-unit baseline did not use actual callback bytes and one owner commit");

    const auto sender_before = fixture.owner(fixture.sender).state;
    const auto receiver_before = fixture.owner(fixture.receiver).state;
    const auto generation_before = fixture.runtime.generation();
    const auto* current = fixture.registry.find_definition(kSender);
    require(current != nullptr, "published-unit sender declaration disappeared");
    auto replacement = *current;
    replacement.version = {1, 1, 0};
    replacement.contract.version = replacement.version;
    replacement.contract.clock = make_periodic_clock_contract(kFast, 500, 256,
        LateDataPolicy::kQueue, DeadlinePolicy::kObserve, 250'000, 0);
    StateMigration migration{{1, 0, 0}, {1, 1, 0}, {1, 0, 0}, {1, 0, 0}};
    succeeds(fixture.runtime.replace_definition(replacement, migration),
             "replace the leaf with the same ClockId at 500 Hz without republishing the graph");
    const auto* live = fixture.registry.find_definition(kSender);
    require(live && live->version == replacement.version && live->contract.clock.id == kFast &&
                live->contract.clock.frequency_hz == 500 && fixture.runtime.generation() != generation_before &&
                fixture.owner(fixture.sender).definition_digest == feature_contract_digest(*live) &&
                unchanged(sender_before, fixture.owner(fixture.sender).state) &&
                unchanged(receiver_before, fixture.owner(fixture.receiver).state),
            "same-ClockId replacement did not retain owner state and publish its new definition binding");
    const auto sender_callbacks = fixture.sender_callbacks;
    succeeds(fixture.runtime.advance(0.002), "advance exactly one newly published 500 Hz source period");
    require(fixture.sender_callbacks == sender_callbacks && unchanged(sender_before, fixture.owner(fixture.sender).state),
            "Call source executed automatically while advancing its replacement Clock schedule");
    succeeds(fixture.send({0x82}), "publish through the graph's frozen author-period route");
    const auto frozen = fixture.owner(fixture.receiver).pending_messages;
    require(frozen.size() == 1 && frozen.front().payload == std::vector<uint8_t>{0x82} &&
                frozen.front().source.instance == fixture.sender && frozen.front().source.clock == kFast &&
                frozen.front().source.invocation.generation == fixture.runtime.generation() &&
                frozen.front().timestamp_ticks == 3 && frozen.front().delivery_id.is_valid() &&
                frozen.front().source.timing && frozen.front().source.timing->origin.nanoseconds == 500'000 &&
                frozen.front().source.timing->expires_at &&
                frozen.front().source.timing->expires_at->nanoseconds == 1'500'000,
            "leaf replacement changed the published edge unit or retained the old 1000 Hz local schedule");
    const auto callbacks_before_expiry = fixture.consumed_inputs.size();
    const auto state_before_expiry = fixture.owner(fixture.receiver).state;
    now.store(2'000'000); // Age 1.5 ms: beyond the published 1 ms, within the new leaf's 2 ms period.
    const auto expired = fixture.consume();
    const bool frozen_rejected = !expired && expired.code == foundation::ResultCode::kTimeout &&
        fixture.consumed_inputs.size() == callbacks_before_expiry &&
        unchanged(state_before_expiry, fixture.owner(fixture.receiver).state) &&
        fixture.owner(fixture.receiver).pending_messages.empty();

    const auto source_node = std::find_if(fixture.graph.nodes.begin(), fixture.graph.nodes.end(),
        [](const auto& node) { return node.node_id == kSenderNode; });
    require(source_node != fixture.graph.nodes.end(), "published-unit graph lost its authored source node");
    source_node->definition_version = replacement.version;
    ++fixture.graph.graph_version;
    fixture.graph.contract_digest = fixture.registry.contract_digest();
    auto candidate = FeatureGraphCompiler{}.compile(fixture.graph, fixture.registry);
    succeeds(candidate.result, "compile updated node version/digest with the new author frequency");
    const auto replaced_generation = fixture.runtime.generation();
    succeeds(fixture.runtime.publish_graph(fixture.graph, candidate.value),
             "explicitly republish the graph to adopt the 500 Hz author-period unit");
    fixture.compiled = std::move(candidate.value);
    fixture.sender = {};
    fixture.receiver = {};
    for (const auto& row : fixture.compiled.provenance) {
        if (row.node_id == kSenderNode) fixture.sender = row.instance_id;
        if (row.node_id == kReceiverNode) fixture.receiver = row.instance_id;
    }
    require(fixture.sender.is_valid() && fixture.receiver.is_valid() && fixture.sender != fixture.receiver &&
                fixture.runtime.generation() != replaced_generation &&
                fixture.owner(fixture.sender).pending_messages.empty() &&
                fixture.owner(fixture.receiver).pending_messages.empty(),
            "explicit republish did not install the actual new owner generation");
    succeeds(fixture.runtime.advance(0.002), "advance the republished graph's first 500 Hz source period");
    succeeds(fixture.send({0x83}), "publish through the explicitly refreshed author-period route");
    const auto refreshed = fixture.owner(fixture.receiver).pending_messages;
    require(refreshed.size() == 1 && refreshed.front().payload == std::vector<uint8_t>{0x83} &&
                refreshed.front().source.instance == fixture.sender && refreshed.front().source.clock == kFast &&
                refreshed.front().source.invocation.generation == fixture.runtime.generation() &&
                refreshed.front().timestamp_ticks == 1 && refreshed.front().delivery_id.is_valid() &&
                refreshed.front().source.timing && refreshed.front().source.timing->origin.nanoseconds == 2'000'000 &&
                refreshed.front().source.timing->expires_at &&
                refreshed.front().source.timing->expires_at->nanoseconds == 4'000'000,
            "explicit graph republish did not bind the new two-millisecond author-period unit");
    const auto callbacks_before_fresh = fixture.consumed_inputs.size();
    const auto state_before_fresh = fixture.owner(fixture.receiver).state;
    now.store(3'500'000); // The same 1.5 ms age is valid after explicit republish.
    const auto consumed = fixture.consume();
    const bool refreshed_accepted = bool(consumed) && fixture.consumed_inputs.size() == callbacks_before_fresh + 1 &&
        fixture.consumed_inputs.back() == std::vector<uint8_t>{0x83} &&
        fixture.owner(fixture.receiver).state.payload == std::vector<uint8_t>{0x83} &&
        fixture.owner(fixture.receiver).state.commit_count == state_before_fresh.commit_count + 1 &&
        fixture.owner(fixture.receiver).pending_messages.empty();
    std::cout << "Clock published source unit leaf-hz=500 old-edge-ns=1000000 frozen-rejected=" << frozen_rejected
        << " old-result-code=" << static_cast<unsigned>(expired.code)
        << " republished-edge-ns=2000000 refreshed-accepted=" << refreshed_accepted
        << " new-result-code=" << static_cast<unsigned>(consumed.code) << '\n';
    require(frozen_rejected && refreshed_accepted,
            "published source-period snapshot or explicit graph republish boundary was not preserved");
}

void integer_phase_chunking() {
    const auto run = [](bool fine) {
        std::atomic<uint64_t> now{0};
        size_t callbacks = 0;
        auto feature = timed_definition("Deadline.Virtual.IntegerPhase", ExecutionKind::kContinuous, DeadlinePolicy::kObserve);
        feature.contract.clock = make_periodic_clock_contract(feature.contract.clock.id, 60, 256,
            LateDataPolicy::kReject, DeadlinePolicy::kObserve, 25, 5'000'000);
        feature.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
            ++callbacks; return state_delta(token);
        };
        VirtualHost host(now, std::move(feature));
        const auto count = fine ? 100'000u : 100u;
        for (unsigned index = 0; index != count; ++index)
            succeeds(host.runtime.advance(fine ? 0.001 : 1.0), "advance long 60 Hz integer phase in chunks");
        require(callbacks == 5999 && host.state().commit_count == 5999,
                "long/chunked 60 Hz release drifted before its phase boundary");
        succeeds(host.runtime.advance(0.005), "reach exact long phase boundary");
        require(callbacks == 6000, "long phase boundary did not release the exact 6000th period");
        succeeds(host.runtime.tick(host.clock), "explicit tick shares automatic owner release path");
        require(callbacks == 6001 && host.state().commit_count == 6001,
                "explicit tick did not share the phase Clock callback/owner commit path");
        return callbacks;
    };
    require(run(false) == run(true), "equal logical time split into different chunks changed the phase plan");
}

void same_clock_replacement_restarts_schedule(Results& results) {
    bool passed = true;
    size_t total_early_callbacks = 0;
    uint64_t total_early_commits = 0;
    for (const bool change_frequency : {true, false}) {
        std::atomic<uint64_t> now{0};
        size_t callbacks = 0;
        auto feature = timed_definition("Deadline.Virtual.Replacement", ExecutionKind::kContinuous, DeadlinePolicy::kObserve);
        feature.contract.clock = make_periodic_clock_contract(feature.contract.clock.id, 100, 256,
            LateDataPolicy::kQueue, DeadlinePolicy::kObserve, 25, 0);
        feature.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
            ++callbacks; return state_delta(token);
        };
        VirtualHost host(now, feature);
        succeeds(host.runtime.advance(0.035), "advance old exact 100 Hz schedule to 35 ms");
        require(callbacks == 3 && host.state().commit_count == 3,
                "replacement fixture did not establish three old callbacks plus a half-period remainder");
        const auto before = host.state();
        const auto generation = host.runtime.generation();
        auto replacement = feature;
        replacement.version = {1, 1, 0};
        replacement.contract.version = replacement.version;
        const uint32_t frequency = change_frequency ? 50 : 100;
        const uint64_t phase = change_frequency ? 5'000'000 : 3'000'000;
        replacement.contract.clock = make_periodic_clock_contract(feature.contract.clock.id, frequency, 256,
            LateDataPolicy::kQueue, DeadlinePolicy::kObserve, 25, phase);
        StateMigration migration{{1, 0, 0}, {1, 1, 0}, {1, 0, 0}, {1, 0, 0}};
        succeeds(host.runtime.replace_definition(replacement, migration), "publish same-ClockId schedule replacement");
        const auto* declaration = host.registry.find_definition(feature.id);
        require(declaration && declaration->contract.clock.id == feature.contract.clock.id &&
                    declaration->contract.clock.frequency_hz == frequency &&
                    declaration->contract.clock.timing->phase_ns == phase &&
                    host.runtime.generation() != generation && unchanged(before, host.state()),
                "replacement did not publish the new schedule while retaining owner state");
        const uint64_t period_ns = 1'000'000'000 / frequency; // Exact 50/100 Hz periods.
        const auto first_release_ns = period_ns + phase;
        succeeds(host.runtime.advance(static_cast<double>(first_release_ns - 1'000'000) / 1e9),
                 "advance replacement to one millisecond before its first new period plus phase");
        const auto early_callbacks = callbacks - 3;
        const auto early_state = host.state();
        succeeds(host.runtime.advance(0.001), "reach replacement's first period plus phase");
        const auto at_release = callbacks - 3;
        const auto at_release_commits = host.state().commit_count - before.commit_count;
        succeeds(host.runtime.advance(static_cast<double>(period_ns) / 1e9), "reach replacement's second exact period");
        const auto next_release = callbacks - 3;
        const bool case_passed = early_callbacks == 0 && unchanged(before, early_state) &&
            at_release == 1 && at_release_commits == 1 && next_release == 2 &&
            host.state().commit_count == before.commit_count + 2;
        passed = passed && case_passed;
        total_early_callbacks += early_callbacks;
        total_early_commits += early_state.commit_count - before.commit_count;
        std::cout << "Clock replacement same-id=1 frequency-hz=" << frequency << " phase-ns=" << phase
            << " old-logical-ns=35000000 first-relative-ns=" << first_release_ns
            << " early-callbacks=" << early_callbacks << " first-callbacks=" << at_release
            << " next-callbacks=" << next_release << " passed=" << case_passed << '\n';
        const auto tick_callbacks = callbacks;
        const auto tick_commits = host.state().commit_count;
        succeeds(host.runtime.tick(host.clock), "explicit replacement Clock callback recovery");
        require(callbacks == tick_callbacks + 1 && host.state().commit_count == tick_commits + 1,
                "replacement owner stopped executing its actual callback/commit path");
    }
    results.observe("same-clock-replacement-first-period-plus-phase", passed, foundation::Result::success(),
                    0, total_early_callbacks, total_early_commits, 0);
}

void nonzero_receipt_ttl_keeps_command_expiry(Results& results) {
    struct Case final {
        const char* name;
        bool fast_caller;
        uint32_t receipt_ticks;
        uint64_t receipt_at, consume_at;
        bool callback_delay, should_consume;
    };
    constexpr Case cases[] = {
        {"long-receipt-ttl-cannot-extend-original-command", true, 10, 50'000'000, 100'000'001, false, false},
        {"short-receipt-ttl-cannot-expire-timely-command", false, 1, 10'000'000, 20'000'000, false, true},
        {"short-receipt-ttl-cannot-reject-valid-receipt-output", false, 1, 10'000'000, 10'000'000, true, true}};
    bool passed = true;
    size_t total_consumer_callbacks = 0;
    uint64_t total_consumer_commits = 0;
    auto last_result = foundation::Result::success();
    for (const auto& scenario : cases) {
        std::atomic<uint64_t> now{0};
        Fixture fixture(EdgeKind::kCommand, scenario.fast_caller, scenario.fast_caller ? 100 : 10,
            FeatureRuntimeOptions{[&] { return now.load(); }}, scenario.receipt_ticks);
        fixture.timely_roundtrip(scenario.fast_caller ? kShortBudgetNs : kLongBudgetNs);
        const auto command = fixture.issue_and_consume();
        require(fixture.owner(fixture.receiver).pending_messages.empty(),
                "nonzero Receipt TTL fixture retained old Command input");
        const auto worker_before = fixture.owner(fixture.receiver).state;
        const auto receipt_callbacks = fixture.receipt_inputs.size();
        if (scenario.callback_delay) fixture.on_receiver_receipt = [&] { now.store(scenario.receipt_at); };
        else now.store(scenario.receipt_at);
        const auto emitted = fixture.receipt();
        const auto expected_expiry = scenario.fast_caller ? 100'000'000u : 1'000'000'000u;
        bool original_expiry_preserved = false;
        if (emitted) {
            fixture.expect_receipt(command.transaction);
            const auto& message = fixture.owner(fixture.sender).pending_messages.front();
            original_expiry_preserved = message.source.timing && message.source.timing->expires_at &&
                message.source.timing->expires_at->nanoseconds == expected_expiry;
        }
        const auto caller_before = fixture.owner(fixture.sender).state;
        const auto callbacks = fixture.sender_callbacks;
        auto consumed = emitted;
        if (emitted) {
            now.store(scenario.consume_at);
            consumed = fixture.runtime.dispatch_call(fixture.sender);
        }
        const auto callback_delta = fixture.sender_callbacks - callbacks;
        const auto commit_delta = fixture.owner(fixture.sender).state.commit_count - caller_before.commit_count;
        const bool consumer_correct = scenario.should_consume
            ? bool(consumed) && callback_delta == 1 && commit_delta == 1
            : !consumed && consumed.code == foundation::ResultCode::kTimeout && callback_delta == 0 &&
                unchanged(caller_before, fixture.owner(fixture.sender).state);
        const bool case_passed = emitted && original_expiry_preserved && consumer_correct &&
            fixture.receipt_inputs.size() == receipt_callbacks + 1 && fixture.receipt_inputs.back().empty() &&
            fixture.owner(fixture.receiver).state.commit_count == worker_before.commit_count + 1 &&
            fixture.owner(fixture.sender).pending_messages.empty();
        passed = passed && case_passed;
        total_consumer_callbacks += callback_delta;
        total_consumer_commits += commit_delta;
        last_result = consumed;
        std::cout << "Clock nonzero-Receipt probe=" << scenario.name << " passed=" << case_passed
            << " receipt-ticks=" << scenario.receipt_ticks << " emitted=" << bool(emitted)
            << " emit-code=" << static_cast<unsigned>(emitted.code)
            << " original-expiry-preserved=" << original_expiry_preserved
            << " consumer-accepted=" << bool(consumed) << " consumer-code=" << static_cast<unsigned>(consumed.code)
            << " callback-delta=" << callback_delta << " commit-delta=" << commit_delta << '\n';
        fixture.on_receiver_receipt = {};
        // All native failures above are business observations. A new immediate
        // transaction must still traverse the same nonzero-TTL routes successfully.
        fixture.clear_caller_input();
        fixture.timely_roundtrip(scenario.fast_caller ? kShortBudgetNs : kLongBudgetNs);
    }
    results.observe("nonzero-receipt-ttl-preserves-command-expiry", passed, last_result, 0,
                    total_consumer_callbacks, total_consumer_commits, 0);
}

void final_sample_gates_state_only_commit(Results& results) {
    bool passed = true;
    uint64_t committed = 0;
    auto last_result = foundation::Result::success();
    for (const auto policy : {DeadlinePolicy::kRejectCommit, DeadlinePolicy::kObserve}) {
        std::atomic<bool> after_schema{false};
        std::atomic<unsigned> post_schema_reads{0};
        size_t callbacks = 0;
        FeatureRegistry registry{test::clock_test_host()};
        auto feature = timed_definition("Deadline.Virtual.FinalSample", ExecutionKind::kCall, policy);
        const auto feature_id = feature.id;
        feature.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
            ++callbacks;
            return state_delta(token); // State-only: no output message can gate this commit accidentally.
        };
        feature.validate_state_payload = [&](auto) {
            after_schema.store(true);
            return foundation::Result::success();
        };
        succeeds(test::register_clock_test_definition(registry, feature), "register final-sample owner");
        FeatureRuntime runtime(registry, {}, FeatureRuntimeOptions{[&]() -> uint64_t {
            if (!after_schema.load()) return 0;
            // The first post-schema read is the transport staging sample, then
            // the two final owner samples straddle the execution deadline.
            return ++post_schema_reads <= 2 ? 0 : 101;
        }});
        const auto expanded = registry.expand(feature_id, {});
        succeeds(expanded.result, "expand final-sample owner");
        const auto active = runtime.activate(expanded.value, ScopeId{"World"});
        succeeds(active.result, "activate final-sample owner");
        const auto before = runtime.find_instance(active.value)->state;
        const auto result = runtime.dispatch_call(active.value);
        require(callbacks == 1 && post_schema_reads >= 3,
                "final-sample fixture did not reach the armed monotonic post-schema deadline crossing");
        const auto observed = runtime.inspect_invocations(active.value);
        succeeds(observed.result, "inspect final-sample execution diagnostics");
        const auto& state = runtime.find_instance(active.value)->state;
        const bool late = observed.value.timing && observed.value.timing->deadline_exceeded;
        const bool case_passed = policy == DeadlinePolicy::kRejectCommit
            ? !result && result.code == foundation::ResultCode::kTimeout && unchanged(before, state)
            : bool(result) && state.commit_count == before.commit_count + 1 && late;
        passed = passed && case_passed;
        committed += state.commit_count - before.commit_count;
        last_result = result;
        std::cout << "Clock final-sample policy=" << static_cast<unsigned>(policy) << " passed=" << case_passed
            << " native-accepted=" << bool(result) << " native-code=" << static_cast<unsigned>(result.code)
            << " post-schema-reads=" << post_schema_reads.load() << " late-observed=" << late
            << " commit-delta=" << state.commit_count - before.commit_count << '\n';
    }
    results.observe("final-time-sample-gates-state-only-commit", passed, last_result, 101, 2, committed, 0);
}

void nominal_60hz_quantization_is_consistent(Results& results) {
    bool passed = true;
    size_t total_callbacks = 0;
    uint64_t total_commits = 0;
    for (const uint64_t phase : {uint64_t{0}, uint64_t{5'000'000}}) {
        const auto run = [&](unsigned grouping) {
            std::atomic<uint64_t> now{0};
            size_t callbacks = 0;
            auto feature = timed_definition("Deadline.Virtual.Nominal60", ExecutionKind::kContinuous, DeadlinePolicy::kObserve);
            feature.contract.clock = make_periodic_clock_contract(feature.contract.clock.id, 60, 256,
                LateDataPolicy::kReject, DeadlinePolicy::kObserve, 25, phase);
            feature.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
                ++callbacks; return state_delta(token);
            };
            VirtualHost host(now, std::move(feature));
            succeeds(host.runtime.advance(static_cast<double>(phase) / 1e9), "advance only the nominal fixture phase");
            require(callbacks == 0 && host.state().commit_count == 0, "phase-only nominal fixture already executed");
            const auto advance = [&](double seconds, size_t expected_delta, std::string_view label) {
                const auto before_callbacks = callbacks;
                const auto before_commits = host.state().commit_count;
                succeeds(host.runtime.advance(seconds), "advance nominal 60 Hz actual Runtime");
                const auto callback_delta = callbacks - before_callbacks;
                const auto commit_delta = host.state().commit_count - before_commits;
                passed = passed && callback_delta == expected_delta && commit_delta == expected_delta;
                std::cout << "Clock nominal60 phase-ns=" << phase << " grouping=" << grouping
                    << " step=" << label << " callback-delta=" << callback_delta << " commit-delta=" << commit_delta
                    << " expected-delta=" << expected_delta << '\n';
                const auto before_zero = host.state();
                const auto callbacks_before_zero = callbacks;
                succeeds(host.runtime.advance(0), "zero-delta pump between nominal frames");
                passed = passed && callbacks == callbacks_before_zero && unchanged(before_zero, host.state());
            };
            if (grouping == 0) {
                advance(1.0 / 60.0, 1, "first-1over60");
                advance(1.0 / 60.0, 1, "second-1over60");
                advance(1.0 / 60.0, 1, "third-1over60");
            } else if (grouping == 1) {
                advance(2.0 / 60.0, 2, "two-periods-together");
                advance(1.0 / 60.0, 1, "third-period");
            } else {
                advance(3.0 / 60.0, 3, "three-periods-together");
            }
            passed = passed && callbacks == 3 && host.state().commit_count == 3;
            total_callbacks += callbacks;
            total_commits += host.state().commit_count;
            return callbacks;
        };
        const auto separate = run(0);
        const auto two_plus_one = run(1);
        const auto combined = run(2);
        passed = passed && separate == two_plus_one && separate == combined;

        std::atomic<uint64_t> drop_now{0};
        size_t drop_callbacks = 0;
        auto drop = timed_definition("Deadline.Virtual.Nominal60.Drop", ExecutionKind::kContinuous, DeadlinePolicy::kObserve);
        drop.contract.clock = make_periodic_clock_contract(drop.contract.clock.id, 60, 1,
            LateDataPolicy::kDrop, DeadlinePolicy::kObserve, 25, phase);
        drop.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
            ++drop_callbacks; return state_delta(token);
        };
        VirtualHost drop_host(drop_now, std::move(drop));
        succeeds(drop_host.runtime.advance(static_cast<double>(phase) / 1e9), "advance Drop fixture phase");
        succeeds(drop_host.runtime.advance(2.0 / 60.0), "drop the second due period at backlog limit one");
        const auto before_zero = drop_host.state();
        const auto before_zero_callbacks = drop_callbacks;
        succeeds(drop_host.runtime.advance(0), "zero delta cannot revive a dropped rounded release");
        passed = passed && before_zero_callbacks == 1 && drop_callbacks == before_zero_callbacks &&
            unchanged(before_zero, drop_host.state());
        succeeds(drop_host.runtime.advance(1.0 / 60.0), "fresh third nominal period after Drop");
        passed = passed && drop_callbacks == 2 && drop_host.state().commit_count == 2;
        std::cout << "Clock nominal60 Drop phase-ns=" << phase << " before-zero=" << before_zero_callbacks
            << " final-callbacks=" << drop_callbacks << " final-commits=" << drop_host.state().commit_count << '\n';
    }
    // 1024 Hz has an exactly representable half-nanosecond first period.
    // Carrying its -0.5 ns remainder through a zero pump must never step backwards.
    std::atomic<uint64_t> tie_now{0};
    size_t tie_callbacks = 0;
    auto tie = timed_definition("Deadline.Virtual.NominalHalfTie", ExecutionKind::kContinuous, DeadlinePolicy::kObserve);
    tie.contract.clock = make_periodic_clock_contract(tie.contract.clock.id, 1024, 4,
        LateDataPolicy::kReject, DeadlinePolicy::kObserve, 25, 0);
    tie.execute = [&](const InvocationToken& token, const StateSnapshot&, auto, auto) {
        ++tie_callbacks; return state_delta(token);
    };
    VirtualHost tie_host(tie_now, std::move(tie));
    for (size_t count = 1; count != 4; ++count) {
        succeeds(tie_host.runtime.advance(1.0 / 1024.0), "advance exact binary nominal half-nanosecond period");
        passed = passed && tie_callbacks == count && tie_host.state().commit_count == count;
        succeeds(tie_host.runtime.advance(0), "zero-delta pump retains half-nanosecond carry");
        passed = passed && tie_callbacks == count && tie_host.state().commit_count == count;
    }
    results.observe("nominal-60hz-phase-quantization", passed, foundation::Result::success(),
                    50'000'000, total_callbacks, total_commits, 0);
}
} // namespace

int main() {
    Results results;
    results.run("delayed Snapshot", [&] { delayed_capture(results, EdgeKind::kSnapshot, "delayed-snapshot-consumption"); });
    results.run("delayed Event", [&] { delayed_capture(results, EdgeKind::kEvent, "delayed-event-consumption"); });
    results.run("late Receipt", [&] { late_receipt(results); });
    results.run("timely Receipt", [&] { timely_receipt_with_larger_receiver_tick(results); });
    results.run("Continuous phase", [&] { continuous_phase_from_contract_json(results); });
    results.run("virtual execution boundaries", [&] { virtual_execution_boundaries(); });
    results.run("virtual transport boundaries", [&] { virtual_transport_boundaries(); });
    results.run("virtual defer/retry", [&] { virtual_defer_and_retry(); });
    results.run("virtual async queue/backlog", [&] { virtual_async_queue_and_backlog(); });
    results.run("virtual output/Command expiry", [&] { virtual_output_origin_and_command_expiry(); });
    results.run("virtual in-flight Snapshot", [&] { virtual_inflight_snapshot_preserves_replacement(); });
    results.run("virtual time faults/reset", [&] { virtual_time_faults_and_reset(); });
    results.run("virtual Composite source period", [&] { virtual_composite_author_period(); });
    results.run("virtual published source-period snapshot", [&] { virtual_published_source_period_snapshot(); });
    results.run("integer phase chunking", [&] { integer_phase_chunking(); });
    results.run("same-Clock replacement schedule", [&] { same_clock_replacement_restarts_schedule(results); });
    results.run("nonzero Receipt TTL", [&] { nonzero_receipt_ttl_keeps_command_expiry(results); });
    results.run("final sample state-only commit", [&] { final_sample_gates_state_only_commit(results); });
    results.run("nominal 60 Hz quantization", [&] { nominal_60hz_quantization_is_consistent(results); });
    std::cout << "Clock Runtime business observations=" << results.observed << " expected=9 failed=" << results.failed
        << " fixture-errors=" << results.fixture_errors << '\n';
    return results.observed == 9 && results.failed == 0 && results.fixture_errors == 0 ? 0 : 1;
}
