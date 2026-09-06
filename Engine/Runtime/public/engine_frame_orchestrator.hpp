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

namespace aeg::engine {
    /** @brief Conventional RenderGraph resource identity for the acquired swapchain image. */
    inline constexpr pipeline::RenderResourceId kSwapchainImageResourceId{"SwapchainImage"};

    /** @brief Per-frame inputs consumed by EngineFrameOrchestrator. */
    struct EngineFrameDesc final {
        bool reset_render_graph{true};
        bool acquire_swapchain_image{false};
        rhi::SwapchainHandle swapchain{};
        rhi::TextureDesc swapchain_image_desc{};
        rhi::ResourceState swapchain_image_initial_state{
            rhi::ResourceUsage::kPresent,
            rhi::ShaderStage::kAll,
            rhi::QueueClass::kGraphics,
            false,
            rhi::PipelineStage::kAllCommands,
        };
        pipeline::RenderResourceId swapchain_resource_id{kSwapchainImageResourceId};
        bool present_acquired_frame{false};
        std::span<const render_graph::RenderGraphFeatureBufferBinding> render_feature_buffers{};
        std::span<const render_graph::RenderGraphFeatureTextureBinding> render_feature_textures{};
        std::span<const render_graph::RenderGraphExecutionLease> render_graph_execution_leases{};
        void* user_data{nullptr};
    };

    /** @brief Drives registered pipeline instances and the frame-local RenderGraph. */
    class EngineFrameOrchestrator final {
    public:
        EngineFrameOrchestrator(
            pipeline::PipelineRegistry& registry,
            pipeline::PipelineScheduler& scheduler,
            render_graph::FrameContext& frame_context,
            render_graph::RenderGraph& render_graph,
            pipeline::CapabilityMask available_capabilities = 0);
        ~EngineFrameOrchestrator();

        EngineFrameOrchestrator(const EngineFrameOrchestrator&) = delete;
        EngineFrameOrchestrator& operator=(const EngineFrameOrchestrator&) = delete;
        EngineFrameOrchestrator(EngineFrameOrchestrator&&) = delete;
        EngineFrameOrchestrator& operator=(EngineFrameOrchestrator&&) = delete;

        /** @brief Registers a non-owning pipeline scope host. */
        [[nodiscard]] foundation::Result register_pipeline(pipeline::PipelineScopeHost& host);
        /** @brief Unregisters a pipeline scope host by instance identity. */
        [[nodiscard]] foundation::Result unregister_pipeline(pipeline::PipelineInstanceId instance);

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

        [[nodiscard]] foundation::Result validate_frame_desc(const EngineFrameDesc& desc) const;
        [[nodiscard]] foundation::Result synchronize_phase_registry();
        [[nodiscard]] foundation::Result activate_all();
        [[nodiscard]] foundation::Result deactivate_all();
        [[nodiscard]] foundation::Result execute_registered_phase(
            pipeline::PhaseId phase, PhaseExecutionMode mode, bool finalize_frame);
        [[nodiscard]] foundation::Result execute_pipeline_phase(
            const PinnedPipeline& pinned_pipeline, pipeline::PhaseId phase);
        [[nodiscard]] foundation::Result build_render_features();
        [[nodiscard]] foundation::Result execute_render_graph();
        [[nodiscard]] foundation::Result finish_frame(bool clear_state);
        [[nodiscard]] foundation::Result fail_frame(foundation::Result result);
        [[nodiscard]] foundation::Result clear_frame_state() noexcept;
        [[nodiscard]] std::vector<pipeline::PipelineScopeHost*> ordered_pipelines() const;
        [[nodiscard]] std::size_t phase_rank(
                const pipeline::PhaseRegistrySnapshot& snapshot, pipeline::PhaseId phase) const noexcept;

        pipeline::PipelineRegistry& registry_;
        pipeline::PipelineScheduler& scheduler_;
        render_graph::FrameContext& frame_context_;
        render_graph::RenderGraph& render_graph_;
        pipeline::CapabilityMask available_capabilities_{0};
        std::vector<pipeline::PipelineScopeHost*> pipelines_{};
        std::vector<pipeline::PipelineScopeHost*> activated_pipelines_{};
        std::vector<PinnedPipeline> pinned_pipelines_{};
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
    };
}  // namespace aeg::engine
