/** @file render_pipeline_variants.cpp @brief Built-in frame renderer definitions. */
#include <string>
#include <utility>
#include <vector>

#include "render_pipeline_contract.hpp"
#include "renderer_frame_contract.hpp"
#include "renderer_variants_internal.hpp"

namespace aeg::renderer {
namespace {
using namespace detail;
foundation::Result declare_variant_pipeline(pipeline::PipelineBuilder& builder, Variant variant) {
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
    stages.emplace_back(stage_name(variant, "RenderExtract"));
    for (size_t index : pass_indices(variant))
        stages.emplace_back(stage_name(variant, kPassNodes[index]));
    stages.emplace_back(stage_name(variant, "BeforePresent"));
    for (size_t index = 0; index < stages.size(); ++index) {
        const bool extract = index == 0;
        const bool present = index + 1 == stages.size();
        const pipeline::StageId id(stages[index]);
        const auto stage = builder.add_stage({.id = id,
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
        if (previous) {
            if (auto result = builder.runs_before(previous, id); !result)
                return result;
        }
        previous = stage;
    }
    return foundation::Result::success();
}

foundation::ResultValue<pipeline::PipelineDefinition> make_variant_pipeline_definition(Variant variant) {
    pipeline::PipelineBuilder builder({.id = pipeline_id(variant),
                                       .name = std::string(pipeline_name(variant)),
                                       .version = kVersion,
                                       .category = pipeline::PipelineCategory::kRendering,
                                       .supported_scopes = kScope,
                                       .default_execution_domain = pipeline::ExecutionDomain::kRenderThread,
                                       .default_time_policy = pipeline::TimePolicy::kFrame,
                                       .owner_module = kModule});
    if (const auto declared = declare_variant_pipeline(builder, variant); !declared) {
        return {declared, {}};
    }
    return builder.finish();
}

}  // namespace

foundation::Result register_deferred_render_pipeline(pipeline::PipelineRegistry& registry) {
    auto definition = make_variant_pipeline_definition(Variant::kDeferred);
    if (!definition)
        return definition.result;
    return registry.register_pipeline(std::move(definition.value));
}
foundation::Result register_forward_render_pipeline(pipeline::PipelineRegistry& registry) {
    auto definition = make_variant_pipeline_definition(Variant::kForward);
    if (!definition) return definition.result;
    return registry.register_pipeline(std::move(definition.value));
}
foundation::Result register_builtin_rendering(pipeline::PipelineRegistry& pipelines,
                                              pipeline::RenderPassRegistry& passes) {
    if (auto r = register_render_pipeline(pipelines); !r)
        return r;
    if (auto r = register_deferred_render_pipeline(pipelines); !r)
        return r;
    if (auto r = register_forward_render_pipeline(pipelines); !r)
        return r;
    return register_builtin_render_passes(passes);
}
}  // namespace aeg::renderer
