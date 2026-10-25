/**
 * @file feature_module.hpp
 * @brief Feature-only authoring surface for static and game modules.
 *
 * This header intentionally contains no Pipeline/RenderPass factories.  A
 * game module publishes FeatureDefinition contracts and executable callbacks;
 * Frame/Render projection registration remains an engine-owned concern.
 */
#pragma once

#include <vector>

#include "feature_catalog.hpp"
#include "feature_contract.hpp"
#include "feature_definition_registration.hpp"
#include "module_descriptor.hpp"

namespace aeg::module {

class ModuleRegistry;

struct FeatureModuleDefinition final {
    foundation::Result status{};
    ModuleDescriptor descriptor{};
    std::vector<feature::FeatureDefinition> feature_definitions{};
    feature::FeatureCatalog feature_catalog{};
};

/** @brief Registers a Feature-only module definition. */
[[nodiscard]] foundation::Result register_static_module(ModuleRegistry&, FeatureModuleDefinition definition);
/** @brief Shared contract/catalog validation for static and dynamic definitions. */
[[nodiscard]] foundation::Result validate_feature_module(const FeatureModuleDefinition& definition);

}  // namespace aeg::module

#define AEG_REGISTER_GAME_FEATURE(definition, value) \
    (::aeg::module::add_feature_definition((definition), (value)))
