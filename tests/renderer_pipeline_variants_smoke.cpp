/**
 * @file renderer_pipeline_variants_smoke.cpp
 * @brief Verifies the Engine-owned deferred/forward renderer execution profiles.
 */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <renderer.hpp>
#include <renderer_variants_internal.hpp>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>
#include "../Engine/Pipeline/private/pipeline_scope_host_execution.hpp"
#include "../Engine/Renderer/private/renderer_feature_profiles.hpp"
#include "../Engine/RenderGraph/private/render_feature_projector.hpp"

namespace {
[[nodiscard]] bool has_stage(const aeg::pipeline::PipelinePlan& plan, aeg::pipeline::StageId id) {
    for (const aeg::pipeline::PipelineNode& node : plan.nodes) {
        if (node.stage == id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_present(const aeg::pipeline::PipelinePlan& plan) {
    return has_stage(plan, aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.BeforePresent")) ||
           has_stage(plan, aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.BeforePresent"));
}

[[nodiscard]] bool has_stage_order(const aeg::pipeline::PipelinePlan& plan,
                                   std::initializer_list<aeg::pipeline::StageId> expected) {
    if (plan.nodes.size() != expected.size() || plan.topological_order.size() != expected.size())
        return false;
    std::size_t next = 0;
    for (const uint32_t node_index : plan.topological_order) {
        if (node_index >= plan.nodes.size()) {
            return false;
        }
        if (plan.nodes[node_index].stage != *(expected.begin() + next))
            return false;
        ++next;
    }
    return next == expected.size() && !plan.topological_order.empty() &&
           plan.nodes[plan.topological_order.back()].phase == aeg::pipeline::EnginePhase::kPresent;
}

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void reject_missing_fixed_pipeline(aeg::pipeline::PipelineScopeHost& host,
                                  aeg::pipeline::PipelineRegistry& registry,
                                  const aeg::pipeline::PipelinePlanHandle& last_good) {
    const auto pipeline = host.pipeline_id();
    require(bool(registry.unregister_pipeline(pipeline)), "fixed pipeline could not be removed before failed rebuild");
    const auto rebuilt = host.rebuild(registry);
    require(!rebuilt && rebuilt.code == aeg::foundation::ResultCode::kNotFound,
            "removed fixed pipeline still rebuilt");
    require(aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(host) == last_good &&
                !aeg::pipeline::detail::PipelineScopeHostExecution::has_pending_plan(host),
            "failed rebuild replaced the last good fixed pipeline plan");
    const auto restored = pipeline == aeg::renderer::kDeferredRenderPipelineId
        ? aeg::renderer::register_deferred_render_pipeline(registry)
        : aeg::renderer::register_forward_render_pipeline(registry);
    require(bool(restored), "fixed pipeline could not be restored after failed rebuild");
}

[[nodiscard]] bool external(const aeg::feature::RenderTextureReference& reference, const char* id) {
    return reference.id == aeg::feature::RenderResourceId(id) && reference.version == 1 && reference.external;
}

void check_profile_operations(aeg::renderer::SceneRenderProfile profile,
                              const aeg::feature::RenderFeatureOutput& output) {
    using namespace aeg::feature;
    const auto& intent = output.intent;
    require(intent.schema_version == 1 && intent.textures.empty() && intent.final_output && intent.depth_output &&
                external(*intent.final_output, "Renderer.SceneColor") &&
                external(*intent.depth_output, "Renderer.SceneDepth"),
            "Scene profile terminal texture contract differs");
    for (const auto& operation : intent.operations) {
        require(std::visit([](const auto& value) {
            if constexpr (requires { value.scene; })
                return value.scene.id == RenderResourceId("Runtime.Scene") && value.scene.version == 1;
            return false;
        }, operation), "Scene profile operation does not consume the actual versioned Scene slot");
    }
    const bool forward = profile == aeg::renderer::SceneRenderProfile::kForward;
    require(intent.operations.size() == (forward ? 5u : 4u), "Scene profile operation count differs");
    const auto* shadow = std::get_if<RenderDrawDepthScene>(&intent.operations[forward ? 1 : 0]);
    require(shadow && shadow->projection == RenderDepthSceneProjection::kDirectionalShadow &&
                external(shadow->depth.target, "Renderer.ShadowDepth") &&
                shadow->depth.load == RenderAttachmentLoad::kClear && shadow->depth.clear_depth == 1.f,
            "Scene profile does not produce its independent initialized directional ShadowDepth");
    if (forward) {
        const auto* depth = std::get_if<RenderDrawDepthScene>(&intent.operations[0]);
        const auto* cull = std::get_if<RenderLightCullingScene>(&intent.operations[2]);
        const auto* draw = std::get_if<RenderDrawClusteredScene>(&intent.operations[3]);
        require(depth && depth->projection == RenderDepthSceneProjection::kCamera &&
                    external(depth->depth.target, "Renderer.SceneDepth") &&
                    depth->depth.load == RenderAttachmentLoad::kClear && depth->depth.clear_depth == 1.f &&
                    cull && external(cull->camera_depth, "Renderer.SceneDepth") && cull->directional_shadow &&
                    external(*cull->directional_shadow, "Renderer.ShadowDepth") &&
                    cull->output.id == RenderResourceId("Renderer.Scene.LightList") && cull->output.version == 1 &&
                    !cull->output.export_port.is_valid() && draw &&
                    draw->light_list == RenderLightListReference{cull->output.id, 1,
                        output.invocation.invocation_id, output.invocation.generation} &&
                    external(draw->hdr.target, "Renderer.SceneLighting") &&
                    external(draw->depth.target, "Renderer.SceneDepth") && draw->depth.load == RenderAttachmentLoad::kLoad &&
                    draw->directional_shadow && external(*draw->directional_shadow, "Renderer.ShadowDepth"),
                "Forward profile lost its camera-depth/Cull/private-LightList/clustered-draw chain");
    } else {
        const auto* gbuffer = std::get_if<RenderDrawGBufferScene>(&intent.operations[1]);
        const auto* lighting = std::get_if<RenderDeferredLightingScene>(&intent.operations[2]);
        require(gbuffer && external(gbuffer->albedo.target, "Renderer.GBuffer.Albedo") &&
                    external(gbuffer->normal.target, "Renderer.GBuffer.Normal") &&
                    external(gbuffer->material.target, "Renderer.GBuffer.Material") &&
                    external(gbuffer->depth.target, "Renderer.SceneDepth") &&
                    gbuffer->albedo.load == RenderAttachmentLoad::kClear &&
                    gbuffer->normal.load == RenderAttachmentLoad::kClear &&
                    gbuffer->material.load == RenderAttachmentLoad::kClear &&
                    gbuffer->depth.load == RenderAttachmentLoad::kClear && gbuffer->depth.clear_depth == 1.f &&
                    lighting && external(lighting->albedo, "Renderer.GBuffer.Albedo") &&
                    external(lighting->normal, "Renderer.GBuffer.Normal") &&
                    external(lighting->depth, "Renderer.SceneDepth") &&
                    external(lighting->hdr.target, "Renderer.SceneLighting") && lighting->directional_shadow &&
                    external(*lighting->directional_shadow, "Renderer.ShadowDepth"),
                "Deferred profile lost its ordered GBuffer/depth/lighting chain");
    }
    const auto* post = std::get_if<RenderPostProcessScene>(&intent.operations.back());
    require(post && external(post->hdr, "Renderer.SceneLighting") && external(post->depth, "Renderer.SceneDepth") &&
                external(post->output.target, "Renderer.SceneColor") &&
                post->output.load == RenderAttachmentLoad::kClear && !post->settings,
            "Scene profile does not consume HDR/depth through its final post-process operation");
}

void check_scene_profile(aeg::renderer::SceneRenderProfile profile,
                         aeg::feature::FeatureDefinitionId expected_id, const char* expected_asset) {
    using namespace aeg::feature;
    constexpr ModuleIdentity kModule{"com.aeg.renderer"};
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    require(bool(aeg::renderer::register_scene_render_profiles(registry)), "Scene profile registration failed");
    const auto catalog = aeg::renderer::scene_render_profile_catalog();
    require(catalog && catalog.value.entries().size() == 3 &&
                catalog.value.contract_digest() == registry.contract_digest(), "Scene catalog differs from executable registry");
    const auto definition = aeg::renderer::scene_render_profile_definition(profile);
    require(bool(definition), "Scene profile definition is missing");
    const auto& contract = definition.value.contract;
    require(definition.value.id == expected_id && definition.value.owner_module == kModule &&
                definition.value.version == Version{1, 0, 0} && bool(definition.value.execute) &&
                contract.definition_id == expected_id && contract.version == Version{1, 0, 0} &&
                contract.structure == FeatureStructure::kAtomic && contract.domains == to_mask(FeatureDomain::kRender) &&
                contract.execution == ExecutionKind::kContinuous && contract.state == StatePolicy::kStateless &&
                contract.scope.id == ScopeId("Frame") && contract.clock.id == ClockId("Frame") &&
                contract.clock.frequency_hz == 60 && contract.clock.max_backlog == 4 &&
                contract.clock.late_data_policy == LateDataPolicy::kDrop && contract.ports.empty() &&
                contract.parameters.empty() && contract.children.empty() && contract.child_port_mappings.empty() &&
                !contract.has_continuation && !contract.allows_child_process,
            "Scene profile definition/module/Atomic Frame contract differs");
    require(contract.clock.capability_version == Version{1, 0, 0} && contract.clock.timing &&
                contract.clock.timing->phase_ns == 0 && contract.clock.timing->relative_deadline_ns == 16'666'667 &&
                contract.clock.timing->deadline_policy == DeadlinePolicy::kObserve && contract.clock.work_budget_ns == 250000,
            "Scene profile Clock timing or declared work budget differs");
    auto permissions = to_mask(RenderIntentCapability::kDrawScene) | to_mask(RenderIntentCapability::kWriteExternal) |
                       to_mask(RenderIntentCapability::kReadExternal) | to_mask(RenderIntentCapability::kExportOutput);
    if (profile == aeg::renderer::SceneRenderProfile::kForward)
        permissions |= to_mask(RenderIntentCapability::kCullLights) | to_mask(RenderIntentCapability::kReadLightList);
    require(contract.permissions.version == 1 && contract.permissions.capability_mask == permissions &&
                contract.side_effects.size() == 1 && contract.side_effects.front().id == kRenderIntentEffect &&
                contract.side_effects.front().version == 1 && contract.side_effects.front().kind == SideEffectKind::kIntent &&
                contract.side_effects.front().commit == CommitPolicy::kOwnerBoundary,
            "Scene profile permissions or owner-boundary commit contract differs");
    const auto* registered = registry.find_definition(expected_id);
    const auto entry = std::find_if(catalog.value.entries().begin(), catalog.value.entries().end(),
        [expected_id](const auto& value) { return value.definition_id == expected_id; });
    require(registered && registered->owner_module == kModule && entry != catalog.value.entries().end() &&
                entry->owner_module == kModule && entry->version == Version{1, 0, 0} &&
                entry->contract_digest == feature_contract_digest(*registered) &&
                entry->contract_digest == feature_contract_digest(definition.value),
            "Scene catalog does not describe its actual executable definition");
    const auto preset = aeg::renderer::scene_render_profile_graph(profile);
    require(preset && preset.value.asset_uuid == expected_asset && preset.value.schema_version == 1 &&
                preset.value.graph_version == 1 && preset.value.contract_digest == registry.contract_digest() &&
                preset.value.nodes.size() == 1 && preset.value.edges.empty() && preset.value.process_subgraphs.empty() &&
                preset.value.nodes.front().node_id == FeatureNodeId("Renderer.Scene.Root") &&
                preset.value.nodes.front().definition_id == expected_id &&
                preset.value.nodes.front().definition_version == Version{1, 0, 0},
            "Scene profile preset does not reference its executable definition");
    auto compiled = FeatureGraphCompiler{}.compile(preset.value, registry);
    require(compiled && compiled.value.provenance.size() == 1 && compiled.value.plans.size() == 1 &&
                compiled.value.plans.front().clock == ClockId("Frame"), "Scene profile preset did not compile");
    FeatureRuntime runtime(registry);
    require(bool(runtime.publish_graph(preset.value, compiled.value)), "Scene profile did not publish");
    const auto generation = runtime.generation();
    std::optional<InvocationToken> previous;
    std::optional<RenderResourceOwner> previous_owner;
    for (uint64_t tick = 1; tick <= 2; ++tick) {
        require(bool(runtime.tick(aeg::renderer::kSceneRenderProfileClock)), "Scene profile Frame tick failed");
        const auto captured = runtime.capture_render_outputs(compiled.value);
        require(captured && captured.value.size() == 1, "Scene profile has no captured committed RenderIntent");
        const auto& output = captured.value.front();
        const auto& provenance = compiled.value.provenance.front();
        const auto* owner = runtime.find_instance(provenance.instance_id);
        require(owner && provenance.definition_id == expected_id && provenance.owner_module == kModule &&
                    provenance.node_id == FeatureNodeId("Renderer.Scene.Root") &&
                    compiled.value.plans.front().instances == std::vector<FeatureInstanceId>{owner->id} &&
                    output.contract.definition_id == expected_id && output.contract.version == Version{1, 0, 0} &&
                    output.contract.permissions.capability_mask == permissions && output.owner_module == kModule &&
                    output.node_id == provenance.node_id && output.instance_id == provenance.instance_id &&
                    owner->definition_id == expected_id && owner->owner_module == kModule &&
                    owner->scope == ScopeId("Frame") && owner->state.commit_count == tick &&
                    owner->state.payload.empty() && owner->last_invocation_committed &&
                    output.invocation == owner->last_invocation_committed->attempt.token &&
                    output.invocation.generation == generation && generation.is_valid() &&
                    output.invocation.invocation_id.is_valid() && output.invocation.attempt_id.is_valid() &&
                    output.resource_owner.instance_id == owner->id && output.resource_owner.node_id == output.node_id &&
                    output.resource_owner.definition_digest == owner->definition_digest &&
                    owner->definition_digest == entry->contract_digest && owner->definition_digest != 0 &&
                    output.resource_owner.resource_epoch == owner->render_resource_epoch && owner->render_resource_epoch != 0 &&
                    (!previous || output.invocation != *previous) &&
                    (!previous_owner || output.resource_owner == *previous_owner),
                "Scene profile capture lost its actual compiled source, commit, generation or resource owner");
        check_profile_operations(profile, output);
        const auto plan = aeg::render_graph::RenderFeatureProjector{}.plan(captured.value);
        const auto work = aeg::render_graph::RenderFeatureProjector{}.project(captured.value, tick);
        require(plan && plan.value.output_order == std::vector<size_t>{0} && work &&
                    work.value.size() == output.intent.operations.size(), "captured Scene operations did not project");
        for (size_t index = 0; index < work.value.size(); ++index) {
            const auto& operation = work.value[index];
            require(operation.id.is_valid() && operation.feature_definition_id == expected_id && operation.owner_module == kModule &&
                        operation.node_id == output.node_id && operation.instance_id == output.instance_id &&
                        operation.invocation == output.invocation && operation.operation_index == index &&
                        operation.frame_index == tick && operation.kind == aeg::render_graph::RenderWorkItemKind::kExecute,
                    "projected Scene work did not come from the captured operation");
        }
        previous = output.invocation;
        previous_owner = output.resource_owner;
    }
    // Publication resolves authored compiler identities to that generation's runtime owners.
    // A second publication therefore starts with another real compile of the same preset.
    auto next_compiled = FeatureGraphCompiler{}.compile(preset.value, registry);
    if (!next_compiled)
        std::cerr << "Scene profile republication compile: " << next_compiled.result.message << '\n';
    require(next_compiled && next_compiled.value.provenance.size() == 1 && next_compiled.value.plans.size() == 1,
            "Scene profile preset did not recompile for republication");
    const auto published_again = runtime.publish_graph(preset.value, next_compiled.value);
    if (!published_again)
        std::cerr << "Scene profile republication: " << published_again.message << '\n';
    require(bool(published_again), "Scene profile republication failed");
    require(runtime.generation() != generation, "Scene profile republication reused its retired runtime generation");
    require(bool(runtime.tick(aeg::renderer::kSceneRenderProfileClock)), "republished Scene profile Frame tick failed");
    const auto republished = runtime.capture_render_outputs(next_compiled.value);
    const auto* new_owner = runtime.find_instance(next_compiled.value.provenance.front().instance_id);
    require(republished && republished.value.size() == 1 && new_owner && new_owner->state.commit_count == 1 &&
                new_owner->last_invocation_committed &&
                republished.value.front().invocation == new_owner->last_invocation_committed->attempt.token &&
                republished.value.front().invocation.generation == runtime.generation() &&
                republished.value.front().invocation.generation != previous->generation &&
                republished.value.front().instance_id == new_owner->id && new_owner->id != previous_owner->instance_id,
            "republished Scene profile capture retained an earlier owner or committed generation");
    check_profile_operations(profile, republished.value.front());
}
}  // namespace

int main() try {
    aeg::pipeline::PipelineRegistry registry;
    if (!aeg::renderer::register_deferred_render_pipeline(registry) ||
        !aeg::renderer::register_forward_render_pipeline(registry)) {
        return 1;
    }
    if (aeg::renderer::register_deferred_render_pipeline(registry) ||
        aeg::renderer::register_forward_render_pipeline(registry)) {
        return 2;
    }
    aeg::feature::FeatureRegistry scene_features{aeg::feature::test::clock_test_host()};
    require(bool(aeg::renderer::register_scene_render_profiles(scene_features)), "actual Scene definitions failed registration");
    for (const auto* name : {"Renderer.Scene.Opaque", "Renderer.Scene.Forward", "Renderer.Scene.Deferred"})
        require(scene_features.find_definition(aeg::feature::FeatureDefinitionId(name)) != nullptr,
                "actual Scene definition is absent from the populated registry");
    for (const auto* name : {"Renderer.DeferredRenderFeature", "Renderer.ForwardRenderFeature"})
        require(scene_features.find_definition(aeg::feature::FeatureDefinitionId(name)) == nullptr,
                "actual Scene registration exposed a retired Renderer Feature identity");

    aeg::pipeline::PipelineScopeHost deferred_host(aeg::pipeline::PipelineScope::kFrame,
                                                   aeg::pipeline::PipelineInstanceId("Renderer.DeferredSmoke"));
    if (!deferred_host.bind_pipeline(aeg::renderer::kDeferredRenderPipelineId) || !deferred_host.rebuild(registry)) {
        return 6;
    }
    const auto deferred_plan = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(deferred_host);
    if (deferred_plan == nullptr ||
        !has_stage(*deferred_plan, aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.GBuffer")) ||
        !has_stage(*deferred_plan, aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.DeferredLighting")) ||
        !has_present(*deferred_plan) ||
        !has_stage_order(*deferred_plan, {aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.RenderExtract"),
                                          aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.Shadow"),
                                          aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.GBuffer"),
                                          aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.DeferredLighting"),
                                          aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.SkyboxPostProcess"),
                                          aeg::pipeline::StageId("Renderer.DeferredRenderPipeline.BeforePresent")})) {
        return 7;
    }

    reject_missing_fixed_pipeline(deferred_host, registry, deferred_plan);

    aeg::pipeline::PipelineScopeHost forward_host(aeg::pipeline::PipelineScope::kFrame,
                                                  aeg::pipeline::PipelineInstanceId("Renderer.ForwardSmoke"));
    if (!forward_host.bind_pipeline(aeg::renderer::kForwardRenderPipelineId) || !forward_host.rebuild(registry)) {
        return 8;
    }
    const auto forward_plan = aeg::pipeline::detail::PipelineScopeHostExecution::active_plan_snapshot(forward_host);
    if (forward_plan == nullptr ||
        !has_stage(*forward_plan, aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.ZPrepass")) ||
        !has_stage(*forward_plan, aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.LightCulling")) ||
        !has_stage(*forward_plan, aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.ForwardOpaque")) ||
        !has_present(*forward_plan) ||
        !has_stage_order(*forward_plan, {aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.RenderExtract"),
                                         aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.Shadow"),
                                         aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.ZPrepass"),
                                         aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.LightCulling"),
                                         aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.ForwardOpaque"),
                                         aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.SkyboxPostProcess"),
                                         aeg::pipeline::StageId("Renderer.ForwardRenderPipeline.BeforePresent")})) {
        return 9;
    }
    reject_missing_fixed_pipeline(forward_host, registry, forward_plan);
    check_scene_profile(aeg::renderer::SceneRenderProfile::kForward,
                         aeg::feature::FeatureDefinitionId("Renderer.Scene.Forward"), "renderer-scene-forward");
    check_scene_profile(aeg::renderer::SceneRenderProfile::kDeferred,
                         aeg::feature::FeatureDefinitionId("Renderer.Scene.Deferred"), "renderer-scene-deferred");

    aeg::pipeline::PipelineRegistry standard_registry;
    if (!aeg::renderer::register_render_pipeline(standard_registry)) {
        return 11;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "renderer variant contract: " << error.what() << '\n';
    return 99;
}
