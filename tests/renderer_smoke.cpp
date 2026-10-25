/**
 * @file renderer_smoke.cpp
 * @brief Exercises Renderer extraction, RenderPipeline compilation, and frame publication.
 */
#include <array>
#include <cstdint>
#include <engine_frame_orchestrator.hpp>
#include "engine_frame_pipeline_host.hpp"
#include "engine_frame_pipeline_registration.hpp"
#include <iostream>
#include <memory>
#include <renderer.hpp>
#include "renderer_frame_contract.hpp"
#include <renderer_variants_internal.hpp>
#include <rhi.hpp>
#include "../Engine/Pipeline/private/pipeline_scope_host_execution.hpp"

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultCode;

struct ExtractorState final {
    aeg::ecs::EntityId mesh_entity{};
    bool user_data_seen{false};
};

struct ResolverState final {
    uint32_t mesh_resolve_count{0};
    uint32_t material_resolve_count{0};
    uint32_t object_resolve_count{0};
};

struct RenderSceneObservation final {
    uint32_t post_build_count{0};
    std::shared_ptr<const aeg::renderer::RenderSceneSnapshot> snapshot{};
    const aeg::renderer::RenderResourceResolver* resolver{nullptr};
};

class SmokeRenderResourceResolver final : public aeg::renderer::RenderResourceResolver {
public:
    explicit SmokeRenderResourceResolver(ResolverState& state) : state_(&state) {}

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMeshResource> resolve_mesh(
        aeg::renderer::MeshAssetId) const override {
        ++state_->mesh_resolve_count;
        return {aeg::foundation::Result::success(), aeg::renderer::RenderMeshResource{
                                                        .vertex_buffer = aeg::rhi::BufferHandle{1},
                                                        .vertex_buffer_desc =
                                                            {
                                                                .size = 144,
                                                                .capabilities = aeg::rhi::BufferCapability::kVertex,
                                                            },
                                                        .index_buffer = aeg::rhi::BufferHandle{2},
                                                        .index_buffer_desc =
                                                            {
                                                                .size = 12,
                                                                .capabilities = aeg::rhi::BufferCapability::kIndex,
                                                            },
                                                        .vertex_stride = 36,
                                                        .vertex_count = 4,
                                                        .index_count = 6,
                                                        .index_format = aeg::rhi::IndexFormat::kUint16,
                                                        .vertex_layout =
                                                            {
                                                                .position_offset = 0,
                                                                .position_format = aeg::rhi::Format::kRgba32Float,
                                                                .uv_offset = sizeof(float) * 4u,
                                                                .uv_format = aeg::rhi::Format::kRg32Float,
                                                                .normal_offset = sizeof(float) * 6u,
                                                                .normal_format = aeg::rhi::Format::kRgb32Float,
                                                            },
                                                    }};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        aeg::renderer::MaterialInstanceId) const override {
        ++state_->material_resolve_count;
        return {aeg::foundation::Result::success(), aeg::renderer::RenderMaterialResource{
                                                        .pipeline = aeg::rhi::PipelineHandle{3},
                                                        .binding_set = aeg::rhi::BindingSetHandle{7},
                                                        .binding_group = 1,
                                                    }};
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderObjectResource> resolve_object(
        const aeg::renderer::RenderInstance&, const aeg::renderer::RenderView&) const override {
        ++state_->object_resolve_count;
        return {aeg::foundation::Result::success(), aeg::renderer::RenderObjectResource{
                                                        .transform_buffer = aeg::rhi::BufferHandle{4},
                                                        .transform_buffer_desc =
                                                            {
                                                                .size = sizeof(aeg::renderer::RenderObjectConstants),
                                                                .capabilities = aeg::rhi::BufferCapability::kUniform,
                                                            },
                                                        .binding_layout = aeg::rhi::BindingLayoutHandle{5},
                                                        .binding_set = aeg::rhi::BindingSetHandle{6},
                                                        .binding_group = 0,
                                                        .transform_offset = 0,
                                                        .transform_size = sizeof(aeg::renderer::RenderObjectConstants),
                                                    }};
    }

private:
    ResolverState* state_{nullptr};
};

[[nodiscard]] Result add_custom_instance(const aeg::renderer::RenderExtractContext& context) {
    auto* state = static_cast<ExtractorState*>(context.user_data);
    if (state == nullptr) {
        return Result::failure(ResultCode::kInvalidArgument, "custom extractor state is null");
    }
    state->user_data_seen = true;
    return context.builder.add_instance({
        aeg::renderer::RenderObjectId("CustomObject"),
        aeg::renderer::MeshAssetId("CustomMesh"),
        aeg::renderer::MaterialInstanceId("CustomMaterial"),
        aeg::math::identity(),
        aeg::math::identity(),
        {},
        1.0f,
        0xffffffffu,
    });
}

[[nodiscard]] Result add_duplicate_instance(const aeg::renderer::RenderExtractContext& context) {
    const aeg::renderer::RenderObjectId object =
        aeg::renderer::make_render_object_id(context.world_snapshot.entity_at(0));
    return context.builder.add_instance({
        object,
        aeg::renderer::MeshAssetId("DuplicateMesh"),
        aeg::renderer::MaterialInstanceId("DuplicateMaterial"),
        aeg::math::identity(),
        aeg::math::identity(),
        {},
        1.0f,
        0xffffffffu,
    });
}

[[nodiscard]] bool has_artifact(const aeg::pipeline::PipelinePlan& plan, aeg::pipeline::ArtifactId artifact) {
    for (const aeg::pipeline::ArtifactDescriptor& descriptor : plan.artifacts) {
        if (descriptor.id == artifact) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_stage(const aeg::pipeline::PipelinePlan& plan, aeg::pipeline::StageId stage) {
    for (const aeg::pipeline::PipelineNode& node : plan.nodes) {
        if (node.stage == stage) {
            return true;
        }
    }
    return false;
}
}  // namespace

int main() {
    aeg::ecs::WorldManager world_manager;
    const auto world_handle = world_manager.create_world({"RendererSmokeWorld", true});
    if (!world_handle) {
        return 1;
    }
    const auto world_result = world_manager.find(world_handle.value);
    if (!world_result) {
        return 2;
    }
    const std::shared_ptr<aeg::ecs::World> world = world_result.value;
    if (!world->register_component<aeg::renderer::RenderMeshComponent>(
            aeg::ecs::ComponentTypeId("RenderMeshComponent")) ||
        !world->register_component<aeg::renderer::RenderLightComponent>(
            aeg::ecs::ComponentTypeId("RenderLightComponent"))) {
        return 3;
    }

    const auto mesh_entity = world->create_entity();
    const auto light_entity = world->create_entity();
    if (!mesh_entity || !light_entity) {
        return 4;
    }
    const auto mesh_component = world->emplace<aeg::renderer::RenderMeshComponent>(
        mesh_entity.value, aeg::renderer::RenderMeshComponent{
                               .transform = aeg::math::Transform{.translation = {1.0f, 2.0f, 3.0f}},
                               .mesh = aeg::renderer::MeshAssetId("Mesh"),
                               .material = aeg::renderer::MaterialInstanceId("Material"),
                               .bounds_center = {},
                               .bounds_radius = 2.0f,
                               .visibility_mask = 0xffffffffu,
                               .enabled = true,
                           });
    const auto light_component = world->emplace<aeg::renderer::RenderLightComponent>(
        light_entity.value, aeg::renderer::RenderLightComponent{
                                .type = aeg::renderer::RenderLightType::kDirectional,
                                .transform = {},
                                .color = {1.0f, 0.8f, 0.6f},
                                .intensity = 4.0f,
                                .range = 10.0f,
                                .inner_cone_cosine = 0.9f,
                                .outer_cone_cosine = 0.5f,
                                .visibility_mask = 0xffffffffu,
                                .casts_shadow = true,
                                .enabled = true,
                            });
    if (!mesh_component || !light_component) {
        return 5;
    }

    const auto snapshot_result = world->commit(7, 3);
    if (!snapshot_result) {
        return 6;
    }

    const aeg::renderer::RenderView view{
        aeg::renderer::make_render_view_id(world_handle.value, 4),
        world_handle.value,
        4,
    };
    aeg::renderer::Renderer renderer;
    if (!renderer.register_builtin_extractors()) {
        return 7;
    }
    ExtractorState extractor_state{mesh_entity.value};
    const auto custom_extractor = renderer.register_extractor(&add_custom_instance);
    if (!custom_extractor) {
        return 8;
    }

    const auto extracted =
        renderer.extract(snapshot_result.value, view, aeg::renderer::RenderExtractionDesc{7, &extractor_state});
    if (!extracted || extracted.value->instances.size() != 2 || extracted.value->lights.size() != 1 ||
        extracted.value->world_revision != snapshot_result.value.revision() || !extractor_state.user_data_seen ||
        extracted.value->visibility.candidate_instance_count != 2 ||
        extracted.value->visibility.visible_instance_count != 2 ||
        extracted.value->visibility.candidate_light_count != 1 ||
        extracted.value->visibility.visible_light_count != 1) {
        return 9;
    }

    ResolverState resolver_state{};
    auto outside_view = view;
    outside_view.frustum_culling = true;
    outside_view.camera.view = aeg::math::translation({-100, 0, 0});
    const auto outside =
        renderer.extract(snapshot_result.value, outside_view, aeg::renderer::RenderExtractionDesc{7, &extractor_state});
    if (!outside || !outside.value->instances.empty() || outside.value->shadow_casters.size() != 2) {
        std::cerr << "Camera culling discarded potential shadow casters\n";
        return 39;
    }
    outside_view.visibility_mask = 0;
    const auto masked =
        renderer.extract(snapshot_result.value, outside_view, aeg::renderer::RenderExtractionDesc{7, &extractor_state});
    if (!masked || !masked.value->shadow_casters.empty())
        return 40;
    SmokeRenderResourceResolver resolver(resolver_state);
    const auto resolved_extracted = renderer.extract(
        snapshot_result.value, view, aeg::renderer::RenderExtractionDesc{7, &extractor_state, &resolver, true});
    if (!resolved_extracted || resolver_state.mesh_resolve_count != 2 || resolver_state.material_resolve_count != 2) {
        return 10;
    }
    if (!renderer.unregister_extractor(custom_extractor.value)) {
        return 11;
    }

    const auto duplicate_extractor = renderer.register_extractor(&add_duplicate_instance);
    if (!duplicate_extractor) {
        return 12;
    }
    if (renderer.extract(snapshot_result.value, view)) {
        return 13;
    }
    if (!renderer.unregister_extractor(duplicate_extractor.value)) {
        return 14;
    }

    aeg::pipeline::PipelineRegistry registry;
    if (!aeg::renderer::register_render_pipeline(registry)) {
        return 15;
    }
    aeg::pipeline::PipelineScopeHost render_host(aeg::pipeline::PipelineScope::kFrame,
                                                 aeg::pipeline::PipelineInstanceId("RendererSmokeRenderHost"));
    if (!render_host.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline")) || !render_host.rebuild(registry)) {
        return 16;
    }
    const aeg::pipeline::PipelinePlanHandle render_plan = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(render_host);
    if (render_plan == nullptr || render_plan->nodes.size() != 8 ||
        !has_artifact(*render_plan, aeg::renderer::kRenderSceneSnapshotArtifactId) ||
        !has_stage(*render_plan, aeg::pipeline::StageId("RenderPipeline.Visibility")) ||
        !has_stage(*render_plan, aeg::pipeline::StageId("RenderPipeline.BeforePresent"))) {
        return 17;
    }

    aeg::world::WorldScope world_scope(world);
    if (!world_scope.initialize()) {
        return 19;
    }

    auto device_result = aeg::rhi::create_device({});
    if (!device_result) {
        return 20;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);
    aeg::pipeline::PipelineScopeHost frame_host(aeg::pipeline::PipelineScope::kFrame,
                                                aeg::pipeline::PipelineInstanceId("RendererSmokeFrameHost"));
    if (!frame_host.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline")) || !frame_host.rebuild(registry)) {
        return 21;
    }

    aeg::pipeline::PipelineScheduler scheduler;
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFramePipelineHost pipeline_host(registry, scheduler);
    aeg::engine::EngineFrameOrchestrator orchestrator(pipeline_host, frame_context, render_graph);
    if (!orchestrator.register_world_frame(
            world_scope, aeg::engine::EngineFramePipelineRegistration{frame_host}, 4) || !orchestrator.initialize()) {
        return 22;
    }

    RenderSceneObservation scene_observation{};
    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.renderer = &renderer;
    frame_desc.render_resource_resolver = &resolver;
    const std::array<aeg::renderer::RenderView, 1> views{view};
    frame_desc.render_views = views;
    frame_desc.render_feature_post_build = [&scene_observation, &frame_host, &world_scope, &resolver](
                                              const aeg::engine::EngineFramePostBuildContext& context) {
        if (context.render_scenes.size() != 1 || context.render_scenes.front() == nullptr ||
            context.render_resource_resolver != &resolver) {
            return Result::failure(ResultCode::kInvalidState, "Renderer post-build scene or resolver is missing");
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
            scene.view.id != aeg::renderer::make_render_view_id(world_scope.handle(), 4) || scene.view.view_id != 4 ||
            scene.frame_number != 1 || context.frame_context.frame_number() != 1 ||
            committed.value->frame_number() != 1 || scene.world_revision != committed.value->revision()) {
            return Result::failure(ResultCode::kInvalidState, "Renderer scene publication lost its owning snapshot");
        }
        ++scene_observation.post_build_count;
        return Result::success();
    };
    if (!orchestrator.run_frame(frame_desc)) {
        return 23;
    }
    if (scene_observation.post_build_count != 1 || scene_observation.snapshot == nullptr ||
        scene_observation.resolver != &resolver || scene_observation.snapshot->instances.size() != 1 ||
        scene_observation.snapshot->lights.size() != 1) {
        return 24;
    }
    const auto frame_snapshot = frame_host.artifacts().read(aeg::renderer::kRenderSceneSnapshotArtifactId,
                                                            aeg::renderer::kRenderSceneSnapshotArtifactTypeId,
                                                            aeg::renderer::kRenderSceneSnapshotArtifactVersion);
    if (frame_snapshot) {
        return 25;
    }

    if (!orchestrator.shutdown() || !orchestrator.unregister_world_frame(world_scope.handle(), 4) ||
        !orchestrator.unregister_world(world_scope.handle()) || !world_scope.shutdown()) {
        return 26;
    }
    if (!scene_observation.snapshot->is_valid() || scene_observation.snapshot->world != world_handle.value ||
        scene_observation.snapshot->frame_number != 1 || scene_observation.snapshot->view.view_id != 4 ||
        scene_observation.snapshot->instances.size() != 1 || scene_observation.snapshot->lights.size() != 1 ||
        scene_observation.snapshot->instances.front().object != aeg::renderer::make_render_object_id(mesh_entity.value)) {
        return 27;
    }
    return 0;
}
