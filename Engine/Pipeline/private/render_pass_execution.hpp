/**
 * @file render_pass_execution.hpp
 * @brief Pipeline-private authored RenderPass expansion helper.
 */
#pragma once

#include "render_pass.hpp"

namespace aeg::pipeline {

/**
 * @brief Expands a registered authored pass into the current graph builder.
 *
 * Registry lookup and pass factory execution are Engine-private execution
 * details; public Pipeline callers use the registry/value contracts directly.
 */
[[nodiscard]] foundation::ResultValue<RenderGraphPass> add_registered_render_pass(
    RenderGraphBuilder&, const RenderPassRegistry&, std::shared_ptr<const RenderPassConfiguration>);

}  // namespace aeg::pipeline
