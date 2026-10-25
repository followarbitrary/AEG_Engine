#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

#include "concurrency.hpp"
#include "feature_runtime.hpp"
#include "feature_graph.hpp"
#include "feature_render_test_fixture.hpp"
#include "render_intent_planner.hpp"

namespace {
using namespace aeg::feature;
using namespace aeg::concurrency;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ReleaseOnExit final {
    CompletionSource& source;
    ~ReleaseOnExit() { static_cast<void>(source.complete()); }
};

FeatureDefinition definition(unsigned index, ExecutionKind execution = ExecutionKind::kContinuous) {
    FeatureDefinition result{};
    result.id = FeatureDefinitionId(100 + index);
    result.version = {1, 0, 0};
    result.contract.definition_id = result.id;
    result.contract.version = result.version;
    result.contract.domains = to_mask(static_cast<FeatureDomain>(index));
    result.contract.execution = execution;
    result.contract.state = StatePolicy::kInstanceState;
    result.contract.scope.id = ScopeId("World");
    result.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId(200 + index), 60, 4, LateDataPolicy::kDrop,
        aeg::feature::DeadlinePolicy::kObserve, result.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    result.contract.permissions.version = 1;
    result.contract.has_continuation = execution == ExecutionKind::kProcess;
    return result;
}

void drain(FeatureRuntime& runtime) {
    for (unsigned spin = 0; spin < 2000 && runtime.pending_execution_count() != 0; ++spin) {
        require(static_cast<bool>(runtime.pump_completions()), "owner completion pump failed");
        static_cast<void>(sleep_for(1'000'000));
    }
    require(runtime.pending_execution_count() == 0, "completion inbox did not converge");
}

void five_domains_execute_independently_and_commit_on_owner() {
    std::array<CompletionSource, 5> started;
    CompletionSource release;
    std::array<ThreadId, 5> threads{};
    std::array<InvocationToken, 5> entered{};
    std::array<FeatureInstanceId, 5> instances{};
    bool foreign_mutation_rejected = false;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async executor initialization failed");
    for (unsigned index = 0; index < 5; ++index) {
        auto executable = definition(index);
        executable.execute = [&, index](const InvocationToken& token, const StateSnapshot& state,
                                        std::span<const uint8_t>, std::span<const uint8_t>) {
            threads[index] = current_thread_id();
            entered[index] = token;
            if (index == 0) {
                foreign_mutation_rejected = !runtime.tick(ClockId(200)) &&
                                            runtime.find_instance(instances[0]) == nullptr &&
                                            !runtime.inspect_invocations(instances[0]);
            }
            static_cast<void>(started[index].complete());
            static_cast<void>(release.completion().wait());
            StateDelta delta{};
            delta.expected_version = token.state_version;
            delta.state_payload = std::vector<uint8_t>{
                state.payload.empty() ? uint8_t{1} : static_cast<uint8_t>(state.payload[0] + 1)};
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
        };
        require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, executable)), "registration failed");
        const auto expanded = registry.expand(executable.id, {});
        require(static_cast<bool>(expanded), "expansion failed");
        const auto activated = runtime.activate(expanded.value, ScopeId("World"));
        require(static_cast<bool>(activated), "activation failed");
        instances[index] = activated.value;
        require(static_cast<bool>(runtime.tick(executable.contract.clock.id)), "async tick did not submit");
    }
    for (unsigned index = 0; index < 5; ++index) {
        require(static_cast<bool>(started[index].completion().wait(2'000'000'000)),
                "another domain was blocked by a long-running Feature");
        require(threads[index] != current_thread_id(), "callback ran on owner thread");
        for (unsigned other = 0; other < index; ++other)
            require(threads[index] != threads[other], "independent domains shared one executor owner");
        require(runtime.find_instance(instances[index])->state.commit_count == 0,
                "worker committed Runtime state before owner pump");
        const auto observed = runtime.inspect_invocations(instances[index]);
        require(observed && observed.value.open_attempts.size() == 1 &&
                observed.value.open_attempts[0].attempt.token == entered[index] &&
                observed.value.open_attempts[0].committable && observed.value.last_issued->token == entered[index] &&
                !observed.value.last_committed && !observed.value.last_failed,
                "gated domain was reported committed or lost its real open token");
        require(static_cast<bool>(runtime.tick(ClockId(200 + index))), "busy domain tick failed");
        require(!runtime.invoke(instances[index]), "manual invoke bypassed in-flight policy");
    }
    require(foreign_mutation_rejected, "worker accessed owner Runtime containers");
    require(runtime.pending_execution_count() == 5, "in-flight tasks were duplicated");
    static_cast<void>(release.complete());
    drain(runtime);
    for (unsigned index = 0; index < 5; ++index) {
        require(runtime.find_instance(instances[index])->state.payload == std::vector<uint8_t>{1},
                "owner did not commit domain result");
        const auto committed = runtime.inspect_invocations(instances[index]).value;
        require(committed.last_committed && committed.last_committed->attempt.token == entered[index] &&
                committed.last_committed->state_version == StateVersion(2) && committed.open_attempts.empty(),
                "owner pump lost actual non-Render/Render committed token");
        require(static_cast<bool>(runtime.tick(ClockId(200 + index))), "next domain tick failed");
    }
    drain(runtime);
    for (const auto id : instances)
        require(runtime.find_instance(id)->state.payload == std::vector<uint8_t>{2},
                "next callback did not read its committed state");
    require(static_cast<bool>(runtime.shutdown_execution()), "execution shutdown failed");
    require(!runtime.tick(ClockId(200)), "shutdown accepted new work");
}

FeatureInstanceId activate(FeatureRegistry& registry, FeatureRuntime& runtime, FeatureDefinition executable) {
    const auto registered = aeg::feature::test::register_clock_test_definition(registry, executable);
    if (!registered) throw std::runtime_error(registered.message);
    const auto expanded = registry.expand(executable.id, {});
    require(static_cast<bool>(expanded), "expansion failed");
    const auto instance = runtime.activate(expanded.value, ScopeId("World"));
    require(static_cast<bool>(instance), "activation failed");
    return instance.value;
}

void queued_cancel_and_late_revision_are_reclaimed() {
    CompletionSource started, release;
    unsigned queued_calls = 0;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto first = definition(0, ExecutionKind::kProcess);
    first.contract.state = StatePolicy::kProcessState;
    first.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        static_cast<void>(started.complete());
        static_cast<void>(release.completion().wait());
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{8};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    const auto first_id = activate(registry, runtime, first);
    auto second = first;
    second.id = FeatureDefinitionId("Queued.Process");
    second.contract.definition_id = second.id;
    second.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        ++queued_calls;
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), StateDelta{token.state_version}};
    };
    const auto second_id = activate(registry, runtime, second);
    require(static_cast<bool>(runtime.dispatch_process(first_id)), "first process submission failed");
    require(static_cast<bool>(started.completion().wait(2'000'000'000)), "first process did not start");
    require(static_cast<bool>(runtime.dispatch_process(second_id)), "queued process submission failed");
    const auto process = runtime.process(first_id).value;
    const auto revision = runtime.find_process(process)->revision;
    require(!runtime.retry(process), "manual retry bypassed in-flight policy");
    require(runtime.find_process(process)->revision == revision, "failed retry changed process revision");
    require(static_cast<bool>(runtime.cancel(runtime.process(second_id).value)), "queued cancel failed");
    require(static_cast<bool>(runtime.suspend(process)) && static_cast<bool>(runtime.resume(process)), "pause failed");
    const auto retired_open = runtime.inspect_invocations(first_id).value;
    require(retired_open.open_attempts.size() == 1 && !retired_open.open_attempts[0].committable &&
            !runtime.inspect_invocations(second_id).value.open_attempts[0].committable,
            "suspend/resume/cancel did not retire open token eligibility");
    static_cast<void>(release.complete());
    drain(runtime);
    for (const auto id : {first_id, second_id}) {
        const auto retired = runtime.inspect_invocations(id).value;
        require(retired.open_attempts.empty() && !retired.last_committed && !retired.last_failed && retired.last_issued,
                "retired async cleanup fabricated a current failure or commit");
    }
    require(queued_calls == 0, "cancelled queued callback still executed");
    require(runtime.find_process(process)->state.commit_count == 0, "late ProcessRevision committed state");
    require(runtime.find_instance(second_id)->state.commit_count == 0, "cancelled work committed state");
    require(static_cast<bool>(runtime.dispatch_process(first_id)), "stale result left owner permanently in-flight");
    drain(runtime);
    require(runtime.find_process(process)->state.payload == std::vector<uint8_t>{8}, "fresh Process result failed");
}

void same_clock_dependencies_wait_for_owner_commit() {
    CompletionSource started, release;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto source = definition(2);
    source.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Byte"), 1}};
    source.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        static_cast<void>(started.complete());
        static_cast<void>(release.completion().wait());
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.output_payload = {42};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    auto target = source;
    target.id = FeatureDefinitionId("Data.Target");
    target.contract.definition_id = target.id;
    target.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Byte"), 1}};
    target.contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    target.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t>) {
        if (input.empty() || input.back() != 42)
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_argument("target snapshot preceded upstream commit"), {}};
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{input.back()};
        delta.render_intent = RenderIntent{};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)) && static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, target)), "data registration failed");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://async-data";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version},
                   {FeatureNodeId("Target"), target.id, target.version}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, source.contract.clock.id, source.contract.clock.id, 1, 1, 0}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    if (!compiled) throw std::runtime_error(compiled.result.message);
    require(static_cast<bool>(runtime.publish_graph(graph, compiled.value)), "data graph publication failed");
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "data tick failed");
    require(static_cast<bool>(started.completion().wait(2'000'000'000)), "data source did not start");
    require(runtime.pending_execution_count() == 1, "dependent submitted with a stale input snapshot");
    static_cast<void>(release.complete());
    drain(runtime);
    const auto target_id = compiled.value.provenance[1].instance_id;
    require(runtime.find_instance(target_id)->state.payload == std::vector<uint8_t>{42}, "dependency did not resume after owner commit");
    const auto captured = runtime.capture_render_outputs(compiled.value);
    require(captured && captured.value.size() == 1 && captured.value.front().input_messages.size() == 1,
            "worker completion lost consumed delivery evidence");
    const auto& message = captured.value.front().input_messages.front();
    require(message.source.instance == compiled.value.provenance[0].instance_id &&
            message.source.invocation.generation == runtime.generation() &&
            message.source.definition_digest == feature_contract_digest(source) &&
            message.source.resource_epoch == 1 && message.source.clock == source.contract.clock.id &&
            message.source_port == PortId("Out") && message.target_port == PortId("In") &&
            message.schema == ValueSchemaId("Byte") && message.payload == std::vector<uint8_t>{42},
            "worker dependency did not carry its committed producer and payload");
}

void complete_render_wave_publishes_before_queued_restart() {
    std::array<std::array<CompletionSource, 3>, 4> started, releases;
    struct ReleaseWaves final {
        decltype(releases)& gates;
        ~ReleaseWaves() { for (auto& wave : gates) for (auto& gate : wave) static_cast<void>(gate.complete()); }
    };
    bool failed_b_once = false;
    InvocationToken failed_b{};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseWaves release_on_exit{releases};
    require(bool(runtime.enable_async_execution()), "publication async initialization failed");
    auto producer = render_feature_test::definition();
    producer.contract.parameters.clear();
    producer.contract.clock.late_data_policy = LateDataPolicy::kQueue;
    producer.contract.ports = {{PortId("List"), PortDirection::kOutput, kRenderLightListSchema, 1}};
    producer.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        const auto wave = static_cast<size_t>(token.input_version);
        require(wave < started.size(), "publication submitted an unexpected producer wave");
        static_cast<void>(started[wave][0].complete());
        static_cast<void>(releases[wave][0].completion().wait());
        RenderLightCullingScene cull{{RenderResourceId("Scene"), 1}, {RenderResourceId("Depth"), 1, true},
            {RenderResourceId("Lights"), 1, PortId("List")}};
        StateDelta delta{token.state_version};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {cull};
        const auto encoded = encode_render_light_list_reference({cull.output.id, cull.output.version,
            token.invocation_id, token.generation});
        require(bool(encoded), "publication reference encoding failed");
        delta.output_payload = encoded.value;
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    require(bool(aeg::feature::test::register_clock_test_definition(registry, producer)), "publication producer registration failed");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://complete-render-wave";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("P"), producer.id, producer.version}};
    for (unsigned channel = 1; channel != 3; ++channel) {
        auto consumer = producer;
        consumer.id = FeatureDefinitionId(900 + channel);
        consumer.contract.definition_id = consumer.id;
        consumer.contract.ports = {{PortId("List"), PortDirection::kInput, kRenderLightListSchema, 1}};
        if (channel == 2) {
            consumer.contract.failure = FailurePolicy::kRetry;
            consumer.contract.retry = {2, 1};
        }
        consumer.execute = [&, channel](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input,
                                       std::span<const uint8_t>) {
            const auto wave = static_cast<size_t>(token.input_version);
            require(wave < started.size(), "publication submitted an unexpected consumer wave");
            static_cast<void>(started[wave][channel].complete());
            static_cast<void>(releases[wave][channel].completion().wait());
            const auto decoded = decode_render_light_list_reference(input);
            if (!decoded) return aeg::foundation::ResultValue<StateDelta>{decoded.result, {}};
            if (wave == 1 && channel == 2 && !failed_b_once) {
                failed_b_once = true;
                failed_b = token;
                return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_state("retryable B fixture"), {}};
            }
            StateDelta delta{token.state_version};
            if (wave != 2 || channel != 2) {
                RenderDrawClusteredScene draw{};
                draw.scene = {RenderResourceId("Scene"), 1};
                draw.light_list = decoded.value;
                draw.hdr.target = {RenderResourceId(100 + channel), 1, true};
                draw.depth = {{RenderResourceId("Depth"), 1, true}, RenderAttachmentLoad::kLoad, 1.f};
                delta.render_intent = RenderIntent{};
                delta.render_intent->operations = {draw};
            }
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
        };
        require(bool(aeg::feature::test::register_clock_test_definition(registry, consumer)), "publication consumer registration failed");
        graph.nodes.push_back({FeatureNodeId(channel), consumer.id, consumer.version});
        graph.edges.push_back({FeatureNodeId("P"), PortId("List"), FeatureNodeId(channel), PortId("List"),
            EdgeKind::kData, producer.contract.clock.id, producer.contract.clock.id, 1, 1, 0});
    }
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    require(compiled && runtime.publish_graph(graph, compiled.value), "publication graph initialization failed");
    const auto pump_until = [&](const auto& condition) {
        for (unsigned spin = 0; spin != 2000 && !condition(); ++spin) {
            require(bool(runtime.pump_completions()), "publication completion pump failed");
            static_cast<void>(sleep_for(1'000'000));
        }
        require(condition(), "publication owner boundary did not converge");
    };
    const auto capture = [&] {
        const auto result = runtime.capture_render_outputs(compiled.value);
        require(bool(result), "publication capture failed");
        return result.value;
    };
    const auto assert_wave = [&](const std::vector<RenderFeatureOutput>& outputs, unsigned wave) {
        require(outputs.size() == 3, "publication omitted part of a settled Render cohort");
        const auto& source = outputs.front();
        require(source.invocation.input_version == wave, "publication switched to the wrong producer wave");
        for (size_t index = 1; index != outputs.size(); ++index) {
            const auto& target = outputs[index];
            const auto& reference = std::get<RenderDrawClusteredScene>(target.intent.operations.front()).light_list;
            require(target.invocation.input_version == wave && reference.producer_invocation == source.invocation.invocation_id &&
                    reference.producer_generation == source.invocation.generation && target.input_messages.size() == 1 &&
                    target.input_messages.front().source.invocation == source.invocation &&
                    target.input_messages.front().source.instance == source.instance_id,
                    "publication mixed producer and consumer waves");
        }
    };
    require(bool(runtime.tick(producer.contract.clock.id)), "publication first wave did not start");
    require(bool(started[0][0].completion().wait(2'000'000'000)), "publication producer did not start");
    require(capture().empty(), "first pending Render wave published partial output");
    std::vector<RenderFeatureOutput> accepted;
    for (unsigned wave = 0; wave != 4; ++wave) {
        for (unsigned channel = 0; channel != 2; ++channel) {
            static_cast<void>(releases[wave][channel].complete());
            pump_until([&] { return started[wave][channel + 1].completion().is_complete() &&
                runtime.find_instance(compiled.value.provenance[channel].instance_id)->state.commit_count == wave + 1; });
            const auto partial = capture();
            if (accepted.empty()) require(partial.empty(), "first pending Render wave published partial output");
            else assert_wave(partial, wave == 1 ? 0 : 1);
        }
        if (wave == 1) require(bool(runtime.tick(producer.contract.clock.id)), "publication next wave did not queue");
        static_cast<void>(releases[wave][2].complete());
        if (wave == 1) {
            pump_until([&] { return runtime.find_instance(compiled.value.provenance[2].instance_id)->retry_pending &&
                runtime.pending_execution_count() == 0; });
            assert_wave(capture(), 0);
            require(runtime.render_publication_status(producer.contract.clock.id).code == aeg::foundation::ResultCode::kNotReady &&
                runtime.find_instance(compiled.value.provenance[2].instance_id)->state.commit_count == 1,
                "worker-free retry backoff published a partial B cohort");
            require(bool(runtime.tick(producer.contract.clock.id)), "publication retry deadline did not advance");
        }
        pump_until([&] { return runtime.find_instance(compiled.value.provenance[2].instance_id)->state.commit_count == wave + 1; });
        if (wave != 2) {
            accepted = capture();
            assert_wave(accepted, wave);
            if (wave == 1) require(accepted.back().invocation.attempt_id != failed_b.attempt_id &&
                accepted.back().invocation.state_version == failed_b.state_version,
                "publication retry changed the committed snapshot or reused a failed attempt");
        } else assert_wave(capture(), 1); // Idle C2 retained its old intent: do not publish P2/C1_2/C2_1.
        if (wave == 1) {
            require(bool(started[2][0].completion().wait(2'000'000'000)), "queued producer did not restart");
            assert_wave(capture(), 1); // Publication must happen before starting that queued wave.
        } else if (wave != 3) {
            require(bool(runtime.tick(producer.contract.clock.id)), "publication subsequent wave did not start");
            require(bool(started[wave + 1][0].completion().wait(2'000'000'000)), "publication subsequent producer did not start");
        }
    }
    assert_wave(accepted, 3);
    require(bool(runtime.render_publication_status(producer.contract.clock.id)),
            "settled publication did not report readiness");
    const auto source_owner = compiled.value.provenance.front().instance_id;
    const auto export_token = runtime.invoke(source_owner);
    require(bool(export_token), "typed export test invocation failed");
    const auto before_export = runtime.find_instance(source_owner)->state;
    StateDelta export_delta{export_token.value.state_version};
    const auto encoded_export = encode_render_light_list_reference({RenderResourceId("Lights"), 1,
        export_token.value.invocation_id, export_token.value.generation});
    require(bool(encoded_export), "typed export test encoding failed");
    export_delta.output_payload = encoded_export.value;
    const auto missing_declaration = runtime.complete(export_token.value, export_delta);
    require(!missing_declaration && missing_declaration.code == aeg::foundation::ResultCode::kInvalidArgument &&
            runtime.find_instance(source_owner)->state.version == before_export.version &&
            runtime.find_instance(source_owner)->state.commit_count == before_export.commit_count,
            "typed export committed state without the same invocation declaration");
    assert_wave(capture(), 3);
    export_delta.render_intent = RenderIntent{};
    export_delta.render_intent->operations = {RenderLightCullingScene{{RenderResourceId("Scene"), 1},
        {RenderResourceId("Depth"), 1, true}, {RenderResourceId("Lights"), 1, PortId("List")}}};
    require(bool(runtime.complete(export_token.value, export_delta)), "valid typed export did not preserve the rejected token");
    require(runtime.render_publication_status(producer.contract.clock.id).code == aeg::foundation::ResultCode::kNotReady,
            "new explicit producer bypassed old consumer publication readiness");
    assert_wave(capture(), 3);
    const auto previous_generation = runtime.generation();
    auto replacement = FeatureGraphCompiler{}.compile(graph, registry);
    require(replacement && runtime.publish_graph(graph, replacement.value), "publication generation replacement failed");
    require(runtime.generation() != previous_generation && runtime.capture_render_outputs(replacement.value).value.empty(),
            "new graph generation recaptured the previous cohort");
}

void render_process_publication_does_not_resurrect_retired_intent() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    auto process_definition = render_feature_test::definition();
    process_definition.contract.execution = ExecutionKind::kProcess;
    process_definition.contract.has_continuation = true;
    process_definition.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                                    std::span<const uint8_t>) {
        StateDelta delta{token.state_version};
        delta.render_intent = RenderIntent{};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    require(bool(aeg::feature::test::register_clock_test_definition(registry, process_definition)), "render Process registration failed");
    auto graph = render_feature_test::document(process_definition);
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    require(compiled && runtime.publish_graph(graph, compiled.value), "render Process graph publication failed");
    const auto clock = process_definition.contract.clock.id;
    require(bool(runtime.tick(clock)), "render Process first inline wave failed");
    auto initial = runtime.capture_render_outputs(compiled.value);
    require(initial && initial.value.size() == 2, "render Process inline cohort did not publish");
    const auto retired_owner = compiled.value.provenance.front().instance_id;
    const auto process = runtime.process(retired_owner);
    require(process && runtime.suspend(process.value), "render Process suspension failed");
    require(runtime.capture_render_outputs(compiled.value).value.empty(), "suspension retained part of the previous cohort");
    const bool reported_retirement = runtime.render_publication_status(clock).code == aeg::foundation::ResultCode::kNotReady;
    require(bool(runtime.tick(clock)), "render Process suspended wave failed");
    auto suspended = runtime.capture_render_outputs(compiled.value);
    require(suspended && suspended.value.size() == 1 && suspended.value.front().instance_id != retired_owner,
            "suspended Process intent was resurrected by another completed owner");
    require(reported_retirement, "publication readiness hid a retired Process cohort");
    require(bool(runtime.resume(process.value)), "render Process resume failed");
    require(runtime.capture_render_outputs(compiled.value).value.empty(), "resume recaptured a prior Process revision");
    require(bool(runtime.tick(clock)), "render Process resumed wave failed");
    auto resumed = runtime.capture_render_outputs(compiled.value);
    require(resumed && resumed.value.size() == 2 && resumed.value.front().invocation != initial.value.front().invocation,
            "fresh resumed Process intent did not replace the retired cohort");
    require(bool(runtime.cancel(process.value)), "render Process cancellation failed");
    require(runtime.capture_render_outputs(compiled.value).value.empty(), "cancel retained a partial old cohort");
    require(runtime.render_publication_status(clock).code == aeg::foundation::ResultCode::kNotReady,
            "publication readiness hid cancellation");
    require(bool(runtime.tick(clock)), "render Process post-cancel wave failed");
    auto cancelled = runtime.capture_render_outputs(compiled.value);
    require(cancelled && cancelled.value.size() == 1 && cancelled.value.front().instance_id != retired_owner,
            "cancelled Process intent reappeared");
}

struct LightListProcessFixture final {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry};
    FeatureDefinition producer{render_feature_test::definition()};
    FeatureGraphDocument graph{};
    CompiledFeatureGraph compiled{};
    bool clear_consumers{false};
    bool targeted_exports{false};
    unsigned consumer_defect{0};
    CompletionSource* producer_started{};
    CompletionSource* producer_release{};
    CompletionSource* consumer_started{};
    CompletionSource* consumer_release{};
    explicit LightListProcessFixture(bool targeted = false) : targeted_exports(targeted) {
        producer.contract.parameters.clear();
        producer.contract.execution = ExecutionKind::kProcess;
        producer.contract.state = StatePolicy::kProcessState;
        producer.contract.has_continuation = true;
        producer.contract.ports = {{PortId("List"), PortDirection::kOutput, kRenderLightListSchema, 1}};
        producer.execute = [this](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
            if (producer_started) {
                static_cast<void>(producer_started->complete());
                static_cast<void>(producer_release->completion().wait());
            }
            RenderLightCullingScene cull{{RenderResourceId("Scene"), 1}, {RenderResourceId("Depth"), 1, true},
                {RenderResourceId("Lights"), 1, PortId("List")}};
            StateDelta delta{token.state_version}; delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {cull};
            const auto encoded = encode_render_light_list_reference({cull.output.id, cull.output.version, token.invocation_id, token.generation});
            require(bool(encoded), "Process LightList encoding failed");
            if (targeted_exports) {
                for (unsigned channel = 1; channel != 3; ++channel) {
                    PortMessage message{}; message.source_port = PortId("List"); message.target_port = PortId(1100 + channel);
                    message.schema = kRenderLightListSchema; message.schema_version = 1; message.payload = encoded.value;
                    delta.output_messages.push_back(std::move(message));
                }
            } else delta.output_payload = encoded.value;
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
        };
        require(bool(aeg::feature::test::register_clock_test_definition(registry, producer)), "Process LightList producer registration failed");
        graph.asset_uuid = "asset://light-list-process-retirement"; graph.schema_version = 1; graph.graph_version = 1;
        graph.nodes = {{FeatureNodeId("P"), producer.id, producer.version}};
        for (unsigned channel = 1; channel != 3; ++channel) {
            auto consumer = producer; consumer.id = FeatureDefinitionId(900 + channel); consumer.contract.definition_id = consumer.id;
            consumer.contract.execution = ExecutionKind::kContinuous; consumer.contract.state = StatePolicy::kInstanceState;
            consumer.contract.has_continuation = false;
            consumer.contract.ports = {{targeted_exports ? PortId(1100 + channel) : PortId("List"),
                PortDirection::kInput, kRenderLightListSchema, 1}};
            consumer.execute = [this, channel](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t>) {
                if (channel == 1 && consumer_started) {
                    static_cast<void>(consumer_started->complete());
                    static_cast<void>(consumer_release->completion().wait());
                }
                StateDelta delta{token.state_version};
                if (clear_consumers) delta.render_intent = RenderIntent{};
                else if (!input.empty()) {
                    const auto decoded = decode_render_light_list_reference(input);
                    if (!decoded) return aeg::foundation::ResultValue<StateDelta>{decoded.result, {}};
                    RenderDrawClusteredScene draw{}; draw.scene = {RenderResourceId("Scene"), 1}; draw.light_list = decoded.value;
                    draw.hdr.target = {RenderResourceId(100 + channel), 1, true};
                    draw.depth = {{RenderResourceId("Depth"), 1, true}, RenderAttachmentLoad::kLoad, 1.f};
                    if (channel == 1) switch (consumer_defect) {
                        case 1: draw.scene.id = RenderResourceId("Other.Scene"); break;
                        case 2: draw.depth.target.id = RenderResourceId("Other.Depth"); break;
                        case 3: draw.directional_shadow = RenderTextureReference{RenderResourceId("Other.Shadow"), 1, true}; break;
                        case 4: draw.light_list.id = RenderResourceId("Other.List"); break;
                        case 5: draw.light_list.producer_invocation = InvocationId(999999); break;
                        case 6: draw.light_list.producer_generation = RuntimeGeneration(999999); break;
                    }
                    delta.render_intent = RenderIntent{}; delta.render_intent->operations = {draw};
                }
                return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
            };
            require(bool(aeg::feature::test::register_clock_test_definition(registry, consumer)), "Process LightList consumer registration failed");
            graph.nodes.push_back({FeatureNodeId(channel), consumer.id, consumer.version});
            graph.edges.push_back({FeatureNodeId("P"), PortId("List"), FeatureNodeId(channel),
                targeted_exports ? PortId(1100 + channel) : PortId("List"),
                EdgeKind::kData, producer.contract.clock.id, producer.contract.clock.id, 1, 1, 0});
        }
        auto result = FeatureGraphCompiler{}.compile(graph, registry);
        require(result && runtime.publish_graph(graph, result.value), "Process LightList graph publication failed"); compiled = std::move(result.value);
    }
    FeatureInstanceId instance(FeatureNodeId node) const {
        const auto found = std::find_if(compiled.provenance.begin(), compiled.provenance.end(), [&](const auto& value) { return value.node_id == node; });
        require(found != compiled.provenance.end(), "Process fixture lost Node provenance"); return found->instance_id;
    }
    ProcessId process() const {
        const auto value = runtime.process(instance(FeatureNodeId("P"))); require(bool(value), "Process LightList owner missing"); return value.value;
    }
    void require_waiting(const char* message) const {
        const auto captured = runtime.capture_render_outputs(compiled);
        require(captured && captured.value.empty() && runtime.render_publication_status(producer.contract.clock.id).code == aeg::foundation::ResultCode::kNotReady, message);
    }
    void accept_initial() {
        require(bool(runtime.tick(producer.contract.clock.id)), "Process LightList first wave failed");
        const auto captured = runtime.capture_render_outputs(compiled);
        require(captured && captured.value.size() == 3 && runtime.render_publication_status(producer.contract.clock.id), "Process LightList first wave was not complete");
        auto orphan = captured.value;
        std::erase_if(orphan, [&](const auto& output) { return output.instance_id == instance(FeatureNodeId("P")); });
        const auto rejected = RenderIntentPlanner{}.plan(orphan);
        require(!rejected && rejected.result.code == aeg::foundation::ResultCode::kInvalidArgument,
            "public planner trusted opaque receipt evidence without Runtime retirement authority");
        const auto& source = *runtime.find_instance(instance(FeatureNodeId("P")))->render_output;
        std::array<MessageDeliveryId, 2> deliveries{};
        for (unsigned channel = 1; channel != 3; ++channel) {
            const auto* owner = runtime.find_instance(instance(FeatureNodeId(channel)));
            require(owner->pending_messages.empty() && owner->render_output && owner->render_output->input_messages.size() == 1, "Process LightList did not consume real routed input");
            const auto& message = owner->render_output->input_messages.front();
            const auto& reference = std::get<RenderDrawClusteredScene>(owner->render_output->intent.operations.front()).light_list;
            require(message.delivery_id.is_valid() && message.source.instance == instance(FeatureNodeId("P")) && message.source.invocation == source.invocation &&
                message.source.definition_digest == feature_contract_digest(producer) && message.source.resource_epoch == 1 &&
                message.source.clock == producer.contract.clock.id && message.kind == EdgeKind::kData && message.schema == kRenderLightListSchema &&
                message.source_port == PortId("List") && message.target_port == (targeted_exports ? PortId(1100 + channel) : PortId("List")) &&
                reference.producer_invocation == source.invocation.invocation_id &&
                reference.producer_generation == runtime.generation(), "Process LightList receipt is not authentic");
            deliveries[channel - 1] = message.delivery_id;
        }
        require(deliveries[0] != deliveries[1] && runtime.find_process(process())->state.commit_count == 1, "Process LightList owners shared delivery or state");
    }
};

void typed_light_list_process_retirement_waits_without_orphan_publication() {
    for (const bool cancel : {false, true}) {
        LightListProcessFixture fixture; fixture.accept_initial();
        const auto process = fixture.process(); const auto before = *fixture.runtime.find_process(process);
        const auto retired = cancel ? fixture.runtime.cancel(process) : fixture.runtime.suspend(process);
        require(bool(retired), "Process LightList retirement failed");
        const auto* after = fixture.runtime.find_process(process);
        require(after->revision.value() == before.revision.value() + 1 && after->state.version == before.state.version &&
            after->state.commit_count == before.state.commit_count && after->status == (cancel ? ProcessStatus::kCancelled : ProcessStatus::kSuspended),
            "Process LightList retirement changed committed state");
        fixture.require_waiting("Process LightList retirement retained an old cohort");
        const auto tick = fixture.runtime.tick(fixture.producer.contract.clock.id);
        if (!tick) std::cerr << "retired producer clock error: " << tick.message << '\n';
        require(bool(tick), "retired producer turned valid waiting into a clock error");
        fixture.require_waiting("idle consumers published orphan LightList work");
        for (unsigned channel = 1; channel != 3; ++channel) {
            const auto* owner = fixture.runtime.find_instance(fixture.instance(FeatureNodeId(channel)));
            require(owner->state.commit_count == 2 && owner->pending_messages.empty() && owner->render_output->input_messages.size() == 1,
                "idle consumer did not preserve its real previously consumed receipt");
        }
        fixture.clear_consumers = true;
        require(bool(fixture.runtime.tick(fixture.producer.contract.clock.id)) && fixture.runtime.render_publication_status(fixture.producer.contract.clock.id),
            "explicit empty independent consumers remained blocked by a retired producer");
        const auto cleared = fixture.runtime.capture_render_outputs(fixture.compiled);
        require(cleared && std::all_of(cleared.value.begin(), cleared.value.end(), [](const auto& output) { return output.intent.operations.empty(); }),
            "cleared consumers recaptured retired light-list work");
    }
}

void typed_light_list_pending_delivery_survives_retirement_and_replacement() {
    for (const bool cancel : {false, true}) for (const bool replace : {false, true}) {
        LightListProcessFixture fixture; fixture.accept_initial();
        require(bool(fixture.runtime.dispatch_process(fixture.instance(FeatureNodeId("P")))), "Process LightList B dispatch failed");
        const auto source_b = fixture.runtime.find_instance(fixture.instance(FeatureNodeId("P")))->render_output->invocation;
        std::array<MessageDeliveryId, 2> deliveries{};
        for (unsigned channel = 1; channel != 3; ++channel) {
            const auto* owner = fixture.runtime.find_instance(fixture.instance(FeatureNodeId(channel)));
            require(owner->pending_messages.size() == 1 && owner->pending_messages.front().source.invocation == source_b &&
                owner->pending_messages.front().render_source, "Process LightList B has no genuine retained delivery evidence");
            deliveries[channel - 1] = owner->pending_messages.front().delivery_id;
        }
        const auto retained_a = fixture.runtime.capture_render_outputs(fixture.compiled);
        require(retained_a && retained_a.value.size() == 3 && retained_a.value.front().invocation != source_b &&
            fixture.runtime.render_publication_status(fixture.producer.contract.clock.id).code == aeg::foundation::ResultCode::kNotReady,
            "pending Process LightList B replaced complete A");
        const auto process = fixture.process();
        require(bool(cancel ? fixture.runtime.cancel(process) : fixture.runtime.suspend(process)), "pending B retirement failed");
        if (replace) {
            auto replacement = fixture.producer; replacement.version = {1, 1, 0}; replacement.contract.version = replacement.version;
            StateMigration migration{fixture.producer.version, replacement.version, fixture.producer.contract.state_payload.schema_version,
                replacement.contract.state_payload.schema_version, {}};
            require(bool(fixture.runtime.replace_definition(replacement, migration)), "retired producer definition replacement failed");
            fixture.producer = std::move(replacement);
        }
        fixture.require_waiting("pending B retirement resurrected A");
        const auto tick = fixture.runtime.tick(fixture.producer.contract.clock.id);
        if (!tick) std::cerr << "pending retired B clock error: " << tick.message << '\n';
        require(bool(tick), "genuine retired B delivery was treated as malformed");
        fixture.require_waiting("retired B consumers published without an admissible producer");
        for (unsigned channel = 1; channel != 3; ++channel) {
            const auto* owner = fixture.runtime.find_instance(fixture.instance(FeatureNodeId(channel)));
            require(owner->pending_messages.empty() && owner->state.commit_count == 2 && owner->render_output->input_messages.size() == 1 &&
                owner->render_output->input_messages.front().delivery_id == deliveries[channel - 1] &&
                owner->render_output->input_messages.front().source.invocation == source_b,
                "retirement lost or fabricated the actual B consumption");
        }
        if (!cancel) {
            require(bool(fixture.runtime.resume(process)), "retired producer resume failed");
            fixture.require_waiting("resume reused the retired source cohort");
            require(bool(fixture.runtime.tick(fixture.producer.contract.clock.id)), "resumed producer fresh wave failed");
            const auto next = fixture.runtime.capture_render_outputs(fixture.compiled);
            require(next && next.value.size() == 3 && fixture.runtime.render_publication_status(fixture.producer.contract.clock.id), "fresh resumed cohort did not publish");
            const auto fresh = fixture.runtime.find_instance(fixture.instance(FeatureNodeId("P")))->render_output->invocation;
            require(fresh != source_b && fresh.generation == fixture.runtime.generation() &&
                fresh.process_revision == fixture.runtime.find_process(process)->revision, "resume did not issue a current producer token");
            for (unsigned channel = 1; channel != 3; ++channel) {
                const auto& receipt = fixture.runtime.find_instance(fixture.instance(FeatureNodeId(channel)))->render_output->input_messages.front();
                require(receipt.source.invocation == fresh && receipt.delivery_id != deliveries[channel - 1] &&
                    receipt.source.definition_digest == feature_contract_digest(fixture.producer), "resumed consumer retained a retired producer receipt");
            }
        }
    }
}

void typed_light_list_retirement_keeps_malformed_inputs_strict() {
    for (unsigned defect = 1; defect != 7; ++defect) {
        LightListProcessFixture fixture; fixture.accept_initial();
        require(bool(fixture.runtime.dispatch_process(fixture.instance(FeatureNodeId("P")))) && fixture.runtime.cancel(fixture.process()), "malformed retirement setup failed");
        fixture.consumer_defect = defect;
        const auto tick = fixture.runtime.tick(fixture.producer.contract.clock.id);
        const auto captured = fixture.runtime.capture_render_outputs(fixture.compiled);
        require(!tick && tick.code == aeg::foundation::ResultCode::kInvalidArgument && !captured &&
            captured.result.code == aeg::foundation::ResultCode::kInvalidArgument &&
            fixture.runtime.render_publication_status(fixture.producer.contract.clock.id).code == aeg::foundation::ResultCode::kInvalidArgument,
            "retirement hid a forged or scene/depth/shadow-mismatched consumer");
    }
}

void typed_light_list_targeted_exports_share_one_retired_declaration() {
    for (const bool cancel : {false, true}) {
        LightListProcessFixture fixture{true}; fixture.accept_initial();
        require(bool(fixture.runtime.dispatch_process(fixture.instance(FeatureNodeId("P")))), "targeted LightList B dispatch failed");
        const auto source = fixture.runtime.find_instance(fixture.instance(FeatureNodeId("P")))->render_output->invocation;
        for (unsigned channel = 1; channel != 3; ++channel) {
            const auto* owner = fixture.runtime.find_instance(fixture.instance(FeatureNodeId(channel)));
            require(owner->pending_messages.size() == 1 && owner->pending_messages.front().target_port == PortId(1100 + channel) &&
                owner->pending_messages.front().source.invocation == source, "targeted LightList delivery was not genuinely routed");
        }
        require(bool(cancel ? fixture.runtime.cancel(fixture.process()) : fixture.runtime.suspend(fixture.process())), "targeted producer retirement failed");
        const auto tick = fixture.runtime.tick(fixture.producer.contract.clock.id);
        if (!tick) std::cerr << "targeted retired declaration error: " << tick.message << '\n';
        require(bool(tick), "one cull exported to two targets became ambiguous retired producers");
        fixture.require_waiting("targeted retired exports published orphan work");
        const auto& first = fixture.runtime.find_instance(fixture.instance(FeatureNodeId{1}))->render_output->input_messages.front();
        const auto& second = fixture.runtime.find_instance(fixture.instance(FeatureNodeId{2}))->render_output->input_messages.front();
        require(first.render_source && first.render_source == second.render_source && first.delivery_id != second.delivery_id,
            "one declaration was duplicated or distinct real deliveries were collapsed");
    }
}

void typed_light_list_retired_worker_is_cleanup_not_publication() {
    for (const bool cancel : {false, true}) {
        CompletionSource started, release;
        LightListProcessFixture fixture;
        ReleaseOnExit release_on_exit{release};
        fixture.accept_initial();
        fixture.producer_started = &started; fixture.producer_release = &release;
        require(bool(fixture.runtime.enable_async_execution()) && fixture.runtime.tick(fixture.producer.contract.clock.id), "Process LightList async wave failed");
        require(bool(started.completion().wait(2'000'000'000)), "Process LightList worker did not start");
        const auto before = fixture.runtime.find_process(fixture.process())->state;
        require(bool(cancel ? fixture.runtime.cancel(fixture.process()) : fixture.runtime.suspend(fixture.process())), "in-flight Process LightList retirement failed");
        fixture.require_waiting("in-flight retirement retained the old cohort");
        static_cast<void>(release.complete()); drain(fixture.runtime);
        require(fixture.runtime.find_process(fixture.process())->state.commit_count == before.commit_count &&
            fixture.runtime.find_process(fixture.process())->state.version == before.version, "retired worker committed a late producer delta");
        for (unsigned channel = 1; channel != 3; ++channel) {
            const auto* owner = fixture.runtime.find_instance(fixture.instance(FeatureNodeId(channel)));
            require(owner->state.commit_count == 1 && owner->pending_messages.empty(), "retired worker dispatched or delivered its deferred consumer");
        }
        fixture.require_waiting("worker cleanup resurrected a retired light-list cohort");
        require(bool(fixture.runtime.tick(fixture.producer.contract.clock.id)), "fresh async waiting tick failed"); drain(fixture.runtime);
        fixture.require_waiting("fresh async idle consumers published orphan work");
    }
}

void typed_light_list_consumer_in_flight_keeps_its_own_commit_boundary() {
    for (const bool cancel : {false, true}) {
        CompletionSource started, release;
        LightListProcessFixture fixture; ReleaseOnExit release_on_exit{release};
        fixture.accept_initial(); fixture.consumer_started = &started; fixture.consumer_release = &release;
        require(bool(fixture.runtime.enable_async_execution()) && fixture.runtime.tick(fixture.producer.contract.clock.id), "in-flight consumer wave did not start");
        for (unsigned spin = 0; spin != 2000 && !started.completion().is_complete(); ++spin) {
            require(bool(fixture.runtime.pump_completions()), "in-flight consumer owner pump failed"); static_cast<void>(sleep_for(1'000'000));
        }
        require(started.completion().is_complete() && fixture.runtime.find_process(fixture.process())->state.commit_count == 2,
            "in-flight consumer did not follow committed producer B");
        const auto source_b = fixture.runtime.find_instance(fixture.instance(FeatureNodeId("P")))->render_output->invocation;
        require(bool(cancel ? fixture.runtime.cancel(fixture.process()) : fixture.runtime.suspend(fixture.process())), "consumer in-flight producer retirement failed");
        fixture.require_waiting("consumer in-flight retirement retained a complete old cohort");
        static_cast<void>(release.complete()); drain(fixture.runtime);
        const auto* consumer = fixture.runtime.find_instance(fixture.instance(FeatureNodeId{1}));
        require(consumer->state.commit_count == 2 && consumer->pending_messages.empty() && consumer->render_output &&
            consumer->render_output->input_messages.size() == 1 && consumer->render_output->input_messages.front().source.invocation == source_b &&
            fixture.runtime.find_process(fixture.process())->state.commit_count == 2,
            "producer retirement rolled back an independent consumer or fabricated its receipt");
        fixture.require_waiting("late independent consumer committed orphan publication");
        require(bool(fixture.runtime.tick(fixture.producer.contract.clock.id)), "post-consumer waiting tick failed"); drain(fixture.runtime);
        fixture.require_waiting("post-consumer tick resurrected retired B work");
    }
}

void clock_queue_preserves_steps_and_applies_its_budget() {
    CompletionSource started, release;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto executable = definition(1);
    executable.contract.clock.late_data_policy = LateDataPolicy::kQueue;
    executable.execute = [&](const InvocationToken& token, const StateSnapshot& state, std::span<const uint8_t>, std::span<const uint8_t>) {
        static_cast<void>(started.complete());
        static_cast<void>(release.completion().wait());
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{state.payload.empty() ? uint8_t{1} : static_cast<uint8_t>(state.payload[0] + 1)};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    const auto instance = activate(registry, runtime, executable);
    require(static_cast<bool>(runtime.tick(executable.contract.clock.id)), "queue first tick failed");
    require(static_cast<bool>(started.completion().wait(2'000'000'000)), "queue worker did not start");
    for (unsigned step = 0; step < 4; ++step)
        require(static_cast<bool>(runtime.tick(executable.contract.clock.id)), "bounded clock queue rejected a valid step");
    require(!runtime.tick(executable.contract.clock.id), "clock queue silently exceeded its backlog budget");
    static_cast<void>(release.complete());
    drain(runtime);
    require(runtime.find_instance(instance)->state.payload == std::vector<uint8_t>{5}, "queued clock steps were lost");
}

void suspended_dependency_discards_the_entire_deferred_chain() {
    CompletionSource started, release;
    unsigned downstream_calls = 0;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto source = definition(0);
    source.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Byte"), 1}};
    source.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        static_cast<void>(started.complete());
        static_cast<void>(release.completion().wait());
        StateDelta delta{token.state_version};
        delta.output_payload = {42};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    auto middle = source;
    middle.id = FeatureDefinitionId("Deferred.Middle");
    middle.contract.definition_id = middle.id;
    middle.contract.execution = ExecutionKind::kProcess;
    middle.contract.has_continuation = true;
    middle.contract.ports.push_back({PortId("In"), PortDirection::kInput, ValueSchemaId("Byte"), 1});
    middle.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t>) {
        StateDelta delta{token.state_version};
        delta.output_payload.assign(input.begin(), input.end());
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    auto target = source;
    target.id = FeatureDefinitionId("Deferred.Target");
    target.contract.definition_id = target.id;
    target.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Byte"), 1}};
    target.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        ++downstream_calls;
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), StateDelta{token.state_version}};
    };
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)) && static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, middle)) &&
            static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, target)), "deferred registration failed");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://async-suspended-chain";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version}, {FeatureNodeId("Middle"), middle.id, middle.version},
                   {FeatureNodeId("Target"), target.id, target.version}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Middle"), PortId("In"),
                    EdgeKind::kData, source.contract.clock.id, source.contract.clock.id, 1, 1, 0},
                   {FeatureNodeId("Middle"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, source.contract.clock.id, source.contract.clock.id, 1, 1, 0}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(graph, compiled.value)), "deferred graph failed");
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "deferred tick failed");
    require(static_cast<bool>(started.completion().wait(2'000'000'000)), "deferred source did not start");
    require(runtime.pending_execution_count() == 1, "dependent was not deferred");
    require(static_cast<bool>(runtime.suspend(runtime.process(compiled.value.provenance[1].instance_id).value)), "middle suspend failed");
    static_cast<void>(release.complete());
    drain(runtime);
    require(downstream_calls == 0, "discarded downstream still executed after its middle dependency was suspended");
}

void cancelled_backoff_discards_uncommitted_dependents() {
    CompletionSource started, release;
    unsigned downstream_calls = 0;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto source = definition(0, ExecutionKind::kProcess);
    source.contract.failure = FailurePolicy::kRetry;
    source.contract.retry = {3, 5};
    source.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Byte"), 1}};
    source.execute = [&](const InvocationToken&, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        static_cast<void>(started.complete());
        static_cast<void>(release.completion().wait());
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_argument("retryable"), {}};
    };
    auto target = definition(0);
    target.id = FeatureDefinitionId("Cancelled.Target");
    target.contract.definition_id = target.id;
    target.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Byte"), 1}};
    target.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        ++downstream_calls;
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), StateDelta{token.state_version}};
    };
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)) && static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, target)), "backoff registration failed");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://async-cancelled-backoff";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version}, {FeatureNodeId("Target"), target.id, target.version}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kData, source.contract.clock.id, source.contract.clock.id, 1, 1, 0}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(graph, compiled.value)), "backoff graph failed");
    const auto source_id = compiled.value.provenance[0].instance_id;
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "backoff tick failed");
    require(static_cast<bool>(started.completion().wait(2'000'000'000)), "backoff source did not start");
    static_cast<void>(release.complete());
    drain(runtime);
    require(runtime.find_instance(source_id)->retry_pending, "backoff was not pending");
    require(static_cast<bool>(runtime.cancel(runtime.process(source_id).value)), "backoff cancel failed");
    require(static_cast<bool>(runtime.pump_completions()), "cancel pump failed");
    drain(runtime);
    require(downstream_calls == 0, "cancelled retry released a dependent without committed input");
}

void terminal_transition_releases_the_clock_after_backoff() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto source = definition(0, ExecutionKind::kProcess);
    source.contract.state = StatePolicy::kProcessState;
    source.contract.failure = FailurePolicy::kRetry;
    source.contract.retry = {3, 5};
    source.execute = [](const InvocationToken&, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_argument("retryable"), {}};
    };
    auto peer = definition(0);
    peer.id = FeatureDefinitionId("Terminal.Peer");
    peer.contract.definition_id = peer.id;
    peer.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), StateDelta{token.state_version}};
    };
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)) && static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, peer)), "terminal registration failed");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://async-terminal-backoff";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version}, {FeatureNodeId("Peer"), peer.id, peer.version}};
    graph.process_subgraphs = {{FeatureNodeId("Source"), {"Running", "Done"}, {"Running->Done"}, 1, "Running", {"Done"}}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(graph, compiled.value)), "terminal graph failed");
    const auto source_id = compiled.value.provenance[0].instance_id;
    const auto peer_id = compiled.value.provenance[1].instance_id;
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "terminal first tick failed");
    drain(runtime);
    require(runtime.find_instance(source_id)->retry_pending && runtime.find_instance(peer_id)->state.commit_count == 1,
            "terminal test did not establish a pending retry and committed peer");
    require(static_cast<bool>(runtime.transition(runtime.process(source_id).value, "Done")), "terminal transition failed");
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "terminal second tick failed");
    drain(runtime);
    require(!runtime.find_instance(source_id)->retry_pending && runtime.find_instance(peer_id)->state.commit_count == 2,
            "terminal Process retained its retry and blocked the same-clock peer");
}

