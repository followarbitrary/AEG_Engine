#include <iostream>
#include <vector>

#include "pipeline.hpp"
#include "pipeline_builder_backend.hpp"
#include "pipeline_configuration.hpp"

namespace {
using namespace aeg;
using namespace aeg::pipeline;

foundation::Result execute_task(PipelineTaskContext&) {
    return foundation::Result::success();
}

class RecordingDeclarationBackend final : public FeatureDeclarationBackend {
public:
    PipelineStageHandle add_stage(const StageDescriptor&) override {
        ++stages;
        return PipelineStageHandle{stages - 1, 1};
    }
    PipelineTaskHandle add_task(const TaskDescriptor&) override {
        ++tasks;
        return PipelineTaskHandle{tasks - 1, 1};
    }
    PipelineArtifactHandle declare_artifact(const ArtifactDescriptor&) override {
        ++artifacts;
        return PipelineArtifactHandle{artifacts - 1, 1};
    }
    PipelineFeaturePointHandle add_feature_point(const FeaturePointDescriptor&) override {
        ++feature_points;
        return PipelineFeaturePointHandle{feature_points - 1, 1};
    }
    foundation::Result bind_feature(const FeatureBindingDescriptor&) override {
        ++feature_bindings;
        return foundation::Result::success();
    }
    foundation::Result bind_task(PipelineTaskHandle, PipelineStageHandle) override {
        ++task_bindings;
        return foundation::Result::success();
    }
    foundation::Result reads(PipelineStageHandle, const ArtifactBinding&) override {
        ++reads_count;
        return foundation::Result::success();
    }
    foundation::Result writes(PipelineStageHandle, const ArtifactBinding&) override {
        ++writes_count;
        return foundation::Result::success();
    }
    foundation::Result reads_writes(PipelineStageHandle, const ArtifactBinding&) override {
        ++read_writes_count;
        return foundation::Result::success();
    }
    foundation::Result publish_snapshot(PipelineStageHandle, const ArtifactBinding&) override {
        ++snapshots;
        return foundation::Result::success();
    }
    foundation::Result runs_before(PipelineStageHandle, StageId other) override {
        before = other;
        return foundation::Result::success();
    }
    foundation::Result runs_after(PipelineStageHandle, StageId other) override {
        after = other;
        return foundation::Result::success();
    }
    foundation::Result expose_artifact(ArtifactId, PipelineScope scope) override {
        exposed_scope = scope;
        return foundation::Result::success();
    }
    foundation::Result status() const noexcept override { return result; }
    const FeatureBuildDescriptor& feature_descriptor() const noexcept override { return feature_context; }
    uint32_t stage_count() const noexcept override { return stages; }
    foundation::Result reject(foundation::ResultCode code, std::string message) override {
        result = foundation::Result::failure(code, std::move(message));
        return result;
    }
    void on_stage_added(PipelineStageHandle, const FeaturePointDescriptor&) override { ++feature_stage_callbacks; }
    void record_contribution(FeatureDomain, uint32_t, uint32_t count) override { contribution_stage_count = count; }

    uint32_t stages{};
    uint32_t tasks{};
    uint32_t artifacts{};
    uint32_t feature_points{};
    uint32_t feature_bindings{};
    uint32_t task_bindings{};
    uint32_t reads_count{};
    uint32_t writes_count{};
    uint32_t read_writes_count{};
    uint32_t snapshots{};
    StageId before{};
    StageId after{};
    PipelineScope exposed_scope{PipelineScope::kEngine};
    foundation::Result result{foundation::Result::success()};
    FeatureBuildDescriptor feature_context{};
    uint32_t feature_stage_callbacks{};
    uint32_t contribution_stage_count{};
};

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
    using namespace aeg::pipeline;

