/** Editor-only names never replace canonical Feature identity or contracts. */
#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>
#include "editor_node_definition.hpp"

namespace aeg::application {
inline const NodeDefinitionDraft* feature_presentation(const feature::FeatureCatalogEntry& entry,
    std::span<const NodeDefinitionDraft> presentations) {
    const NodeDefinitionDraft* found = nullptr;
    for (const auto& value : presentations) {
        if (value.entry.id == entry.definition_id && value.entry.version == entry.version &&
            value.entry.owner_module == entry.owner_module &&
            feature::feature_contract_digest(value.entry) == entry.contract_digest &&
            (found == nullptr || (found->generated && !value.generated))) found = &value;
    }
    return found;
}

inline std::string feature_presentation_name(const feature::FeatureCatalogEntry& entry,
    std::span<const NodeDefinitionDraft> presentations) {
    if (const auto* value = feature_presentation(entry, presentations)) {
        if (!value->display_name.empty()) return value->display_name;
        if (!value->name.empty()) return value->name;
    }
    return "Feature " + std::to_string(entry.definition_id.value());
}

inline bool feature_presentation_matches(const feature::FeatureCatalogEntry& entry,
    std::span<const NodeDefinitionDraft> presentations, std::string_view query) {
    const auto contains = [query](std::string_view text) {
        return std::search(text.begin(), text.end(), query.begin(), query.end(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        }) != text.end();
    };
    if (query.empty() || contains(std::to_string(entry.definition_id.value()))) return true;
    const auto* value = feature_presentation(entry, presentations);
    return value && (contains(value->name) || contains(value->display_name));
}
} // namespace aeg::application
