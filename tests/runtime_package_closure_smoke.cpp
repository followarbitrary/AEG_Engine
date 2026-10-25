#include <array>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

#include "asset_dependency_packer.hpp"
#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "material_payload.hpp"
#include "scene_payload.hpp"

using namespace aeg;
using aeg::foundation::AssetId;
using aeg::foundation::AssetKind;
using aeg::tooling::CookedAsset;

static AssetId id(std::string_view s) {
    auto r = AssetId::parse(s);
    assert(r);
    return r.value;
}

int main() {
    const AssetId scene = id("11111111-2222-3333-4444-555555555555");
    const AssetId mesh = id("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    const AssetId material = id("aaaaaaaa-bbbb-cccc-dddd-ffffffffffff");
    const AssetId texture = id("aaaaaaaa-bbbb-cccc-dddd-000000000001");
    const AssetId shader = id("aaaaaaaa-bbbb-cccc-dddd-000000000002");
    const aeg::tooling::CookedAssetCodec codec;
    aeg::asset::ScenePayload scene_payload{};
    scene_payload.serialized_scene = {std::byte{1}};
    aeg::asset::SceneAssetReference mesh_ref{};
    mesh_ref.scene_entity_uuid[0] = 1;
    mesh_ref.component_type = "MeshRenderer";
    mesh_ref.component_version = 1;
    mesh_ref.field_id = 1;
    mesh_ref.asset = mesh;
    mesh_ref.kind = AssetKind::kMesh;
    auto material_ref = mesh_ref;
    material_ref.field_id = 2;
    material_ref.asset = material;
    material_ref.kind = AssetKind::kMaterial;
    scene_payload.references = {mesh_ref, material_ref};
    auto encoded_scene = aeg::asset::encode_scene_payload(scene_payload);
    if (!encoded_scene)
        return 10;
    std::vector<std::byte> scene_bytes =
        codec.encode({scene, AssetKind::kScene, 1, {material, mesh, material}, encoded_scene.value}).value;
    std::vector<std::byte> mesh_bytes = codec.encode({mesh, AssetKind::kMesh, 1, {}, {std::byte{2}}}).value;
    aeg::asset::MaterialPayload material_payload{};
    material_payload.shader = shader;
    material_payload.textures = {{1, texture}};
    const auto encoded_material = aeg::asset::encode_material_payload(material_payload);
    if (!encoded_material)
        return 11;
    std::vector<std::byte> material_bytes =
        codec.encode({material, AssetKind::kMaterial, 1, {shader, texture}, encoded_material.value}).value;
    std::vector<std::byte> shader_bytes = codec.encode({shader, AssetKind::kShader, 1, {}, {std::byte{5}}}).value;
    std::vector<std::byte> texture_bytes = codec.encode({texture, AssetKind::kTexture, 1, {}, {std::byte{4}}}).value;
    std::vector<std::pair<AssetId, std::vector<std::byte>>> assets{{scene, scene_bytes},
                                                                   {mesh, mesh_bytes},
                                                                   {material, material_bytes},
                                                                   {shader, shader_bytes},
                                                                   {texture, texture_bytes}};
    const auto resolve = [&](AssetId key) -> foundation::ResultValue<std::vector<std::byte>> {
        for (auto& x : assets)
            if (x.first == key)
                return {foundation::Result::success(), x.second};
        return {foundation::not_found("missing dependency UUID " + key.to_string()), {}};
    };
    const auto out = std::filesystem::temp_directory_path() / "aeg_runtime_package_closure_smoke.aegbin";
    const std::array roots{scene};
    auto result = aeg::tooling::AssetDependencyPacker{}.write(out, roots, resolve);
    if (!result) {
        std::cerr << result.message << "\n";
        return 1;
    }
    auto package = aeg::tooling::AssetPackageReader::open(out);
    if (!package || package.value.entry_count() != 5)
        return 2;
    auto missing_resolve = [&](AssetId key) -> foundation::ResultValue<std::vector<std::byte>> {
        if (key == scene)
            return {foundation::Result::success(), scene_bytes};
        return {foundation::not_found("missing dependency UUID"), {}};
    };
    auto missing = aeg::tooling::AssetDependencyPacker{}.write(out, roots, missing_resolve);
    if (missing || missing.message.find("missing") == std::string::npos)
        return 3;
    const auto cycle_bytes = codec.encode({texture, AssetKind::kTexture, 1, {scene}, {std::byte{4}}}).value;
    assets.back().second = cycle_bytes;
    auto cycle = aeg::tooling::AssetDependencyPacker{}.write(out, roots, resolve);
    if (cycle || cycle.message.find("cycle") == std::string::npos)
        return 4;
    assets.back().second = texture_bytes;
    auto wrong_kind_bytes = codec.encode({mesh, AssetKind::kTexture, 1, {}, {std::byte{2}}}).value;
    assets[1].second = wrong_kind_bytes;
    auto wrong_kind = aeg::tooling::AssetDependencyPacker{}.write(out, roots, resolve);
    if (wrong_kind || wrong_kind.message.find("kind") == std::string::npos)
        return 5;
    assets[1].second = mesh_bytes;
    auto wrong_uuid_bytes = codec.encode({texture, AssetKind::kMesh, 1, {}, {std::byte{2}}}).value;
    assets[1].second = wrong_uuid_bytes;
    auto wrong_uuid = aeg::tooling::AssetDependencyPacker{}.write(out, roots, resolve);
    if (wrong_uuid || wrong_uuid.message.find("identity") == std::string::npos)
        return 6;
    assets[1].second = mesh_bytes;
    assets[3].second = codec.encode({shader, AssetKind::kTexture, 1, {}, {std::byte{5}}}).value;
    auto wrong_shader = aeg::tooling::AssetDependencyPacker{}.write(out, roots, resolve);
    if (wrong_shader || wrong_shader.message.find("kind") == std::string::npos)
        return 7;
    assets[3].second = shader_bytes;
    assets[2].second = codec.encode({material, AssetKind::kMaterial, 1, {texture}, encoded_material.value}).value;
    auto omitted_shader = aeg::tooling::AssetDependencyPacker{}.write(out, roots, resolve);
    if (omitted_shader || omitted_shader.message.find("dependencies") == std::string::npos)
        return 8;
    assets[2].second = material_bytes;
    assets[4].second = codec.encode({texture, AssetKind::kMesh, 1, {}, {std::byte{4}}}).value;
    auto wrong_texture = aeg::tooling::AssetDependencyPacker{}.write(out, roots, resolve);
    if (wrong_texture || wrong_texture.message.find("kind") == std::string::npos)
        return 9;
    assets[4].second = texture_bytes;
    auto conflicting_material = material_payload;
    conflicting_material.textures.front().texture = shader;
    const auto encoded_conflict = aeg::asset::encode_material_payload(conflicting_material);
    if (!encoded_conflict)
        return 12;
    assets[2].second = codec.encode({material, AssetKind::kMaterial, 1, {shader}, encoded_conflict.value}).value;
    auto conflicting = aeg::tooling::AssetDependencyPacker{}.write(out, roots, resolve);
    if (conflicting || conflicting.message.find("conflicting") == std::string::npos)
        return 13;
    // A dependency visited as an earlier root must still be checked when a later material types it.
    assets[2].second = material_bytes;
    assets[3].second = codec.encode({shader, AssetKind::kTexture, 1, {}, {std::byte{5}}}).value;
    const std::array roots_previsited{shader, material};
    auto previsited = aeg::tooling::AssetDependencyPacker{}.write(out, roots_previsited, resolve);
    if (previsited || previsited.message.find("kind") == std::string::npos)
        return 14;
    std::filesystem::remove(out);
    std::cout << "runtime package closure and failure paths passed\n";
}
