#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "../Engine/Pipeline/private/pipeline_compiler_execution.hpp"
#include "feature_logic_test_fixture.hpp"
#include "pipeline.hpp"

namespace {
using namespace aeg;
using namespace aeg::pipeline;

const PipelineDescriptor kDescriptor{.id = PipelineId("Fixture.Pipeline"),
                                     .name = "Fixture Pipeline",
                                     .supported_scopes = to_mask(PipelineScope::kFrame)};

foundation::Result declare_fixture(PipelineBuilder& builder) {
    const auto first = builder.add_stage(StageDescriptor{.id = StageId("Fixture.Begin")});
    const auto second = builder.add_stage(StageDescriptor{.id = StageId("Fixture.End")});
    const auto artifact = builder.declare_artifact(ArtifactDescriptor{.id = ArtifactId("Fixture.Artifact"),
                                                                      .type = ArtifactTypeId("Fixture.Value"),
                                                                      .owner_scope = PipelineScope::kFrame,
                                                                      .visible_to = to_mask(PipelineScope::kFrame)});
    const auto marker = builder.add_stage(StageDescriptor{.id = StageId("Fixture.MarkerStage")});
    if (!first || !second || !artifact || !marker)
        return builder.status();
    if (const auto result = builder.writes(
            first, ArtifactBinding{ArtifactId("Fixture.Artifact"), ArtifactAccess::kWrite, 1, AccessFlags::kNone});
        !result)
        return result;
    if (const auto result = builder.reads(
            second, ArtifactBinding{ArtifactId("Fixture.Artifact"), ArtifactAccess::kRead, 1, AccessFlags::kNone});
        !result)
        return result;
    if (const auto result = builder.runs_before(first, StageId("Fixture.End")); !result)
        return result;
    if (const auto result = builder.runs_after(second, StageId("Fixture.Begin")); !result)
        return result;
    if (const auto result = builder.expose_artifact(ArtifactId("Fixture.Artifact"), PipelineScope::kEditor); !result)
        return result;
    return builder.runs_before(marker, StageId("Fixture.Begin"));
}

foundation::ResultValue<PipelineDefinition> fixture_definition(bool require_capability = false) {
    // All declaration names must remain owned after this local source dies.
    auto descriptor = kDescriptor;
    descriptor.required_capabilities = require_capability ? 1 : 0;
    const std::string name = "Fixture Pipeline";
    descriptor.name = name;
    PipelineBuilder builder(descriptor);
    if (const auto result = declare_fixture(builder); !result)
        return {result, {}};
    return builder.finish();
}

bool require(bool value, const char* expression, int line) {
    if (!value)
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
    return value;
}
}  // namespace

