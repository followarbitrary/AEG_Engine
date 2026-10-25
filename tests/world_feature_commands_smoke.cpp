/** @brief Exercises the production typed Feature-to-World command boundary. */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Components/component_bundle.hpp"
#include "concurrency.hpp"
#include "feature_graph.hpp"
#include "feature_world_transform.hpp"
#include "serialization.hpp"
#include "world_scope.hpp"
#include "../Engine/Feature/private/feature_outbox.hpp"
#include "../Engine/Runtime/private/feature_world_commands.hpp"

namespace {
using namespace aeg;
using namespace aeg::feature;
using foundation::Result;
using foundation::ResultCode;
using Transform = engine::components::Transform;
using Command = engine::detail::WorldTransformCommand;
using Consumer = engine::detail::FeatureWorldCommandConsumer;
using Access = feature::detail::FeatureOutboxAccess;
using Delivery = feature::detail::OutboxDeliveryState;
constexpr SideEffectId kEffect{"World.Transform"};
constexpr SideEffectId kOtherEffect{"Fixture.World.Other"};
constexpr FeatureDefinitionId kDefinition{"Fixture.World.CommandProducer"};
constexpr ModuleIdentity kModule{"com.aeg.world.command.fixture"};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class Value> void succeeds(const Value& result, const char* message) {
    require(static_cast<bool>(result), message);
}

Transform transform(float x) {
    Transform value{};
    value.translation = {x, 1.0f, -2.0f};
    value.rotation = {0.0f, 0.0f, 0.6f, 0.8f};
    value.scale = {1.0f, 2.0f, 3.0f};
    return value;
}

bool has_value(const Transform* value, float x) {
    return value && value->translation.x == x && value->translation.y == 1.0f &&
        value->translation.z == -2.0f && value->rotation.x == 0.0f && value->rotation.y == 0.0f &&
        value->rotation.z == 0.6f && value->rotation.w == 0.8f &&
        value->scale.x == 1.0f && value->scale.y == 2.0f && value->scale.z == 3.0f;
}

std::vector<uint8_t> reflected_blob(const reflection::TypeRegistry& registry, const Transform& value) {
    serialization::BinaryWriter writer;
    succeeds(serialization::serialize_object(registry, reflection::TypeId("Game.Transform"), &value, writer),
             "serialize actual Game.Transform fields");
    return {writer.data().begin(), writer.data().end()};
}

OutboxIntent wire_command(uint64_t revision, std::span<const uint8_t> expected,
                          std::span<const uint8_t> replacement) {
    serialization::BinaryWriter writer;
    succeeds(writer.write_u64(revision), "write expected World revision");
    succeeds(writer.write_u32(static_cast<uint32_t>(expected.size())), "write expected Transform length");
    succeeds(writer.write_bytes(expected), "write expected Transform blob");
    succeeds(writer.write_u32(static_cast<uint32_t>(replacement.size())), "write replacement Transform length");
    succeeds(writer.write_bytes(replacement), "write replacement Transform blob");
    return {kEffect, 1, {writer.data().begin(), writer.data().end()}};
}

struct Field final { uint64_t id{}; std::vector<uint8_t> bytes{}; };
struct Archive final { uint64_t type{}; uint32_t version{}; std::vector<Field> fields{}; };

Archive read_archive(std::span<const uint8_t> bytes) {
    serialization::BinaryReader reader(bytes);
    const auto type = reader.read_u64(); const auto version = reader.read_u32(); const auto count = reader.read_u32();
    require(type && version && count, "real Transform archive header is truncated");
    Archive archive{type.value, version.value, {}};
    for (uint32_t index = 0; index != count.value; ++index) {
        const auto id = reader.read_u64(); const auto size = reader.read_u32();
        require(id && size, "real Transform field header is truncated");
        const auto payload = reader.read_bytes(size.value);
        succeeds(payload, "read actual serialized Transform field");
        archive.fields.push_back({id.value, {payload.value.begin(), payload.value.end()}});
    }
    require(reader.remaining() == 0 && archive.fields.size() == 3, "unexpected real Transform archive");
    return archive;
}

std::vector<uint8_t> write_archive(const Archive& archive) {
    serialization::BinaryWriter writer;
    succeeds(writer.write_u64(archive.type), "write reflected type");
    succeeds(writer.write_u32(archive.version), "write reflected version");
    succeeds(writer.write_u32(static_cast<uint32_t>(archive.fields.size())), "write reflected field count");
    for (const auto& field : archive.fields) {
        succeeds(writer.write_u64(field.id), "write reflected field identity");
        succeeds(writer.write_u32(static_cast<uint32_t>(field.bytes.size())), "write reflected field size");
        succeeds(writer.write_bytes(field.bytes), "write reflected field payload");
    }
    return {writer.data().begin(), writer.data().end()};
}

void test_codec_preserves_semantics_and_rejects_noncanonical_records() {
    reflection::TypeRegistry registry;
    succeeds(engine::components::register_render_component_reflection(registry), "register actual Transform schema");
    Command input{17, transform(5.0f), transform(7.0f)};
    input.replacement.translation.y = -4.0f;
    input.replacement.rotation = {0.0f, 0.6f, 0.0f, 0.8f};
    input.replacement.scale = {2.0f, 3.0f, 4.0f};
    const auto encoded = engine::detail::encode_world_transform_command(registry, input);
    succeeds(encoded, "encode typed World Transform command");
    const auto expected = reflected_blob(registry, input.expected);
    const auto replacement = reflected_blob(registry, input.replacement);
    require(encoded.value.effect == kEffect && encoded.value.version == 1 &&
            encoded.value.payload == wire_command(17, expected, replacement).payload,
            "command wire did not contain the two canonical reflected values and World revision");
    const auto decoded = engine::detail::decode_world_transform_command(registry, encoded.value);
    succeeds(decoded, "decode typed World Transform command");
    const auto& after = decoded.value.replacement;
    require(decoded.value.expected_world_revision == 17 && has_value(&decoded.value.expected, 5.0f) &&
        after.translation.x == 7.0f && after.translation.y == -4.0f && after.translation.z == -2.0f &&
        after.rotation.x == 0.0f && after.rotation.y == 0.6f && after.rotation.z == 0.0f && after.rotation.w == 0.8f &&
        after.scale.x == 2.0f && after.scale.y == 3.0f && after.scale.z == 4.0f,
        "typed codec changed literal translation, rotation, scale or revision");
    const auto rejected = [&](const OutboxIntent& bad, const char* message) {
        require(!engine::detail::decode_world_transform_command(registry, bad), message);
    };
    auto bad = encoded.value; bad.effect = kOtherEffect;
    rejected(bad, "typed codec accepted a foreign effect");
    bad = encoded.value; bad.version = 2;
    rejected(bad, "typed codec accepted a future command version");
    bad = encoded.value; bad.payload.pop_back();
    rejected(bad, "typed codec accepted a truncated replacement blob");
    bad = encoded.value; bad.payload.push_back(0xa5);
    rejected(bad, "typed codec accepted trailing command bytes");
    auto archive = read_archive(expected);
    archive.version = 2;
    rejected(wire_command(17, write_archive(archive), replacement), "typed codec accepted future Transform schema");
    archive = read_archive(expected); archive.fields.erase(archive.fields.begin());
    rejected(wire_command(17, write_archive(archive), replacement), "typed codec defaulted a missing Transform field");
    archive = read_archive(expected); archive.fields.push_back(archive.fields.front());
    rejected(wire_command(17, write_archive(archive), replacement), "typed codec accepted a duplicated Transform field");
    archive = read_archive(expected); archive.fields.front().id = reflection::FieldId("unknown_transform_field").value();
    rejected(wire_command(17, write_archive(archive), replacement), "typed codec skipped an unknown Transform field");
    auto nonfinite = input.expected;
    nonfinite.translation.x = (std::numeric_limits<float>::quiet_NaN)();
    rejected(wire_command(17, reflected_blob(registry, nonfinite), replacement), "typed codec accepted NaN from a real archive");
    input.replacement.scale.z = (std::numeric_limits<float>::infinity)();
    require(!engine::detail::encode_world_transform_command(registry, input), "typed encoder accepted infinite Transform data");
}

enum class Metadata { kValid, kRaw, kWrongType, kWrongVersion, kWrongSize, kWrongAlignment, kMissing };
struct WorldFixture final {
    reflection::TypeRegistry schemas{};
    ecs::WorldManager manager{};
    std::shared_ptr<ecs::World> state{};
    std::unique_ptr<world::WorldScope> scope{};
    ecs::EntityId entity{}, other{};

