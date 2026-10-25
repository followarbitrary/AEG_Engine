/**
 * @file feature_render_projection_smoke.cpp
 * @brief RenderFeature to internal RenderGraph work projection tests.
 */
#include "feature_clock_test_fixture.hpp"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <cstddef>
#include <stdexcept>

#include <feature_graph.hpp>
#include "render_feature_projector.hpp"
#include "render_intent_planner.hpp"

namespace {
using namespace aeg::feature;
using namespace aeg::render_graph;

FeatureDefinition render_definition() {
    FeatureDefinition result{};
    result.id = FeatureDefinitionId("Render.Temporal");
    result.version = {1, 0, 0};
    result.owner_module = ModuleIdentity("Renderer.Smoke");
    result.contract.definition_id = result.id;
    result.contract.version = result.version;
    result.contract.domains = to_mask(FeatureDomain::kRender);
    result.contract.execution = ExecutionKind::kContinuous;
    result.contract.state = StatePolicy::kInstanceState;
    result.contract.scope = ScopeContract{ScopeId("Frame")};
    result.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("Frame"), 60, 1, LateDataPolicy::kUseLatest,
        aeg::feature::DeadlinePolicy::kObserve, result.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    result.contract.permissions.version = 1;
    result.contract.ports = {{PortId("Color"), PortDirection::kInput,
                              ValueSchemaId("Texture.v1"), 1}};
    return result;
}

void test_render_feature_expands_internal_work_with_provenance() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const auto definition = render_definition();
    assert(aeg::feature::test::register_clock_test_definition(registry, definition));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://render";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("TemporalA"), definition.id, definition.version,
                    OwnerPath{{FeatureNodeId("Root")}}, {}},
                   {FeatureNodeId("TemporalB"), definition.id, definition.version,
                    OwnerPath{{FeatureNodeId("Root")}}, {}}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    const auto work = RenderFeatureProjector{}.project(compiled.value, 42);
    assert(work);
    assert(work.value.size() == 4);
    for (size_t index = 0; index < work.value.size(); ++index) {
        const auto& item = work.value[index];
        assert(item.domain == FeatureDomain::kRender);
        assert(item.frame_index == 42);
        assert(item.feature_definition_id == definition.id);
        assert(item.owner_module == definition.owner_module);
        assert(item.node_id.is_valid());
        assert(item.instance_id.is_valid());
        assert(item.id.is_valid());
        assert(item.kind == (index % 2 == 0 ? RenderWorkItemKind::kPrepare
                                            : RenderWorkItemKind::kExecute));
        if (index > 0) {
            assert(item.id != work.value[index - 1].id);
        }
    }

    const auto repeated = RenderFeatureProjector{}.project(compiled.value, 42);
    assert(repeated);
    assert(repeated.value.size() == work.value.size());
    for (size_t index = 0; index < work.value.size(); ++index) {
        assert(repeated.value[index].id == work.value[index].id);
    }

    const auto next_frame = RenderFeatureProjector{}.project(compiled.value, 43);
    assert(next_frame);
    assert(next_frame.value.front().id != work.value.front().id);
}

void test_cross_domain_composite_keeps_render_projection_bit() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    auto child = render_definition();
    child.id = FeatureDefinitionId("Render.Child");
    child.contract.definition_id = child.id;
    assert(aeg::feature::test::register_clock_test_definition(registry, child));
    auto logic_child = child;
    logic_child.id = FeatureDefinitionId("Logic.Child");
    logic_child.contract.definition_id = logic_child.id;
    logic_child.contract.domains = to_mask(FeatureDomain::kLogic);
    logic_child.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("LogicTick"), 60, 1, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, logic_child.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    assert(aeg::feature::test::register_clock_test_definition(registry, logic_child));
    auto composite = child;
    composite.id = FeatureDefinitionId("Gameplay.Presentation");
    composite.contract.definition_id = composite.id;
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.domains = to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kRender);
    composite.contract.execution = ExecutionKind::kCall;
    composite.contract.state = StatePolicy::kStateless;
    composite.contract.has_continuation = false;
    composite.contract.children = {child.id, logic_child.id};
    assert(aeg::feature::test::register_clock_test_definition(registry, composite));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://composite-render";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Presentation"), composite.id, composite.version, {}, {}}};
    const auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    assert(compiled);
    assert((compiled.value.provenance.front().domains & to_mask(FeatureDomain::kRender)) != 0);
    assert(RenderFeatureProjector{}.project(compiled.value, 1));
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

