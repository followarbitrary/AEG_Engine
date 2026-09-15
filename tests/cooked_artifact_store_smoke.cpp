#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "cooked_artifact_store.hpp"

namespace {
[[nodiscard]] aeg::tooling::Uuid uuid(std::string_view value) {
    const auto parsed = aeg::tooling::Uuid::parse(value);
    assert(parsed);
    return parsed.value;
}
}  // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("aeg_cooked_store_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const aeg::tooling::CookedArtifactStore store(root);
    const aeg::tooling::SourceId source{uuid("11111111-2222-3333-4444-555555555555")};
    const aeg::tooling::AssetId mesh{uuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")};
    const aeg::tooling::AssetId material{uuid("12345678-9abc-def0-1234-56789abcdef0")};

    const std::array<std::byte, 3> mesh_bytes{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    const std::array<std::byte, 2> material_bytes{std::byte{0x04}, std::byte{0x05}};
    const std::vector<aeg::tooling::CookedArtifact> first{
        {mesh, aeg::tooling::AssetKind::kMesh, 3, {mesh_bytes.begin(), mesh_bytes.end()}},
        {material, aeg::tooling::AssetKind::kMaterial, 2, {material_bytes.begin(), material_bytes.end()}},
    };

    const auto published = store.publish(source, first);
    assert(published);
    assert(published.value.artifacts.size() == 2U);
    assert(std::filesystem::exists(published.value.manifest));

    const auto loaded = store.load_current(source);
    assert(loaded);
    assert(loaded.value.artifacts.size() == 2U);
    assert(loaded.value.artifacts[0].id == material);
    assert(loaded.value.artifacts[0].bytes == first[1].bytes);

    const std::vector<aeg::tooling::CookedArtifact> duplicate{
        first[0],
        first[0],
    };
    assert(!store.publish(source, duplicate));
    const auto still_loaded = store.load_current(source);
    assert(still_loaded);
    assert(still_loaded.value.artifacts.size() == 2U);

    const std::array<std::byte, 1> replacement_bytes{std::byte{0x09}};
    const std::vector<aeg::tooling::CookedArtifact> replacement{
        {mesh, aeg::tooling::AssetKind::kMesh, 4, {replacement_bytes.begin(), replacement_bytes.end()}},
    };
    const auto republished = store.publish(source, replacement);
    assert(republished);
    assert(republished.value.manifest != published.value.manifest);
    const auto replacement_loaded = store.load_current(source);
    assert(replacement_loaded);
    assert(replacement_loaded.value.artifacts.size() == 1U);
    assert(replacement_loaded.value.artifacts[0].schema_version == 4U);
    assert(replacement_loaded.value.artifacts[0].bytes == replacement[0].bytes);
    assert(std::filesystem::exists(published.value.manifest));

    aeg::tooling::CookedArtifactIndex index(root);
    assert(index.refresh());
    const auto indexed = index.read(mesh);
    assert(indexed);
    assert(indexed.value == replacement[0].bytes);
    assert(!index.read(material));

    // A truncated metadata field must become a failed Result, without throwing.
    const auto manifest = republished.value.manifest;
    for (const std::string& bad_field : {std::string{}, std::string{"id = \"\""}}) {
        std::ofstream(manifest, std::ios::binary | std::ios::trunc)
            << "version = 1\nsource_uuid = \"" << source.to_string()
            << "\"\n[[artifacts]]\n" << bad_field
            << "\n\nschema_version = 4\nfile = \"missing.aegmesh\"\n";
        try {
            if (store.load_current(source) || index.refresh() || !index.read(mesh)) {
                std::cerr << "Malformed cooked metadata must fail and preserve the previous index\n";
                return 1;
            }
        } catch (const std::exception& exception) {
            std::cerr << "Malformed cooked metadata threw: " << exception.what() << '\n';
            return 2;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