void retries_keep_owned_input_and_state() {
    for (const auto backoff : {0u, 2u}) {
        CompletionSource started, release;
        unsigned calls = 0;
        std::vector<InvocationToken> entered;
        FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        FeatureRuntime runtime(registry);
        ReleaseOnExit release_on_exit{release};
        require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
        auto executable = definition(0, ExecutionKind::kCall);
        executable.contract.failure = FailurePolicy::kRetry;
        executable.contract.retry = {3, backoff};
        executable.execute = [&](const InvocationToken& token, const StateSnapshot& state, std::span<const uint8_t> input, std::span<const uint8_t>) {
            ++calls;
            entered.push_back(token);
            if (calls == 1) {
                static_cast<void>(started.complete());
                static_cast<void>(release.completion().wait());
            }
            if (state.payload.empty() && input.size() == 2 && input[0] == 1 && input[1] == 2) {
                if (calls < 3) return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_argument("retryable"), {}};
                StateDelta delta{};
                delta.expected_version = token.state_version;
                delta.state_payload = std::vector<uint8_t>{4};
                return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
            }
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_argument("retry snapshot changed"), {}};
        };
        const auto id = activate(registry, runtime, executable);
        std::vector<uint8_t> input{1, 2};
        require(static_cast<bool>(runtime.dispatch_call(id, input)), "retry submit failed");
        require(static_cast<bool>(started.completion().wait(2'000'000'000)), "retry callback did not start");
        input = {9, 9};
        static_cast<void>(release.complete());
        drain(runtime);
        if (backoff != 0) {
            const auto waiting = runtime.inspect_invocations(id).value;
            require(waiting.retry && waiting.open_attempts.empty() && !waiting.last_committed &&
                    waiting.last_failed->attempt.token == entered[0] && waiting.retry->origin == entered[0] &&
                    waiting.retry->next_attempt == 2, "async failed retry appeared committed or still open");
        }
        for (unsigned tick = 0; runtime.find_instance(id)->retry_pending && tick < 8; ++tick) {
            require(static_cast<bool>(runtime.tick(executable.contract.clock.id)), "retry tick failed");
            drain(runtime);
        }
        require(calls == 3 && runtime.find_instance(id)->state.payload == std::vector<uint8_t>{4}, "async retry did not converge");
        const auto committed = runtime.inspect_invocations(id).value;
        require(committed.last_committed->attempt.token == entered[2] &&
                committed.last_failed->attempt.token == entered[1] &&
                committed.last_failed->stage == InvocationFailureStage::kCallback &&
                committed.last_issued->token == entered[2] && !committed.retry && committed.open_attempts.empty(),
                "async retries collapsed failed and committed attempts");
    }
}

void callback_exception_is_reclaimed_and_shutdown_joins() {
    CompletionSource started, release, join_ready, returned;
    bool fail = true;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto executable = definition(0, ExecutionKind::kCall);
    executable.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        if (fail) { fail = false; throw std::runtime_error("Feature failure"); }
        static_cast<void>(started.complete());
        static_cast<void>(release.completion().wait());
        static_cast<void>(returned.complete());
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{5};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    const auto id = activate(registry, runtime, executable);
    require(static_cast<bool>(runtime.dispatch_call(id)), "throwing submit failed");
    auto error = aeg::foundation::Result::success();
    for (unsigned spin = 0; runtime.pending_execution_count() != 0 && spin < 2000; ++spin) {
        error = runtime.pump_completions();
        static_cast<void>(sleep_for(1'000'000));
    }
    require(!error && error.message == "Feature failure" && runtime.pending_execution_count() == 0,
            "throwing callback left an unreachable invocation");
    const auto failed = runtime.inspect_invocations(id).value;
    require(failed.last_failed && failed.last_failed->stage == InvocationFailureStage::kCallback &&
            failed.last_failed->result.message == "Feature failure" && !failed.last_committed && failed.open_attempts.empty(),
            "exception observation lost the genuine callback failure");
    require(static_cast<bool>(runtime.dispatch_call(id)), "exception left instance busy");
    require(static_cast<bool>(started.completion().wait(2'000'000'000)), "shutdown callback did not start");
    WorkerExecutor helper({ExecutorDomain::kIoWorker, 1, false});
    TaskDesc task{};
    task.domain = ExecutorDomain::kIoWorker;
    task.execute = [&](const TaskContext&) {
        static_cast<void>(join_ready.completion().wait());
        static_cast<void>(release.complete());
        return aeg::foundation::Result::success();
    };
    require(static_cast<bool>(helper.submit(std::move(task))), "shutdown helper failed");
    static_cast<void>(join_ready.complete());
    require(static_cast<bool>(runtime.shutdown_execution()), "shutdown join failed");
    require(returned.completion().is_complete() && runtime.pending_execution_count() == 0,
            "shutdown returned with live callbacks");
    require(runtime.find_instance(id)->state.payload.empty(), "shutdown committed a pending result");
}

void migration_invalidates_an_in_flight_snapshot() {
    CompletionSource started, release;
    bool snapshot_preserved = false;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto executable = definition(0, ExecutionKind::kCall);
    executable.execute = [&](const InvocationToken& token, const StateSnapshot& state, std::span<const uint8_t>, std::span<const uint8_t>) {
        static_cast<void>(started.complete());
        static_cast<void>(release.completion().wait());
        snapshot_preserved = state.schema_version == Version{1, 0, 0} && state.payload.empty();
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{9};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    const auto id = activate(registry, runtime, executable);
    require(static_cast<bool>(runtime.dispatch_call(id)), "migration submit failed");
    require(static_cast<bool>(started.completion().wait(2'000'000'000)), "old callback did not start");
    const auto before = runtime.inspect_invocations(id).value;
    require(before.open_attempts.size() == 1 && before.open_attempts[0].committable, "migration lost old issued token");
    auto replacement = executable;
    replacement.version = {2, 0, 0};
    replacement.contract.version = replacement.version;
    replacement.contract.state_payload.schema_version = {2, 0, 0};
    replacement.execute = [](const InvocationToken& token, const StateSnapshot& state, std::span<const uint8_t>, std::span<const uint8_t>) {
        require(state.schema_version == Version{2, 0, 0}, "new callback received old schema");
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{2};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    require(static_cast<bool>(runtime.replace_definition(replacement, {{1,0,0},{2,0,0},{1,0,0},{2,0,0}})), "in-flight migration failed");
    const auto migrated = runtime.inspect_invocations(id).value;
    require(!migrated.open_attempts[0].committable && !migrated.last_committed && !migrated.last_failed &&
            migrated.last_issued->token.generation != runtime.generation(), "migration invented a new invocation commit");
    static_cast<void>(release.complete());
    drain(runtime);
    require(snapshot_preserved, "migration modified worker snapshot");
    require(runtime.find_instance(id)->state.schema_version == Version{2,0,0} &&
            runtime.find_instance(id)->state.payload.empty(), "late old-schema result committed");
    const auto retired = runtime.inspect_invocations(id).value;
    require(retired.open_attempts.empty() && !retired.last_failed && !retired.last_committed &&
            retired.last_issued->token == before.last_issued->token, "late generation polluted current histories");
    require(static_cast<bool>(runtime.dispatch_call(id)), "new generation stayed busy");
    drain(runtime);
    require(runtime.find_instance(id)->state.payload == std::vector<uint8_t>{2}, "new generation result failed");
    const auto committed = runtime.inspect_invocations(id).value;
    require(committed.last_committed->attempt.token.generation == runtime.generation() && !committed.last_failed,
            "new generation did not retain its actual committed invocation");
}

void invalid_delta_and_republication_preserve_message_ownership() {
    CompletionSource started, release;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime(registry);
    ReleaseOnExit release_on_exit{release};
    require(static_cast<bool>(runtime.enable_async_execution()), "async init failed");
    auto source = definition(0);
    source.contract.state_payload.max_size = 1;
    source.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Byte"), 1}};
    source.execute = [&](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        static_cast<void>(started.complete());
        static_cast<void>(release.completion().wait());
        StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = std::vector<uint8_t>{1, 2}; // Reject the complete transaction.
        delta.output_payload = {7};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), delta};
    };
    auto target = definition(1, ExecutionKind::kCall);
    target.contract.state = StatePolicy::kStateless;
    target.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Byte"), 1}};
    target.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>) {
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), StateDelta{token.state_version}};
    };
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)) && static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, target)), "queue registration failed");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://async-transaction";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version}, {FeatureNodeId("Target"), target.id, target.version}};
    graph.edges = {{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                    EdgeKind::kQueue, source.contract.clock.id, target.contract.clock.id, 1, 2, 0}};
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(graph, compiled.value)), "queue graph publication failed");
    const auto source_id = compiled.value.provenance[0].instance_id;
    const auto target_id = compiled.value.provenance[1].instance_id;
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "bad delta submit failed");
    require(static_cast<bool>(started.completion().wait(2'000'000'000)), "bad delta callback did not start");
    static_cast<void>(release.complete());
    auto rejected = aeg::foundation::Result::success();
    for (unsigned spin = 0; runtime.pending_execution_count() && spin < 2000; ++spin) {
        rejected = runtime.pump_completions();
        static_cast<void>(sleep_for(1'000'000));
    }
    require(!rejected && runtime.pending_execution_count() == 0, "invalid state result was not rejected");
    const auto rejected_attempt = runtime.inspect_invocations(source_id).value;
    require(rejected_attempt.last_failed && rejected_attempt.last_failed->stage == InvocationFailureStage::kCommit &&
            !rejected_attempt.last_committed && rejected_attempt.open_attempts.empty(),
            "rejected callback delta was reported as a commit");
    require(runtime.find_instance(source_id)->state.commit_count == 0 &&
            runtime.find_instance(source_id)->state.payload.empty() &&
            runtime.find_instance(target_id)->pending_input.empty(), "invalid delta partially committed state or output");

    // Run another generation while an old result is terminal but uncollected.
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "old generation submit failed");
    auto replacement = FeatureGraphCompiler{}.compile(graph, registry);
    require(static_cast<bool>(replacement) && static_cast<bool>(runtime.publish_graph(graph, replacement.value)), "new graph publication failed");
    drain(runtime);
    require(runtime.find_instance(source_id) == nullptr && runtime.find_instance(target_id) == nullptr,
            "republication retained old owners");
    require(runtime.find_instance(replacement.value.provenance[0].instance_id)->state.commit_count == 0 &&
            runtime.find_instance(replacement.value.provenance[1].instance_id)->pending_input.empty(),
            "old generation changed newly published state or messages");
}
}

