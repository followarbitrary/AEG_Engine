/**
 * @file feature_catalog.hpp
 * @brief Feature-only module/catalog projection.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "feature_contract.hpp"

namespace aeg::feature {

struct FeatureCatalogEntry final {
    FeatureDefinitionId definition_id{};
    Version version{};
    FeatureDomainMask domains{0};
    ExecutionKind execution{ExecutionKind::kCall};
    StatePolicy state{StatePolicy::kStateless};
    ClockId clock{};
    uint64_t contract_digest{0};
    // Immutable port schema copied from the module-owned Feature contract.
    // The executable callback never crosses this metadata boundary.
    std::vector<PortContract> ports{};
    // Canonical semantic contract copied for authoring, validation, and
    // deterministic module metadata.  The callback remains module-owned.
    FeatureContract contract{};
    /** Stable module identity that published this Feature definition. */
    ModuleIdentity owner_module{};
};

class FeatureCatalog final {
public:
    [[nodiscard]] foundation::Result add_entry(FeatureCatalogEntry entry);
    [[nodiscard]] foundation::Result export_definition(const FeatureDefinition& definition);
    [[nodiscard]] const std::vector<FeatureCatalogEntry>& entries() const noexcept { return entries_; }
    /** Stable aggregate digest used by graph documents and module metadata. */
    [[nodiscard]] uint64_t contract_digest() const noexcept;

private:
    std::vector<FeatureCatalogEntry> entries_{};
};

}  // namespace aeg::feature
