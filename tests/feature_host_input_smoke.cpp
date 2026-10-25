/** @brief Engine-private host input leases project immutable bytes into actual Feature invocations. */
#include "feature_clock_test_fixture.hpp"
#include "feature_input.hpp"
#include "feature_execution_control.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "concurrency.hpp"
#include "feature_graph.hpp"

namespace {
using namespace aeg;
using namespace aeg::feature;
using namespace aeg::feature::detail;
using Access = FeatureInputAccess;
using Binding = FeatureInputBinding;
using Context = FeatureInputContext;
using Projection = FeatureInputProjection;

constexpr FeatureDefinitionId kSender{"HostInput.Sender"}, kTarget{"HostInput.Target"};
constexpr FeatureNodeId kSenderNode{"Sender"}, kTargetNode{"Target"};
constexpr PortId kOut{"Output"}, kChannel{"Channel"}, kHost{"Host"}, kWrongDirection{"OutputHost"};
constexpr ValueSchemaId kByte{"HostInput.Byte"}, kHostSchema{"HostInput.Opaque"};
constexpr ClockId kSenderClock{"HostInput.SourceClock"}, kTargetClock{"HostInput.TargetClock"};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void succeeds(const foundation::Result& result, const char* message) {
    require(bool(result), std::string(message) + ": " + result.message);
}
template <typename T>
void succeeds(const foundation::ResultValue<T>& result, const char* message) {
    succeeds(result.result, message);
}

struct Invocation final {
    InvocationToken token{};
    std::vector<uint8_t> bytes{};
    concurrency::ThreadId thread{};
};
struct Script final {
    std::mutex mutex{};
    std::vector<uint8_t> outgoing{};
    std::vector<Invocation> calls{};
    std::atomic_bool fail_first{false};
    std::atomic_uint32_t attempts{0};
    concurrency::Completion invocation_gate{};
    std::atomic_uint32_t gated_entries{0};

