/**
 * @file engine_frame_orchestrator.cpp
 * @brief Implements the unified engine frame lifecycle.
 */
#include "../public/engine_frame_orchestrator.hpp"
#include "../../RenderGraph/private/render_graph_resource_access.hpp"
#include "../../Pipeline/private/phase_registry_execution.hpp"
#include "../../Pipeline/private/pipeline_scope_host_execution.hpp"
#include "world_snapshot_execution.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include "engine_frame_inspection.hpp"
#include "engine_frame_world_commands.hpp"
#include "feature_world_commands.hpp"
#include "renderer_frame_contract.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace aeg::engine {
struct EngineFrameOrchestrator::Impl final {
    Impl(EngineFrameOrchestrator& owner, const EngineFramePipelineHost& pipeline_host,
         render_graph::FrameContext& frame_context, render_graph::RenderGraph& render_graph,
         uint64_t available_capabilities);
    ~Impl();
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



    EngineFrameOrchestrator& owner_;
    detail::FeatureWorldCommandConsumer* world_commands_{nullptr};

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
    };

    /** @brief Internal rollback record for one registered World view. */
    struct WorldFrameBinding final {
        world::WorldScope* world{nullptr};
        pipeline::PipelineScopeHost* frame_host{nullptr};
        uint32_t view_id{0};
        bool frame_host_registered_by_orchestrator{false};
    };

    [[nodiscard]] foundation::Result validate_frame_desc(const EngineFrameDesc& desc) const;
    [[nodiscard]] foundation::Result register_pipeline_typed(pipeline::PipelineScopeHost& host);
    [[nodiscard]] foundation::Result unregister_pipeline(pipeline::PipelineInstanceId instance);
    [[nodiscard]] foundation::Result register_world_frame_impl(world::WorldScope& world_scope,
        pipeline::PipelineScopeHost* frame_host, uint32_t view_id);
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
    [[nodiscard]] foundation::Result extract_render_scenes();
    [[nodiscard]] foundation::Result bind_feature_texture(feature::RenderResourceId id,
                                                          render_graph::RenderGraphTexture texture);
    [[nodiscard]] foundation::Result post_build_render_features();
    [[nodiscard]] foundation::Result execute_render_graph();
    [[nodiscard]] foundation::Result release_frame_artifacts();
    [[nodiscard]] foundation::Result finish_frame(bool clear_state);
    [[nodiscard]] foundation::Result fail_frame(foundation::Result result);
    [[nodiscard]] foundation::Result clear_frame_state() noexcept;
    [[nodiscard]] std::vector<pipeline::PipelineScopeHost*> ordered_pipelines() const;
    [[nodiscard]] bool contains_pipeline(const pipeline::PipelineScopeHost& host) const noexcept;
    [[nodiscard]] world::WorldFrameScope* find_frame_scope_for_host(
        const pipeline::PipelineScopeHost& host) const noexcept;
    [[nodiscard]] pipeline::PipelineScopeHost* find_pipeline_registration(
        pipeline::PipelineInstanceId instance) const noexcept;
    void remove_pipeline_registration(pipeline::PipelineScopeHost& host) noexcept;
    void clear_world_frames() noexcept;
    [[nodiscard]] std::size_t phase_rank(const pipeline::PhaseRegistrySnapshot& snapshot,
                                         pipeline::PhaseId phase) const noexcept;

    const EngineFramePipelineHost* pipeline_host_{nullptr};
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
    std::vector<render_graph::RenderGraphFeatureTextureBinding> feature_textures_{};
    std::vector<render_graph::RenderGraphExecutionLease> execution_leases_{};
    std::vector<renderer::RenderView> render_views_{};
    std::vector<std::shared_ptr<const renderer::RenderSceneSnapshot>> render_scene_snapshots_{};
    render_graph::RenderGraphTexture swapchain_resource_{};
    EngineFrameDesc frame_desc_{};
    uint64_t phase_revision_{0};
    std::size_t last_phase_rank_{0};
    bool has_last_phase_{false};
    bool initialized_{false};
    bool frame_active_{false};
    bool render_feature_prepared_{false};
    bool render_feature_post_built_{false};
    bool render_graph_executed_{false};
    bool frame_presented_{false};
    bool worlds_committed_{false};
    uint32_t draw_call_count_{0};
    bool inspection_enabled_{false};
    detail::EngineFrameInspectionTrace inspection_trace_{};

};

namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::out_of_memory;
using foundation::unsupported;

[[nodiscard]] bool is_present_status(foundation::ResultCode code) noexcept {
    return code == foundation::ResultCode::kOutOfDate || code == foundation::ResultCode::kSurfaceLost;
}

[[nodiscard]] bool is_builtin_phase(pipeline::PhaseId phase, pipeline::EnginePhase builtin) noexcept {
    return phase == pipeline::to_phase_id(builtin);
}

