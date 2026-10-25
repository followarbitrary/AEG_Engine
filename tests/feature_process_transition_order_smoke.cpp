/** @brief Feature-private transition arbitration and real Process/Outbox commit acceptance. */
// Intended final location: tests/feature_process_transition_order_smoke.cpp.
#include "feature_clock_test_fixture.hpp"
#include "concurrency.hpp"
#include "feature_graph.hpp"
#include "../Engine/Feature/private/feature_outbox.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace aeg::feature;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using Access = detail::FeatureOutboxAccess;
using Handle = detail::FeatureOutboxHandle;
using Delivery = detail::OutboxDeliveryState;
using aeg::concurrency::CompletionSource;

constexpr FeatureDefinitionId kDefinition{"Fixture.Process.TransitionOrder"};
constexpr FeatureNodeId kOwnerNode{"Fixture.Process.TransitionOrder.Owner"};
constexpr ClockId kClock{"Fixture.Process.TransitionOrder.Clock"};
constexpr SideEffectId kCommand{"Fixture.Process.TransitionOrder.Command"};
constexpr uint64_t kInitialNs = 10'000;
constexpr uint64_t kDeadlineNs = 1'000'000; // The explicit 1000 Hz Clock period.
constexpr uint64_t kWaitNs = 2'000'000'000;
constexpr std::size_t kPayloadSize = 32;
constexpr uint8_t kMagic = 0xc7, kSeed = 1, kTransition = 2, kIdle = 1, kTarget = 2;
constexpr uint8_t kExit = 0xe1, kState = 0x51, kEnter = 0xe2;
constexpr uint8_t kBaselineApplied = 0xb1, kBaselinePending = 0xb2;
constexpr uint8_t kFirstCommand = 0xd1, kSecondCommand = 0xd2;
constexpr uint32_t kTrace = (uint32_t{kExit} << 16) | (uint32_t{kState} << 8) | kEnter;
constexpr std::array<uint8_t, 1> kGuardInput{0xa5};

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void succeeds(const Result& result, std::string_view operation) {
    require(bool(result), std::string(operation) + ": " + result.message);
}

template <class T> void succeeds(const aeg::foundation::ResultValue<T>& result, std::string_view operation) {
    succeeds(result.result, operation);
}

uint64_t read_u64(std::span<const uint8_t> bytes, std::size_t offset) noexcept {
    uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index)
        value |= uint64_t{bytes[offset + index]} << (index * 8);
    return value;
}

