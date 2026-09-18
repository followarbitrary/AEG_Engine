#pragma once

#include "pipeline.hpp"

namespace game::render {

class CustomForwardPipeline final : public aeg::pipeline::Pipeline {
public:
    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override;
    [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override;
};

}  // namespace game::render
