#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "pipeline_authoring.hpp"

namespace aeg::application {

enum class NodeContractSource : uint8_t { kEditorDraft, kCompiled, kConflict };
struct NodePortSchema final {
    pipeline::PortValueKind kind{};
    uint64_t id{1};
    const char* display_name{};
    pipeline::ResourceClass resource_class{};
};
[[nodiscard]] std::span<const NodePortSchema> node_port_schemas() noexcept;

struct NodeContractUsage final {
    struct Edge final {
        std::string edge_id{};
        uint64_t port_id{};
        uint64_t binding_id{};
    };
    struct Binding final {
        uint64_t binding_id{};
        uint64_t port_id{};
    };
    std::filesystem::path document_path{};
    std::string node_id{};
    std::vector<std::string> edge_ids{};
    std::vector<uint64_t> binding_ids{};
    /** Optional exact identities. Legacy callers may leave these empty. */
    std::vector<Edge> edges{};
    std::vector<Binding> bindings{};
    /** Identity of the open editor document that owns this usage. */
    const void* document_identity{};
};

struct NodeDefinitionDraft final {
    pipeline::TypeCatalogEntry entry{};
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
    std::string node_id{};
    std::string edge_id{};
    uint64_t port_id{};
    uint64_t parameter_id{};
    uint64_t binding_id{};
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
    std::vector<uint64_t> affected_binding_ids{};
    std::vector<NodeContractImpact> impacts{};
    NodeContractVersionLevel version_level{NodeContractVersionLevel::kUnchanged};
    bool requires_confirmation{false};
};

[[nodiscard]] foundation::Result validate_node_definition(const NodeDefinitionDraft& draft);
[[nodiscard]] NodeContractDiff diff_node_definition(const pipeline::TypeCatalogEntry& before,
                                                    const NodeDefinitionDraft& after,
                                                    std::span<const NodeContractUsage> usages);

}  // namespace aeg::application

\n