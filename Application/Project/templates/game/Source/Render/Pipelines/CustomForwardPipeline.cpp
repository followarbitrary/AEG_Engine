#include "CustomForwardPipeline.hpp"

namespace game::render {

const aeg::pipeline::PipelineDescriptor& CustomForwardPipeline::descriptor() const noexcept {
    static const aeg::pipeline::PipelineDescriptor value{
        .id = aeg::pipeline::PipelineId("Game.CustomForward"),
        .name = "Game.CustomForward",
        .version = {1, 0, 0},
        .category = aeg::pipeline::PipelineCategory::kRendering,
        .supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame),
        .domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender),
        .default_execution_domain = aeg::pipeline::ExecutionDomain::kRenderThread,
        .default_time_policy = aeg::pipeline::TimePolicy::kFrame,
        .owner_module = aeg::pipeline::ModuleId("GameModule"),
    };
    return value;
}

aeg::foundation::Result CustomForwardPipeline::build(aeg::pipeline::PipelineBuilder& builder) const {
    const auto stage = builder.add_stage({
        .id = aeg::pipeline::StageId("Game.CustomForward.RenderBuild"),
        .phase = aeg::pipeline::EnginePhase::kRenderBuild,
        .execution_domain = aeg::pipeline::ExecutionDomain::kRenderThread,
        .time_policy = aeg::pipeline::TimePolicy::kFrame,
        .parallelizable = false,
        .phase_id = aeg::pipeline::to_phase_id(aeg::pipeline::EnginePhase::kRenderBuild),
    });
    if (!stage) {
        return builder.status();
    }
    const auto point = builder.add_feature_point({
        .id = aeg::pipeline::FeaturePointId("Game.CustomForward.PostProcess"),
        .name = "Post Process",
        .pipeline = descriptor().id,
        .anchor_stage = aeg::pipeline::StageId("Game.CustomForward.RenderBuild"),
        .position = aeg::pipeline::FeaturePointPosition::kAfter,
        .allowed_domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender),
        .allowed_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame),
        .flags = aeg::pipeline::FeaturePointFlags::kMayCreateGraphicsPass,
    });
    return point ? aeg::foundation::Result::success() : builder.status();
}

}  // namespace game::render
