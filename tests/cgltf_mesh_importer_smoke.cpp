#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "cgltf_mesh_importer.hpp"

namespace {
[[nodiscard]] aeg::tooling::Uuid uuid(std::string_view text) {
    const auto parsed = aeg::tooling::Uuid::parse(text);
    assert(parsed);
    return parsed.value;
}
}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("aeg_cgltf_mesh_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    {
        const std::array<float, 9> vertices{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
        std::ofstream bytes(root / "triangle.bin", std::ios::binary);
        bytes.write(reinterpret_cast<const char*>(vertices.data()), static_cast<std::streamsize>(sizeof(vertices)));
    }
    std::ofstream(root / "triangle.gltf")
        << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"triangle.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]})";

    const aeg::tooling::AssetMeta meta{
        1,
        {uuid("11111111-2222-3333-4444-555555555555")},
        "cgltf_mesh",
        {{{uuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")}, aeg::tooling::AssetKind::kMesh, "triangle"}}};
    const auto cooked = aeg::tooling::CgltfMeshImporter{}.cook(root / "triangle.gltf", meta);
    assert(cooked);
    assert(cooked.value.size() == 1U);
    assert(cooked.value[0].payload.size() == 16U + 3U * 32U + 3U * 4U);
    std::filesystem::remove_all(root);
    return 0;
}