void write_u64(std::vector<uint8_t>& bytes, std::size_t offset, uint64_t value) {
    for (unsigned index = 0; index != 8; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}

bool same_state(const StateStore& a, const StateStore& b) {
    return a.version == b.version && a.commit_count == b.commit_count &&
        a.payload_revision == b.payload_revision && a.schema_version == b.schema_version && a.payload == b.payload;
}

bool same_process(const ProcessInstance& a, const ProcessInstance& b) {
    return a.id == b.id && a.owner == b.owner && a.owner_module == b.owner_module &&
        a.parent == b.parent && a.children == b.children && a.revision == b.revision &&
        same_state(a.state, b.state) && a.status == b.status && a.states == b.states &&
        a.transitions == b.transitions && a.state_schema_version == b.state_schema_version &&
        a.initial_state == b.initial_state && a.terminal_states == b.terminal_states && a.current_state == b.current_state;
}

Result validate_payload(std::span<const uint8_t> bytes) {
    if (bytes.empty()) return Result::success(); // Actual freshly activated Process state.
    if (bytes.size() != kPayloadSize || bytes[0] != kMagic)
        return aeg::foundation::invalid_argument("fixture state schema rejects malformed transition payload");
    if (bytes[1] == kSeed && bytes[2] == kIdle && bytes[3] == 0 &&
        read_u64(bytes, 4) == 0 && bytes[12] == 0 && bytes[13] == 0 && bytes[14] == 0 &&
        bytes[15] == 0 && read_u64(bytes, 16) == 0)
        return Result::success();
    if (bytes[1] == kTransition && bytes[2] == kTarget && bytes[3] == 3 &&
        read_u64(bytes, 4) != 0 && bytes[12] == kExit && bytes[13] == kState && bytes[14] == kEnter &&
        bytes[15] == 3 && read_u64(bytes, 16) == 1)
        return Result::success();
    return aeg::foundation::invalid_argument("fixture state schema rejects malformed transition payload");
}

// These are Feature-private author action identities, not new Runtime scheduling APIs.
struct Transition final {
    int32_t priority{};
    uint64_t causal{};
    FeatureNodeId node{};
    const char* target{};
    bool enabled{true};
};

bool enabled(const Transition& candidate, const StateSnapshot& state, std::span<const uint8_t> input) noexcept {
    // Pure guard: no mutation, retained alias, clock read, or external application.
    return candidate.enabled && state.current_state == "Idle" && input.size() == kGuardInput.size() &&
        input[0] == kGuardInput[0];
}

const Transition* choose(std::span<const Transition> table, const StateSnapshot& state,
                         std::span<const uint8_t> input) noexcept {
    const Transition* winner = nullptr;
    for (const auto& candidate : table) {
        if (!enabled(candidate, state, input)) continue;
        if (!winner || candidate.priority > winner->priority ||
            (candidate.priority == winner->priority && candidate.causal < winner->causal) ||
            (candidate.priority == winner->priority && candidate.causal == winner->causal && candidate.node < winner->node))
            winner = &candidate;
    }
    return winner;
}

std::vector<Transition> priority_table() {
    return {{8, 1, FeatureNodeId(1), "ChoiceA"},
            {9, 900, FeatureNodeId(0x3'0000'0007ull), "ChoiceB"},
            {7, 0, FeatureNodeId(2), "ChoiceC"},
            {100, 0, FeatureNodeId(3), "ChoiceD", false}};
}

std::vector<Transition> causal_table() {
    return {{9, 0x2'0000'0001ull, FeatureNodeId(0x1'0000'0007ull), "ChoiceA"},
            {9, 0x1'0000'0009ull, FeatureNodeId(0x3'0000'0007ull), "ChoiceB"},
            {9, 0x1'0000'000aull, FeatureNodeId(0x2'0000'0007ull), "ChoiceC"},
            {100, 0, FeatureNodeId(1), "ChoiceD", false}};
}

std::vector<Transition> node_table() {
    return {{9, 42, FeatureNodeId(0x2'0000'0007ull), "ChoiceA"},
            {9, 42, FeatureNodeId(0x1'0000'0007ull), "ChoiceB"},
            {9, 42, FeatureNodeId(0x3'0000'0007ull), "ChoiceC"},
            {100, 0, FeatureNodeId(1), "ChoiceD", false}};
}

OutboxIntent command(uint8_t code, FeatureNodeId winner = {}, uint8_t enter_proof = 0) {
    OutboxIntent intent{kCommand, 1, std::vector<uint8_t>(10)};
    intent.payload[0] = code;
    write_u64(intent.payload, 1, winner.value());
    intent.payload[9] = enter_proof;
    return intent;
}

enum class Mode { kSeed, kTransition, kBadState, kInvalidLastIntent };
struct Gate final { CompletionSource prepared{}, release{}; };

struct Script final {
    std::vector<Transition> table{};
    Mode mode{Mode::kTransition};
    Gate* gate{};
    std::atomic<unsigned> callbacks{0}, candidates_prepared{0};
    std::atomic<uint64_t> callback_thread{0}, prepared_winner{0};
    std::atomic<uint32_t> prepared_trace{0};

    aeg::foundation::ResultValue<StateDelta> execute(const InvocationToken& token, const StateSnapshot& state,
                                                    std::span<const uint8_t> input) {
        callbacks.fetch_add(1);
        callback_thread.store(aeg::concurrency::current_thread_id().value);
        StateDelta delta{token.state_version};
        std::vector<uint8_t> payload(kPayloadSize);
        payload[0] = kMagic;
        write_u64(payload, 24, token.state_version.value());
        if (mode == Mode::kSeed) {
            payload[1] = kSeed;
            payload[2] = kIdle;
            delta.state_payload = std::move(payload);
            delta.outbox = {command(kBaselineApplied), command(kBaselinePending)};
            return {Result::success(), std::move(delta)};
        }
        const auto* winner = choose(table, state, input);
        if (!winner) return {aeg::foundation::invalid_state("fixture has no enabled transition"), {}};
        payload[1] = kTransition;
        payload[2] = kIdle;
        write_u64(payload, 4, winner->node.value());

        // Private Actions stage candidate bytes only. None can apply a command.
        const auto append = [&](uint8_t action) {
            payload[12 + payload[3]++] = action;
            ++payload[15];
        };
        if (state.current_state != "Idle" || payload[2] != kIdle)
            return {aeg::foundation::invalid_state("Exit did not observe the old state"), {}};
        append(kExit);
        payload[2] = kTarget;
        delta.next_state = winner->target;
        write_u64(payload, 16, 1); // Exactly one selected transition.
        append(kState);
        if (payload[2] != kTarget || delta.next_state != winner->target || payload[13] != kState)
            return {aeg::foundation::invalid_state("Enter did not observe the staged new state"), {}};
        append(kEnter);
        prepared_winner.store(winner->node.value());
        prepared_trace.store((uint32_t{payload[12]} << 16) | (uint32_t{payload[13]} << 8) | payload[14]);
        candidates_prepared.fetch_add(1);
        delta.outbox = {command(kFirstCommand, winner->node, payload[14]),
                        command(kSecondCommand, winner->node, payload[14])};
        if (mode == Mode::kBadState) payload[14] = kExit; // Size/version valid; the actual schema callback rejects it.
        delta.state_payload = std::move(payload);
        if (mode == Mode::kInvalidLastIntent) {
            auto invalid = command(0xff, winner->node, kEnter);
            invalid.version = 2; // Two valid commands precede the undeclared protocol version.
            delta.outbox.push_back(std::move(invalid));
        }
        if (gate) {
            if (const auto result = gate->prepared.complete(); !result) return {result, {}};
            if (const auto result = gate->release.completion().wait(kWaitNs); !result) return {result, {}};
        }
        return {Result::success(), std::move(delta)};
    }
};

// The only actual application in this fixture. Fixed buffers keep noexcept apply non-fallible.
struct Consumer final {
    FeatureRuntime* runtime{};
    ProcessId process{};
    std::string expected_state{"Idle"};
    StateVersion expected_version{};
    ProcessRevision expected_revision{};
    ProcessStatus expected_status{ProcessStatus::kReady};
    FeatureNodeId expected_winner{};
    uint64_t prior_state_version{};
    bool require_enter{false}, violation{false};
    unsigned entries_seen{0}, applied{0}, entered_before_apply{0};
    std::array<uint8_t, 8> commands{};
};

void apply(void* opaque, const OutboxIntent& intent) noexcept {
    auto& consumer = *static_cast<Consumer*>(opaque);
    ++consumer.entries_seen;
    const auto* process = consumer.runtime->find_process(consumer.process);
    bool valid = process && process->current_state == consumer.expected_state &&
        process->state.version == consumer.expected_version && process->revision == consumer.expected_revision &&
        process->status == consumer.expected_status && process->state.payload.size() == kPayloadSize &&
        intent.effect == kCommand && intent.version == 1 && intent.payload.size() == 10;
    if (valid && consumer.require_enter) {
        const auto& payload = process->state.payload;
        valid = payload[0] == kMagic && payload[1] == kTransition && payload[2] == kTarget &&
            payload[3] == 3 && payload[12] == kExit && payload[13] == kState && payload[14] == kEnter &&
            payload[15] == 3 && read_u64(payload, 4) == consumer.expected_winner.value() &&
            read_u64(payload, 16) == 1 && read_u64(payload, 24) == consumer.prior_state_version &&
            read_u64(intent.payload, 1) == consumer.expected_winner.value() && intent.payload[9] == kEnter;
    } else if (valid) {
        valid = process->state.payload[1] == kSeed && process->state.payload[2] == kIdle &&
            read_u64(intent.payload, 1) == 0 && intent.payload[9] == 0;
    }
    if (!valid || consumer.applied == consumer.commands.size()) {
        consumer.violation = true;
        return;
    }
    if (consumer.require_enter) ++consumer.entered_before_apply;
    consumer.commands[consumer.applied++] = intent.payload[0];
}

struct Fixture final {
    std::atomic<uint64_t> now{kInitialNs};
    Script script{};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry, {}, FeatureRuntimeOptions{[this] { return now.load(); }}};
    CompiledFeatureGraph compiled{};
    FeatureInstanceId owner{};
    ProcessId process{};
    detail::FeatureOutboxBinding binding{};
    Consumer consumer{};

    explicit Fixture(std::vector<Transition> table) {
        script.table = std::move(table);
        FeatureDefinition definition{};
        definition.id = kDefinition;
        definition.version = {1, 0, 0};
        definition.owner_module = ModuleIdentity("com.aeg.fixture.transition-order");
        auto& contract = definition.contract;
        contract.definition_id = definition.id;
        contract.version = definition.version;
        contract.domains = to_mask(FeatureDomain::kLogic);
        contract.execution = ExecutionKind::kProcess;
        contract.state = StatePolicy::kProcessState;
        contract.has_continuation = true;
        contract.scope = {ScopeId("World")};
        contract.clock = make_periodic_clock_contract(kClock, 1000, 4, LateDataPolicy::kReject,
                                                      DeadlinePolicy::kRejectCommit, 1, 0);
        contract.permissions.version = 1;
        contract.state_payload = {{1, 0, 0}, static_cast<uint32_t>(kPayloadSize)};
        contract.side_effects = {{kCommand, 1, SideEffectKind::kCommand, CommitPolicy::kOutbox}};
        definition.validate_state_payload = validate_payload;
        definition.execute = [this](const InvocationToken& token, const StateSnapshot& state,
                                    std::span<const uint8_t> input, std::span<const uint8_t>) {
            return script.execute(token, state, input);
        };
        succeeds(aeg::feature::test::register_clock_test_definition(registry, std::move(definition)),
                 "approve/register actual Process definition");
        FeatureGraphDocument graph{};
        graph.asset_uuid = "fixture://process-transition-order";
        graph.schema_version = 1;
        graph.graph_version = 1;
        graph.contract_digest = registry.contract_digest();
        graph.nodes = {{kOwnerNode, kDefinition, {1, 0, 0}, {}, {}}};
        graph.process_subgraphs = {{kOwnerNode,
            {"Idle", "ChoiceA", "ChoiceB", "ChoiceC", "ChoiceD"},
            {"Idle->ChoiceA", "Idle->ChoiceB", "Idle->ChoiceC", "Idle->ChoiceD"}, 1, "Idle",
            {"ChoiceA", "ChoiceB", "ChoiceC", "ChoiceD"}}};
        auto result = FeatureGraphCompiler{}.compile(graph, registry);
        succeeds(result, "compile actual Process graph");
        compiled = std::move(result.value);
        succeeds(runtime.publish_graph(graph, compiled), "publish actual Process graph");
        require(compiled.provenance.size() == 1 && compiled.provenance[0].node_id == kOwnerNode,
                "actual Process owner provenance differs");
        owner = compiled.provenance[0].instance_id;
        const auto actual_process = runtime.process(owner);
        succeeds(actual_process, "resolve actual Process identity");
        process = actual_process.value;
        require(runtime.find_process(process)->current_state == "Idle", "Process did not enter its declared initial state");
        const auto actual_binding = Access::bind(runtime, owner, kCommand, 1);
        succeeds(actual_binding, "bind real private outbox consumer");
        binding = actual_binding.value;
        require(binding.source() && binding.source()->instance_id == owner && binding.source()->node_id == kOwnerNode,
                "consumer was not bound to actual published provenance");
        consumer.runtime = &runtime;
        consumer.process = process;
    }

    ~Fixture() {
        if (script.gate) static_cast<void>(script.gate->release.complete());
        static_cast<void>(runtime.shutdown_execution());
    }

    auto capture() { return Access::capture(runtime, binding); }
    Result dispatch() { return runtime.dispatch_process(owner, kGuardInput); }

    void consume(const Handle& handle) {
        require(handle.intent() && handle.intent()->effect == kCommand && handle.intent()->version == 1 &&
                handle.intent()->payload.size() == 10, "consumer preflight rejected committed command schema");
        succeeds(Access::consume(runtime, binding, handle, apply, &consumer), "consume actual committed command");
        require(!consumer.violation, "application observed uncommitted Enter/state or malformed command");
    }

    Result pump_until_idle() {
        auto first_error = Result::success();
        for (unsigned spin = 0; spin != 2000 && runtime.pending_execution_count() != 0; ++spin) {
            const auto result = runtime.pump_completions();
            if (!result && first_error) first_error = result;
            if (runtime.pending_execution_count() != 0)
                succeeds(aeg::concurrency::sleep_for(1'000'000), "bounded owner completion yield");
        }
        require(runtime.pending_execution_count() == 0, "actual worker did not settle within bounded completion pumping");
        return first_error;
    }
};