    explicit WorldFixture(Metadata metadata = Metadata::kValid) {
        succeeds(engine::components::register_render_component_reflection(schemas), "register World fixture reflection");
        const auto handle = manager.create_world({"TypedCommandWorld", true}); succeeds(handle, "create actual World");
        const auto found = manager.find(handle.value); succeeds(found, "find actual World"); state = found.value;
        if (metadata != Metadata::kMissing) {
            ecs::ComponentDescriptor descriptor{ecs::ComponentTypeId("Game.Transform"), "Game.Transform",
                sizeof(Transform), alignof(Transform), true, reflection::TypeId("Game.Transform"), 1};
            if (metadata == Metadata::kRaw) { descriptor.schema_type = {}; descriptor.schema_version = 0; }
            if (metadata == Metadata::kWrongType) descriptor.schema_type = reflection::TypeId("Fixture.OtherTransform");
            if (metadata == Metadata::kWrongVersion) descriptor.schema_version = 2;
            if (metadata == Metadata::kWrongSize) descriptor.size += 4;
            if (metadata == Metadata::kWrongAlignment) descriptor.alignment *= 2;
            succeeds(state->register_component<Transform>(descriptor), "register actual typed component storage");
        }
        scope = std::make_unique<world::WorldScope>(state); succeeds(scope->initialize(), "initialize actual World Scope");
        const auto first = state->create_entity(), second = state->create_entity();
        require(first && second, "create actual routed entities"); entity = first.value; other = second.value;
        if (metadata != Metadata::kMissing) {
            succeeds(state->emplace<Transform>(entity, transform(5.0f)), "place target Transform");
            succeeds(state->emplace<Transform>(other, transform(5.0f)), "place other Transform");
        }
    }
    void expect(float x, uint64_t revision = 0) const {
        require(has_value(state->get<Transform>(entity), x) && has_value(state->get<Transform>(other), 5.0f) &&
            state->revision() == revision, "World command changed unexpected values, entity or snapshot revision");
    }
    OutboxIntent command(float before, float after, uint64_t revision = 0) const {
        const auto encoded = engine::detail::encode_world_transform_command(schemas, {revision, transform(before), transform(after)});
        succeeds(encoded, "encode actual producer command"); return encoded.value;
    }
};

struct FeatureFixture final {
    std::vector<OutboxIntent> outgoing{};
    std::vector<InvocationToken> calls{};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry};
    FeatureInstanceId primary{}, other{};

    FeatureFixture() {
        FeatureDefinition definition{};
        definition.id = kDefinition; definition.version = {1, 0, 0}; definition.owner_module = kModule;
        auto& contract = definition.contract;
        contract.definition_id = definition.id; contract.version = definition.version;
        contract.domains = to_mask(FeatureDomain::kLogic); contract.execution = ExecutionKind::kCall;
        contract.state = StatePolicy::kInstanceState; contract.scope = {ScopeId("World")};
        contract.state_payload = {{1, 0, 0}, 1};
        contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("Fixture.WorldCommandTick"), 60, 4, LateDataPolicy::kReject,
            aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
        contract.permissions.version = 1;
        contract.side_effects = {{kEffect, 1, SideEffectKind::kCommand, CommitPolicy::kOutbox},
                                {kOtherEffect, 1, SideEffectKind::kCommand, CommitPolicy::kOutbox}};
        // This callback owns no World pointer. Only Runtime accepts its immutable command bytes.
        definition.execute = [this](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                                    std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
            calls.push_back(token); StateDelta delta{token.state_version}; delta.outbox = outgoing;
            return {Result::success(), std::move(delta)};
        };
        succeeds(aeg::feature::test::register_clock_test_definition(registry, std::move(definition)), "register real World command producer");
        FeatureGraphDocument graph{};
        graph.asset_uuid = "fixture://world-commands"; graph.schema_version = 1; graph.graph_version = 1;
        graph.contract_digest = registry.contract_digest();
        graph.nodes = {{FeatureNodeId("Primary"), kDefinition, {1, 0, 0}, OwnerPath{{FeatureNodeId("WorldBusiness")}}, {}},
                       {FeatureNodeId("Other"), kDefinition, {1, 0, 0}, OwnerPath{{FeatureNodeId("OtherBusiness")}}, {}}};
        auto compiled = FeatureGraphCompiler{}.compile(graph, registry); succeeds(compiled, "compile real producer graph");
        succeeds(runtime.publish_graph(graph, compiled.value), "publish real producer graph");
        for (const auto& source : compiled.value.provenance) {
            if (source.node_id == FeatureNodeId("Primary")) primary = source.instance_id;
            if (source.node_id == FeatureNodeId("Other")) other = source.instance_id;
        }
        require(primary.is_valid() && other.is_valid() && primary != other, "published command owners are not distinct");
    }
    void dispatch(FeatureInstanceId owner, std::vector<OutboxIntent> intents) {
        outgoing = std::move(intents); succeeds(runtime.dispatch_call(owner), "dispatch actual World command Feature");
    }
    auto bind(FeatureInstanceId owner) { return Access::bind(runtime, owner, kEffect, 1); }
    std::vector<feature::detail::FeatureOutboxHandle> capture(FeatureInstanceId owner) {
        const auto binding = bind(owner); succeeds(binding, "bind canonical journal observation");
        const auto entries = Access::capture(runtime, binding.value); succeeds(entries, "capture committed World commands");
        return entries.value;
    }
};

