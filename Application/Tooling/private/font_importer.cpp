#include "font_importer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "font_payload.hpp"

namespace aeg::tooling {
namespace {
[[nodiscard]] std::string extension_lower(const std::filesystem::path& source) {
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}
}  // namespace

bool FontImporter::supports(const std::filesystem::path& source) const noexcept {
    const std::string extension = extension_lower(source);
    return extension == ".ttf" || extension == ".otf" || extension == ".ttc";
}

foundation::ResultValue<std::vector<CookedAsset>> FontImporter::cook(const std::filesystem::path& source,
                                                                     const AssetMeta& meta) const {
    std::ifstream stream(source, std::ios::binary);
    if (!stream) {
        return {foundation::not_found("Font source cannot be opened"), {}};
    }
    const std::vector<char> chars{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (chars.empty()) {
        return {foundation::invalid_argument("Font source is empty"), {}};
    }
    std::vector<std::byte> raw(chars.size());
    std::transform(chars.begin(), chars.end(), raw.begin(),
                   [](char value) { return static_cast<std::byte>(static_cast<unsigned char>(value)); });

    std::vector<CookedAsset> result;
    result.reserve(meta.products.size());
    for (const CookedProduct& product : meta.products) {
        if (product.kind != AssetKind::kFont) {
            return {foundation::invalid_argument("Font importer received non-font product"), {}};
        }
        asset::FontPayload payload{};
        payload.font_bytes = raw;
        const auto encoded = asset::encode_font_payload(payload);
        if (!encoded) {
            return {encoded.result, {}};
        }
        result.push_back({product.id, product.kind, 1, {}, std::move(encoded.value)});
    }
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::tooling
