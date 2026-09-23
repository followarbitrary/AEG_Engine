/** @file render_pipeline_variants.cpp @brief Built-in frame renderer definitions. */
#include "opaque_render_feature.hpp"
#include "pipeline_configuration.hpp"
#include "render_pipeline_contract.hpp"
#include "render_pipeline_graph.hpp"

namespace aeg::renderer {
namespace {
using namespace detail;
class VariantPipeline final : public pipeline::Pipeline {
public:
    explicit VariantPipeline(Variant variant) : variant_(variant) {
        descriptor_ = {.id = pipeline_id(variant),
                       .name = pipeline_name(variant),
                       .version = kVersion,
                       .category = pipeline::PipelineCategory::kRendering,
                       .supported_scopes = kScope,
                       .domains = kDomain,
                       .default_execution_domain = pipeline::ExecutionDomain::kRenderThread,
                       .default_time_policy = pipeline::TimePolicy::kFrame,
                       .owner_module = kModule};
    }
    const pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }
    foundation::Result build(pipeline::PipelineBuilder& builder) const override {
        if (!builder.declare_artifact({.id = kRenderSceneSnapshotArtifactId,
                                       .type = kRenderSceneSnapshotArtifactTypeId,
                                       .owner_scope = pipeline::PipelineScope::kFrame,
                                       .storage = pipeline::ArtifactStorage::kImmutableSnapshot,
                                       .lifetime = pipeline::ArtifactLifetime::kFrame,
                                       .ownership = pipeline::OwnershipPolicy::kSharedImmutable,
                                       .version = kRenderSceneSnapshotArtifactVersion,
                                       .external_input = true}))
            return builder.status();
        pipeline::PipelineStageHandle previous{};
        std::vector<std::string> stages;
        stages.emplace_back(stage_name(variant_, "RenderExtract"));
        for (size_t index : pass_indices(variant_))
            stages.emplace_back(stage_name(variant_, kPassNodes[index]));
        stages.emplace_back(stage_name(variant_, "BeforePresent"));
        for (size_t index = 0; index < stages.size(); ++index) {
            const bool extract = index == 0;
            const bool present = index + 1 == stages.size();
            const char* suffix = extract   ? "RenderExtract"
                                 : present ? "BeforePresent"
                                           : kPassNodes[pass_indices(variant_)[index - 1]];
            const pipeline::StageId id(stages[index]);
            const auto stage =
                builder.add_stage({.id = id,
                                   .phase = extract   ? pipeline::EnginePhase::kRenderExtract
                                            : present ? pipeline::EnginePhase::kPresent
                                                      : pipeline::EnginePhase::kRenderBuild,
                                   .execution_domain = extract ? pipeline::ExecutionDomain::kCpuWorker
                                                               : pipeline::ExecutionDomain::kRenderThread,
                                   .time_policy = pipeline::TimePolicy::kFrame,
                                   .parallelizable = false});
            if (!stage)
                return builder.status();
            if (extract) {
                if (auto result = builder.reads(stage, {kRenderSceneSnapshotArtifactId, pipeline::ArtifactAccess::kRead,
                                                        kRenderSceneSnapshotArtifactVersion});
                    !result)
                    return result;
            }
            if (!builder.add_feature_point({.id = pipeline::FeaturePointId(stages[index]),
                                            .name = stages[index],
                                            .anchor_stage = id,
                                            .position = pipeline::FeaturePointPosition::kBefore,
                                            .allowed_domains = kDomain,
                                            .allowed_scopes = kScope,
                                            .flags = point_flags(variant_, suffix)}))
                return builder.status();
            if (previous) {
                if (auto result = builder.runs_before(previous, id); !result)
                    return result;
            }
            previous = stage;
        }
        return foundation::Result::success();
    }

private:
    Variant variant_;
    pipeline::PipelineDescriptor descriptor_{};
};

class VariantFeature final : public pipeline::RenderFeature {
public:
    explicit VariantFeature(Variant variant)
        : variant_(variant),
          descriptor_{.id = feature_id(variant),
                      .name = feature_name(variant),
                      .domain = pipeline::FeatureDomain::kRender,
                      .domain_mask = detail::kDomain,
                      .supported_scopes = detail::kScope,
                      .version = detail::kVersion,
                      .owner_module = detail::kModule,
                      .pipeline_affinity = pipeline_id(variant)} {}
    const pipeline::FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }
    bool uses_render_state_output() const noexcept override { return true; }
    foundation::Result build_render_graph(pipeline::RenderGraphBuilder& builder) const override {
        const RenderFeatureContext* context = render_feature_context(builder);
        if (context == nullptr || !context->is_valid()) {
            return foundation::invalid_state("built-in renderer feature requires a valid RenderFeatureContext");
        }
        if (variant_ == Variant::kDeferred) {
            return build_deferred_render_graph(builder, *context);
        }
        return build_forward_render_graph(builder, *context);
    }

protected:
    foundation::Result setup(pipeline::RenderGraphBuilder& builder) const override {
        if (builder.pipeline_id() != pipeline_id(variant_) || builder.scope() != pipeline::PipelineScope::kFrame ||
            builder.feature_point() != pipeline::FeaturePointId(stage_name(variant_, "BeforePresent")))
            return foundation::invalid_argument("renderer feature requires its own BeforePresent frame anchor");
        if (const auto configuration = builder.configuration()) {
            const pipeline::FeatureInstanceConfiguration* authored = nullptr;
            for (const auto& feature : configuration->features)
                if (feature.instance == builder.feature_instance_id())
                    authored = &feature;
            if (authored == nullptr || authored->passes.size() != pass_indices(variant_).size())
                return foundation::unsupported("built-in renderer requires its complete default pass topology");
            for (size_t i = 0; i < authored->passes.size(); ++i) {
                const auto index = pass_indices(variant_)[i];
                const auto& pass = authored->passes[i];
                const auto queue = index == 5 ? rhi::QueueClass::kCompute : rhi::QueueClass::kGraphics;
                if (pass.type != pipeline::RenderPassTypeId(kPassNames[index]) || !pass.enabled ||
                    pass.queue != queue || !pass.parameters.empty() || !pass.resource_bindings.empty())
                    return foundation::unsupported(
                        "built-in renderer pass topology, parameters or resource rebinding is unsupported");
            }
        }
        const auto stage = builder.add_stage({.id = pipeline::StageId(feature_name(variant_)),
                                              .phase = pipeline::EnginePhase::kRenderBuild,
                                              .execution_domain = pipeline::ExecutionDomain::kRenderThread,
                                              .time_policy = pipeline::TimePolicy::kFrame,
                                              .parallelizable = false});
        return stage ? foundation::Result::success()
                     : foundation::invalid_state("renderer contribution stage declaration failed");
    }

private:
    Variant variant_;
    pipeline::FeatureDescriptor descriptor_{};
};
}  // namespace