RenderFeatureOutput flow_output(uint64_t instance, bool producer, uint64_t local_id = 7) {
    auto definition = render_definition();
    definition.id = FeatureDefinitionId(1000 + instance);
    definition.contract.definition_id = definition.id;
    definition.contract.permissions.capability_mask = kAllRenderIntentCapabilities;
    definition.contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    definition.contract.ports = {{PortId("List"), producer ? PortDirection::kOutput : PortDirection::kInput,
                                  kRenderLightListSchema, 1}};
    RenderFeatureOutput output{};
    output.contract = definition.contract;
    output.owner_module = definition.owner_module;
    output.node_id = FeatureNodeId(instance);
    output.instance_id = FeatureInstanceId(instance);
    output.invocation = {InvocationId(100 + instance), AttemptId(50 + instance), RuntimeGeneration(2),
                         4, {}, StateVersion(5)};
    output.resource_owner = {output.instance_id, output.node_id, feature_contract_digest(definition), 3};
    if (producer) {
        RenderLightCullingScene cull{};
        cull.scene = {RenderResourceId("Scene"), 1};
        cull.camera_depth = {RenderResourceId("Depth"), 1, true};
        cull.output = {RenderResourceId(local_id), 1, PortId("List")};
        output.intent.operations = {cull};
    }
    return output;
}

void connect_light_list(RenderFeatureOutput& consumer, const RenderFeatureOutput& producer) {
    const auto& cull = std::get<RenderLightCullingScene>(producer.intent.operations.front());
    const RenderLightListReference reference{cull.output.id, cull.output.version,
        producer.invocation.invocation_id, producer.invocation.generation};
    const auto payload = encode_render_light_list_reference(reference);
    require(bool(payload), "light-list reference encode failed");
    PortMessage message{};
    message.source_port = cull.output.export_port;
    message.target_port = PortId("List");
    message.schema = kRenderLightListSchema;
    message.schema_version = 1;
    message.kind = EdgeKind::kData;
    message.delivery_id = MessageDeliveryId(300 + consumer.instance_id.value());
    message.source = {producer.instance_id, producer.invocation, producer.resource_owner.definition_digest,
                      producer.resource_owner.resource_epoch, producer.contract.clock.id};
    message.payload = payload.value;
    consumer.input_messages = {message};
    RenderDrawClusteredScene draw{};
    draw.scene = cull.scene;
    draw.light_list = reference;
    draw.hdr.target = {RenderResourceId(10000 + consumer.instance_id.value()), 1, true};
    draw.depth = {cull.camera_depth, RenderAttachmentLoad::kLoad, 1.f};
    consumer.intent.operations = {draw};
}

