/**
 * @file render_feature_runtime.hpp
 * @brief Expands Pipeline render features into a concrete RenderGraph.
 */
#pragma once

#include <functional>
#include <memory>
#include <span>

#include "pipeline.hpp"
#include "render_graph.hpp"
#include "render_state_flow.hpp"

namespace aeg::render_graph {
/** @brief One terminal state texture and the blend mode requested by its owning Feature. */
struct RenderStateCompositeInput final {
    pipeline::RenderGraphTexture texture{};
    pipeline::RenderStateBlendMode blend_mode{pipeline::RenderStateBlendMode::kReplace};
};

/**
 * @brief Invoked to combine multiple terminal state textures into the Present target.
 *
 * The callback owns the
 * visual blend semantics and must declare every resource
 * access through the supplied builder. It is called only
 * while the feature-build
 * transaction is active.
 */
using RenderStateCompositeFunction = std::function<foundation::Result(pipeline::RenderGraphBuilder& builder,
                                                                      std::span<const RenderStateCompositeInput> inputs,
                                                                      pipeline::RenderGraphTexture destination)>;

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

/** @brief Optional transient texture template used for per-feature render-state outputs. */
struct RenderGraphFeatureStateDesc {
    bool enabled{false};
    rhi::TextureDesc texture_desc{};
    /** Bound RenderResourceId receiving the terminal state at Present. */
    pipeline::RenderResourceId present_target_id{};
    rhi::Extent3D present_extent{};
    /** Editor-authored state links layered over the compiled Pipeline dependencies. */
    std::span<const RenderStateLinkOverride> link_overrides{};
    /** Editor-authored per-Feature blend choices, resolved by compiled plan node. */
    std::span<const RenderStateBlendOverride> blend_overrides{};
    /** Required when more than one state branch reaches the Present target. */
    RenderStateCompositeFunction compose_terminal_states{};
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
    RenderGraphFeatureStateDesc state{};
    /** Borrowed pipeline configuration retained by the active frame host. */
    std::shared_ptr<const pipeline::PipelineInstanceConfiguration> configuration{};
    /** Borrowed pass registry used by authored RenderFeatures. */
    const pipeline::RenderPassRegistry* render_passes{nullptr};
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
