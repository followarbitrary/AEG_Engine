/** @file render_pipeline_graph.hpp @brief RenderGraph topology for built-in renderer variants. */
#pragma once

#include "pipeline.hpp"
#include "renderer.hpp"

namespace aeg::renderer {

/** @brief Adds the deferred renderer's resource and pass topology to a frame graph. */
[[nodiscard]] foundation::Result build_deferred_render_graph(pipeline::RenderGraphBuilder& builder,
                                                             const RenderFeatureContext& context);

/** @brief Adds the forward renderer's resource and pass topology to a frame graph. */
[[nodiscard]] foundation::Result build_forward_render_graph(pipeline::RenderGraphBuilder& builder,
                                                            const RenderFeatureContext& context);

}  // namespace aeg::renderer