[[nodiscard]] foundation::Result append_failure(foundation::Result primary, const foundation::Result& secondary) {
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

EngineFramePreparationContext::EngineFramePreparationContext(render_graph::FrameContext& frame_context,
                                                             render_graph::RenderGraph& render_graph,
                                                             const rhi::FrameAcquireResult* acquired_frame,
                                                             render_graph::RenderGraphTexture swapchain_resource,
                                                             std::span<const std::shared_ptr<const renderer::RenderSceneSnapshot>> scenes,
                                                             EngineFrameOrchestrator& owner) noexcept
    : frame_context(frame_context),
      render_graph(render_graph),
      acquired_frame(acquired_frame),
      swapchain_resource(swapchain_resource),
      render_scenes(scenes),
      owner_(&owner) {}

foundation::Result EngineFramePreparationContext::bind_feature_texture(feature::RenderResourceId id,
                                                                       render_graph::RenderGraphTexture texture) const {
    if (owner_ == nullptr) {
        return invalid_state("render feature preparation context is no longer active");
    }
    return owner_->bind_feature_texture(id, texture);
}

EngineFrameOrchestrator::Impl::Impl(EngineFrameOrchestrator& owner, const EngineFramePipelineHost& pipeline_host,
                                                 render_graph::FrameContext& frame_context,
                                                 render_graph::RenderGraph& render_graph,
                                                 pipeline::CapabilityMask available_capabilities)
    : owner_(owner), pipeline_host_(&pipeline_host),
      frame_context_(frame_context),
      render_graph_(render_graph),
      available_capabilities_(available_capabilities) {}

EngineFrameOrchestrator::Impl::~Impl() {
    if (frame_active_ || frame_context_.is_frame_active()) {
        (void)frame_context_.abort_frame();
    }
    (void)release_frame_artifacts();
    if (initialized_ && !frame_context_.is_frame_active()) {
        (void)deactivate_all();
    }
    if (!frame_context_.is_frame_active()) {
        (void)clear_frame_state();
    }
}

foundation::Result EngineFrameOrchestrator::Impl::register_pipeline(
    const EngineFramePipelineRegistration& registration) {
    return register_pipeline_typed(registration.host());
}

foundation::Result EngineFrameOrchestrator::Impl::register_pipeline_typed(pipeline::PipelineScopeHost& host) {
    if (initialized_ || frame_active_) {
        return invalid_state("cannot register a pipeline while the orchestrator is running");
    }
    if (host.scope() == pipeline::PipelineScope::kWorld) {
        return unsupported("Raw World Pipeline execution is retired; register the World aggregate");
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

bool EngineFrameOrchestrator::Impl::contains_pipeline(const pipeline::PipelineScopeHost& host) const noexcept {
    return std::find(pipelines_.begin(), pipelines_.end(), &host) != pipelines_.end();
}

pipeline::PipelineScopeHost* EngineFrameOrchestrator::Impl::find_pipeline_registration(
    pipeline::PipelineInstanceId instance) const noexcept {
    const auto iterator =
        std::find_if(pipelines_.begin(), pipelines_.end(), [instance](const pipeline::PipelineScopeHost* host) {
            return host != nullptr && host->instance_id() == instance;
        });
    return iterator == pipelines_.end() ? nullptr : *iterator;
}

void EngineFrameOrchestrator::Impl::remove_pipeline_registration(pipeline::PipelineScopeHost& host) noexcept {
    const auto iterator = std::find(pipelines_.begin(), pipelines_.end(), &host);
    if (iterator != pipelines_.end()) {
        pipelines_.erase(iterator);
    }
}

foundation::Result EngineFrameOrchestrator::Impl::register_world(world::WorldScope& world_scope) {
    if (initialized_ || frame_active_) {
        return invalid_state("cannot register a World while the orchestrator is running");
    }
    if (!world_scope.handle().is_valid()) {
        return invalid_argument("World Scope registration is invalid");
    }
    if (!world_scope.is_initialized()) {
        return invalid_state("World Scope must be initialized before registration");
    }
    for (const WorldRegistration& registration : worlds_) {
        if (registration.world == &world_scope ||
            (registration.world != nullptr && registration.world->handle() == world_scope.handle())) {
            return invalid_state("World Scope is already registered");
        }
    }

    try {
        worlds_.push_back(WorldRegistration{&world_scope});
    } catch (const std::bad_alloc&) {
        return out_of_memory("World Scope registration allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::register_world_frame(world::WorldScope& world_scope,
                                                                 const EngineFramePipelineRegistration& registration,
                                                                 uint32_t view_id) {
    return register_world_frame_impl(world_scope, &registration.host(), view_id);
}

foundation::Result EngineFrameOrchestrator::Impl::register_world_frame(world::WorldScope& world_scope, uint32_t view_id) {
    return register_world_frame_impl(world_scope, nullptr, view_id);
}

foundation::Result EngineFrameOrchestrator::Impl::register_world_frame_impl(world::WorldScope& world_scope,
    pipeline::PipelineScopeHost* frame_host, uint32_t view_id) {
    if (initialized_ || frame_active_) {
        return invalid_state("cannot register a World view while the orchestrator is running");
    }
    if (frame_host && frame_host->scope() != pipeline::PipelineScope::kFrame) {
        return invalid_argument("World view requires a Frame pipeline host");
    }
    for (const WorldFrameBinding& binding : world_frames_) {
        if (binding.world == &world_scope && binding.view_id == view_id) {
            return invalid_state("World view is already registered");
        }
        if (frame_host && binding.frame_host == frame_host) {
            return invalid_state("Frame pipeline host is already bound to a World view");
        }
    }

    bool world_registered = false;
    for (const WorldRegistration& registration : worlds_) {
        if (registration.world == &world_scope) {
            world_registered = true;
            break;
        }
    }
    bool world_registered_here = false;
    if (!world_registered) {
        if (const foundation::Result result = register_world(world_scope); !result) {
            return result;
        }
        world_registered_here = true;
    }

    const bool pipeline_registered = !frame_host || contains_pipeline(*frame_host);
    if (!pipeline_registered) {
        if (const foundation::Result result = register_pipeline_typed(*frame_host); !result) {
            if (world_registered_here) {
                static_cast<void>(unregister_world(world_scope.handle()));
            }
            return result;
        }
    }
    try {
        world_frames_.push_back(WorldFrameBinding{&world_scope, frame_host, view_id, !pipeline_registered});
    } catch (const std::bad_alloc&) {
        if (!pipeline_registered) {
            static_cast<void>(unregister_pipeline(frame_host->instance_id()));
        }
        if (world_registered_here) {
            static_cast<void>(unregister_world(world_scope.handle()));
        }
        return out_of_memory("World view registration allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::unregister_pipeline(pipeline::PipelineInstanceId instance) {
    if (initialized_ || frame_active_) {
        return invalid_state("cannot unregister a pipeline while the orchestrator is running");
    }
    if (!instance.is_valid()) {
        return invalid_argument("pipeline host instance id is invalid");
    }
    pipeline::PipelineScopeHost* host = find_pipeline_registration(instance);
    if (host == nullptr) {
        return not_found("pipeline host instance is not registered");
    }
    for (const WorldFrameBinding& binding : world_frames_) {
        if (binding.frame_host == host) {
            return invalid_state("Frame pipeline host is still bound to a World view");
        }
    }
    remove_pipeline_registration(*host);
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::unregister_world_frame(ecs::WorldHandle world_handle, uint32_t view_id) {
    if (initialized_ || frame_active_) {
        return invalid_state("cannot unregister a World view while the orchestrator is running");
    }
    const auto iterator = std::find_if(
        world_frames_.begin(), world_frames_.end(), [world_handle, view_id](const WorldFrameBinding& binding) {
            return binding.world != nullptr && binding.world->handle() == world_handle && binding.view_id == view_id;
        });
    if (iterator == world_frames_.end()) {
        return not_found("World view is not registered");
    }
    const WorldFrameBinding binding = *iterator;
    const bool remove_frame_host = binding.frame_host_registered_by_orchestrator && binding.frame_host != nullptr;
    if (remove_frame_host) {
        pipeline::PipelineScopeHost* registered_host = find_pipeline_registration(binding.frame_host->instance_id());
        if (registered_host != binding.frame_host) {
            return invalid_state("World view registration references an inconsistent Frame pipeline host");
        }
        if (std::any_of(world_frames_.begin(), world_frames_.end(), [&binding](const WorldFrameBinding& other) {
                return other.frame_host == binding.frame_host &&
                       (other.world != binding.world || other.view_id != binding.view_id);
            })) {
            return invalid_state("Frame pipeline host is still bound to another World view");
        }
    }
    world_frames_.erase(iterator);
    if (remove_frame_host) {
        remove_pipeline_registration(*binding.frame_host);
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::unregister_world(ecs::WorldHandle world_handle) {
    if (initialized_ || frame_active_) {
        return invalid_state("cannot unregister a World while the orchestrator is running");
    }
    const auto world_iterator =
        std::find_if(worlds_.begin(), worlds_.end(), [world_handle](const WorldRegistration& registration) {
            return registration.world != nullptr && registration.world->handle() == world_handle;
        });
    if (world_iterator == worlds_.end()) {
        return not_found("World Scope is not registered");
    }
    const WorldRegistration registration = *world_iterator;
    world::WorldScope* world_scope = registration.world;
    const auto frame_iterator =
        std::find_if(world_frames_.begin(), world_frames_.end(),
                     [world_scope](const WorldFrameBinding& binding) { return binding.world == world_scope; });
    if (frame_iterator != world_frames_.end()) {
        return invalid_state("World still has registered Frame views");
    }
    worlds_.erase(world_iterator);
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::validate_frame_desc(const EngineFrameDesc& desc) const {
    if (!desc.render_output_extent.is_valid()) {
        return invalid_argument("render output extent is invalid");
    }
    if (desc.render_view_resolver && desc.renderer == nullptr) {
        return invalid_argument("render view resolver requires a Renderer instance");
    }
    if (desc.render_view_resolver && !desc.render_views.empty()) {
        return invalid_argument("render view resolver cannot be combined with explicit render views");
    }
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

std::vector<pipeline::PipelineScopeHost*> EngineFrameOrchestrator::Impl::ordered_pipelines() const {
    std::vector<pipeline::PipelineScopeHost*> ordered = pipelines_;
    std::stable_sort(ordered.begin(), ordered.end(),
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

std::size_t EngineFrameOrchestrator::Impl::phase_rank(const pipeline::PhaseRegistrySnapshot& snapshot,
                                                pipeline::PhaseId phase) const noexcept {
    const auto iterator = std::find(snapshot.ordered_phases.begin(), snapshot.ordered_phases.end(), phase);
    if (iterator == snapshot.ordered_phases.end()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(std::distance(snapshot.ordered_phases.begin(), iterator));
}

foundation::Result EngineFrameOrchestrator::Impl::synchronize_phase_registry() {
    const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
        pipeline::detail::PhaseRegistryExecution::snapshot(pipeline_host_->registry().phase_registry());
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
            const pipeline::PipelinePlanHandle plan =
                pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(*host);
            rebuild_required = rebuild_required || plan == nullptr || plan->phase_revision != revision;
        }
        if (rebuild_required) {
            if (const foundation::Result result = host->rebuild(pipeline_host_->registry(), available_capabilities_); !result) {
                return result;
            }
        }
        if (pipeline::detail::PipelineScopeHostExecution::has_pending_plan(*host)) {
            if (const foundation::Result result = host->apply_pending_plan(); !result) {
                return result;
            }
        }
        if (const foundation::Result result = host->collect_retired_plans(); !result) {
            return result;
        }
    }

    if (pipeline::detail::PhaseRegistryExecution::revision(pipeline_host_->registry().phase_registry()) != revision) {
        return invalid_state("pipeline phase registry changed during synchronization");
    }
    phase_revision_ = revision;
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::activate_all() {
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
            (void)deactivate_all();
            return result;
        }
        try {
            activated_pipelines_.push_back(host);
        } catch (const std::bad_alloc&) {
            (void)host->deactivate({});
            (void)deactivate_all();
            return out_of_memory("pipeline activation bookkeeping allocation failed");
        }
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::deactivate_all() {
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

foundation::Result EngineFrameOrchestrator::Impl::initialize() {
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
        pipeline::detail::PhaseRegistryExecution::snapshot(pipeline_host_->registry().phase_registry());
    if (!phase_snapshot) {
        (void)deactivate_all();
        initialized_ = false;
        phase_revision_ = 0;
        return phase_snapshot.result;
    }
    const std::size_t startup_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kStartup));
    if (startup_rank == std::numeric_limits<std::size_t>::max()) {
        (void)deactivate_all();
        initialized_ = false;
        phase_revision_ = 0;
        return invalid_state("startup phase is not registered");
    }
    for (std::size_t rank = 0; rank <= startup_rank; ++rank) {
        const foundation::Result startup_result =
            execute_registered_phase(phase_snapshot.value.ordered_phases[rank], PhaseExecutionMode::kStartup, false);
        if (!startup_result) {
            (void)deactivate_all();
            initialized_ = false;
            phase_revision_ = 0;
            return startup_result;
        }
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::begin_frame(const EngineFrameDesc& desc) {
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
        feature_textures_.assign(desc.render_feature_textures.begin(), desc.render_feature_textures.end());
        execution_leases_.assign(desc.render_graph_execution_leases.begin(), desc.render_graph_execution_leases.end());
        render_views_.assign(desc.render_views.begin(), desc.render_views.end());
        render_scene_snapshots_.clear();
    } catch (const std::bad_alloc&) {
        return out_of_memory("frame descriptor copy allocation failed");
    }
    frame_desc_ = desc;
    frame_desc_.render_feature_textures = std::span<const render_graph::RenderGraphFeatureTextureBinding>(
        feature_textures_.data(), feature_textures_.size());
    frame_desc_.render_graph_execution_leases =
        std::span<const render_graph::RenderGraphExecutionLease>(execution_leases_.data(), execution_leases_.size());
    frame_desc_.render_views = std::span<const renderer::RenderView>(render_views_.data(), render_views_.size());

    const foundation::Result begin_result = frame_context_.begin_frame();
    if (!begin_result) {
        (void)clear_frame_state();
        return begin_result;
    }
    frame_active_ = true;
    render_feature_prepared_ = false;
    render_feature_post_built_ = false;
    render_graph_executed_ = false;
    frame_presented_ = false;
    worlds_committed_ = false;
    draw_call_count_ = 0;
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
            if (swapchain_desc.format == rhi::Format::kUnknown || swapchain_desc.extent.width == 0 ||
                swapchain_desc.extent.height == 0) {
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
            if (swapchain_desc.format == rhi::Format::kUnknown || swapchain_desc.extent.width == 0 ||
                swapchain_desc.extent.height == 0) {
                return fail_frame(invalid_argument("swapchain image description is incomplete"));
            }
            const rhi::FrameAcquireResult& acquired = *frame_context_.acquired_frame();
            if (frame_desc_.swapchain_image_initial_state.usage == rhi::ResourceUsage::kUndefined) {
                frame_desc_.swapchain_image_initial_state = acquired.initial_state;
            }
            if (frame_desc_.swapchain_image_initial_state.usage == rhi::ResourceUsage::kUndefined) {
                swapchain_desc.initial_usage = rhi::ResourceUsage::kUndefined;
            }
            frame_desc_.swapchain_image_desc = swapchain_desc;

            if (!desc.reset_render_graph && swapchain_resource_.is_valid() &&
                render_graph::detail::RenderGraphResourceAccess::contains_texture(render_graph_, swapchain_resource_)) {
                if (const foundation::Result result = render_graph_.update_imported_texture(
                        swapchain_resource_, acquired.image.texture, acquired.image.view,
                        frame_desc_.swapchain_image_desc, frame_desc_.swapchain_image_initial_state);
                    !result) {
                    return fail_frame(result);
                }
            } else {
                const foundation::ResultValue<render_graph::RenderGraphTexture> import_result =
                    render_graph_.import_swapchain_image(*frame_context_.acquired_frame(),
                                                         frame_desc_.swapchain_image_desc,
                                                         frame_desc_.swapchain_image_initial_state);
                if (!import_result) {
                    return fail_frame(import_result.result);
                }
                swapchain_resource_ = import_result.value;
            }

            const auto existing_binding =
                std::find_if(feature_textures_.begin(), feature_textures_.end(),
                             [this](const render_graph::RenderGraphFeatureTextureBinding& binding) {
                                 return binding.id == frame_desc_.swapchain_resource_id;
                             });
            if (existing_binding != feature_textures_.end()) {
                if (existing_binding->texture != swapchain_resource_) {
                    return fail_frame(invalid_state("swapchain resource id is bound to another texture"));
                }
            } else {
                feature_textures_.push_back({frame_desc_.swapchain_resource_id, swapchain_resource_});
                frame_desc_.render_feature_textures = std::span<const render_graph::RenderGraphFeatureTextureBinding>(
                    feature_textures_.data(), feature_textures_.size());
            }
        }

        std::vector<pipeline::PipelineScopeHost*> ordered = ordered_pipelines();
        pinned_pipelines_.reserve(ordered.size());
        for (pipeline::PipelineScopeHost* host : ordered) {
            if (host == nullptr || !host->is_active()) {
                return fail_frame(invalid_state("all registered pipeline hosts must be active"));
            }
            pipeline::PipelinePlanHandle plan =
                pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(*host);
            if (plan == nullptr || plan->phase_revision != phase_revision_) {
                return fail_frame(invalid_state("pipeline plan is missing or uses a stale phase registry"));
            }
            pinned_pipelines_.push_back(PinnedPipeline{host, std::move(plan)});
        }
        if (const foundation::Result result = prepare_world_frames(); !result) {
            return fail_frame(result);
        }
    } catch (const std::bad_alloc&) {
        return fail_frame(out_of_memory("frame pipeline pinning allocation failed"));
    }

    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::execute_pipeline_phase(const PinnedPipeline& pinned_pipeline,
                                                                   pipeline::PhaseId phase) {
    if (pinned_pipeline.host == nullptr || pinned_pipeline.plan == nullptr) {
        return invalid_state("pinned pipeline is incomplete");
    }
    pipeline::PipelineExecutionContext context(pinned_pipeline.host->scope(), &pinned_pipeline.host->artifacts());
    if (const auto result = pinned_pipeline.host->execute_phase(pipeline_host_->scheduler(), context, phase,
                                                               pinned_pipeline.plan); !result) return result;
    if (!inspection_enabled_) return foundation::Result::success();
    try {
        std::vector<detail::EngineFrameStageObservation> observations;
        observations.reserve(context.visited_nodes().size());
        for (const uint32_t index : context.visited_nodes()) {
            if (index >= pinned_pipeline.plan->nodes.size()) return invalid_state("Stage inspection has an invalid visit");
            const auto& node = pinned_pipeline.plan->nodes[index];
            detail::EngineFrameStageObservation observation{};
            observation.instance = pinned_pipeline.host->instance_id();
            observation.stage = node.stage;
            observation.phase = node.phase_id;
            observation.scope = pinned_pipeline.host->scope();
            observation.frame_number = frame_context_.frame_number();
            observation.fixed_tick = frame_desc_.fixed_tick;
            observation.user_data_identity = reinterpret_cast<uintptr_t>(frame_desc_.user_data);
            if (auto* scope = find_frame_scope_for_host(*pinned_pipeline.host); scope != nullptr) {
                observation.scope_identity = reinterpret_cast<uintptr_t>(scope);
                observation.world_frame = scope->key();
                const bool reads_snapshot = std::any_of(node.artifact_bindings.begin(), node.artifact_bindings.end(),
                    [](const pipeline::ArtifactBinding& binding) {
                        return binding.artifact == world::detail::kWorldSnapshotArtifactId &&
                               binding.access == pipeline::ArtifactAccess::kRead;
                    });
                if (reads_snapshot) {
                    const auto typed = scope->read_snapshot();
                    if (!typed) return typed.result;
                    const auto* bridge = pipeline::detail::PipelineScopeHostExecution::scope_bridge(*pinned_pipeline.host);
                    if (bridge == nullptr) return invalid_state("Stage inspection lost the current host bridge");
                    const auto bridged = bridge->read(world::detail::kWorldSnapshotArtifactId,
                        world::detail::kWorldSnapshotArtifactTypeId, world::detail::kWorldSnapshotArtifactVersion);
                    if (!bridged) return bridged.result;
                    const auto* value = bridged.value.as<ecs::WorldSnapshot>(world::detail::kWorldSnapshotArtifactTypeId);
                    if (value == nullptr || bridged.value.owner_scope() != pipeline::PipelineScope::kWorld ||
                        value->world() != typed.value.world() || value->revision() != typed.value.revision() ||
                        value->frame_number() != typed.value.frame_number() || value->fixed_tick() != typed.value.fixed_tick())
                        return invalid_state("Stage inspection disagrees with the captured World bridge");
                    observation.world_snapshot = typed.value;
                    observation.bridged_snapshot = bridged.value;
                }
            }
            observations.push_back(std::move(observation));
        }
        inspection_trace_.stages.insert(inspection_trace_.stages.end(), observations.begin(), observations.end());
    } catch (const std::bad_alloc&) {
        return out_of_memory("Stage inspection allocation failed");
    }
    return foundation::Result::success();
}

world::WorldFrameScope* EngineFrameOrchestrator::Impl::find_frame_scope_for_host(
    const pipeline::PipelineScopeHost& host) const noexcept {
    for (const ActiveWorldFrame& active : active_world_frames_) {
        if (active.binding.frame_host == &host && active.scope != nullptr) {
            return active.scope.get();
        }
    }
    return nullptr;
}

foundation::Result EngineFrameOrchestrator::Impl::prepare_world_frames() {
    clear_world_frames();
    try {
        active_world_frames_.reserve(world_frames_.size());
        for (const WorldFrameBinding& binding : world_frames_) {
            if (binding.world == nullptr) {
                return invalid_state("World view registration contains a null owner");
            }
            if (!binding.world->is_initialized()) {
                return invalid_state("all registered World Scopes must be initialized");
            }
            if (binding.frame_host && binding.frame_host->scope() != pipeline::PipelineScope::kFrame) {
                return invalid_state("World view is bound to a non-Frame pipeline host");
            }
            foundation::ResultValue<world::WorldFrameScope> scope_result =
                binding.world->create_frame_scope(frame_context_.frame_number(), binding.view_id);
            if (!scope_result) {
                return scope_result.result;
            }
            ActiveWorldFrame active{};
            active.binding = binding;
            active.scope = std::make_unique<world::WorldFrameScope>(std::move(scope_result.value));
            active_world_frames_.push_back(std::move(active));
            ActiveWorldFrame& stored = active_world_frames_.back();
            if (binding.frame_host) {
                stored.previous_bridge = pipeline::detail::PipelineScopeHostExecution::scope_bridge(*binding.frame_host);
                pipeline::detail::PipelineScopeHostExecution::set_scope_bridge(
                    *binding.frame_host, world::detail::WorldSnapshotExecution::bridge(*stored.scope));
            }
        }
    } catch (const std::bad_alloc&) {
        clear_world_frames();
        return out_of_memory("World frame bridge allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::apply_world_commands() {
    for (const WorldRegistration& registration : worlds_) {
        world::WorldScope* world_scope = registration.world;
        if (world_scope == nullptr) {
            return invalid_state("orchestrator contains a null World Scope");
        }
        if (const foundation::Result result = world_scope->apply_commands(world_scope->command_buffer()); !result) {
            return result;
        }
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::commit_worlds() {
    if (worlds_committed_) {
        return foundation::Result::success();
    }
    try {
        for (const WorldRegistration& registration : worlds_) {
            world::WorldScope* world_scope = registration.world;
            if (world_scope == nullptr) {
                return invalid_state("orchestrator contains a null World Scope");
            }
            if (world_commands_) {
                if (const auto result = world_commands_->consume(*world_scope); !result) return result;
            }
            const foundation::ResultValue<std::shared_ptr<const ecs::WorldSnapshot>> snapshot =
                world_scope->commit(frame_context_.frame_number(), frame_desc_.fixed_tick);
            if (!snapshot) {
                return snapshot.result;
            }
        }
        for (ActiveWorldFrame& active : active_world_frames_) {
            if (active.binding.world == nullptr || active.scope == nullptr) {
                return invalid_state("active World frame binding is incomplete");
            }
            if (const foundation::Result result = active.scope->capture(); !result) {
                return result;
            }
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("World snapshot bookkeeping allocation failed");
    }
    worlds_committed_ = true;
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::extract_render_scenes() {
    if (frame_desc_.renderer == nullptr && !render_views_.empty()) {
        return invalid_argument("render views require a Renderer instance");
    }
    if (frame_desc_.renderer == nullptr) {
        return foundation::Result::success();
    }
    if (!worlds_committed_) {
        return invalid_state("RenderExtract requires committed World snapshots");
    }

    try {
        if (render_views_.empty()) {
            render_views_.reserve(active_world_frames_.size());
            for (const ActiveWorldFrame& active : active_world_frames_) {
                if (active.binding.world == nullptr || active.scope == nullptr) {
                    return invalid_state("active World Frame binding has no World Scope");
                }
                renderer::RenderView view{};
                const renderer::RenderExtent output_extent =
                    frame_desc_.acquire_swapchain_image
                        ? renderer::RenderExtent{frame_desc_.swapchain_image_desc.extent.width,
                                                 frame_desc_.swapchain_image_desc.extent.height}
                        : frame_desc_.render_output_extent;
                if (frame_desc_.render_view_resolver) {
                    const auto snapshot_artifact = active.scope->read_snapshot();
                    if (!snapshot_artifact) {
                        return snapshot_artifact.result;
                    }
                    const auto& snapshot = snapshot_artifact.value;
                    if (!snapshot.is_valid() || snapshot.world() != active.binding.world->handle()) {
                        return invalid_state("render view resolver received an invalid World snapshot");
                    }
                    const auto resolved = frame_desc_.render_view_resolver(
                        EngineFrameRenderViewContext{snapshot, active.binding.view_id, output_extent});
                    if (!resolved) {
                        return resolved.result;
                    }
                    view = std::move(resolved.value);
                    if (view.world != active.binding.world->handle() || view.view_id != active.binding.view_id) {
                        return invalid_state("render view resolver returned a mismatched World view");
                    }
                } else {
                    view.id = renderer::make_render_view_id(active.binding.world->handle(), active.binding.view_id);
                    view.world = active.binding.world->handle();
                    view.view_id = active.binding.view_id;
                    view.output_extent = output_extent;
                }
                render_views_.push_back(view);
            }
        }
        if (render_views_.empty()) {
            return foundation::Result::success();
        }

        const bool single_swapchain_view = frame_desc_.acquire_swapchain_image && render_views_.size() == 1;
        const feature::RenderResourceId default_color_target =
            single_swapchain_view ? frame_desc_.swapchain_resource_id : feature::RenderResourceId{};
        render_scene_snapshots_.reserve(render_views_.size());
        for (std::size_t view_index = 0; view_index < render_views_.size(); ++view_index) {
            renderer::RenderView& view = render_views_[view_index];
            if (single_swapchain_view && !view.color_target.is_valid()) {
                view.output_extent = {frame_desc_.swapchain_image_desc.extent.width,
                                      frame_desc_.swapchain_image_desc.extent.height};
            }
            if (const foundation::Result result = frame_desc_.renderer->prepare_view(view, default_color_target);
                !result) {
                return result;
            }
            if (frame_desc_.acquire_swapchain_image && render_views_.size() > 1 && !view.color_target.is_valid()) {
                return invalid_argument("multiple swapchain views require explicit color targets");
            }
            for (std::size_t previous_index = 0; previous_index < view_index; ++previous_index) {
                const renderer::RenderView& previous = render_views_[previous_index];
                if (previous.id == view.id || (previous.world == view.world && previous.view_id == view.view_id)) {
                    return invalid_state("RenderExtract received duplicate RenderView identity");
                }
            }

            ActiveWorldFrame* active_frame = nullptr;
            for (ActiveWorldFrame& candidate : active_world_frames_) {
                if (candidate.binding.world != nullptr && candidate.scope != nullptr &&
                    candidate.binding.world->handle() == view.world && candidate.binding.view_id == view.view_id) {
                    if (active_frame != nullptr) {
                        return invalid_state("RenderView matches more than one active World view");
                    }
                    active_frame = &candidate;
                }
            }
            if (active_frame == nullptr || active_frame->scope == nullptr) {
                return not_found("RenderView has no registered World Frame scope");
            }

            const foundation::ResultValue<std::shared_ptr<const renderer::RenderSceneSnapshot>> snapshot =
                frame_desc_.renderer->extract(
                    *active_frame->scope, view,
                    renderer::RenderExtractionDesc{frame_context_.frame_number(), frame_desc_.user_data,
                                                   frame_desc_.render_resource_resolver,
                                                   frame_desc_.require_resolved_render_resources});
            if (!snapshot) {
                return snapshot.result;
            }
            if (active_frame->binding.frame_host)
              if (const foundation::Result result =
                    renderer::publish_frame_snapshot(*frame_desc_.renderer, *active_frame->binding.frame_host,
                                                      snapshot.value);
                !result) {
                return result;
            }
            render_scene_snapshots_.push_back(snapshot.value);
        }
        frame_desc_.render_views = std::span<const renderer::RenderView>(render_views_.data(), render_views_.size());
    } catch (const std::bad_alloc&) {
        return out_of_memory("RenderExtract scene snapshot bookkeeping allocation failed");
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "RenderExtract threw an unknown exception");
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::release_frame_artifacts() {
    foundation::Result first_error = foundation::Result::success();
    for (pipeline::PipelineScopeHost* host : pipelines_) {
        if (host == nullptr) {
            continue;
        }
        const foundation::Result result = host->artifacts().release_lifetime(pipeline::ArtifactLifetime::kFrame);
        if (!result && first_error) {
            first_error = result;
        }
    }
    return first_error;
}

foundation::Result EngineFrameOrchestrator::Impl::bind_feature_texture(feature::RenderResourceId id,
                                                                 render_graph::RenderGraphTexture texture) {
    if (!frame_active_ || !frame_context_.is_frame_active()) {
        return invalid_state("feature texture binding requires an active frame");
    }
    if (render_feature_prepared_) {
        return invalid_state("feature texture binding must finish during frame preparation");
    }
    if (!id.is_valid() || !texture.is_valid()) {
        return invalid_argument("feature texture binding is invalid");
    }
    if (!render_graph::detail::RenderGraphResourceAccess::contains_texture(render_graph_, texture)) {
        return not_found("feature texture does not belong to the active RenderGraph");
    }

    const auto existing =
        std::find_if(feature_textures_.begin(), feature_textures_.end(),
                     [id](const render_graph::RenderGraphFeatureTextureBinding& binding) { return binding.id == id; });
    if (existing != feature_textures_.end()) {
        if (existing->texture == texture) {
            return foundation::Result::success();
        }
        return invalid_state("feature texture resource id is already bound to another texture");
    }

    try {
        feature_textures_.push_back({id, texture});
    } catch (const std::bad_alloc&) {
        return out_of_memory("feature texture binding allocation failed");
    }
    frame_desc_.render_feature_textures = std::span<const render_graph::RenderGraphFeatureTextureBinding>(
        feature_textures_.data(), feature_textures_.size());
    return foundation::Result::success();
}

void EngineFrameOrchestrator::Impl::clear_world_frames() noexcept {
    for (const ActiveWorldFrame& active : active_world_frames_) {
        if (active.binding.frame_host != nullptr) {
            pipeline::detail::PipelineScopeHostExecution::set_scope_bridge(*active.binding.frame_host,
                                                                            active.previous_bridge);
        }
    }
    active_world_frames_.clear();
    worlds_committed_ = false;
}

foundation::Result EngineFrameOrchestrator::Impl::post_build_render_features() {
    if (render_feature_post_built_) {
        return foundation::Result::success();
    }
    if (!render_feature_prepared_) {
        return invalid_state("frame post-build hook requires completed preparation");
    }
    if (frame_desc_.render_feature_post_build) {
        const EngineFramePostBuildContext post_build_context{
            frame_context_,
            render_graph_,
            frame_context_.acquired_frame(),
            swapchain_resource_,
            feature_textures_,
            render_scene_snapshots_,
            frame_desc_.render_resource_resolver,
        };
        try {
            if (const foundation::Result result = frame_desc_.render_feature_post_build(post_build_context); !result) {
                return result;
            }
        } catch (const std::bad_alloc&) {
            return out_of_memory("render feature post-build allocation failed");
        } catch (const std::exception& exception) {
            return foundation::unknown_exception(exception);
        } catch (...) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "render feature post-build hook threw an unknown exception");
        }
    }
    render_feature_post_built_ = true;
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::execute_render_graph() {
    if (render_graph_executed_) {
        return foundation::Result::success();
    }
    render_graph::RenderGraphExecuteDesc desc{};
    desc.present_acquired_frame = false;
    desc.execution_leases =
        std::span<const render_graph::RenderGraphExecutionLease>(execution_leases_.data(), execution_leases_.size());
    const foundation::ResultValue<render_graph::RenderGraphExecutionResult> execution =
        render_graph_.execute(frame_context_, desc);
    if (!execution) {
        return execution.result;
    }
    draw_call_count_ = execution.value.draw_call_count;
    render_graph_executed_ = true;
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::execute_registered_phase(pipeline::PhaseId phase, PhaseExecutionMode mode,
                                                                     bool finalize_frame) {
    if (!initialized_) {
        return invalid_state("engine frame orchestrator is not initialized");
    }
    if (!phase.is_valid()) {
        return invalid_argument("phase id is invalid");
    }
    const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
        pipeline::detail::PhaseRegistryExecution::snapshot(pipeline_host_->registry().phase_registry());
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

    const std::size_t startup_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kStartup));
    const std::size_t frame_begin_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kFrameBegin));
    const std::size_t frame_end_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kFrameEnd));
    const std::size_t shutdown_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kShutdown));
    if (startup_rank == std::numeric_limits<std::size_t>::max() ||
        frame_begin_rank == std::numeric_limits<std::size_t>::max() ||
        frame_end_rank == std::numeric_limits<std::size_t>::max() ||
        shutdown_rank == std::numeric_limits<std::size_t>::max() || startup_rank >= frame_begin_rank ||
        frame_begin_rank > frame_end_rank || frame_end_rank >= shutdown_rank) {
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
        (mode != PhaseExecutionMode::kFrame || !is_builtin_phase(phase, pipeline::EnginePhase::kFrameEnd))) {
        return invalid_argument("only FrameEnd can finalize an orchestrator frame");
    }

    if (mode == PhaseExecutionMode::kPreFrame || mode == PhaseExecutionMode::kFrame) {
        if (has_last_phase_ && current_rank < last_phase_rank_) {
            return invalid_state("frame phases must execute in global registry order");
        }
        has_last_phase_ = true;
        last_phase_rank_ = current_rank;
    }

    const bool is_world_commit_phase =
        mode == PhaseExecutionMode::kFrame && is_builtin_phase(phase, pipeline::EnginePhase::kWorldCommit);
    if (mode == PhaseExecutionMode::kFrame && is_builtin_phase(phase, pipeline::EnginePhase::kPreUpdate)) {
        if (const foundation::Result result = apply_world_commands(); !result) {
            return result;
        }
    }
    if (mode == PhaseExecutionMode::kFrame && is_builtin_phase(phase, pipeline::EnginePhase::kRenderExtract)) {
        if (const foundation::Result result = extract_render_scenes(); !result) {
            return result;
        }
    }
    if (frame_active_) {
        bool world_commit_executed = false;
        for (const PinnedPipeline& pinned_pipeline : pinned_pipelines_) {
            if (is_world_commit_phase && !world_commit_executed && pinned_pipeline.host != nullptr &&
                static_cast<uint8_t>(pinned_pipeline.host->scope()) >
                    static_cast<uint8_t>(pipeline::PipelineScope::kWorld)) {
                if (const foundation::Result result = commit_worlds(); !result) {
                    return result;
                }
                world_commit_executed = true;
            }
            if (const foundation::Result result = execute_pipeline_phase(pinned_pipeline, phase); !result) {
                return result;
            }
        }
        if (is_world_commit_phase && !world_commit_executed) {
            if (const foundation::Result result = commit_worlds(); !result) {
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
            const PinnedPipeline pinned_pipeline{
                host, pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(*host)};
            if (const foundation::Result result = execute_pipeline_phase(pinned_pipeline, phase); !result) {
                return result;
            }
        }
    }

    if (mode == PhaseExecutionMode::kFrame && is_builtin_phase(phase, pipeline::EnginePhase::kRenderBuild) &&
        !render_feature_prepared_) {
        if (frame_desc_.render_feature_preparation) {
            const EngineFramePreparationContext preparation_context{
                frame_context_, render_graph_, frame_context_.acquired_frame(), swapchain_resource_,
                render_scene_snapshots_, owner_,
            };
            try {
                if (const foundation::Result result = frame_desc_.render_feature_preparation(preparation_context);
                    !result) {
                    return result;
                }
            } catch (const std::bad_alloc&) {
                return out_of_memory("render feature preparation allocation failed");
            } catch (const std::exception& exception) {
                return foundation::unknown_exception(exception);
            } catch (...) {
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                   "render feature preparation threw an unknown exception");
            }
        }
        render_feature_prepared_ = true;
    }

    if (mode == PhaseExecutionMode::kFrame && is_builtin_phase(phase, pipeline::EnginePhase::kRenderBuild)) {
        if (const foundation::Result result = post_build_render_features(); !result) {
            return result;
        }
    } else if (mode == PhaseExecutionMode::kFrame && is_builtin_phase(phase, pipeline::EnginePhase::kRenderExecute)) {
        if (const foundation::Result result = execute_render_graph(); !result) {
            return result;
        }
    } else if (mode == PhaseExecutionMode::kFrame && is_builtin_phase(phase, pipeline::EnginePhase::kPresent) &&
               frame_desc_.present_acquired_frame && frame_context_.acquired_frame() != nullptr) {
        const foundation::ResultValue<rhi::PresentResult> present_result = frame_context_.present();
        if (!present_result) {
            return present_result.result;
        }
        frame_presented_ = true;
    }

    if (pipeline::detail::PhaseRegistryExecution::revision(pipeline_host_->registry().phase_registry()) != phase_revision_) {
        return invalid_state("phase registry changed during phase execution");
    }

    if (finalize_frame) {
        return finish_frame(true);
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::finish_frame(bool clear_state) {
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

    const foundation::Result artifact_release_result = release_frame_artifacts();

    frame_active_ = false;
    if (clear_state) {
        (void)clear_frame_state();
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
    if (!artifact_release_result) {
        retirement_error = append_failure(std::move(retirement_error), artifact_release_result);
    }
    if (!retirement_error) {
        return retirement_error;
    }
    return presentation_status;
}

foundation::Result EngineFrameOrchestrator::Impl::fail_frame(foundation::Result result) {
    if (frame_active_ || frame_context_.is_frame_active()) {
        const foundation::Result abort_result = frame_context_.abort_frame();
        result = append_failure(std::move(result), abort_result);
    }
    if (!frame_context_.is_frame_active()) {
        (void)clear_frame_state();
    } else {
        frame_active_ = true;
    }
    result = append_failure(std::move(result), release_frame_artifacts());
    return result;
}

foundation::Result EngineFrameOrchestrator::Impl::clear_frame_state() noexcept {
    const bool preserve_swapchain_resource = !frame_desc_.reset_render_graph;
    clear_world_frames();
    pinned_pipelines_.clear();
    feature_textures_.clear();
    execution_leases_.clear();
    render_views_.clear();
    render_scene_snapshots_.clear();
    if (!preserve_swapchain_resource) {
        swapchain_resource_ = {};
    }
    frame_desc_ = {};
    last_phase_rank_ = 0;
    has_last_phase_ = false;
    frame_active_ = false;
    render_feature_prepared_ = false;
    render_feature_post_built_ = false;
    render_graph_executed_ = false;
    frame_presented_ = false;
    worlds_committed_ = false;
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::run_frame(const EngineFrameDesc& desc) {
    if (const foundation::Result result = begin_frame(desc); !result) {
        return result;
    }

    const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
        pipeline::detail::PhaseRegistryExecution::snapshot(pipeline_host_->registry().phase_registry());
    if (!phase_snapshot) {
        return fail_frame(phase_snapshot.result);
    }
    if (phase_snapshot.value.revision != phase_revision_) {
        return fail_frame(invalid_state("phase registry changed after frame synchronization"));
    }
    const std::size_t frame_begin_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kFrameBegin));
    const std::size_t frame_end_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kFrameEnd));
    const std::size_t shutdown_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kShutdown));
    const std::size_t startup_rank =
        phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kStartup));
    if (startup_rank == std::numeric_limits<std::size_t>::max() ||
        frame_begin_rank == std::numeric_limits<std::size_t>::max() ||
        frame_end_rank == std::numeric_limits<std::size_t>::max() ||
        shutdown_rank == std::numeric_limits<std::size_t>::max() || startup_rank >= frame_begin_rank ||
        frame_begin_rank > frame_end_rank || frame_end_rank >= shutdown_rank) {
        return fail_frame(invalid_state("ordinary frame interval is not registered"));
    }

    try {
        for (std::size_t rank = startup_rank + 1; rank < frame_begin_rank; ++rank) {
            const foundation::Result result = execute_registered_phase(phase_snapshot.value.ordered_phases[rank],
                                                                       PhaseExecutionMode::kPreFrame, false);
            if (!result) {
                return fail_frame(result);
            }
        }

        for (std::size_t rank = frame_begin_rank; rank <= frame_end_rank; ++rank) {
            const foundation::Result result =
                execute_registered_phase(phase_snapshot.value.ordered_phases[rank], PhaseExecutionMode::kFrame, false);
            if (!result) {
                return fail_frame(result);
            }
        }

        if (frame_active_) {
            if (const foundation::Result result = finish_frame(false); !result) {
                if (!frame_active_) {
                    (void)clear_frame_state();
                }
                return result;
            }
        }

        foundation::Result post_frame_result = foundation::Result::success();
        for (std::size_t rank = frame_end_rank + 1; rank < shutdown_rank; ++rank) {
            const foundation::Result result = execute_registered_phase(phase_snapshot.value.ordered_phases[rank],
                                                                       PhaseExecutionMode::kPostFrame, false);
            if (!result) {
                post_frame_result = result;
                break;
            }
        }

        (void)clear_frame_state();
        return post_frame_result;
    } catch (const std::bad_alloc&) {
        const foundation::Result result = out_of_memory("frame phase execution allocation failed");
        if (frame_active_) {
            return fail_frame(result);
        }
        (void)clear_frame_state();
        return result;
    } catch (const std::exception& exception) {
        const foundation::Result result =
            foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
        if (frame_active_) {
            return fail_frame(result);
        }
        (void)clear_frame_state();
        return result;
    } catch (...) {
        const foundation::Result result = foundation::Result::failure(
            foundation::ResultCode::kUnknown, "frame phase execution threw an unknown exception");
        if (frame_active_) {
            return fail_frame(result);
        }
        (void)clear_frame_state();
        return result;
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::Impl::shutdown() {
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
        const foundation::Result artifact_release_result = release_frame_artifacts();
        if (!artifact_release_result && first_error) {
            first_error = artifact_release_result;
        }
        (void)clear_frame_state();
    }

    const foundation::Result synchronize_result = synchronize_phase_registry();
    if (!synchronize_result) {
        if (first_error) {
            first_error = synchronize_result;
        }
    } else {
        const foundation::ResultValue<pipeline::PhaseRegistrySnapshot> phase_snapshot =
        pipeline::detail::PhaseRegistryExecution::snapshot(pipeline_host_->registry().phase_registry());
        if (!phase_snapshot) {
            if (first_error) {
                first_error = phase_snapshot.result;
            }
        } else {
            const std::size_t shutdown_rank =
                phase_rank(phase_snapshot.value, pipeline::to_phase_id(pipeline::EnginePhase::kShutdown));
            if (shutdown_rank == std::numeric_limits<std::size_t>::max()) {
                if (first_error) {
                    first_error = invalid_state("shutdown phase is not registered");
                }
            } else {
                for (std::size_t rank = shutdown_rank; rank < phase_snapshot.value.ordered_phases.size(); ++rank) {
                    const foundation::Result shutdown_result = execute_registered_phase(
                        phase_snapshot.value.ordered_phases[rank], PhaseExecutionMode::kShutdown, false);
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
    // RenderGraph imports are frame-owned references. Release them at the
    // orchestration boundary so host-owned textures and views can be retired
    // immediately after shutdown.
    render_graph_.reset();
    swapchain_resource_ = {};
    initialized_ = false;
    phase_revision_ = 0;
    (void)clear_frame_state();
    return first_error;
}
EngineFrameOrchestrator::EngineFrameOrchestrator(const EngineFramePipelineHost& pipeline_host,
    render_graph::FrameContext& frame_context, render_graph::RenderGraph& render_graph,
    uint64_t available_capabilities)
    : impl_(std::make_unique<Impl>(*this, pipeline_host, frame_context, render_graph, available_capabilities)) {}
EngineFrameOrchestrator::~EngineFrameOrchestrator() = default;
foundation::Result EngineFrameOrchestrator::register_pipeline(const EngineFramePipelineRegistration& registration) {
    return impl_->register_pipeline(registration);
}
foundation::Result EngineFrameOrchestrator::register_world(world::WorldScope& world_scope) {
    return impl_->register_world(world_scope);
}
foundation::Result EngineFrameOrchestrator::register_world_frame(world::WorldScope& world_scope, uint32_t view_id) {
    return impl_->register_world_frame(world_scope, view_id);
}
foundation::Result EngineFrameOrchestrator::register_world_frame(world::WorldScope& world_scope,
    const EngineFramePipelineRegistration& registration, uint32_t view_id) {
    return impl_->register_world_frame(world_scope, registration, view_id);
}
foundation::Result EngineFrameOrchestrator::unregister_world_frame(ecs::WorldHandle world, uint32_t view_id) {
    return impl_->unregister_world_frame(world, view_id);
}
foundation::Result EngineFrameOrchestrator::unregister_world(ecs::WorldHandle world) {
    return impl_->unregister_world(world);
}
foundation::Result EngineFrameOrchestrator::initialize() { return impl_->initialize(); }
foundation::Result EngineFrameOrchestrator::begin_frame(const EngineFrameDesc& desc) { return impl_->begin_frame(desc); }
foundation::Result EngineFrameOrchestrator::run_frame(const EngineFrameDesc& desc) { return impl_->run_frame(desc); }
foundation::Result EngineFrameOrchestrator::shutdown() { return impl_->shutdown(); }
bool EngineFrameOrchestrator::is_initialized() const noexcept { return impl_->initialized_; }
bool EngineFrameOrchestrator::is_frame_active() const noexcept { return impl_->frame_active_; }
std::size_t EngineFrameOrchestrator::world_count() const noexcept { return impl_->worlds_.size(); }
std::size_t EngineFrameOrchestrator::world_frame_count() const noexcept { return impl_->world_frames_.size(); }
uint32_t EngineFrameOrchestrator::draw_call_count() const noexcept { return impl_->draw_call_count_; }
foundation::Result EngineFrameOrchestrator::bind_feature_texture(feature::RenderResourceId id,
    render_graph::RenderGraphTexture texture) { return impl_->bind_feature_texture(id, texture); }
foundation::Result detail::EngineFrameInspection::enable(EngineFrameOrchestrator& owner, bool enabled) {
    if (owner.impl_->frame_active_ || owner.impl_->frame_context_.is_frame_active())
        return invalid_state("cannot change Stage inspection during an active frame");
    owner.impl_->inspection_trace_.stages.clear();
    owner.impl_->inspection_enabled_ = enabled;
    return foundation::Result::success();
}
foundation::Result detail::EngineFrameWorldCommands::attach(EngineFrameOrchestrator& owner,
    FeatureWorldCommandConsumer& consumer) {
    if (owner.impl_->frame_active_ || owner.impl_->frame_context_.is_frame_active())
        return invalid_state("cannot attach World command consumption during an active frame");
    owner.impl_->world_commands_ = &consumer;
    return foundation::Result::success();
}
foundation::ResultValue<detail::EngineFrameInspectionTrace> detail::EngineFrameInspection::capture(
    const EngineFrameOrchestrator& owner) {
    try { return {foundation::Result::success(), owner.impl_->inspection_trace_}; }
    catch (const std::bad_alloc&) { return {out_of_memory("Stage inspection copy allocation failed"), {}}; }
}
}  // namespace aeg::engine
