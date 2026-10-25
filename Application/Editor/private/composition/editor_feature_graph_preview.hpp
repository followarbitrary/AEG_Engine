#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "feature_graph.hpp"

namespace aeg::application {

enum class EditorFeatureGraphPreviewSeverity : uint8_t { kError };

struct EditorFeatureGraphPreviewDiagnostic final {
    std::string message{};
    EditorFeatureGraphPreviewSeverity severity{EditorFeatureGraphPreviewSeverity::kError};
    std::optional<feature::FeatureNodeId> node_id{};
    std::optional<feature::PortId> port_id{};
    std::optional<std::size_t> edge_index{};
    feature::OwnerPath owner_path{};
};

struct EditorFeatureGraphPreviewLane final {
    feature::FeatureDomain domain{feature::FeatureDomain::kLogic};
    std::vector<feature::FeatureNodeId> node_ids{};
};

struct EditorFeatureGraphPreviewClockGroup final {
    feature::ClockId clock{};
    std::vector<feature::FeatureNodeId> node_ids{};
};

struct EditorFeatureGraphPreviewEdge final {
    std::size_t edge_index{0};
    feature::EdgeKind transport{feature::EdgeKind::kData};
    feature::FeatureDomainMask source_domains{0};
    feature::FeatureDomainMask target_domains{0};
    feature::ClockId source_clock{};
    feature::ClockId target_clock{};
    uint32_t capacity{0};
    uint32_t deadline_ticks{0};
    bool cross_domain{false};
    foundation::Result validation{};
};

struct EditorFeatureGraphPreviewProvenance final {
    feature::FeatureNodeId node_id{};
    feature::FeatureDefinitionId definition_id{};
    feature::OwnerPath owner_path{};
    feature::ModuleIdentity owner_module{};
};

struct EditorFeatureGraphPreview final {
    uint64_t graph_revision{0};
    uint64_t catalog_digest{0};
    std::vector<EditorFeatureGraphPreviewLane> lanes{};
    std::vector<EditorFeatureGraphPreviewClockGroup> clock_groups{};
    std::vector<EditorFeatureGraphPreviewEdge> edges{};
    std::vector<EditorFeatureGraphPreviewProvenance> provenance{};
    std::vector<EditorFeatureGraphPreviewDiagnostic> diagnostics{};

    [[nodiscard]] bool has_errors() const noexcept { return !diagnostics.empty(); }
};

class EditorFeatureGraphPreviewCompiler final {
public:
    [[nodiscard]] foundation::ResultValue<EditorFeatureGraphPreview> compile(
        const feature::FeatureGraphDocument&, const feature::FeatureCatalog&) const;
};

}  // namespace aeg::application
