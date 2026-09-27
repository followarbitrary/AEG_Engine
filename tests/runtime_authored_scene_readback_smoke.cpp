/** @brief Package-authored Camera/MeshRenderer/light -> owned Vulkan host -> pixel readback. */
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

#include "Components/component_ids.hpp"
#include "Components/component_reflection.hpp"
#include "Components/render_components.hpp"
#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "game_build_manifest.hpp"
#include "material_payload.hpp"
#include "module_bundle.hpp"
#include "pipeline_asset.hpp"
#include "renderer_variant_document_fixture.hpp"
#include "runtime_bootstrap.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"
#include "world_scope.hpp"

namespace {
using namespace aeg;
namespace fs = std::filesystem;

bool check(const foundation::Result& result, std::string_view step) {
    if (!result)
        std::cerr << step << ": " << result.message << '\n';
    return static_cast<bool>(result);
}

template <typename T>
void append(std::vector<std::byte>& bytes, const T& value) {
    const auto data = std::as_bytes(std::span{&value, 1});
    bytes.insert(bytes.end(), data.begin(), data.end());
}

std::vector<std::byte> triangle_payload() {
    std::vector<std::byte> bytes;
    for (const uint32_t word : {3u, 3u, 32u, 0u})
        append(bytes, word);
    // Cooked ABI: position, normal, UV. Runtime converts this to the shader vertex ABI.
    const std::array<std::array<float, 8>, 3> vertices{{
        {-1.0f, -0.85f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {1.0f, -0.85f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {0.0f, 0.85f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f},
    }};
    for (const auto& vertex : vertices)
        append(bytes, vertex);
    for (uint32_t index : {0u, 1u, 2u})
        append(bytes, index);
    return bytes;
}

std::vector<std::byte> red_texture_payload() {
    std::vector<std::byte> bytes;
    for (uint32_t word : {1u, 1u, 4u})
        append(bytes, word);
    for (uint8_t channel : {uint8_t{230}, uint8_t{12}, uint8_t{6}, uint8_t{255}})
        append(bytes, channel);
    return bytes;
}

bool prepare(const fs::path& root, bool forward, module::ModuleDefinition& definition, asset::AssetId shader_id) {
    const auto scene_id = asset::AssetId::parse("00000000-0000-4000-8000-000000000101").value;
    const auto pipeline_id = asset::AssetId::parse("00000000-0000-4000-8000-000000000102").value;
    const auto mesh_id = asset::AssetId::parse("00000000-0000-4000-8000-000000000103").value;
    const auto material_id = asset::AssetId::parse("00000000-0000-4000-8000-000000000104").value;
    const auto texture_id = asset::AssetId::parse("00000000-0000-4000-8000-000000000105").value;
    reflection::TypeRegistry reflection;
    if (!check(engine::components::register_render_component_reflection(reflection), "component reflection"))
        return false;
    scene::SceneAsset scene(scene::SceneAssetId{0x4000, 0x8000000000000101ull}, "Authored triangle", 1);
    const scene::SceneEntityId camera_id{1, 1}, object_id{1, 2}, light_id{1, 3};
    if (!scene.add_entity({camera_id, {}, "Camera", scene::SceneEntityFlags::kNone, {}}) ||
        !scene.add_entity({object_id, {}, "Red triangle", scene::SceneEntityFlags::kNone, {}}) ||
        !scene.add_entity({light_id, {}, "Directional light", scene::SceneEntityFlags::kNone, {}}))
        return false;
    const auto component = [&](scene::SceneEntityId entity, std::string_view name, const auto& value) {
        return check(scene.add_reflected_component(entity, ecs::ComponentTypeId(name), reflection,
                                                   reflection::TypeId(name), &value),
                     name);
    };
    engine::components::Transform camera_transform{};
    camera_transform.translation = {0.0f, 0.0f, 3.0f};
    engine::components::MeshRenderer mesh{};
    mesh.mesh_asset_id = 0xabcu;
    mesh.material_asset_id = 0xdefu;
    mesh.bounds_radius = 1.5f;
    engine::components::DirectionalLight light{};
    light.casts_shadow = true;
    if (!component(camera_id, engine::components::kTransformName, camera_transform) ||
        !component(camera_id, engine::components::kCameraName, engine::components::Camera{}) ||
        !component(object_id, engine::components::kTransformName, engine::components::Transform{}) ||
        !component(object_id, engine::components::kMeshRendererName, mesh) ||
        !component(light_id, engine::components::kDirectionalLightName, light))
        return false;
    serialization::BinaryWriter archive;
    if (!check(scene::SceneSerializer::serialize(scene, archive), "serialize Scene"))
        return false;
    asset::ScenePayload scene_payload;
    const auto& bytes = archive.data();
    scene_payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                                          reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
    asset::SceneAssetReference reference{};
    for (int i = 0; i != 8; ++i) {
        reference.scene_entity_uuid[i] = static_cast<uint8_t>(object_id.high() >> (56 - i * 8));
        reference.scene_entity_uuid[8 + i] = static_cast<uint8_t>(object_id.low() >> (56 - i * 8));
    }
    reference.component_type = "MeshRenderer";
    reference.component_version = 1;
    reference.field_id = reflection::FieldId("mesh_asset_id").value();
    reference.asset = mesh_id;
    reference.kind = asset::AssetKind::kMesh;
    scene_payload.references.push_back(reference);
    reference.field_id = reflection::FieldId("material_asset_id").value();
    reference.asset = material_id;
    reference.kind = asset::AssetKind::kMaterial;
    scene_payload.references.push_back(reference);
    const auto scene_bytes = asset::encode_scene_payload(scene_payload);
    pipeline::TypeCatalog catalog;
    if (!check(renderer::register_renderer_authoring_types(catalog), "renderer authoring"))
        return false;
    const auto pipeline_document = test::renderer_document(catalog, forward);
    const auto pipeline_bytes = asset::encode_pipeline_asset(pipeline_document);
    if (!pipeline_bytes) {
        std::cerr << "pipeline encode: " << pipeline_bytes.result.message << '\n';
        return false;
    }
    const auto pipeline_roundtrip = asset::decode_pipeline_asset(pipeline_bytes.value);
    if (!pipeline_roundtrip || pipeline_roundtrip.value.present_connections.empty()) {
        std::cerr << "pipeline present connection lost during codec input="
                  << pipeline_document.present_connections.size()
                  << " output=" << (pipeline_roundtrip ? pipeline_roundtrip.value.present_connections.size() : 0)
                  << "\n";
        return false;
    }
    const auto material_bytes = asset::encode_material_payload({1, shader_id, {{1, texture_id}}, {}});
    if (!scene_bytes || !material_bytes)
        return false;
    std::vector<tooling::AssetPackageEntry> entries;
    const auto add = [&](asset::AssetId id, asset::AssetKind kind, std::vector<std::byte> payload,
                         std::vector<asset::AssetId> dependencies = {}) {
        const auto cooked = asset::encode_cooked_asset({id, kind, 1, std::move(dependencies), std::move(payload)});
        if (!check(cooked.result, "encode cooked asset"))
            return false;
        entries.push_back({id, kind, 1, cooked.value});
        return true;
    };
    if (!add(scene_id, asset::AssetKind::kScene, scene_bytes.value, {mesh_id, material_id}) ||
        !add(pipeline_id, asset::AssetKind::kPipeline, pipeline_bytes.value) ||
        !add(mesh_id, asset::AssetKind::kMesh, triangle_payload()) ||
        !add(material_id, asset::AssetKind::kMaterial, material_bytes.value, {texture_id}) ||
        !add(texture_id, asset::AssetKind::kTexture, red_texture_payload()) ||
        !check(tooling::AssetPackageWriter{}.write(root / "game.aegbin", entries), "write package"))
        return false;
    definition.descriptor.id = pipeline::ModuleId("AuthoredReadback.Game");
    definition.descriptor.name = "AuthoredReadback.Game";
    module::ModuleMetadata metadata;
    metadata.module = definition.descriptor;
    metadata.descriptor = definition.descriptor;
    metadata.catalog = definition.catalog;
    const auto contract = module::module_contract_sha256(metadata);
    std::ifstream input(root / "game.aegbin", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    const auto digest = module::module_content_sha256(contents);
    if (!contract || !digest)
        return false;
    asset::GameBuildManifest manifest{};
    manifest.project_uuid = "authored-readback";
    manifest.module_id = "AuthoredReadback.Game";
    manifest.module_contract_digest = contract.value;
    manifest.build_id = "authored-readback";
    manifest.target = "windows-x64-vulkan";
    manifest.configuration = "Debug";
    manifest.package_name = "game.aegbin";
    manifest.package_digest = digest.value;
    manifest.startup_scene = scene_id;
    manifest.startup_pipelines = {pipeline_id};
    const auto encoded = asset::encode_game_build_manifest(manifest);
    if (!encoded)
        return false;
    std::ofstream binary(root / "game.manifest.bin", std::ios::binary);
    binary.write(reinterpret_cast<const char*>(encoded.value.data()), encoded.value.size());
    std::ofstream(root / "game.manifest") << "package = \"game.aegbin\"\nbuild_id = \"authored-readback\"\n";
    return static_cast<bool>(binary);
}

bool verify_pixels(const fs::path& path) {
    std::ifstream image(path, std::ios::binary);
    std::string magic;
    unsigned width = 0, height = 0, maximum = 0;
    image >> magic >> width >> height >> maximum;
    image.get();
    if (!image || magic != "P6" || width != 320 || height != 240 || maximum != 255) {
        std::cerr << "invalid readback image header\n";
        return false;
    }
    std::vector<unsigned char> pixels((std::istreambuf_iterator<char>(image)), {});
    if (pixels.size() != width * height * 3u)
        return false;
    const auto center = (height / 2 * width + width / 2) * 3u;
    unsigned red_pixels = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 3)
        if (pixels[i] > pixels[i + 1] + 40 && pixels[i] > pixels[i + 2] + 40)
            ++red_pixels;
    // Sky/clear alone cannot satisfy this red region or center color assertion.
    if (red_pixels < 1500 || pixels[center] <= pixels[center + 1] + 40 || pixels[center] <= pixels[center + 2] + 40) {
        std::cerr << "authored triangle missing: red pixels=" << red_pixels << " center=" << unsigned(pixels[center])
                  << ',' << unsigned(pixels[center + 1]) << ',' << unsigned(pixels[center + 2]) << '\n';
        return false;
    }
    return true;
}

bool run_variant(const fs::path& root, bool forward) {
    fs::create_directories(root);
    module::ModuleDefinition definition;
    const auto shader_id = asset::AssetId::parse("11111111-1111-4111-8111-111111111111").value;
    if (!prepare(root, forward, definition, shader_id))
        return false;
    application::runtime::RuntimeBootRequest request{};
    request.manifest_path = root / "game.manifest";
    request.owned_host = true;
    request.width = 320;
    request.height = 240;
    request.material_shader = shader_id;
    request.readback_path = root / "frame.ppm";
    application::runtime::RuntimeBootstrap runtime;
    if (!check(runtime.initialize(request, std::move(definition), {"authored-readback", "windows-x64-vulkan", "Debug"}),
               "initialize") ||
        runtime.startup_world_scope() == nullptr || runtime.startup_pipeline_count() != 1)
        return false;
    for (int frame = 0; frame != 2; ++frame)
        if (!check(runtime.tick(), "authored frame") || !verify_pixels(request.readback_path))
            return false;
    return check(runtime.shutdown(), "shutdown");
}
}  // namespace

int main() {
    // Unique, task-owned temporary path: retained on failure for pixel diagnostics.
    const auto root = fs::temp_directory_path() / "aeg_runtime_authored_scene_readback";
    if (!run_variant(root / "forward", true) || !run_variant(root / "deferred", false))
        return 1;
    std::cout << "authored forward/deferred scenes draw and read back on the owned runtime host\n";
    return 0;
}
