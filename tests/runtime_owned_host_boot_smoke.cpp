#include "feature_clock_test_fixture.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
/** @file runtime_owned_host_boot_smoke.cpp
 *  @brief Verifies the Windows owned runtime host creates its shader/upload contract.
 */
#include <iostream>
#include <cstring>
#include <atomic>
#include <cmath>

#include "material_payload.hpp"

#include "frame_context.hpp"
#include "renderer.hpp"
#include "runtime_boot_fixture.hpp"
#include "runtime_asset_upload_host.hpp"
#include "../Engine/Shader/private/shader_pipeline_layout_access.hpp"
#include "runtime_feature_control.hpp"
#include "shadow_rendering.hpp"
#include "world_scope.hpp"
#include "Components/component_reflection.hpp"
#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "feature_graph_asset.hpp"
#include "feature_render_test_fixture.hpp"
#include "concurrency.hpp"
#include "render_feature_draw.hpp"
#include "asset_package_reader.hpp"
#include "game_build_manifest.hpp"
#include "module_bundle.hpp"
#include "module_api.h"

namespace {
double owned_clock_seconds = 0;
bool owned_clock_advances = true;
double owned_clock(void*) noexcept {
    if (owned_clock_advances) owned_clock_seconds += 1.0 / 60.0;
    return owned_clock_seconds;
}

std::vector<unsigned char> read_owned_pixels(const std::filesystem::path& path) {
    std::ifstream readback(path, std::ios::binary);
    std::string magic;
    unsigned width = 0, height = 0, maximum = 0;
    readback >> magic >> width >> height >> maximum;
    readback.get();
    if (magic != "P6" || width != 320 || height != 240 || maximum != 255) return {};
    std::vector<unsigned char> pixels(width * height * 3);
    readback.read(reinterpret_cast<char*>(pixels.data()), pixels.size());
    return readback ? pixels : std::vector<unsigned char>{};
}
}