struct Record final {
    InvocationToken token{};
    uint32_t index{};
    SideEffectId effect{};
    uint32_t version{};
    std::vector<uint8_t> bytes{};
    Delivery state{};
    ResultCode error{};
};

Record record(const Handle& handle) {
    require(handle.invocation() && handle.intent(), "journal handle has no owning token/bytes");
    const auto receipt = handle.receipt();
    require(receipt.invocation == *handle.invocation() && receipt.entry_index == handle.entry_index(),
            "journal receipt identity differs from canonical handle");
    return {*handle.invocation(), handle.entry_index(), handle.intent()->effect, handle.intent()->version,
            handle.intent()->payload, receipt.state, receipt.error_code};
}

bool same_record(const Record& a, const Record& b) {
    return a.token == b.token && a.index == b.index && a.effect == b.effect && a.version == b.version &&
        a.bytes == b.bytes && a.state == b.state && a.error == b.error;
}

struct Baseline final {
    ProcessInstance process{};
    StateStore instance_state{};
    Handle pending{}, applied{};
    Record pending_record{}, applied_record{};
    unsigned applications{}, entries_seen{};
    std::array<uint8_t, 8> applied_commands{};
};

Baseline seed_nonempty_baseline(Fixture& fixture) {
    fixture.script.mode = Mode::kSeed;
    succeeds(fixture.dispatch(), "commit real non-transition baseline");
    const auto outputs = fixture.capture();
    succeeds(outputs, "capture two real baseline commands");
    require(outputs.value.size() == 2, "baseline did not establish two actual committed commands");
    const auto* process = fixture.runtime.find_process(fixture.process);
    require(process && process->current_state == "Idle" && process->revision == ProcessRevision(1) &&
            process->state.commit_count == 1, "baseline unexpectedly performed a transition");
    fixture.consumer.expected_version = process->state.version;
    fixture.consumer.expected_revision = process->revision;
    fixture.consume(outputs.value[0]);
    require(fixture.consumer.applied == 1 && fixture.consumer.commands[0] == kBaselineApplied &&
            outputs.value[0].receipt().state == Delivery::kApplied, "baseline command was not actually applied once");
    const auto pending = fixture.capture();
    succeeds(pending, "capture retained nonempty pending baseline");
    require(pending.value.size() == 1 && pending.value[0].intent()->payload[0] == kBaselinePending &&
            pending.value[0].receipt().state == Delivery::kPending, "baseline has no retained pending delivery");
    Baseline baseline{*process, fixture.runtime.find_instance(fixture.owner)->state,
        pending.value[0], outputs.value[0], record(pending.value[0]), record(outputs.value[0]),
        fixture.consumer.applied, fixture.consumer.entries_seen, fixture.consumer.commands};
    fixture.script.mode = Mode::kTransition;
    return baseline;
}