void test_multiple_old_commits_apply_in_order_to_the_explicit_owner() {
    FeatureFixture feature; WorldFixture world;
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.primary, *world.scope, world.entity, kEffect, 1), "bind real typed World route");
    const auto canonical = feature.bind(feature.primary); succeeds(canonical, "inspect canonical typed route owner");
    require(canonical.value.source() && canonical.value.source()->node_id == FeatureNodeId("Primary") &&
        canonical.value.source()->definition_id == kDefinition && canonical.value.source()->owner_module == kModule &&
        canonical.value.source()->owner_path.nodes == std::vector<FeatureNodeId>{FeatureNodeId("WorldBusiness")},
        "typed route lost real authored node, definition, module or exact owner path");
    feature.dispatch(feature.primary, {world.command(5.0f, 7.0f)});
    feature.dispatch(feature.primary, {world.command(7.0f, 9.0f)});
    feature.dispatch(feature.other, {world.command(5.0f, 11.0f)});
    const auto commands = feature.capture(feature.primary), other_commands = feature.capture(feature.other);
    require(commands.size() == 2 && other_commands.size() == 1 && feature.calls.size() == 3 &&
        *commands[0].invocation() == feature.calls[0] && *commands[1].invocation() == feature.calls[1] &&
        commands[0].entry_index() == 0 && commands[1].entry_index() == 0,
        "actual dispatches did not retain both older commits independently");
    world.expect(5.0f);
    succeeds(consumer.consume(*world.scope), "consume two committed typed World commands"); world.expect(9.0f);
    require(commands[0].receipt().state == Delivery::kApplied && commands[1].receipt().state == Delivery::kApplied &&
        other_commands[0].receipt().state == Delivery::kPending && feature.capture(feature.primary).empty(),
        "typed consumer lost acknowledgements or consumed another actual owner");
    succeeds(consumer.consume(*world.scope), "consume drained World route"); world.expect(9.0f);
    const auto snapshot = world.scope->commit(23, 13); succeeds(snapshot, "publish applied World snapshot");
    require(snapshot.value->revision() == 1 && snapshot.value->frame_number() == 23 && snapshot.value->fixed_tick() == 13 &&
        has_value(snapshot.value->get<Transform>(world.entity), 9.0f), "snapshot lost actual typed command result");
}

void test_malformed_second_entry_preserves_partial_acknowledgements() {
    FeatureFixture feature; WorldFixture world; Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.primary, *world.scope, world.entity, kEffect, 1), "bind partial delivery route");
    auto malformed = world.command(7.0f, 9.0f); malformed.payload.push_back(0xa5);
    feature.dispatch(feature.primary, {world.command(5.0f, 7.0f), malformed, world.command(7.0f, 9.0f)});
    const auto commands = feature.capture(feature.primary); require(commands.size() == 3, "missing real pending entries");
    require(!consumer.consume(*world.scope), "typed consumer accepted a malformed second command"); world.expect(7.0f);
    require(commands[0].receipt().state == Delivery::kApplied && commands[1].receipt().state == Delivery::kPending &&
        commands[2].receipt().state == Delivery::kPending && commands[0].entry_index() == 0 &&
        commands[1].entry_index() == 1 && commands[2].entry_index() == 2,
        "typed preflight failure acknowledged unexecuted commands or undid an applied entry");
    require(!consumer.consume(*world.scope), "retry skipped the malformed pending command"); world.expect(7.0f);
    const auto binding = feature.bind(feature.primary); succeeds(binding, "observe malformed command authority");
    succeeds(Access::reject(feature.runtime, binding.value, commands[1], ResultCode::kInvalidArgument), "retire malformed command explicitly");
    succeeds(consumer.consume(*world.scope), "retry after explicit malformed command rejection"); world.expect(9.0f);
    require(commands[0].receipt().state == Delivery::kApplied && commands[1].receipt().state == Delivery::kRejected &&
        commands[2].receipt().state == Delivery::kApplied, "retry replayed the first command or lost final application");
}

void test_stale_expected_values_and_revision_remain_pending() {
    FeatureFixture feature; WorldFixture world; Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.primary, *world.scope, world.entity, kEffect, 1), "bind stale value route");
    feature.dispatch(feature.primary, {world.command(5.0f, 7.0f), world.command(5.0f, 9.0f)});
    const auto commands = feature.capture(feature.primary);
    require(commands.size() == 2 && !consumer.consume(*world.scope), "typed consumer ignored a stale expected Transform");
    world.expect(7.0f);
    require(commands[0].receipt().state == Delivery::kApplied && commands[1].receipt().state == Delivery::kPending &&
        !consumer.consume(*world.scope), "stale expected-before retry changed terminal delivery status"); world.expect(7.0f);
    *world.state->get<Transform>(world.entity) = transform(5.0f);
    succeeds(consumer.consume(*world.scope), "retry pending command after owner restores its expected value"); world.expect(9.0f);
    feature.dispatch(feature.primary, {world.command(9.0f, 11.0f)});
    const auto revision_pending = feature.capture(feature.primary); require(revision_pending.size() == 1, "missing revision probe");
    succeeds(world.scope->commit(1, 1), "advance real World snapshot revision");
    require(!consumer.consume(*world.scope) && revision_pending[0].receipt().state == Delivery::kPending,
            "typed consumer ignored a stale expected World revision"); world.expect(9.0f, 1);
}

