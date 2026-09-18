#include "CustomOutlineFeature.hpp"

namespace game::render {

const aeg::pipeline::FeatureDescriptor& CustomOutlineFeature::descriptor() const noexcept {
    static const aeg::pipeline::FeatureDescriptor value{
        .id = aeg::pipeline::FeatureId("Game.CustomOutlineFeature"),
        .name = "Game.CustomOutlineFeature",
        .domain = aeg::pipeline::FeatureDomain::kRender,
        .domain_mask = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender),
        .supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame),
        .version = {1, 0, 0},
        .owner_module = aeg::pipeline::ModuleId("GameModule"),
    };
    return value;
}

aeg::foundation::Result CustomOutlineFeature::setup(aeg::pipeline::RenderGraphBuilder&) const {
    // The authored Pass graph is expanded by the runtime adapter. The Feature
    // owns that graph and contributes no independent top-level operation here.
    return aeg::foundation::Result::success();
}

}  // namespace game::render