int main(int argc, char** argv) {
    const bool no_scene = argc == 2 && std::string_view(argv[1]) == "--no-scene";
    const bool depth = argc == 2 && std::string_view(argv[1]) == "--depth";
    const bool depth_only = argc == 2 && std::string_view(argv[1]) == "--depth-only";
    const bool shadow_depth = argc == 2 && std::string_view(argv[1]) == "--shadow-depth";
    const bool shadow_draw = argc == 2 && std::string_view(argv[1]) == "--shadow-draw";
    const bool gbuffer = argc == 2 && std::string_view(argv[1]) == "--gbuffer";
    const bool post = argc == 2 && std::string_view(argv[1]) == "--feature-post-process";
    const bool clustered_shadow = argc == 2 && (std::string_view(argv[1]) == "--feature-clustered-shadow-publication" ||
        std::string_view(argv[1]) == "--feature-clustered-shadow-process" || std::string_view(argv[1]) == "--feature-clustered-shadow-cancel");
    const bool process_suspend = argc == 2 && (std::string_view(argv[1]) == "--feature-clustered-process" ||
        std::string_view(argv[1]) == "--feature-clustered-shadow-process");
    const bool process_cancel = argc == 2 && (std::string_view(argv[1]) == "--feature-clustered-cancel" ||
        std::string_view(argv[1]) == "--feature-clustered-shadow-cancel");
    const bool process_lifecycle = process_suspend || process_cancel;
    const bool clustered = clustered_shadow || process_lifecycle || (argc == 2 && std::string_view(argv[1]) == "--feature-clustered-publication");
    const std::string feature_asset_uuid = "00000000-0000-4000-8000-000000000002";
    owned_clock_advances = !clustered;
    std::array<aeg::concurrency::CompletionSource, 3> slow_started, slow_release;
    std::array<std::atomic<uint64_t>, 3> computed{};
    const bool lighting = post || (argc == 2 && std::string_view(argv[1]) == "--deferred-lighting");
    runtime_test::Fixture fixture{};
    aeg::scene::SceneAsset camera_scene({0x4000, 0x8000000000000001ull}, "OwnedDraw", 1);
    const aeg::scene::SceneEntityId camera_id{3, 4};
    aeg::reflection::TypeRegistry reflection;
    aeg::engine::components::Transform transform{};
    aeg::engine::components::Camera camera{};
    if (no_scene ? !runtime_test::prepare(fixture) :
        (!camera_scene.add_entity({camera_id, {}, "Camera", aeg::scene::SceneEntityFlags::kNone, {}}) ||
        !aeg::engine::components::register_render_component_reflection(reflection) ||
        !camera_scene.add_reflected_component(camera_id, aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName),
            reflection, aeg::reflection::TypeId(aeg::engine::components::kTransformName), &transform) ||
        !camera_scene.add_reflected_component(camera_id, aeg::ecs::ComponentTypeId(aeg::engine::components::kCameraName),
            reflection, aeg::reflection::TypeId(aeg::engine::components::kCameraName), &camera) ||
        !runtime_test::prepare(fixture, &camera_scene)))
        return 1;

    aeg::concurrency::CompletionSource executed;
    const auto scene_id = aeg::foundation::AssetId::parse("00000000-0000-4000-8000-000000000001");
    if (!no_scene) {
    auto source = render_feature_test::definition();
    source.owner_module = aeg::feature::ModuleIdentity(fixture.definition.descriptor.id.value());
    source.execute = [&executed, depth, depth_only, shadow_depth, shadow_draw, gbuffer, lighting, post](const aeg::feature::InvocationToken& token, const aeg::feature::StateSnapshot&,
                                std::span<const uint8_t>, std::span<const uint8_t>) {
        using namespace aeg::feature;
        const RenderTextureReference target{RenderResourceId("Renderer.SceneColor"), 1, true};
        RenderIntent intent{};
        RenderDrawScene draw{{aeg::renderer::kRuntimeSceneResourceId, 1}, target,
            RenderAttachmentLoad::kClear, {0.f, 1.f, 0.f, 1.f}};
        if (depth || shadow_draw) draw.depth = RenderDepthAttachment{{RenderResourceId("Renderer.SceneDepth"), 1, true},
                                                     RenderAttachmentLoad::kClear, 0.4f};
        if (shadow_draw) draw.directional_shadow = RenderTextureReference{RenderResourceId("Renderer.ShadowDepth"), 1, true};
        intent.operations = {draw};
        if (gbuffer || lighting) {
            RenderDrawGBufferScene mrt{};
            mrt.scene = {aeg::renderer::kRuntimeSceneResourceId, 1};
            mrt.albedo.target = {RenderResourceId("Renderer.GBuffer.Albedo"), 1, true};
            mrt.normal.target = {RenderResourceId("Renderer.GBuffer.Normal"), 1, true};
            mrt.material.target = {RenderResourceId("Renderer.GBuffer.Material"), 1, true};
            mrt.depth = {{RenderResourceId("Renderer.SceneDepth"), 1, true}, RenderAttachmentLoad::kClear, .4f};
            if (post) mrt.depth.clear_depth = 1.f;
            intent.operations = {RenderClearColor{target, {0, 1, 0, 1}}, mrt};
            intent.depth_output = mrt.depth.target;
            if (lighting) {
                RenderDeferredLightingScene consumer{};
                consumer.scene = mrt.scene;
                consumer.albedo = mrt.albedo.target;
                consumer.normal = mrt.normal.target;
                consumer.depth = mrt.depth.target;
                consumer.hdr.target = {RenderResourceId("Renderer.SceneLighting"), 1, true};
                intent.operations.push_back(consumer);
                if (post) {
                    RenderPostProcessScene output{};
                    output.scene = consumer.scene;
                    output.hdr = consumer.hdr.target;
                    output.depth = consumer.depth;
                    output.output.target = target;
                    output.settings = RenderPostProcessSettings{};
                    output.settings->sky_zenith = output.settings->sky_horizon = output.settings->sky_ground = {1, 0, 0};
                    output.settings->sky_enabled = true;
                    output.settings->exposure = 20;
                    intent.operations = {mrt, consumer};
                    intent.operations.push_back(output);
                }
            }
        }
        if (depth_only || shadow_depth) {
            const RenderDepthAttachment attachment{{RenderResourceId("Renderer.SceneDepth"), 1, true},
                RenderAttachmentLoad::kClear, .4f};
            intent.operations = {RenderClearColor{target, {0.f, 1.f, 0.f, 1.f}},
                RenderDrawDepthScene{{aeg::renderer::kRuntimeSceneResourceId, 1}, attachment}};
            intent.depth_output = attachment.target;
            if (shadow_depth) {
                RenderDrawDepthScene shadow{{aeg::renderer::kRuntimeSceneResourceId, 1},
                    {{RenderResourceId("Renderer.ShadowDepth"), 1, true}, RenderAttachmentLoad::kClear, .6f}};
                shadow.projection = RenderDepthSceneProjection::kDirectionalShadow;
                intent.operations.push_back(shadow);
                intent.depth_output = shadow.depth.target;
            }
        }
        intent.final_output = target;
        StateDelta delta{token.state_version};
        delta.render_intent = std::move(intent);
        (void)executed.complete();
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    auto consumer = source;
    if (clustered) {
        using namespace aeg::feature;
        source.id = FeatureDefinitionId("Owned.Cull");
        source.contract.definition_id = source.id;
        source.contract.parameters.clear();
        if (process_lifecycle) {
            source.contract.execution = ExecutionKind::kProcess;
            source.contract.state = StatePolicy::kProcessState;
            source.contract.has_continuation = true;
        }
        source.contract.ports = {{PortId("List"), PortDirection::kOutput, kRenderLightListSchema, 1}};
        source.execute = [&computed, clustered_shadow](const InvocationToken& token, const StateSnapshot&,
            std::span<const uint8_t>, std::span<const uint8_t>) {
            const RenderSceneReference scene{aeg::renderer::kRuntimeSceneResourceId, 1};
            const RenderTextureReference depth{RenderResourceId("Renderer.SceneDepth"), 1, true};
            RenderDrawDepthScene draw{scene, {depth, RenderAttachmentLoad::kClear, 1.f}};
            RenderLightCullingScene cull{scene, depth,
                {RenderResourceId("Owned.List"), token.state_version.value(), PortId("List")}};
            StateDelta delta{token.state_version};
            delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {draw};
            if (clustered_shadow) {
                auto shadow = draw;
                shadow.projection = RenderDepthSceneProjection::kDirectionalShadow;
                shadow.depth.target = {RenderResourceId("Renderer.ShadowDepth"), 1, true};
                cull.directional_shadow = shadow.depth.target;
                delta.render_intent->operations.push_back(shadow);
            }
            delta.render_intent->operations.push_back(cull);
            const auto encoded = encode_render_light_list_reference({cull.output.id, cull.output.version,
                token.invocation_id, token.generation});
            if (!encoded) return aeg::foundation::ResultValue<StateDelta>{encoded.result, {}};
            delta.output_payload = encoded.value;
            computed[0].store(cull.output.version, std::memory_order_release);
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
        };
        consumer.id = FeatureDefinitionId("Owned.Clustered");
        consumer.contract.definition_id = consumer.id;
        consumer.contract.ports = {{PortId("List"), PortDirection::kInput, kRenderLightListSchema, 1}};
        consumer.execute = [schema = consumer.contract.parameters, &computed, &slow_started, &slow_release, process_lifecycle, clustered_shadow]
            (const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t> input,
             std::span<const uint8_t> parameters) {
            // Preserve the genuine preceding demand and receipt when the Process
            // has no fresh delivery. Runtime decides whether it can be published.
            if (process_lifecycle && input.empty())
                return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), StateDelta{token.state_version}};
            const auto list = decode_render_light_list_reference(input);
            const auto decoded = decode_feature_parameters(parameters, schema);
            if (!list || !decoded) return aeg::foundation::ResultValue<StateDelta>{!list ? list.result : decoded.result, {}};
            const auto version = list.value.version;
            if (version < 1 || version > 3)
                return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::invalid_state("unexpected owned render wave"), {}};
            const bool last = decoded.value.front().payload.front() == 2;
            if (last && version != 2) {
                static_cast<void>(slow_started[version - 1].complete());
                static_cast<void>(slow_release[version - 1].completion().wait());
            }
            RenderDrawClusteredScene draw{};
            draw.scene = {aeg::renderer::kRuntimeSceneResourceId, 1};
            draw.light_list = list.value;
            draw.hdr.target = {RenderResourceId("Renderer.SceneLighting"), 1, true};
            // The final consumer loads the sibling's HDR clear. Both outputs
            // remain real data dependencies of the final post-process frame.
            draw.hdr.load = last ? RenderAttachmentLoad::kLoad : RenderAttachmentLoad::kClear;
            draw.hdr.clear_color = version == 3 ? std::array{.4f, .2f, .6f, 1.f} : std::array{.25f, .5f, .75f, 1.f};
            draw.depth = {{RenderResourceId("Renderer.SceneDepth"), 1, true}, RenderAttachmentLoad::kLoad, 1.f};
            if (clustered_shadow) draw.directional_shadow = RenderTextureReference{RenderResourceId("Renderer.ShadowDepth"), 1, true};
            StateDelta delta{token.state_version};
            delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {draw};
            // The first complete wave has no PP operation: clustered demand
            // must bind HDR itself, rather than accidentally relying on PP.
            if (last && version != 1) {
                RenderPostProcessScene output{};
                output.scene = draw.scene;
                output.hdr = draw.hdr.target;
                output.depth = draw.depth.target;
                output.output.target = {RenderResourceId("Renderer.SceneColor"), 1, true};
                output.settings = RenderPostProcessSettings{};
                output.settings->tone_mapping = true;
                output.settings->exposure = version == 3 ? 2.f : 1.f;
                delta.render_intent->operations.push_back(output);
                delta.render_intent->final_output = output.output.target;
            }
            computed[last ? 2 : 1].store(version, std::memory_order_release);
            return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
        };
    }
    aeg::feature::FeatureRegistry features{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(features, source) || !aeg::module::add_feature_definition(fixture.definition, source)) return 12;
    auto graph_document = render_feature_test::document(clustered ? consumer : source);
    if (clustered) {
        if (!aeg::feature::test::register_clock_test_definition(features, consumer) || !aeg::module::add_feature_definition(fixture.definition, consumer)) return 12;
        graph_document.nodes.insert(graph_document.nodes.begin(), {aeg::feature::FeatureNodeId("Cull"), source.id, source.version});
        for (uint64_t channel : {1ull, 2ull}) graph_document.edges.push_back({aeg::feature::FeatureNodeId("Cull"),
            aeg::feature::PortId("List"), aeg::feature::FeatureNodeId(channel), aeg::feature::PortId("List"),
            aeg::feature::EdgeKind::kData, source.contract.clock.id, consumer.contract.clock.id, 1, 1, 0});
    } else graph_document.nodes.resize(1);
    const auto graph_id = aeg::foundation::AssetId::parse(feature_asset_uuid);
    graph_document.asset_uuid = graph_id.value.to_string();
    graph_document.contract_digest = features.contract_digest();
    const auto graph_payload = aeg::asset::encode_feature_graph_asset(graph_document);
    const auto package = aeg::asset::AssetPackageReader::open(fixture.root / "game.aegbin");
    auto cooked_scene = package.value.read_cooked(scene_id.value);
    std::vector<aeg::tooling::AssetPackageEntry> entries;
    if (post || clustered) {
        using namespace aeg;
        const auto mesh_id = foundation::AssetId::parse("10000000-0000-4000-8000-000000000001");
        const auto material_id = foundation::AssetId::parse("10000000-0000-4000-8000-000000000002");
        const auto texture_id = foundation::AssetId::parse("10000000-0000-4000-8000-000000000003");
        const auto shader_id = foundation::AssetId::parse("11111111-1111-1111-1111-111111111111");
        if (!mesh_id || !material_id || !texture_id || !shader_id || !cooked_scene) return 23;
        const scene::SceneEntityId mesh_entity{5, 6};
        engine::components::Transform mesh_transform{};
        mesh_transform.translation = {0, 0, -2};
        engine::components::MeshRenderer mesh_renderer{};
        mesh_renderer.mesh_asset_id = 1;
        mesh_renderer.material_asset_id = 2;
        mesh_renderer.bounds_radius = 1;
        if (!camera_scene.add_entity({mesh_entity, {}, "PostProcess.Mesh", scene::SceneEntityFlags::kNone, {}}) ||
            !camera_scene.add_reflected_component(mesh_entity, ecs::ComponentTypeId(engine::components::kTransformName),
                reflection, reflection::TypeId(engine::components::kTransformName), &mesh_transform) ||
            !camera_scene.add_reflected_component(mesh_entity, ecs::ComponentTypeId(engine::components::kMeshRendererName),
                reflection, reflection::TypeId(engine::components::kMeshRendererName), &mesh_renderer)) return 23;
        if (clustered) {
            const scene::SceneEntityId light_entity{7, 8};
            engine::components::Transform light_transform{};
            light_transform.translation = {0, 0, -1};
            engine::components::PointLight point{};
            point.range = 2.f;
            if (!camera_scene.add_entity({light_entity, {}, "Clustered.Point", scene::SceneEntityFlags::kNone, {}}) ||
                !camera_scene.add_reflected_component(light_entity, ecs::ComponentTypeId(engine::components::kTransformName),
                    reflection, reflection::TypeId(engine::components::kTransformName), &light_transform) ||
                !camera_scene.add_reflected_component(light_entity, ecs::ComponentTypeId(engine::components::kPointLightName),
                    reflection, reflection::TypeId(engine::components::kPointLightName), &point)) return 23;
        }
        const scene::SceneEntityId caster_entity{9, 10};
        if (clustered_shadow) {
            auto caster_transform = mesh_transform;
            caster_transform.translation = {-.30f, 0, -1.4f};
            caster_transform.scale = {.15f, .15f, 1};
            const scene::SceneEntityId directional_entity{11, 12};
            engine::components::Transform directional_transform{};
            engine::components::DirectionalLight directional{};
            directional.direction = {.5f, 0, -1}; // Extractor consumes this world-space field.
            directional.intensity = .8f;
            directional.casts_shadow = true;
            if (!camera_scene.add_entity({caster_entity, {}, "Clustered.Caster", scene::SceneEntityFlags::kNone, {}}) ||
                !camera_scene.add_reflected_component(caster_entity, ecs::ComponentTypeId(engine::components::kTransformName),
                    reflection, reflection::TypeId(engine::components::kTransformName), &caster_transform) ||
                !camera_scene.add_reflected_component(caster_entity, ecs::ComponentTypeId(engine::components::kMeshRendererName),
                    reflection, reflection::TypeId(engine::components::kMeshRendererName), &mesh_renderer) ||
                !camera_scene.add_entity({directional_entity, {}, "Clustered.Directional", scene::SceneEntityFlags::kNone, {}}) ||
                !camera_scene.add_reflected_component(directional_entity, ecs::ComponentTypeId(engine::components::kTransformName),
                    reflection, reflection::TypeId(engine::components::kTransformName), &directional_transform) ||
                !camera_scene.add_reflected_component(directional_entity, ecs::ComponentTypeId(engine::components::kDirectionalLightName),
                    reflection, reflection::TypeId(engine::components::kDirectionalLightName), &directional)) return 45;
        }
        serialization::BinaryWriter archive;
        if (!scene::SceneSerializer::serialize(camera_scene, archive)) return 23;
        asset::ScenePayload payload{};
        payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(archive.data().data()),
            reinterpret_cast<const std::byte*>(archive.data().data() + archive.data().size()));
        asset::SceneAssetReference mesh_reference{};
        for (size_t index = 0; index < 8; ++index) {
            mesh_reference.scene_entity_uuid[index] = static_cast<uint8_t>(mesh_entity.high() >> (56 - index * 8));
            mesh_reference.scene_entity_uuid[index + 8] = static_cast<uint8_t>(mesh_entity.low() >> (56 - index * 8));
        }
        mesh_reference.component_type = "MeshRenderer"; // Cooked relocation schema name.
        mesh_reference.component_version = 1;
        mesh_reference.field_id = reflection::FieldId("mesh_asset_id").value();
        mesh_reference.asset = mesh_id.value;
        mesh_reference.kind = foundation::AssetKind::kMesh;
        auto material_reference = mesh_reference;
        material_reference.field_id = reflection::FieldId("material_asset_id").value();
        material_reference.asset = material_id.value;
        material_reference.kind = foundation::AssetKind::kMaterial;
        payload.references = {mesh_reference, material_reference};
        if (clustered_shadow) {
            for (auto reference : {mesh_reference, material_reference}) {
                for (size_t index = 0; index < 8; ++index) {
                    reference.scene_entity_uuid[index] = static_cast<uint8_t>(caster_entity.high() >> (56 - index * 8));
                    reference.scene_entity_uuid[index + 8] = static_cast<uint8_t>(caster_entity.low() >> (56 - index * 8));
                }
                payload.references.push_back(std::move(reference));
            }
        }
        const auto encoded_scene = asset::encode_scene_payload(payload);
        if (!encoded_scene) return 23;
        cooked_scene.value.payload = encoded_scene.value;
        cooked_scene.value.runtime_dependencies = {mesh_id.value, material_id.value};

        // Existing cooked layout: header + position/normal/UV vertices + uint32 indices.
        const std::array<uint32_t, 4> mesh_header{4, 6, 32, 0};
        const std::array<std::array<float, 8>, 4> vertices{{
            {-.5f, -.5f, 0, 0, 0, 1, 0, 0}, {.5f, -.5f, 0, 0, 0, 1, 1, 0},
            {.5f, .5f, 0, 0, 0, 1, 1, 1}, {-.5f, .5f, 0, 0, 0, 1, 0, 1}}};
        const std::array<uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
        std::vector<std::byte> mesh_bytes(sizeof(mesh_header) + sizeof(vertices) + sizeof(indices));
        std::memcpy(mesh_bytes.data(), mesh_header.data(), sizeof(mesh_header));
        std::memcpy(mesh_bytes.data() + sizeof(mesh_header), vertices.data(), sizeof(vertices));
        std::memcpy(mesh_bytes.data() + sizeof(mesh_header) + sizeof(vertices), indices.data(), sizeof(indices));
        const std::array<uint32_t, 3> texture_header{1, 1, 4};
        std::vector<std::byte> texture_bytes(sizeof(texture_header) + 4);
        std::memcpy(texture_bytes.data(), texture_header.data(), sizeof(texture_header));
        texture_bytes[13] = texture_bytes[15] = std::byte{255}; // Pure green, alpha one.
        asset::MaterialPayload material{};
        material.shader = shader_id.value;
        material.textures = {{1, texture_id.value}};
        const auto encoded_material = asset::encode_material_payload(material);
        if (!encoded_material) return 23;
        const std::array assets{
            tooling::CookedAsset{mesh_id.value, foundation::AssetKind::kMesh, 1, {}, std::move(mesh_bytes)},
            tooling::CookedAsset{material_id.value, foundation::AssetKind::kMaterial, 1, {texture_id.value}, encoded_material.value},
            tooling::CookedAsset{texture_id.value, foundation::AssetKind::kTexture, 1, {}, std::move(texture_bytes)}};
        for (const auto& asset : assets) {
            const auto envelope = tooling::CookedAssetCodec{}.encode(asset);
            if (!envelope) return 23;
            entries.push_back({asset.id, asset.kind, asset.schema_version, envelope.value});
        }
    }
    const auto scene_envelope = aeg::tooling::CookedAssetCodec{}.encode(cooked_scene.value);
    aeg::tooling::CookedAsset cooked_graph{};
    cooked_graph.id = graph_id.value;
    cooked_graph.kind = aeg::foundation::AssetKind::kFeatureGraph;
    cooked_graph.schema_version = 1;
    cooked_graph.payload = graph_payload.value;
    const auto graph_envelope = aeg::tooling::CookedAssetCodec{}.encode(cooked_graph);
    if (!graph_payload || !package || !cooked_scene || !scene_envelope || !graph_envelope) return 13;
    entries.push_back({scene_id.value, aeg::foundation::AssetKind::kScene, 1, scene_envelope.value});
    entries.push_back({graph_id.value, aeg::foundation::AssetKind::kFeatureGraph, 1, graph_envelope.value});
    if (!aeg::tooling::AssetPackageWriter{}.write(fixture.root / "game.aegbin", std::move(entries))) return 13;
    aeg::module::ModuleMetadata metadata{};
    metadata.schema_version = aeg::module::kModuleMetadataSchemaVersion;
    metadata.module = fixture.definition.descriptor;
    metadata.feature_catalog = fixture.definition.feature_catalog;
    metadata.abi_major = AEG_MODULE_ABI_MAJOR;
    metadata.abi_minor = AEG_MODULE_ABI_MINOR;
    metadata.sdk_contract_major = aeg::module::kModuleSdkContractMajor;
    metadata.sdk_contract_minor = aeg::module::kModuleSdkContractMinor;
    const auto digest = aeg::module::module_contract_sha256(metadata);
    std::ifstream package_file(fixture.root / "game.aegbin", std::ios::binary);
    const std::string package_bytes((std::istreambuf_iterator<char>(package_file)), {});
    const auto package_digest = aeg::module::module_content_sha256(package_bytes);
    aeg::asset::GameBuildManifest manifest{};
    manifest.project_uuid = "owned-draw";
    manifest.module_id = "Task1.GameModule";
    manifest.build_id = fixture.identity.build_id;
    manifest.target = fixture.identity.target;
    manifest.configuration = fixture.identity.configuration;
    manifest.module_contract_digest = digest.value;
    manifest.package_digest = package_digest.value;
    manifest.startup_scene = scene_id.value;
    manifest.startup_feature_graphs = {graph_id.value};
    manifest.package_name = "game.aegbin";
    manifest.module_manifest_name = "module/module.manifest.json";
    manifest.module_manifest_sha256 = "test-module-manifest";
    const auto encoded = aeg::asset::encode_game_build_manifest(manifest);
    if (!digest || !package_digest || !encoded) return 17;
    std::ofstream(fixture.root / "game.manifest") << "package = \"game.aegbin\"\nbuild_id = \"task1\"\n";
    std::ofstream binary(fixture.root / "game.manifest.bin", std::ios::binary);
    binary.write(reinterpret_cast<const char*>(encoded.value.data()), encoded.value.size());
    binary.close();
    }

    const auto shader_id = aeg::foundation::AssetId::parse("11111111-1111-1111-1111-111111111111");
    if (!shader_id) {
        runtime_test::cleanup(fixture);
        return 2;
    }

    aeg::application::runtime::RuntimeBootRequest request{};
    request.manifest_path = fixture.root / "game.manifest";
    request.package_path = fixture.root / "game.aegbin";
    request.owned_host = true;
    request.width = 320;
    request.height = 240;
    request.material_shader = shader_id.value;
    request.startup_scene = scene_id.value;
    request.clock.now_seconds = &owned_clock;
    request.readback_path = fixture.root / "draw.ppm";
