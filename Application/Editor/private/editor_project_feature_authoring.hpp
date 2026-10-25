/**
 * @file editor_project_feature_authoring.hpp
 * @brief Project-local FeatureGraph authoring data available before a game module is built.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "feature_graph.hpp"
#include "feature_catalog.hpp"
#include "result.hpp"
#include "composition/editor_node_contract_storage.hpp"

namespace aeg::application {

/**
 * A project-authored FeatureGraph document and its source path.
 *
 * FeatureGraph is the canonical authoring format.  The load result is kept
 * alongside the document so a malformed asset remains visible to the editor
 * without making the entire project catalog unavailable.
 */
struct EditorProjectFeatureGraphDocument final {
    std::filesystem::path path{};
    feature::FeatureGraphDocument graph{};
    foundation::Result load_result{};
};

enum class FeatureCatalogChangeKind : uint8_t { kAdded, kRemoved, kChanged };

struct FeatureCatalogEntryChange final {
    FeatureCatalogChangeKind kind{FeatureCatalogChangeKind::kChanged};
    feature::FeatureDefinitionId definition_id{};
    feature::Version before_version{};
    feature::Version after_version{};
    uint64_t before_digest{0};
    uint64_t after_digest{0};
    std::vector<std::string> changed_fields{};
    std::vector<feature::PortId> added_ports{}, removed_ports{}, changed_ports{};
    std::vector<feature::ParameterId> added_parameters{}, removed_parameters{}, changed_parameters{};
};

struct FeatureCatalogGraphImpact final {
    std::filesystem::path path{};
    std::string asset_uuid{};
    std::vector<feature::FeatureDefinitionId> definitions{};
    std::vector<feature::FeatureNodeId> node_ids{};
    std::vector<feature::FeatureEdge> edges{};
};

struct FeatureCatalogDiff final {
    uint64_t before_digest{0};
    uint64_t after_digest{0};
    std::vector<FeatureCatalogEntryChange> changes{};
    std::vector<FeatureCatalogGraphImpact> impacts{};
    [[nodiscard]] bool empty() const noexcept { return changes.empty(); }
};

/** Compares catalogs by stable Feature identity and reports exact authored graph usage. */
[[nodiscard]] FeatureCatalogDiff diff_feature_catalogs(
    const feature::FeatureCatalog& before, const feature::FeatureCatalog& after,
    std::span<const EditorProjectFeatureGraphDocument> documents);

struct EditorProjectFeatureConflict final {
    feature::FeatureDefinitionId definition_id{};
    NodeDefinitionDraft draft{};
    feature::FeatureCatalogEntry compiled{};
    std::string message{};
};

struct EditorProjectFeatureAuthoring final {
    /** Feature definitions exported by loaded module metadata for graph authoring. */
    feature::FeatureCatalog feature_catalog{};
    std::vector<feature::FeatureDefinitionId> compiled_feature_ids{};
    /** Canonical FeatureGraph authoring documents discovered under Assets. */
    std::vector<EditorProjectFeatureGraphDocument> feature_graphs{};
    std::vector<NodeContractFile> contracts{};
    /** Editor labels whose complete owner/version/contract matches the authoring catalog. */
    std::vector<NodeDefinitionDraft> presentations{};
    /** Non-generated editor drafts kept available for the definition editor. */
    std::vector<NodeContractFile> drafts{};
    std::vector<EditorProjectFeatureConflict> conflicts{};
    std::vector<std::string> contract_diagnostics{};
};

/**
 * Loads source module metadata and authored FeatureGraph assets without
 * loading a game DLL. Retired renderer authoring assets are diagnosed by
 * source path and never parsed or exposed as executable documents.
 */
[[nodiscard]] foundation::ResultValue<EditorProjectFeatureAuthoring> load_editor_project_feature_authoring(
    const std::filesystem::path& project_root, const std::filesystem::path& source_directory,
    const std::filesystem::path& assets_directory);

}  // namespace aeg::application
