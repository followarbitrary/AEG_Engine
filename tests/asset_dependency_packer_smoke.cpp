#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "asset_dependency_packer.hpp"
#include "cooked_artifact_store.hpp"

namespace {
[[nodiscard]] aeg::tooling::Uuid uuid(std::string_view text) {
    const auto parsed = aeg::tooling::Uuid::parse(text);
    assert(parsed);
    return parsed.value;
}
}  // namespace

int main() {
    const aeg::tooling::AssetId scene{uuid("11111111-2222-3333-4444-555555555555")};
    const aeg::tooling::AssetId mesh{uuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")};
    const aeg::tooling::CookedAssetCodec codec;
    const auto scene_bytes = codec.encode({scene, aeg::tooling::AssetKind::kScene, 1, {mesh}, {std::byte{0x01}}});
    const auto mesh_bytes = codec.encode({mesh, aeg::tooling::AssetKind::kMesh, 2, {}, {std::byte{0x02}}});
    assert(scene_bytes && mesh_bytes);
    const auto root =
        std::filesystem::temp_directory_path() /
        ("aeg_dependency_pack_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const aeg::tooling::SourceId source{uuid("22222222-3333-4444-5555-666666666666")};
    aeg::tooling::CookedArtifactStore store(root / "Cooked");
    assert(store.publish(source, {{scene, aeg::tooling::AssetKind::kScene, 1, scene_bytes.value},
                                  {mesh, aeg::tooling::AssetKind::kMesh, 2, mesh_bytes.value}}));
    aeg::tooling::CookedArtifactIndex index(root / "Cooked");
    assert(index.refresh());
    const auto output = root / "project.aegbin";
    const std::array roots{scene};
    assert(aeg::tooling::AssetDependencyPacker{}.write(output, roots,
                                                       [&index](aeg::tooling::AssetId id) { return index.read(id); }));
    const auto package = aeg::tooling::AssetPackageReader::open(output);
    assert(package);
    assert(package.value.entry_count() == 2U);
    assert(package.value.read(scene));
    assert(package.value.read(mesh));
    std::filesystem::remove_all(root);
    return 0;
}
