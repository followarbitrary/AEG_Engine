#include <iostream>
#include <memory>
#include <vector>

#include "editor_composition_preview.hpp"
#include "editor_composition_test_support.hpp"
#include "pipeline_configuration.hpp"

namespace {
uint32_t g_preview_task_runs = 0;
std::vector<aeg::pipeline::FeatureInstanceId> g_preview_feature_builds;

aeg::foundation::Result preview_task(aeg::pipeline::PipelineTaskContext&) {
    ++g_preview_task_runs;
    return aeg::foundation::Result::success();
}

class PreviewFeature final : public aeg::pipeline::PipelineFeature {
public:
    PreviewFeature() {
        descriptor_.id = aeg::pipeline::FeatureId("Game.EditorFeature");
        descriptor_.name = "Game.EditorFeature";
        descriptor_.domain = aeg::pipeline::FeatureDomain::kRender;
        descriptor_.domain_mask = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender);
        descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame);
        descriptor_.owner_module = aeg::pipeline::ModuleId("Game.Editor");
    }

    const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

    aeg::foundation::Result collect_contributions(aeg::pipeline::FeatureBuildContext& context) const override {
        g_preview_feature_builds.push_back(context.feature_instance_id());
        return aeg::foundation::Result::success();
    }

private:
    aeg::pipeline::FeatureDescriptor descriptor_{};
};

class PreviewPipeline final : public aeg::pipeline::Pipeline {
public:
    PreviewPipeline() {
        descriptor_.id = aeg::pipeline::PipelineId("Game.EditorPipeline");
        descriptor_.name = "Game.EditorPipeline";
        descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame);
        descriptor_.domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender);
        descriptor_.owner_module = aeg::pipeline::ModuleId("Game.Editor");
    }

    const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({.id = aeg::pipeline::StageId("RenderBuild"),
                                              .phase = aeg::pipeline::EnginePhase::kFrameBegin,
                                              .execution_domain = aeg::pipeline::ExecutionDomain::kMainThread,
                                              .time_policy = aeg::pipeline::TimePolicy::kFrame,
                                              .parallelizable = false});
        const auto task = builder.add_task({.id = aeg::pipeline::TaskId("PreviewTask"),
                                            .flags = aeg::pipeline::TaskFlags::kNone,
                                            .execute = &preview_task});
        const auto point =
            builder.add_feature_point({.id = aeg::pipeline::FeaturePointId("PostProcess"),
                                       .name = "Post Process",
                                       .pipeline = descriptor_.id,
                                       .anchor_stage = aeg::pipeline::StageId("RenderBuild"),
                                       .position = aeg::pipeline::FeaturePointPosition::kAfter,
                                       .allowed_domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender),
                                       .allowed_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame),
                                       .flags = aeg::pipeline::FeaturePointFlags::kMayCreateGraphicsPass});
        if (!stage || !task || !point)
            return builder.status();
        if (const auto result = builder.bind_task(task, stage); !result)
            return result;
        return builder.bind_feature(
            {.feature = aeg::pipeline::FeatureId("Game.EditorFeature"),
             .pipeline = descriptor_.id,
             .scope = aeg::pipeline::PipelineScope::kFrame,
             .point = aeg::pipeline::FeaturePointId("PostProcess"),
             .priority = 0,
             .enabled = true,
             .instance = aeg::pipeline::make_feature_instance_id(aeg::pipeline::FeatureId("Game.EditorFeature"),
                                                                 descriptor_.id, aeg::pipeline::PipelineScope::kFrame,
                                                                 aeg::pipeline::FeaturePointId("PostProcess"))});
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class PreviewPass final : public aeg::pipeline::RenderPass {
public:
    const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override { return descriptor_; }
    aeg::foundation::Result setup(aeg::pipeline::RenderPassBuilder&,
                                  const aeg::pipeline::RenderPassConfiguration&) const override {
        return aeg::foundation::Result::success();
    }
    aeg::foundation::Result execute(aeg::pipeline::RenderPassExecutionContext&,
                                    const aeg::pipeline::RenderPassConfiguration&) const override {
        return aeg::foundation::Result::success();
    }

private:
    aeg::pipeline::RenderPassDescriptor descriptor_{
        .type = aeg::pipeline::RenderPassTypeId("Game.EditorPass"),
        .owner_module = aeg::pipeline::ModuleId("Game.Editor"),
        .name = "Game.EditorPass",
        .version = {1, 0, 0},
        .graph_type = aeg::pipeline::RenderGraphPassType::kGraphics,
        .allowed_queue_mask = uint64_t{1} << static_cast<uint8_t>(aeg::rhi::QueueClass::kGraphics)};
};
}  // namespace

