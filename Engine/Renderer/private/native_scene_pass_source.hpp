/** @brief Renderer-private observation of the actual immutable prepared Scene operation. */
#pragma once

#include "render_pass.hpp"
#include "render_scene_lowerer.hpp"

namespace aeg::renderer::detail {
struct NativeScenePassInspection final {
    [[nodiscard]] static foundation::ResultValue<render_graph::RenderSceneOperationSource> source(
        const pipeline::RenderPassConfiguration& configuration);
};
} // namespace aeg::renderer::detail
