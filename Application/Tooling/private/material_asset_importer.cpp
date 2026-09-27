/** @file material_asset_importer.cpp */
#include "material_asset_importer.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <toml.hpp>
#include <utility>

#include "material_payload.hpp"

namespace aeg::tooling {
namespace {
constexpr uint32_t kMaterialSchemaVersion = 1;

[[nodiscard]] foundation::ResultValue<AssetId> parse_uuid(const toml::value& table, std::string_view key) {
    try {
        return AssetId::parse(toml::find<std::string>(table, std::string(key)));
    } catch (const std::exception& exception) {
        return {foundation::invalid_argument(std::string("Material source field '") + std::string(key) +
                                             "' must be a UUID: " + exception.what()),
                {}};
    }
}

[[nodiscard]] foundation::ResultValue<std::vector<asset::MaterialTextureBinding>> parse_textures(
    const toml::value& document) {
    std::vector<asset::MaterialTextureBinding> textures;
    if (!document.contains("textures"))
        return {foundation::Result::success(), std::move(textures)};
    try {
        const auto& values = toml::find(document, "textures").as_array();
        textures.reserve(values.size());
        for (const toml::value& value : values) {
            for (const auto& entry : value.as_table()) {
                if (entry.first != "binding" && entry.first != "texture")
                    return {foundation::invalid_argument("Unsupported Material texture field: " + entry.first), {}};
            }
            const int64_t binding = toml::find<int64_t>(value, "binding");
            if (binding <= 0) {
                return {foundation::invalid_argument("Material texture binding must be positive"), {}};
            }
            const auto texture = parse_uuid(value, "texture");
            if (!texture)
                return {texture.result, {}};
            textures.push_back({static_cast<uint64_t>(binding), texture.value});
        }
    } catch (const std::exception& exception) {
        return {foundation::invalid_argument(std::string("Material texture table is invalid: ") + exception.what()),
                {}};
    }
    std::sort(textures.begin(), textures.end(),
              [](const auto& left, const auto& right) { return left.binding_id < right.binding_id; });
    for (std::size_t index = 1; index < textures.size(); ++index) {
        if (textures[index - 1].binding_id == textures[index].binding_id)
            return {foundation::invalid_argument("Material source contains duplicate texture bindings"), {}};
    }
    return {foundation::Result::success(), std::move(textures)};
}
}  // namespace

bool MaterialAssetImporter::supports(const std::filesystem::path& source) const noexcept try {
    return source.extension() == ".aegmat";
} catch (...) {
    return false;
}

foundation::ResultValue<std::vector<CookedAsset>> MaterialAssetImporter::cook(const std::filesystem::path& source,
                                                                              const AssetMeta& meta) const try {
    if (meta.products.size() != 1 || meta.products.front().kind != AssetKind::kMaterial ||
        meta.products.front().id == AssetId{}) {
        return {foundation::invalid_argument("Material metadata must declare exactly one valid Material product"), {}};
    }
    const toml::value document = toml::parse(source);
    if (document.contains("shader_path") || document.contains("texture_path") || document.contains("tint") ||
        document.contains("Tint")) {
        return {foundation::invalid_argument("Legacy path/Tint Material sources are rejected; use direct UUID fields"),
                {}};
    }
    if (document.contains("parameters")) {
        return {foundation::invalid_argument("Authored Material parameters are unsupported in v1"), {}};
    }
    for (const auto& entry : document.as_table()) {
        if (entry.first != "schema_version" && entry.first != "shader" && entry.first != "textures") {
            return {foundation::invalid_argument("Unsupported Material source field: " + entry.first), {}};
        }
    }
    const int64_t schema = toml::find<int64_t>(document, "schema_version");
    if (schema != kMaterialSchemaVersion)
        return {foundation::unsupported("Unsupported Material source schema version"), {}};
    const auto shader = parse_uuid(document, "shader");
    if (!shader)
        return {shader.result, {}};
    const auto textures = parse_textures(document);
    if (!textures)
        return {textures.result, {}};

    asset::MaterialPayload payload{};
    payload.schema_version = kMaterialSchemaVersion;
    payload.shader = shader.value;
    payload.textures = textures.value;
    const auto encoded = asset::encode_material_payload(payload);
    if (!encoded)
        return {encoded.result, {}};

    std::vector<AssetId> dependencies;
    dependencies.reserve(1U + textures.value.size());
    dependencies.push_back(shader.value);
    std::vector<AssetId> textures_by_id;
    textures_by_id.reserve(textures.value.size());
    for (const auto& texture : textures.value)
        textures_by_id.push_back(texture.texture);
    std::sort(textures_by_id.begin(), textures_by_id.end());
    textures_by_id.erase(std::unique(textures_by_id.begin(), textures_by_id.end()), textures_by_id.end());
    dependencies.insert(dependencies.end(), textures_by_id.begin(), textures_by_id.end());
    return {foundation::Result::success(),
            {{meta.products.front().id, AssetKind::kMaterial, kMaterialSchemaVersion, std::move(dependencies),
              std::move(encoded.value)}}};
} catch (const std::exception& exception) {
    return {foundation::invalid_argument(std::string("Material source is invalid: ") + exception.what()), {}};
}
}  // namespace aeg::tooling