int main() {
    using namespace aeg;
    pipeline::PipelineRegistry registry;
    pipeline::RenderPassRegistry render_passes;
    engine::PipelineCompositionRuntime runtime;
    application::EditorCompositionPreviewController preview(registry, render_passes, runtime);
    tests::EditorCompositionFixture fixture;
    if (!registry.register_feature([] { return std::make_unique<PreviewFeature>(); }) ||
        !registry.register_pipeline([] { return std::make_unique<PreviewPipeline>(); }))
        return 1;

    pipeline::PipelineScopeHost host(pipeline::PipelineScope::kFrame, pipeline::PipelineInstanceId("pipeline"));
    const engine::PipelineHostBinding binding{"pipeline", &host};
    auto document = std::make_shared<const pipeline::PipelineDocument>(fixture.document);
    const auto missing_pass =
        preview.request(document, fixture.catalog, 6, std::span<const engine::PipelineHostBinding>(&binding, 1));
    if (missing_pass.code != foundation::ResultCode::kNotFound || runtime.has_pending() || !preview.state().stale)
        return 8;
    if (!render_passes.register_pass([] { return std::make_unique<PreviewPass>(); }))
        return 9;
    const auto request =
        preview.request(document, fixture.catalog, 7, std::span<const engine::PipelineHostBinding>(&binding, 1));
    if (!request)
        std::cerr << "preview request failed: " << request.message << '\n';
    if (!request || !preview.state().rebuilding || preview.state().stale || preview.state().active_revision != 0 ||
        preview.state().draft_revision != 7 || !runtime.has_pending())
        return 2;
    if (const auto result = preview.apply_at_frame_boundary(); !result) {
        std::cerr << "apply failed: " << result.message << "\n";
        return 3;
    }
    if (preview.state().active_revision != 7)
        return 3;
    if (const auto result = host.bind_pipeline(pipeline::PipelineId("Game.EditorPipeline")); !result) {
        std::cerr << "bind failed: " << result.message << "\n";
        return 4;
    }
    if (const auto result = host.rebuild(registry); !result) {
        std::cerr << "rebuild failed: " << result.message << "\n";
        return 4;
    }
    if (g_preview_feature_builds != std::vector{pipeline::FeatureInstanceId("feature")})
        return 10;
    if (const auto result = host.activate({}); !result) {
        std::cerr << "activate failed: " << result.message << "\n";
        return 5;
    }
    pipeline::PipelineScheduler scheduler({1, 0, 1, 1});
    pipeline::PipelineExecutionContext context(nullptr, pipeline::PipelineScope::kFrame, &host.artifacts());
    if (const auto result = host.execute(scheduler, context); !result || g_preview_task_runs != 1)
        return 6;
    if (!preview.stop() || !preview.state().stopped || preview.state().rebuilding || host.is_active() ||
        runtime.active_host_count() != 0 || runtime.active_configuration_count() != 0)
        return 7;
    auto duplicate = std::make_shared<pipeline::PipelineInstanceConfiguration>(*host.configuration());
    duplicate->features.push_back(duplicate->features.front());
    duplicate->features.back().instance = pipeline::FeatureInstanceId("duplicate");
    if (!host.set_configuration(duplicate) || host.rebuild(registry).code != foundation::ResultCode::kInvalidArgument)
        return 11;
    std::cout << "Editor composition preview smoke passed\n";
    return 0;
}
