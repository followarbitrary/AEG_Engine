/**
 * @file asset_metadata.cpp
 * @brief Implements asset metadata validation and TOML encoding.
 */
#include "asset_metadata.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string_view>
#include <toml.hpp>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aeg::tooling {
namespace {
constexpr uint32_t kCurrentMetaVersion = 1;

[[nodiscard]] foundation::Result validate_meta(const AssetMeta& meta) {
    if (meta.version != kCurrentMetaVersion || meta.importer.empty()) {
        return foundation::invalid_argument("Asset meta version and importer must be valid");
    }

    std::vector<AssetId> ids;
    std::vector<std::string> keys;
    ids.reserve(meta.products.size());
    keys.reserve(meta.products.size());
    for (const CookedProduct& product : meta.products) {
        if (product.stable_key.empty()) {
            return foundation::invalid_argument("Asset product stable key must be non-empty");
        }
        ids.push_back(product.id);
        keys.push_back(product.stable_key);
    }
    std::sort(ids.begin(), ids.end());
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end() ||
        std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
        return foundation::invalid_argument("Asset meta contains duplicate product identities");
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result write_text(const std::filesystem::path& file, std::string_view text) {
    const std::filesystem::path temporary = file.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot open asset meta file for writing");
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    if (!stream) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write asset meta file");
    }
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace asset meta file");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace asset meta file");
    }
#endif
    return foundation::Result::success();
}

[[nodiscard]] foundation::ResultValue<AssetKind> parse_asset_kind(std::string_view name) {
    if (name == "mesh") {
        return {foundation::Result::success(), AssetKind::kMesh};
    }
    if (name == "texture") {
        return {foundation::Result::success(), AssetKind::kTexture};
    }
    if (name == "material") {
        return {foundation::Result::success(), AssetKind::kMaterial};
    }
    if (name == "scene") {
        return {foundation::Result::success(), AssetKind::kScene};
    }
    if (name == "font") {
        return {foundation::Result::success(), AssetKind::kFont};
    }
    if (name == "pipeline") {
        return {foundation::Result::success(), AssetKind::kPipeline};
    }
    if (name == "shader") {
        return {foundation::Result::success(), AssetKind::kShader};
    }
    return {foundation::unsupported("Unsupported asset product kind"), {}};
}

[[nodiscard]] std::string_view asset_kind_name(AssetKind kind) noexcept {
    switch (kind) {
        case AssetKind::kMesh:
            return "mesh";
        case AssetKind::kTexture:
            return "texture";
        case AssetKind::kMaterial:
            return "material";
        case AssetKind::kScene:
            return "scene";
        case AssetKind::kFont:
            return "font";
        case AssetKind::kPipeline:
            return "pipeline";
        case AssetKind::kShader:
            return "shader";
    }
    return "unknown";
}
}  // namespace

foundation::ResultValue<SourceId> SourceId::parse(std::string_view text) {
    const auto uuid = Uuid::parse(text);
    return uuid ? foundation::ResultValue<SourceId>{foundation::Result::success(), SourceId{uuid.value}}
                : foundation::ResultValue<SourceId>{uuid.result, {}};
}

std::string SourceId::to_string() const {
    return value.to_string();
}

foundation::ResultValue<AssetMeta> AssetMetaCodec::load(const std::filesystem::path& file) const {
    try {
        const toml::value document = toml::parse(file);
        const int64_t version = toml::find<int64_t>(document, "version");
        if (version < 0 || version > std::numeric_limits<uint32_t>::max() ||
            static_cast<uint32_t>(version) != kCurrentMetaVersion) {
            return {foundation::unsupported("Unsupported asset meta format version"), {}};
        }

        const auto source = SourceId::parse(toml::find<std::string>(document, "source_uuid"));
        if (!source) {
            return {source.result, {}};
        }

        AssetMeta meta{};
        meta.version = static_cast<uint32_t>(version);
        meta.source = source.value;
        meta.importer = toml::find<std::string>(document, "importer");
        for (const toml::value& product_value : toml::find(document, "products").as_array()) {
            const auto id = AssetId::parse(toml::find<std::string>(product_value, "uuid"));
            const auto kind = parse_asset_kind(toml::find<std::string>(product_value, "kind"));
            if (!id) {
                return {id.result, {}};
            }
            if (!kind) {
                return {kind.result, {}};
            }
            meta.products.push_back({id.value, kind.value, toml::find<std::string>(product_value, "key")});
        }
        if (const foundation::Result result = validate_meta(meta); !result) {
            return {result, {}};
        }
        return {foundation::Result::success(), std::move(meta)};
    } catch (const std::exception& exception) {
        return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument, exception.what()), {}};
    }
}

foundation::Result AssetMetaCodec::save(const std::filesystem::path& file, const AssetMeta& meta) const {
    if (const foundation::Result result = validate_meta(meta); !result) {
        return result;
    }
    try {
        toml::value document(toml::table{});
        document["version"] = static_cast<int64_t>(meta.version);
        document["source_uuid"] = meta.source.to_string();
        document["importer"] = meta.importer;
        document["products"] = toml::array{};
        for (const CookedProduct& product : meta.products) {
            toml::value encoded_product(toml::table{});
            encoded_product["uuid"] = product.id.to_string();
            encoded_product["kind"] = asset_kind_name(product.kind);
            encoded_product["key"] = product.stable_key;
            document["products"].as_array().push_back(std::move(encoded_product));
        }
        return write_text(file, toml::format(document));
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    }
}
}  // namespace aeg::tooling
