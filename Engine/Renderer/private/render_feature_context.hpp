/**
 * @file render_feature_context.hpp
 * @brief Renderer-private host context for Scene lowering and fixed native assembly.
 *
 * The context carries engine-owned scene/resource state and the borrowed
 * RenderPass execution host used while a frame graph is assembled. It is deliberately
 * kept out of the public Renderer aggregate so Feature authors cannot depend on
 * Renderer execution internals.
 */
#pragma once

#include "render_pass_execution_host.hpp"
#include "render_scene_lowerer.hpp"
#include "renderer.hpp"
#include "native_pass_sequence.hpp"

namespace aeg::renderer {

/** @brief Private per-build native resources and the actual Scene operation source. */
struct RenderFeatureContext final {
    const RenderSceneSnapshot* scene_snapshot{nullptr};
    const RenderResourceResolver* resource_resolver{nullptr};
    void* user_data{nullptr};
    /** Private host capability; graph retains each resolved pass after execution. */
    render_graph::RenderPassExecutionHost pass_execution{};
    bool registered_native{false};
    const render_graph::RenderSceneOperationSource* operation_source{nullptr};
    /** Fixed native topology options; independent of business Feature configuration. */
    const NativePassSequence* native_sequence{nullptr};

    [[nodiscard]] bool is_valid() const noexcept { return scene_snapshot != nullptr && scene_snapshot->is_valid(); }
};

/** @brief Returns the typed renderer context attached to a fixed native graph build. */
[[nodiscard]] const RenderFeatureContext* render_feature_context(
    const pipeline::RenderGraphBuilder& builder) noexcept;

}  // namespace aeg::renderer