    const PipelineDescriptor descriptor{.id = PipelineId("Fixture.Pipeline"),
                                        .name = "Fixture Pipeline",
                                        .supported_scopes = to_mask(PipelineScope::kFrame),
                                        .domains = to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kRender)};
    RecordingDeclarationBackend backend;
    PipelineBuilder builder(backend, descriptor);

    const auto first = builder.add_stage(StageDescriptor{.id = StageId("Fixture.Begin")});
    const auto second = builder.add_stage(StageDescriptor{.id = StageId("Fixture.End")});
    const auto task = builder.add_task(TaskDescriptor{.id = TaskId("Fixture.Task"), .execute = &execute_task});
    const auto artifact = builder.declare_artifact(ArtifactDescriptor{.id = ArtifactId("Fixture.Artifact"),
                                                                      .type = ArtifactTypeId("Fixture.Value"),
                                                                      .owner_scope = PipelineScope::kFrame,
                                                                      .visible_to = to_mask(PipelineScope::kFrame)});
    const auto point =
        builder.add_feature_point(FeaturePointDescriptor{.id = FeaturePointId("Fixture.Point"),
                                                         .name = "Fixture Point",
                                                         .pipeline = descriptor.id,
                                                         .anchor_stage = StageId("Fixture.Begin"),
                                                         .allowed_domains = to_mask(FeatureDomain::kLogic),
                                                         .allowed_scopes = to_mask(PipelineScope::kFrame)});
    CHECK(first && second && task && artifact && point);
    CHECK(builder.bind_task(task, first));
    CHECK(builder.reads(first,
                        ArtifactBinding{ArtifactId("Fixture.Artifact"), ArtifactAccess::kRead, 1, AccessFlags::kNone}));
    CHECK(builder.writes(
        second, ArtifactBinding{ArtifactId("Fixture.Artifact"), ArtifactAccess::kWrite, 1, AccessFlags::kNone}));
    CHECK(builder.runs_before(first, StageId("Fixture.End")));
    CHECK(builder.runs_after(second, StageId("Fixture.Begin")));
    CHECK(builder.expose_artifact(ArtifactId("Fixture.Artifact"), PipelineScope::kWorld));
    CHECK(backend.stages == 2 && backend.tasks == 1 && backend.artifacts == 1 && backend.feature_points == 1);
    CHECK(backend.task_bindings == 1 && backend.reads_count == 1 && backend.writes_count == 1);
    CHECK(backend.before == StageId("Fixture.End") && backend.after == StageId("Fixture.Begin"));
    CHECK(backend.exposed_scope == PipelineScope::kWorld);
    CHECK(!builder.finish());

    backend.result = foundation::Result::success();
    backend.feature_context.scope = PipelineScope::kFrame;
    backend.feature_context.pipeline = descriptor.id;
    backend.feature_context.feature = FeatureId("Fixture.Feature");
    backend.feature_context.instance = FeatureInstanceId("Fixture.Feature.Instance");
    backend.feature_context.point = FeaturePointDescriptor{.id = FeaturePointId("Fixture.Point"),
                                                           .name = "Fixture Point",
                                                           .pipeline = descriptor.id,
                                                           .anchor_stage = StageId("Fixture.Begin"),
                                                           .allowed_domains = to_mask(FeatureDomain::kLogic),
                                                           .allowed_scopes = to_mask(PipelineScope::kFrame),
                                                           .flags = FeaturePointFlags::kNone};
    backend.feature_context.domain = FeatureDomain::kLogic;
    backend.feature_context.configuration = std::make_shared<const PipelineInstanceConfiguration>(
        PipelineInstanceConfiguration{.pipeline = descriptor.id, .revision = 7, .source = "module-authoring"});
    auto feature_context = FeatureBuildContext::from_backend(backend, backend.feature_context);
    CHECK(feature_context.configuration() == backend.feature_context.configuration);
    auto logic = feature_context.logic_builder();
    CHECK(logic.add_stage(StageDescriptor{.id = StageId("Fixture.FeatureStage")}));
    CHECK(backend.feature_stage_callbacks == 1);
    CHECK(!logic.declare_artifact(ArtifactDescriptor{.id = ArtifactId("Fixture.Forbidden"),
                                                     .type = ArtifactTypeId("Fixture.Value"),
                                                     .owner_scope = PipelineScope::kFrame,
                                                     .visible_to = to_mask(PipelineScope::kFrame)}));
    CHECK(backend.result.code == foundation::ResultCode::kUnsupported);

    std::cout << "Module authoring backend smoke passed\n";
    return 0;
}
