/**
 * @file runtime_component_orchestrator_smoke.cpp
 * @brief Verifies snapshot-only Runtime camera view resolution through EngineFrameOrchestrator.
 */
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>

#include "Components/component_bundle.hpp"
#include "Components/component_render_extraction.hpp"
#include "engine_frame_orchestrator.hpp"
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include "pipeline.hpp"
#include "renderer.hpp"
#include "renderer_frame_contract.hpp"
#include "renderer_variants_internal.hpp"
#include "rhi.hpp"
#include "world_scope.hpp"

namespace {
/** @brief Reports one failed contract assertion and keeps smoke-test exits concise. */
template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

/** @brief Captures observable state from the snapshot-only render-view resolver. */
struct RenderViewResolverState final {
    aeg::ecs::EntityId camera_entity{};
    uint32_t call_count{0};
    bool snapshot_contains_camera{false};
    uint64_t frame_number{0};
    uint64_t fixed_tick{0};
    uint64_t world_revision{0};
    aeg::renderer::RenderView resolved_view{};
};

/** @brief Retains the extracted scene independently of the frame artifact lifetime. */
struct RenderSceneObservation final {
    uint32_t post_build_count{0};
    std::shared_ptr<const aeg::renderer::RenderSceneSnapshot> snapshot{};
    const aeg::renderer::RenderResourceResolver* resolver{nullptr};
};
}  // namespace