void require_preserved(Fixture& fixture, const Baseline& baseline) {
    const auto* process = fixture.runtime.find_process(fixture.process);
    require(process && same_process(*process, baseline.process), "failed candidate partially changed actual Process/state");
    require(same_state(fixture.runtime.find_instance(fixture.owner)->state, baseline.instance_state),
            "failed candidate changed the separate instance state store");
    const auto outputs = fixture.capture();
    succeeds(outputs, "capture journal after rejected candidate");
    require(outputs.value.size() == 1 && same_record(record(outputs.value[0]), baseline.pending_record) &&
            same_record(record(baseline.pending), baseline.pending_record) &&
            same_record(record(baseline.applied), baseline.applied_record),
            "failed candidate appended/lost/rewrote a retained pending or already-applied delivery");
    require(fixture.consumer.applied == baseline.applications && fixture.consumer.entries_seen == baseline.entries_seen &&
            fixture.consumer.commands == baseline.applied_commands && fixture.consumer.entered_before_apply == 0 &&
            !fixture.consumer.violation, "rejected candidate caused an actual application or modified prior effects");
}

void require_prepared_once(Fixture& fixture, uint64_t expected_node, unsigned callbacks) {
    require(fixture.script.candidates_prepared.load() == 1 && fixture.script.callbacks.load() == callbacks &&
            fixture.script.prepared_trace.load() == kTrace && fixture.script.prepared_winner.load() == expected_node,
            "actual author callback did not prepare exactly one ordered winner candidate");
}

