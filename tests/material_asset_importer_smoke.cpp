#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "asset_cooker.hpp"
#include "material_asset_importer.hpp"
#include "material_payload.hpp"

namespace {
using aeg::foundation::AssetId;
using aeg::foundation::AssetKind;
using aeg::asset::MaterialPayload;
using aeg::tooling::AssetCooker;
using aeg::tooling::AssetMeta;
using aeg::tooling::AssetMetaCodec;
using aeg::tooling::MaterialAssetImporter;

AssetId id(std::string_view value) {
    const auto parsed = AssetId::parse(value);
    assert(parsed);
    return parsed.value;
}

AssetMeta material_meta(const AssetId product) {
    return {
        1, {id("11111111-2222-3333-4444-555555555555").value}, "material", {{product, AssetKind::kMaterial, "main"}}};
}

std::filesystem::path write_source(const std::filesystem::path& root, std::string_view name, std::string_view source) {
    const auto path = root / std::string(name);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << source;
    stream.close();
    return path;
}

void assert_invalid(const MaterialAssetImporter& importer, const std::filesystem::path& path, const AssetMeta& meta) {
    const auto result = importer.cook(path, meta);
    assert(!result);
    assert(result.result.code == aeg::foundation::ResultCode::kInvalidArgument);
}
}  // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("aeg_material_importer_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    const AssetId product = id("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    const AssetId shader = id("11111111-1111-1111-1111-111111111111");
    const AssetId texture_a = id("22222222-2222-2222-2222-222222222222");
    const AssetId texture_b = id("33333333-3333-3333-3333-333333333333");
    const AssetMeta meta = material_meta(product);
    assert(AssetMetaCodec{}.save(root / "valid.aegmat.meta", meta));

    const auto valid = write_source(root, "valid.aegmat",
                                    "schema_version = 1\n"
                                    "shader = \"11111111-1111-1111-1111-111111111111\"\n\n"
                                    "[[textures]]\n"
                                    "binding = 2\n"
                                    "texture = \"33333333-3333-3333-3333-333333333333\"\n\n"
                                    "[[textures]]\n"
                                    "binding = 1\n"
                                    "texture = \"22222222-2222-2222-2222-222222222222\"\n");
    MaterialAssetImporter importer;
    assert(importer.supports(valid));
    assert(!importer.supports(root / "valid.material"));

    const auto cooked = importer.cook(valid, meta);
    assert(cooked);
    assert(cooked.value.size() == 1U);
    assert(cooked.value.front().id == product);
    assert(cooked.value.front().kind == AssetKind::kMaterial);
    assert(cooked.value.front().schema_version == 1U);
    assert(cooked.value.front().runtime_dependencies.size() == 3U);
    assert(cooked.value.front().runtime_dependencies[0] == shader);
    assert(cooked.value.front().runtime_dependencies[1] == texture_a);
    assert(cooked.value.front().runtime_dependencies[2] == texture_b);
    const auto decoded = aeg::asset::decode_material_payload(cooked.value.front().payload);
    assert(decoded);
    assert(decoded.value.shader == shader);
    assert(decoded.value.parameters.empty());
    assert(decoded.value.textures.size() == 2U);
    assert(decoded.value.textures[0].binding_id == 1U);
    assert(decoded.value.textures[0].texture == texture_a);
    assert(decoded.value.textures[1].binding_id == 2U);
    assert(decoded.value.textures[1].texture == texture_b);
    const auto encoded_again = aeg::asset::encode_material_payload(decoded.value);
    assert(encoded_again);
    assert(encoded_again.value == cooked.value.front().payload);
    auto reordered = decoded.value;
    std::swap(reordered.textures[0], reordered.textures[1]);
    assert(aeg::asset::encode_material_payload(reordered).value == encoded_again.value);
    reordered.parameters = {{7, true},
                            {6, int64_t{-8}},
                            {5, uint64_t{9}},
                            {4, 0.5},
                            {3, std::string("value")},
                            {2, std::vector<double>{1.0, 2.0}},
                            {1, aeg::foundation::AssetReference{texture_a.to_string()}}};
    const auto with_parameters = aeg::asset::encode_material_payload(reordered);
    assert(with_parameters);
    const auto round_trip = aeg::asset::decode_material_payload(with_parameters.value);
    assert(round_trip);
    assert(round_trip.value.parameters == reordered.parameters);
    assert(aeg::asset::encode_material_payload(round_trip.value).value == with_parameters.value);

    const auto legacy = write_source(root, "legacy.aegmat",
                                     "schema_version = 1\nshader_path = \"Shaders/Tint.shader\"\n"
                                     "texture_path = \"Textures/white.png\"\n[tint]\nvalue = [1, 1, 1, 1]\n");
    assert_invalid(importer, legacy, meta);

    assert_invalid(importer, write_source(root, "missing_shader.aegmat", "schema_version = 1\n"), meta);
    assert_invalid(importer,
                   write_source(root, "duplicate_binding.aegmat",
                                "schema_version = 1\nshader = \"11111111-1111-1111-1111-111111111111\"\n"
                                "[[textures]]\nbinding = 1\ntexture = \"22222222-2222-2222-2222-222222222222\"\n"
                                "[[textures]]\nbinding = 1\ntexture = \"33333333-3333-3333-3333-333333333333\"\n"),
                   meta);
    assert_invalid(importer,
                   write_source(root, "authored_parameters.aegmat",
                                "schema_version = 1\nshader = \"11111111-1111-1111-1111-111111111111\"\n"
                                "[parameters]\ntint = [1.0, 1.0, 1.0, 1.0]\n"),
                   meta);

    AssetMeta wrong_meta = meta;
    wrong_meta.products.front().kind = AssetKind::kTexture;
    assert_invalid(importer, valid, wrong_meta);

    MaterialPayload invalid_payload{};
    invalid_payload.shader = shader;
    invalid_payload.textures = {{0, texture_a}};
    assert(!aeg::asset::encode_material_payload(invalid_payload));
    invalid_payload.textures = {{1, texture_a}, {1, texture_b}};
    assert(!aeg::asset::encode_material_payload(invalid_payload));
    invalid_payload.textures = {{1, AssetId{}}};
    assert(!aeg::asset::encode_material_payload(invalid_payload));
    invalid_payload.textures = {{1, texture_a}};
    invalid_payload.shader = {};
    assert(!aeg::asset::encode_material_payload(invalid_payload));
    invalid_payload.shader = shader;
    invalid_payload.parameters = {{1, std::numeric_limits<double>::infinity()}};
    assert(!aeg::asset::encode_material_payload(invalid_payload));
    invalid_payload.parameters = {{1, std::vector<double>{std::numeric_limits<double>::quiet_NaN()}}};
    assert(!aeg::asset::encode_material_payload(invalid_payload));
    auto zero_shader = encoded_again.value;
    std::fill(zero_shader.begin() + 8, zero_shader.begin() + 24, std::byte{0});
    assert(!aeg::asset::decode_material_payload(zero_shader));
    auto zero_texture = encoded_again.value;
    std::fill(zero_texture.begin() + 36, zero_texture.begin() + 52, std::byte{0});
    assert(!aeg::asset::decode_material_payload(zero_texture));

    // Default registration must make valid Material sources cook. A Material
    // source with a mismatched Font product is rejected by the selected
    // Material importer before any artifact is published.
    AssetCooker cooker(root / "Cache" / "Cooked");
    assert(cooker.register_default_importers());
    const auto default_result = cooker.cook(valid);
    assert(default_result);
    assert(default_result.value.artifacts.size() == 1U);

    const auto font = write_source(root, "font.aegmat", "schema_version = 1\n");
    AssetMeta font_meta = meta;
    font_meta.products.front().kind = AssetKind::kFont;
    font_meta.importer = "font";
    assert(AssetMetaCodec{}.save(font.string() + ".meta", font_meta));
    const auto font_result = cooker.cook(font);
    assert(!font_result);
    assert(font_result.result.code == aeg::foundation::ResultCode::kInvalidArgument);
    assert(font_result.result.message.find("Material metadata") != std::string::npos);

    std::filesystem::remove_all(root);
    return 0;
}
