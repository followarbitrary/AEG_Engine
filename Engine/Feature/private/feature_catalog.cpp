#include "feature_catalog.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace aeg::feature {

foundation::Result FeatureCatalog::export_definition(const FeatureDefinition& definition) {
    if (definition.id != definition.contract.definition_id || definition.version != definition.contract.version) {
        return foundation::invalid_argument("definition identity does not match its contract");
    }
    if (auto result = validate_feature_contract(definition.contract); !result) {
        return result;
    }
    return add_entry({definition.id, definition.version, definition.contract.domains,
                      definition.contract.execution, definition.contract.state,
                      definition.contract.clock.id, feature_contract_digest(definition),
                      definition.contract.ports, definition.contract, definition.owner_module});
}

foundation::Result FeatureCatalog::add_entry(FeatureCatalogEntry entry) {
    if (!entry.definition_id.is_valid() || entry.version.major == 0 || entry.domains == 0 ||
        (entry.domains & ~kAllFeatureDomains) != 0 || !entry.clock.is_valid()) {
        return foundation::invalid_argument("feature catalog entry is invalid");
    }
    std::unordered_set<PortId, StableIdHash<PortIdTag>> ports;
    for (const auto& port : entry.ports) {
        if (!port.id.is_valid() || !port.schema.is_valid() || port.version == 0 ||
            !ports.insert(port.id).second)
            return foundation::invalid_argument("feature catalog port schema is invalid");
    }
    if (entry.contract.definition_id.is_valid()) {
        if (auto result = validate_feature_contract(entry.contract); !result)
            return result;
        if (entry.contract.definition_id != entry.definition_id ||
            entry.contract.version != entry.version || entry.contract.domains != entry.domains ||
            entry.contract.execution != entry.execution || entry.contract.state != entry.state ||
            entry.contract.clock.id != entry.clock || entry.contract.ports != entry.ports)
            return foundation::invalid_argument("feature catalog contract summary does not match");
        FeatureDefinition definition{};
        definition.id = entry.definition_id;
        definition.version = entry.version;
        definition.owner_module = entry.owner_module;
        definition.contract = entry.contract;
        if (entry.contract_digest != feature_contract_digest(definition))
            return foundation::invalid_argument("feature catalog contract digest does not match (entry=" +
                                                std::to_string(entry.contract_digest) + ", computed=" +
                                                std::to_string(feature_contract_digest(definition)) + ")");
    }
    for (const auto& existing : entries_) {
        if (existing.contract.definition_id.is_valid() && entry.contract.definition_id.is_valid() &&
            existing.clock == entry.clock && !same_clock_schedule(existing.contract.clock, entry.contract.clock))
            return foundation::invalid_argument("Catalog Features sharing a ClockId require one Clock schedule");
        if (existing.definition_id == entry.definition_id) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                               "feature definition is already catalogued");
        }
    }
    entries_.push_back(std::move(entry));
    return foundation::Result::success();
}

uint64_t FeatureCatalog::contract_digest() const noexcept {
    std::vector<const FeatureCatalogEntry*> ordered;
    ordered.reserve(entries_.size());
    for (const auto& entry : entries_)
        ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->definition_id.value() < right->definition_id.value();
    });

    // Keep this byte-for-byte identical to FeatureRegistry::contract_digest;
    // graph documents compare the catalog snapshot with the live registry.
    uint64_t digest = 14695981039346656037ull;
    const auto mix = [&digest](uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            digest ^= (value >> shift) & 0xffu;
            digest *= 1099511628211ull;
        }
    };
    for (const auto* entry : ordered) {
        mix(entry->contract_digest);
    }
    return digest == 0 ? 1 : digest;
}
}  // namespace aeg::feature
