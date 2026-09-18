#include "Render/Features/CustomOutlineFeature.hpp"
#include "Render/Passes/VolumetricFogPass.hpp"
#include "Render/Pipelines/CustomForwardPipeline.hpp"
#include "game_module.hpp"
#include "module_export.hpp"

namespace {
using namespace aeg::pipeline;

void define_identity(aeg::module::ModuleDefinition& definition) {
    definition.descriptor.id = ModuleId("GameModule");
    definition.descriptor.name = "Game Module";
    definition.descriptor.version = {1, 0, 0};
}

TypeCatalogEntry pipeline_entry() {
    TypeCatalogEntry entry;
    entry.key = {AuthoringTypeKind::kPipeline, PipelineId("Game.CustomForward").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = ModuleId("GameModule");
    entry.name = "Game.CustomForward";
    entry.display_name = "Custom Forward Pipeline";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = to_mask(PipelineScope::kFrame);
    PipelineAuthoringDeclaration declaration;
    declaration.stages = {{.id = StageId("Game.CustomForward.RenderBuild"),
                           .name = "Render Build",
                           .phase = PhaseId("RenderBuild"),
                           .execution_domain = ExecutionDomain::kRenderThread,
                           .time_policy = TimePolicy::kFrame}};
    declaration.feature_points = {{.id = FeaturePointId("Game.CustomForward.PostProcess"),
                                   .name = "Post Process",
                                   .anchor_stage = StageId("Game.CustomForward.RenderBuild"),
                                   .position = FeaturePointPosition::kAfter,
                                   .domains = to_mask(FeatureDomain::kRender),
                                   .scopes = to_mask(PipelineScope::kFrame),
                                   .flags = FeaturePointFlags::kMayCreateGraphicsPass}};
    entry.declaration = std::move(declaration);
    return entry;
}

TypeCatalogEntry feature_entry() {
    TypeCatalogEntry entry;
    entry.key = {AuthoringTypeKind::kFeature, FeatureId("Game.CustomOutlineFeature").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = ModuleId("GameModule");
    entry.name = "Game.CustomOutlineFeature";
    entry.display_name = "Custom Outline Feature";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = to_mask(PipelineScope::kFrame);
    FeatureAuthoringDeclaration declaration;
    declaration.parts = {{.part_index = 0, .domain = FeatureDomain::kRender, .name = "render"}};
    declaration.allowed_pass_types = {RenderPassTypeId("Game.VolumetricFogPass")};
    declaration.allow_add_passes = true;
    declaration.default_passes = {{.node_id = "VolumetricFog",
                                   .type = RenderPassTypeId("Game.VolumetricFogPass"),
                                   .version = {1, 0, 0},
                                   .part_id = "render"}};
    entry.declaration = std::move(declaration);
    entry.ports = {{.id = 22,
                    .name = "SceneColor",
                    .direction = PortDirection::kInput,
                    .value_kind = PortValueKind::kTexture,
                    .schema_id = 1,
                    .binding_id = 77,
                    .resource_constraints = {.resource_class = ResourceClass::kTexture}}};
    return entry;
}

TypeCatalogEntry pass_entry() {
    TypeCatalogEntry entry;
    entry.key = {AuthoringTypeKind::kRenderPass, RenderPassTypeId("Game.VolumetricFogPass").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = ModuleId("GameModule");
    entry.name = "Game.VolumetricFogPass";
    entry.display_name = "Volumetric Fog Pass";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = to_mask(PipelineScope::kFrame);
    entry.declaration =
        PassAuthoringDeclaration{.type = RenderPassTypeId("Game.VolumetricFogPass"),
                                 .allowed_queue_mask = 1ull << static_cast<uint8_t>(aeg::rhi::QueueClass::kGraphics)};
    entry.ports = {{.id = 11,
                    .name = "ColorOut",
                    .direction = PortDirection::kOutput,
                    .value_kind = PortValueKind::kTexture,
                    .schema_id = 1,
                    .binding_id = 77,
                    .resource_constraints = {.resource_class = ResourceClass::kTexture}}};
    return entry;
}
}  // namespace

aeg::module::ModuleDefinition aeg_define_GameModule() {
    aeg::module::ModuleDefinition definition;
    define_identity(definition);
    if (auto result = AEG_REGISTER_PIPELINE(definition, game::render::CustomForwardPipeline, pipeline_entry());
        !result) {
        return definition;
    }
    if (auto result = AEG_REGISTER_FEATURE(definition, game::render::CustomOutlineFeature, feature_entry()); !result) {
        return definition;
    }
    if (auto result = AEG_REGISTER_RENDER_PASS(definition, game::render::VolumetricFogPass, pass_entry()); !result) {
        return definition;
    }
    return definition;
}

AEG_EXPORT_GAME_MODULE_API();
