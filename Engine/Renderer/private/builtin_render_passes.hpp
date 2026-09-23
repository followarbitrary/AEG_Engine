/** @file builtin_render_passes.hpp @brief Prepares registered passes with immutable frame commands. */
#pragma once

#include "render_pass.hpp"
#include "renderer.hpp"
#include "renderer_variants.hpp"

namespace aeg::renderer::detail {
/** Resource resolution/import belongs to the Feature; all declarations and commands run through the pass. */
[[nodiscard]] foundation::ResultValue<pipeline::RenderGraphPass> add_builtin_render_pass(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderPassTypeId type,
    pipeline::RenderPassSetupFunction setup, pipeline::RenderPassExecuteFunction execute);
}  // namespace aeg::renderer::detail
