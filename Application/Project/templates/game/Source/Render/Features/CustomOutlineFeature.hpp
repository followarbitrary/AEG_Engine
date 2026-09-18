#pragma once
#include "pipeline.hpp"
namespace game::render {
class CustomOutlineFeature final : public aeg::pipeline::RenderFeature {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override;

protected:
    [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::RenderGraphBuilder&) const override;
};
}  // namespace game::render
