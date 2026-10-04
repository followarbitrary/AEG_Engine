/**
 * @file renderer_pipeline_variants_smoke.cpp
 * @brief Verifies the public deferred/forward renderer registration seams.
 */
#include <initializer_list>
#include <iostream>
#include <renderer.hpp>
#include <renderer_variants.hpp>

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

[[nodiscard]] bool has_feature(const aeg::pipeline::PipelinePlan& plan, aeg::pipeline::FeatureId feature) {
    for (const aeg::pipeline::PipelineFeatureContributionRecord& contribution : plan.feature_contributions) {
        if (contribution.feature == feature) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_stage_order(const aeg::pipeline::PipelinePlan& plan,
                                   std::initializer_list<aeg::pipeline::StageId> expected) {
    std::size_t next = 0;
    for (const uint32_t node_index : plan.topological_order) {
        if (node_index >= plan.nodes.size()) {
            return false;
        }
        if (next < expected.size() && plan.nodes[node_index].stage == *(expected.begin() + next)) {
            ++next;
        }
    }
    return next == expected.size() && !plan.topological_order.empty() &&
           plan.nodes[plan.topological_order.back()].phase == aeg::pipeline::EnginePhase::kPresent;
}
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    if (!aeg::renderer::register_deferred_render_pipeline(registry) ||
        !aeg::renderer::register_forward_render_pipeline(registry)) {
        return 1;
    }
    if (aeg::renderer::register_deferred_render_pipeline(registry)) {
        return 2;
    }
    if (!aeg::renderer::register_deferred_render_feature(registry) ||
        !aeg::renderer::register_forward_render_feature(registry) ||
        !aeg::renderer::bind_deferred_render_feature(registry) ||
        !aeg::renderer::bind_forward_render_feature(registry)) {
        return 3;
    }
    if (aeg::renderer::register_deferred_render_feature(registry)) {
        return 4;
    }
    const aeg::pipeline::FeatureBindingDescriptor invalid_binding{
        .feature = aeg::renderer::kDeferredRenderFeatureId,
        .pipeline = aeg::renderer::kForwardRenderPipelineId,
        .scope = aeg::pipeline::PipelineScope::kFrame,
        .point = aeg::pipeline::FeaturePointId("Renderer.ForwardRenderPipeline.BeforePresent"),
        .priority = 0,
        .enabled = true,
        .instance = {},
    };
    if (registry.bind_feature(invalid_binding)) {
        return 5;
    }

    aeg::pipeline::PipelineScopeHost deferred_host(aeg::pipeline::PipelineScope::kFrame,
                                                   aeg::pipeline::PipelineInstanceId("Renderer.DeferredSmoke"));
    if (!deferred_host.bind_pipeline(aeg::renderer::kDeferredRenderPipelineId) || !deferred_host.rebuild(registry)) {
        return 6;
    }
    const aeg::pipeline::PipelinePlan* deferred_plan = deferred_host.active_plan();
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

    // The built-in renderer is an editable authored template. A configuration
    // with a reduced Pass set must still compile its pipeline contribution;
    // runtime graph validation will report missing inputs/final output later.
    auto edited_deferred = std::make_shared<aeg::pipeline::PipelineInstanceConfiguration>();
    edited_deferred->instance = aeg::pipeline::PipelineInstanceId("Renderer.DeferredSmoke");
    edited_deferred->pipeline = aeg::renderer::kDeferredRenderPipelineId;
    edited_deferred->scope = aeg::pipeline::PipelineScope::kFrame;
    aeg::pipeline::FeatureInstanceConfiguration edited_feature;
    edited_feature.instance = aeg::pipeline::FeatureInstanceId("EditedDeferredFeature");
    edited_feature.feature = aeg::renderer::kDeferredRenderFeatureId;
    edited_feature.point =
        aeg::pipeline::FeaturePointId("Renderer.DeferredRenderPipeline.BeforePresent");
    edited_feature.expand_authored_passes = true;
    edited_feature.passes.push_back({.type = aeg::renderer::kShadowPassTypeId,
                                     .instance = aeg::pipeline::RenderPassInstanceId("EditedShadow"),
                                     .source = "EditedShadow"});
    edited_deferred->features.push_back(std::move(edited_feature));
    if (const auto set_result = deferred_host.set_configuration(edited_deferred); !set_result) {
        std::cerr << "edited deferred configuration: " << set_result.message << '\n';
        return 20;
    }
    if (const auto edited_result = deferred_host.rebuild(registry); !edited_result) {
        std::cerr << "edited deferred rebuild: " << edited_result.message << '\n';
        return 20;
    }

    aeg::pipeline::PipelineScopeHost forward_host(aeg::pipeline::PipelineScope::kFrame,
                                                  aeg::pipeline::PipelineInstanceId("Renderer.ForwardSmoke"));
    if (!forward_host.bind_pipeline(aeg::renderer::kForwardRenderPipelineId) || !forward_host.rebuild(registry)) {
        return 8;
    }
    const aeg::pipeline::PipelinePlan* forward_plan = forward_host.active_plan();
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
    if (!has_feature(*deferred_plan, aeg::renderer::kDeferredRenderFeatureId) ||
        !has_feature(*forward_plan, aeg::renderer::kForwardRenderFeatureId)) {
        return 10;
    }

    aeg::pipeline::PipelineRegistry legacy_registry;
    if (!aeg::renderer::register_render_pipeline(legacy_registry)) {
        return 11;
    }
    return 0;
}
