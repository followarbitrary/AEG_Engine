#include "CustomOutlineFeature.hpp"

#include <string>

namespace game::render {

const aeg::pipeline::FeatureDescriptor& CustomOutlineFeature::descriptor() const noexcept {
    static const aeg::pipeline::FeatureDescriptor kValue{
        .id = aeg::pipeline::FeatureId("Game.CustomOutlineFeature"),
        .name = "Game.CustomOutlineFeature",
        .domain = aeg::pipeline::FeatureDomain::kRender,
        .domain_mask = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender),
        .supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame),
        .version = {1, 0, 0},
        .owner_module = aeg::pipeline::ModuleId("GameModule"),
    };
    return kValue;
}

aeg::foundation::Result CustomOutlineFeature::setup(aeg::pipeline::RenderGraphBuilder& builder) const {
    const std::string stage_name =
        "Game.CustomOutlineFeature.RenderBuild." + std::to_string(builder.feature_instance_id().value());
    const auto stage = builder.add_stage({.id = aeg::pipeline::StageId(stage_name),
                                          .phase = aeg::pipeline::EnginePhase::kRenderBuild,
                                          .execution_domain = aeg::pipeline::ExecutionDomain::kRenderThread,
                                          .time_policy = aeg::pipeline::TimePolicy::kFrame,
                                          .parallelizable = false});
    return stage ? aeg::foundation::Result::success()
                 : aeg::foundation::invalid_state("custom outline render stage declaration failed");
}

}  // namespace game::render
