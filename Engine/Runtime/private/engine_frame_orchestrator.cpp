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
                                                             EngineFrameOrchestrator& owner) noexcept
    : frame_context(frame_context),
      render_graph(render_graph),
      acquired_frame(acquired_frame),
      swapchain_resource(swapchain_resource),
      owner_(&owner) {}

foundation::Result EngineFramePreparationContext::bind_feature_texture(pipeline::RenderResourceId id,
                                                                       render_graph::RenderGraphTexture texture) const {
    if (owner_ == nullptr) {
        return invalid_state("render feature preparation context is no longer active");
    }
    return owner_->bind_feature_texture(id, texture);
}

EngineFrameOrchestrator::EngineFrameOrchestrator(pipeline::PipelineRegistry& registry,
                                                 pipeline::PipelineScheduler& scheduler,
                                                 render_graph::FrameContext& frame_context,
                                                 render_graph::RenderGraph& render_graph,
                                                 pipeline::CapabilityMask available_capabilities)
    : registry_(registry),
      scheduler_(scheduler),
      frame_context_(frame_context),
      render_graph_(render_graph),
      available_capabilities_(available_capabilities) {}

EngineFrameOrchestrator::~EngineFrameOrchestrator() {
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

bool EngineFrameOrchestrator::contains_pipeline(const pipeline::PipelineScopeHost& host) const noexcept {
    return std::find(pipelines_.begin(), pipelines_.end(), &host) != pipelines_.end();
}

pipeline::PipelineScopeHost* EngineFrameOrchestrator::find_pipeline_registration(
    pipeline::PipelineInstanceId instance) const noexcept {
    const auto iterator =
        std::find_if(pipelines_.begin(), pipelines_.end(), [instance](const pipeline::PipelineScopeHost* host) {
            return host != nullptr && host->instance_id() == instance;
        });
    return iterator == pipelines_.end() ? nullptr : *iterator;
}

void EngineFrameOrchestrator::remove_pipeline_registration(pipeline::PipelineScopeHost& host) noexcept {
    const auto iterator = std::find(pipelines_.begin(), pipelines_.end(), &host);
    if (iterator != pipelines_.end()) {
        pipelines_.erase(iterator);
    }
}

foundation::Result EngineFrameOrchestrator::register_world(world::WorldScope& world_scope) {
    if (initialized_ || frame_active_) {
        return invalid_state("cannot register a World while the orchestrator is running");
    }
    if (!world_scope.handle().is_valid() || world_scope.pipeline_host().scope() != pipeline::PipelineScope::kWorld) {
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

    const bool should_register_pipeline = world_scope.has_pipeline();
    const bool pipeline_registered = contains_pipeline(world_scope.pipeline_host());
    if (should_register_pipeline && !pipeline_registered) {
        if (const foundation::Result result = register_pipeline(world_scope.pipeline_host()); !result) {
            return result;
        }
    }
    try {
        worlds_.push_back(WorldRegistration{&world_scope, should_register_pipeline && !pipeline_registered});
    } catch (const std::bad_alloc&) {
        if (should_register_pipeline && !pipeline_registered) {
            static_cast<void>(unregister_pipeline(world_scope.pipeline_host().instance_id()));
        }
        return out_of_memory("World Scope registration allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::register_world_frame(world::WorldScope& world_scope,
                                                                 pipeline::PipelineScopeHost& frame_host,
                                                                 uint32_t view_id) {
    if (initialized_ || frame_active_) {
        return invalid_state("cannot register a World view while the orchestrator is running");
    }
    if (frame_host.scope() != pipeline::PipelineScope::kFrame) {
        return invalid_argument("World view requires a Frame pipeline host");
    }
    for (const WorldFrameBinding& binding : world_frames_) {
        if (binding.world == &world_scope && binding.view_id == view_id) {
            return invalid_state("World view is already registered");
        }
        if (binding.frame_host == &frame_host) {
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

    const bool pipeline_registered = contains_pipeline(frame_host);
    if (!pipeline_registered) {
        if (const foundation::Result result = register_pipeline(frame_host); !result) {
            if (world_registered_here) {
                static_cast<void>(unregister_world(world_scope.handle()));
            }
            return result;
        }
    }
    try {
        world_frames_.push_back(WorldFrameBinding{&world_scope, &frame_host, view_id, !pipeline_registered});
    } catch (const std::bad_alloc&) {
        if (!pipeline_registered) {
            static_cast<void>(unregister_pipeline(frame_host.instance_id()));
        }
        if (world_registered_here) {
            static_cast<void>(unregister_world(world_scope.handle()));
        }
        return out_of_memory("World view registration allocation failed");
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
    pipeline::PipelineScopeHost* host = find_pipeline_registration(instance);
    if (host == nullptr) {
        return not_found("pipeline host instance is not registered");
    }
    for (const WorldRegistration& registration : worlds_) {
        if (registration.world != nullptr && &registration.world->pipeline_host() == host) {
            return invalid_state("World pipeline host must be unregistered through unregister_world");
        }
    }
    for (const WorldFrameBinding& binding : world_frames_) {
        if (binding.frame_host == host) {
            return invalid_state("Frame pipeline host is still bound to a World view");
        }
    }
    remove_pipeline_registration(*host);
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::unregister_world_frame(ecs::WorldHandle world_handle, uint32_t view_id) {
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

foundation::Result EngineFrameOrchestrator::unregister_world(ecs::WorldHandle world_handle) {
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
    const pipeline::PipelineInstanceId instance = world_scope->pipeline_host().instance_id();
    if (registration.pipeline_registered_by_orchestrator) {
        if (find_pipeline_registration(instance) != &world_scope->pipeline_host()) {
            return invalid_state("World registration references an inconsistent pipeline host");
        }
    }
    worlds_.erase(world_iterator);
    if (registration.pipeline_registered_by_orchestrator) {
        remove_pipeline_registration(world_scope->pipeline_host());
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::validate_frame_desc(const EngineFrameDesc& desc) const {
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

std::vector<pipeline::PipelineScopeHost*> EngineFrameOrchestrator::ordered_pipelines() const {
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

std::size_t EngineFrameOrchestrator::phase_rank(const pipeline::PhaseRegistrySnapshot& snapshot,
                                                pipeline::PhaseId phase) const noexcept {
    const auto iterator = std::find(snapshot.ordered_phases.begin(), snapshot.ordered_phases.end(), phase);
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
        execution_leases_.assign(desc.render_graph_execution_leases.begin(), desc.render_graph_execution_leases.end());
        render_views_.assign(desc.render_views.begin(), desc.render_views.end());
        render_scene_snapshots_.clear();
        render_feature_contexts_.clear();
    } catch (const std::bad_alloc&) {
        return out_of_memory("frame descriptor copy allocation failed");
    }
    frame_desc_ = desc;
    frame_desc_.render_feature_buffers = std::span<const render_graph::RenderGraphFeatureBufferBinding>(
        feature_buffers_.data(), feature_buffers_.size());
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
    render_features_built_ = false;
    render_graph_executed_ = false;
    frame_presented_ = false;
    worlds_committed_ = false;
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
                render_graph_.contains_texture(swapchain_resource_)) {
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
            pipeline::PipelinePlanHandle plan = host->active_plan_snapshot();
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

foundation::Result EngineFrameOrchestrator::execute_pipeline_phase(const PinnedPipeline& pinned_pipeline,
                                                                   pipeline::PhaseId phase) {
    if (pinned_pipeline.host == nullptr || pinned_pipeline.plan == nullptr) {
        return invalid_state("pinned pipeline is incomplete");
    }
    void* user_data = frame_desc_.user_data;
    void* scope_data = nullptr;
    if (world::WorldScope* world_scope = find_world_for_host(*pinned_pipeline.host); world_scope != nullptr) {
        scope_data = world_scope;
    } else if (world::WorldFrameScope* frame_scope = find_frame_scope_for_host(*pinned_pipeline.host);
               frame_scope != nullptr) {
        scope_data = frame_scope;
    }
    pipeline::PipelineExecutionContext context(user_data, pinned_pipeline.host->scope(),
                                               &pinned_pipeline.host->artifacts(), nullptr, scope_data);
    return pinned_pipeline.host->execute_phase(scheduler_, context, phase, pinned_pipeline.plan);
}

world::WorldScope* EngineFrameOrchestrator::find_world_for_host(
    const pipeline::PipelineScopeHost& host) const noexcept {
    for (const WorldRegistration& registration : worlds_) {
        if (registration.world != nullptr && &registration.world->pipeline_host() == &host) {
            return registration.world;
        }
    }
    return nullptr;
}

world::WorldFrameScope* EngineFrameOrchestrator::find_frame_scope_for_host(
    const pipeline::PipelineScopeHost& host) const noexcept {
    for (const ActiveWorldFrame& active : active_world_frames_) {
        if (active.binding.frame_host == &host && active.scope != nullptr) {
            return active.scope.get();
        }
    }
    return nullptr;
}

const renderer::RenderSceneSnapshot* EngineFrameOrchestrator::find_render_scene_snapshot(
    const renderer::RenderView& view) const noexcept {
    for (std::size_t index = 0; index < render_views_.size() && index < render_scene_snapshots_.size(); ++index) {
        const renderer::RenderView& candidate = render_views_[index];
        if (candidate.id == view.id || (candidate.world == view.world && candidate.view_id == view.view_id)) {
            return render_scene_snapshots_[index].get();
        }
    }
    return nullptr;
}

foundation::Result EngineFrameOrchestrator::prepare_world_frames() {
    clear_world_frames();
    try {
        active_world_frames_.reserve(world_frames_.size());
        for (const WorldFrameBinding& binding : world_frames_) {
            if (binding.world == nullptr || binding.frame_host == nullptr) {
                return invalid_state("World view registration contains a null owner");
            }
            if (!binding.world->is_initialized()) {
                return invalid_state("all registered World Scopes must be initialized");
            }
            if (binding.frame_host->scope() != pipeline::PipelineScope::kFrame) {
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
            stored.previous_bridge = binding.frame_host->scope_bridge();
            binding.frame_host->set_scope_bridge(stored.scope->bridge());
        }
    } catch (const std::bad_alloc&) {
        clear_world_frames();
        return out_of_memory("World frame bridge allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::apply_world_commands() {
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

foundation::Result EngineFrameOrchestrator::commit_worlds() {
    if (worlds_committed_) {
        return foundation::Result::success();
    }
    try {
        for (const WorldRegistration& registration : worlds_) {
            world::WorldScope* world_scope = registration.world;
            if (world_scope == nullptr) {
                return invalid_state("orchestrator contains a null World Scope");
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

foundation::Result EngineFrameOrchestrator::extract_render_scenes() {
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
                    const auto* snapshot =
                        snapshot_artifact.value.as<ecs::WorldSnapshot>(world::kWorldSnapshotArtifactTypeId);
                    if (snapshot == nullptr || snapshot->world() != active.binding.world->handle()) {
                        return invalid_state("render view resolver received an invalid World snapshot");
                    }
                    const auto resolved = frame_desc_.render_view_resolver(
                        EngineFrameRenderViewContext{*snapshot, active.binding.view_id, output_extent});
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
        const pipeline::RenderResourceId default_color_target =
            single_swapchain_view ? frame_desc_.swapchain_resource_id : pipeline::RenderResourceId{};
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
            if (active_frame == nullptr || active_frame->binding.frame_host == nullptr ||
                active_frame->scope == nullptr) {
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
            if (const foundation::Result result =
                    frame_desc_.renderer->publish_frame_snapshot(*active_frame->binding.frame_host, snapshot.value);
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

foundation::Result EngineFrameOrchestrator::release_frame_artifacts() {
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

foundation::Result EngineFrameOrchestrator::bind_feature_texture(pipeline::RenderResourceId id,
                                                                 render_graph::RenderGraphTexture texture) {
    if (!frame_active_ || !frame_context_.is_frame_active()) {
        return invalid_state("feature texture binding requires an active frame");
    }
    if (render_feature_prepared_ || render_features_built_) {
        return invalid_state("feature texture binding must finish before RenderFeature construction");
    }
    if (!id.is_valid() || !texture.is_valid()) {
        return invalid_argument("feature texture binding is invalid");
    }
    if (!render_graph_.contains_texture(texture)) {
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

void EngineFrameOrchestrator::clear_world_frames() noexcept {
    for (const ActiveWorldFrame& active : active_world_frames_) {
        if (active.binding.frame_host != nullptr) {
            active.binding.frame_host->set_scope_bridge(active.previous_bridge);
        }
    }
    active_world_frames_.clear();
    worlds_committed_ = false;
}

foundation::Result EngineFrameOrchestrator::build_render_features() {
    if (render_features_built_) {
        return foundation::Result::success();
    }
    try {
        render_feature_contexts_.clear();
        render_feature_contexts_.reserve(pinned_pipelines_.size());
        for (const PinnedPipeline& pinned_pipeline : pinned_pipelines_) {
            const renderer::RenderFeatureContext* feature_context = nullptr;
            if (frame_desc_.renderer != nullptr && pinned_pipeline.host != nullptr &&
                pinned_pipeline.host->scope() == pipeline::PipelineScope::kFrame) {
                for (const ActiveWorldFrame& active : active_world_frames_) {
                    if (active.binding.frame_host != pinned_pipeline.host || active.scope == nullptr ||
                        active.binding.world == nullptr) {
                        continue;
                    }
                    renderer::RenderView view{};
                    view.id = renderer::make_render_view_id(active.binding.world->handle(), active.binding.view_id);
                    view.world = active.binding.world->handle();
                    view.view_id = active.binding.view_id;
                    const renderer::RenderSceneSnapshot* snapshot = find_render_scene_snapshot(view);
                    if (snapshot == nullptr) {
                        return not_found("Frame pipeline has no extracted RenderSceneSnapshot");
                    }
                    auto context = std::make_unique<renderer::RenderFeatureContext>(
                        renderer::RenderFeatureContext{snapshot, frame_desc_.render_resource_resolver,
                                                       frame_desc_.user_data, frame_desc_.render_passes});
                    feature_context = context.get();
                    render_feature_contexts_.push_back(
                        RenderFeatureContextBinding{pinned_pipeline.host, std::move(context)});
                    break;
                }
            }
            const render_graph::RenderGraphFeatureRuntimeDesc desc{
                std::span<const render_graph::RenderGraphFeatureBufferBinding>(feature_buffers_.data(),
                                                                               feature_buffers_.size()),
                std::span<const render_graph::RenderGraphFeatureTextureBinding>(feature_textures_.data(),
                                                                                feature_textures_.size()),
                frame_desc_.user_data, feature_context, frame_desc_.render_feature_state};
            pipeline::PipelinePlanHandle plan = pinned_pipeline.plan;
            if (const foundation::Result result =
                    render_graph::build_render_graph_features(render_graph_, std::move(plan), registry_, desc);
                !result) {
                return result;
            }
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("render feature graph build allocation failed");
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "render feature graph build threw an unknown exception");
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
    desc.execution_leases =
        std::span<const render_graph::RenderGraphExecutionLease>(execution_leases_.data(), execution_leases_.size());
    const foundation::ResultValue<render_graph::RenderGraphExecutionResult> execution =
        render_graph_.execute(frame_context_, desc);
    if (!execution) {
        return execution.result;
    }
    render_graph_executed_ = true;
    return foundation::Result::success();
}

foundation::Result EngineFrameOrchestrator::execute_registered_phase(pipeline::PhaseId phase, PhaseExecutionMode mode,
                                                                     bool finalize_frame) {
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
            const PinnedPipeline pinned_pipeline{host, host->active_plan_snapshot()};
            if (const foundation::Result result = execute_pipeline_phase(pinned_pipeline, phase); !result) {
                return result;
            }
        }
    }

    if (mode == PhaseExecutionMode::kFrame && is_builtin_phase(phase, pipeline::EnginePhase::kRenderBuild) &&
        !render_feature_prepared_) {
        if (frame_desc_.render_feature_preparation) {
            const EngineFramePreparationContext preparation_context{
                frame_context_, render_graph_, frame_context_.acquired_frame(), swapchain_resource_, *this,
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
        if (const foundation::Result result = build_render_features(); !result) {
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
    const foundation::Result result = execute_registered_phase(phase, PhaseExecutionMode::kFrame, finalize_frame);
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

foundation::Result EngineFrameOrchestrator::fail_frame(foundation::Result result) {
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

foundation::Result EngineFrameOrchestrator::clear_frame_state() noexcept {
    const bool preserve_swapchain_resource = !frame_desc_.reset_render_graph;
    clear_world_frames();
    pinned_pipelines_.clear();
    feature_buffers_.clear();
    feature_textures_.clear();
    execution_leases_.clear();
    render_views_.clear();
    render_scene_snapshots_.clear();
    render_feature_contexts_.clear();
    if (!preserve_swapchain_resource) {
        swapchain_resource_ = {};
    }
    frame_desc_ = {};
    last_phase_rank_ = 0;
    has_last_phase_ = false;
    frame_active_ = false;
    render_feature_prepared_ = false;
    render_features_built_ = false;
    render_graph_executed_ = false;
    frame_presented_ = false;
    worlds_committed_ = false;
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
            registry_.phase_registry().snapshot();
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
    initialized_ = false;
    phase_revision_ = 0;
    (void)clear_frame_state();
    return first_error;
}
}  // namespace aeg::engine
