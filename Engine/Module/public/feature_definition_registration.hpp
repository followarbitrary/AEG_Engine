/**
 * @file feature_definition_registration.hpp
 * @brief Shared FeatureDefinition authoring helper used by module facades.
 */
#pragma once

#include <concepts>
#include <new>
#include <utility>

#include "feature_catalog.hpp"
#include "feature_contract.hpp"

namespace aeg::module {
namespace detail {
template <typename Definition>
concept FeatureDefinitionContainer = requires(Definition& definition) {
    definition.status;
    definition.descriptor.id;
    definition.feature_definitions;
    definition.feature_catalog;
};
}  // namespace detail

/** @brief Adds one validated FeatureDefinition to any module authoring facade. */
template <detail::FeatureDefinitionContainer Definition>
[[nodiscard]] inline foundation::Result add_feature_definition(Definition& definition,
                                                               feature::FeatureDefinition feature_definition) {
    if (!definition.status) {
        return definition.status;
    }
    if (!definition.descriptor.id.is_valid()) {
        definition.status = foundation::invalid_argument("Feature module identity is invalid");
        return definition.status;
    }
    if (feature_definition.owner_module.is_valid() &&
        feature_definition.owner_module != definition.descriptor.id) {
        definition.status = foundation::invalid_argument("Feature owner module does not match module descriptor");
        return definition.status;
    }
    feature_definition.owner_module = definition.descriptor.id;
    if (feature_definition.id != feature_definition.contract.definition_id ||
        feature_definition.version != feature_definition.contract.version ||
        !feature_definition.id.is_valid()) {
        definition.status = foundation::invalid_argument("Feature definition identity is invalid");
        return definition.status;
    }
    if (const auto result = feature::validate_feature_contract(feature_definition.contract); !result) {
        definition.status = result;
        return definition.status;
    }
    try {
        definition.feature_definitions.push_back(std::move(feature_definition));
        if (const auto result = definition.feature_catalog.export_definition(
                definition.feature_definitions.back()); !result) {
            definition.feature_definitions.pop_back();
            definition.status = result;
            return definition.status;
        }
    } catch (const std::bad_alloc&) {
        definition.status = foundation::out_of_memory("Feature definition registration allocation failed");
        return definition.status;
    }
    return foundation::Result::success();
}

}  // namespace aeg::module
