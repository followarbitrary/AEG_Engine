/**
 * @file renderer_smoke.cpp
 * @brief Exercises Renderer extraction, RenderPipeline compilation, and frame publication.
 */
#include <array>
#include <cstdint>
#include <engine_frame_orchestrator.hpp>
#include <memory>
#include <renderer.hpp>
#include <rhi.hpp>

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

struct RenderFeatureState final {
    bool context_seen{false};
    uint32_t instance_count{0};
    uint32_t light_count{0};
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
                                                                .size = 64,
                                                                .capabilities = aeg::rhi::BufferCapability::kVertex,
                                                            },
                                                        .index_buffer = aeg::rhi::BufferHandle{2},
                                                        .index_buffer_desc =
                                                            {
                                                                .size = 12,
                                                                .capabilities = aeg::rhi::BufferCapability::kIndex,
                                                            },
                                                        .vertex_stride = 16,
                                                        .vertex_count = 4,
                                                        .index_count = 6,
                                                        .index_format = aeg::rhi::IndexFormat::kUint16,
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
                                                                .size = sizeof(aeg::math::Mat4),
                                                                .capabilities = aeg::rhi::BufferCapability::kUniform,
                                                            },
                                                        .binding_layout = aeg::rhi::BindingLayoutHandle{5},
                                                        .binding_set = aeg::rhi::BindingSetHandle{6},
                                                        .binding_group = 0,
                                                        .transform_offset = 0,
                                                        .transform_size = sizeof(aeg::math::Mat4),
                                                    }};
    }

private:
    ResolverState* state_{nullptr};
};

class SmokeRenderFeature final : public aeg::pipeline::RenderFeature {
public:
    explicit SmokeRenderFeature(RenderFeatureState& state) : state_(&state) {}

    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const aeg::pipeline::FeatureDescriptor kDescriptor{
            .id = aeg::pipeline::FeatureId("RendererSmokeFeature"),
            .name = "RendererSmokeFeature",
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
                                                    "RendererSmokeFeature did not receive a render context");
        }
        state_->context_seen = true;
        state_->instance_count = static_cast<uint32_t>(context->scene_snapshot->instances.size());
        state_->light_count = static_cast<uint32_t>(context->scene_snapshot->lights.size());
        return aeg::foundation::Result::success();
    }

protected:
    [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::RenderGraphBuilder& builder) const override {
        const aeg::pipeline::PipelineStageHandle stage = builder.add_stage({
            .id = aeg::pipeline::StageId("RendererSmokeFeature.Stage"),
            .phase = aeg::pipeline::EnginePhase::kRenderBuild,
            .execution_domain = aeg::pipeline::ExecutionDomain::kRenderThread,
            .time_policy = aeg::pipeline::TimePolicy::kFrame,
            .parallelizable = true,
        });
        return stage ? aeg::foundation::Result::success()
                     : aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                        "RendererSmokeFeature declaration failed");
    }

private:
    RenderFeatureState* state_{nullptr};
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
    const aeg::pipeline::PipelinePlanHandle render_plan = render_host.active_plan_snapshot();
    if (render_plan == nullptr || render_plan->nodes.size() != 8 ||
        !has_artifact(*render_plan, aeg::renderer::kRenderSceneSnapshotArtifactId) ||
        !has_stage(*render_plan, aeg::pipeline::StageId("RenderPipeline.Visibility")) ||
        !has_stage(*render_plan, aeg::pipeline::StageId("RenderPipeline.BeforePresent"))) {
        return 17;
    }

    RenderFeatureState render_feature_state{};
    if (!registry.register_feature(
            [&render_feature_state] { return std::make_unique<SmokeRenderFeature>(render_feature_state); }) ||
        !registry.bind_feature({
            .feature = aeg::pipeline::FeatureId("RendererSmokeFeature"),
            .pipeline = aeg::pipeline::PipelineId("RenderPipeline"),
            .scope = aeg::pipeline::PipelineScope::kFrame,
            .point = aeg::pipeline::FeaturePointId("RenderPipeline.Opaque"),
            .priority = 0,
            .enabled = true,
        })) {
        return 18;
    }

    aeg::world::WorldScope world_scope(world, registry, {});
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

    aeg::pipeline::PipelineScheduler scheduler({1, 1, 1, 1});
    aeg::render_graph::FrameContext frame_context(*device);
    aeg::render_graph::RenderGraph render_graph(*device);
    aeg::engine::EngineFrameOrchestrator orchestrator(registry, scheduler, frame_context, render_graph);
    if (!orchestrator.register_world_frame(world_scope, frame_host, 4) || !orchestrator.initialize()) {
        return 22;
    }

    aeg::engine::EngineFrameDesc frame_desc{};
    frame_desc.renderer = &renderer;
    frame_desc.render_resource_resolver = &resolver;
    const std::array<aeg::renderer::RenderView, 1> views{view};
    frame_desc.render_views = views;
    if (!orchestrator.run_frame(frame_desc)) {
        return 23;
    }
    if (!render_feature_state.context_seen || render_feature_state.instance_count != 1 ||
        render_feature_state.light_count != 1) {
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
    return 0;
}
