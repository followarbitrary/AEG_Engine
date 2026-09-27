#include <array>
#include <iostream>

#include "asset_metadata.hpp"
#include "cooked_asset.hpp"
#include "scene_payload.hpp"

using aeg::asset::AssetId;
using aeg::asset::AssetKind;

static AssetId id(const char* text) {
    return AssetId::parse(text).value;
}

int main() {
    const AssetId mesh = id("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    const auto zero_dep = aeg::asset::encode_cooked_asset({mesh, AssetKind::kMesh, 1, {AssetId{}}, {}});
    if (zero_dep || zero_dep.result.message.find("dependency") == std::string::npos)
        return 1;
    const auto bad_kind = aeg::asset::encode_cooked_asset({mesh, static_cast<AssetKind>(255), 1, {}, {}});
    if (bad_kind)
        return 2;

    aeg::asset::ScenePayload payload{};
    aeg::asset::SceneAssetReference reference{};
    reference.scene_entity_uuid[0] = 1;
    reference.component_type = "MeshRenderer";
    reference.component_version = 1;
    reference.field_id = 1;
    reference.kind = AssetKind::kMesh;
    payload.references.push_back(reference);
    const AssetId resolved = id("11111111-2222-3333-4444-555555555555");
    const std::array<uint64_t, 1> stable_ids{42};
    const auto relocated = aeg::asset::relocate_scene_assets(
        payload, stable_ids, [&](uint64_t stable_id) -> aeg::foundation::ResultValue<AssetId> {
            if (stable_id != 42)
                return {aeg::foundation::not_found("missing stable id"), {}};
            return {aeg::foundation::Result::success(), resolved};
        });
    if (!relocated || payload.references.front().asset != resolved)
        return 3;
    aeg::tooling::StableAssetIdIndex index;
    if (!index.add(42, resolved) || !index.resolve(42) || index.resolve(7))
        return 4;
    if (index.add(42, mesh))
        return 5;

    auto encoded = aeg::asset::encode_scene_payload(payload);
    if (!encoded || !aeg::asset::has_scene_payload_header(encoded.value))
        return 6;
    std::vector<std::byte> malformed = encoded.value;
    malformed[4] = std::byte{0};
    malformed[5] = std::byte{0};
    malformed[6] = std::byte{0};
    malformed[7] = std::byte{0};
    if (aeg::asset::decode_scene_payload(malformed))
        return 7;
    std::cout << "task 2 strict contracts passed\n";
    return 0;
}
