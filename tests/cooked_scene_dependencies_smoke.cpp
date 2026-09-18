#include <cassert>
#include <cstring>
#include <iostream>

#include "asset_handle_table.hpp"
#include "material_payload.hpp"
#include "scene_payload.hpp"
#include "texture_payload.hpp"

namespace {
using aeg::asset::AssetId;
using aeg::asset::AssetKind;

AssetId id(std::string_view text) {
    const auto parsed = AssetId::parse(text);
    assert(parsed);
    return parsed.value;
}

template <typename T>
void write(std::vector<std::byte>& bytes, T value) {
    const auto* ptr = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), ptr, ptr + sizeof(T));
}
void string(std::vector<std::byte>& bytes, std::string_view value) {
    write<uint32_t>(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(value.data()),
                 reinterpret_cast<const std::byte*>(value.data() + value.size()));
}
void uuid(std::vector<std::byte>& bytes, AssetId value) {
    for (uint8_t byte : value.value.bytes)
        write<uint8_t>(bytes, byte);
}
}  // namespace

int main() {
    const AssetId mesh = id("11111111-2222-3333-4444-555555555555");
    const AssetId material = id("11111111-2222-3333-4444-666666666666");
    const AssetId texture = id("11111111-2222-3333-4444-777777777777");
    const AssetId shader = id("11111111-2222-3333-4444-888888888888");

    // Full UUIDs with the same prefix remain distinct in the session handle table.
    aeg::asset::AssetHandleTable handles;
    const auto hm = handles.intern(mesh, AssetKind::kMesh);
    const auto hmaterial = handles.intern(material, AssetKind::kMaterial);
    if (!hm || !hmaterial || hm.value == hmaterial.value || handles.resolve(hm.value, AssetKind::kMaterial))
        return 1;

    std::vector<std::byte> texture_bytes(12 + 8, std::byte{0});
    const uint32_t width = 2, height = 1, channels = 4;
    std::memcpy(texture_bytes.data(), &width, 4);
    std::memcpy(texture_bytes.data() + 4, &height, 4);
    std::memcpy(texture_bytes.data() + 8, &channels, 4);
    texture_bytes[12] = std::byte{0xFF};
    const auto texture_payload = aeg::asset::decode_texture_payload(texture_bytes);
    if (!texture_payload || texture_payload.value.width != 2 || texture_payload.value.pixels.size() != 8)
        return 2;

    aeg::asset::ScenePayload scene{};
    scene.serialized_scene = {std::byte{0xA1}, std::byte{0xB2}};
    aeg::asset::SceneAssetReference reference{};
    reference.scene_entity_uuid[0] = 7;
    reference.component_type = "MeshRenderer";
    reference.component_version = 1;
    reference.field_id = 42;
    reference.asset = mesh;
    reference.kind = AssetKind::kMesh;
    scene.references.push_back(reference);
    const auto encoded_scene = aeg::asset::encode_scene_payload(scene);
    if (!encoded_scene)
        return 3;
    const auto decoded_scene = aeg::asset::decode_scene_payload(encoded_scene.value);
    if (!decoded_scene || decoded_scene.value.references.size() != 1 ||
        decoded_scene.value.references.front().asset != mesh ||
        decoded_scene.value.references.front().kind != AssetKind::kMesh)
        return 4;
    auto corrupted_scene = encoded_scene.value;
    corrupted_scene.push_back(std::byte{0});
    if (aeg::asset::decode_scene_payload(corrupted_scene))
        return 5;

    // Material payload: AGM1, schema, shader UUID, one texture binding, one bool parameter.
    std::vector<std::byte> material_bytes;
    write<uint32_t>(material_bytes, 0x314d4741u);
    write<uint32_t>(material_bytes, 1u);
    uuid(material_bytes, shader);
    write<uint32_t>(material_bytes, 1u);
    write<uint64_t>(material_bytes, 77u);
    uuid(material_bytes, texture);
    write<uint32_t>(material_bytes, 1u);
    write<uint64_t>(material_bytes, 99u);
    write<uint8_t>(material_bytes, 0u);
    write<uint8_t>(material_bytes, 1u);
    const auto decoded_material = aeg::asset::decode_material_payload(material_bytes);
    if (!decoded_material || decoded_material.value.shader != shader || decoded_material.value.textures.size() != 1 ||
        decoded_material.value.parameters.size() != 1)
        return 6;
    std::cout << "Cooked scene dependencies smoke passed\n";
    return 0;
}
