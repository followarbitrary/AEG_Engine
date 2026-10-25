/**
 * @file cooked_artifact_store.hpp
 * @brief Transactional publication of immutable cooked asset bundles.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "asset_metadata.hpp"

namespace aeg::tooling {
/** @brief Replaces a selector file atomically after a complete bundle is staged. */
[[nodiscard]] foundation::Result replace_file_atomically(const std::filesystem::path& source,
                                                         const std::filesystem::path& destination);

/** @brief One complete FlatBuffers product awaiting cache publication. */
struct CookedArtifact final {
    AssetId id{};
    AssetKind kind{AssetKind::kMesh};
    uint32_t schema_version{};
    std::vector<std::byte> bytes{};
};

/** @brief The selected immutable bundle for a source asset. */
struct CookedArtifactBundle final {
    std::filesystem::path manifest{};
    std::vector<CookedArtifact> artifacts{};
};

/**
 * @brief Stores complete cooked bundles and switches their current manifest last.
 *
 * Product files and their versioned manifest are never modified after publication.
 * A source-local `current.toml` is the only mutable selection point.
 */
class CookedArtifactStore final {
public:
    explicit CookedArtifactStore(std::filesystem::path cache_root);

    [[nodiscard]] foundation::ResultValue<CookedArtifactBundle> publish(
        const SourceId& source, const std::vector<CookedArtifact>& artifacts) const;
    [[nodiscard]] foundation::ResultValue<CookedArtifactBundle> load_current(const SourceId& source) const;

private:
    std::filesystem::path cache_root_{};
};

/** @brief Rebuilds an AssetId lookup from the current immutable bundle of every source. */
class CookedArtifactIndex final {
public:
    explicit CookedArtifactIndex(std::filesystem::path cache_root);

    [[nodiscard]] foundation::Result refresh();
    [[nodiscard]] foundation::ResultValue<std::vector<std::byte>> read(AssetId id) const;

private:
    std::filesystem::path cache_root_{};
    std::map<AssetId, CookedArtifact> artifacts_{};
};
}  // namespace aeg::tooling