    std::vector<Invocation> observations() {
        std::scoped_lock lock(mutex);
        return calls;
    }
};

FeatureDefinition definition(FeatureDefinitionId id, ClockId clock, ExecutionKind kind) {
    FeatureDefinition result{};
    result.id = id; result.version = {1, 0, 0}; result.owner_module = ModuleIdentity("HostInput.Module");
    auto& contract = result.contract;
    contract.definition_id = id; contract.version = result.version;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = kind; contract.state = StatePolicy::kInstanceState;
    contract.scope = {ScopeId("World")};
    contract.clock = aeg::feature::make_periodic_clock_contract(clock, 50, 8, LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    return result;
}

struct Fixture final {
    Script script{};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry};
    FeatureGraphDocument graph{};
    CompiledFeatureGraph compiled{};
    FeatureInstanceId sender{}, target{};

    explicit Fixture(ExecutionKind target_kind = ExecutionKind::kCall, bool retry = false,
                     uint32_t retry_backoff_ticks = 2) {
        auto source = definition(kSender, kSenderClock, ExecutionKind::kCall);
        source.contract.domains = to_mask(FeatureDomain::kStream);
        source.contract.ports = {{kOut, PortDirection::kOutput, kByte, 1}};
        source.execute = [this](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                                std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
            StateDelta delta{token.state_version};
            PortMessage message{};
            message.source_port = kOut; message.schema = kByte; message.schema_version = 1;
            {
                std::scoped_lock lock(script.mutex);
                message.payload = script.outgoing;
            }
            delta.output_messages = {std::move(message)};
            return {foundation::Result::success(), std::move(delta)};
        };
        auto consumer = definition(kTarget, kTargetClock, target_kind);
        consumer.contract.ports = {{kHost, PortDirection::kInput, kHostSchema, 1},
                                   {kChannel, PortDirection::kInput, kByte, 1},
                                   {kWrongDirection, PortDirection::kOutput, kHostSchema, 1}};
        if (retry) {
            consumer.contract.failure = FailurePolicy::kRetry;
            consumer.contract.retry = {2, retry_backoff_ticks};
            script.fail_first = true;
        }
        consumer.execute = [this](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input,
                                  std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
            if (script.invocation_gate.is_valid()) {
                script.gated_entries.fetch_add(1);
                const auto ready = script.invocation_gate.wait(2'000'000'000);
                if (!ready) return {ready, {}};
            }
            const auto attempt = script.attempts.fetch_add(1);
            {
                std::scoped_lock lock(script.mutex);
                script.calls.push_back({token, {input.begin(), input.end()}, concurrency::current_thread_id()});
            }
            if (script.fail_first.load() && attempt == 0)
                return {foundation::not_ready("intentional host input retry"), {}};
            return {foundation::Result::success(), StateDelta{token.state_version}};
        };
        succeeds(aeg::feature::test::register_clock_test_definition(registry, std::move(source)), "register actual Channel producer");
        succeeds(aeg::feature::test::register_clock_test_definition(registry, std::move(consumer)), "register actual host input consumer");
        graph.asset_uuid = "fixture://feature-host-input"; graph.schema_version = 1; graph.graph_version = 1;
        graph.contract_digest = registry.contract_digest();
        graph.nodes = {{kSenderNode, kSender, {1, 0, 0}, {}, {}}, {kTargetNode, kTarget, {1, 0, 0}, {}, {}}};
        graph.edges = {{kSenderNode, kOut, kTargetNode, kChannel, EdgeKind::kQueue, kSenderClock, kTargetClock, 1, 8, 0}};
        publish();
    }

    void publish() {
        auto candidate = FeatureGraphCompiler{}.compile(graph, registry);
        succeeds(candidate, "compile actual host input graph");
        succeeds(runtime.publish_graph(graph, candidate.value), "publish actual host input graph");
        compiled = std::move(candidate.value);
        sender = {}; target = {};
        for (const auto& row : compiled.provenance) {
            if (row.node_id == kSenderNode) sender = row.instance_id;
            if (row.node_id == kTargetNode) target = row.instance_id;
        }
        require(sender.is_valid() && target.is_valid() && sender != target, "actual graph owners are missing or aliased");
    }
    void send(uint8_t byte) {
        {
            std::scoped_lock lock(script.mutex);
            script.outgoing = {byte};
        }
        succeeds(runtime.dispatch_call(sender), "execute actual Channel producer");
    }
    void drain() {
        for (unsigned attempt = 0; attempt != 2000 && runtime.pending_execution_count() != 0; ++attempt) {
            succeeds(runtime.pump_completions(), "pump actual async input completion");
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        require(runtime.pending_execution_count() == 0, "async input completion did not settle");
    }
};

struct ProjectedContext final {
    FeatureInstanceId owner{};
    RuntimeGeneration generation{};
    ClockId clock{};
    double elapsed{};
    std::vector<uint8_t> routed{};
};
struct Projector final {
    bool ready{true};
    uint8_t prefix{0xa1};
    std::vector<ProjectedContext> calls{};
    concurrency::ThreadId owner_thread{concurrency::current_thread_id()};

    foundation::ResultValue<Projection> operator()(const Context& context) {
        require(concurrency::current_thread_id() == owner_thread, "host projector ran outside the Runtime owner");
        calls.push_back({context.owner, context.generation, context.clock, context.elapsed_seconds,
                         {context.routed_input.begin(), context.routed_input.end()}});
        Projection result{};
        result.ready = ready;
        result.bytes = {prefix};
        result.bytes.insert(result.bytes.end(), context.routed_input.begin(), context.routed_input.end());
        return {foundation::Result::success(), std::move(result)};
    }
};

auto bind(Fixture& fixture, Projector& projector) {
    return Access::bind(fixture.runtime, fixture.target, kHost, kHostSchema, 1,
        [&projector](const Context& context) { return projector(context); });
}

void binding_validates_actual_port_and_owner_thread() {
    Fixture fixture;
    Projector projector;
    const auto callback = [&projector](const Context& context) { return projector(context); };
    require(!Access::bind(fixture.runtime, {}, kHost, kHostSchema, 1, callback), "missing host input owner was accepted");
    require(!Access::bind(fixture.runtime, fixture.target, PortId("Absent"), kHostSchema, 1, callback),
            "undeclared host input port was accepted");
    require(!Access::bind(fixture.runtime, fixture.target, kWrongDirection, kHostSchema, 1, callback),
            "output port was accepted for host input");
    require(!Access::bind(fixture.runtime, fixture.target, kHost, kByte, 1, callback), "wrong host input schema was accepted");
    require(!Access::bind(fixture.runtime, fixture.target, kHost, kHostSchema, 0, callback) &&
            !Access::bind(fixture.runtime, fixture.target, kHost, kHostSchema, 2, callback),
            "zero or mismatched host input schema version was accepted");
    require(!Access::bind(fixture.runtime, fixture.target, kHost, kHostSchema, 1, FeatureInputProjector{}),
            "empty host projector was accepted");
    foundation::Result foreign;
    concurrency::ThreadId foreign_thread{};
    concurrency::WorkerExecutor worker({concurrency::ExecutorDomain::kCpuWorker, 1, false});
    concurrency::TaskDesc task{};
    task.domain = concurrency::ExecutorDomain::kCpuWorker;
    task.execute = [&](const concurrency::TaskContext&) {
        foreign_thread = concurrency::current_thread_id();
        auto result = Access::bind(fixture.runtime, fixture.target, kHost, kHostSchema, 1, callback);
        foreign = result.result;
        return foundation::Result::success();
    };
    const auto submitted = worker.submit(std::move(task));
    succeeds(submitted, "submit foreign host input binding probe");
    succeeds(submitted.value.wait(2'000'000'000), "wait for foreign host input binding probe");
    require(foreign_thread != concurrency::current_thread_id() && !foreign &&
            foreign.code == foundation::ResultCode::kInvalidState && projector.calls.empty(),
            "foreign-thread binding accessed Runtime authority or invoked the projector");
    auto bound = bind(fixture, projector);
    succeeds(bound, "bind actual versioned input port");
    require(!bind(fixture, projector), "duplicate host input binding was accepted");
    Binding moved = std::move(bound.value);
    succeeds(fixture.runtime.dispatch_call(fixture.target), "invoke moved host input lease");
    require(projector.calls.size() == 1, "moving the lease retired its actual binding");
}

void projection_preserves_unbound_bytes_and_consumes_actual_channel() {
    Fixture fixture;
    fixture.send(0x11);
    const auto* target = fixture.runtime.find_instance(fixture.target);
    require(target && target->pending_messages.size() == 1 && target->pending_messages.front().delivery_id.is_valid() &&
            target->pending_messages.front().source.instance == fixture.sender &&
            target->pending_messages.front().target_port == kChannel,
            "real producer did not create an actual Channel delivery");
    succeeds(fixture.runtime.dispatch_call(fixture.target), "dispatch unbound input consumer");
    require(fixture.script.observations().back().bytes == std::vector<uint8_t>{0x11} &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.empty(), "unbound raw input semantics changed");
    Projector projector;
    auto bound = bind(fixture, projector);
    succeeds(bound, "bind projected Channel consumer");
    fixture.send(0x22);
    succeeds(fixture.runtime.dispatch_call(fixture.target), "dispatch projected input consumer");
    require(fixture.script.observations().back().bytes == std::vector<uint8_t>({0xa1, 0x22}) &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.empty() && projector.calls.size() == 1,
            "projection lost/duplicated the actual Channel or failed to consume its accepted delivery");
    const auto& context = projector.calls.back();
    require(context.owner == fixture.target && context.generation == fixture.runtime.generation() &&
            context.clock == kTargetClock && context.elapsed == 0.0 && context.routed == std::vector<uint8_t>{0x22},
            "direct dispatch did not project the actual owner/generation/clock/raw Channel with zero elapsed");
    bound.value = Binding{};
    fixture.send(0x33);
    const auto delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
    const auto before = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(before, "inspect invocation before retired input lease dispatch");
    require(!fixture.runtime.dispatch_call(fixture.target), "retired host input lease silently fell back to raw input");
    const auto after = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(after, "inspect invocation after retired input lease dispatch");
    target = fixture.runtime.find_instance(fixture.target);
    require(projector.calls.size() == 1 && fixture.script.observations().size() == 2 &&
            before.value.last_issued && after.value.last_issued &&
            before.value.last_issued->token == after.value.last_issued->token &&
            target->pending_messages.size() == 1 && target->pending_messages.front().delivery_id == delivery &&
            target->pending_input == std::vector<uint8_t>{0x33},
            "expired input lease issued an attempt, invoked a callback, or changed its actual Channel delivery");
    auto rebound = bind(fixture, projector);
    succeeds(rebound, "replace expired input lease with fresh binding");
    succeeds(fixture.runtime.dispatch_call(fixture.target), "dispatch after fresh host input binding");
    require(projector.calls.size() == 2 &&
            fixture.script.observations().back().bytes == std::vector<uint8_t>({0xa1, 0x33}) &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.empty(),
            "fresh input binding did not project and consume the preserved actual Channel delivery");
}

void clock_waits_without_attempts_and_preserves_backlog() {
    Fixture fixture(ExecutionKind::kContinuous);
    Projector projector;
    projector.ready = false;
    auto bound = bind(fixture, projector);
    succeeds(bound, "bind clock input readiness gate");
    fixture.send(0x41);
    const auto delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
    succeeds(fixture.runtime.tick(kTargetClock), "defer first real clock step");
    succeeds(fixture.runtime.tick(kTargetClock), "queue second real clock step behind host readiness");
    const auto waiting = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(waiting, "inspect host-deferred owner");
    require(!waiting.value.last_issued && !waiting.value.last_committed && waiting.value.open_attempts.empty() &&
            fixture.script.observations().empty() && fixture.runtime.find_instance(fixture.target)->state.commit_count == 0 &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id == delivery,
            "not-ready projection issued an attempt, consumed a Channel, or committed state");
    projector.ready = true;
    for (unsigned attempt = 0; attempt != 32 && fixture.runtime.find_instance(fixture.target)->state.commit_count < 2; ++attempt)
        succeeds(fixture.runtime.advance(0.0), "resume deferred clock work without advancing time");
    const auto actual = fixture.script.observations();
    require(actual.size() == 2 && fixture.runtime.find_instance(fixture.target)->state.commit_count == 2 &&
            actual[0].bytes == std::vector<uint8_t>({0xa1, 0x41}) && actual[1].bytes == std::vector<uint8_t>{0xa1} &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.empty(),
            "host readiness lost/duplicated a due clock step or reused a consumed Channel");
    require(!projector.calls.empty(), "clock owner bypassed the bound host projector");
    for (const auto& context : projector.calls)
        require(context.clock == kTargetClock && std::abs(context.elapsed - 1.0 / 50.0) < 1e-12,
                "clock-issued host input lost its actual one-period elapsed value");
    succeeds(fixture.runtime.advance(0.0), "pump settled host input clock");
    require(fixture.script.observations().size() == 2, "zero-time pump invented another Feature tick");
}

void async_retry_keeps_projected_bytes_and_old_delivery() {
    Fixture fixture(ExecutionKind::kCall, true);
    Projector projector;
    auto bound = bind(fixture, projector);
    succeeds(bound, "bind retry host input");
    fixture.send(0x51);
    const auto old_delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
    succeeds(fixture.runtime.enable_async_execution(), "enable actual async host input execution");
    succeeds(fixture.runtime.dispatch_call(fixture.target), "submit actual projected async invocation");
    projector.prefix = 0xb2;
    fixture.drain();
    const auto failed = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(failed, "inspect actual projected retry");
    require(failed.value.retry && failed.value.last_failed && !failed.value.last_committed &&
            failed.value.open_attempts.empty() && projector.calls.size() == 1,
            "failed projected attempt did not become one real owner retry");
    fixture.send(0x52);
    fixture.drain();
    const auto* pending = fixture.runtime.find_instance(fixture.target);
    require(pending->pending_messages.size() == 2 && pending->pending_messages.front().delivery_id == old_delivery &&
            pending->pending_messages.back().payload == std::vector<uint8_t>{0x52},
            "new Channel replaced the retry's captured delivery");
    for (unsigned step = 0; step != 8 && fixture.runtime.find_instance(fixture.target)->retry_pending; ++step) {
        succeeds(fixture.runtime.tick(kTargetClock), "advance actual retry backoff");
        fixture.drain();
    }
    const auto calls = fixture.script.observations();
    const auto committed = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(committed, "inspect accepted projected retry");
    require(calls.size() == 2 && projector.calls.size() == 1 &&
            calls[0].bytes == std::vector<uint8_t>({0xa1, 0x51}) && calls[1].bytes == calls[0].bytes &&
            calls[0].thread != concurrency::current_thread_id() && calls[1].thread != concurrency::current_thread_id() &&
            calls[0].token.state_version == calls[1].token.state_version &&
            calls[0].token.attempt_id != calls[1].token.attempt_id && committed.value.last_committed &&
            committed.value.last_committed->attempt.token == calls[1].token,
            "retry refreshed projected bytes/Channel input or lost its actual immutable token state");
    pending = fixture.runtime.find_instance(fixture.target);
    require(pending->pending_messages.size() == 1 && pending->pending_messages.front().payload == std::vector<uint8_t>{0x52} &&
            pending->pending_messages.front().delivery_id != old_delivery,
            "retry consumed a later Channel or retained its already accepted old delivery");
    succeeds(fixture.runtime.dispatch_call(fixture.target), "submit fresh post-retry host projection");
    fixture.drain();
    require(projector.calls.size() == 2 && projector.calls.back().routed == std::vector<uint8_t>{0x52} &&
            fixture.script.observations().back().bytes == std::vector<uint8_t>({0xb2, 0x52}) &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.empty(),
            "fresh invocation did not project the new host state and remaining Channel");
}

void leases_do_not_cross_generation_or_runtime_lifetime() {
    Fixture fixture;
    Projector old_projector, fresh_projector;
    auto old = bind(fixture, old_projector);
    succeeds(old, "bind preceding generation input");
    const auto old_generation = fixture.runtime.generation();
    ++fixture.graph.graph_version;
    fixture.publish();
    require(fixture.runtime.generation() != old_generation, "graph republication did not produce an actual new generation");
    auto fresh = bind(fixture, fresh_projector);
    succeeds(fresh, "bind current owner while preceding lease remains alive");
    old.value = Binding{};
    fixture.send(0x61);
    succeeds(fixture.runtime.dispatch_call(fixture.target), "execute current owner after preceding lease retirement");
    require(old_projector.calls.empty() && fresh_projector.calls.size() == 1 &&
            fresh_projector.calls.front().generation == fixture.runtime.generation() &&
            fixture.script.observations().back().bytes == std::vector<uint8_t>({0xa1, 0x61}),
            "retiring an old generation lease erased or invoked the current binding");
    Binding survivor;
    {
        auto dying = std::make_unique<Fixture>();
        auto result = Access::bind(dying->runtime, dying->target, kHost, kHostSchema, 1,
            [](const Context&) -> foundation::ResultValue<Projection> {
                Projection projected{}; projected.bytes = {0x71};
                return {foundation::Result::success(), std::move(projected)};
            });
        succeeds(result, "bind lease that will outlive its Runtime");
        survivor = std::move(result.value);
        dying.reset();
    }
    survivor = Binding{};
}

void direct_busy_and_suspended_rejections_do_not_project() {
    size_t busy_projections = 0;
    {
        Fixture fixture;
        Projector projector;
        auto bound = bind(fixture, projector);
        succeeds(bound, "bind busy direct-dispatch owner");
        fixture.send(0x81);
        const auto delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
        concurrency::CompletionSource release;
        fixture.script.invocation_gate = release.completion();
        succeeds(fixture.runtime.enable_async_execution(), "enable worker for busy direct-dispatch guard");
        succeeds(fixture.runtime.dispatch_call(fixture.target), "submit gated actual owner invocation");
        const auto before = fixture.runtime.inspect_invocations(fixture.target);
        succeeds(before, "inspect actual in-flight invocation");
        require(before.value.last_issued && before.value.open_attempts.size() == 1 &&
                fixture.runtime.pending_execution_count() == 1 && projector.calls.size() == 1,
                "busy guard did not begin with one actual submitted invocation and one projection");
        const auto rejected = fixture.runtime.dispatch_call(fixture.target);
        const auto after = fixture.runtime.inspect_invocations(fixture.target);
        busy_projections = projector.calls.size();
        const auto* waiting = fixture.runtime.find_instance(fixture.target);
        const bool unchanged_delivery = waiting->pending_messages.size() == 1 &&
            waiting->pending_messages.front().delivery_id == delivery &&
            waiting->pending_input == std::vector<uint8_t>{0x81};
        succeeds(release.complete(), "release actual worker after rejected busy dispatch");
        fixture.drain();
        succeeds(after, "inspect owner after busy direct rejection");
        require(!rejected && rejected.code == foundation::ResultCode::kInvalidState &&
                after.value.last_issued && after.value.open_attempts.size() == 1 &&
                after.value.last_issued->token == before.value.last_issued->token && unchanged_delivery &&
                fixture.script.observations().size() == 1 &&
                fixture.script.observations().front().bytes == std::vector<uint8_t>({0xa1, 0x81}) &&
                fixture.runtime.find_instance(fixture.target)->pending_messages.empty(),
                "busy rejection issued work, changed its Channel, or damaged the accepted worker invocation");
    }
    size_t suspended_projections = 0;
    {
        Fixture fixture(ExecutionKind::kProcess);
        Projector projector;
        auto bound = bind(fixture, projector);
        succeeds(bound, "bind suspended direct-dispatch Process");
        fixture.send(0x82);
        const auto delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
        const auto process = fixture.runtime.process(fixture.target);
        succeeds(process, "find actual Process to suspend");
        succeeds(fixture.runtime.suspend(process.value), "suspend actual input-bound Process");
        const auto rejected = fixture.runtime.dispatch_process(fixture.target);
        const auto after = fixture.runtime.inspect_invocations(fixture.target);
        succeeds(after, "inspect suspended owner after direct rejection");
        suspended_projections = projector.calls.size();
        const auto* waiting = fixture.runtime.find_instance(fixture.target);
        require(!rejected && rejected.code == foundation::ResultCode::kInvalidState &&
                !after.value.last_issued && after.value.open_attempts.empty() &&
                fixture.script.observations().empty() && waiting->pending_messages.size() == 1 &&
                waiting->pending_messages.front().delivery_id == delivery &&
                waiting->pending_input == std::vector<uint8_t>{0x82} &&
                fixture.runtime.find_process(process.value)->status == ProcessStatus::kSuspended,
                "suspended rejection issued an attempt, consumed a Channel, or changed Process lifecycle");
        succeeds(fixture.runtime.resume(process.value), "resume actual input-bound Process");
        succeeds(fixture.runtime.dispatch_process(fixture.target), "dispatch actual resumed Process");
        require(projector.calls.size() == suspended_projections + 1 &&
                fixture.script.observations().size() == 1 &&
                fixture.script.observations().front().bytes == std::vector<uint8_t>({0xa1, 0x82}) &&
                fixture.runtime.find_instance(fixture.target)->pending_messages.empty(),
                "resumed Process failed to project and consume its preserved actual Channel");
    }
    std::cout << "DIRECT_INPUT_REJECTION busy_projections=" << busy_projections << " expected=1"
              << " suspended_projections=" << suspended_projections << " expected=0\n";
    require(busy_projections == 1 && suspended_projections == 0,
            "rejected busy/suspended direct dispatch unnecessarily invoked its host input projector");
}

void private_drain_finishes_issued_work_without_admitting_clock_backlog() {
    Fixture fixture(ExecutionKind::kContinuous);
    Projector projector;
    auto bound = bind(fixture, projector);
    succeeds(bound, "bind private-drain clock owner");
    fixture.send(0x91);
    concurrency::CompletionSource release;
    fixture.script.invocation_gate = release.completion();
    succeeds(fixture.runtime.enable_async_execution(), "enable actual private-drain worker");
    succeeds(fixture.runtime.tick(kTargetClock), "issue first real clock quantum before Pause drain");
    for (unsigned attempt = 0; fixture.script.gated_entries.load() == 0 && attempt != 2000; ++attempt)
        static_cast<void>(concurrency::sleep_for(1'000'000));
    require(fixture.script.gated_entries.load() == 1, "private-drain worker did not enter its actual gate");
    succeeds(fixture.runtime.tick(kTargetClock), "queue second actual clock quantum behind gated work");
    const auto first = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(first, "inspect issued work before Pause drain");
    require(first.value.last_issued && first.value.open_attempts.size() == 1 && projector.calls.size() == 1 &&
            fixture.runtime.find_instance(fixture.target)->state.commit_count == 0,
            "private-drain fixture did not retain one actual issued invocation and its queued next tick");
    succeeds(release.complete(), "release worker for strict issued-completion drain");
    for (unsigned attempt = 0; fixture.runtime.find_instance(fixture.target)->state.commit_count == 0 && attempt != 2000;
         ++attempt) {
        succeeds(FeatureExecutionControl::drain_issued_completions(fixture.runtime), "drain actual issued clock completion");
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    for (unsigned repeat = 0; repeat != 3; ++repeat)
        succeeds(FeatureExecutionControl::drain_issued_completions(fixture.runtime), "repeat strict Pause completion drain");
    const auto paused = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(paused, "inspect strict Pause-drained owner");
    require(paused.value.last_committed && paused.value.last_issued && paused.value.open_attempts.empty() &&
            paused.value.last_issued->token == first.value.last_issued->token &&
            paused.value.last_committed->attempt.token == first.value.last_issued->token &&
            fixture.runtime.find_instance(fixture.target)->state.commit_count == 1 &&
            fixture.runtime.pending_execution_count() == 0 && projector.calls.size() == 1 &&
            fixture.script.observations().size() == 1 &&
            fixture.script.observations().front().bytes == std::vector<uint8_t>({0xa1, 0x91}) &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.empty(),
            "Pause drain admitted another clock attempt/projection or failed to commit only its issued Channel consumer");
    succeeds(fixture.runtime.advance(0.0), "resume original queued clock quantum without advancing time");
    fixture.drain();
    const auto resumed = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(resumed, "inspect resumed original clock backlog");
    const auto calls = fixture.script.observations();
    require(calls.size() == 2 && projector.calls.size() == 2 &&
            calls.back().bytes == std::vector<uint8_t>{0xa1} &&
            calls.back().token.attempt_id != first.value.last_issued->token.attempt_id &&
            resumed.value.last_committed && resumed.value.last_committed->attempt.token == calls.back().token &&
            fixture.runtime.find_instance(fixture.target)->state.commit_count == 2,
            "strict drain lost the original backlog or resumed it with already consumed Channel bytes");
    succeeds(fixture.runtime.advance(0.0), "verify resumed clock backlog settled");
    require(fixture.script.observations().size() == 2, "strict drain or resume invented an extra clock quantum");
}

void private_drain_keeps_zero_backoff_retry_unissued_and_immutable() {
    Fixture fixture(ExecutionKind::kCall, true, 0);
    Projector projector;
    auto bound = bind(fixture, projector);
    succeeds(bound, "bind private-drain zero-backoff retry owner");
    fixture.send(0x92);
    const auto delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
    concurrency::CompletionSource release;
    fixture.script.invocation_gate = release.completion();
    succeeds(fixture.runtime.enable_async_execution(), "enable private-drain retry worker");
    succeeds(fixture.runtime.dispatch_call(fixture.target), "issue actual projected retry origin");
    for (unsigned attempt = 0; fixture.script.gated_entries.load() == 0 && attempt != 2000; ++attempt)
        static_cast<void>(concurrency::sleep_for(1'000'000));
    require(fixture.script.gated_entries.load() == 1, "retry origin did not enter its actual worker gate");
    const auto first = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(first, "inspect actual retry origin before strict drain");
    require(first.value.last_issued && first.value.open_attempts.size() == 1, "private-drain retry origin was not issued");
    succeeds(release.complete(), "release actual first failure for strict drain");
    auto paused = fixture.runtime.inspect_invocations(fixture.target);
    for (unsigned attempt = 0; paused && !paused.value.last_failed && attempt != 2000; ++attempt) {
        succeeds(FeatureExecutionControl::drain_issued_completions(fixture.runtime), "drain zero-backoff failure without retrying");
        static_cast<void>(concurrency::sleep_for(1'000'000));
        paused = fixture.runtime.inspect_invocations(fixture.target);
    }
    succeeds(paused, "inspect strict-drained retry state");
    require(paused.value.last_failed && paused.value.retry && paused.value.retry->next_attempt == 2 &&
            paused.value.last_issued && paused.value.last_issued->token == first.value.last_issued->token &&
            paused.value.open_attempts.empty() && !paused.value.last_committed &&
            fixture.runtime.pending_execution_count() == 0 && fixture.script.observations().size() == 1 &&
            projector.calls.size() == 1,
            "strict Pause drain issued a zero-backoff retry or failed to retain the actual failed attempt");
    projector.prefix = 0xb2;
    fixture.send(0x93);
    for (unsigned attempt = 0; fixture.runtime.pending_execution_count() != 0 && attempt != 2000; ++attempt) {
        succeeds(FeatureExecutionControl::drain_issued_completions(fixture.runtime), "drain actual newer Channel producer only");
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    const auto* waiting = fixture.runtime.find_instance(fixture.target);
    require(fixture.runtime.pending_execution_count() == 0 && waiting->pending_messages.size() == 2 &&
            waiting->pending_messages.front().delivery_id == delivery &&
            waiting->pending_messages.back().payload == std::vector<uint8_t>{0x93} &&
            fixture.script.observations().size() == 1 && projector.calls.size() == 1,
            "strict drain admitted retry while retiring another owner or damaged its captured/later Channels");
    succeeds(fixture.runtime.pump_completions(), "restore normal completion pump after Pause");
    succeeds(fixture.runtime.tick(kTargetClock), "resume zero-backoff retry through its actual clock");
    fixture.drain();
    const auto retried = fixture.script.observations();
    const auto accepted = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(accepted, "inspect resumed immutable retry commit");
    require(retried.size() == 2 && projector.calls.size() == 1 &&
            retried.front().bytes == std::vector<uint8_t>({0xa1, 0x92}) && retried.back().bytes == retried.front().bytes &&
            retried.back().token.attempt_id != retried.front().token.attempt_id &&
            retried.back().token.state_version == retried.front().token.state_version &&
            accepted.value.last_committed && accepted.value.last_committed->attempt.token == retried.back().token &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.size() == 1 &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.front().payload == std::vector<uint8_t>{0x93},
            "resumed retry refreshed its projected input or consumed the later actual Channel");
    succeeds(fixture.runtime.dispatch_call(fixture.target), "issue fresh post-Pause invocation");
    fixture.drain();
    require(projector.calls.size() == 2 && fixture.script.observations().size() == 3 &&
            fixture.script.observations().back().bytes == std::vector<uint8_t>({0xb2, 0x93}) &&
            fixture.runtime.find_instance(fixture.target)->pending_messages.empty(),
            "normal admission did not resume fresh projection and actual Channel consumption after strict drain");
}

class TemporaryIoFile final {
public:
    explicit TemporaryIoFile(const char* tag) {
        static uint64_t ordinal = 0;
        file = std::filesystem::temp_directory_path() / ("aeg_clock_readiness_" + std::string(tag) + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(++ordinal) + ".bin");
        require(!std::filesystem::exists(file), "IO fixture temporary path already exists");
    }
    ~TemporaryIoFile() { std::error_code error; std::filesystem::remove(file, error); }
    void write(std::span<const uint8_t> bytes) const {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        require(bool(output), "IO fixture could not write/flush the actual file");
    }
    std::filesystem::path file;
};

class FileReadRequest final {
public:
    explicit FileReadRequest(const std::filesystem::path& file) {
        state->file = file;
        concurrency::TaskDesc task;
        task.domain = concurrency::ExecutorDomain::kIoWorker;
        task.execute = [request = state](const concurrency::TaskContext&) {
            request->thread = concurrency::current_thread_id();
            static_cast<void>(request->started.complete());
            const auto ready = request->release.completion().wait(2'000'000'000);
            if (!ready) return ready;
            std::ifstream input(request->file, std::ios::binary);
            if (!input) return foundation::not_found("actual readiness file open failed");
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (input.bad()) return foundation::invalid_state("actual readiness file read failed");
            std::scoped_lock lock(request->mutex);
            request->bytes = std::move(bytes);
            return foundation::Result::success();
        };
        const auto submitted = worker.submit(std::move(task));
        succeeds(submitted, "submit actual file read on Concurrency IO worker");
        completion = submitted.value;
        succeeds(state->started.completion().wait(2'000'000'000), "actual file read worker did not enter");
    }
    ~FileReadRequest() {
        static_cast<void>(state->release.complete());
        static_cast<void>(worker.wait_idle());
    }
    void finish() {
        succeeds(state->release.complete(), "release actual file read");
        succeeds(completion.wait(2'000'000'000), "actual file read did not complete successfully");
        require(state->thread != owner_thread, "actual file read ran on the Runtime owner");
    }
    foundation::ResultValue<Projection> project(const Context& context) {
        require(concurrency::current_thread_id() == owner_thread, "file projector escaped Runtime owner thread");
        ++projections;
        Projection projected;
        projected.ready = completion.is_complete();
        if (!projected.ready) return {foundation::Result::success(), std::move(projected)};
        const auto result = completion.result();
        if (!result) return {result, {}};
        {
            std::scoped_lock lock(state->mutex);
            projected.bytes = state->bytes;
        }
        projected.bytes.insert(projected.bytes.end(), context.routed_input.begin(), context.routed_input.end());
        return {foundation::Result::success(), std::move(projected)};
    }
    uint32_t projections{0};
private:
    struct Shared final {
        std::filesystem::path file;
        concurrency::CompletionSource started, release;
        std::mutex mutex;
        std::vector<uint8_t> bytes;
        concurrency::ThreadId thread{};
    };
    concurrency::ThreadId owner_thread{concurrency::current_thread_id()};
    std::shared_ptr<Shared> state{std::make_shared<Shared>()};
    concurrency::WorkerExecutor worker{{concurrency::ExecutorDomain::kIoWorker, 1, false}};
    concurrency::Completion completion;
};

constexpr ClockId kIoReadClock{"HostInput.RealIo.Clock"};

struct IoFixture final {
    struct Call final {
        InvocationToken token;
        std::vector<uint8_t> input, state;
        concurrency::ThreadId thread;
    };
    std::mutex mutex;
    std::vector<uint8_t> outgoing;
    std::vector<Call> calls;
    std::function<foundation::ResultValue<StateDelta>(const InvocationToken&, const StateSnapshot&,
                                                     std::span<const uint8_t>)> handler;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry};
    FeatureInstanceId sender{}, target{};

    explicit IoFixture(bool retry = false) {
        auto producer = definition(FeatureDefinitionId{"HostInput.RealIo.Sender"}, kSenderClock, ExecutionKind::kCall);
        producer.contract.ports = {{kOut, PortDirection::kOutput, kByte, 1}};
        producer.execute = [this](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                                  std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
            PortMessage message; message.source_port = kOut; message.schema = kByte; message.schema_version = 1;
            { std::scoped_lock lock(mutex); message.payload = outgoing; }
            StateDelta delta{token.state_version}; delta.output_messages = {std::move(message)};
            return {foundation::Result::success(), std::move(delta)};
        };
        auto stream = definition(FeatureDefinitionId{"HostInput.RealIo.Stream"}, kIoReadClock, ExecutionKind::kProcess);
        stream.contract.domains = to_mask(FeatureDomain::kStream); stream.contract.has_continuation = true;
        stream.contract.ports = {{kHost, PortDirection::kInput, kHostSchema, 1},
                                {kChannel, PortDirection::kInput, kByte, 1}};
        if (retry) {
            stream.contract.failure = FailurePolicy::kRetry; stream.contract.retry = {2, 2};
            // A busy periodic release is dropped here; this case isolates retry admission from queued new releases.
            stream.contract.clock.late_data_policy = LateDataPolicy::kDrop;
        }
        stream.execute = [this](const InvocationToken& token, const StateSnapshot& state, std::span<const uint8_t> input,
                                std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
            { std::scoped_lock lock(mutex);
              calls.push_back({token, {input.begin(), input.end()}, state.payload, concurrency::current_thread_id()}); }
            if (handler) return handler(token, state, input);
            StateDelta delta{token.state_version}; delta.state_payload = std::vector<uint8_t>{input.begin(), input.end()};
            return {foundation::Result::success(), std::move(delta)};
        };
        const auto sender_id = producer.id, target_id = stream.id;
        succeeds(aeg::feature::test::register_clock_test_definition(registry, std::move(producer)), "register real IO Channel producer");
        succeeds(aeg::feature::test::register_clock_test_definition(registry, std::move(stream)), "register real Stream Process");
        FeatureGraphDocument graph;
        graph.asset_uuid = "fixture://host-input-real-io"; graph.schema_version = 1; graph.graph_version = 1;
        graph.contract_digest = registry.contract_digest();
        graph.nodes = {{kSenderNode, sender_id, {1, 0, 0}, {}, {}}, {kTargetNode, target_id, {1, 0, 0}, {}, {}}};
        graph.edges = {{kSenderNode, kOut, kTargetNode, kChannel, EdgeKind::kQueue, kSenderClock, kIoReadClock, 1, 8, 0}};
        auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
        succeeds(compiled, "compile actual IO graph with versioned Queue");
        succeeds(runtime.publish_graph(graph, compiled.value), "publish actual IO graph");
        for (const auto& row : compiled.value.provenance) {
            if (row.node_id == kSenderNode) sender = row.instance_id;
            if (row.node_id == kTargetNode) target = row.instance_id;
        }
        require(sender.is_valid() && target.is_valid() && sender != target, "IO graph lost actual owner identities");
    }
    std::vector<Call> observations() { std::scoped_lock lock(mutex); return calls; }
    void drain() {
        for (unsigned spin = 0; spin != 2000 && runtime.pending_execution_count() != 0; ++spin) {
            succeeds(runtime.pump_completions(), "pump actual IO Feature completions");
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        require(runtime.pending_execution_count() == 0, "actual IO Feature work did not settle");
    }
    void send(uint8_t value) {
        { std::scoped_lock lock(mutex); outgoing = {value}; }
        succeeds(runtime.dispatch_call(sender), "dispatch actual IO Channel producer");
        drain();
    }
    auto bind_file(FileReadRequest& request) {
        return Access::bind(runtime, target, kHost, kHostSchema, 1,
            [&request](const Context& context) { return request.project(context); });
    }
};

void actual_file_completion_controls_stream_readiness_and_late_cancel() {
    for (const bool cancel : {false, true}) {
        TemporaryIoFile file(cancel ? "cancel" : "ready");
        file.write(std::vector<uint8_t>{0x00, 0x7f, 0x80, 0xff, 0x42});
        IoFixture fixture;
        FileReadRequest request(file.file);
        auto binding = fixture.bind_file(request); succeeds(binding, "bind actual file completion to Stream input");
        fixture.send(0xd1);
        const auto first_delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
        succeeds(fixture.runtime.enable_async_execution(), "enable actual IO/Logic Feature workers");
        succeeds(fixture.runtime.tick(kIoReadClock), "defer Stream while the real IO request is incomplete");
        fixture.send(0xd2); // An independent Clock/owner still progresses while Stream readiness is false.
        const auto waiting = fixture.runtime.inspect_invocations(fixture.target);
        succeeds(waiting, "inspect real IO-not-ready owner");
        const auto* pending = fixture.runtime.find_instance(fixture.target);
        require(!waiting.value.last_issued && waiting.value.open_attempts.empty() && !waiting.value.last_committed &&
            fixture.observations().empty() && pending->state.commit_count == 0 && pending->pending_messages.size() == 2 &&
            pending->pending_messages.front().delivery_id == first_delivery &&
            fixture.runtime.find_instance(fixture.sender)->state.commit_count == 2,
            "incomplete real IO issued a Stream attempt/consumed input or blocked another owner");
        if (cancel) {
            const auto process = fixture.runtime.process(fixture.target); succeeds(process, "find IO Process to cancel");
            succeeds(fixture.runtime.cancel(process.value), "cancel actual not-ready IO Process");
        }
        request.finish();
        succeeds(fixture.runtime.pump_completions(), "admit completed IO or discard logically cancelled late completion");
        fixture.drain();
        const auto observed = fixture.observations();
        const auto after = fixture.runtime.inspect_invocations(fixture.target);
        succeeds(after, "inspect actual Stream after IO completion");
        pending = fixture.runtime.find_instance(fixture.target);
        if (cancel) {
            succeeds(fixture.runtime.tick(kIoReadClock), "tick cancelled IO Process after late file completion");
            fixture.drain();
            const auto process = fixture.runtime.process(fixture.target); succeeds(process, "inspect cancelled IO lifecycle");
            const auto cancelled = fixture.runtime.inspect_invocations(fixture.target);
            succeeds(cancelled, "inspect cancelled IO after its later clock release");
            require(observed.empty() && fixture.observations().empty() && !cancelled.value.last_issued &&
                cancelled.value.open_attempts.empty() && !cancelled.value.last_committed && pending->state.commit_count == 0 &&
                pending->pending_messages.size() == 2 && pending->pending_messages.front().delivery_id == first_delivery &&
                pending->pending_messages.front().payload == std::vector<uint8_t>{0xd1} &&
                pending->pending_messages.back().payload == std::vector<uint8_t>{0xd2} &&
                fixture.runtime.find_process(process.value)->status == ProcessStatus::kCancelled,
                "late actual IO completion admitted or committed logically cancelled work");
        } else {
            require(observed.size() == 1 && observed.front().input == std::vector<uint8_t>({0x00, 0x7f, 0x80, 0xff, 0x42, 0xd1, 0xd2}) &&
                observed.front().thread != concurrency::current_thread_id() && pending->state.commit_count == 1 &&
                pending->state.payload == observed.front().input && pending->pending_messages.empty() &&
                after.value.last_committed && after.value.last_committed->attempt.token == observed.front().token,
                "completed actual file bytes did not reach and commit the Stream callback exactly once");
        }
    }
}

void actual_io_failure_retries_original_input_and_cancel_retires_backoff() {
    for (const bool cancel : {false, true}) {
        TemporaryIoFile file(cancel ? "retry-cancel" : "retry"); // Missing file makes the first real open fail.
        std::atomic_uint32_t failed_reads{0}, successful_reads{0};
        IoFixture fixture(true);
        fixture.handler = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>)
            -> foundation::ResultValue<StateDelta> {
            std::ifstream input(file.file, std::ios::binary);
            if (!input) { failed_reads.fetch_add(1); return {foundation::not_found("actual IO retry file is absent"), {}}; }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (input.bad()) return {foundation::invalid_state("actual IO retry read failed"), {}};
            successful_reads.fetch_add(1);
            StateDelta delta{token.state_version}; delta.state_payload = std::move(bytes);
            return {foundation::Result::success(), std::move(delta)};
        };
        fixture.send(0xe1);
        const auto old_delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
        succeeds(fixture.runtime.enable_async_execution(), "enable actual IO retry worker");
        succeeds(fixture.runtime.dispatch_process(fixture.target), "dispatch first actual failing IO read");
        fixture.drain();
        const auto failed = fixture.runtime.inspect_invocations(fixture.target);
        succeeds(failed, "inspect actual IO callback failure and clock-owned retry");
        const auto origin_calls = fixture.observations();
        require(origin_calls.size() == 1 && failed_reads.load() == 1 && successful_reads.load() == 0 &&
            failed.value.last_failed && failed.value.retry && !failed.value.last_committed &&
            failed.value.retry->origin == origin_calls.front().token &&
            fixture.runtime.find_instance(fixture.target)->state.commit_count == 0,
            "actual failed file open did not become an uncommitted Runtime retry");
        fixture.send(0xe2);
        file.write(std::vector<uint8_t>{0x46, 0x00, 0xfe, 0x49});
        if (cancel) {
            const auto process = fixture.runtime.process(fixture.target); succeeds(process, "find retrying IO Process");
            succeeds(fixture.runtime.cancel(process.value), "cancel actual IO retry during backoff");
        }
        succeeds(fixture.runtime.tick(kIoReadClock), "advance before actual IO retry backoff is due");
        fixture.drain();
        require(fixture.observations().size() == 1, "actual IO retry ran before its clock-owned backoff");
        succeeds(fixture.runtime.tick(kIoReadClock), "advance actual IO retry to its due clock tick");
        fixture.drain();
        const auto calls = fixture.observations();
        const auto observed = fixture.runtime.inspect_invocations(fixture.target);
        succeeds(observed, "inspect actual IO retry terminal observation");
        const auto* owner = fixture.runtime.find_instance(fixture.target);
        if (cancel) {
            const auto process = fixture.runtime.process(fixture.target); succeeds(process, "inspect cancelled retry Process");
            require(calls.size() == 1 && failed_reads.load() == 1 && successful_reads.load() == 0 &&
                !owner->retry_pending && owner->state.commit_count == 0 && owner->pending_messages.size() == 2 &&
                owner->pending_messages.front().delivery_id == old_delivery && !observed.value.last_committed &&
                observed.value.open_attempts.empty() && observed.value.last_issued &&
                observed.value.last_issued->token == origin_calls.front().token &&
                fixture.runtime.find_process(process.value)->status == ProcessStatus::kCancelled,
                "cancelled IO backoff reissued a real read or consumed/committed its old delivery");
        } else {
            require(calls.size() == 2 && failed_reads.load() == 1 && successful_reads.load() == 1 &&
                calls[0].input == std::vector<uint8_t>{0xe1} && calls[1].input == calls[0].input &&
                calls[0].state == calls[1].state && calls[0].token.state_version == calls[1].token.state_version &&
                calls[0].token.attempt_id != calls[1].token.attempt_id && !owner->retry_pending &&
                calls[0].thread != concurrency::current_thread_id() && calls[1].thread != concurrency::current_thread_id() &&
                owner->state.commit_count == 1 && owner->state.payload == std::vector<uint8_t>({0x46, 0x00, 0xfe, 0x49}) &&
                owner->pending_messages.size() == 1 && owner->pending_messages.front().payload == std::vector<uint8_t>{0xe2} &&
                owner->pending_messages.front().delivery_id != old_delivery && observed.value.last_committed &&
                observed.value.last_committed->attempt.token == calls[1].token,
                "actual IO recovery refreshed captured retry input/state or lost the later delivery");
        }
    }
}

void private_drain_does_not_admit_a_completed_real_io_request() {
    TemporaryIoFile file("strict-drain");
    file.write(std::vector<uint8_t>{0x31, 0x00, 0xff});
    IoFixture fixture;
    FileReadRequest request(file.file);
    auto binding = fixture.bind_file(request); succeeds(binding, "bind strict-drain actual IO input");
    fixture.send(0xf1);
    const auto delivery = fixture.runtime.find_instance(fixture.target)->pending_messages.front().delivery_id;
    succeeds(fixture.runtime.enable_async_execution(), "enable strict-drain IO worker");
    succeeds(fixture.runtime.tick(kIoReadClock), "defer actual Stream while its file read is unfinished");
    const auto deferred = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(deferred, "inspect actual IO-deferred origin");
    require(!deferred.value.last_issued && deferred.value.open_attempts.empty() && fixture.observations().empty(),
            "strict-drain precondition already issued a Stream invocation");
    request.finish();
    succeeds(FeatureExecutionControl::drain_issued_completions(fixture.runtime), "strict drain after actual IO became ready");
    const auto paused = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(paused, "inspect strict-drained real IO owner");
    const auto* owner = fixture.runtime.find_instance(fixture.target);
    const bool held = !paused.value.last_issued && paused.value.open_attempts.empty() && !paused.value.last_committed &&
        owner->state.commit_count == 0 && owner->pending_messages.size() == 1 &&
        owner->pending_messages.front().delivery_id == delivery && fixture.observations().empty();
    std::cout << "CLOCK_READY_DEFERRED strict-issued=" << bool(paused.value.last_issued)
              << " open=" << paused.value.open_attempts.size() << " committed=" << owner->state.commit_count << '\n';
    // Settle normally even after a strict-drain violation so the RED proves both admission and recovery.
    succeeds(fixture.runtime.pump_completions(), "resume ordinary admission after strict drain");
    fixture.drain();
    const auto calls = fixture.observations();
    const auto accepted = fixture.runtime.inspect_invocations(fixture.target);
    succeeds(accepted, "inspect recovered actual IO invocation");
    require(calls.size() == 1 && calls.front().input == std::vector<uint8_t>({0x31, 0x00, 0xff, 0xf1}) &&
        fixture.runtime.find_instance(fixture.target)->pending_messages.empty() &&
        fixture.runtime.find_instance(fixture.target)->state.commit_count == 1 && accepted.value.last_committed &&
        accepted.value.last_committed->attempt.token == calls.front().token,
        "normal pump did not recover one exact real IO input/delivery after strict drain");
    require(held, "strict drain admitted ready-deferred Stream work before ordinary admission resumed");
}
}  // namespace

int main() try {
    binding_validates_actual_port_and_owner_thread();
    projection_preserves_unbound_bytes_and_consumes_actual_channel();
    clock_waits_without_attempts_and_preserves_backlog();
    async_retry_keeps_projected_bytes_and_old_delivery();
    leases_do_not_cross_generation_or_runtime_lifetime();
    direct_busy_and_suspended_rejections_do_not_project();
    private_drain_finishes_issued_work_without_admitting_clock_backlog();
    private_drain_keeps_zero_backoff_retry_unissued_and_immutable();
    unsigned failures = 0;
    const std::pair<const char*, void(*)()> probes[] = {
        {"file-completion-readiness-cancel", actual_file_completion_controls_stream_readiness_and_late_cancel},
        {"file-failure-runtime-retry-cancel", actual_io_failure_retries_original_input_and_cancel_retires_backoff},
        {"strict-drain-ready-deferred", private_drain_does_not_admit_a_completed_real_io_request},
    };
    for (const auto& [name, probe] : probes) {
        try {
            probe();
            std::cout << "CLOCK_READINESS probe=" << name << " passed=1\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "CLOCK_READINESS probe=" << name << " passed=0 reason=" << error.what() << '\n';
        }
    }
    if (failures != 0) return 1;
    std::cout << "Feature host input contracts, real Channel consumption, clock deferral, retry and owning leases passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Feature host input: " << error.what() << '\n';
    return 1;
}
