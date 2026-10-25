/** @file builtin_render_passes.hpp @brief Prepares registered passes with immutable frame commands. */
#pragma once

#include "render_pass.hpp"
#include "render_feature_context.hpp"
#include "renderer.hpp"
#include "renderer_variants_internal.hpp"

namespace aeg::renderer::detail {
/** Renderer resolves/imports resources and prepares the fixed native recorder.
 *  Actual Scene operations derive native identity from their projector source;
 *  fixed technical NativePassSequence calls retain their native instance/configuration. */
[[nodiscard]] foundation::ResultValue<pipeline::RenderGraphPass> add_builtin_render_pass(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderPassTypeId type,
    pipeline::RenderPassSetupFunction setup, pipeline::RenderPassExecuteFunction execute,
    pipeline::RenderPassInstanceId authored_instance = {});
}  // namespace aeg::renderer::detail
