#pragma once

#include <optional>

#include "render_feature_runtime.hpp"
#include "render_feature_history.hpp"
#include "render_scene_lowerer.hpp"
#include "render_pass_execution_host.hpp"
#include "renderer.hpp"

namespace aeg::renderer {
/** Engine host binding only. Borrowed resolver is used during construction, never during recording. */
struct RenderFeatureSceneBinding final {
    feature::RenderResourceId id{};
    uint64_t version{1};
    feature::FeatureInstanceId owner{};
    std::shared_ptr<const RenderSceneSnapshot> snapshot{};
    const RenderResourceResolver* resolver{nullptr};
};
inline constexpr feature::RenderResourceId kRuntimeSceneResourceId{"Runtime.Scene"};

/** Projects owning Feature outputs using this frame's bindings and the complete live owner set. */
[[nodiscard]] foundation::Result build_render_feature_outputs(
    render_graph::RenderGraph& graph, render_graph::FrameContext& frame,
    std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> outputs,
    std::span<const feature::RenderResourceOwner> live_owners,
    std::span<const render_graph::RenderGraphFeatureTextureBinding> textures,
    std::span<const RenderFeatureSceneBinding> scenes, render_graph::RenderFeatureHistory& history,
    uint64_t history_key = 0, std::optional<render_graph::RenderPassExecutionHost> pass_host = std::nullopt);

/** Copies one Feature output into a stable View scope and rewrites scene/shadow external identities. */
[[nodiscard]] feature::RenderFeatureOutput scope_render_feature_output(
    const feature::RenderFeatureOutput& output, RenderViewId view, bool canonical_view);

/** Renderer-owned adapter for neutral Feature scene draws, reusing native color and depth paths. */
class RenderFeatureDrawLowerer final : public render_graph::RenderSceneDrawLowerer {
public:
    explicit RenderFeatureDrawLowerer(std::span<const RenderFeatureSceneBinding> scenes) : scenes_(scenes) {}
    RenderFeatureDrawLowerer(std::span<const RenderFeatureSceneBinding> scenes,
        render_graph::RenderPassExecutionHost host) : scenes_(scenes), pass_host_(host), registered_native_(true) {}
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const void>> lower(
        pipeline::RenderGraphBuilder& builder, const feature::RenderDrawScene& draw,
        const render_graph::RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner,
        uint64_t frame_index) const override;
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const void>> lower(
        pipeline::RenderGraphBuilder& builder, const feature::RenderDrawDepthScene& draw,
        const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
        uint64_t frame_index) const override;
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const void>> lower(
        pipeline::RenderGraphBuilder& builder, const feature::RenderDrawGBufferScene& draw,
        const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
        uint64_t frame_index) const override;
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const void>> lower(
        pipeline::RenderGraphBuilder& builder, const feature::RenderDeferredLightingScene& draw,
        const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
        uint64_t frame_index) const override;
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const void>> lower(
        pipeline::RenderGraphBuilder& builder, const feature::RenderPostProcessScene& draw,
        const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
        uint64_t frame_index) const override;
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const render_graph::RenderLightListLease>> lower(
        pipeline::RenderGraphBuilder& builder, const feature::RenderLightCullingScene& draw,
        const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
        const render_graph::RenderFeatureBuildScope& scope,
        const render_graph::RenderLightListProduction& production) const override;
    [[nodiscard]] foundation::Result lower(pipeline::RenderGraphBuilder& builder,
        const feature::RenderDrawClusteredScene& draw, const render_graph::RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner, const render_graph::RenderFeatureBuildScope& scope,
        const render_graph::RenderLightListLease& light_list) const override;
private:
    [[nodiscard]] foundation::Result validate_native_source(const render_graph::RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner, uint64_t frame_index,
        render_graph::RenderSceneOperationKind kind) const;
    std::span<const RenderFeatureSceneBinding> scenes_;
    render_graph::RenderPassExecutionHost pass_host_{};
    bool registered_native_{false};
};
}
