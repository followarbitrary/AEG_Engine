/**
 * @file renderer_pipeline.hpp
 * @brief Renderer-private standard frame pipeline declaration.
 *
 * The standard frame topology is an owning Engine-private declaration value.
 */
#pragma once

#include "pipeline.hpp"

namespace aeg::renderer {

[[nodiscard]] foundation::ResultValue<pipeline::PipelineDefinition> make_render_pipeline_definition();

}  // namespace aeg::renderer
