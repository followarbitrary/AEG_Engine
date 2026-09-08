/**
 * @file render_feature_runtime.hpp
 * @brief Expands Pipeline render features into a concrete RenderGraph.
 */
#pragma once

#include <memory>
#include <span>

#include "pipeline.hpp"
#include "render_graph.hpp"

namespace aeg::render_graph {
/** @brief Associates a public resource id with an existing graph buffer. */
struct RenderGraphFeatureBufferBinding {
    pipeline::RenderResourceId id{};
    RenderGraphBuffer buffer{};
};

/** @brief Associates a public resource id with an existing graph texture. */
struct RenderGraphFeatureTextureBinding {
    pipeline::RenderResourceId id{};
    RenderGraphTexture texture{};
};

/**
 * @brief Runtime inputs supplied to RenderFeature graph construction.
 *
 * The binding spans are borrowed and must remain valid until build() returns.
 * The referenced graph handles must belong to the RenderGraph passed to the
 * runtime. user_data is borrowed; it must remain valid for build() and for
 * graph execution when a feature captures it in a pass callback. The runtime
 * does not retain the binding arrays themselves after construction.
 */
struct RenderGraphFeatureRuntimeDesc {
    std::span<const RenderGraphFeatureBufferBinding> buffers{};
    std::span<const RenderGraphFeatureTextureBinding> textures{};
    void* user_data{nullptr};
    const void* feature_data{nullptr};
};

/**
 * @brief Builds the RenderGraph contribution of a compiled pipeline plan.
 *
 * The PipelinePlanHandle overload transfers plan ownership to the graph
 * until the graph is submitted through a FrameContext.
 */
class RenderGraphFeatureRuntime final {
public:
    explicit RenderGraphFeatureRuntime(RenderGraph& graph, const pipeline::PipelinePlan& plan,
                                       const pipeline::PipelineRegistry& registry,
                                       const RenderGraphFeatureRuntimeDesc& desc = {});
    explicit RenderGraphFeatureRuntime(RenderGraph& graph, pipeline::PipelinePlanHandle plan,
                                       const pipeline::PipelineRegistry& registry,
                                       const RenderGraphFeatureRuntimeDesc& desc = {});
    ~RenderGraphFeatureRuntime();

    RenderGraphFeatureRuntime(const RenderGraphFeatureRuntime&) = delete;
    RenderGraphFeatureRuntime& operator=(const RenderGraphFeatureRuntime&) = delete;
    RenderGraphFeatureRuntime(RenderGraphFeatureRuntime&&) = delete;
    RenderGraphFeatureRuntime& operator=(RenderGraphFeatureRuntime&&) = delete;

    /** @brief Adds all render-domain feature passes and resources to the graph. */
    [[nodiscard]] foundation::Result build();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Builds render-feature contributions with one short-lived runtime object. */
[[nodiscard]] foundation::Result build_render_graph_features(RenderGraph& graph, const pipeline::PipelinePlan& plan,
                                                             const pipeline::PipelineRegistry& registry,
                                                             const RenderGraphFeatureRuntimeDesc& desc = {});

/** @brief Builds contributions while retaining the plan through frame completion. */
[[nodiscard]] foundation::Result build_render_graph_features(RenderGraph& graph, pipeline::PipelinePlanHandle plan,
                                                             const pipeline::PipelineRegistry& registry,
                                                             const RenderGraphFeatureRuntimeDesc& desc = {});
}  // namespace aeg::render_graph