foundation::Result register_deferred_render_pipeline(pipeline::PipelineRegistry& registry) {
    return registry.register_pipeline([] { return std::make_unique<VariantPipeline>(Variant::kDeferred); });
}
foundation::Result register_forward_render_pipeline(pipeline::PipelineRegistry& registry) {
    return registry.register_pipeline([] { return std::make_unique<VariantPipeline>(Variant::kForward); });
}
foundation::Result register_deferred_render_feature(pipeline::PipelineRegistry& registry) {
    return registry.register_feature([] { return std::make_unique<VariantFeature>(Variant::kDeferred); });
}
foundation::Result register_forward_render_feature(pipeline::PipelineRegistry& registry) {
    return registry.register_feature([] { return std::make_unique<VariantFeature>(Variant::kForward); });
}
foundation::Result bind_deferred_render_feature(pipeline::PipelineRegistry& registry, int32_t priority) {
    return registry.bind_feature({.feature = kDeferredRenderFeatureId,
                                  .pipeline = kDeferredRenderPipelineId,
                                  .scope = pipeline::PipelineScope::kFrame,
                                  .point = pipeline::FeaturePointId(stage_name(Variant::kDeferred, "BeforePresent")),
                                  .priority = priority});
}
foundation::Result bind_forward_render_feature(pipeline::PipelineRegistry& registry, int32_t priority) {
    return registry.bind_feature({.feature = kForwardRenderFeatureId,
                                  .pipeline = kForwardRenderPipelineId,
                                  .scope = pipeline::PipelineScope::kFrame,
                                  .point = pipeline::FeaturePointId(stage_name(Variant::kForward, "BeforePresent")),
                                  .priority = priority});
}
foundation::Result register_builtin_rendering(pipeline::PipelineRegistry& pipelines,
                                              pipeline::RenderPassRegistry& passes) {
    if (auto r = register_render_pipeline(pipelines); !r)
        return r;
    if (auto r = register_deferred_render_pipeline(pipelines); !r)
        return r;
    if (auto r = register_forward_render_pipeline(pipelines); !r)
        return r;
    if (auto r = register_opaque_render_feature(pipelines); !r)
        return r;
    if (auto r = register_deferred_render_feature(pipelines); !r)
        return r;
    if (auto r = register_forward_render_feature(pipelines); !r)
        return r;
    return register_builtin_render_passes(passes);
}
}  // namespace aeg::renderer