void test_route_validation_and_foreign_execution_do_not_mutate_worlds() {
    FeatureFixture feature; WorldFixture world, foreign_world; Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.primary, *world.scope, world.entity, kEffect, 1), "bind explicit target World");
    require(!consumer.bind(feature.primary, *world.scope, world.entity, kEffect, 1) &&
        !consumer.bind(feature.primary, *world.scope, world.other, kEffect, 1) &&
        !consumer.bind(feature.primary, *foreign_world.scope, foreign_world.entity, kEffect, 1) &&
        !consumer.bind(feature.other, *world.scope, world.entity, kOtherEffect, 1) &&
        !consumer.bind(feature.other, *world.scope, world.entity, kEffect, 2) &&
        !consumer.bind({}, *world.scope, world.entity, kEffect, 1),
        "typed route accepted duplicate authority, another protocol or invalid owner");
    feature.dispatch(feature.primary, {world.command(5.0f, 7.0f)});
    const auto commands = feature.capture(feature.primary);
    succeeds(consumer.consume(*foreign_world.scope), "unrouted World consumes zero commands");
    world.expect(5.0f); foreign_world.expect(5.0f);
    FeatureFixture foreign_runtime;
    foreign_runtime.dispatch(foreign_runtime.primary, {foreign_world.command(5.0f, 19.0f)});
    const auto foreign_commands = foreign_runtime.capture(foreign_runtime.primary);
    const auto owner_thread = concurrency::current_thread_id();
    concurrency::ThreadId probe_thread{}; Result foreign_result;
    concurrency::WorkerExecutor worker({concurrency::ExecutorDomain::kCpuWorker, 1, false});
    concurrency::TaskDesc probe{}; probe.domain = concurrency::ExecutorDomain::kCpuWorker;
    probe.execute = [&](const concurrency::TaskContext&) {
        probe_thread = concurrency::current_thread_id(); foreign_result = consumer.consume(*world.scope);
        return Result::success();
    };
    const auto probe_submission = worker.submit(std::move(probe)); succeeds(probe_submission, "submit foreign World owner probe");
    succeeds(probe_submission.value.wait(2'000'000'000), "wait for real foreign World owner probe");
    require(probe_thread.is_valid() && probe_thread != owner_thread && !foreign_result &&
        commands.size() == 1 && commands[0].receipt().state == Delivery::kPending,
        "foreign owner thread applied or acknowledged the World command"); world.expect(5.0f);
    succeeds(consumer.consume(*world.scope), "valid owner retries after foreign-thread rejection"); world.expect(7.0f);
    require(commands[0].receipt().state == Delivery::kApplied && foreign_commands.size() == 1 &&
        foreign_commands[0].receipt().state == Delivery::kPending,
        "consumer crossed Runtime authority or failed to acknowledge its actual owner"); foreign_world.expect(5.0f);
    for (const auto metadata : {Metadata::kRaw, Metadata::kWrongType, Metadata::kWrongVersion,
                               Metadata::kWrongSize, Metadata::kWrongAlignment, Metadata::kMissing}) {
        WorldFixture incompatible(metadata); Consumer guarded(feature.runtime, incompatible.schemas);
        require(!guarded.bind(feature.other, *incompatible.scope, incompatible.entity, kEffect, 1),
                "typed route accepted missing, raw or incompatible actual component metadata");
        if (metadata != Metadata::kMissing) incompatible.expect(5.0f);
    }
}

void test_world_shutdown_generation_retires_existing_routes() {
    FeatureFixture feature; WorldFixture world; Consumer old_consumer(feature.runtime, world.schemas);
    succeeds(old_consumer.bind(feature.primary, *world.scope, world.entity, kEffect, 1), "bind original World generation");
    feature.dispatch(feature.primary, {world.command(5.0f, 7.0f)});
    const auto commands = feature.capture(feature.primary); const auto handle = world.scope->handle();
    succeeds(world.scope->shutdown(), "retire actual World Scope generation");
    require(!old_consumer.consume(*world.scope), "retired World route silently consumed its pending command");
    succeeds(world.scope->initialize(), "initialize a new World Scope generation at the same handle");
    require(world.scope->handle() == handle && !old_consumer.consume(*world.scope) && commands.size() == 1 &&
        commands[0].receipt().state == Delivery::kPending, "same-handle reinitialization revived a retired World route");
    world.expect(5.0f);
    Consumer fresh_consumer(feature.runtime, world.schemas);
    succeeds(fresh_consumer.bind(feature.primary, *world.scope, world.entity, kEffect, 1), "bind actual replacement World generation");
    succeeds(fresh_consumer.consume(*world.scope), "consume pending command through fresh World binding"); world.expect(7.0f);
    require(commands[0].receipt().state == Delivery::kApplied, "fresh binding lost the retained accepted command");
}

constexpr PortId kObservationPort{"WorldObservation"};
constexpr ClockId kObservationClock{"Fixture.WorldObservationTick"};

struct ObservationFixture final {
    struct Call final {
        InvocationToken token{};
        WorldTransformInput input{};
        std::vector<uint8_t> bytes{};
    };
    std::array<std::vector<Call>, 2> calls{};
    std::array<std::vector<OutboxIntent>, 2> outgoing{};
    std::function<foundation::ResultValue<StateDelta>(unsigned, const InvocationToken&)> handler{};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    FeatureRuntime runtime{registry};
    std::array<FeatureInstanceId, 2> owners{};

    explicit ObservationFixture(ExecutionKind execution = ExecutionKind::kCall, unsigned owner_count = 1,
                                std::vector<PortContract> ports = {{kObservationPort, PortDirection::kInput,
                                    ValueSchemaId("World.Transform.Observation"), 1}}, bool retry = false) {
        FeatureGraphDocument graph{};
        graph.asset_uuid = "fixture://world-observation"; graph.schema_version = 1; graph.graph_version = 1;
        for (unsigned index = 0; index != owner_count; ++index) {
            FeatureDefinition definition{};
            definition.id = FeatureDefinitionId(0x3710 + index); definition.version = {1, 0, 0};
            definition.owner_module = kModule;
            auto& contract = definition.contract;
            contract.definition_id = definition.id; contract.version = definition.version;
            contract.domains = to_mask(FeatureDomain::kLogic); contract.execution = execution;
            contract.state = StatePolicy::kInstanceState; contract.scope = {ScopeId("World")};
            contract.state_payload = {{1, 0, 0}, 1};
            contract.clock = aeg::feature::make_periodic_clock_contract(kObservationClock, 60, 8, LateDataPolicy::kQueue,
                aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
            contract.permissions.version = 1; contract.ports = ports;
            contract.side_effects = {{kEffect, 1, SideEffectKind::kCommand, CommitPolicy::kOutbox}};
            if (retry) { contract.failure = FailurePolicy::kRetry; contract.retry = {2, 1}; }
            definition.execute = [this, index](const InvocationToken& token, const StateSnapshot&,
                std::span<const uint8_t> bytes, std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
                const auto input = decode_world_transform_input(bytes);
                if (!input) return {input.result, {}};
                calls[index].push_back({token, input.value, {bytes.begin(), bytes.end()}});
                if (handler) return handler(index, token);
                StateDelta delta{token.state_version}; delta.outbox = outgoing[index];
                return {Result::success(), std::move(delta)};
            };
            succeeds(aeg::feature::test::register_clock_test_definition(registry, definition), "register real observation Feature");
            graph.nodes.push_back({FeatureNodeId(0x3810 + index), definition.id, definition.version,
                                   OwnerPath{{FeatureNodeId("ObservationBusiness")}}, {}});
        }
        graph.contract_digest = registry.contract_digest();
        auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
        succeeds(compiled, "compile real observation graph");
        succeeds(runtime.publish_graph(graph, compiled.value), "publish real observation graph");
        for (const auto& source : compiled.value.provenance)
            for (unsigned index = 0; index != owner_count; ++index)
                if (source.node_id == FeatureNodeId(0x3810 + index)) owners[index] = source.instance_id;
        for (unsigned index = 0; index != owner_count; ++index)
            require(owners[index].is_valid(), "observation graph lost its actual published owner");
    }

    std::vector<feature::detail::FeatureOutboxHandle> capture(unsigned index = 0) {
        const auto binding = Access::bind(runtime, owners[index], kEffect, 1);
        succeeds(binding, "bind real observation journal owner");
        const auto pending = Access::capture(runtime, binding.value);
        succeeds(pending, "capture real observation commands");
        return pending.value;
    }
};

void expect_observation(const ObservationFixture::Call& call, uint64_t revision, float before,
                        double elapsed, double frame_delta, const std::vector<uint8_t>& channel) {
    const auto& value = call.input.observation;
    require(value.world_revision == revision &&
        value.before.translation == std::array<float, 3>{before, 1.f, -2.f} &&
        value.before.rotation == std::array<float, 4>{0.f, 0.f, 0.6f, 0.8f} &&
        value.before.scale == std::array<float, 3>{1.f, 2.f, 3.f} &&
        std::abs(value.elapsed_seconds - elapsed) < 1e-12 && value.frame_delta_seconds == frame_delta &&
        call.input.channel_input == channel, "World observation lost actual revision, before, logical time, frame time or Channel bytes");
}

void test_observation_reads_actual_world_and_holds_even_empty_cohorts() {
    WorldFixture world, foreign; ObservationFixture feature;
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
             "bind explicit typed World observation port");
    succeeds(world.scope->commit(7, 3), "publish actual pre-observation World revision");
    succeeds(consumer.set_frame_delta(0.125), "set actual host frame duration");
    require(!consumer.set_frame_delta(-0.1) &&
        !consumer.set_frame_delta((std::numeric_limits<double>::quiet_NaN)()) &&
        !consumer.set_frame_delta((std::numeric_limits<double>::infinity)()),
        "observation accepted a negative or non-finite host duration");
    feature.outgoing[0] = {world.command(5.f, 7.f, 1)};
    succeeds(feature.runtime.dispatch_call(feature.owners[0], std::vector<uint8_t>{0xc1, 0, 7}),
             "dispatch real projected direct invocation");
    require(feature.calls[0].size() == 1, "direct observation did not execute exactly once");
    expect_observation(feature.calls[0][0], 1, 5.f, 0, 0.125, {0xc1, 0, 7});
    const auto commands = feature.capture();
    require(commands.size() == 1 && commands[0].receipt().state == Delivery::kPending,
            "observation producer did not commit its real command");
    const auto held = feature.runtime.dispatch_call(feature.owners[0], std::vector<uint8_t>{0x99});
    require(!held && held.code == ResultCode::kNotReady && feature.calls[0].size() == 1,
            "Pending observation command admitted another invocation");
    succeeds(consumer.consume(*world.scope), "consume observed World replacement");
    require(!consumer.observation_waiting() && commands[0].receipt().state == Delivery::kApplied,
            "successful observed command did not retire its cohort");
    world.expect(7.f, 1);
    succeeds(world.scope->commit(8, 4), "commit first observed replacement");
    *world.state->get<Transform>(world.entity) = transform(41.f);
    succeeds(world.scope->commit(9, 5), "commit actual external owner replacement");
    succeeds(consumer.set_frame_delta(0.5), "set next actual host duration");
    feature.outgoing[0].clear();
    succeeds(feature.runtime.dispatch_call(feature.owners[0], std::vector<uint8_t>{0xd2}),
             "dispatch legal empty-outbox observation");
    require(feature.calls[0].size() == 2 && feature.capture().empty(), "empty-outbox callback invented a command");
    expect_observation(feature.calls[0][1], 3, 41.f, 0, 0.5, {0xd2});
    const auto empty_held = feature.runtime.dispatch_call(feature.owners[0]);
    require(!empty_held && empty_held.code == ResultCode::kNotReady && feature.calls[0].size() == 2,
            "empty outbox bypassed the World boundary admission latch");
    succeeds(consumer.consume(*foreign.scope), "foreign unrouted World consumes nothing");
    require(!feature.runtime.dispatch_call(feature.owners[0]), "foreign World retired another World's observation cohort");
    succeeds(consumer.consume(*world.scope), "retire legal empty-outbox cohort at its actual World boundary");
    succeeds(feature.runtime.dispatch_call(feature.owners[0]), "empty-outbox cohort prevented the next real admission");
    require(feature.calls[0].size() == 3 && !consumer.observation_waiting(), "empty-outbox boundary left a false wait");
    succeeds(consumer.consume(*world.scope), "retire final empty observation");
}

void test_observation_requires_actual_input_port_schema() {
    for (const auto& ports : std::vector<std::vector<PortContract>>{
        {}, {{kObservationPort, PortDirection::kOutput, ValueSchemaId("World.Transform.Observation"), 1}},
        {{kObservationPort, PortDirection::kInput, ValueSchemaId("Fixture.OtherObservation"), 1}},
        {{kObservationPort, PortDirection::kInput, ValueSchemaId("World.Transform.Observation"), 2}}}) {
        WorldFixture world; ObservationFixture feature(ExecutionKind::kCall, 1, ports);
        Consumer consumer(feature.runtime, world.schemas);
        require(!consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
                "World observation accepted a missing, output, foreign or future input contract");
        succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1),
                 "failed observation bind left a ghost route instead of preserving the original opt-out API");
    }
}