#define CHECK(value)                                              \
    do {                                                          \
        if (!require(static_cast<bool>(value), #value, __LINE__)) \
            return __LINE__;                                      \
    } while (false)

int main() {
    PipelineBuilder builder(kDescriptor);
    CHECK(declare_fixture(builder));
    const auto definition = builder.finish();
    CHECK(definition);
    CHECK(!builder.finish());
    CHECK(definition.value.stages.size() == 3);
    CHECK(definition.value.stages[0].descriptor.id == StageId("Fixture.Begin"));
    CHECK(definition.value.stages[1].descriptor.id == StageId("Fixture.End"));
    CHECK(definition.value.stages[2].descriptor.id == StageId("Fixture.MarkerStage"));
    CHECK(definition.value.artifacts.size() == 1);
    CHECK(definition.value.artifacts[0].descriptor.id == ArtifactId("Fixture.Artifact"));
    CHECK(definition.value.artifacts[0].descriptor.owner_scope == PipelineScope::kFrame);
    CHECK(definition.value.bindings.size() == 2);
    CHECK(definition.value.bindings[0].stage_index == 0 &&
          definition.value.bindings[0].binding.access == ArtifactAccess::kWrite);
    CHECK(definition.value.bindings[1].stage_index == 1 &&
          definition.value.bindings[1].binding.access == ArtifactAccess::kRead);
    for (const auto& binding : definition.value.bindings)
        CHECK(binding.binding.artifact == ArtifactId("Fixture.Artifact") && binding.binding.required_version == 1);
    CHECK(definition.value.orderings.size() == 3);
    CHECK(definition.value.orderings[0].source_stage_index == 0 &&
          definition.value.orderings[0].target_stage == StageId("Fixture.End") &&
          definition.value.orderings[0].source_runs_before);
    CHECK(definition.value.orderings[1].source_stage_index == 1 &&
          definition.value.orderings[1].target_stage == StageId("Fixture.Begin") &&
          !definition.value.orderings[1].source_runs_before);
    CHECK(definition.value.orderings[2].source_stage_index == 2 &&
          definition.value.orderings[2].target_stage == StageId("Fixture.Begin") &&
          definition.value.orderings[2].source_runs_before);
    CHECK(definition.value.exposures.size() == 1 &&
          definition.value.exposures[0].artifact == ArtifactId("Fixture.Artifact") &&
          definition.value.exposures[0].target_scope == PipelineScope::kEditor);

    PipelineRegistry registry;
    auto registered_definition = fixture_definition();
    CHECK(registered_definition);
    CHECK(registry.register_pipeline(std::move(registered_definition.value)));
    {
        const auto actual = registry.find_definition(kDescriptor.id);
        CHECK(actual && actual.value->descriptor.name == "Fixture Pipeline" && actual.value->stages.size() == 3 &&
              actual.value->stages[2].descriptor.id == StageId("Fixture.MarkerStage"));
    }
    const PipelineCompileInput input{.pipeline = kDescriptor.id,
                                      .scope = PipelineScope::kFrame,
                                      .registry = &registry,
                                      .instance = PipelineInstanceId("Fixture.View")};
    auto compiled = aeg::pipeline::detail::PipelineCompilerExecution::compile(input);
    CHECK(compiled);
    CHECK(compiled.value.instance == input.instance && compiled.value.scope == input.scope &&
          compiled.value.pipeline == input.pipeline);
    CHECK(compiled.value.nodes.size() == 3);
    // The compiler orders nodes by StageId; this field preserves each declaration's original index.
    for (size_t declared_index = 0; declared_index < definition.value.stages.size(); ++declared_index) {
        const auto stage_id = definition.value.stages[declared_index].descriptor.id;
        const auto node = std::find_if(compiled.value.nodes.begin(), compiled.value.nodes.end(),
            [stage_id](const auto& candidate) { return candidate.stage == stage_id; });
        CHECK(node != compiled.value.nodes.end());
        CHECK(node->definition_stage_index == declared_index);
    }
    {
        const auto actual = registry.find_definition(kDescriptor.id);
        CHECK(actual && actual.value == compiled.value.definition_owner);
    }
    CHECK(compiled.value.artifacts.size() == 1 && compiled.value.exposures.size() == 1);
    CHECK(compiled.value.topological_order.size() == 3);
    CHECK(compiled.value.nodes[compiled.value.topological_order[0]].stage == StageId("Fixture.MarkerStage"));
    CHECK(compiled.value.nodes[compiled.value.topological_order[1]].stage == StageId("Fixture.Begin"));
    CHECK(compiled.value.nodes[compiled.value.topological_order[2]].stage == StageId("Fixture.End"));

    auto mismatched_input = input;
    mismatched_input.scope = PipelineScope::kEngine;
    const auto mismatched = aeg::pipeline::detail::PipelineCompilerExecution::compile(mismatched_input);
    CHECK(!mismatched && mismatched.result.code == foundation::ResultCode::kUnsupported);
    CHECK(mismatched.value.revision == 0 && mismatched.value.nodes.empty() && mismatched.value.dependencies.empty() &&
          mismatched.value.topological_order.empty() && mismatched.value.batches.empty() &&
          mismatched.value.artifacts.empty() && mismatched.value.exposures.empty() && !mismatched.value.definition_owner);

    PipelineRegistry forbidden_registry;
    auto forbidden_definition = fixture_definition(true);
    CHECK(forbidden_definition && forbidden_registry.register_pipeline(std::move(forbidden_definition.value)));
    auto forbidden_input = input;
    forbidden_input.registry = &forbidden_registry;
    const auto forbidden = aeg::pipeline::detail::PipelineCompilerExecution::compile(forbidden_input);
    CHECK(!forbidden && forbidden.result.code == foundation::ResultCode::kUnsupported);
    CHECK(forbidden.value.revision == 0 && forbidden.value.nodes.empty() && forbidden.value.dependencies.empty() &&
          forbidden.value.topological_order.empty() && forbidden.value.batches.empty() &&
          forbidden.value.artifacts.empty() && forbidden.value.exposures.empty() && !forbidden.value.definition_owner);
    CHECK(compiled.value.nodes.size() == 3 && compiled.value.artifacts.size() == 1);

    // Technical definition values/capability tests above do not claim to execute
    // business. The module's business identity uses the actual modern contract.
    namespace logic = aeg::tests::logic;
    std::vector<logic::CallbackObservation> callbacks;
    feature::FeatureRegistry features{aeg::feature::test::clock_test_host()};
    const auto authored_value = logic::definition("Fixture.Module.AuthoredValue", {.value = 17}, callbacks);
    CHECK(authored_value.owner_module == logic::kModule);
    CHECK(aeg::feature::test::register_clock_test_definition(features, authored_value));
    auto document = logic::graph("fixture://module-business", features);
    document.nodes = {{feature::FeatureNodeId("OwnedValue"), authored_value.id, authored_value.version},
        {feature::FeatureNodeId("OtherOwner"), authored_value.id, authored_value.version}};
    feature::FeatureRuntime runtime(features);
    const auto published = logic::publish(runtime, features, document);
    CHECK(published);
    const auto* owner = logic::owner(published.value, feature::FeatureNodeId("OwnedValue"));
    const auto* other = logic::owner(published.value, feature::FeatureNodeId("OtherOwner"));
    CHECK(owner && other && owner->instance_id != other->instance_id && owner->owner_module == authored_value.owner_module);
    CHECK(!runtime.dispatch_call(owner->instance_id, logic::encode(-999)));
    const auto schema_failure = runtime.inspect_invocations(owner->instance_id);
    CHECK(schema_failure && schema_failure.value.last_failed && !schema_failure.value.last_committed &&
        schema_failure.value.last_failed->stage == feature::InvocationFailureStage::kCommit &&
        schema_failure.value.last_failed->attempt.token == callbacks.back().token);
    CHECK(runtime.dispatch_call(owner->instance_id));
    const auto token = logic::committed_token(runtime, owner->instance_id);
    CHECK(token && token.value == callbacks.back().token);
    logic::ScalarProjection projection(*owner);
    const auto scalar = projection.read(features, runtime, published.value, owner->instance_id, token.value);
    CHECK(scalar && scalar.value == 17 && runtime.find_instance(owner->instance_id)->state.commit_count == 1);
    CHECK(runtime.dispatch_call(other->instance_id));
    const auto other_token = logic::committed_token(runtime, other->instance_id);
    CHECK(other_token && !projection.read(features, runtime, published.value, other->instance_id, other_token.value) &&
        !projection.read(features, runtime, published.value, owner->instance_id, other_token.value));
    const auto state_before_replay = runtime.find_instance(owner->instance_id)->state;
    projection.applied(token.value);
    CHECK(!projection.read(features, runtime, published.value, owner->instance_id, token.value));
    CHECK(runtime.find_instance(owner->instance_id)->state.version == state_before_replay.version &&
        runtime.find_instance(owner->instance_id)->state.payload == state_before_replay.payload);
    CHECK(runtime.shutdown_execution());

    // The real compiled native owners survive unregister, then retire when the
    // last owning plan is released. No task callback is needed for this lease.
    const std::weak_ptr<const PipelineDefinition> definition_life = compiled.value.definition_owner;
    CHECK(registry.unregister_pipeline(kDescriptor.id));
    CHECK(!definition_life.expired());
    compiled.value = {};
    CHECK(definition_life.expired());

    std::cout << "Internal module declaration smoke passed\n";
    return 0;
}