void require_failed_commit(Fixture& fixture, ResultCode code) {
    const auto inspection = fixture.runtime.inspect_invocations(fixture.owner);
    succeeds(inspection, "inspect actual rejected Process attempt");
    require(inspection.value.open_attempts.empty() && inspection.value.last_failed && inspection.value.last_issued &&
            inspection.value.last_committed && inspection.value.last_failed->stage == InvocationFailureStage::kCommit &&
            inspection.value.last_failed->result.code == code &&
            inspection.value.last_failed->attempt.token == inspection.value.last_issued->token &&
            inspection.value.last_failed->attempt.process == fixture.process &&
            inspection.value.last_committed->attempt.token != inspection.value.last_issued->token,
            "failure did not retire the real attempt while preserving the preceding commit");
}

void require_success_and_consume(Fixture& fixture, const ProcessInstance& before, const StateStore& instance_before,
                                 FeatureNodeId winner, unsigned expected_callbacks = 1) {
    require_prepared_once(fixture, winner.value(), expected_callbacks);
    const auto* process = fixture.runtime.find_process(fixture.process);
    require(process && process->current_state == "ChoiceB" && process->status == ProcessStatus::kCompleted &&
            process->revision == ProcessRevision(before.revision.value() + 1) &&
            process->state.version == StateVersion(before.state.version.value() + 1) &&
            process->state.commit_count == before.state.commit_count + 1 && process->state.payload.size() == kPayloadSize &&
            validate_payload(process->state.payload) && read_u64(process->state.payload, 4) == winner.value() &&
            read_u64(process->state.payload, 16) == 1 &&
            read_u64(process->state.payload, 24) == before.state.version.value(),
            "real owner did not atomically commit exactly one Exit/State/Enter transition");
    require(same_state(fixture.runtime.find_instance(fixture.owner)->state, instance_before),
            "Process transition wrote the separate instance state store");
    const auto inspection = fixture.runtime.inspect_invocations(fixture.owner);
    succeeds(inspection, "inspect actual successful Process attempt");
    require(inspection.value.open_attempts.empty() && inspection.value.last_issued && inspection.value.last_committed &&
            inspection.value.last_committed->attempt.token == inspection.value.last_issued->token &&
            inspection.value.last_committed->attempt.process == fixture.process &&
            inspection.value.last_committed->state_version == process->state.version &&
            inspection.value.last_committed->process_revision == process->revision,
            "actual Process commit provenance differs from the issued winner attempt");
    const auto outputs = fixture.capture();
    succeeds(outputs, "capture ordered winner outbox");
    require(outputs.value.size() == 2 && fixture.consumer.applied == 0 && fixture.consumer.entries_seen == 0,
            "actions applied a side effect or the commit published more than one transition's commands");
    fixture.consumer.require_enter = true;
    fixture.consumer.expected_state = "ChoiceB";
    fixture.consumer.expected_status = ProcessStatus::kCompleted;
    fixture.consumer.expected_version = process->state.version;
    fixture.consumer.expected_revision = process->revision;
    fixture.consumer.expected_winner = winner;
    fixture.consumer.prior_state_version = before.state.version.value();
    for (unsigned index = 0; index != 2; ++index) {
        const auto receipt = outputs.value[index].receipt();
        require(outputs.value[index].invocation() &&
                *outputs.value[index].invocation() == inspection.value.last_issued->token &&
                outputs.value[index].entry_index() == index && receipt.state == Delivery::kPending &&
                outputs.value[index].intent()->payload[0] == (index == 0 ? kFirstCommand : kSecondCommand),
                "journal changed selected command order or real invocation provenance");
        fixture.consume(outputs.value[index]);
        require(outputs.value[index].receipt().state == Delivery::kApplied,
                "actual consumer did not settle its canonical delivery");
    }
    require(fixture.consumer.applied == 2 && fixture.consumer.entries_seen == 2 &&
            fixture.consumer.entered_before_apply == 2 && fixture.consumer.commands[0] == kFirstCommand &&
            fixture.consumer.commands[1] == kSecondCommand,
            "ordered actual applications did not both observe Enter already committed");
    for (const auto& handle : outputs.value)
        require(!Access::consume(fixture.runtime, fixture.binding, handle, apply, &fixture.consumer),
                "committed transition command was applied twice");
    const auto empty = fixture.capture();
    succeeds(empty, "capture drained winner journal");
    require(empty.value.empty() && fixture.consumer.applied == 2 && fixture.consumer.entries_seen == 2,
            "winner replay changed actual effects or retained a terminal delivery");
}