void test_clock_quantum_and_deferred_work_do_not_pin_world_commit() {
    WorldFixture world; ObservationFixture feature(ExecutionKind::kContinuous);
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
             "bind real clock-owned World observation");
    succeeds(consumer.set_frame_delta(0.05), "set backlog frame duration");
    succeeds(feature.runtime.advance(3.0 / 60.0), "advance real clock by three logical quanta");
    require(feature.calls[0].size() == 1, "same clock backlog reused one World observation before its boundary");
    expect_observation(feature.calls[0][0], 0, 5.f, 1.0 / 60.0, 0.05, {});
    succeeds(consumer.consume(*world.scope), "deferred work or empty cohort falsely pinned World commit");
    require(!consumer.observation_waiting(), "deferred work was reported as a real open invocation");
    succeeds(world.scope->commit(1, 1), "commit first empty clock cohort");
    succeeds(feature.runtime.pump_completions(), "resume actual deferred clock admission");
    require(feature.calls[0].size() == 2, "World boundary did not resume its actual deferred invocation");
    expect_observation(feature.calls[0][1], 1, 5.f, 1.0 / 60.0, 0.05, {});
    succeeds(feature.runtime.pump_completions(), "pump held deferred clock again");
    require(feature.calls[0].size() == 2, "completion pump bypassed the active observation cohort");
    succeeds(consumer.consume(*world.scope), "retire second empty clock cohort");
}

