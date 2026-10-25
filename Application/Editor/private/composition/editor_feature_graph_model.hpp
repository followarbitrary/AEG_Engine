/**
 * @file editor_feature_graph_model.hpp
 * @brief Feature-only editor composition model.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "feature_catalog.hpp"
#include "feature_graph.hpp"

namespace aeg::application {

struct FeaturePaletteFilter final {
    std::optional<aeg::feature::FeatureDomain> domain{};
    std::optional<aeg::feature::ExecutionKind> execution{};
    // Empty selects every clock; an engaged zero ClockId selects unclocked entries.
    std::optional<aeg::feature::ClockId> clock{};
    std::optional<aeg::feature::FeatureStructure> structure{};
    // Empty selects every scope; an engaged zero selects summary entries without a declared scope.
    std::optional<aeg::feature::ScopeId> scope{};
};

struct FeaturePaletteEntry final {
    aeg::feature::FeatureDefinitionId definition_id{};
    aeg::feature::FeatureDomainMask domains{0};
    aeg::feature::ExecutionKind execution{aeg::feature::ExecutionKind::kCall};
    aeg::feature::ClockId clock{};
    aeg::feature::FeatureStructure structure{aeg::feature::FeatureStructure::kAtomic};
    aeg::feature::ScopeId scope{};
};

[[nodiscard]] bool feature_palette_matches(const FeaturePaletteEntry& entry,
                                           const FeaturePaletteFilter& filter) noexcept;

struct RuntimeInspectPath final {
    aeg::feature::FeatureDefinitionId definition_id{};
    aeg::feature::FeatureNodeId node_id{};
    aeg::feature::FeatureInstanceId instance_id{};
    aeg::feature::ProcessId process_id{};
    aeg::feature::InvocationId invocation_id{};
    aeg::feature::OwnerPath owner_path{};
};

class EditorFeatureGraphModel final {
public:
    [[nodiscard]] aeg::foundation::Result replace_catalog(
        const aeg::feature::FeatureCatalog& catalog);
    [[nodiscard]] std::vector<FeaturePaletteEntry> palette(
        const FeaturePaletteFilter& filter) const;
    [[nodiscard]] aeg::foundation::Result open_document(
        const aeg::feature::FeatureGraphDocument& document);
    [[nodiscard]] aeg::foundation::ResultValue<const aeg::feature::ProcessSubgraph*> open_process(
        aeg::feature::FeatureNodeId owner) const;
    [[nodiscard]] static RuntimeInspectPath inspect(
        const aeg::feature::FeatureNodeProvenance& provenance);

private:
    std::vector<FeaturePaletteEntry> entries_{};
    std::optional<aeg::feature::FeatureGraphDocument> document_{};
};

}  // namespace aeg::application