int main() {
    try {
        five_domains_execute_independently_and_commit_on_owner();
        queued_cancel_and_late_revision_are_reclaimed();
        same_clock_dependencies_wait_for_owner_commit();
        complete_render_wave_publishes_before_queued_restart();
        render_process_publication_does_not_resurrect_retired_intent();
        typed_light_list_process_retirement_waits_without_orphan_publication();
        typed_light_list_pending_delivery_survives_retirement_and_replacement();
        typed_light_list_retirement_keeps_malformed_inputs_strict();
        typed_light_list_targeted_exports_share_one_retired_declaration();
        typed_light_list_retired_worker_is_cleanup_not_publication();
        typed_light_list_consumer_in_flight_keeps_its_own_commit_boundary();
        clock_queue_preserves_steps_and_applies_its_budget();
        retries_keep_owned_input_and_state();
        callback_exception_is_reclaimed_and_shutdown_joins();
        migration_invalidates_an_in_flight_snapshot();
        invalid_delta_and_republication_preserve_message_ownership();
        bool regressions_passed = true;
        for (const auto regression : {suspended_dependency_discards_the_entire_deferred_chain,
                                      cancelled_backoff_discards_uncommitted_dependents,
                                      terminal_transition_releases_the_clock_after_backoff}) {
            try { regression(); }
            catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                regressions_passed = false;
            }
        }
        if (!regressions_passed) return 1;
        std::cout << "Feature async execution smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
