/** @file builtin_render_passes.hpp @brief Prepares registered passes with immutable frame commands. */
#pragma once

#include "render_pass.hpp"
#include "renderer.hpp"
#include "renderer_variants.hpp"

namespace aeg::renderer::detail {
/** Resource resolution/import belongs to the Feature; all declarations and commands run through the pass.
 *  An authored instance ID is supplied by built-in graph adapters so repeated
 *  Pass nodes of one type retain independent configuration and ordering. */
[[nodiscard]] foundation::ResultValue<pipeline::RenderGraphPass> add_builtin_render_pass(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderPassTypeId type,
    pipeline::RenderPassSetupFunction setup, pipeline::RenderPassExecuteFunction execute,
    pipeline::RenderPassInstanceId authored_instance = {});
}  // namespace aeg::renderer::detail
