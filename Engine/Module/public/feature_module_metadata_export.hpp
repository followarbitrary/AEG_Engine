#pragma once

#include <string_view>
#include <utility>

#include "feature_module.hpp"
#include "module_bundle.hpp"

namespace aeg::module {

/**
 * Exports contracts from the same definition factory used by the game module.
 * This validates declarations and copies values; it never registers a Runtime
 * or invokes execute/state-validation callbacks. The factory itself must only
 * construct definitions and must be safe to run in a build host process.
 */
[[nodiscard]] inline foundation::ResultValue<ModuleMetadata> export_feature_module_metadata(
    const FeatureModuleDefinition& definition, std::string_view build_id,
    std::string_view target, std::string_view configuration) {
    if (!definition.status)
        return {definition.status, {}};
    if (build_id.empty() || target.empty() || configuration.empty())
        return {foundation::invalid_argument("module metadata export requires build id, target and configuration"), {}};
    if (const auto valid = validate_feature_module(definition); !valid)
        return {valid, {}};

    ModuleMetadata metadata{};
    metadata.module = definition.descriptor;
    metadata.build_id = build_id;
    metadata.target = target;
    metadata.configuration = configuration;
    for (const auto& item : definition.feature_definitions) {
        if (item.contract.structure == feature::FeatureStructure::kAtomic && !item.execute)
            return {foundation::invalid_argument("dynamic atomic Feature has no executable callback"), {}};
        if (const auto exported = metadata.feature_catalog.export_definition(item); !exported)
            return {exported, {}};
    }
    const auto digest = module_contract_sha256(metadata);
    if (!digest)
        return {digest.result, {}};
    metadata.contract_digest = digest.value;
    return {foundation::Result::success(), std::move(metadata)};
}

}  // namespace aeg::module
