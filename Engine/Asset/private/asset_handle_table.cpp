/** @file asset_handle_table.cpp */
#include "asset_handle_table.hpp"

#include <limits>

namespace aeg::asset {
foundation::ResultValue<uint64_t> AssetHandleTable::intern(foundation::AssetId id, foundation::AssetKind kind) {
    const Key key{id, kind};
    if (const auto found = by_key_.find(key); found != by_key_.end()) {
        return {foundation::Result::success(), found->second};
    }
    if (next_handle_ == 0 || next_handle_ == std::numeric_limits<uint64_t>::max()) {
        return {foundation::out_of_memory("Runtime asset handle table is exhausted"), {}};
    }
    const uint64_t handle = next_handle_++;
    by_key_.emplace(key, handle);
    by_handle_.emplace(handle, Entry{key, handle});
    return {foundation::Result::success(), handle};
}

foundation::ResultValue<foundation::AssetId> AssetHandleTable::resolve(uint64_t handle, foundation::AssetKind expected) const {
    const auto found = by_handle_.find(handle);
    if (found == by_handle_.end())
        return {foundation::not_found("Runtime asset handle is unknown"), {}};
    if (found->second.key.kind != expected) {
        return {foundation::invalid_argument("Runtime asset handle kind does not match"), {}};
    }
    return {foundation::Result::success(), found->second.key.id};
}

foundation::ResultValue<uint64_t> AssetHandleTable::find(foundation::AssetId id, foundation::AssetKind expected) const {
    const auto found = by_key_.find(Key{id, expected});
    if (found == by_key_.end())
        return {foundation::not_found("Runtime asset identity is not interned"), {}};
    return {foundation::Result::success(), found->second};
}
}  // namespace aeg::asset
