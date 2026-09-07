/**
 * @file engine_frame_orchestrator.hpp
 * @brief Coordinates pipeline runtime phases with per-frame RenderGraph execution.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "frame_context.hpp"
#include "pipeline.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"
#include "world_scope.hpp"

namespace aeg::engine {
/** @brief Conventional RenderGraph resource identity for the acquired swapchain image. */
inline constexpr pipeline::RenderResourceId kSwapchainImageResourceId{"SwapchainImage"};

/** @brief Per-frame inputs consumed by EngineFrameOrchestrator. */
struct EngineFrameDesc final {
    uint64_t fixed_tick{0};
    bool reset_render_graph{true};
    bool acquire_swapchain_image{false};
    rhi::SwapchainHandle swapchain{};
    rhi::TextureDesc swapchain_image_desc{};
    rhi::ResourceState swapchain_image_initial_state{
        rhi::ResourceUsage::kPresent,     rhi::ShaderStage::kAll, rhi::QueueClass::kGraphics, false,
        rhi::PipelineStage::kAllCommands,
    };
    pipeline::RenderResourceId swapchain_resource_id{kSwapchainImageResourceId};
    bool present_acquired_frame{false};
    std::span<const render_graph::RenderGraphFeatureBufferBinding> render_feature_buffers{};
    std::span<const render_graph::RenderGraphFeatureTextureBinding> render_feature_textures{};
    std::span<const render_graph::RenderGraphExecutionLease> render_graph_execution_leases{};
    void* user_data{nullptr};
};

/** @brief Non-owning registration of one World view and its Frame pipeline host. */
struct WorldFrameBinding final {
    world::WorldScope* world{nullptr};
    pipeline::PipelineScopeHost* frame_host{nullptr};
    uint32_t view_id{0};
    bool frame_host_registered_by_orchestrator{false};
};

/** @brief Drives registered pipeline instances and the frame-local RenderGraph. */
class EngineFrameOrchestrator final {
public:
    EngineFrameOrchestrator(pipeline::PipelineRegistry& registry, pipeline::PipelineScheduler& scheduler,
                            render_graph::FrameContext& frame_context, render_graph::RenderGraph& render_graph,
                            pipeline::CapabilityMask available_capabilities = 0);
    ~EngineFrameOrchestrator();

    EngineFrameOrchestrator(const EngineFrameOrchestrator&) = delete;
    EngineFrameOrchestrator& operator=(const EngineFrameOrchestrator&) = delete;
    EngineFrameOrchestrator(EngineFrameOrchestrator&&) = delete;
    EngineFrameOrchestrator& operator=(EngineFrameOrchestrator&&) = delete;

    /** @brief Registers a non-owning pipeline scope host. */
    [[nodiscard]] foundation::Result register_pipeline(pipeline::PipelineScopeHost& host);
    /** @brief Registers a World Scope and its World pipeline host. */
    [[nodiscard]] foundation::Result register_world(world::WorldScope& world_scope);
    /** @brief Registers a World view and connects it to a Frame pipeline host. */
    [[nodiscard]] foundation::Result register_world_frame(world::WorldScope& world_scope,
                                                          pipeline::PipelineScopeHost& frame_host,
                                                          uint32_t view_id = 0);
    /** @brief Unregisters a pipeline scope host by instance identity. */
    [[nodiscard]] foundation::Result unregister_pipeline(pipeline::PipelineInstanceId instance);
    /** @brief Unregisters a World view registration. */
    [[nodiscard]] foundation::Result unregister_world_frame(ecs::WorldHandle world, uint32_t view_id);
    /** @brief Unregisters a World Scope and its World pipeline host. */
    [[nodiscard]] foundation::Result unregister_world(ecs::WorldHandle world);

    /** @brief Builds and activates all registered pipeline instances. */
    [[nodiscard]] foundation::Result initialize();
    /** @brief Begins a frame and pins all active pipeline plans. */
    [[nodiscard]] foundation::Result begin_frame(const EngineFrameDesc& desc = {});
    /** @brief Executes one frame phase and its automatic RenderGraph boundary work. */
    [[nodiscard]] foundation::Result execute_phase(pipeline::PhaseId phase);
    /** @brief Executes all ordinary frame phases in the registered global order. */
    [[nodiscard]] foundation::Result run_frame(const EngineFrameDesc& desc = {});
    /** @brief Executes shutdown work and deactivates registered pipelines. */
    [[nodiscard]] foundation::Result shutdown();

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool is_frame_active() const noexcept { return frame_active_; }
    [[nodiscard]] std::size_t pipeline_count() const noexcept { return pipelines_.size(); }
    [[nodiscard]] std::size_t world_count() const noexcept { return worlds_.size(); }
    [[nodiscard]] std::size_t world_frame_count() const noexcept { return world_frames_.size(); }

private:
    enum class PhaseExecutionMode : uint8_t {
        kStartup,
        kPreFrame,
        kFrame,
        kPostFrame,
        kShutdown,
    };

