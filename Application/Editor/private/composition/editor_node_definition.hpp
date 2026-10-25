#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "feature_catalog.hpp"

namespace aeg::application {

enum class NodeContractSource : uint8_t { kEditorDraft, kCompiled, kConflict };
struct NodePortLabel final {
    feature::PortId id{};
    std::string name{};
};
struct NodeContractUsage final {
    struct Edge final { std::string edge_id{}; uint64_t port_id{}; std::size_t edge_index{}; };
    std::filesystem::path document_path{};
    feature::FeatureNodeId node_id{};
    std::vector<Edge> edges{};
    const void* document_identity{};
};

struct NodeDefinitionDraft final {
    feature::FeatureDefinition entry{};
    std::string name{};
    std::string display_name{};
    std::vector<NodePortLabel> port_labels{};
    std::vector<uint64_t> retired_port_ids{};
    std::vector<uint64_t> retired_parameter_ids{};
    NodeContractSource source{NodeContractSource::kEditorDraft};
    bool generated{false};
    std::string draft_revision_hash{};
};

struct NodeContractIssue final {
    std::string code{};
    std::string message{};
    uint64_t port_id{};
    uint64_t parameter_id{};
};

struct NodeContractImpact final {
    std::filesystem::path document_path{};
    feature::FeatureNodeId node_id{};
    std::string edge_id{};
    uint64_t port_id{};
    uint64_t parameter_id{};
    std::size_t edge_index{};
    /** Identity of the open editor document that owns this impact. */
    const void* document_identity{};
};

enum class NodeContractVersionLevel : uint8_t { kUnchanged, kPatch, kMinor, kMajor };

struct NodeContractDiff final {
    std::vector<uint64_t> added_ports{};
    std::vector<uint64_t> removed_ports{};
    std::vector<uint64_t> renamed_ports{};
    std::vector<uint64_t> changed_ports{};
    std::vector<uint64_t> added_parameters{};
    std::vector<uint64_t> removed_parameters{};
    std::vector<uint64_t> renamed_parameters{};
    std::vector<uint64_t> changed_parameters{};
    std::vector<std::string> affected_edge_ids{};
    std::vector<NodeContractImpact> impacts{};
    NodeContractVersionLevel version_level{NodeContractVersionLevel::kUnchanged};
    bool requires_confirmation{false};
};

[[nodiscard]] NodeDefinitionDraft node_definition_from_catalog(const feature::FeatureCatalogEntry&);
[[nodiscard]] foundation::Result validate_node_definition(const NodeDefinitionDraft& draft);
[[nodiscard]] NodeContractDiff diff_node_definition(const NodeDefinitionDraft& before,
                                                    const NodeDefinitionDraft& after,
                                                    std::span<const NodeContractUsage> usages);

}  // namespace aeg::application
