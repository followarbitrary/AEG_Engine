/**
 * @file engine_frame_orchestrator.hpp
 * @brief Coordinates pipeline runtime phases with per-frame RenderGraph execution.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "frame_context.hpp"
#include "feature_types.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "world_scope.hpp"

namespace aeg::engine {
namespace detail { struct EngineFrameInspection; struct EngineFrameWorldCommands; }
class EngineFrameOrchestrator;
class EngineFramePipelineHost;
class EngineFramePipelineRegistration;

/** @brief Conventional RenderGraph resource identity for the acquired swapchain image. */
inline constexpr feature::RenderResourceId kSwapchainImageResourceId{"SwapchainImage"};

/**
 * @brief Context supplied to a frame-local render-feature preparation callback.
 *
 * The callback runs after
 * FrameContext has begun and, when requested, after a
 * swapchain image has been acquired and imported into the
 * RenderGraph. It is a
 * controlled RenderBuild hook for producers that must upload or prepare
 * frame-local
 * resources before fixed native or modern Feature graph construction. The callback
 * must not retain the context or its acquired-frame
 * pointer.
 */
struct EngineFramePreparationContext final {
    render_graph::FrameContext& frame_context;
    render_graph::RenderGraph& render_graph;
    const rhi::FrameAcquireResult* acquired_frame{nullptr};
    /** @brief Logical RenderGraph texture for the acquired swapchain image. */
    render_graph::RenderGraphTexture swapchain_resource{};
    /** Borrowed extracted snapshots, available to RenderBuild preparation for per-View targets. */
    std::span<const std::shared_ptr<const renderer::RenderSceneSnapshot>> render_scenes{};

    /** @brief Registers an imported preparation texture for modern Feature projection by stable resource id. */
    [[nodiscard]] foundation::Result bind_feature_texture(feature::RenderResourceId id,
                                                          render_graph::RenderGraphTexture texture) const;

private:
    EngineFramePreparationContext(render_graph::FrameContext& frame_context, render_graph::RenderGraph& render_graph,
                                  const rhi::FrameAcquireResult* acquired_frame,
                                  render_graph::RenderGraphTexture swapchain_resource,
                                  std::span<const std::shared_ptr<const renderer::RenderSceneSnapshot>> scenes,
                                  EngineFrameOrchestrator& owner) noexcept;

    EngineFrameOrchestrator* owner_{nullptr};

    friend class EngineFrameOrchestrator;
};

using EngineFramePreparationFunction = std::function<foundation::Result(const EngineFramePreparationContext& context)>;

/**
 * @brief Context supplied after frame preparation and before RenderGraph execution.
 *
 * The callback is
 * intended for modern Feature projection and frame-local terminal work such as copying
 * the authored SceneColor into a readback allocation. It may
 * add passes and
 * dependencies to the active RenderGraph, but must not retain this context or
 * mutate
 * feature-resource bindings.
 */
struct EngineFramePostBuildContext final {
    render_graph::FrameContext& frame_context;
    render_graph::RenderGraph& render_graph;
    const rhi::FrameAcquireResult* acquired_frame{nullptr};
    /** @brief Logical RenderGraph texture for the acquired swapchain image. */
    render_graph::RenderGraphTexture swapchain_resource{};
    /** Resolved frame bindings, including resources added by the preparation hook. */
    std::span<const render_graph::RenderGraphFeatureTextureBinding> feature_textures{};
    /** Borrowed span of owning current-frame snapshots; lowerers retain selected owners. */
    std::span<const std::shared_ptr<const renderer::RenderSceneSnapshot>> render_scenes{};
    const renderer::RenderResourceResolver* render_resource_resolver{nullptr};

private:
    EngineFramePostBuildContext(render_graph::FrameContext& frame_context, render_graph::RenderGraph& render_graph,
                                const rhi::FrameAcquireResult* acquired_frame,
                                render_graph::RenderGraphTexture swapchain_resource,
                                std::span<const render_graph::RenderGraphFeatureTextureBinding> textures,
                                std::span<const std::shared_ptr<const renderer::RenderSceneSnapshot>> scenes,
                                const renderer::RenderResourceResolver* resolver) noexcept
        : frame_context(frame_context),
          render_graph(render_graph),
          acquired_frame(acquired_frame),
          swapchain_resource(swapchain_resource), feature_textures(textures), render_scenes(scenes),
          render_resource_resolver(resolver) {}

    friend class EngineFrameOrchestrator;
};

using EngineFramePostBuildFunction = std::function<foundation::Result(const EngineFramePostBuildContext& context)>;

/**
 * @brief Immutable World input supplied while resolving one generated RenderView.
 *
 * The snapshot is borrowed
 * and is valid only for the resolver invocation. The
 * view_id identifies the registered World/Frame binding being
 * resolved.
 */
struct EngineFrameRenderViewContext final {
    /** @brief Committed immutable World state captured for this resolver call. */
    const ecs::WorldSnapshot& world_snapshot;
    /** @brief Registered World/Frame view slot being resolved. */
    uint32_t view_id{0};
    /** @brief Output pixel extent selected for the generated RenderView. */
    renderer::RenderExtent output_extent{};
};

