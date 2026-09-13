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
#include "pipeline.hpp"
#include "renderer.hpp"
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
    aeg::renderer::RenderView resolved_view{};
};

/** @brief Captures the RenderSceneSnapshot made visible to the test RenderFeature. */
struct RenderFeatureState final {
    bool context_seen{false};
    uint32_t instance_count{0};
    aeg::renderer::RenderView view{};
};

/** @brief Reads the extracted Runtime scene through the standard RenderPipeline feature context. */
class RuntimeComponentRenderFeature final : public aeg::pipeline::RenderFeature {
public:
    explicit RuntimeComponentRenderFeature(RenderFeatureState& state) : state_(&state) {}

    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const aeg::pipeline::FeatureDescriptor kDescriptor{
            .id = aeg::pipeline::FeatureId("RuntimeComponentOrchestratorFeature"),
            .name = "RuntimeComponentOrchestratorFeature",
            .domain = aeg::pipeline::FeatureDomain::kRender,
            .domain_mask = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender),
            .supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame),
            .version = {1, 0, 0},
        };
        return kDescriptor;
    }

    [[nodiscard]] aeg::foundation::Result build_render_graph(
        aeg::pipeline::RenderGraphBuilder& builder) const override {
        const aeg::renderer::RenderFeatureContext* context = aeg::renderer::render_feature_context(builder);
        if (context == nullptr || !context->is_valid()) {
            return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                    "Runtime component feature did not receive a render context");
        }
        state_->context_seen = true;
        state_->instance_count = static_cast<uint32_t>(context->scene_snapshot->instances.size());
        state_->view = context->scene_snapshot->view;
        return aeg::foundation::Result::success();
    }

protected:
    [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::RenderGraphBuilder& builder) const override {
        const aeg::pipeline::PipelineStageHandle stage = builder.add_stage({
            .id = aeg::pipeline::StageId("RuntimeComponentOrchestratorFeature.Stage"),
            .phase = aeg::pipeline::EnginePhase::kRenderBuild,
            .execution_domain = aeg::pipeline::ExecutionDomain::kRenderThread,
            .time_policy = aeg::pipeline::TimePolicy::kFrame,
            .parallelizable = true,
        });
        return stage ? aeg::foundation::Result::success()
                     : aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                        "Runtime component feature declaration failed");
    }

private:
    RenderFeatureState* state_{nullptr};
};
}  // namespace

int main() {
    using aeg::engine::components::Camera;
    using aeg::engine::components::MeshRenderer;
    using aeg::engine::components::Transform;

    constexpr uint32_t kViewId = 7;
    aeg::pipeline::PipelineRegistry registry;
    RenderFeatureState render_feature_state{};
    if (!require(aeg::renderer::register_render_pipeline(registry), "RenderPipeline registration failed") ||
        !require(registry.register_feature([&render_feature_state] {
            return std::make_unique<RuntimeComponentRenderFeature>(render_feature_state);
        }),
                 "Runtime component render feature registration failed") ||
        !require(registry.bind_feature({
                     .feature = aeg::pipeline::FeatureId("RuntimeComponentOrchestratorFeature"),
                     .pipeline = aeg::pipeline::PipelineId("RenderPipeline"),
                     .scope = aeg::pipeline::PipelineScope::kFrame,
                     .point = aeg::pipeline::FeaturePointId("RenderPipeline.Opaque"),
                     .priority = 0,
                     .enabled = true,
                 }),
                 "Runtime component render feature binding failed")) {
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

    aeg::world::WorldScope world_scope(world_result.value, registry, {});
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
    aeg::pipeline::PipelineScheduler scheduler({1, 1, 1, 1});
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFrameOrchestrator orchestrator(registry, scheduler, frame_context, render_graph);
    if (!require(orchestrator.register_world_frame(world_scope, frame_host, kViewId),
                 "Runtime component World view registration failed") ||
        !require(orchestrator.initialize(), "Runtime component orchestrator initialization failed")) {
        return 10;
    }

    RenderViewResolverState resolver_state{};
    resolver_state.camera_entity = camera_entity.value;
    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.fixed_tick = 42;
    frame_desc.renderer = &renderer;
    frame_desc.render_view_resolver = [&resolver_state](const aeg::engine::EngineFrameRenderViewContext& context) {
        ++resolver_state.call_count;
        resolver_state.snapshot_contains_camera =
            context.world_snapshot.is_valid() &&
            context.world_snapshot.get<Transform>(resolver_state.camera_entity) != nullptr &&
            context.world_snapshot.get<Camera>(resolver_state.camera_entity) != nullptr;

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
    if (!require(orchestrator.run_frame(frame_desc), "Runtime component orchestrated frame failed")) {
        return 11;
    }
    if (!require(
            resolver_state.call_count == 1 && resolver_state.snapshot_contains_camera &&
                resolver_state.resolved_view.id == aeg::renderer::make_render_view_id(world_handle.value, kViewId) &&
                resolver_state.resolved_view.world == world_handle.value &&
                resolver_state.resolved_view.view_id == kViewId &&
                resolver_state.resolved_view.output_extent == aeg::renderer::RenderExtent{1, 1} &&
                resolver_state.resolved_view.frustum_culling &&
                aeg::math::nearly_equal(resolver_state.resolved_view.camera.position, camera_transform.translation),
            "Runtime render-view resolver did not receive the committed World contract") ||
        !require(render_feature_state.context_seen && render_feature_state.instance_count == 1 &&
                     render_feature_state.view.id == resolver_state.resolved_view.id &&
                     render_feature_state.view.world == world_handle.value &&
                     render_feature_state.view.view_id == kViewId &&
                     aeg::math::nearly_equal(render_feature_state.view.camera.position, camera_transform.translation),
                 "Runtime RenderFeature did not receive the resolver-generated scene snapshot")) {
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
    return 0;
}
