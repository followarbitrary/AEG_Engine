#pragma once
#include <filesystem>
#include <fstream>

#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "runtime_bootstrap.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"

namespace runtime_test {
struct Fixture {
    std::filesystem::path root;
    aeg::module::FeatureModuleDefinition definition{};
    aeg::application::runtime::RuntimeBuildIdentity identity{"task1", "windows-x64-vulkan", "Debug"};
};
inline bool prepare(Fixture& f, const aeg::scene::SceneAsset* authored_scene = nullptr) {
    using namespace aeg;
    namespace fs = std::filesystem;
    f.root = fs::temp_directory_path() / "aeg_runtime_task1_fixture";
    std::error_code ec;
    fs::remove_all(f.root, ec);
    fs::create_directories(f.root, ec);
    const auto id = tooling::AssetId::parse("00000000-0000-4000-8000-000000000001");
    if (!id)
        return false;
    tooling::CookedAsset cooked_scene{};
    cooked_scene.id = id.value;
    cooked_scene.kind = tooling::AssetKind::kScene;
    cooked_scene.schema_version = 1;
    scene::SceneAsset document(scene::SceneAssetId{0x4000, 0x8000000000000001ull}, "RuntimeFixture", 1);
    serialization::BinaryWriter archive;
    if (!scene::SceneSerializer::serialize(authored_scene ? *authored_scene : document, archive))
        return false;
    asset::ScenePayload payload{};
    payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(archive.data().data()),
                                    reinterpret_cast<const std::byte*>(archive.data().data() + archive.data().size()));
    auto bytes = asset::encode_scene_payload(payload);
    if (!bytes)
        return false;
    cooked_scene.payload = bytes.value;
    auto envelope = tooling::CookedAssetCodec{}.encode(cooked_scene);
    if (!envelope)
        return false;
    if (!tooling::AssetPackageWriter{}.write(f.root / "game.aegbin", {{cooked_scene.id, cooked_scene.kind,
                                                                       cooked_scene.schema_version, envelope.value}}))
        return false;
    std::ofstream(f.root / "game.manifest") << "package = \"game.aegbin\"\n";
    f.definition.descriptor.id = module::ModuleId{"Task1.GameModule"};
    f.definition.descriptor.name = "Task1.GameModule";
    return true;
}
inline void cleanup(Fixture& f) {
    std::error_code ec;
    std::filesystem::remove_all(f.root, ec);
}
}  // namespace runtime_test
