#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "engine_frame_orchestrator.hpp"
#include "game_build_manifest.hpp"
#include "module_bundle.hpp"
#include "pipeline_asset.hpp"
#include "runtime_bootstrap.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"
#include "world_scope.hpp"

namespace {
uint32_t g_runtime_task_runs = 0;
uint32_t g_world_task_runs = 0;

aeg::foundation::Result runtime_task(aeg::pipeline::PipelineTaskContext&) {
    ++g_runtime_task_runs;
    return aeg::foundation::Result::success();
}

aeg::foundation::Result world_task(aeg::pipeline::PipelineTaskContext&) {
    ++g_world_task_runs;
    return aeg::foundation::Result::success();
}

class WorldRuntimePipeline final : public aeg::pipeline::Pipeline {
public:
    WorldRuntimePipeline() {
        descriptor_.id = aeg::pipeline::PipelineId("Smoke.WorldPipeline");
        descriptor_.name = "Smoke.WorldPipeline";
        descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kWorld);
        descriptor_.domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic);
        descriptor_.owner_module = aeg::pipeline::ModuleId("Smoke.GameModule");
    }
    const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }
    aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({.id = aeg::pipeline::StageId("Smoke.WorldStage"),
                                              .phase = aeg::pipeline::EnginePhase::kVariableUpdate,
                                              .execution_domain = aeg::pipeline::ExecutionDomain::kMainThread,
                                              .time_policy = aeg::pipeline::TimePolicy::kFrame});
        const auto task = builder.add_task({.id = aeg::pipeline::TaskId("Smoke.WorldTask"), .execute = &world_task});
        return stage && task ? builder.bind_task(task, stage) : builder.status();
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class RuntimePipeline final : public aeg::pipeline::Pipeline {
public:
    RuntimePipeline() {
        descriptor_.id = aeg::pipeline::PipelineId("Smoke.RuntimePipeline");
        descriptor_.name = "Smoke.RuntimePipeline";
        descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine);
        descriptor_.domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic);
        descriptor_.owner_module = aeg::pipeline::ModuleId("Smoke.GameModule");
    }

    const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({.id = aeg::pipeline::StageId("Smoke.RuntimeStage"),
                                              .phase = aeg::pipeline::EnginePhase::kVariableUpdate,
                                              .execution_domain = aeg::pipeline::ExecutionDomain::kMainThread,
                                              .time_policy = aeg::pipeline::TimePolicy::kFrame,
                                              .parallelizable = false});
        const auto task = builder.add_task({.id = aeg::pipeline::TaskId("Smoke.RuntimeTask"),
                                            .flags = aeg::pipeline::TaskFlags::kNone,
                                            .execute = &runtime_task,
                                            .user_data = nullptr});
        if (!stage || !task)
            return builder.status();
        return builder.bind_task(task, stage);
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class ResolverProbe final : public aeg::renderer::RenderResourceResolver {
public:
    aeg::foundation::ResultValue<aeg::renderer::RenderMeshResource> resolve_mesh(
        aeg::renderer::MeshAssetId) const override {
        return {aeg::foundation::not_ready("resolver probe"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        aeg::renderer::MaterialInstanceId) const override {
        return {aeg::foundation::not_ready("resolver probe"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderObjectResource> resolve_object(
        const aeg::renderer::RenderInstance&, const aeg::renderer::RenderView&) const override {
        return {aeg::foundation::not_ready("resolver probe"), {}};
    }
};
}  // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace aeg;
    {
        module::ModuleDefinition validation_definition{};
        validation_definition.descriptor.id = pipeline::ModuleId{"OwnedHost.Validation"};
        validation_definition.descriptor.name = "OwnedHost.Validation";
        application::runtime::RuntimeBootRequest invalid_request{};
        invalid_request.owned_host = true;
        invalid_request.headless = true;
        application::runtime::RuntimeBootstrap validation_bootstrap;
        const auto invalid_result =
            validation_bootstrap.initialize(invalid_request, std::move(validation_definition),
                                            {"owned-host-validation", "windows-x64-vulkan", "Debug"});
        if (invalid_result || invalid_result.code != foundation::ResultCode::kInvalidArgument)
            return 1;
    }
    const fs::path root = fs::temp_directory_path() / "aeg_runtime_boot_smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);

    const auto scene_id = tooling::AssetId::parse("00000000-0000-4000-8000-000000000001");
    const auto pipeline_id = tooling::AssetId::parse("00000000-0000-4000-8000-000000000002");
    const auto world_pipeline_id = tooling::AssetId::parse("00000000-0000-4000-8000-000000000003");
    if (!scene_id || !pipeline_id || !world_pipeline_id)
        return 1;
    tooling::CookedAsset scene{};
    scene.id = scene_id.value;
    scene.kind = tooling::AssetKind::kScene;
    scene.schema_version = 1;
    aeg::asset::ScenePayload scene_payload{};
    scene::SceneAsset scene_document(scene::SceneAssetId{0x4000, 0x8000000000000001ull}, "RuntimeWorld", 1);
    if (!scene_document.add_entity({scene::SceneEntityId{1, 2}, {}, "Entity", scene::SceneEntityFlags::kNone, {}}))
        return 2;
    serialization::BinaryWriter archive;
    if (!scene::SceneSerializer::serialize(scene_document, archive))
        return 2;
    scene_payload.serialized_scene.assign(
        reinterpret_cast<const std::byte*>(archive.data().data()),
        reinterpret_cast<const std::byte*>(archive.data().data() + archive.data().size()));
    const auto scene_bytes = aeg::asset::encode_scene_payload(scene_payload);
    if (!scene_bytes)
        return 2;
    scene.payload = scene_bytes.value;
    tooling::CookedAsset pipeline = scene;
    pipeline.id = pipeline_id.value;
    pipeline.kind = tooling::AssetKind::kPipeline;
    aeg::pipeline::PipelineDocument pipeline_document{};
    pipeline_document.asset_uuid = pipeline_id.value.to_string();
    pipeline_document.name = "SmokeStartupPipeline";
    pipeline_document.pipeline_nodes.push_back({.node_id = "Runtime",
                                                .type = aeg::pipeline::PipelineId("Smoke.RuntimePipeline"),
                                                .version = {1, 0, 0},
                                                .scope = aeg::pipeline::PipelineScope::kEngine});
    const auto pipeline_payload = aeg::asset::encode_pipeline_asset(pipeline_document);
    if (!pipeline_payload)
        return 3;
    pipeline.payload = pipeline_payload.value;
    tooling::CookedAsset world_pipeline = pipeline;
    world_pipeline.id = world_pipeline_id.value;
    aeg::pipeline::PipelineDocument world_document{};
    world_document.asset_uuid = world_pipeline_id.value.to_string();
    world_document.name = "SmokeWorldPipeline";
    world_document.pipeline_nodes.push_back({.node_id = "World",
                                             .type = aeg::pipeline::PipelineId("Smoke.WorldPipeline"),
                                             .version = {1, 0, 0},
                                             .scope = aeg::pipeline::PipelineScope::kWorld});
    const auto world_payload = aeg::asset::encode_pipeline_asset(world_document);
    if (!world_payload)
        return 3;
    world_pipeline.payload = world_payload.value;
    const auto scene_envelope = tooling::CookedAssetCodec{}.encode(scene);
    const auto pipeline_envelope = tooling::CookedAssetCodec{}.encode(pipeline);
    const auto world_envelope = tooling::CookedAssetCodec{}.encode(world_pipeline);
    if (!scene_envelope || !pipeline_envelope || !world_envelope)
        return 4;
    if (const auto result = tooling::AssetPackageWriter{}.write(
            root / "game.aegbin",
            {{scene.id, scene.kind, scene.schema_version, scene_envelope.value},
             {pipeline.id, pipeline.kind, pipeline.schema_version, pipeline_envelope.value},
             {world_pipeline.id, world_pipeline.kind, world_pipeline.schema_version, world_envelope.value}});
        !result)
        return 3;
    std::ofstream(root / "game.manifest") << "package = \"game.aegbin\"\n"
                                          << "build_id = \"smoke-build\"\n"
                                          << "target = \"windows-x64-vulkan\"\n"
                                          << "configuration = \"Debug\"\n";
    std::ifstream package_input(root / "game.aegbin", std::ios::binary);
    const std::string package_bytes((std::istreambuf_iterator<char>(package_input)), {});
    module::ModuleDefinition definition{};
    definition.descriptor.id = pipeline::ModuleId{"Smoke.GameModule"};
    definition.descriptor.name = "Smoke.GameModule";
    definition.pipelines.emplace_back([] { return std::make_unique<RuntimePipeline>(); });
    definition.pipelines.emplace_back([] { return std::make_unique<WorldRuntimePipeline>(); });
    pipeline::TypeCatalogEntry runtime_pipeline_entry{};
    runtime_pipeline_entry.key = {pipeline::AuthoringTypeKind::kPipeline,
                                  pipeline::PipelineId("Smoke.RuntimePipeline").value()};
    runtime_pipeline_entry.version = {1, 0, 0};
    runtime_pipeline_entry.owner_module = definition.descriptor.id;
    runtime_pipeline_entry.name = "Smoke.RuntimePipeline";
    runtime_pipeline_entry.display_name = "Smoke Runtime Pipeline";
    runtime_pipeline_entry.scopes = pipeline::to_mask(pipeline::PipelineScope::kEngine);
    runtime_pipeline_entry.domains = pipeline::to_mask(pipeline::FeatureDomain::kLogic);
    runtime_pipeline_entry.declaration = pipeline::PipelineAuthoringDeclaration{
        .stages = {pipeline::AuthoringStage{.id = pipeline::StageId("Smoke.RuntimeStage"),
                                            .name = "Smoke Runtime Stage",
                                            .phase = pipeline::PhaseId("VariableUpdate"),
                                            .execution_domain = pipeline::ExecutionDomain::kMainThread,
                                            .time_policy = pipeline::TimePolicy::kFrame,
                                            .editable = false}}};
    if (const auto result = definition.catalog.add(runtime_pipeline_entry); !result)
        return 4;
    auto world_entry = runtime_pipeline_entry;
    world_entry.key = {pipeline::AuthoringTypeKind::kPipeline, pipeline::PipelineId("Smoke.WorldPipeline").value()};
    world_entry.name = "Smoke.WorldPipeline";
    world_entry.display_name = "Smoke World Pipeline";
    world_entry.scopes = pipeline::to_mask(pipeline::PipelineScope::kWorld);
    auto& world_decl = std::get<pipeline::PipelineAuthoringDeclaration>(world_entry.declaration);
    world_decl.stages[0].id = pipeline::StageId("Smoke.WorldStage");
    if (const auto result = definition.catalog.add(world_entry); !result)
        return 4;
    module::ModuleMetadata module_metadata{};
    module_metadata.module = definition.descriptor;
    module_metadata.descriptor = definition.descriptor;
    module_metadata.catalog = definition.catalog;
    const auto module_contract = module::module_contract_sha256(module_metadata);
    if (!module_contract)
        return 4;
    aeg::asset::GameBuildManifest game_manifest{};
    game_manifest.project_uuid = "smoke-project";
    game_manifest.module_id = "Smoke.GameModule";
    game_manifest.build_id = "smoke-build";
    game_manifest.module_contract_digest = module_contract.value;
    const auto package_digest = aeg::module::module_content_sha256(package_bytes);
    if (!package_digest)
        return 5;
    game_manifest.package_digest = package_digest.value;
    game_manifest.startup_scene = aeg::asset::AssetId::parse("00000000-0000-4000-8000-000000000001").value;
    game_manifest.startup_pipelines.push_back(aeg::asset::AssetId::parse("00000000-0000-4000-8000-000000000002").value);
    game_manifest.startup_pipelines.push_back(world_pipeline_id.value);
    game_manifest.target = "windows-x64-vulkan";
    game_manifest.configuration = "Debug";
    game_manifest.package_name = "game.aegbin";
    const auto encoded_manifest = aeg::asset::encode_game_build_manifest(game_manifest);
    if (!encoded_manifest)
        return 6;
    std::ofstream binary_manifest(root / "game.manifest.bin", std::ios::binary);
    binary_manifest.write(reinterpret_cast<const char*>(encoded_manifest.value.data()),
                          static_cast<std::streamsize>(encoded_manifest.value.size()));
    if (!binary_manifest)
        return 7;
    binary_manifest.close();

    application::runtime::RuntimeBootstrap bootstrap;
    const application::runtime::RuntimeBuildIdentity identity{"smoke-build", "windows-x64-vulkan", "Debug"};
    application::runtime::RuntimeBootRequest request{};
    request.manifest_path = root / "game.manifest";
    request.max_frames = 2;
    request.trace_path = root / "trace.txt";
    application::runtime::RuntimeAssetResolver::UploadContext upload_context{};
    application::runtime::RuntimeFrameHostResources host_contract{};
    host_contract.upload_context = &upload_context;
    host_contract.require_resolved_render_resources = true;
    ResolverProbe resolver_probe;
    engine::EngineFrameDesc frame_desc_probe{};
    host_contract.apply_frame_desc(frame_desc_probe, &resolver_probe);
    if (frame_desc_probe.render_resource_resolver != &resolver_probe ||
        !frame_desc_probe.require_resolved_render_resources)
        return 8;
    request.frame_host = &host_contract;
    if (const auto result = bootstrap.initialize(request, std::move(definition), identity); !result) {
        std::cerr << "initialize failed: " << result.message << "\n";
        return 8;
    }
    if (bootstrap.startup_pipeline_count() != 2 || !bootstrap.startup_world_scope())
        return 9;
    if (!bootstrap.tick() || !bootstrap.tick() || bootstrap.frame_count() != 2 || g_runtime_task_runs != 2 ||
        g_world_task_runs != 2)
        return 10;
    const auto snapshot = bootstrap.startup_world_scope()->latest_snapshot();
    if (!snapshot || snapshot.value->frame_number() != 2 || snapshot.value->entity_count() != 1)
        return 12;
    if (!bootstrap.shutdown() || !fs::exists(root / "trace.txt"))
        return 11;
    fs::remove_all(root, error);
    std::cout << "Runtime boot smoke passed\n";
    return 0;
}