void test_choice(std::string_view name, const std::vector<Transition>& table, FeatureNodeId expected) {
    for (const bool reverse : {false, true}) {
        auto order = table;
        if (reverse) std::reverse(order.begin(), order.end());
        Fixture fixture(std::move(order));
        const auto before = *fixture.runtime.find_process(fixture.process);
        const auto instance_before = fixture.runtime.find_instance(fixture.owner)->state;
        succeeds(fixture.dispatch(), "dispatch actual multi-enabled transition table");
        require_success_and_consume(fixture, before, instance_before, expected);
        std::cout << "Process transition probe=" << name << " reversed=" << reverse
                  << " selected=" << expected.value() << " committed=1 applied=2\n";
    }
}

void prepare_async(Fixture& fixture, Gate& gate, unsigned expected_callbacks) {
    fixture.script.gate = &gate;
    succeeds(fixture.runtime.enable_async_execution(), "enable real Process worker execution");
    succeeds(fixture.dispatch(), "dispatch real gated Process candidate");
    succeeds(gate.prepared.completion().wait(kWaitNs), "wait for actual prepared transition candidate");
    require(fixture.script.callbacks.load() == expected_callbacks &&
            fixture.script.callback_thread.load() != aeg::concurrency::current_thread_id().value &&
            fixture.runtime.pending_execution_count() == 1, "candidate was not prepared by an actual outstanding worker");
    const auto inspection = fixture.runtime.inspect_invocations(fixture.owner);
    succeeds(inspection, "inspect gated actual Process candidate");
    require(inspection.value.open_attempts.size() == 1 && inspection.value.open_attempts[0].committable &&
            inspection.value.open_attempts[0].attempt.process == fixture.process,
            "prepared candidate has no live committable Process attempt");
}

