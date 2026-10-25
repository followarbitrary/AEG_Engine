/** @brief Verifies owning complete technical definitions and their Stage/Artifact compilation. */
#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../Engine/Pipeline/private/pipeline_compiler_execution.hpp"
#include "pipeline.hpp"

namespace {
using namespace aeg;
using namespace aeg::pipeline;

constexpr PipelineId kPipeline{"ValueValidation.Pipeline"};
constexpr PipelineInstanceId kHost{"ValueValidation.Host"};
constexpr StageId kBefore{"ValueValidation.Before"};
constexpr StageId kAnchor{"ValueValidation.Anchor"};
constexpr StageId kPublisher{"ValueValidation.Publish"};
constexpr StageId kReader{"ValueValidation.Read"};
constexpr ArtifactId kSnapshot{"ValueValidation.Snapshot.Artifact"};
constexpr ArtifactTypeId kSnapshotType{"ValueValidation.Snapshot.Type"};
constexpr uint32_t kSnapshotVersion = 7;

bool check(bool condition, std::string_view message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

foundation::ResultValue<PipelineDefinition> definition() {
    std::string pipeline_name = "Owned technical pipeline";
    PipelineBuilder builder({.id = kPipeline, .name = pipeline_name,
        .supported_scopes = to_mask(PipelineScope::kFrame)});
    const auto before = builder.add_stage({.id = kBefore, .phase = EnginePhase::kPreUpdate});
    const auto anchor = builder.add_stage({.id = kAnchor, .phase = EnginePhase::kPreUpdate});
    const auto publisher = builder.add_stage({.id = kPublisher, .phase = EnginePhase::kPreUpdate});
    const auto reader = builder.add_stage({.id = kReader, .phase = EnginePhase::kPreUpdate});
    if (!before || !anchor || !publisher || !reader ||
        !builder.declare_artifact({.id = kSnapshot, .type = kSnapshotType, .owner_scope = PipelineScope::kFrame,
            .storage = ArtifactStorage::kImmutableSnapshot, .lifetime = ArtifactLifetime::kFrame,
            .ownership = OwnershipPolicy::kSharedImmutable, .version = kSnapshotVersion}))
        return {builder.status(), {}};
    if (const auto result = builder.publish_snapshot(publisher, {kSnapshot, ArtifactAccess::kPublishSnapshot, kSnapshotVersion});
        !result) return {result, {}};
    if (const auto result = builder.reads(reader, {kSnapshot, ArtifactAccess::kRead, kSnapshotVersion, AccessFlags::kPreserveContents});
        !result) return {result, {}};
    if (const auto result = builder.reads(reader, {kSnapshot, ArtifactAccess::kRead, kSnapshotVersion, AccessFlags::kOptional});
        !result) return {result, {}};
    if (const auto result = builder.runs_before(before, kAnchor); !result) return {result, {}};
    if (const auto result = builder.runs_before(publisher, kAnchor); !result) return {result, {}};
    if (const auto result = builder.runs_before(reader, kAnchor); !result) return {result, {}};
    if (const auto result = builder.runs_before(reader, kBefore); !result) return {result, {}};
    if (const auto result = builder.expose_artifact(kSnapshot, PipelineScope::kEditor); !result) return {result, {}};
    pipeline_name.front() = 'X';
    return builder.finish();
}

foundation::ResultValue<PipelinePlan> compile(const PipelineRegistry& registry) {
    return aeg::pipeline::detail::PipelineCompilerExecution::compile({.pipeline = kPipeline, .scope = PipelineScope::kFrame,
        .registry = &registry, .instance = kHost});
}

bool empty_plan(const PipelinePlan& plan) {
    return plan.revision == 0 && plan.nodes.empty() && plan.dependencies.empty() && plan.topological_order.empty() &&
        plan.batches.empty() && plan.artifacts.empty() && plan.exposures.empty() && !plan.definition_owner;
}

size_t node_index(const PipelinePlan& plan, StageId stage) {
    const auto found = std::find_if(plan.nodes.begin(), plan.nodes.end(),
        [stage](const auto& node) { return node.stage == stage; });
    return static_cast<size_t>(found - plan.nodes.begin());
}

bool verify_snapshot_plan(const PipelinePlan& plan) {
    if (!check(plan.revision != 0 && plan.pipeline == kPipeline && plan.instance == kHost &&
        plan.scope == PipelineScope::kFrame && plan.nodes.size() == 4,
        "snapshot definition has an invalid technical plan")) return false;
    const auto publisher = node_index(plan, kPublisher);
    const auto reader = node_index(plan, kReader);
    const auto before = node_index(plan, kBefore);
    const auto anchor = node_index(plan, kAnchor);
    if (!check(publisher < plan.nodes.size() && reader < plan.nodes.size() && before < plan.nodes.size() &&
        anchor < plan.nodes.size(), "snapshot definition lost a declared Stage")) return false;
    if (!check(plan.nodes[publisher].definition_stage_index == 2 && plan.nodes[reader].definition_stage_index == 3 &&
        plan.nodes[before].definition_stage_index == 0 && plan.nodes[anchor].definition_stage_index == 1,
        "Stage records no longer identify the actual registered definition")) return false;
    const auto& writes = plan.nodes[publisher].artifact_bindings;
    const auto& reads = plan.nodes[reader].artifact_bindings;
    if (!check(writes.size() == 1 && writes[0].artifact == kSnapshot &&
        writes[0].access == ArtifactAccess::kPublishSnapshot && writes[0].required_version == kSnapshotVersion &&
        reads.size() == 1 && reads[0].artifact == kSnapshot && reads[0].access == ArtifactAccess::kRead &&
        reads[0].required_version == kSnapshotVersion &&
        reads[0].flags == (AccessFlags::kPreserveContents | AccessFlags::kOptional),
        "publish/read bindings or compatible duplicate flag merging changed")) return false;
    const auto has_edge = [&](size_t source, size_t destination, PipelineDependencyKind kind, ArtifactId artifact) {
        return std::any_of(plan.dependencies.begin(), plan.dependencies.end(), [&](const auto& edge) {
            return edge.source_node == source && edge.destination_node == destination &&
                edge.kind == kind && edge.artifact == artifact && !edge.runtime_override;
        });
    };
    if (!check(has_edge(publisher, reader, PipelineDependencyKind::kArtifact, kSnapshot) &&
        has_edge(reader, before, PipelineDependencyKind::kOrdering, {}) &&
        has_edge(before, anchor, PipelineDependencyKind::kOrdering, {}),
        "actual artifact or explicit Stage ordering dependency is missing")) return false;
    if (!check(plan.topological_order == std::vector<uint32_t>{static_cast<uint32_t>(publisher),
        static_cast<uint32_t>(reader), static_cast<uint32_t>(before), static_cast<uint32_t>(anchor)},
        "snapshot Stage dependency order is wrong")) return false;
    if (!check(plan.artifacts.size() == 1 && plan.artifacts[0].id == kSnapshot &&
        plan.artifacts[0].type == kSnapshotType && plan.artifacts[0].version == kSnapshotVersion &&
        plan.artifacts[0].owner_scope == PipelineScope::kFrame &&
        plan.artifacts[0].storage == ArtifactStorage::kImmutableSnapshot &&
        plan.artifacts[0].ownership == OwnershipPolicy::kSharedImmutable && plan.exposures.size() == 1 &&
        plan.exposures[0].artifact == kSnapshot && plan.exposures[0].target_scope == PipelineScope::kEditor,
        "snapshot contract or Editor exposure is wrong")) return false;
    return check(plan.definition_owner && plan.definition_owner->descriptor.name == "Owned technical pipeline" &&
        plan.definition_owner->stages.size() == 4 && plan.definition_owner->artifacts.size() == 1 &&
        plan.definition_owner->stages[2].descriptor.id == kPublisher &&
        plan.definition_owner->stages[3].descriptor.id == kReader &&
        plan.definition_owner->artifacts.front().descriptor.version == kSnapshotVersion,
        "compiled definition owner aliases caller data");
}

bool positive_snapshot_and_contract() {
    std::weak_ptr<const PipelineDefinition> weak_definition;
    {
        PipelineRegistry registry;
        auto base = definition();
        if (!check(base && registry.register_pipeline(base.value), "positive pipeline registration failed")) return false;
        base.value.descriptor.name.front() = 'X';
        base.value.stages[2].descriptor.id = StageId{"Caller.ChangedStage"};
        ++base.value.artifacts.front().descriptor.version;
        const auto compiled = compile(registry);
        if (!check(bool(compiled), "positive snapshot compile failed") || !verify_snapshot_plan(compiled.value)) return false;
        {
            const auto actual = registry.find_definition(kPipeline);
            if (!check(actual && actual.value == compiled.value.definition_owner,
                "plan retained fabricated definition copies instead of its actual registry owner")) return false;
        }
        weak_definition = compiled.value.definition_owner;
        const auto revision = compiled.value.revision;
        PipelineRegistry denied_registry;
        auto denied_definition = definition();
        if (!check(bool(denied_definition), "snapshot negative definition failed")) return false;
        denied_definition.value.orderings.push_back({1, kPublisher, true});
        if (!check(bool(denied_registry.register_pipeline(std::move(denied_definition.value))),
            "snapshot cycle negative fixture could not register its actual definition")) return false;
        const auto denied = compile(denied_registry);
        if (!check(!denied && denied.result.code == foundation::ResultCode::kInvalidArgument && empty_plan(denied.value),
            "cyclic snapshot dependencies compiled or returned a partial plan")) return false;
        if (!check(compiled.value.revision == revision, "failed compile changed an earlier plan revision") ||
            !verify_snapshot_plan(compiled.value)) return false;
        if (!check(registry.unregister_pipeline(kPipeline) && !registry.find_definition(kPipeline), "registry removal failed"))
            return false;
        if (!check(!weak_definition.expired(), "definition retired while the compiled plan still owned it") ||
            !verify_snapshot_plan(compiled.value)) return false;
    }
    return check(weak_definition.expired(), "technical definition owner leaked after the registry and actual plan retired");
}

bool expect_rejected(PipelineDefinition candidate, std::string_view label) {
    PipelineRegistry registry;
    const PipelineId guard_id{"ValueValidation.Guard"};
    PipelineBuilder builder({.id = guard_id, .name = "Owned guard definition",
        .supported_scopes = to_mask(PipelineScope::kFrame)});
    if (!check(bool(builder.add_stage({.id = StageId{"ValueValidation.GuardStage"}, .phase = EnginePhase::kPreUpdate})),
        "guard Stage declaration failed")) return false;
    auto definition = builder.finish();
    if (!check(definition && registry.register_pipeline(std::move(definition.value)), "guard definition registration failed"))
        return false;
    const auto guard = registry.find_definition(guard_id);
    if (!check(bool(guard), "guard definition lookup failed")) return false;
    const auto id = candidate.descriptor.id;
    const auto result = registry.register_pipeline(std::move(candidate));
    bool passed = check(!result && result.code == foundation::ResultCode::kInvalidArgument, label);
    const auto absent = registry.find_definition(id);
    passed = check(!absent && absent.result.code == foundation::ResultCode::kNotFound,
        "rejected definition was installed in the registry") && passed;
    const auto retained = registry.find_definition(guard_id);
    return check(retained && retained.value == guard.value &&
        retained.value->descriptor.name == "Owned guard definition" && retained.value->stages.size() == 1,
        "failed registration changed the previous successful definition") && passed;
}

PipelineDefinition binding_definition(PipelineId id) {
    PipelineDefinition result{};
    result.descriptor = {.id = id, .name = "Owned binding definition", .supported_scopes = to_mask(PipelineScope::kFrame)};
    result.stages = {{{.id = StageId{"ValueValidation.BindingStage"}, .phase = EnginePhase::kPreUpdate}}};
    result.artifacts = {{{.id = ArtifactId{"ValueValidation.LocalValue"}, .type = ArtifactTypeId{"ValueValidation.ValueType"},
        .owner_scope = PipelineScope::kFrame, .lifetime = ArtifactLifetime::kFrame, .external_input = true}}};
    result.bindings = {{0, {ArtifactId{"ValueValidation.LocalValue"}, ArtifactAccess::kRead, 1}}};
    return result;
}

bool discarded_read_registration() {
    auto discarded_read = binding_definition(PipelineId{"ValueValidation.ReadDiscard"});
    discarded_read.bindings[0].binding.flags = AccessFlags::kDiscardContents;
    return expect_rejected(std::move(discarded_read), "Read|DiscardContents registration was accepted");
}

bool local_binding_conflicts() {
    auto access_conflict = binding_definition(PipelineId{"ValueValidation.AccessConflict"});
    access_conflict.bindings.push_back({0, {ArtifactId{"ValueValidation.LocalValue"}, ArtifactAccess::kWrite, 1}});
    bool passed = expect_rejected(std::move(access_conflict), "conflicting access registration was accepted");
    auto version_conflict = binding_definition(PipelineId{"ValueValidation.VersionConflict"});
    version_conflict.bindings.push_back({0, {ArtifactId{"ValueValidation.LocalValue"}, ArtifactAccess::kRead, 2}});
    passed = expect_rejected(std::move(version_conflict), "conflicting required versions registration was accepted") && passed;
    auto flags_conflict = binding_definition(PipelineId{"ValueValidation.FlagsConflict"});
    flags_conflict.bindings[0].binding.access = ArtifactAccess::kWrite;
    flags_conflict.bindings[0].binding.flags = AccessFlags::kPreserveContents;
    flags_conflict.bindings.push_back({0, {ArtifactId{"ValueValidation.LocalValue"}, ArtifactAccess::kWrite, 1,
        AccessFlags::kDiscardContents}});
    passed = expect_rejected(std::move(flags_conflict), "merged PreserveContents|DiscardContents registration was accepted") && passed;
    auto unknown_flags = binding_definition(PipelineId{"ValueValidation.UnknownFlags"});
    unknown_flags.bindings[0].binding.flags = static_cast<AccessFlags>(1u << 31u);
    passed = expect_rejected(std::move(unknown_flags), "unknown access flag registration was accepted") && passed;
    auto mutable_snapshot = definition();
    if (!check(bool(mutable_snapshot), "mutable snapshot negative definition failed")) return false;
    mutable_snapshot.value.descriptor.id = PipelineId{"ValueValidation.MutableSnapshot"};
    mutable_snapshot.value.artifacts[0].descriptor.storage = ArtifactStorage::kMutableState;
    return expect_rejected(std::move(mutable_snapshot.value), "PublishSnapshot on mutable storage was accepted") && passed;
}

bool stage_identity_collision() {
    PipelineRegistry registry;
    auto base = definition();
    if (!check(base && registry.register_pipeline(base.value), "distinct Stage fixture could not register its actual definition"))
        return false;
    const auto distinct = compile(registry);
    if (!check(distinct && distinct.value.nodes.size() == 4 && distinct.value.definition_owner &&
        distinct.value.nodes[2].stage != distinct.value.nodes[3].stage, "distinct technical Stages did not compile")) return false;
    const auto revision = distinct.value.revision;
    const auto original_owner = distinct.value.definition_owner;
    const auto original_first = distinct.value.nodes[2].stage;
    const auto original_second = distinct.value.nodes[3].stage;
    base.value.stages[3].descriptor.id = base.value.stages[2].descriptor.id;
    const auto colliding = registry.register_pipeline(std::move(base.value));
    bool passed = check(!colliding && colliding.code == foundation::ResultCode::kInvalidArgument,
        "different Stage records sharing one identity were accepted");
    const auto retained = registry.find_definition(kPipeline);
    passed = check(retained && retained.value == original_owner, "failed Stage registration changed the actual registry owner") && passed;
    PipelineRegistry multiple_writers;
    auto malformed = definition();
    if (!check(bool(malformed), "multiple writer negative definition failed")) return false;
    malformed.value.bindings[1].binding.access = ArtifactAccess::kPublishSnapshot;
    malformed.value.bindings[1].binding.flags = AccessFlags::kNone;
    if (!check(bool(multiple_writers.register_pipeline(std::move(malformed.value))),
        "multiple writer fixture could not register its actual definition")) return false;
    const auto rejected = compile(multiple_writers);
    passed = check(!rejected && rejected.result.code == foundation::ResultCode::kInvalidArgument && empty_plan(rejected.value),
        "unordered snapshot producers compiled or returned a partial plan") && passed;
    passed = check(distinct.value.revision == revision && distinct.value.nodes.size() == 4 &&
        distinct.value.nodes[2].stage == original_first && distinct.value.nodes[3].stage == original_second &&
        distinct.value.definition_owner == original_owner,
        "failed registration or compile changed the earlier successful owning plan") && passed;
    return passed && verify_snapshot_plan(distinct.value);
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "all";
    if (argc > 2) return 64;
    if (mode == "discard") return discarded_read_registration() ? 0 : 42;
    if (mode == "collision") return stage_identity_collision() ? 0 : 42;
    if (mode == "local_bindings") return local_binding_conflicts() ? 0 : 42;
    if (mode == "projection") return positive_snapshot_and_contract() ? 0 : 42;
    if (mode != "all") return 64;
    const bool discarded = discarded_read_registration();
    const bool registration = local_binding_conflicts();
    const bool collision = stage_identity_collision();
    const bool snapshot = positive_snapshot_and_contract();
    return discarded && registration && collision && snapshot ? 0 : 42;
}