int main() {
    using aeg::engine::components::Camera;
    using aeg::engine::components::MeshRenderer;
    using aeg::engine::components::Transform;

    constexpr uint32_t kViewId = 7;
    aeg::pipeline::PipelineRegistry registry;
    if (!require(aeg::renderer::register_render_pipeline(registry), "RenderPipeline registration failed")) {
        return 1;
    }

    aeg::reflection::TypeRegistry reflection_registry;
    aeg::ecs::WorldManager world_manager;
    const auto world_handle = world_manager.create_world({"RuntimeComponentOrchestratorWorld", true});
    if (!require(world_handle, "failed to create Runtime component orchestrator World")) {
        return 2;
    }
    const auto world_result = world_manager.find(world_handle.value);
    if (!require(world_result, "failed to resolve Runtime component orchestrator World") ||
        !require(aeg::engine::components::register_render_component_bundle(*world_result.value, reflection_registry),
                 "Runtime component bundle registration failed")) {
        return 3;
    }

    Transform camera_transform{};
    camera_transform.translation = {0.0f, 1.0f, 3.0f};
    Camera camera{};
    camera.vertical_fov_radians = 1.0f;
    camera.near_plane = 0.25f;
    camera.far_plane = 100.0f;
    const auto camera_entity = world_result.value->create_entity();
    if (!require(camera_entity, "failed to create Runtime camera entity") ||
        !require(world_result.value->emplace<Transform>(camera_entity.value, camera_transform),
                 "failed to add Runtime camera Transform") ||
        !require(world_result.value->emplace<Camera>(camera_entity.value, camera), "failed to add Runtime Camera")) {
        return 4;
    }

    Transform mesh_transform{};
    mesh_transform.translation = {0.0f, 0.0f, -4.0f};
    MeshRenderer mesh_renderer{};
    mesh_renderer.mesh_asset_id = 0x1111ull;
    mesh_renderer.material_asset_id = 0x2222ull;
    mesh_renderer.bounds_radius = 1.0f;
    const auto mesh_entity = world_result.value->create_entity();
    if (!require(mesh_entity, "failed to create Runtime mesh entity") ||
        !require(world_result.value->emplace<Transform>(mesh_entity.value, mesh_transform),
                 "failed to add Runtime mesh Transform") ||
        !require(world_result.value->emplace<MeshRenderer>(mesh_entity.value, mesh_renderer),
                 "failed to add Runtime MeshRenderer")) {
        return 5;
    }

    aeg::world::WorldScope world_scope(world_result.value);
    if (!require(world_scope.initialize(), "Runtime component WorldScope initialization failed")) {
        return 6;
    }
    aeg::pipeline::PipelineScopeHost frame_host(aeg::pipeline::PipelineScope::kFrame,
                                                aeg::pipeline::PipelineInstanceId("RuntimeComponentOrchestratorFrame"));
    if (!require(frame_host.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline")),
                 "Runtime component frame host pipeline binding failed") ||
        !require(frame_host.rebuild(registry), "Runtime component frame host rebuild failed")) {
        return 7;
    }

    aeg::renderer::Renderer renderer;
    const auto extractor = aeg::engine::components::register_mesh_renderer_extractor(renderer);
    if (!require(extractor, "Runtime mesh extractor registration failed")) {
        return 8;
    }
    auto device_result = aeg::rhi::create_device({});
    if (!require(device_result, "RHI device creation failed")) {
        return 9;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);
    aeg::pipeline::PipelineScheduler scheduler;
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFramePipelineHost pipeline_host(registry, scheduler);
    aeg::engine::EngineFrameOrchestrator orchestrator(pipeline_host, frame_context, render_graph);
    if (!require(orchestrator.register_world_frame(
                     world_scope, aeg::engine::EngineFramePipelineRegistration{frame_host}, kViewId),
                 "Runtime component World view registration failed") ||
        !require(orchestrator.initialize(), "Runtime component orchestrator initialization failed")) {
        return 10;
    }

    RenderViewResolverState resolver_state{};
    resolver_state.camera_entity = camera_entity.value;
    RenderSceneObservation scene_observation{};
    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.fixed_tick = 42;
    frame_desc.renderer = &renderer;
    frame_desc.render_view_resolver = [&resolver_state](const aeg::engine::EngineFrameRenderViewContext& context) {
        ++resolver_state.call_count;
        resolver_state.snapshot_contains_camera =
            context.world_snapshot.is_valid() &&
            context.world_snapshot.get<Transform>(resolver_state.camera_entity) != nullptr &&
            context.world_snapshot.get<Camera>(resolver_state.camera_entity) != nullptr;
        resolver_state.frame_number = context.world_snapshot.frame_number();
        resolver_state.fixed_tick = context.world_snapshot.fixed_tick();
        resolver_state.world_revision = context.world_snapshot.revision();

        aeg::engine::components::RuntimeRenderViewDesc view_desc{};
        view_desc.camera_entity = resolver_state.camera_entity;
        view_desc.view_id = context.view_id;
        view_desc.output_extent = context.output_extent;
        view_desc.visibility_mask = 0xffffffffu;
        view_desc.frustum_culling = true;
        const auto view = aeg::engine::components::make_render_view(context.world_snapshot, view_desc);
        if (view) {
            resolver_state.resolved_view = view.value;
        }
        return view;
    };
    frame_desc.render_feature_post_build = [&scene_observation, &frame_host, &world_scope](
                                              const aeg::engine::EngineFramePostBuildContext& context) {
        if (context.render_scenes.size() != 1 || context.render_scenes.front() == nullptr ||
            context.render_resource_resolver != nullptr) {
            return aeg::foundation::invalid_state("Runtime post-build scene or resolver identity is incorrect");
        }
        scene_observation.snapshot = context.render_scenes.front();
        scene_observation.resolver = context.render_resource_resolver;
        const auto& scene = *scene_observation.snapshot;
        const auto published = frame_host.artifacts().read(aeg::renderer::kRenderSceneSnapshotArtifactId,
                                                          aeg::renderer::kRenderSceneSnapshotArtifactTypeId,
                                                          aeg::renderer::kRenderSceneSnapshotArtifactVersion);
        const auto committed = world_scope.latest_snapshot();
        if (!published || !committed || !scene.is_valid() ||
            published.value.owner_scope() != aeg::pipeline::PipelineScope::kFrame ||
            published.value.storage() != aeg::pipeline::ArtifactStorage::kImmutableSnapshot ||
            published.value.version() != aeg::renderer::kRenderSceneSnapshotArtifactVersion ||
            published.value.as<aeg::renderer::RenderSceneSnapshot>(
                aeg::renderer::kRenderSceneSnapshotArtifactTypeId) != scene_observation.snapshot.get() ||
            scene.world != world_scope.handle() || scene.view.world != world_scope.handle() ||
            scene.frame_number != 1 || context.frame_context.frame_number() != 1 ||
            committed.value->frame_number() != 1 || committed.value->fixed_tick() != 42 ||
            scene.world_revision != committed.value->revision()) {
            return aeg::foundation::invalid_state("Runtime scene publication lost its owning snapshot");
        }
        ++scene_observation.post_build_count;
        return aeg::foundation::Result::success();
    };
    if (!require(orchestrator.run_frame(frame_desc), "Runtime component orchestrated frame failed")) {
        return 11;
    }
    if (!require(
            resolver_state.call_count == 1 && resolver_state.snapshot_contains_camera &&
                resolver_state.frame_number == 1 && resolver_state.fixed_tick == 42 &&
                resolver_state.resolved_view.id == aeg::renderer::make_render_view_id(world_handle.value, kViewId) &&
                resolver_state.resolved_view.world == world_handle.value &&
                resolver_state.resolved_view.view_id == kViewId &&
                resolver_state.resolved_view.output_extent == aeg::renderer::RenderExtent{1, 1} &&
                resolver_state.resolved_view.frustum_culling &&
                aeg::math::nearly_equal(resolver_state.resolved_view.camera.position, camera_transform.translation),
            "Runtime render-view resolver did not receive the committed World contract") ||
        !require(scene_observation.post_build_count == 1 && scene_observation.snapshot != nullptr &&
                     scene_observation.resolver == nullptr && scene_observation.snapshot->instances.size() == 1 &&
                     scene_observation.snapshot->world_revision == resolver_state.world_revision &&
                     scene_observation.snapshot->view.id == resolver_state.resolved_view.id &&
                     scene_observation.snapshot->view.world == world_handle.value &&
                     scene_observation.snapshot->view.view_id == kViewId &&
                     aeg::math::nearly_equal(scene_observation.snapshot->view.camera.position,
                                             camera_transform.translation),
                 "Runtime post-build did not receive the resolver-generated scene snapshot") ||
        !require(!frame_host.artifacts().read(aeg::renderer::kRenderSceneSnapshotArtifactId,
                                             aeg::renderer::kRenderSceneSnapshotArtifactTypeId,
                                             aeg::renderer::kRenderSceneSnapshotArtifactVersion),
                 "Runtime frame retained its scene artifact after run_frame")) {
        return 12;
    }

    const uint32_t resolver_call_count = resolver_state.call_count;
    aeg::engine::EngineFrameDesc missing_renderer_desc{};
    missing_renderer_desc.render_view_resolver = frame_desc.render_view_resolver;
    const aeg::foundation::Result missing_renderer_result = orchestrator.run_frame(missing_renderer_desc);
    if (!require(
            !missing_renderer_result && missing_renderer_result.code == aeg::foundation::ResultCode::kInvalidArgument,
            "EngineFrameDesc accepted a RenderView resolver without a Renderer") ||
        !require(resolver_state.call_count == resolver_call_count,
                 "missing Renderer configuration invoked the resolver")) {
        return 13;
    }

    const std::array<aeg::renderer::RenderView, 1> explicit_views{resolver_state.resolved_view};
    aeg::engine::EngineFrameDesc conflicting_desc = frame_desc;
    conflicting_desc.render_views = explicit_views;
    const aeg::foundation::Result conflicting_result = orchestrator.run_frame(conflicting_desc);
    if (!require(!conflicting_result && conflicting_result.code == aeg::foundation::ResultCode::kInvalidArgument,
                 "EngineFrameDesc accepted explicit RenderViews with a RenderView resolver") ||
        !require(resolver_state.call_count == resolver_call_count,
                 "conflicting RenderView configuration invoked the resolver")) {
        return 14;
    }

    aeg::engine::EngineFrameDesc mismatched_desc{};
    mismatched_desc.renderer = &renderer;
    mismatched_desc.render_view_resolver = [](const aeg::engine::EngineFrameRenderViewContext& context)
        -> aeg::foundation::ResultValue<aeg::renderer::RenderView> {
        aeg::renderer::RenderView view{};
        view.id = aeg::renderer::make_render_view_id(context.world_snapshot.world(), context.view_id + 1);
        view.world = context.world_snapshot.world();
        view.view_id = context.view_id + 1;
        view.output_extent = context.output_extent;
        return {aeg::foundation::Result::success(), view};
    };
    const aeg::foundation::Result mismatched_result = orchestrator.run_frame(mismatched_desc);
    if (!require(!mismatched_result && mismatched_result.code == aeg::foundation::ResultCode::kInvalidState,
                 "EngineFrameOrchestrator accepted a mismatched resolver World view")) {
        return 15;
    }

    if (!require(orchestrator.shutdown(), "Runtime component orchestrator shutdown failed") ||
        !require(orchestrator.unregister_world_frame(world_scope.handle(), kViewId),
                 "Runtime component World view unregistration failed") ||
        !require(orchestrator.unregister_world(world_scope.handle()),
                 "Runtime component World unregistration failed") ||
        !require(world_scope.shutdown(), "Runtime component WorldScope shutdown failed") ||
        !require(renderer.unregister_extractor(extractor.value), "Runtime mesh extractor unregistration failed")) {
        return 16;
    }
    if (!require(scene_observation.post_build_count == 1 && scene_observation.snapshot->is_valid() &&
                     scene_observation.snapshot->world == world_handle.value &&
                     scene_observation.snapshot->frame_number == 1 &&
                     scene_observation.snapshot->world_revision == resolver_state.world_revision &&
                     scene_observation.snapshot->instances.size() == 1 &&
                     scene_observation.snapshot->view.id == resolver_state.resolved_view.id &&
                     scene_observation.snapshot->view.world == world_handle.value &&
                     scene_observation.snapshot->view.view_id == kViewId &&
                     aeg::math::nearly_equal(scene_observation.snapshot->view.camera.position,
                                             camera_transform.translation),
                 "Runtime owning scene snapshot did not survive shutdown")) {
        return 17;
    }
    return 0;
}