void test_light_list_flow_authentication_and_order() {
    auto producer_a = flow_output(1, true);
    auto producer_b = flow_output(2, true); // same local id, distinct owners
    auto consumer_a = flow_output(3, false);
    auto consumer_b = flow_output(4, false);
    auto consumer_c = flow_output(5, false);
    connect_light_list(consumer_a, producer_a);
    connect_light_list(consumer_b, producer_b);
    connect_light_list(consumer_c, producer_a); // fan-out shares producer A
    const std::vector outputs{consumer_c, consumer_b, producer_b, consumer_a, producer_a};
    const auto plan = RenderFeatureProjector{}.plan(outputs);
    require(bool(plan), "reverse-order independent light-list plan rejected");
    require(plan.value.light_list_uses.size() == 3, "fan-out light-list bindings missing");
    const auto position = [&](size_t output) {
        return std::find(plan.value.output_order.begin(), plan.value.output_order.end(), output) - plan.value.output_order.begin();
    };
    require(position(4) < position(0) && position(4) < position(3) && position(2) < position(1),
            "consumer scheduled before actual producer");
    for (const auto& use : plan.value.light_list_uses)
        require(use.producer_output == (use.consumer_output == 1 ? 2 : 4) &&
                use.producer_operation == 0 && use.consumer_operation == 0, "same-name producer binding aliased");
    auto pending = outputs;
    pending[4].invocation.invocation_id = InvocationId(999);
    const auto waiting = RenderIntentPlanner{}.plan(pending, outputs);
    require(!waiting && waiting.result.code == aeg::foundation::ResultCode::kNotReady && waiting.value.output_order.empty(),
            "previously accepted producer was emitted as a mixed execution plan");
    require(!RenderFeatureProjector{}.plan(pending), "physical lowering accepted a partial publication");
    for (unsigned invalid = 0; invalid != 5; ++invalid) {
        auto malformed_pending = pending;
        switch (invalid) {
            case 0: std::get<RenderDrawClusteredScene>(malformed_pending[0].intent.operations.front()).scene.version = 2; break;
            case 1: ++malformed_pending[0].input_messages.front().source.resource_epoch; break;
            case 2: ++malformed_pending[4].resource_owner.resource_epoch; break;
            case 3: malformed_pending[0].input_messages.front().payload.push_back(0); break;
            case 4: malformed_pending[0].input_messages.front().source.invocation.attempt_id = AttemptId(999); break;
        }
        const auto rejected = RenderIntentPlanner{}.plan(malformed_pending, outputs);
        require(!rejected && rejected.result.code != aeg::foundation::ResultCode::kNotReady,
                "invalid publication was masked as normal readiness");
    }
    // Invocation numbers can coincide in independent runtime hosts. The
    // consumed source receipt, rather than the numeric reference, selects
    // the actual owner even when all public reference fields coincide.
    auto colliding = outputs;
    colliding[2].invocation = colliding[4].invocation;
    connect_light_list(colliding[1], colliding[2]);
    const auto collision_plan = RenderFeatureProjector{}.plan(colliding);
    require(collision_plan && collision_plan.value.light_list_uses.size() == 3,
            "owner-qualified colliding reference numbers were rejected");
    for (const auto& use : collision_plan.value.light_list_uses)
        require(use.producer_output == (use.consumer_output == 1 ? 2 : 4),
                "colliding numeric references selected a foreign owner");
    for (unsigned invalid = 0; invalid != 39; ++invalid) {
        auto rejected = outputs;
        auto& consumer = rejected[0];
        auto& message = consumer.input_messages.front();
        auto& reference = std::get<RenderDrawClusteredScene>(consumer.intent.operations.front()).light_list;
        switch (invalid) {
            case 0: consumer.input_messages.clear(); break; // explicit bytes have no authorization
            case 1: message.source.instance = producer_b.instance_id; break;
            case 2: message.source.invocation.attempt_id = AttemptId(99); break;
            case 3: ++message.source.invocation.input_version; break;
            case 4: message.source.invocation.state_version = StateVersion(99); break;
            case 5: ++message.source.definition_digest; break;
            case 6: ++message.source.resource_epoch; break;
            case 7: message.source.clock = ClockId("Other"); break;
            case 8: message.kind = EdgeKind::kSnapshot; break;
            case 9: message.schema = ValueSchemaId("Forged"); break;
            case 10: message.schema_version = 2; break;
            case 11: message.source_port = PortId("Other"); break;
            case 12: message.target_port = PortId("Other"); break;
            case 13: message.delivery_id = {}; break;
            case 14: reference.version = 2; break;
            case 15: reference.producer_invocation = producer_b.invocation.invocation_id; break;
            case 16: reference.producer_generation = RuntimeGeneration(99); break;
            case 17: message.payload.push_back(0); break;
            case 18: message.payload[0] ^= 1; break;
            case 19: rejected[4].intent.operations.push_back(rejected[4].intent.operations.front()); break;
            case 20: rejected[4].resource_owner.instance_id = FeatureInstanceId(99); break;
            case 21: rejected[4].intent.operations.clear(); break;
            case 22: message.source.invocation.invocation_id = InvocationId(99); break;
            case 23: message.source.invocation.generation = RuntimeGeneration(99); break;
            case 24: rejected.pop_back(); break;
            case 25: rejected.push_back(rejected[4]); break;
            case 26: std::get<RenderDrawClusteredScene>(consumer.intent.operations.front()).scene.version = 2; break;
            case 27: std::get<RenderDrawClusteredScene>(consumer.intent.operations.front()).depth.target.version = 2; break;
            case 28: std::get<RenderDrawClusteredScene>(consumer.intent.operations.front()).depth.load = RenderAttachmentLoad::kClear; break;
            case 29: std::get<RenderDrawClusteredScene>(consumer.intent.operations.front()).directional_shadow =
                         RenderTextureReference{RenderResourceId("Shadow"), 1, true}; break;
            case 30: std::get<RenderLightCullingScene>(rejected[4].intent.operations.front()).directional_shadow =
                         RenderTextureReference{RenderResourceId("Shadow"), 1, true}; break;
            case 31: std::get<RenderLightCullingScene>(rejected[4].intent.operations.front()).output.export_port = {}; break;
            case 32: rejected[4].resource_owner.node_id = FeatureNodeId(99); break;
            case 33: rejected[4].resource_owner.definition_digest = 0; break;
            case 34: rejected[4].resource_owner.resource_epoch = 0; break;
            case 35: rejected[4].intent.operations.push_back(rejected[4].intent.operations.front());
                     std::get<RenderLightCullingScene>(rejected[4].intent.operations.back()).output.version = 2; break;
            case 36: consumer.contract.ports.front().direction = PortDirection::kOutput; break;
            case 37: rejected[4].contract.ports.front().version = 2; break;
            case 38: message.source.invocation.process_revision = ProcessRevision(99); break;
        }
        require(!RenderFeatureProjector{}.plan(rejected), "invalid light-list flow accepted");
    }
    auto cyclic = outputs;
    PortMessage reverse_edge = cyclic[0].input_messages.front();
    reverse_edge.source.instance = cyclic[0].instance_id;
    reverse_edge.source.clock = cyclic[4].contract.clock.id;
    cyclic[4].input_messages = {reverse_edge};
    require(!RenderFeatureProjector{}.plan(cyclic), "cycle through a non-light DataEdge was accepted");
    auto stable = std::array{flow_output(10, false), flow_output(11, false), flow_output(12, false)};
    const auto unrelated = RenderFeatureProjector{}.plan(stable);
    require(unrelated && unrelated.value.output_order == std::vector<size_t>{0, 1, 2},
            "unrelated outputs changed their stable order");
    auto private_producer = producer_a;
    auto private_cull = std::get<RenderLightCullingScene>(private_producer.intent.operations.front());
    private_cull.output.export_port = {};
    private_producer.intent.operations.front() = private_cull;
    auto local_draw = std::get<RenderDrawClusteredScene>(consumer_a.intent.operations.front());
    private_producer.intent.operations.push_back(local_draw);
    require(bool(RenderFeatureProjector{}.plan(std::array{private_producer})), "same-owner private bundle use rejected");
    std::swap(private_producer.intent.operations[0], private_producer.intent.operations[1]);
    require(!RenderFeatureProjector{}.plan(std::array{private_producer}), "same-owner forward reference accepted");

    // Canonical schema bytes reject malformed/trailing data rather than native struct padding.
    const auto encoded = encode_render_light_list_reference(local_draw.light_list);
    require(encoded && encoded.value.size() == 32, "light-list wire format is not canonical");
    require(decode_render_light_list_reference(encoded.value).value == local_draw.light_list, "light-list decode mismatch");
    auto malformed = encoded.value;
    malformed.pop_back();
    require(!decode_render_light_list_reference(malformed), "truncated light-list value accepted");
    malformed = encoded.value;
    malformed.push_back(0);
    require(!decode_render_light_list_reference(malformed), "trailing light-list value accepted");
}
}  // namespace

int main() {
    test_render_feature_expands_internal_work_with_provenance();
    test_cross_domain_composite_keeps_render_projection_bit();
    try { test_light_list_flow_authentication_and_order(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "feature render projection smoke passed\n";
    return 0;
}
