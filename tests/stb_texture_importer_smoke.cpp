#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "cooked_asset.hpp"
#include "stb_texture_importer.hpp"

namespace {
[[nodiscard]] aeg::tooling::Uuid uuid(std::string_view text) {
    const auto parsed = aeg::tooling::Uuid::parse(text);
    assert(parsed);
    return parsed.value;
}
}  // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("aeg_stb_texture_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto source = root / "pixel.png";
    std::ofstream stream(source, std::ios::binary);
    stream << "P6\n1 1\n255\n";
    stream.put(static_cast<char>(0x10));
    stream.put(static_cast<char>(0x20));
    stream.put(static_cast<char>(0x30));
    stream.close();

    const aeg::tooling::AssetMeta meta{
        1,
        {uuid("11111111-2222-3333-4444-555555555555")},
        "stb_texture",
        {{{uuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")}, aeg::tooling::AssetKind::kTexture, "pixel"}}};
    aeg::tooling::StbTextureImporter importer;
    assert(importer.supports(source));
    const auto cooked = importer.cook(source, meta);
    assert(cooked);
    assert(cooked.value.size() == 1U);
    assert(cooked.value[0].payload.size() == 16U);
    assert(cooked.value[0].payload[12] == std::byte{0x10});
    assert(cooked.value[0].payload[13] == std::byte{0x20});
    assert(cooked.value[0].payload[14] == std::byte{0x30});
    assert(cooked.value[0].payload[15] == std::byte{0xff});

    std::filesystem::remove_all(root);
    return 0;
}
