#pragma once
#include "render_pass.hpp"
namespace game::render {
class VolumetricFogPass final : public aeg::pipeline::RenderPass {
public:
    [[nodiscard]] const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override;
    [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::RenderPassBuilder&,
                                                const aeg::pipeline::RenderPassConfiguration&) const override;
    [[nodiscard]] aeg::foundation::Result execute(aeg::pipeline::RenderPassExecutionContext&,
                                                  const aeg::pipeline::RenderPassConfiguration&) const override;
};
}  // namespace game::render