/** @brief Produces one RenderView from the committed snapshot of a registered World view. */
using EngineFrameRenderViewResolver =
    std::function<foundation::ResultValue<renderer::RenderView>(const EngineFrameRenderViewContext& context)>;

/** @brief Per-frame inputs consumed by EngineFrameOrchestrator. */
struct EngineFrameDesc final {
    uint64_t fixed_tick{0};
    bool reset_render_graph{true};
    bool acquire_swapchain_image{false};
    rhi::SwapchainHandle swapchain{};
    rhi::TextureDesc swapchain_image_desc{};
    /** @brief Output extent supplied to generated offscreen RenderViews when no swapchain image is acquired. */
    renderer::RenderExtent render_output_extent{};
    /** @brief Optional override; undefined uses the state reported by image acquisition. */
    rhi::ResourceState swapchain_image_initial_state{};
    feature::RenderResourceId swapchain_resource_id{kSwapchainImageResourceId};
    bool present_acquired_frame{false};
    std::span<const render_graph::RenderGraphFeatureTextureBinding> render_feature_textures{};
    std::span<const render_graph::RenderGraphExecutionLease> render_graph_execution_leases{};
    renderer::Renderer* renderer{nullptr};
    std::span<const renderer::RenderView> render_views{};
    /**
     * @brief Optional snapshot-only RenderView resolver used when render_views is empty.
     *
     * It runs
     * once for each registered World view during RenderExtract and must
     * return a view for the context's World
     * and view_id. It must not retain the
     * borrowed snapshot reference.
     */
    EngineFrameRenderViewResolver render_view_resolver{};
    void* user_data{nullptr};
    const renderer::RenderResourceResolver* render_resource_resolver{nullptr};
    bool require_resolved_render_resources{false};
    /** @brief Optional controlled hook for fixed native work and frame-local resource preparation. */
    EngineFramePreparationFunction render_feature_preparation{};
    /** @brief Optional modern Feature projection and terminal-work hook before graph execution. */
    EngineFramePostBuildFunction render_feature_post_build{};
};

/** @brief Drives registered pipeline instances and the frame-local RenderGraph. */
class EngineFrameOrchestrator final {
public:
    EngineFrameOrchestrator(const EngineFramePipelineHost& pipeline_host,
                            render_graph::FrameContext& frame_context, render_graph::RenderGraph& render_graph,
                            uint64_t available_capabilities = 0);
    ~EngineFrameOrchestrator();

    EngineFrameOrchestrator(const EngineFrameOrchestrator&) = delete;
    EngineFrameOrchestrator& operator=(const EngineFrameOrchestrator&) = delete;
    EngineFrameOrchestrator(EngineFrameOrchestrator&&) = delete;
    EngineFrameOrchestrator& operator=(EngineFrameOrchestrator&&) = delete;

    /** @brief Registers a non-owning pipeline scope host. */
    [[nodiscard]] foundation::Result register_pipeline(const EngineFramePipelineRegistration& registration);
    /** @brief Registers a World aggregate for command application and snapshot publication. */
    [[nodiscard]] foundation::Result register_world(world::WorldScope& world_scope);
    /** Registers snapshot extraction for a FeatureGraph view without a Pipeline host. */
    [[nodiscard]] foundation::Result register_world_frame(world::WorldScope& world_scope, uint32_t view_id = 0);
    /** @brief Registers a World view and connects it to a Frame pipeline host. */
    [[nodiscard]] foundation::Result register_world_frame(world::WorldScope& world_scope,
                                                          const EngineFramePipelineRegistration& registration,
                                                          uint32_t view_id = 0);
    /** @brief Unregisters a World view registration. */
    [[nodiscard]] foundation::Result unregister_world_frame(ecs::WorldHandle world, uint32_t view_id);
    /** @brief Unregisters a World aggregate after its views have been removed. */
    [[nodiscard]] foundation::Result unregister_world(ecs::WorldHandle world);

    /** @brief Builds and activates all registered pipeline instances. */
    [[nodiscard]] foundation::Result initialize();
    /** @brief Begins a frame and pins all active pipeline plans. */
    [[nodiscard]] foundation::Result begin_frame(const EngineFrameDesc& desc = {});
    /** @brief Executes all ordinary frame phases in the registered global order. */
    [[nodiscard]] foundation::Result run_frame(const EngineFrameDesc& desc = {});
    /** @brief Executes shutdown work and deactivates registered pipelines. */
    [[nodiscard]] foundation::Result shutdown();

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] bool is_frame_active() const noexcept;
    [[nodiscard]] std::size_t world_count() const noexcept;
    [[nodiscard]] std::size_t world_frame_count() const noexcept;
    /** @brief Number of successful draw commands recorded by the last executed frame graph. */
    [[nodiscard]] uint32_t draw_call_count() const noexcept;

private:
    friend struct EngineFramePreparationContext;
    friend struct detail::EngineFrameInspection;
    friend struct detail::EngineFrameWorldCommands;
    [[nodiscard]] foundation::Result bind_feature_texture(feature::RenderResourceId id,
                                                          render_graph::RenderGraphTexture texture);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace aeg::engine
