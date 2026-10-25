/**
 * @file asset_handle_table.hpp
 * @brief Session-local UUID to opaque handle mapping.
 */
#pragma once

#include <cstdint>
#include <map>

#include "asset_identity.hpp"

namespace aeg::asset {
/** @brief Monotonic, non-serializable runtime handles keyed by UUID and kind. */
class AssetHandleTable final {
public:
    [[nodiscard]] foundation::ResultValue<uint64_t> intern(foundation::AssetId id, foundation::AssetKind kind);
    [[nodiscard]] foundation::ResultValue<foundation::AssetId> resolve(uint64_t handle, foundation::AssetKind expected) const;
    [[nodiscard]] foundation::ResultValue<uint64_t> find(foundation::AssetId id, foundation::AssetKind expected) const;

private:
    struct Key final {
        foundation::AssetId id{};
        foundation::AssetKind kind{foundation::AssetKind::kMesh};
        auto operator<=>(const Key&) const = default;
    };
    struct Entry final {
        Key key{};
        uint64_t handle{};
    };

    uint64_t next_handle_{1};
    std::map<Key, uint64_t> by_key_{};
    std::map<uint64_t, Entry> by_handle_{};
};
}  // namespace aeg::asset
