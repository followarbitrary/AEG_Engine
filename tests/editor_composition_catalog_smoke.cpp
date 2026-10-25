#include "feature_definition_fixture.hpp"
#include <fstream>
#include "editor_composition_catalog.hpp"
#include "module_bundle.hpp"
#include <array>
#include <iostream>
int main() {
    using namespace aeg;
    auto definition=tests::feature_definition(); feature::FeatureCatalog a; if (!a.export_definition(definition)) return 1;
    auto other=definition; other.id=feature::FeatureDefinitionId{"Game.Other"}; other.contract.definition_id=other.id;
    feature::FeatureCatalog b; if (!b.export_definition(other)) return 1;
    tooling::EditorCompositionCatalog view; std::array<feature::FeatureCatalog,2> catalogs{a,b};
    if (!view.replace_module_catalogs(catalogs) || view.entries().size()!=2 || !view.find(definition.id)) return 1;
    auto duplicate=definition; duplicate.owner_module=feature::ModuleIdentity{"com.aeg.other"};
    feature::FeatureCatalog duplicate_catalog; if (!duplicate_catalog.export_definition(duplicate)) return 2;
    std::array<feature::FeatureCatalog,2> conflicts{a,duplicate_catalog};
    if (view.replace_module_catalogs(conflicts) || view.entries().size()!=2 || view.find(definition.id)->owner_module!=definition.owner_module) return 2;
    module::ModuleMetadata metadata{}; metadata.module={definition.owner_module,"Metadata",{1,0,0},{}}; metadata.feature_catalog=a;
    auto digest=module::module_contract_sha256(metadata); if (!digest) return 3; metadata.contract_digest=digest.value;
    auto encoded=module::encode_module_metadata(metadata); if (!encoded) return 4;
    const auto path=std::filesystem::temp_directory_path() / "aeg_editor_module_metadata.json";
    { std::ofstream out(path); out << encoded.value; }
    if (!view.replace_module_metadata(path) || view.entries().size()!=1 || view.find(definition.id)->contract.parameters!=definition.contract.parameters) return 5;
    metadata.contract_digest="stale"; encoded=module::encode_module_metadata(metadata);
    { std::ofstream out(path); out << encoded.value; }
    if (view.replace_module_metadata(path) || view.entries().size()!=1 || !view.find(definition.id)) return 6;
    std::filesystem::remove(path); std::cout << "Feature module catalog view passed\n";
}