void test_async_exit_state_enter_outbox() {
    Gate gate; // Outlives Fixture even when an assertion throws while the worker is gated.
    Fixture fixture(node_table());
    const auto before = *fixture.runtime.find_process(fixture.process);
    const auto instance_before = fixture.runtime.find_instance(fixture.owner)->state;
    prepare_async(fixture, gate, 1);
    require_prepared_once(fixture, 0x1'0000'0007ull, 1);
    const auto before_commit = fixture.capture();
    succeeds(before_commit, "capture journal while complete candidate is gated");
    require(same_process(*fixture.runtime.find_process(fixture.process), before) &&
            same_state(fixture.runtime.find_instance(fixture.owner)->state, instance_before) &&
            before_commit.value.empty() && fixture.consumer.applied == 0 && fixture.consumer.entries_seen == 0,
            "Exit/State/Enter preparation leaked live state or an actual application before owner commit");
    succeeds(gate.release.complete(), "release prepared candidate to the owner");
    succeeds(fixture.pump_until_idle(), "commit actual prepared Process on owner");
    require_success_and_consume(fixture, before, instance_before, FeatureNodeId(0x1'0000'0007ull));
    succeeds(fixture.runtime.pump_completions(), "repeat settled Process completion pump");
    require(fixture.runtime.find_process(fixture.process)->state.commit_count == 1 && fixture.consumer.applied == 2,
            "repeat completion pumping committed or applied the same transition twice");
}

void test_bad_state_validation() {
    Fixture fixture(node_table());
    const auto baseline = seed_nonempty_baseline(fixture);
    fixture.script.mode = Mode::kBadState;
    const auto rejected = fixture.dispatch();
    require(!rejected && rejected.code == ResultCode::kInvalidArgument &&
            rejected.message == "fixture state schema rejects malformed transition payload",
            "malformed state did not reach and fail the actual owner schema callback");
    require_prepared_once(fixture, 0x1'0000'0007ull, 2);
    require_failed_commit(fixture, ResultCode::kInvalidArgument);
    require_preserved(fixture, baseline);
}

