#include <algorithm>
#include <cstddef>
#include <iostream>
#include <span>

#include "feature_world_transform.hpp"

#include "game_build_manifest.hpp"

int main() {
    using namespace aeg::asset;
    using namespace aeg::foundation;
    const auto scene = AssetId::parse("00000000-0000-4000-8000-000000000001");
    const auto pipeline = AssetId::parse("00000000-0000-4000-8000-000000000002");
    if (!scene || !pipeline)
        return 1;
    GameBuildManifest source{};
    source.project_uuid = "project";
    source.module_id = "GameModule";
    source.build_id = "build";
    source.module_contract_digest = "contract";
    source.package_digest = "package";
    source.startup_scene = scene.value;
    source.startup_feature_graphs = {pipeline.value};
    source.target = "windows-x64-vulkan";
    source.configuration = "Release";
    source.package_name = "game.aegbin";
    source.module_manifest_name = "module/module.manifest.json";
    source.module_manifest_sha256 = "module-sha";
    if (source.schema_version != 3) {
        std::cerr << "Game build manifest must default to schema3\n";
        return 5;
    }
    aeg::asset::WorldCommandRoute route{};
    route.graph_asset_uuid = "ABCDEFAB-CDEF-4ABC-8DEF-ABCDEFABCDEF";
    route.node_id = aeg::feature::FeatureNodeId{0x0102030405060708ull};
    route.owner_path.nodes = {aeg::feature::FeatureNodeId{11}, aeg::feature::FeatureNodeId{22}};
    route.scene_asset = scene.value;
    route.scene_entity = Uuid::parse("fedcba98-7654-4321-8abc-def012345678").value;
    route.effect = aeg::feature::kWorldTransformCommandEffect;
    route.version = aeg::feature::kWorldTransformCommandVersion;
    route.observation_port = aeg::feature::PortId{"Observation"};
    source.world_command_routes = {route};
    const auto encoded = encode_game_build_manifest(source);
    if (!encoded)
        return 2;
    const auto decoded = decode_game_build_manifest(encoded.value);
    if (!decoded || decoded.value.startup_scene != source.startup_scene ||
        decoded.value.startup_feature_graphs != source.startup_feature_graphs ||
        decoded.value.package_name != source.package_name ||
        decoded.value.module_manifest_name != source.module_manifest_name ||
        decoded.value.module_manifest_sha256 != source.module_manifest_sha256 ||
        decoded.value.schema_version != 3 || decoded.value.world_command_routes.size() != 1)
        return 3;
    const auto& restored = decoded.value.world_command_routes.front();
    if (restored.graph_asset_uuid != route.graph_asset_uuid || restored.node_id != route.node_id ||
        restored.owner_path.nodes != route.owner_path.nodes || restored.scene_asset != route.scene_asset ||
        restored.scene_entity != route.scene_entity || restored.effect != route.effect ||
        restored.version != route.version || restored.observation_port != route.observation_port) {
        std::cerr << "Manifest route roundtrip lost full authored identity\n";
        return 6;
    }
    const auto route_blob = encode_world_command_routes(source.world_command_routes);
    if (!route_blob || encoded.value.size() < route_blob.value.size() + 4 ||
        !std::equal(route_blob.value.begin(), route_blob.value.end(), encoded.value.end() - route_blob.value.size())) {
        std::cerr << "Manifest must embed the shared route codec as its final block\n";
        return 7;
    }
    auto malformed = encoded.value;
    malformed.push_back(std::byte{0});
    if (decode_game_build_manifest(malformed))
        return 4;
    for (std::size_t length = 0; length < encoded.value.size(); ++length) {
        if (decode_game_build_manifest(std::span{encoded.value}.first(length))) {
            std::cerr << "Truncated schema3 manifest accepted\n";
            return 8;
        }
    }
    for (const uint32_t version : {0u, 1u, 2u, 4u}) {
        auto invalid = source;
        invalid.schema_version = version;
        if (encode_game_build_manifest(invalid)) {
            std::cerr << "Encoder accepted a manifest version other than schema3\n";
            return 9;
        }
        malformed = encoded.value;
        malformed[4] = static_cast<std::byte>(version);
        if (decode_game_build_manifest(malformed)) {
            std::cerr << "Decoder accepted a manifest version other than schema3\n";
            return 10;
        }
    }
    malformed = encoded.value;
    const auto route_start = malformed.size() - route_blob.value.size();
    malformed[route_start + 8] = std::byte{1};
    if (decode_game_build_manifest(malformed)) {
        std::cerr << "Manifest accepted embedded route schema1\n";
        return 11;
    }
    malformed = encoded.value;
    for (std::size_t index = route_start - 4; index < route_start; ++index) malformed[index] = std::byte{0};
    if (decode_game_build_manifest(malformed)) {
        std::cerr << "Manifest accepted a missing explicit route block\n";
        return 12;
    }
    auto invalid_route = source;
    invalid_route.world_command_routes.front().scene_entity = {};
    if (encode_game_build_manifest(invalid_route)) {
        std::cerr << "Manifest encoder bypassed route validation\n";
        return 13;
    }
    invalid_route = source;
    invalid_route.world_command_routes.push_back(route);
    invalid_route.world_command_routes.back().scene_entity.bytes[0] ^= 1;
    if (encode_game_build_manifest(invalid_route)) {
        std::cerr << "Manifest encoder accepted duplicate route source\n";
        return 14;
    }
    auto empty_routes = source;
    empty_routes.world_command_routes.clear();
    const auto empty_encoded = encode_game_build_manifest(empty_routes);
    const auto empty_blob = encode_world_command_routes({});
    if (!empty_encoded || !empty_blob || empty_blob.value.empty() ||
        empty_encoded.value.size() < empty_blob.value.size() + 4 ||
        !std::equal(empty_blob.value.begin(), empty_blob.value.end(),
                    empty_encoded.value.end() - empty_blob.value.size())) {
        std::cerr << "Empty manifest route block is not explicit\n";
        return 15;
    }
    const auto empty_decoded = decode_game_build_manifest(empty_encoded.value);
    if (!empty_decoded || !empty_decoded.value.world_command_routes.empty()) return 16;
    {
        // Four legal per-string maxima still exceed the aggregate manifest budget.
        auto large = empty_routes;
        const auto original_strings = large.project_uuid.size() + large.module_id.size() +
                                      large.build_id.size() + large.module_contract_digest.size();
        const auto overhead = empty_encoded.value.size() - original_strings;
        large.project_uuid.assign(16u * 1024u * 1024u, 'p');
        large.module_id.assign(16u * 1024u * 1024u, 'm');
        large.build_id.assign(16u * 1024u * 1024u, 'b');
        large.module_contract_digest.assign(16u * 1024u * 1024u, 'c');
        if (encode_game_build_manifest(large)) {
            std::cerr << "Manifest above 64 MiB encoded\n";
            return 17;
        }
        large.module_contract_digest.resize(16u * 1024u * 1024u - overhead);
        const auto boundary = encode_game_build_manifest(large);
        if (!boundary || boundary.value.size() != 64u * 1024u * 1024u ||
            !decode_game_build_manifest(boundary.value)) {
            std::cerr << "Exact 64 MiB manifest boundary rejected\n";
            return 18;
        }
        auto oversized = boundary.value;
        oversized.push_back(std::byte{0});
        if (decode_game_build_manifest(oversized)) {
            std::cerr << "Manifest above 64 MiB decoded\n";
            return 19;
        }
    }
    std::cout << "Game build manifest smoke passed\n";
    return 0;
}
