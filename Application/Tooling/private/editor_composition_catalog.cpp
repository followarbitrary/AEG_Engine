#include "../public/editor_composition_catalog.hpp"

#include "../../../Engine/Module/public/module_bundle.hpp"
namespace aeg::tooling {
foundation::Result EditorCompositionCatalog::replace_catalog(pipeline::TypeCatalog catalog) {
    catalog_ = std::move(catalog);
    return foundation::Result::success();
}
foundation::Result EditorCompositionCatalog::replace_module_catalogs(std::span<const pipeline::TypeCatalog> catalogs) {
    pipeline::TypeCatalog merged;
    for (const auto& catalog : catalogs)
        for (const auto& entry : catalog.entries())
            if (auto result = merged.add(entry); !result)
                return result;
    catalog_ = std::move(merged);
    return foundation::Result::success();
}

foundation::Result EditorCompositionCatalog::replace_module_metadata(const std::filesystem::path& metadata_path) {
    const auto loaded = module::read_module_metadata(metadata_path);
    if (!loaded)
        return loaded.result;
    if (loaded.value.contract_digest.empty())
        return foundation::invalid_argument("Module metadata contract digest is missing");
    const auto contract = module::module_contract_sha256(loaded.value);
    if (!contract)
        return contract.result;
    if (contract.value != loaded.value.contract_digest)
        return foundation::invalid_argument("Module metadata contract digest is stale");
    return replace_catalog(loaded.value.catalog);
}
}  // namespace aeg::tooling
