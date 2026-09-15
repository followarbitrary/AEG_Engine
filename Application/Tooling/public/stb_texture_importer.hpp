/**
 * @file stb_texture_importer.hpp
 * @brief stb_image-backed importer for raster texture source files.
 */
#pragma once

#include "asset_cooker.hpp"

namespace aeg::tooling {
/** @brief Decodes one raster source into an RGBA8 texture payload. */
class StbTextureImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports(const std::filesystem::path& source) const noexcept override;
    [[nodiscard]] foundation::ResultValue<std::vector<CookedAsset>> cook(const std::filesystem::path& source,
                                                                         const AssetMeta& meta) const override;
};
}  // namespace aeg::tooling
