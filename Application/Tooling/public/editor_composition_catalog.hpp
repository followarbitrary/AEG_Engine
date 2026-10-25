#pragma once
#include <filesystem>
#include <algorithm>
#include <span>

#include "feature_catalog.hpp"
namespace aeg::tooling {
class EditorCompositionCatalog final {
public:
    [[nodiscard]] foundation::Result replace_catalog(feature::FeatureCatalog catalog);
    /** Replace the view with the deterministic union of catalogs exported by loaded modules. */
    [[nodiscard]] foundation::Result replace_module_catalogs(std::span<const feature::FeatureCatalog> catalogs);
    /**
     * Load one module's pure metadata without loading its DLL. The metadata
     * contract digest is checked before its catalog becomes visible to the UI.
     */
    [[nodiscard]] foundation::Result replace_module_metadata(const std::filesystem::path& metadata_path);
    [[nodiscard]] std::span<const feature::FeatureCatalogEntry> entries() const noexcept { return catalog_.entries(); }
    [[nodiscard]] const feature::FeatureCatalogEntry* find(feature::FeatureDefinitionId key) const noexcept {
        const auto& entries = catalog_.entries();
        const auto found = std::find_if(entries.begin(), entries.end(), [key](const auto& entry) { return entry.definition_id == key; });
        return found == entries.end() ? nullptr : &*found;
    }

private:
    feature::FeatureCatalog catalog_{};
};
}  // namespace aeg::tooling
