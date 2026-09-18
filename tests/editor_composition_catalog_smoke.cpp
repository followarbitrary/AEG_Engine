#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "editor_composition_catalog.hpp"
#include "module_bundle.hpp"
int main() {
    using namespace aeg::pipeline;
    TypeCatalog catalog;
    TypeCatalogEntry entry;
    entry.key = {AuthoringTypeKind::kFeature, FeatureId("Game.Outline").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = ModuleId("Game.Main");
    entry.name = "Game.Outline";
    entry.display_name = "Outline";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = kAllPipelineScopes;
    entry.declaration = FeatureAuthoringDeclaration{};
    if (auto result = catalog.add(entry); !result) {
        std::cerr << result.message << '\n';
        return 1;
    }
    TypeCatalog engine_catalog;
    TypeCatalogEntry engine_entry = entry;
    engine_entry.key = {AuthoringTypeKind::kFeature, FeatureId("Game.EngineOutline").value()};
    engine_entry.name = "Game.EngineOutline";
    if (!engine_catalog.add(engine_entry))
        return 2;
    aeg::tooling::EditorCompositionCatalog view;
    std::array<TypeCatalog, 2> catalogs{std::move(catalog), std::move(engine_catalog)};
    if (!view.replace_module_catalogs(catalogs) || view.entries().size() != 2 || view.find(entry.key) == nullptr)
        return 2;
    TypeCatalog conflicting;
    TypeCatalogEntry duplicate = entry;
    duplicate.owner_module = ModuleId("Game.Other");
    if (!conflicting.add(duplicate))
        return 3;
    std::array<TypeCatalog, 2> conflict_catalogs{std::move(catalogs[0]), std::move(conflicting)};
    if (view.replace_module_catalogs(conflict_catalogs))
        return 4;
    aeg::module::ModuleMetadata metadata{};
    metadata.module.id = ModuleId("Game.Metadata");
    metadata.module.name = "Game Metadata";
    metadata.descriptor = metadata.module;
    TypeCatalogEntry metadata_entry = entry;
    metadata_entry.owner_module = metadata.module.id;
    if (!metadata.catalog.add(metadata_entry))
        return 5;
    const auto contract = aeg::module::module_contract_sha256(metadata);
    if (!contract)
        return 6;
    metadata.contract_digest = contract.value;
    const auto encoded = aeg::module::encode_module_metadata(metadata);
    if (!encoded)
        return 7;
    const auto metadata_path = std::filesystem::temp_directory_path() / "aeg_editor_module_metadata.json";
    {
        std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
        output << encoded.value;
    }
    aeg::tooling::EditorCompositionCatalog metadata_view;
    const auto metadata_result = metadata_view.replace_module_metadata(metadata_path);
    if (!metadata_result)
        std::cerr << "metadata load failed: " << metadata_result.message << '\n';
    if (!metadata_result || metadata_view.entries().size() != 1 || metadata_view.find(metadata_entry.key) == nullptr) {
        std::cerr << "metadata entries: " << metadata_view.entries().size() << '\n';
        return 8;
    }
    std::filesystem::remove(metadata_path);
    std::cout << "editor composition catalog smoke passed\n";
    return 0;
}