#ifdef AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH
    request.material_shader_package = AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH;
#else
    runtime_test::cleanup(fixture);
    return 3;
#endif

    aeg::application::runtime::RuntimeBootstrap bootstrap;
    if (process_lifecycle && (aeg::application::runtime::RuntimeFeatureControl::capture_render_outputs(bootstrap) ||
        aeg::application::runtime::RuntimeFeatureControl::inspect_node(bootstrap, "uninitialized", aeg::feature::FeatureNodeId("Cull")) ||
        aeg::application::runtime::RuntimeFeatureControl::control_process(bootstrap, {}))) return 31;
    struct ReleaseSlowOnExit final {
        decltype(slow_release)& gates;
        ~ReleaseSlowOnExit() { for (auto& gate : gates) static_cast<void>(gate.complete()); }
    } release_slow_on_exit{slow_release};
    const auto initialized = bootstrap.initialize(request, std::move(fixture.definition), fixture.identity);
    if (!initialized) {
        std::cerr << "owned host initialize failed: " << initialized.message << '\n';
        runtime_test::cleanup(fixture);
        return 4;
    }
    const auto* host = bootstrap.frame_host_resources();
    if (host == nullptr || !host->require_resolved_render_resources || host->upload_context == nullptr ||
        host->upload_context->device == nullptr || host->upload_context->frame_context == nullptr ||
        host->upload_context->graph == nullptr || host->upload_context->uploader == nullptr ||
        host->upload_context->texture_uploader == nullptr || !host->upload_context->material_pipeline.is_valid() ||
        !host->upload_context->material_binding_layout.is_valid() ||
        host->upload_context->material_shader != shader_id.value || !host->upload_context->deferred_lighting_resolver ||
        !host->upload_context->shadow_resolver || !host->upload_context->light_culling_resolver ||
        !host->upload_context->post_process_resolver || !host->upload_context->shadow_pipeline.is_valid() ||
        !host->upload_context->clustered_pipeline.is_valid() || !host->upload_context->depth_pipeline.is_valid() ||
        !host->upload_context->gbuffer_pipeline.is_valid()) {
        runtime_test::cleanup(fixture);
        return 5;
    }

    auto& upload = *host->upload_context;
    const aeg::renderer::RenderMaterialAttachmentContract clustered_contract{
        aeg::rhi::Format::kRgba16Float, aeg::rhi::SampleCount::kX1, aeg::rhi::Format::kD32Float};
    if (upload.clustered_pipeline_contract != clustered_contract ||
        !upload.clustered_profile_contract.is_valid(aeg::renderer::RendererProfile::kForwardClustered) ||
        upload.clustered_profile_contract.attachment != clustered_contract || !upload.unshadowed_clustered_variant ||
        !upload.unshadowed_clustered_variant->pipeline.is_valid() ||
        upload.unshadowed_clustered_variant->pipeline == upload.clustered_pipeline ||
        upload.unshadowed_clustered_variant->attachment_contract != clustered_contract ||
        !upload.unshadowed_clustered_variant->profile_contract.is_valid(aeg::renderer::RendererProfile::kForwardClustered)) return 22;
    if (upload.depth_pipeline_contract != aeg::renderer::RenderMaterialAttachmentContract{
        aeg::rhi::Format::kUnknown, aeg::rhi::SampleCount::kX1, aeg::rhi::Format::kD32Float}) return 19;
    if (!upload.depth_shader_layout || !upload.depth_shader_layout->is_valid() ||
        !upload.depth_profile_contract.is_valid(aeg::renderer::RendererProfile::kShadow) ||
        upload.depth_profile_contract.attachment != upload.depth_pipeline_contract ||
        upload.depth_profile_contract.shader.layout_hash !=
            aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(*upload.depth_shader_layout) ||
        upload.depth_profile_contract.shader.cache_epoch !=
            aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*upload.depth_shader_layout) ||
        upload.depth_profile_contract.cache_epoch != upload.depth_profile_contract.shader.cache_epoch) return 45;
    if (upload.shadow_pipeline_contract != aeg::renderer::RenderMaterialAttachmentContract{
        upload.material_attachment_contract.color_format, aeg::rhi::SampleCount::kX1, aeg::rhi::Format::kD32Float}) return 20;
    if (!upload.shadow_shader_layout || !upload.shadow_shader_layout->is_valid() ||
        !upload.shadow_shader_contract.is_valid() ||
        upload.shadow_shader_contract.layout_hash !=
            aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(*upload.shadow_shader_layout) ||
        upload.shadow_shader_contract.cache_epoch !=
            aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*upload.shadow_shader_layout) ||
        upload.shadow_pipeline_cache_epoch == 0 || upload.shadow_pipeline_cache_epoch != upload.variant_cache_epoch)
        return 47;
    if (upload.gbuffer_pipeline_contract != aeg::renderer::RenderMaterialAttachmentContract{
        aeg::rhi::Format::kRgba16Float, aeg::rhi::SampleCount::kX1, aeg::rhi::Format::kD32Float,
        {aeg::rhi::Format::kRgba16Float, aeg::rhi::Format::kRgba8Unorm}}) return 21;
    if (!upload.gbuffer_shader_layout || !upload.gbuffer_shader_layout->is_valid() ||
        !upload.gbuffer_profile_contract.is_valid(aeg::renderer::RendererProfile::kGBuffer) ||
        upload.gbuffer_profile_contract.attachment != upload.gbuffer_pipeline_contract ||
        upload.gbuffer_profile_contract.shader.layout_hash !=
            aeg::shader::detail::ShaderPipelineLayoutAccess::contract_hash(*upload.gbuffer_shader_layout) ||
        upload.gbuffer_profile_contract.shader.cache_epoch !=
            aeg::shader::detail::ShaderPipelineLayoutAccess::cache_epoch(*upload.gbuffer_shader_layout) ||
        upload.gbuffer_profile_contract.cache_epoch != upload.variant_cache_epoch) return 46;
    if (!upload.depth_attachment_variant || !upload.depth_attachment_variant->pipeline.is_valid() ||
        upload.depth_attachment_variant->attachment_contract.color_format != upload.material_attachment_contract.color_format ||
        upload.depth_attachment_variant->attachment_contract.depth_stencil_format != aeg::rhi::Format::kD32Float ||
        upload.material_attachment_contract.depth_stencil_format != aeg::rhi::Format::kUnknown) return 18;
    auto& frame_context = *host->frame_context;
    if (!frame_context.begin_frame()) {
        runtime_test::cleanup(fixture);
        return 6;
    }
    const auto* world_scope = bootstrap.startup_world_scope();
    const auto world = world_scope != nullptr && world_scope->handle().is_valid() ? world_scope->handle()
                                                                                  : aeg::ecs::WorldHandle{0, 1};
    if (!world.is_valid()) {
        (void)frame_context.abort_frame();
        runtime_test::cleanup(fixture);
        return 7;
    }
    aeg::renderer::RenderSceneSnapshot snapshot{};
    snapshot.frame_number = frame_context.frame_number();
    snapshot.world_revision = 1;
    snapshot.world = world;
    snapshot.view.world = snapshot.world;
    snapshot.view.id = aeg::renderer::make_render_view_id(snapshot.world, 0);
    snapshot.view.output_extent = {320, 240};
    snapshot.view.camera.view = aeg::math::identity();
    snapshot.view.camera.projection = aeg::math::identity();
    snapshot.view.camera.near_plane = 0.1f;
    snapshot.view.camera.far_plane = 100.0f;
    aeg::renderer::RenderShadowView shadow{};
    shadow.view = snapshot.view;
    shadow.constants.clip_to_shadow = aeg::math::identity();
    shadow.constants.parameters = {1.0f, 1.0f, 0.0f, 0.0f};
    {
        // These frame resources retain the owned shader-resource implementations
        // through callbacks. Release the values before Bootstrap::shutdown()
        // tears down the device that owns their pipelines.
        const auto deferred = upload.deferred_lighting_resolver(snapshot.view);
        const auto shadow_resource = upload.shadow_resolver(shadow);
        const auto culling = upload.light_culling_resolver(snapshot, shadow);
        const auto post_process = upload.post_process_resolver(snapshot.view);
        if (!deferred || !deferred.value.pipeline.is_valid() || !shadow_resource ||
            !shadow_resource.value.buffer.is_valid() || !culling || !culling.value.pipeline.is_valid() ||
            !post_process || !post_process.value.pipeline.is_valid()) {
            (void)frame_context.abort_frame();
            runtime_test::cleanup(fixture);
            return 8;
        }
        const aeg::renderer::RenderMaterialAttachmentContract hdr_contract{
            aeg::rhi::Format::kRgba16Float, aeg::rhi::SampleCount::kX1, aeg::rhi::Format::kUnknown};
        if (deferred.value.attachment_contract != hdr_contract ||
            !deferred.value.shadow_pipeline.is_valid() || deferred.value.shadow_pipeline_contract != hdr_contract)
            return 22;
    }
    if (!frame_context.abort_frame()) {
        runtime_test::cleanup(fixture);
        return 9;
    }
    if (const auto tick = bootstrap.tick(); !tick) {
        std::cerr << "owned first tick failed: " << tick.message << '\n';
        runtime_test::cleanup(fixture);
        return 10;
    }
    if (clustered) {
        const auto tick = [&] {
            const auto result = bootstrap.tick();
            if (!result) std::cerr << "owned clustered frame failed: " << result.message << '\n';
            return bool(result);
        };
        const auto wave_visible = [&](uint64_t version, bool require_live = true) {
            aeg::feature::FeatureInstanceId producer{};
            std::vector<aeg::feature::FeatureInstanceId> consumers;
            bool shadow_work = false;
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(*upload.graph).pass_plans) {
                if ((require_live && !pass.live) || !pass.feature_work) continue;
                const auto& work = *pass.feature_work;
                if (work.invocation.state_version.value() != version || !work.node_id.is_valid() ||
                    work.owner_module != aeg::feature::ModuleIdentity("Task1.GameModule")) return false;
                if (work.feature_definition_id == aeg::feature::FeatureDefinitionId("Owned.Cull")) {
                    if (producer.is_valid() && producer != work.instance_id) return false;
                    producer = work.instance_id;
                    if (pass.live && work.operation_index == 1) shadow_work = true;
                } else if (work.feature_definition_id == aeg::feature::FeatureDefinitionId("Owned.Clustered")) {
                    if (std::find(consumers.begin(), consumers.end(), work.instance_id) == consumers.end())
                        consumers.push_back(work.instance_id);
                } else return false;
            }
            return producer.is_valid() && consumers.size() == 2 && (!clustered_shadow || !require_live || shadow_work) &&
                std::find(consumers.begin(), consumers.end(), producer) == consumers.end();
        };
        const auto pump_until = [&](const auto& ready) {
            for (unsigned attempt = 0; attempt != 80; ++attempt) {
                if (!tick()) return false;
                if (ready()) return true;
                static_cast<void>(aeg::concurrency::sleep_for(1'000'000));
            }
            std::cerr << "owned clustered phase timed out; computed versions " << computed[0].load() << ','
                << computed[1].load() << ',' << computed[2].load() << '\n';
            std::cerr << "graph passes " << aeg::render_graph::detail::RenderGraphExecution::plan(*upload.graph).pass_plans.size() << '\n';
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(*upload.graph).pass_plans) if (pass.feature_work)
                std::cerr << "work node " << pass.feature_work->node_id.value() << " state "
                    << pass.feature_work->invocation.state_version.value() << " live " << pass.live << '\n';
            return false;
        };
        if (!pump_until([&] { return slow_started[0].completion().is_complete(); })) return 24;
        const auto first_pixels = read_owned_pixels(request.readback_path);
        if (first_pixels.empty() || !std::all_of(first_pixels.begin(), first_pixels.end(),
            [](unsigned char value) { return value == 0; }) || wave_visible(1)) return 25;
        static_cast<void>(slow_release[0].complete());
        if (!pump_until([&] { return wave_visible(1, false); })) return 26;

        const bool srgb = upload.material_attachment_contract.color_format == aeg::rhi::Format::kRgba8Srgb;
        const auto encode = [srgb](double value) {
            if (srgb) value = value <= .0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - .055;
            return static_cast<int>(std::lround(value * 255));
        };
        const auto pixels_match = [&](uint64_t version) {
            const auto pixels = read_owned_pixels(request.readback_path);
            if (pixels.empty()) return false;
            const auto check = [&](unsigned x, unsigned y, std::array<int, 3> expected) {
                const auto offset = (y * 320 + x) * 3;
                for (unsigned channel = 0; channel != 3; ++channel) {
                    if (std::abs(int(pixels[offset + channel]) - expected[channel]) <= 2) continue;
                    std::cerr << "owned clustered wave " << version << " pixel " << x << ',' << y
                        << " channel " << channel << " expected " << expected[channel] << " actual "
                        << int(pixels[offset + channel]) << '\n';
                    return false;
                }
                return true;
            };
            const double exposure = version == 3 ? 2.0 : 1.0;
            for (unsigned y : {119u, 120u}) for (unsigned x : {159u, 160u, 195u}) {
                if (!clustered_shadow && x == 195) continue;
                const double tangent = std::tan(double(camera.vertical_fov_radians) / 2.0);
                const double sx = 2 * tangent * (320.0 / 240) * (2 * (x + .5) / 320 - 1);
                const double sy = -2 * tangent * (2 * (y + .5) / 240 - 1);
                const double distance = std::sqrt(sx * sx + sy * sy + 1);
                const double attenuation = 1 - distance / 2;
                const double visibility = clustered_shadow && x == 195 ? 1.0 : 0.0;
                const double hdr = .05 + attenuation * attenuation / distance + .8 / std::sqrt(1.25) * visibility;
                const double mapped = exposure * hdr / (1 + exposure * hdr);
                if (!check(x, y, {0, encode(mapped), 0})) return false;
            }
            const auto background = version == 3 ? std::array{.4, .2, .6} : std::array{.25, .5, .75};
            std::array<int, 3> expected{};
            for (unsigned channel = 0; channel != 3; ++channel)
                expected[channel] = encode(exposure * background[channel] / (1 + exposure * background[channel]));
            for (unsigned y : {8u, 231u}) for (unsigned x : {8u, 311u})
                if (!check(x, y, expected)) return false;
            return true;
        };
        owned_clock_seconds += 1.0 / 60.0;
        if (!pump_until([&] { return wave_visible(2); }) || !pixels_match(2)) return 27;
        if (process_lifecycle) {
            using namespace aeg::feature;
            using namespace aeg::application::runtime;
            const auto inspect = [&](FeatureNodeId node) {
                const auto snapshot = RuntimeFeatureControl::inspect_node(bootstrap, feature_asset_uuid, node);
                if (!snapshot || snapshot.value.owners.size() != 1)
                    throw std::runtime_error("Owned authored node did not resolve its live owner");
                return snapshot.value;
            };
            const auto producer = inspect(FeatureNodeId("Cull"));
            const auto first_consumer = inspect(FeatureNodeId(1));
            const auto last_consumer = inspect(FeatureNodeId(2));
            const auto& owner = producer.owners[0];
            if (owner.processes.size() != 1 || owner.processes[0].state.commit_count != 2 ||
                owner.processes[0].status != ProcessStatus::kReady || !owner.render_output ||
                !first_consumer.owners[0].render_output || !last_consumer.owners[0].render_output) return 32;
            RuntimeProcessCommand command{producer.generation, producer.asset_uuid, producer.node_id,
                owner.owner, owner.processes[0].id, owner.processes[0].revision,
                process_cancel ? RuntimeProcessAction::kCancel : RuntimeProcessAction::kSuspend};
            auto wrong = command;
            wrong.generation = RuntimeGeneration(producer.generation.value() + 1);
            if (RuntimeFeatureControl::control_process(bootstrap, wrong)) return 33;
            wrong = command;
            wrong.owner = first_consumer.owners[0].owner;
            if (RuntimeFeatureControl::control_process(bootstrap, wrong)) return 33;
            wrong = command;
            wrong.owner.resource_epoch++;
            if (RuntimeFeatureControl::control_process(bootstrap, wrong)) return 33;
            if (!RuntimeFeatureControl::control_process(bootstrap, command) ||
                RuntimeFeatureControl::control_process(bootstrap, command)) return 34;
            const auto stopped = inspect(FeatureNodeId("Cull"));
            const auto& stopped_owner = stopped.owners[0];
            if (stopped_owner.processes[0].status != (process_cancel ? ProcessStatus::kCancelled : ProcessStatus::kSuspended) ||
                stopped_owner.processes[0].revision == command.expected_revision ||
                stopped_owner.processes[0].state.commit_count != 2 ||
                (process_cancel ? stopped_owner.owner.resource_epoch <= owner.owner.resource_epoch : stopped_owner.owner != owner.owner)) return 35;
            if (process_cancel && stopped_owner.render_output) return 35;
            const auto waiting = [&] {
                const auto output = RuntimeFeatureControl::capture_render_outputs(bootstrap);
                const auto readiness = RuntimeFeatureControl::render_publication_status(bootstrap, ClockId("Frame"));
                return output && output.value.empty() && readiness.code == aeg::foundation::ResultCode::kNotReady;
            };
            if (!waiting()) return 36;
            owned_clock_seconds += 1.0 / 60.0;
            if (!pump_until([&] {
                return inspect(FeatureNodeId(1)).owners[0].state.commit_count > first_consumer.owners[0].state.commit_count &&
                    inspect(FeatureNodeId(2)).owners[0].state.commit_count > last_consumer.owners[0].state.commit_count;
            })) return 37;
            // A fresh submitted graph has Present/readback, but zero Feature work,
            // including culled entries. Waiting is a successful empty frame.
            for (unsigned frame = 0; frame != 2; ++frame) {
                if (!tick() || !waiting() || std::any_of(aeg::render_graph::detail::RenderGraphExecution::plan(*upload.graph).pass_plans.begin(),
                    aeg::render_graph::detail::RenderGraphExecution::plan(*upload.graph).pass_plans.end(), [](const auto& pass) { return pass.feature_work.has_value(); })) return 38;
                const auto pixels = read_owned_pixels(request.readback_path);
                if (pixels.empty() || !std::all_of(pixels.begin(), pixels.end(), [](auto value) { return value == 0; })) return 38;
            }
            const auto before_resume = inspect(FeatureNodeId("Cull"));
            const auto empty_consumer = inspect(FeatureNodeId(1));
            if (before_resume.owners[0].processes[0].state.commit_count != 2 || computed[0].load() != 2 ||
                empty_consumer.owners[0].render_output->invocation != first_consumer.owners[0].render_output->invocation ||
                empty_consumer.owners[0].render_output->input_messages.size() != 1 ||
                empty_consumer.owners[0].render_output->input_messages[0].source !=
                    first_consumer.owners[0].render_output->input_messages[0].source) return 39;
            command.owner = before_resume.owners[0].owner;
            command.expected_revision = before_resume.owners[0].processes[0].revision;
            command.action = RuntimeProcessAction::kResume;
            if (process_cancel) {
                if (RuntimeFeatureControl::control_process(bootstrap, command) || !waiting()) return 40;
                std::cout << "Owned real GPU wave2 -> cancel -> fresh empty waiting frames passed\n";
            } else {
                if (!RuntimeFeatureControl::control_process(bootstrap, command) || !waiting()) return 40;
                owned_clock_seconds += 1.0 / 60.0;
                if (!pump_until([&] { return slow_started[2].completion().is_complete() && computed[1].load() == 3; }) ||
                    computed[2].load() != 2 || !waiting()) return 41;
                static_cast<void>(slow_release[2].complete());
                const auto resumed_visible = [&] {
                    const auto accepted = RuntimeFeatureControl::capture_render_outputs(bootstrap);
                    if (!accepted || accepted.value.size() != 3) return false;
                    const auto p = std::find_if(accepted.value.begin(), accepted.value.end(),
                        [&](const auto& output) { return output.instance_id == owner.owner.instance_id; });
                    if (p == accepted.value.end() || p->invocation.state_version.value() != 3 || p->resource_owner != owner.owner ||
                        p->invocation.process_revision == owner.render_output->invocation.process_revision) return false;
                    unsigned consumers = 0;
                    std::vector<FeatureInstanceId> gpu_owners;
                    bool shadow_work = false;
                    for (const auto& output : accepted.value) {
                        if (output.instance_id == p->instance_id) continue;
                        ++consumers;
                        if (output.input_messages.size() != 1 || output.invocation.state_version.value() <= 3) return false;
                        const auto& message = output.input_messages[0];
                        const auto reference = decode_render_light_list_reference(message.payload);
                        if (!reference || reference.value.version != 3 || reference.value.producer_invocation != p->invocation.invocation_id ||
                            reference.value.producer_generation != p->invocation.generation || message.source.instance != p->instance_id ||
                            message.source.invocation != p->invocation || message.source.definition_digest != p->resource_owner.definition_digest ||
                            message.source.resource_epoch != p->resource_owner.resource_epoch || !message.render_source) return false;
                    }
                    for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(*upload.graph).pass_plans) if (pass.live && pass.feature_work) {
                        const auto& work = *pass.feature_work;
                        const auto output = std::find_if(accepted.value.begin(), accepted.value.end(),
                            [&](const auto& item) { return item.instance_id == work.instance_id; });
                        if (output == accepted.value.end() || work.invocation != output->invocation || work.node_id != output->node_id ||
                            work.owner_module != output->owner_module || work.feature_definition_id != output->contract.definition_id) return false;
                        if (pass.live && work.instance_id == p->instance_id && work.operation_index == 1) shadow_work = true;
                        if (std::find(gpu_owners.begin(), gpu_owners.end(), work.instance_id) == gpu_owners.end()) gpu_owners.push_back(work.instance_id);
                    }
                    return consumers == 2 && gpu_owners.size() == 3 && (!clustered_shadow || shadow_work);
                };
                if (!pump_until(resumed_visible) || !pixels_match(3)) return 42;
                const auto resumed = inspect(FeatureNodeId("Cull"));
                if (resumed.owners[0].processes[0].state.commit_count != 3 || resumed.owners[0].owner != owner.owner ||
                    resumed.owners[0].processes[0].status != ProcessStatus::kReady) return 43;
                std::cout << "Owned real GPU wave2 -> suspend -> empty waiting frames -> fresh per-owner wave3 receipts/pixels passed\n";
            }
        } else {
        owned_clock_seconds += 1.0 / 60.0;
        if (!pump_until([&] { return slow_started[2].completion().is_complete() && computed[1].load() == 3; })) return 28;
        if (computed[2].load() != 2) return 28;
        for (unsigned frame = 0; frame != 2; ++frame)
            if (!tick() || !wave_visible(2) || !pixels_match(2)) return 29;
        static_cast<void>(slow_release[2].complete());
        if (!pump_until([&] { return wave_visible(3); }) || !pixels_match(3)) return 30;
        std::cout << "owned cooked PointLight: pending default, independent HDR demand, retained two-frame cohort and full switch passed\n";
        }
    } else if (!no_scene) {
    if (!executed.completion().wait(2'000'000'000ull)) return 14;
    bool green = false;
    for (unsigned attempt = 0; attempt < 20 && !green; ++attempt) {
        if (const auto result = bootstrap.tick(); !result) {
            std::cerr << "owned neutral draw failed: " << result.message << '\n';
            return 15;
        }
        const auto pixels = read_owned_pixels(request.readback_path);
        green = !pixels.empty() && pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0;
        if (post && !pixels.empty()) {
            const auto matches = [&](unsigned x, unsigned y, std::array<unsigned char, 3> expected) {
                const auto offset = (y * 320 + x) * 3;
                return std::equal(expected.begin(), expected.end(), pixels.begin() + offset);
            };
            green = true;
            for (unsigned y : {110u, 120u, 130u})
                for (unsigned x : {150u, 160u, 170u}) green = green && matches(x, y, {0, 255, 0});
            for (unsigned y : {0u, 239u})
                for (unsigned x : {0u, 319u}) green = green && matches(x, y, {255, 0, 0});
        }
    }
    if (!green) return 16;
    }
    if (const auto shutdown = bootstrap.shutdown(); !shutdown) {
        std::cerr << "owned host shutdown failed: " << shutdown.message << '\n';
        runtime_test::cleanup(fixture);
        return 11;
    }
    if (process_lifecycle && (aeg::application::runtime::RuntimeFeatureControl::capture_render_outputs(bootstrap) ||
        aeg::application::runtime::RuntimeFeatureControl::inspect_node(bootstrap, "stopped", aeg::feature::FeatureNodeId("Cull")) ||
        aeg::application::runtime::RuntimeFeatureControl::control_process(bootstrap, {}))) return 44;
    runtime_test::cleanup(fixture);
    std::cout << "runtime owned-host graphics bootstrap passed\n";
    return 0;
}
