/**
 * @file cgltf_mesh_importer.hpp
 * @brief cgltf-backed importer for triangle mesh primitives.
 */
#pragma once

#include "asset_cooker.hpp"

namespace aeg::tooling {
/** @brief Decodes glTF triangle primitives into interleaved position, normal, and UV mesh payloads. */
class CgltfMeshImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports(const std::filesystem::path& source) const noexcept override;
    [[nodiscard]] foundation::ResultValue<std::vector<CookedAsset>> cook(const std::filesystem::path& source,
                                                                         const AssetMeta& meta) const override;
};
}  // namespace aeg::tooling