    struct PinnedPipeline final {
        pipeline::PipelineScopeHost* host{nullptr};
        pipeline::PipelinePlanHandle plan{};
    };

    struct WorldRegistration final {
        world::WorldScope* world{nullptr};
        bool pipeline_registered_by_orchestrator{false};
    };

    [[nodiscard]] foundation::Result validate_frame_desc(const EngineFrameDesc& desc) const;
    [[nodiscard]] foundation::Result synchronize_phase_registry();
    [[nodiscard]] foundation::Result activate_all();
    [[nodiscard]] foundation::Result deactivate_all();
    [[nodiscard]] foundation::Result execute_registered_phase(pipeline::PhaseId phase, PhaseExecutionMode mode,
                                                              bool finalize_frame);
    [[nodiscard]] foundation::Result execute_pipeline_phase(const PinnedPipeline& pinned_pipeline,
                                                            pipeline::PhaseId phase);
    [[nodiscard]] foundation::Result prepare_world_frames();
    [[nodiscard]] foundation::Result apply_world_commands();
    [[nodiscard]] foundation::Result commit_worlds();
    [[nodiscard]] foundation::Result build_render_features();
    [[nodiscard]] foundation::Result execute_render_graph();
    [[nodiscard]] foundation::Result finish_frame(bool clear_state);
    [[nodiscard]] foundation::Result fail_frame(foundation::Result result);
    [[nodiscard]] foundation::Result clear_frame_state() noexcept;
    [[nodiscard]] std::vector<pipeline::PipelineScopeHost*> ordered_pipelines() const;
    [[nodiscard]] bool contains_pipeline(const pipeline::PipelineScopeHost& host) const noexcept;
    [[nodiscard]] world::WorldScope* find_world_for_host(const pipeline::PipelineScopeHost& host) const noexcept;
    [[nodiscard]] world::WorldFrameScope* find_frame_scope_for_host(
        const pipeline::PipelineScopeHost& host) const noexcept;
    [[nodiscard]] pipeline::PipelineScopeHost* find_pipeline_registration(
        pipeline::PipelineInstanceId instance) const noexcept;
    void remove_pipeline_registration(pipeline::PipelineScopeHost& host) noexcept;
    void clear_world_frames() noexcept;
    [[nodiscard]] std::size_t phase_rank(const pipeline::PhaseRegistrySnapshot& snapshot,
                                         pipeline::PhaseId phase) const noexcept;

    pipeline::PipelineRegistry& registry_;
    pipeline::PipelineScheduler& scheduler_;
    render_graph::FrameContext& frame_context_;
    render_graph::RenderGraph& render_graph_;
    pipeline::CapabilityMask available_capabilities_{0};
    std::vector<pipeline::PipelineScopeHost*> pipelines_{};
    std::vector<WorldRegistration> worlds_{};
    std::vector<WorldFrameBinding> world_frames_{};
    std::vector<pipeline::PipelineScopeHost*> activated_pipelines_{};
    std::vector<PinnedPipeline> pinned_pipelines_{};
    struct ActiveWorldFrame final {
        WorldFrameBinding binding{};
        std::unique_ptr<world::WorldFrameScope> scope{};
        const pipeline::ArtifactScopeBridge* previous_bridge{nullptr};
    };
    std::vector<ActiveWorldFrame> active_world_frames_{};
    std::vector<render_graph::RenderGraphFeatureBufferBinding> feature_buffers_{};
    std::vector<render_graph::RenderGraphFeatureTextureBinding> feature_textures_{};
    std::vector<render_graph::RenderGraphExecutionLease> execution_leases_{};
    render_graph::RenderGraphTexture swapchain_resource_{};
    EngineFrameDesc frame_desc_{};
    uint64_t phase_revision_{0};
    std::size_t last_phase_rank_{0};
    bool has_last_phase_{false};
    bool initialized_{false};
    bool frame_active_{false};
    bool render_features_built_{false};
    bool render_graph_executed_{false};
    bool frame_presented_{false};
    bool worlds_committed_{false};
};
}  // namespace aeg::engine
