/**
 * @file asset_dependency_packer.cpp
 * @brief Dependency-closure package builder implementation.
 */
#include "asset_dependency_packer.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace aeg::tooling {
foundation::Result AssetDependencyPacker::write(const std::filesystem::path& output, std::span<const AssetId> roots,
                                                const CookedAssetResolver& resolve) const {
    if (output.empty() || roots.empty() || !resolve) {
        return foundation::invalid_argument("package output, roots, and resolver are required");
    }

    std::vector<AssetPackageEntry> entries;
    std::vector<AssetId> visited;
    std::function<foundation::Result(AssetId)> visit = [&](AssetId id) -> foundation::Result {
        if (std::find(visited.begin(), visited.end(), id) != visited.end()) {
            return foundation::Result::success();
        }
        const auto bytes = resolve(id);
        if (!bytes) {
            return bytes.result;
        }
        const auto asset = CookedAssetCodec{}.decode(bytes.value);
        if (!asset) {
            return asset.result;
        }
        if (asset.value.id != id) {
            return foundation::invalid_argument("cooked asset resolver returned bytes for the wrong asset identity");
        }
        visited.push_back(id);
        for (const AssetId dependency : asset.value.runtime_dependencies) {
            if (const foundation::Result result = visit(dependency); !result) {
                return result;
            }
        }
        entries.push_back({asset.value.id, asset.value.kind, asset.value.schema_version, std::move(bytes.value)});
        return foundation::Result::success();
    };
    for (const AssetId root : roots) {
        if (const foundation::Result result = visit(root); !result) {
            return result;
        }
    }
    return AssetPackageWriter{}.write(output, std::move(entries));
}
}  // namespace aeg::tooling