void test_partial_observation_failure_keeps_cohort_and_individual_receipts() {
    WorldFixture world; ObservationFixture feature;
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
             "bind partial observation delivery");
    auto malformed = world.command(7.f, 9.f); malformed.payload.push_back(0xa5);
    feature.outgoing[0] = {world.command(5.f, 7.f), malformed, world.command(7.f, 9.f)};
    succeeds(feature.runtime.dispatch_call(feature.owners[0]), "dispatch real partial observation commands");
    const auto commands = feature.capture();
    require(commands.size() == 3 && !consumer.consume(*world.scope) && !consumer.observation_waiting(),
            "malformed delivery was confused with an invocation still executing");
    world.expect(7.f);
    require(commands[0].receipt().state == Delivery::kApplied && commands[1].receipt().state == Delivery::kPending &&
        commands[2].receipt().state == Delivery::kPending && !feature.runtime.dispatch_call(feature.owners[0]) &&
        feature.calls[0].size() == 1, "partial delivery lost receipts or released the failed cohort");
    require(!consumer.consume(*world.scope), "partial observation retry skipped malformed Pending");
    world.expect(7.f);
    const auto binding = Access::bind(feature.runtime, feature.owners[0], kEffect, 1);
    succeeds(binding, "bind malformed observation receipt authority");
    succeeds(Access::reject(feature.runtime, binding.value, commands[1], ResultCode::kInvalidArgument),
             "explicitly reject malformed observation command");
    succeeds(consumer.consume(*world.scope), "finish remaining observation command without replay");
    world.expect(9.f);
    require(commands[0].receipt().state == Delivery::kApplied && commands[1].receipt().state == Delivery::kRejected &&
        commands[2].receipt().state == Delivery::kApplied, "observation retry replayed or lost individual acknowledgements");
    feature.outgoing[0].clear();
    succeeds(feature.runtime.dispatch_call(feature.owners[0]), "successful retry did not release the observation cohort");
    succeeds(consumer.consume(*world.scope), "retire post-retry empty cohort");
}

