/** @brief Verifies project-authored Pipeline types are available without a built game module. */
#include <filesystem>
#include <fstream>
#include <iostream>

#include "editor_project_pipeline_authoring.hpp"
#include "module_bundle.hpp"
#include "pipeline_document_io.hpp"

namespace {
bool require(bool condition, const char* message) {
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

aeg::pipeline::TypeCatalogEntry pipeline_entry(aeg::pipeline::ModuleId module) {
    using namespace aeg::pipeline;
    TypeCatalogEntry entry{};
    entry.key = {AuthoringTypeKind::kPipeline, PipelineId("Game.CustomForward").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = module;
    entry.name = "Game.CustomForward";
    entry.display_name = "Custom Forward Pipeline";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = to_mask(PipelineScope::kFrame);
    PipelineAuthoringDeclaration declaration{};
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

aeg::pipeline::TypeCatalogEntry feature_entry(aeg::pipeline::ModuleId module) {
    using namespace aeg::pipeline;
    TypeCatalogEntry entry{};
    entry.key = {AuthoringTypeKind::kFeature, FeatureId("Game.CustomOutlineFeature").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = module;
    entry.name = "Game.CustomOutlineFeature";
    entry.display_name = "Custom Outline Feature";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = to_mask(PipelineScope::kFrame);
    FeatureAuthoringDeclaration declaration{};
    declaration.parts = {{.part_index = 0, .domain = FeatureDomain::kRender, .name = "render"}};
    declaration.allowed_pass_types = {RenderPassTypeId("Game.VolumetricFogPass")};
    declaration.allow_add_passes = true;
    entry.declaration = std::move(declaration);
    return entry;
}

aeg::pipeline::TypeCatalogEntry pass_entry(aeg::pipeline::ModuleId module) {
    using namespace aeg::pipeline;
    TypeCatalogEntry entry{};
    entry.key = {AuthoringTypeKind::kRenderPass, RenderPassTypeId("Game.VolumetricFogPass").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = module;
    entry.name = "Game.VolumetricFogPass";
    entry.display_name = "Volumetric Fog Pass";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = to_mask(PipelineScope::kFrame);
    entry.declaration =
        PassAuthoringDeclaration{.type = RenderPassTypeId("Game.VolumetricFogPass"), .allowed_queue_mask = 1ull};
    return entry;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace aeg;
    if (argc == 2) {
        const std::filesystem::path root = argv[1];
        const auto loaded = application::load_editor_project_pipeline_authoring(root, root / "Source", root / "Assets");
        if (!require(static_cast<bool>(loaded), "named project authoring data did not load") ||
            !require(loaded.value.catalog.find({pipeline::AuthoringTypeKind::kPipeline,
                                                pipeline::PipelineId("Game.CustomForward").value()}) != nullptr,
                     "named project Pipeline type is missing") ||
            !require(loaded.value.catalog.find({pipeline::AuthoringTypeKind::kFeature,
                                                pipeline::FeatureId("Game.CustomOutlineFeature").value()}) != nullptr,
                     "named project Feature type is missing") ||
            !require(
                loaded.value.catalog.find({pipeline::AuthoringTypeKind::kRenderPass,
                                           pipeline::RenderPassTypeId("Game.VolumetricFogPass").value()}) != nullptr,
                "named project Pass type is missing") ||
            !require(loaded.value.documents.size() == 1 &&
                         loaded.value.documents.front().document.pipeline_nodes.size() == 1 &&
                         loaded.value.documents.front().document.feature_nodes.size() == 1 &&
                         loaded.value.documents.front().document.pass_nodes.size() == 1,
                     "named project authored graph is incomplete")) {
            return 6;
        }
        std::cout << "named project pipeline authoring smoke passed\n";
        return 0;
    }
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aeg_editor_project_pipeline_authoring";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "Source", error);
    std::filesystem::create_directories(root / "Assets" / "Pipelines", error);
    if (!require(!error, "could not create project fixture"))
        return 1;

    const pipeline::ModuleId module{"GameModule"};
    module::ModuleMetadata metadata{};
    metadata.module.id = module;
    metadata.module.name = "Game Module";
    metadata.descriptor = metadata.module;
    if (!metadata.catalog.add(pipeline_entry(module)) || !metadata.catalog.add(feature_entry(module)) ||
        !metadata.catalog.add(pass_entry(module))) {
        return 2;
    }
    const auto encoded_metadata = module::encode_module_metadata(metadata);
    if (!require(static_cast<bool>(encoded_metadata), "could not encode module metadata"))
        return 3;
    {
        std::ofstream output(root / "Source" / "GameModule.metadata.json", std::ios::binary);
        output << encoded_metadata.value;
    }

    pipeline::PipelineDocument document{};
    document.asset_uuid = "00000000-0000-0000-0000-000000000101";
    document.name = "Default";
    document.module_requirements = {module};
    document.pipeline_nodes.push_back({"custom-forward",
                                       pipeline::PipelineId("Game.CustomForward"),
                                       {1, 0, 0},
                                       pipeline::PipelineScope::kFrame,
                                       "Renderer",
                                       {}});
    document.feature_nodes.push_back({"outline-feature",
                                      pipeline::FeatureId("Game.CustomOutlineFeature"),
                                      {1, 0, 0},
                                      "custom-forward",
                                      pipeline::FeaturePointId("Game.CustomForward.PostProcess"),
                                      true,
                                      {}});
    document.pass_nodes.push_back({"outline-pass",
                                   pipeline::RenderPassTypeId("Game.VolumetricFogPass"),
                                   {1, 0, 0},
                                   "outline-feature",
                                   "render",
                                   {},
                                   true});
    if (!require(static_cast<bool>(application::project::save_pipeline_document(
                     root / "Assets" / "Pipelines" / "Default.aegpipe", document)),
                 "could not write pipeline document")) {
        return 4;
    }

    const auto loaded = application::load_editor_project_pipeline_authoring(root, root / "Source", root / "Assets");
    if (!require(static_cast<bool>(loaded), "project authoring data did not load") ||
        !require(loaded.value.catalog.entries().size() == 3, "Pipeline catalog is missing project types") ||
        !require(loaded.value.documents.size() == 1, "Pipeline document was not loaded") ||
        !require(loaded.value.documents.front().document.pipeline_nodes.size() == 1, "Pipeline node was not loaded") ||
        !require(loaded.value.documents.front().document.feature_nodes.size() == 1, "Feature node was not loaded") ||
        !require(loaded.value.documents.front().document.pass_nodes.size() == 1, "Pass node was not loaded")) {
        std::filesystem::remove_all(root, error);
        return 5;
    }
    std::filesystem::remove_all(root, error);
    std::cout << "editor project pipeline authoring smoke passed\n";
    return 0;
}
