#include <iostream>

#include "game_build_manifest.hpp"

int main() {
    using namespace aeg::asset;
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
    source.startup_pipelines = {pipeline.value};
    source.target = "windows-x64-vulkan";
    source.configuration = "Release";
    source.package_name = "game.aegbin";
    const auto encoded = encode_game_build_manifest(source);
    if (!encoded)
        return 2;
    const auto decoded = decode_game_build_manifest(encoded.value);
    if (!decoded || decoded.value.startup_scene != source.startup_scene ||
        decoded.value.startup_pipelines != source.startup_pipelines ||
        decoded.value.package_name != source.package_name)
        return 3;
    auto malformed = encoded.value;
    malformed.push_back(std::byte{0});
    if (decode_game_build_manifest(malformed))
        return 4;
    std::cout << "Game build manifest smoke passed\n";
    return 0;
}
