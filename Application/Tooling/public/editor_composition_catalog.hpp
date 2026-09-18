#pragma once
#include <filesystem>
#include <span>

#include "../../../Engine/Pipeline/public/pipeline_authoring.hpp"
namespace aeg::tooling {
class EditorCompositionCatalog final {
public:
    [[nodiscard]] foundation::Result replace_catalog(pipeline::TypeCatalog catalog);
    /** Replace the view with the deterministic union of catalogs exported by loaded modules. */
    [[nodiscard]] foundation::Result replace_module_catalogs(std::span<const pipeline::TypeCatalog> catalogs);
    /**
     * Load one module's pure metadata without loading its DLL. The metadata
     * contract digest is checked before its catalog becomes visible to the UI.
     */
    [[nodiscard]] foundation::Result replace_module_metadata(const std::filesystem::path& metadata_path);
    [[nodiscard]] std::span<const pipeline::TypeCatalogEntry> entries() const noexcept { return catalog_.entries(); }
    [[nodiscard]] const pipeline::TypeCatalogEntry* find(pipeline::AuthoringTypeKey key) const noexcept {
        return catalog_.find(key);
    }

private:
    pipeline::TypeCatalog catalog_{};
};
}  // namespace aeg::tooling
