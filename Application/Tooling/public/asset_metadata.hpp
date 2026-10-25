/**
 * @file asset_metadata.hpp
 * @brief Stable source and cooked-product identity metadata.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "asset_identity.hpp"
#include "result.hpp"

namespace aeg::tooling {
/**
 * @brief Compatibility aliases for the Engine-owned runtime identity types.
 *
 * Tooling persists and validates these values, but does not define a second
 * UUID or AssetKind ABI. Keeping one identity type prevents metadata, cooked
 * packages and runtime lookups from drifting apart.
 */
using Uuid = foundation::Uuid;
using AssetId = foundation::AssetId;
using AssetKind = foundation::AssetKind;

/** @brief Identity of one source file and its import settings. */
struct SourceId final {
    Uuid value{};

    [[nodiscard]] static foundation::ResultValue<SourceId> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;

    auto operator<=>(const SourceId&) const = default;
};

/** @brief One stable product identity generated from a source file. */
struct CookedProduct final {
    AssetId id{};
    AssetKind kind{AssetKind::kMesh};
    std::string stable_key{};

    auto operator<=>(const CookedProduct&) const = default;
};

/** @brief Persistent metadata beside one raw asset file. */
struct AssetMeta final {
    uint32_t version{1};
    SourceId source{};
    std::string importer{};
    std::vector<CookedProduct> products{};
};

/** @brief Cook-time index translating editor stable ids to canonical runtime AssetIds. */
class StableAssetIdIndex final {
public:
    [[nodiscard]] foundation::Result add(uint64_t stable_id, AssetId asset);
    [[nodiscard]] foundation::ResultValue<AssetId> resolve(uint64_t stable_id) const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    std::map<uint64_t, AssetId> entries_{};
};

/** @brief Parses and writes versioned `.meta` TOML files. */
class AssetMetaCodec final {
public:
    [[nodiscard]] foundation::ResultValue<AssetMeta> load(const std::filesystem::path& file) const;
    [[nodiscard]] foundation::Result save(const std::filesystem::path& file, const AssetMeta& meta) const;
};
}  // namespace aeg::tooling
