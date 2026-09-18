/**
 * @file game_build_manifest.hpp
 * @brief Runtime-neutral identity manifest for one cooked game build.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "asset_identity.hpp"

namespace aeg::asset {
struct GameBuildManifest final {
    uint32_t schema_version{1};
    std::string project_uuid{};
    std::string module_id{};
    std::string build_id{};
    std::string module_contract_digest{};
    std::string package_digest{};
    AssetId startup_scene{};
    std::vector<AssetId> startup_pipelines{};
    std::string target{};
    std::string configuration{};
    std::string package_name{};
};

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_game_build_manifest(
    const GameBuildManifest& manifest);
[[nodiscard]] foundation::ResultValue<GameBuildManifest> decode_game_build_manifest(std::span<const std::byte> bytes);
}  // namespace aeg::asset
