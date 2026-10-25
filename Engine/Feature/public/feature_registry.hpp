/**
 * @file feature_registry.hpp
 * @brief Immutable Feature definition catalog and recursive expansion.
 */
#pragma once

#include <memory>
#include <utility>
#include <unordered_map>
#include <vector>

#include "feature_contract.hpp"
#include "feature_clock_host.hpp"

namespace aeg::feature {
namespace detail {
struct ClockHostContext;
using ClockHostContextPtr = std::shared_ptr<const ClockHostContext>;
struct ClockAdmissionAccess;
}
class FeatureCatalog;

struct ExpandedFeature final {
    FeatureDefinitionId definition_id{};
    Version version{};
    FeatureDomainMask domains{0};
    FeatureContract contract{};
    OwnerPath owner_path{};
    std::vector<ExpandedFeature> children{};
    /** Module provenance is preserved through Composite expansion. */
    ModuleIdentity owner_module{};
};

class FeatureRegistry final {
public:
    explicit FeatureRegistry(detail::ClockHostContextPtr host = {}) : clock_host_(std::move(host)) {}
    /** Host policy alone does not approve any Clock; registration still requires engine admission. */
    explicit FeatureRegistry(const ClockHost& host);
    /**
     * Builds a metadata-only snapshot under the fixed default Clock limits.
     * Supports lookup, expansion, digest and graph compilation; carries no
     * callbacks or execution/registration authority. Failure returns an empty registry.
     */
    [[nodiscard]] static foundation::ResultValue<FeatureRegistry> from_catalog(const FeatureCatalog& catalog);
    [[nodiscard]] foundation::Result register_definition(FeatureDefinition definition);
    [[nodiscard]] foundation::Result replace_definition(FeatureDefinition definition);
    [[nodiscard]] foundation::Result unregister_definition(FeatureDefinitionId id);
    [[nodiscard]] const FeatureDefinition* find_definition(FeatureDefinitionId id) const noexcept;
    [[nodiscard]] uint64_t contract_digest() const noexcept;
    [[nodiscard]] foundation::ResultValue<ExpandedFeature> expand(
        FeatureDefinitionId id, const OwnerPath& owner_path) const;

private:
    friend struct detail::ClockAdmissionAccess;
    [[nodiscard]] foundation::ResultValue<ExpandedFeature> expand_impl(
        FeatureDefinitionId id, const OwnerPath& owner_path,
        std::vector<FeatureDefinitionId>& stack) const;

    std::unordered_map<FeatureDefinitionId, FeatureDefinition,
                       StableIdHash<FeatureDefinitionIdTag>> definitions_{};
    detail::ClockHostContextPtr clock_host_{};
    std::vector<std::pair<ClockId, Version>> clock_bindings_{};
};

}  // namespace aeg::feature
