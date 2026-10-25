/**
 * @file render_feature_runtime.hpp
 * @brief Expands committed neutral Feature intents into a concrete RenderGraph.
 */
#pragma once

#include <memory>
#include <span>

#include "feature_runtime.hpp"
#include "render_graph.hpp"

namespace aeg::render_graph {
/** @brief Associates a public resource id with an existing graph texture. */
struct RenderGraphFeatureTextureBinding {
    feature::RenderResourceId id{};
    RenderGraphTexture texture{};
    uint64_t version{1};
    /** Invalid means a shared host resource; otherwise only this instance may bind it. */
    feature::FeatureInstanceId owner{};
};

/**
 * @brief Runtime inputs supplied to committed Feature output construction.
 *
 * The texture binding span is borrowed until build_render_graph_features() returns.
 * The referenced graph handles must belong to the RenderGraph passed to the
 * runtime. The runtime does not retain the binding array itself after construction.
 */
struct RenderGraphFeatureRuntimeDesc {
    std::span<const RenderGraphFeatureTextureBinding> textures{};
    /** Opaque Renderer-owned lowerer used only during this build call. */
    const void* draw_lowerer{nullptr};
    /** Opaque active FrameContext owner required for native light-list operations. */
    const void* current_frame_owner{nullptr};
    /** Opaque stable temporal identity supplied by the host, typically RenderView::id.value(). */
    uint64_t history_key{0};
};

[[nodiscard]] foundation::Result build_render_graph_features(RenderGraph& graph,
    std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> outputs,
    const RenderGraphFeatureRuntimeDesc& desc = {}, uint64_t frame_index = 0,
    const void* history_owner = nullptr);
}  // namespace aeg::render_graph
