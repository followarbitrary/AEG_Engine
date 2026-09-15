/**
 * @file stb_texture_importer.cpp
 * @brief stb_image texture importer implementation.
 */
#include "stb_texture_importer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace aeg::tooling {
namespace {
constexpr uint32_t kTexturePayloadSchemaVersion = 1;
constexpr uint32_t kRgba8ChannelCount = 4;

void write_u32(std::vector<std::byte>& bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::string lowercase_extension(const std::filesystem::path& source) {
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension;
}
}  // namespace

bool StbTextureImporter::supports(const std::filesystem::path& source) const noexcept try {
    const std::string extension = lowercase_extension(source);
    constexpr std::array<std::string_view, 7> kExtensions{
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".hdr",
    };
    return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
} catch (...) {
    return false;
}

foundation::ResultValue<std::vector<CookedAsset>> StbTextureImporter::cook(const std::filesystem::path& source,
                                                                           const AssetMeta& meta) const {
    const auto product = std::find_if(meta.products.begin(), meta.products.end(), [](const CookedProduct& candidate) {
        return candidate.kind == AssetKind::kTexture;
    });
    if (product == meta.products.end()) {
        return {foundation::invalid_argument("texture source metadata does not declare a texture product"), {}};
    }
    if (std::find_if(std::next(product), meta.products.end(), [](const CookedProduct& candidate) {
            return candidate.kind == AssetKind::kTexture;
        }) != meta.products.end()) {
        return {foundation::unsupported("one raster source may declare only one texture product"), {}};
    }

    int width{};
    int height{};
    int channels{};
    stbi_uc* pixels =
        stbi_load(source.string().c_str(), &width, &height, &channels, static_cast<int>(kRgba8ChannelCount));
    if (pixels == nullptr) {
        const char* failure = stbi_failure_reason();
        return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            failure == nullptr ? "stb_image could not decode texture" : failure),
                {}};
    }
    const auto release_pixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>(pixels, stbi_image_free);
    if (width <= 0 || height <= 0 ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) >
            std::numeric_limits<uint32_t>::max() / kRgba8ChannelCount) {
        return {foundation::invalid_argument("decoded texture dimensions are invalid or too large"), {}};
    }

    const size_t pixel_byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * kRgba8ChannelCount;
    std::vector<std::byte> payload(12U + pixel_byte_count);
    write_u32(payload, 0, static_cast<uint32_t>(width));
    write_u32(payload, 4, static_cast<uint32_t>(height));
    write_u32(payload, 8, kRgba8ChannelCount);
    for (size_t index = 0; index < pixel_byte_count; ++index) {
        payload[12U + index] = static_cast<std::byte>(pixels[index]);
    }
    return {{}, {{product->id, AssetKind::kTexture, kTexturePayloadSchemaVersion, {}, std::move(payload)}}};
}
}  // namespace aeg::tooling
