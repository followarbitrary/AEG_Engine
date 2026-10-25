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
#include "world_command_routes.hpp"

namespace aeg::asset {
inline constexpr std::size_t kMaxGameBuildManifestBytes = 64u * 1024u * 1024u;

struct GameBuildManifest final {
    uint32_t schema_version{3};
    std::string project_uuid{};
    std::string module_id{};
    std::string build_id{};
    std::string module_contract_digest{};
    std::string package_digest{};
    foundation::AssetId startup_scene{};
    std::vector<foundation::AssetId> startup_feature_graphs{};
    std::string target{};
    std::string configuration{};
    std::string package_name{};
    // Relative path and content digest of the executable Feature module bundle.
    std::string module_manifest_name{};
    std::string module_manifest_sha256{};
    std::vector<WorldCommandRoute> world_command_routes{};
};

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_game_build_manifest(
    const GameBuildManifest& manifest);
[[nodiscard]] foundation::ResultValue<GameBuildManifest> decode_game_build_manifest(std::span<const std::byte> bytes);
}  // namespace aeg::asset