void test_scope_preflight_waits_for_every_real_open_observation() {
    concurrency::CompletionSource entered, release;
    WorldFixture world; ObservationFixture feature(ExecutionKind::kCall, 2);
    struct ReleaseOnExit final {
        concurrency::CompletionSource& source;
        ~ReleaseOnExit() { static_cast<void>(source.complete()); }
    } release_on_exit{release};
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
             "bind first observation in actual World cohort");
    succeeds(consumer.bind(feature.owners[1], *world.scope, world.other, kEffect, 1, kObservationPort),
             "bind second observation in actual World cohort");
    feature.outgoing[0] = {world.command(5.f, 7.f)}; feature.outgoing[1] = {world.command(5.f, 11.f)};
    feature.handler = [&](unsigned index, const InvocationToken& token) -> foundation::ResultValue<StateDelta> {
        if (index == 1) {
            static_cast<void>(entered.complete());
            const auto released = release.completion().wait(); if (!released) return {released, {}};
        }
        StateDelta delta{token.state_version}; delta.outbox = feature.outgoing[index];
        return {Result::success(), std::move(delta)};
    };
    succeeds(feature.runtime.enable_async_execution(), "enable actual observation workers");
    succeeds(feature.runtime.dispatch_call(feature.owners[0]), "submit first actual async observation");
    for (unsigned spin = 0; spin != 2000 && feature.runtime.find_instance(feature.owners[0])->state.commit_count == 0; ++spin) {
        succeeds(feature.runtime.pump_completions(), "pump first actual observation completion");
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    const auto first = feature.capture();
    require(first.size() == 1, "first actual observation did not commit its Pending command");
    succeeds(feature.runtime.dispatch_call(feature.owners[1]), "submit second actual async observation");
    succeeds(entered.completion().wait(2'000'000'000), "second actual observation never entered its worker");
    const auto open = feature.runtime.inspect_invocations(feature.owners[1]);
    require(open && open.value.open_attempts.size() == 1 && open.value.open_attempts[0].committable,
            "gated observation has no real committable invocation");
    const auto waiting = consumer.consume(*world.scope);
    require(!waiting && waiting.code == ResultCode::kNotReady && consumer.observation_waiting() &&
        first[0].receipt().state == Delivery::kPending, "scope preflight applied a command before another actual source finished");
    world.expect(5.f);
    succeeds(consumer.set_frame_delta(0.25), "reset actual per-tick observation waiting fact");
    require(!consumer.observation_waiting(), "new frame retained a preceding consume wait fact");
    require(!consumer.consume(*world.scope) && consumer.observation_waiting(), "real open invocation did not restore its wait fact");
    succeeds(release.complete(), "release second actual observation worker");
    for (unsigned spin = 0; spin != 2000 && feature.runtime.pending_execution_count() != 0; ++spin) {
        succeeds(feature.runtime.pump_completions(), "pump remaining actual observation completion");
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    const auto second = feature.capture(1);
    require(feature.runtime.pending_execution_count() == 0 && second.size() == 1,
            "released actual observation did not commit once");
    succeeds(consumer.consume(*world.scope), "consume entire ready actual World cohort");
    require(!consumer.observation_waiting() && first[0].receipt().state == Delivery::kApplied &&
        second[0].receipt().state == Delivery::kApplied && has_value(world.state->get<Transform>(world.entity), 7.f) &&
        has_value(world.state->get<Transform>(world.other), 11.f) && world.state->revision() == 0,
        "ready World cohort failed to apply both owners exactly once before snapshot commit");
}

void test_retry_pins_original_observation_without_reprojection() {
    WorldFixture world; ObservationFixture feature(ExecutionKind::kCall, 1,
        {{kObservationPort, PortDirection::kInput, ValueSchemaId("World.Transform.Observation"), 1}}, true);
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
             "bind retry-owned World observation");
    succeeds(consumer.set_frame_delta(0.125), "set original retry host duration");
    feature.handler = [&](unsigned, const InvocationToken& token) -> foundation::ResultValue<StateDelta> {
        if (feature.calls[0].size() == 1) return {foundation::invalid_state("real retryable observation callback"), {}};
        StateDelta delta{token.state_version}; delta.outbox = {world.command(5.f, 7.f)};
        return {Result::success(), std::move(delta)};
    };
    succeeds(feature.runtime.dispatch_call(feature.owners[0], std::vector<uint8_t>{0xe3}), "issue real retryable observation");
    const auto retry = feature.runtime.inspect_invocations(feature.owners[0]);
    require(retry && retry.value.open_attempts.empty() && retry.value.retry && feature.calls[0].size() == 1,
            "callback failure did not retain a real clock-owned retry");
    const auto waiting = consumer.consume(*world.scope);
    require(!waiting && waiting.code == ResultCode::kNotReady && consumer.observation_waiting(),
            "retry backoff did not pin its actual observed World generation");
    succeeds(consumer.set_frame_delta(0.75), "change frame duration while retry owns original input");
    succeeds(feature.runtime.tick(kObservationClock), "run real clock-owned retry");
    require(feature.calls[0].size() == 2 && feature.calls[0][0].bytes == feature.calls[0][1].bytes &&
        feature.calls[0][0].token.invocation_id != feature.calls[0][1].token.invocation_id &&
        feature.calls[0][0].token.input_version == feature.calls[0][1].token.input_version,
        "retry reprojected host facts or lost its original input/version");
    expect_observation(feature.calls[0][1], 0, 5.f, 0, 0.125, {0xe3});
    succeeds(consumer.consume(*world.scope), "apply command from actual retried observation");
    require(!consumer.observation_waiting(), "completed retry kept a false World wait"); world.expect(7.f);
}

void test_pending_committed_before_observation_binding_blocks_first_projection() {
    WorldFixture world; ObservationFixture feature;
    const auto token = feature.runtime.invoke(feature.owners[0]);
    succeeds(token, "open actual producer invocation before observation binding");
    StateDelta delta{token.value.state_version}; delta.outbox = {world.command(5.f, 7.f)};
    succeeds(feature.runtime.complete(token.value, delta), "commit actual command before installing its observation route");
    const auto pending = feature.capture();
    require(pending.size() == 1 && *pending[0].invocation() == token.value && feature.calls[0].empty(),
            "pre-binding command is not a real canonical pending commit");
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
             "bind observation route after its owner has committed a command");
    const auto held = feature.runtime.dispatch_call(feature.owners[0], std::vector<uint8_t>{0xf1});
    require(!held && held.code == ResultCode::kNotReady && feature.calls[0].empty() &&
        pending[0].receipt().state == Delivery::kPending,
        "fresh observation latch admitted a callback while its actual owner already had Pending commands");
    world.expect(5.f);
    succeeds(consumer.consume(*world.scope), "consume command committed before observation binding");
    require(!consumer.observation_waiting() && pending[0].receipt().state == Delivery::kApplied,
            "pre-binding Pending was mistaken for an executing observation");
    world.expect(7.f);
    succeeds(feature.runtime.dispatch_call(feature.owners[0], std::vector<uint8_t>{0xf2}),
             "drained pre-binding Pending prevented the first real observation");
    require(feature.calls[0].size() == 1, "first real observation after pre-binding Pending was lost or duplicated");
    expect_observation(feature.calls[0][0], 0, 7.f, 0, 0, {0xf2});
    succeeds(consumer.consume(*world.scope), "retire empty first observation after pre-binding command");
}

void test_partial_route_failure_preserves_the_whole_scope_cohort() {
    WorldFixture world; ObservationFixture feature(ExecutionKind::kCall, 2);
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
             "bind first source of partial World cohort");
    succeeds(consumer.bind(feature.owners[1], *world.scope, world.other, kEffect, 1, kObservationPort),
             "bind second source of partial World cohort");
    auto malformed = world.command(5.f, 11.f); malformed.payload.push_back(0xa5);
    feature.outgoing[0] = {world.command(5.f, 7.f)}; feature.outgoing[1] = {malformed};
    succeeds(feature.runtime.dispatch_call(feature.owners[0]), "commit first source of partial World cohort");
    succeeds(feature.runtime.dispatch_call(feature.owners[1]), "commit second source of partial World cohort");
    const auto first = feature.capture(), second = feature.capture(1);
    require(first.size() == 1 && second.size() == 1 && !consumer.consume(*world.scope) &&
        !consumer.observation_waiting() && first[0].receipt().state == Delivery::kApplied &&
        second[0].receipt().state == Delivery::kPending && feature.capture().empty(),
        "partial World cohort did not retain each actual source's delivery state");
    world.expect(7.f);
    const auto held = feature.runtime.dispatch_call(feature.owners[0]);
    require(!held && held.code == ResultCode::kNotReady && feature.calls[0].size() == 1,
            "already Applied source escaped a scope cohort whose later route still failed");
    require(!consumer.consume(*world.scope), "partial scope retry skipped another source's malformed Pending");
    world.expect(7.f);
    const auto binding = Access::bind(feature.runtime, feature.owners[1], kEffect, 1);
    succeeds(binding, "bind second source's canonical rejection authority");
    succeeds(Access::reject(feature.runtime, binding.value, second[0], ResultCode::kInvalidArgument),
             "explicitly reject failing second source command");
    succeeds(consumer.consume(*world.scope), "retire complete scope after explicit rejection");
    feature.outgoing[0] = {world.command(7.f, 9.f)}; feature.outgoing[1].clear();
    succeeds(feature.runtime.dispatch_call(feature.owners[0]), "whole-scope success did not release first source");
    succeeds(feature.runtime.dispatch_call(feature.owners[1]), "whole-scope success did not release second source");
    succeeds(consumer.consume(*world.scope), "apply actual next cohort after partial-scope recovery");
    require(feature.calls[0].size() == 2 && feature.calls[1].size() == 2 &&
        first[0].receipt().state == Delivery::kApplied && second[0].receipt().state == Delivery::kRejected &&
        !consumer.observation_waiting(), "scope recovery replayed a terminal command or retained a stale cohort");
    world.expect(9.f);
}

