/**
 * @file module_identity.hpp
 * @brief Stable module identity and version with no execution-graph dependency.
 */
#pragma once

#include <compare>
#include <cstdint>

#include "feature_types.hpp"

namespace aeg::module {

// Module identity is shared with Feature metadata so ownership can be checked
// without making the Feature layer depend on ModuleRegistry.
using ModuleIdTag = feature::ModuleIdentityTag;
using ModuleId = feature::ModuleIdentity;
using ModuleIdHash = feature::ModuleIdentityHash;

/** @brief Version shared by module metadata and its internal execution projections. */
struct Version final {
    uint32_t major{0};
    uint32_t minor{0};
    uint32_t patch{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return major != 0 || minor != 0 || patch != 0; }
    friend constexpr bool operator==(Version, Version) = default;
    friend constexpr auto operator<=>(Version, Version) = default;
};

}  // namespace aeg::module
