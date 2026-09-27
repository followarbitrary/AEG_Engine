#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asset_cooker.hpp"
#include "cooked_asset.hpp"
#include "font_payload.hpp"

namespace {
class TestImporter final : public aeg::tooling::AssetImporter {
public:
    [[nodiscard]] bool supports(const std::filesystem::path& source) const noexcept override {
        return source.extension() == ".test";
    }

    [[nodiscard]] aeg::foundation::ResultValue<std::vector<aeg::tooling::CookedAsset>> cook(
        const std::filesystem::path&, const aeg::tooling::AssetMeta& meta) const override {
        const std::vector<std::byte> payload{std::byte{0x01}, std::byte{0x02}};
        return {{}, {{meta.products.front().id, meta.products.front().kind, 7, {}, payload}}};
    }
};

[[nodiscard]] aeg::tooling::Uuid uuid(std::string_view text) {
    const auto parsed = aeg::tooling::Uuid::parse(text);
    assert(parsed);
    return parsed.value;
}
}  // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("aeg_asset_cooker_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "Assets");
    const auto source = root / "Assets" / "sample.test";
    std::ofstream(source) << "source";

    const aeg::tooling::AssetMeta meta{
        1,
        {uuid("11111111-2222-3333-4444-555555555555")},
        "test",
        {{{uuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")}, aeg::tooling::AssetKind::kMesh, "main"}}};
    aeg::tooling::AssetMetaCodec meta_codec;
    assert(meta_codec.save(source.string() + ".meta", meta));

    aeg::tooling::AssetCooker cooker(root / "Cache" / "Cooked");
    assert(cooker.register_importer(std::make_unique<TestImporter>()));
    assert(!cooker.register_importer(nullptr));
    const auto result = cooker.cook(source);
    assert(result);
    assert(result.value.artifacts.size() == 1U);
    assert(result.value.artifacts[0].id == meta.products.front().id);
    assert(result.value.artifacts[0].schema_version == 7U);
    assert(!cooker.cook(root / "Assets" / "unsupported.bin"));

    const auto texture_source = root / "Assets" / "texture.png";
    std::ofstream texture_stream(texture_source, std::ios::binary);
    texture_stream << "P6\n1 1\n255\n";
    texture_stream.put(static_cast<char>(0x10));
    texture_stream.put(static_cast<char>(0x20));
    texture_stream.put(static_cast<char>(0x30));
    texture_stream.close();
    const aeg::tooling::AssetMeta texture_meta{
        1,
        {uuid("22222222-3333-4444-5555-666666666666")},
        "stb_texture",
        {{{uuid("bbbbbbbb-cccc-dddd-eeee-ffffffffffff")}, aeg::tooling::AssetKind::kTexture, "texture"}}};
    assert(meta_codec.save(texture_source.string() + ".meta", texture_meta));
    aeg::tooling::AssetCooker default_cooker(root / "Cache" / "Cooked");
    assert(default_cooker.register_default_importers());
    const auto texture_result = default_cooker.cook(texture_source);
    assert(texture_result);
    assert(texture_result.value.artifacts.size() == 1U);
    const auto decoded = aeg::tooling::CookedAssetCodec{}.decode(texture_result.value.artifacts[0].bytes);
    assert(decoded);
    assert(decoded.value.kind == aeg::tooling::AssetKind::kTexture);
    assert(decoded.value.payload.size() == 16U);

    // Font payloads are copied into the runtime ABI and retain their face index.
    for (const auto kind : {aeg::tooling::AssetKind::kFont}) {
        const auto source_path = root / "Assets" / "sample_font.ttf";
        // Minimal well-formed sfnt directory (no tables) for importer validation.
        const std::array<unsigned char, 12> minimal_font{0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        std::ofstream font_stream(source_path, std::ios::binary);
        font_stream.write(reinterpret_cast<const char*>(minimal_font.data()),
                          static_cast<std::streamsize>(minimal_font.size()));
        font_stream.close();
        const aeg::tooling::AssetMeta unsupported_meta{
            1,
            {uuid("33333333-4444-5555-6666-777777777777")},
            "test",
            {{{uuid("dddddddd-eeee-ffff-0000-111111111111")}, kind, "main"}}};
        assert(meta_codec.save(source_path.string() + ".meta", unsupported_meta));
        aeg::tooling::AssetCooker font_cooker(root / "Cache" / "Cooked");
        assert(font_cooker.register_default_importers());
        const auto cooked = font_cooker.cook(source_path);
        assert(cooked);
        assert(cooked.value.artifacts.size() == 1U);
        const auto decoded_font = aeg::tooling::CookedAssetCodec{}.decode(cooked.value.artifacts[0].bytes);
        assert(decoded_font);
        assert(decoded_font.value.kind == kind);
        assert(decoded_font.value.payload.size() > 16U);
        const auto font_payload = aeg::asset::decode_font_payload(
            std::span<const std::byte>{decoded_font.value.payload.data(), decoded_font.value.payload.size()});
        assert(font_payload);
        assert(font_payload.value.face_index == 0U);
        assert(font_payload.value.font_bytes.size() == 12U);
    }

    std::filesystem::remove_all(root);
    return 0;
}