void test_invalid_last_intent() {
    Fixture fixture(node_table());
    const auto baseline = seed_nonempty_baseline(fixture);
    fixture.script.mode = Mode::kInvalidLastIntent;
    const auto rejected = fixture.dispatch();
    require(!rejected && rejected.code == ResultCode::kInvalidArgument,
            "undeclared last command version did not reject the actual commit");
    require_prepared_once(fixture, 0x1'0000'0007ull, 2);
    require_failed_commit(fixture, ResultCode::kInvalidArgument);
    require_preserved(fixture, baseline);
}

void test_late_reject_commit() {
    Gate gate;
    Fixture fixture(node_table());
    const auto baseline = seed_nonempty_baseline(fixture);
    prepare_async(fixture, gate, 2);
    require_prepared_once(fixture, 0x1'0000'0007ull, 2);
    require_preserved(fixture, baseline);
    fixture.now.store(kInitialNs + kDeadlineNs + 1); // Only this monotonic virtual jump makes the result late.
    const auto late = fixture.runtime.inspect_invocations(fixture.owner);
    succeeds(late, "inspect actual now-late candidate");
    require(late.value.open_attempts.size() == 1 && !late.value.open_attempts[0].committable,
            "RejectCommit candidate remained committable after its virtual deadline");
    succeeds(gate.release.complete(), "release actual late candidate");
    const auto rejected = fixture.pump_until_idle();
    require(!rejected && rejected.code == ResultCode::kTimeout,
            "actual owner did not reject the prepared late result with typed Timeout");
    require_failed_commit(fixture, ResultCode::kTimeout);
    require_preserved(fixture, baseline);
}

void test_cancel_then_late_completion() {
    Gate gate;
    Fixture fixture(node_table());
    const auto baseline = seed_nonempty_baseline(fixture);
    prepare_async(fixture, gate, 2);
    require_prepared_once(fixture, 0x1'0000'0007ull, 2);
    require_preserved(fixture, baseline);
    succeeds(fixture.runtime.cancel(fixture.process), "cancel actual prepared Process");
    auto cancelled = baseline;
    cancelled.process.status = ProcessStatus::kCancelled;
    cancelled.process.revision = ProcessRevision(baseline.process.revision.value() + 1);
    require_preserved(fixture, cancelled); // cancel() itself legitimately changes lifecycle/revision only.
    fixture.now.store(kInitialNs + kDeadlineNs + 1);
    const auto retired = fixture.runtime.inspect_invocations(fixture.owner);
    succeeds(retired, "inspect actual cancelled candidate");
    require(retired.value.open_attempts.size() == 1 && !retired.value.open_attempts[0].committable,
            "cancelled Process candidate remained committable");
    succeeds(gate.release.complete(), "release actual cancelled late completion");
    succeeds(fixture.pump_until_idle(), "retire cancelled late result as cleanup");
    require_preserved(fixture, cancelled);
    const auto settled = fixture.runtime.inspect_invocations(fixture.owner);
    succeeds(settled, "inspect settled cancelled Process");
    require(settled.value.open_attempts.empty() && settled.value.last_committed &&
            settled.value.last_committed->attempt.token == baseline.applied_record.token &&
            fixture.script.callbacks.load() == 2 && fixture.script.candidates_prepared.load() == 1,
            "cancelled completion escaped retirement, replaced last good commit, or reran an action");
}

} // namespace

int main() try {
    test_choice("priority-descending", priority_table(), FeatureNodeId(0x3'0000'0007ull));
    test_choice("causal-ascending", causal_table(), FeatureNodeId(0x3'0000'0007ull));
    test_choice("full64-node-ascending", node_table(), FeatureNodeId(0x1'0000'0007ull));
    test_async_exit_state_enter_outbox();
    test_bad_state_validation();
    test_invalid_last_intent();
    test_late_reject_commit();
    test_cancel_then_late_completion();
    std::cout << "Process transition order: 11 real Runtime cases passed; guards/arbitration, "
                 "Exit/State/Enter/Outbox and nonempty rollback baselines\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Process transition order: " << error.what() << '\n';
    return 74;
}
