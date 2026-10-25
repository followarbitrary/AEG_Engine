#include "../public/feature_module.hpp"
#include "../public/module.hpp"

#include <algorithm>
#include <unordered_set>

namespace aeg::module {
namespace {
foundation::Result validate_feature_catalog(const std::vector<feature::FeatureDefinition>& definitions,
                                            const feature::FeatureCatalog& catalog, const ModuleId owner_module) {
    if (definitions.size() != catalog.entries().size()) {
        return foundation::invalid_argument("Feature catalog does not match module definitions");
    }
    std::unordered_set<feature::FeatureDefinitionId, feature::StableIdHash<feature::FeatureDefinitionIdTag>>
        definition_ids;
    std::unordered_set<feature::FeatureDefinitionId, feature::StableIdHash<feature::FeatureDefinitionIdTag>> catalog_ids;
    for (const auto& entry : catalog.entries()) {
        if (!entry.definition_id.is_valid() || entry.owner_module != owner_module ||
            !catalog_ids.insert(entry.definition_id).second)
            return foundation::invalid_argument("Feature catalog contains duplicate definition identities");
    }
    for (const auto& definition : definitions) {
        if (definition.id != definition.contract.definition_id ||
            definition.version != definition.contract.version || !definition.id.is_valid() ||
            !definition_ids.insert(definition.id).second || definition.owner_module != owner_module) {
            return foundation::invalid_argument("Feature definition identity is invalid");
        }
        if (const auto result = feature::validate_feature_contract(definition.contract); !result) {
            return result;
        }
        const auto iterator = std::find_if(catalog.entries().begin(), catalog.entries().end(),
                                           [&definition](const feature::FeatureCatalogEntry& entry) {
                                               return entry.definition_id == definition.id;
                                           });
        if (iterator == catalog.entries().end() || iterator->version != definition.version ||
            iterator->owner_module != owner_module ||
            iterator->domains != definition.contract.domains || iterator->execution != definition.contract.execution ||
            iterator->state != definition.contract.state || iterator->clock != definition.contract.clock.id ||
            iterator->contract_digest != feature::feature_contract_digest(definition) ||
            iterator->ports != definition.contract.ports) {
            return foundation::invalid_argument("Feature catalog entry does not match its definition");
        }
    }
    for (const auto& entry : catalog.entries()) {
        const auto definition = std::find_if(definitions.begin(), definitions.end(), [&entry](const auto& value) {
            return value.id == entry.definition_id;
        });
        if (definition == definitions.end())
            return foundation::invalid_argument("Feature catalog contains an unregistered definition");
    }
    return foundation::Result::success();
}

class StaticModule final : public Module {
public:
    explicit StaticModule(FeatureModuleDefinition d) : definition_(std::move(d)) {}
    const ModuleDescriptor& descriptor() const noexcept override { return definition_.descriptor; }
    foundation::Result register_module(ModuleRegistrationContext& context) override {
        for (auto& feature_definition : definition_.feature_definitions)
            if (auto r = context.register_feature(std::move(feature_definition)); !r)
                return r;
        return foundation::Result::success();
    }
    foundation::Result unregister_module() override { return foundation::Result::success(); }

private:
    FeatureModuleDefinition definition_;
};
}  // namespace
foundation::Result validate_feature_module(const FeatureModuleDefinition& definition) {
    if (!definition.status || !definition.descriptor.id.is_valid())
        return foundation::invalid_argument("feature module definition is invalid");
    return validate_feature_catalog(definition.feature_definitions, definition.feature_catalog, definition.descriptor.id);
}
foundation::Result register_static_module(ModuleRegistry& registry, FeatureModuleDefinition definition) {
    if (const auto result = validate_feature_module(definition); !result)
        return result;

    return registry.register_module([definition = std::move(definition)]() mutable {
        return std::make_unique<StaticModule>(std::move(definition));
    });
}
}  // namespace aeg::module
