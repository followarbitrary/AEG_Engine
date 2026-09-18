#include "VolumetricFogPass.hpp"

namespace game::render {

const aeg::pipeline::RenderPassDescriptor& VolumetricFogPass::descriptor() const noexcept {
    static const aeg::pipeline::RenderPassDescriptor value{
        .type = aeg::pipeline::RenderPassTypeId("Game.VolumetricFogPass"),
        .owner_module = aeg::pipeline::ModuleId("GameModule"),
        .name = "Game.VolumetricFogPass",
        .version = {1, 0, 0},
        .graph_type = aeg::pipeline::RenderGraphPassType::kGraphics,
        .allowed_queue_mask = 1ull << static_cast<uint8_t>(aeg::rhi::QueueClass::kGraphics),
    };
    return value;
}

aeg::foundation::Result VolumetricFogPass::setup(aeg::pipeline::RenderPassBuilder&,
                                                 const aeg::pipeline::RenderPassConfiguration&) const {
    return aeg::foundation::Result::success();
}

aeg::foundation::Result VolumetricFogPass::execute(aeg::pipeline::RenderPassExecutionContext&,
                                                   const aeg::pipeline::RenderPassConfiguration&) const {
    return aeg::foundation::Result::success();
}

}  // namespace game::render