void test_sample_block_commits_while_actual_world_cohort_is_not_ready() {
    concurrency::CompletionSource world_entered, release_world, audio_entered;
    concurrency::ThreadId world_thread{}, audio_thread{};
    InvocationToken audio_token{};
    WorldFixture world;
    ObservationFixture feature(ExecutionKind::kCall, 2);
    struct ReleaseOnExit final {
        concurrency::CompletionSource& source;
        ~ReleaseOnExit() { static_cast<void>(source.complete()); }
    } release_on_exit{release_world};
    Consumer consumer(feature.runtime, world.schemas);
    succeeds(consumer.bind(feature.owners[0], *world.scope, world.entity, kEffect, 1, kObservationPort),
             "SampleBlock bind first actual World observation");
    succeeds(consumer.bind(feature.owners[1], *world.scope, world.other, kEffect, 1, kObservationPort),
             "SampleBlock bind gated actual World observation");
    feature.outgoing[0] = {world.command(5.f, 7.f)};
    feature.outgoing[1] = {world.command(5.f, 11.f)};
    feature.handler = [&](unsigned index, const InvocationToken& token) -> foundation::ResultValue<StateDelta> {
        if (index == 1) {
            world_thread = concurrency::current_thread_id();
            static_cast<void>(world_entered.complete());
            const auto ready = release_world.completion().wait(2'000'000'000);
            if (!ready) return {ready, {}};
        }
        StateDelta delta{token.state_version}; delta.outbox = feature.outgoing[index];
        return {Result::success(), std::move(delta)};
    };

    constexpr ClockId kSampleBlock{"Fixture.Audio.SampleBlock"};
    const std::vector<uint8_t> samples{0x00, 0x7f, 0x80, 0xff, 0x11, 0x22, 0x33, 0x44};
    FeatureDefinition audio;
    audio.id = FeatureDefinitionId{"Fixture.Audio.Block"}; audio.version = {1, 0, 0}; audio.owner_module = kModule;
    auto& contract = audio.contract;
    contract.definition_id = audio.id; contract.version = audio.version;
    contract.domains = to_mask(FeatureDomain::kAudio); contract.execution = ExecutionKind::kContinuous;
    contract.state = StatePolicy::kInstanceState; contract.scope = {ScopeId{"Audio"}};
    contract.clock = aeg::feature::make_periodic_clock_contract(kSampleBlock, 200, 8, LateDataPolicy::kDrop, aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0); contract.permissions.version = 1;
    audio.execute = [&, samples](const InvocationToken& token, const StateSnapshot&,
        std::span<const uint8_t>, std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
        audio_thread = concurrency::current_thread_id(); audio_token = token;
        static_cast<void>(audio_entered.complete());
        StateDelta delta{token.state_version}; delta.state_payload = samples;
        return {Result::success(), std::move(delta)};
    };
    succeeds(aeg::feature::test::register_clock_test_definition(feature.registry, audio), "register actual Audio SampleBlock Feature");
    const auto expanded = feature.registry.expand(audio.id, {});
    succeeds(expanded, "expand actual Audio SampleBlock Feature");
    const auto audio_owner = feature.runtime.activate(expanded.value, ScopeId{"Audio"});
    succeeds(audio_owner, "activate Audio in the same World-observing Runtime");
    succeeds(feature.runtime.enable_async_execution(), "enable World and Audio actual workers");
    succeeds(feature.runtime.dispatch_call(feature.owners[0]), "submit first real World command");
    for (unsigned spin = 0; spin != 2000 && feature.runtime.find_instance(feature.owners[0])->state.commit_count == 0; ++spin) {
        succeeds(feature.runtime.pump_completions(), "pump first SampleBlock fixture World command");
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    const auto pending = feature.capture();
    require(pending.size() == 1 && pending[0].receipt().state == Delivery::kPending,
            "SampleBlock precondition has no real Pending World command");
    succeeds(feature.runtime.dispatch_call(feature.owners[1]), "submit gated World observation");
    succeeds(world_entered.completion().wait(2'000'000'000), "World worker never entered its actual gate");
    const auto world_wait = consumer.consume(*world.scope);
    require(!world_wait && world_wait.code == ResultCode::kNotReady && consumer.observation_waiting(),
            "SampleBlock precondition did not establish real World Consumer NotReady");
    world.expect(5.f);

    succeeds(feature.runtime.tick(kSampleBlock), "tick SampleBlock while actual World is NotReady");
    succeeds(audio_entered.completion().wait(2'000'000'000), "SampleBlock callback waited for arbitrary World commit");
    for (unsigned spin = 0; spin != 2000 && feature.runtime.find_instance(audio_owner.value)->state.commit_count == 0; ++spin) {
        succeeds(feature.runtime.pump_completions(), "pump Audio without draining the blocked World");
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    const auto audio_state = feature.runtime.find_instance(audio_owner.value);
    const auto audio_inspect = feature.runtime.inspect_invocations(audio_owner.value);
    const auto still_open = feature.runtime.inspect_invocations(feature.owners[1]);
    const auto still_waiting = consumer.consume(*world.scope);
    require(audio_state && audio_state->state.commit_count == 1 && audio_state->state.payload == samples &&
        audio_inspect && audio_inspect.value.last_committed &&
        audio_inspect.value.last_committed->attempt.token == audio_token &&
        audio_thread != concurrency::current_thread_id() && audio_thread != world_thread &&
        !release_world.is_complete() && still_open && still_open.value.open_attempts.size() == 1 &&
        still_open.value.open_attempts[0].committable && !still_waiting &&
        still_waiting.code == ResultCode::kNotReady && consumer.observation_waiting() &&
        pending[0].receipt().state == Delivery::kPending,
        "Audio SampleBlock failed to commit its real bytes independently of the open World cohort");
    world.expect(5.f);

    succeeds(release_world.complete(), "release World after observing independent SampleBlock commit");
    for (unsigned spin = 0; spin != 2000 && feature.runtime.pending_execution_count() != 0; ++spin) {
        succeeds(feature.runtime.pump_completions(), "retire released World work after Audio assertion");
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    succeeds(consumer.consume(*world.scope), "apply actual World cohort after Audio already committed");
    require(feature.runtime.pending_execution_count() == 0 && pending[0].receipt().state == Delivery::kApplied &&
        has_value(world.state->get<Transform>(world.entity), 7.f) &&
        has_value(world.state->get<Transform>(world.other), 11.f),
        "SampleBlock fixture did not recover the actual World cohort after release");
}
}  // namespace

int main() try {
    test_codec_preserves_semantics_and_rejects_noncanonical_records();
    test_multiple_old_commits_apply_in_order_to_the_explicit_owner();
    test_malformed_second_entry_preserves_partial_acknowledgements();
    test_stale_expected_values_and_revision_remain_pending();
    test_route_validation_and_foreign_execution_do_not_mutate_worlds();
    test_world_shutdown_generation_retires_existing_routes();
    test_observation_reads_actual_world_and_holds_even_empty_cohorts();
    test_observation_requires_actual_input_port_schema();
    test_clock_quantum_and_deferred_work_do_not_pin_world_commit();
    test_partial_observation_failure_keeps_cohort_and_individual_receipts();
    test_scope_preflight_waits_for_every_real_open_observation();
    test_retry_pins_original_observation_without_reprojection();
    test_pending_committed_before_observation_binding_blocks_first_projection();
    test_partial_route_failure_preserves_the_whole_scope_cohort();
    try {
        test_sample_block_commits_while_actual_world_cohort_is_not_ready();
        std::cout << "CLOCK_READINESS probe=sample-block-world-not-ready passed=1\n";
    } catch (const std::exception& error) {
        std::cerr << "CLOCK_READINESS probe=sample-block-world-not-ready passed=0 reason=" << error.what() << '\n';
        return 73;
    }
    std::cout << "World typed commands preserve canonical commits, route ownership and individual acknowledgements\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "World Feature commands: " << error.what() << '\n';
    return 73;
}
