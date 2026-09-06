/**
 * @file engine_frame_orchestrator.cpp
 * @brief Implements the unified engine frame lifecycle.
 */
#include "../public/engine_frame_orchestrator.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace aeg::engine {
    namespace {
        [[nodiscard]] foundation::Result invalid_argument(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
        }

        [[nodiscard]] foundation::Result invalid_state(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState, std::move(message));
        }

        [[nodiscard]] foundation::Result not_found(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound, std::move(message));
        }

        [[nodiscard]] foundation::Result out_of_memory(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory, std::move(message));
        }

        [[nodiscard]] foundation::Result unsupported(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kUnsupported, std::move(message));
        }

        [[nodiscard]] bool is_present_status(foundation::ResultCode code) noexcept {
            return code == foundation::ResultCode::kOutOfDate ||
                   code == foundation::ResultCode::kSurfaceLost;
        }

        [[nodiscard]] bool is_builtin_phase(pipeline::PhaseId phase, pipeline::EnginePhase builtin) noexcept {
            return phase == pipeline::to_phase_id(builtin);
        }

        [[nodiscard]] foundation::Result append_failure(
                foundation::Result primary, const foundation::Result& secondary) {
            if (secondary) {
                return primary;
            }
            if (primary.message.empty()) {
                primary.message = secondary.message;
            } else if (!secondary.message.empty()) {
                primary.message += "; ";
                primary.message += secondary.message;
            }
            if (primary.code == foundation::ResultCode::kSuccess) {
                primary.code = secondary.code;
            }
            return primary;
        }
    }  // namespace

    EngineFrameOrchestrator::EngineFrameOrchestrator(
            pipeline::PipelineRegistry& registry,
            pipeline::PipelineScheduler& scheduler,
            render_graph::FrameContext& frame_context,
            render_graph::RenderGraph& render_graph,
            pipeline::CapabilityMask available_capabilities)
        : registry_(registry),
          scheduler_(scheduler),
          frame_context_(frame_context),
          render_graph_(render_graph),
          available_capabilities_(available_capabilities) {
    }

    EngineFrameOrchestrator::~EngineFrameOrchestrator() {
        if (frame_active_ || frame_context_.is_frame_active()) {
            (void) frame_context_.abort_frame();
        }
        if (initialized_ && !frame_context_.is_frame_active()) {
            (void) deactivate_all();
        }
        if (!frame_context_.is_frame_active()) {
            (void) clear_frame_state();
        }
    }

    foundation::Result EngineFrameOrchestrator::register_pipeline(pipeline::PipelineScopeHost& host) {
        if (initialized_ || frame_active_) {
            return invalid_state("cannot register a pipeline while the orchestrator is running");
        }
        if (!host.instance_id().is_valid()) {
            return invalid_argument("pipeline host instance id is invalid");
        }
        for (pipeline::PipelineScopeHost* registered_host : pipelines_) {
            if (registered_host == &host || registered_host->instance_id() == host.instance_id()) {
                return invalid_state("pipeline host instance is already registered");
            }
        }
        try {
            pipelines_.push_back(&host);
        } catch (const std::bad_alloc&) {
            return out_of_memory("pipeline host registration allocation failed");
        }
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::unregister_pipeline(pipeline::PipelineInstanceId instance) {
        if (initialized_ || frame_active_) {
            return invalid_state("cannot unregister a pipeline while the orchestrator is running");
        }
        if (!instance.is_valid()) {
            return invalid_argument("pipeline host instance id is invalid");
        }
        const auto iterator = std::find_if(
                pipelines_.begin(), pipelines_.end(),
                [instance](const pipeline::PipelineScopeHost* host) {
                    return host != nullptr && host->instance_id() == instance;
                });
        if (iterator == pipelines_.end()) {
            return not_found("pipeline host instance is not registered");
        }
        pipelines_.erase(iterator);
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::validate_frame_desc(const EngineFrameDesc& desc) const {
        if (desc.present_acquired_frame && !desc.acquire_swapchain_image) {
            return invalid_argument("present_acquired_frame requires acquire_swapchain_image");
        }
        if (!desc.acquire_swapchain_image) {
            return foundation::Result::success();
        }
        if (!desc.swapchain.is_valid()) {
            return invalid_argument("swapchain acquisition requires a valid swapchain handle");
        }
        if (!desc.swapchain_resource_id.is_valid()) {
            return invalid_argument("swapchain resource id is invalid");
        }
        return foundation::Result::success();
    }

    std::vector<pipeline::PipelineScopeHost*> EngineFrameOrchestrator::ordered_pipelines() const {
        std::vector<pipeline::PipelineScopeHost*> ordered = pipelines_;
        std::stable_sort(
                ordered.begin(), ordered.end(),
                [](const pipeline::PipelineScopeHost* left, const pipeline::PipelineScopeHost* right) {
                    if (left == nullptr || right == nullptr) {
                        return left != nullptr;
                    }
                    if (left->scope() != right->scope()) {
                        return static_cast<uint8_t>(left->scope()) < static_cast<uint8_t>(right->scope());
                    }
                    if (left->pipeline_id() != right->pipeline_id()) {
                        return left->pipeline_id() < right->pipeline_id();
                    }
                    return left->instance_id() < right->instance_id();
                });
        return ordered;
    }

    std::size_t EngineFrameOrchestrator::phase_rank(
            const pipeline::PhaseRegistrySnapshot& snapshot, pipeline::PhaseId phase) const noexcept {
        const auto iterator = std::find(
                snapshot.ordered_phases.begin(), snapshot.ordered_phases.end(), phase);
        if (iterator == snapshot.ordered_phases.end()) {
            return std::numeric_limits<std::size_t>::max();
        }
        return static_cast<std::size_t>(std::distance(snapshot.ordered_phases.begin(), iterator));
    }

    foundation::Result EngineFrameOrchestrator::synchronize_phase_registry() {
        const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
                registry_.phase_registry().snapshot();
        if (!phase_snapshot) {
            return phase_snapshot.result;
        }
        const uint64_t revision = phase_snapshot.value.revision;
        if (revision == 0 || phase_snapshot.value.ordered_phases.empty()) {
            return invalid_state("pipeline phase registry has no valid order");
        }

        std::vector<pipeline::PipelineScopeHost*> ordered;
        try {
            ordered = ordered_pipelines();
        } catch (const std::bad_alloc&) {
            return out_of_memory("pipeline host ordering allocation failed");
        }

        const bool registry_changed = phase_revision_ != revision;
        for (pipeline::PipelineScopeHost* host : ordered) {
            if (host == nullptr) {
                return invalid_state("orchestrator contains a null pipeline host");
            }

            bool rebuild_required = registry_changed;
            {
                const pipeline::PipelinePlanHandle plan = host->active_plan_snapshot();
                rebuild_required = rebuild_required || plan == nullptr || plan->phase_revision != revision;
            }
            if (rebuild_required) {
                if (const foundation::Result result = host->rebuild(registry_, available_capabilities_); !result) {
                    return result;
                }
            }
            if (host->has_pending_plan()) {
                if (const foundation::Result result = host->apply_pending_plan(); !result) {
                    return result;
                }
            }
            if (const foundation::Result result = host->collect_retired_plans(); !result) {
                return result;
            }
        }

        if (registry_.phase_registry().revision() != revision) {
            return invalid_state("pipeline phase registry changed during synchronization");
        }
        phase_revision_ = revision;
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::activate_all() {
        activated_pipelines_.clear();

        std::vector<pipeline::PipelineScopeHost*> ordered;
        try {
            ordered = ordered_pipelines();
            activated_pipelines_.reserve(ordered.size());
        } catch (const std::bad_alloc&) {
            return out_of_memory("pipeline activation ordering allocation failed");
        }

        for (pipeline::PipelineScopeHost* host : ordered) {
            if (host == nullptr) {
                return invalid_state("orchestrator contains a null pipeline host");
            }
            if (host->is_active()) {
                continue;
            }
            if (const foundation::Result result = host->activate({}); !result) {
                (void) deactivate_all();
                return result;
            }
            try {
                activated_pipelines_.push_back(host);
            } catch (const std::bad_alloc&) {
                (void) host->deactivate({});
                (void) deactivate_all();
                return out_of_memory("pipeline activation bookkeeping allocation failed");
            }
        }
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::deactivate_all() {
        foundation::Result first_error = foundation::Result::success();
        for (auto iterator = activated_pipelines_.rbegin(); iterator != activated_pipelines_.rend(); ++iterator) {
            pipeline::PipelineScopeHost* host = *iterator;
            if (host == nullptr || !host->is_active()) {
                continue;
            }
            const foundation::Result result = host->deactivate({});
            if (!result && first_error) {
                first_error = result;
            }
        }
        activated_pipelines_.clear();
        return first_error;
    }

    foundation::Result EngineFrameOrchestrator::initialize() {
        if (initialized_) {
            return invalid_state("engine frame orchestrator is already initialized");
        }
        if (frame_active_) {
            return invalid_state("cannot initialize while a frame is active");
        }

        if (const foundation::Result result = synchronize_phase_registry(); !result) {
            return result;
        }
        if (const foundation::Result result = activate_all(); !result) {
            return result;
        }

        initialized_ = true;
        const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
                registry_.phase_registry().snapshot();
        if (!phase_snapshot) {
            (void) deactivate_all();
            initialized_ = false;
            phase_revision_ = 0;
            return phase_snapshot.result;
        }
        const std::size_t startup_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kStartup));
        if (startup_rank == std::numeric_limits<std::size_t>::max()) {
            (void) deactivate_all();
            initialized_ = false;
            phase_revision_ = 0;
            return invalid_state("startup phase is not registered");
        }
        for (std::size_t rank = 0; rank <= startup_rank; ++rank) {
            const foundation::Result startup_result = execute_registered_phase(
                    phase_snapshot.value.ordered_phases[rank], PhaseExecutionMode::kStartup, false);
            if (!startup_result) {
                (void) deactivate_all();
                initialized_ = false;
                phase_revision_ = 0;
                return startup_result;
            }
        }
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::begin_frame(const EngineFrameDesc& desc) {
        if (!initialized_) {
            return invalid_state("engine frame orchestrator is not initialized");
        }
        if (frame_active_) {
            return invalid_state("an orchestrator frame is already active");
        }
        if (const foundation::Result result = validate_frame_desc(desc); !result) {
            return result;
        }
        if (const foundation::Result result = synchronize_phase_registry(); !result) {
            return result;
        }

        try {
            feature_buffers_.assign(desc.render_feature_buffers.begin(), desc.render_feature_buffers.end());
            feature_textures_.assign(desc.render_feature_textures.begin(), desc.render_feature_textures.end());
            execution_leases_.assign(desc.render_graph_execution_leases.begin(),
                                     desc.render_graph_execution_leases.end());
        } catch (const std::bad_alloc&) {
            return out_of_memory("frame descriptor copy allocation failed");
        }
        frame_desc_ = desc;
        frame_desc_.render_feature_buffers =
                std::span<const render_graph::RenderGraphFeatureBufferBinding>(
                        feature_buffers_.data(), feature_buffers_.size());
        frame_desc_.render_feature_textures =
                std::span<const render_graph::RenderGraphFeatureTextureBinding>(
                        feature_textures_.data(), feature_textures_.size());
        frame_desc_.render_graph_execution_leases =
                std::span<const render_graph::RenderGraphExecutionLease>(
                        execution_leases_.data(), execution_leases_.size());

        const foundation::Result begin_result = frame_context_.begin_frame();
        if (!begin_result) {
            (void) clear_frame_state();
            return begin_result;
        }
        frame_active_ = true;
        render_features_built_ = false;
        render_graph_executed_ = false;
        frame_presented_ = false;
        has_last_phase_ = false;
        last_phase_rank_ = 0;

        if (desc.reset_render_graph) {
            render_graph_.reset();
            swapchain_resource_ = {};
        } else {
            render_graph_.clear_passes();
        }

        try {
            if (desc.acquire_swapchain_image) {
                const foundation::ResultValue<rhi::FrameAcquireResult> acquire_result =
                        frame_context_.acquire_next_image(desc.swapchain);
                if (!acquire_result) {
                    return fail_frame(acquire_result.result);
                }

                rhi::TextureDesc swapchain_desc = desc.swapchain_image_desc;
                if (swapchain_desc.format == rhi::Format::kUnknown ||
                    swapchain_desc.extent.width == 0 || swapchain_desc.extent.height == 0) {
                    const foundation::ResultValue<rhi::SwapchainInfo> info_result =
                            frame_context_.device().get_swapchain_info(desc.swapchain);
                    if (!info_result) {
                        return fail_frame(info_result.result);
                    }
                    if (swapchain_desc.format == rhi::Format::kUnknown) {
                        swapchain_desc.format = info_result.value.format;
                    }
                    if (swapchain_desc.extent.width == 0) {
                        swapchain_desc.extent.width = info_result.value.extent.width;
                    }
                    if (swapchain_desc.extent.height == 0) {
                        swapchain_desc.extent.height = info_result.value.extent.height;
                    }
                }
                if (swapchain_desc.extent.depth == 0) {
                    swapchain_desc.extent.depth = 1;
                }
                if (swapchain_desc.mip_levels == 0) {
                    swapchain_desc.mip_levels = 1;
                }
                if (swapchain_desc.array_layers == 0) {
                    swapchain_desc.array_layers = 1;
                }
                if (swapchain_desc.capabilities == rhi::TextureCapability::kNone) {
                    swapchain_desc.capabilities = rhi::TextureCapability::kColorAttachment;
                }
                if (swapchain_desc.format == rhi::Format::kUnknown ||
                    swapchain_desc.extent.width == 0 || swapchain_desc.extent.height == 0) {
                    return fail_frame(invalid_argument("swapchain image description is incomplete"));
                }
                if (frame_desc_.swapchain_image_initial_state.usage == rhi::ResourceUsage::kUndefined) {
                    frame_desc_.swapchain_image_initial_state.usage = rhi::ResourceUsage::kPresent;
                    frame_desc_.swapchain_image_initial_state.shader_stages = rhi::ShaderStage::kAll;
                    frame_desc_.swapchain_image_initial_state.queue = rhi::QueueClass::kGraphics;
                    frame_desc_.swapchain_image_initial_state.pipeline_stages = rhi::PipelineStage::kAllCommands;
                }
                frame_desc_.swapchain_image_desc = swapchain_desc;

                if (!desc.reset_render_graph && swapchain_resource_.is_valid() &&
                    render_graph_.contains_texture(swapchain_resource_)) {
                    const rhi::FrameAcquireResult& acquired = *frame_context_.acquired_frame();
                    if (const foundation::Result result = render_graph_.update_imported_texture(
                                swapchain_resource_, acquired.image.texture, acquired.image.view,
                                frame_desc_.swapchain_image_desc,
                                frame_desc_.swapchain_image_initial_state);
                        !result) {
                        return fail_frame(result);
                    }
                } else {
                    const foundation::ResultValue<render_graph::RenderGraphTexture> import_result =
                            render_graph_.import_swapchain_image(
                                    *frame_context_.acquired_frame(), frame_desc_.swapchain_image_desc,
                                    frame_desc_.swapchain_image_initial_state);
                    if (!import_result) {
                        return fail_frame(import_result.result);
                    }
                    swapchain_resource_ = import_result.value;
                }

                const auto existing_binding = std::find_if(
                        feature_textures_.begin(), feature_textures_.end(),
                        [this](const render_graph::RenderGraphFeatureTextureBinding& binding) {
                            return binding.id == frame_desc_.swapchain_resource_id;
                        });
                if (existing_binding != feature_textures_.end()) {
                    if (existing_binding->texture != swapchain_resource_) {
                        return fail_frame(invalid_state("swapchain resource id is bound to another texture"));
                    }
                } else {
                    feature_textures_.push_back({frame_desc_.swapchain_resource_id, swapchain_resource_});
                    frame_desc_.render_feature_textures =
                            std::span<const render_graph::RenderGraphFeatureTextureBinding>(
                                    feature_textures_.data(), feature_textures_.size());
                }
            }

            std::vector<pipeline::PipelineScopeHost*> ordered = ordered_pipelines();
            pinned_pipelines_.reserve(ordered.size());
            for (pipeline::PipelineScopeHost* host : ordered) {
                if (host == nullptr || !host->is_active()) {
                    return fail_frame(invalid_state("all registered pipeline hosts must be active"));
                }
                pipeline::PipelinePlanHandle plan = host->active_plan_snapshot();
                if (plan == nullptr || plan->phase_revision != phase_revision_) {
                    return fail_frame(invalid_state("pipeline plan is missing or uses a stale phase registry"));
                }
                pinned_pipelines_.push_back(PinnedPipeline{host, std::move(plan)});
            }
        } catch (const std::bad_alloc&) {
            return fail_frame(out_of_memory("frame pipeline pinning allocation failed"));
        }

        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::execute_pipeline_phase(
            const PinnedPipeline& pinned_pipeline, pipeline::PhaseId phase) {
        if (pinned_pipeline.host == nullptr || pinned_pipeline.plan == nullptr) {
            return invalid_state("pinned pipeline is incomplete");
        }
        pipeline::PipelineExecutionContext context(
                frame_desc_.user_data, pinned_pipeline.host->scope(), &pinned_pipeline.host->artifacts());
        return pinned_pipeline.host->execute_phase(
                scheduler_, context, phase, pinned_pipeline.plan);
    }

    foundation::Result EngineFrameOrchestrator::build_render_features() {
        if (render_features_built_) {
            return foundation::Result::success();
        }
        const render_graph::RenderGraphFeatureRuntimeDesc desc{
                std::span<const render_graph::RenderGraphFeatureBufferBinding>(
                        feature_buffers_.data(), feature_buffers_.size()),
                std::span<const render_graph::RenderGraphFeatureTextureBinding>(
                        feature_textures_.data(), feature_textures_.size()),
                frame_desc_.user_data};
        try {
            for (const PinnedPipeline& pinned_pipeline : pinned_pipelines_) {
                pipeline::PipelinePlanHandle plan = pinned_pipeline.plan;
                if (const foundation::Result result = render_graph::build_render_graph_features(
                            render_graph_, std::move(plan), registry_, desc);
                    !result) {
                    return result;
                }
            }
        } catch (const std::bad_alloc&) {
            return out_of_memory("render feature graph build allocation failed");
        } catch (const std::exception& exception) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
        } catch (...) {
            return foundation::Result::failure(
                    foundation::ResultCode::kUnknown, "render feature graph build threw an unknown exception");
        }
        render_features_built_ = true;
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::execute_render_graph() {
        if (render_graph_executed_) {
            return foundation::Result::success();
        }
        render_graph::RenderGraphExecuteDesc desc{};
        desc.present_acquired_frame = false;
        desc.execution_leases = std::span<const render_graph::RenderGraphExecutionLease>(
                execution_leases_.data(), execution_leases_.size());
        const foundation::ResultValue<render_graph::RenderGraphExecutionResult> execution =
                render_graph_.execute(frame_context_, desc);
        if (!execution) {
            return execution.result;
        }
        render_graph_executed_ = true;
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::execute_registered_phase(
            pipeline::PhaseId phase, PhaseExecutionMode mode, bool finalize_frame) {
        if (!initialized_) {
            return invalid_state("engine frame orchestrator is not initialized");
        }
        if (!phase.is_valid()) {
            return invalid_argument("phase id is invalid");
        }
        const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
                registry_.phase_registry().snapshot();
        if (!phase_snapshot) {
            return phase_snapshot.result;
        }
        if (phase_snapshot.value.revision == 0 || phase_snapshot.value.revision != phase_revision_) {
            return invalid_state("phase registry changed outside a frame boundary");
        }
        const std::size_t current_rank = phase_rank(phase_snapshot.value, phase);
        if (current_rank == std::numeric_limits<std::size_t>::max()) {
            return not_found("phase is not registered");
        }

        const std::size_t startup_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kStartup));
        const std::size_t frame_begin_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kFrameBegin));
        const std::size_t frame_end_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kFrameEnd));
        const std::size_t shutdown_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kShutdown));
        if (startup_rank == std::numeric_limits<std::size_t>::max() ||
            frame_begin_rank == std::numeric_limits<std::size_t>::max() ||
            frame_end_rank == std::numeric_limits<std::size_t>::max() ||
            shutdown_rank == std::numeric_limits<std::size_t>::max() ||
            startup_rank >= frame_begin_rank || frame_begin_rank > frame_end_rank ||
            frame_end_rank >= shutdown_rank) {
            return invalid_state("built-in phase order is invalid");
        }

        switch (mode) {
            case PhaseExecutionMode::kStartup:
                if (frame_active_) {
                    return invalid_state("startup phases cannot execute during an active frame");
                }
                if (current_rank > startup_rank) {
                    return unsupported("phase is outside the startup interval");
                }
                break;
            case PhaseExecutionMode::kPreFrame:
                if (!frame_active_) {
                    return invalid_state("pre-frame phase requires an active frame");
                }
                if (current_rank <= startup_rank || current_rank >= frame_begin_rank) {
                    return unsupported("phase is outside the pre-frame interval");
                }
                break;
            case PhaseExecutionMode::kFrame:
                if (!frame_active_) {
                    return invalid_state("frame phase requires an active frame");
                }
                if (current_rank < frame_begin_rank || current_rank > frame_end_rank) {
                    return unsupported("phase is outside the active frame interval");
                }
                break;
            case PhaseExecutionMode::kPostFrame:
                if (frame_active_) {
                    return invalid_state("post-frame phases require an inactive frame");
                }
                if (current_rank <= frame_end_rank || current_rank >= shutdown_rank) {
                    return unsupported("phase is outside the post-frame interval");
                }
                break;
            case PhaseExecutionMode::kShutdown:
                if (frame_active_) {
                    return invalid_state("shutdown phases cannot execute during an active frame");
                }
                if (current_rank < shutdown_rank) {
                    return unsupported("phase is outside the shutdown interval");
                }
                break;
        }

        if (finalize_frame &&
            (mode != PhaseExecutionMode::kFrame ||
             !is_builtin_phase(phase, pipeline::EnginePhase::kFrameEnd))) {
            return invalid_argument("only FrameEnd can finalize an orchestrator frame");
        }

        if (mode == PhaseExecutionMode::kPreFrame || mode == PhaseExecutionMode::kFrame) {
            if (has_last_phase_ && current_rank < last_phase_rank_) {
                return invalid_state("frame phases must execute in global registry order");
            }
            has_last_phase_ = true;
            last_phase_rank_ = current_rank;
        }

        if (frame_active_) {
            for (const PinnedPipeline& pinned_pipeline : pinned_pipelines_) {
                if (const foundation::Result result = execute_pipeline_phase(pinned_pipeline, phase); !result) {
                    return result;
                }
            }
        } else {
            std::vector<pipeline::PipelineScopeHost*> ordered;
            try {
                ordered = ordered_pipelines();
            } catch (const std::bad_alloc&) {
                return out_of_memory("lifecycle pipeline ordering allocation failed");
            }
            for (pipeline::PipelineScopeHost* host : ordered) {
                if (host == nullptr) {
                    return invalid_state("orchestrator contains a null pipeline host");
                }
                const PinnedPipeline pinned_pipeline{host, host->active_plan_snapshot()};
                if (const foundation::Result result = execute_pipeline_phase(pinned_pipeline, phase); !result) {
                    return result;
                }
            }
        }

        if (mode == PhaseExecutionMode::kFrame &&
            is_builtin_phase(phase, pipeline::EnginePhase::kRenderBuild)) {
            if (const foundation::Result result = build_render_features(); !result) {
                return result;
            }
        } else if (mode == PhaseExecutionMode::kFrame &&
                   is_builtin_phase(phase, pipeline::EnginePhase::kRenderExecute)) {
            if (const foundation::Result result = execute_render_graph(); !result) {
                return result;
            }
        } else if (mode == PhaseExecutionMode::kFrame &&
                   is_builtin_phase(phase, pipeline::EnginePhase::kPresent) &&
                   frame_desc_.present_acquired_frame && frame_context_.acquired_frame() != nullptr) {
            const foundation::ResultValue<rhi::PresentResult> present_result = frame_context_.present();
            if (!present_result) {
                return present_result.result;
            }
            frame_presented_ = true;
        }

        if (registry_.phase_registry().revision() != phase_revision_) {
            return invalid_state("phase registry changed during phase execution");
        }

        if (finalize_frame) {
            return finish_frame(true);
        }
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::execute_phase(pipeline::PhaseId phase) {
        const bool finalize_frame = is_builtin_phase(phase, pipeline::EnginePhase::kFrameEnd);
        const foundation::Result result = execute_registered_phase(
                phase, PhaseExecutionMode::kFrame, finalize_frame);
        if (!result && frame_active_) {
            return fail_frame(result);
        }
        return result;
    }

    foundation::Result EngineFrameOrchestrator::finish_frame(bool clear_state) {
        if (!frame_active_) {
            return invalid_state("no orchestrator frame is active");
        }

        foundation::Result presentation_status = foundation::Result::success();
        if (frame_context_.acquired_frame() != nullptr) {
            if (frame_desc_.present_acquired_frame) {
                const foundation::ResultValue<rhi::PresentResult> present_result = frame_context_.present();
                if (!present_result) {
                    if (is_present_status(present_result.result.code)) {
                        presentation_status = present_result.result;
                        frame_presented_ = true;
                    } else {
                        return fail_frame(present_result.result);
                    }
                } else {
                    frame_presented_ = true;
                }
            } else {
                const foundation::Result cancel_result = frame_context_.cancel_frame();
                if (!cancel_result) {
                    if (cancel_result.code != foundation::ResultCode::kInvalidState) {
                        return fail_frame(cancel_result);
                    }
                    const foundation::ResultValue<rhi::PresentResult> present_result = frame_context_.present();
                    if (!present_result) {
                        if (is_present_status(present_result.result.code)) {
                            presentation_status = present_result.result;
                            frame_presented_ = true;
                        } else {
                            return fail_frame(present_result.result);
                        }
                    } else {
                        frame_presented_ = true;
                    }
                }
            }
        }

        const foundation::Result end_result = frame_context_.end_frame();
        if (!end_result) {
            return fail_frame(end_result);
        }

        frame_active_ = false;
        if (clear_state) {
            (void) clear_frame_state();
        }
        foundation::Result retirement_error = foundation::Result::success();
        for (pipeline::PipelineScopeHost* host : pipelines_) {
            if (host == nullptr) {
                continue;
            }
            const foundation::Result result = host->collect_retired_plans();
            if (!result && retirement_error) {
                retirement_error = result;
            }
        }
        if (!retirement_error) {
            return retirement_error;
        }
        return presentation_status;
    }

    foundation::Result EngineFrameOrchestrator::fail_frame(foundation::Result result) {
        if (frame_active_ || frame_context_.is_frame_active()) {
            const foundation::Result abort_result = frame_context_.abort_frame();
            result = append_failure(std::move(result), abort_result);
        }
        if (!frame_context_.is_frame_active()) {
            (void) clear_frame_state();
        } else {
            frame_active_ = true;
        }
        return result;
    }

    foundation::Result EngineFrameOrchestrator::clear_frame_state() noexcept {
        const bool preserve_swapchain_resource = !frame_desc_.reset_render_graph;
        pinned_pipelines_.clear();
        feature_buffers_.clear();
        feature_textures_.clear();
        execution_leases_.clear();
        if (!preserve_swapchain_resource) {
            swapchain_resource_ = {};
        }
        frame_desc_ = {};
        last_phase_rank_ = 0;
        has_last_phase_ = false;
        frame_active_ = false;
        render_features_built_ = false;
        render_graph_executed_ = false;
        frame_presented_ = false;
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::run_frame(const EngineFrameDesc& desc) {
        if (const foundation::Result result = begin_frame(desc); !result) {
            return result;
        }

        const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
                registry_.phase_registry().snapshot();
        if (!phase_snapshot) {
            return fail_frame(phase_snapshot.result);
        }
        if (phase_snapshot.value.revision != phase_revision_) {
            return fail_frame(invalid_state("phase registry changed after frame synchronization"));
        }
        const std::size_t frame_begin_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kFrameBegin));
        const std::size_t frame_end_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kFrameEnd));
        const std::size_t shutdown_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kShutdown));
        const std::size_t startup_rank = phase_rank(
                phase_snapshot.value,
                pipeline::to_phase_id(pipeline::EnginePhase::kStartup));
        if (startup_rank == std::numeric_limits<std::size_t>::max() ||
            frame_begin_rank == std::numeric_limits<std::size_t>::max() ||
            frame_end_rank == std::numeric_limits<std::size_t>::max() ||
            shutdown_rank == std::numeric_limits<std::size_t>::max() ||
            startup_rank >= frame_begin_rank || frame_begin_rank > frame_end_rank ||
            frame_end_rank >= shutdown_rank) {
            return fail_frame(invalid_state("ordinary frame interval is not registered"));
        }

        try {
            for (std::size_t rank = startup_rank + 1; rank < frame_begin_rank; ++rank) {
                const foundation::Result result = execute_registered_phase(
                        phase_snapshot.value.ordered_phases[rank], PhaseExecutionMode::kPreFrame, false);
                if (!result) {
                    return fail_frame(result);
                }
            }

            for (std::size_t rank = frame_begin_rank; rank <= frame_end_rank; ++rank) {
                const foundation::Result result = execute_registered_phase(
                        phase_snapshot.value.ordered_phases[rank], PhaseExecutionMode::kFrame, false);
                if (!result) {
                    return fail_frame(result);
                }
            }

            if (frame_active_) {
                if (const foundation::Result result = finish_frame(false); !result) {
                    if (!frame_active_) {
                        (void) clear_frame_state();
                    }
                    return result;
                }
            }

            foundation::Result post_frame_result = foundation::Result::success();
            for (std::size_t rank = frame_end_rank + 1; rank < shutdown_rank; ++rank) {
                const foundation::Result result = execute_registered_phase(
                        phase_snapshot.value.ordered_phases[rank], PhaseExecutionMode::kPostFrame, false);
                if (!result) {
                    post_frame_result = result;
                    break;
                }
            }

            (void) clear_frame_state();
            return post_frame_result;
        } catch (const std::bad_alloc&) {
            const foundation::Result result = out_of_memory("frame phase execution allocation failed");
            if (frame_active_) {
                return fail_frame(result);
            }
            (void) clear_frame_state();
            return result;
        } catch (const std::exception& exception) {
            const foundation::Result result = foundation::Result::failure(
                    foundation::ResultCode::kUnknown, exception.what());
            if (frame_active_) {
                return fail_frame(result);
            }
            (void) clear_frame_state();
            return result;
        } catch (...) {
            const foundation::Result result = foundation::Result::failure(
                    foundation::ResultCode::kUnknown,
                    "frame phase execution threw an unknown exception");
            if (frame_active_) {
                return fail_frame(result);
            }
            (void) clear_frame_state();
            return result;
        }
        return foundation::Result::success();
    }

    foundation::Result EngineFrameOrchestrator::shutdown() {
        if (!initialized_) {
            return invalid_state("engine frame orchestrator is not initialized");
        }

        foundation::Result first_error = foundation::Result::success();
        if (frame_active_ || frame_context_.is_frame_active()) {
            const foundation::Result abort_result = frame_context_.abort_frame();
            if (!abort_result && first_error) {
                first_error = abort_result;
            }
            if (frame_context_.is_frame_active()) {
                if (first_error) {
                    first_error = invalid_state("cannot shut down while the active frame remains unresolved");
                }
                return first_error;
            }
            (void) clear_frame_state();
        }

        const foundation::Result synchronize_result = synchronize_phase_registry();
        if (!synchronize_result) {
            if (first_error) {
                first_error = synchronize_result;
            }
        } else {
            const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
                    registry_.phase_registry().snapshot();
            if (!phase_snapshot) {
                if (first_error) {
                    first_error = phase_snapshot.result;
                }
            } else {
                const std::size_t shutdown_rank = phase_rank(
                        phase_snapshot.value,
                        pipeline::to_phase_id(pipeline::EnginePhase::kShutdown));
                if (shutdown_rank == std::numeric_limits<std::size_t>::max()) {
                    if (first_error) {
                        first_error = invalid_state("shutdown phase is not registered");
                    }
                } else {
                    for (std::size_t rank = shutdown_rank;
                         rank < phase_snapshot.value.ordered_phases.size();
                         ++rank) {
                        const foundation::Result shutdown_result = execute_registered_phase(
                                phase_snapshot.value.ordered_phases[rank],
                                PhaseExecutionMode::kShutdown,
                                false);
                        if (!shutdown_result) {
                            if (first_error) {
                                first_error = shutdown_result;
                            }
                            break;
                        }
                    }
                }
            }
        }

        const foundation::Result deactivate_result = deactivate_all();
        if (!deactivate_result && first_error) {
            first_error = deactivate_result;
        }
        initialized_ = false;
        phase_revision_ = 0;
        (void) clear_frame_state();
        return first_error;
    }
}  // namespace aeg::engine
