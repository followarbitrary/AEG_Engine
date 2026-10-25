/**
 * @file asset_identity.hpp
 * @brief Runtime-owned UUID and asset-kind identities.
 */
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "result.hpp"

namespace aeg::foundation {
/** @brief Canonical 128-bit UUID with no Tooling dependencies. */
struct Uuid final {
    std::array<uint8_t, 16> bytes{};

    [[nodiscard]] static ResultValue<Uuid> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;
    auto operator<=>(const Uuid&) const = default;
};

/** @brief Stable identity of one independently addressable runtime asset. */
struct AssetId final {
    Uuid value{};

    [[nodiscard]] static ResultValue<AssetId> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;
    auto operator<=>(const AssetId&) const = default;
};

/** @brief Runtime asset categories; numeric values are package ABI. */
enum class AssetKind : uint8_t {
    kMesh = 0,
    kTexture = 1,
    kMaterial = 2,
    kScene = 3,
    kFont = 4,
    kFeatureGraph = 5,
    kShader = 6,
};
}  // namespace aeg::foundation
