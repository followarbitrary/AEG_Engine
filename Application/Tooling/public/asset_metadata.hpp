/**
 * @file asset_metadata.hpp
 * @brief Stable source and cooked-product identity metadata.
 */
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "result.hpp"

namespace aeg::tooling {
/** @brief A canonical 128-bit UUID stored without platform-specific types. */
struct Uuid final {
    std::array<uint8_t, 16> bytes{};

    [[nodiscard]] static foundation::ResultValue<Uuid> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;

    auto operator<=>(const Uuid&) const = default;
};

/** @brief Identity of one source file and its import settings. */
struct SourceId final {
    Uuid value{};

    [[nodiscard]] static foundation::ResultValue<SourceId> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;

    auto operator<=>(const SourceId&) const = default;
};

/** @brief Identity of one independently addressable cooked product. */
struct AssetId final {
    Uuid value{};

    [[nodiscard]] static foundation::ResultValue<AssetId> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;

    auto operator<=>(const AssetId&) const = default;
};

/** @brief Runtime asset data category emitted by a cooker. */
enum class AssetKind : uint8_t {
    kMesh,
    kTexture,
    kMaterial,
    kScene,
    kFont,
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

/** @brief Parses and writes versioned `.meta` TOML files. */
class AssetMetaCodec final {
public:
    [[nodiscard]] foundation::ResultValue<AssetMeta> load(const std::filesystem::path& file) const;
    [[nodiscard]] foundation::Result save(const std::filesystem::path& file, const AssetMeta& meta) const;
};
}  // namespace aeg::tooling
