/**
 * @file editor_feature_graph_document.hpp
 * @brief Undoable editor-owned snapshot for a FeatureGraph asset.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "feature_graph.hpp"
#include "feature_catalog.hpp"
#include "feature_graph_io.hpp"

namespace aeg::application {

struct ProcessEditorLayout;
struct FeatureEditorLayout;

/** Resolves an explicit or unique project file. A projectless directory yields an empty path. */
[[nodiscard]] foundation::ResultValue<std::filesystem::path> resolve_editor_project_file(
    const std::filesystem::path& project_root_or_file);

/** Resolves one selected or unambiguous project's configured asset directory. */
[[nodiscard]] foundation::ResultValue<std::filesystem::path> resolve_editor_asset_directory(
    const std::filesystem::path& project_root_or_file);

/** Resolves the selected project's configured source directory; projectless roots use Source. */
[[nodiscard]] foundation::ResultValue<std::filesystem::path> resolve_editor_source_directory(
    const std::filesystem::path& project_root_or_file);

/** Authoring resolver: re-reads canonical product UUIDs from configured project metadata on lookup. */
[[nodiscard]] feature::AssetKindResolver make_editor_asset_kind_resolver(
    const std::filesystem::path& project_root_or_file);
/** Runtime resolver: retains one immutable index for an accepted executable session, including resets.
 * Module/session candidates rebuild it; authoring refreshes do not change an active session's authority.
 * Lookups do no filesystem I/O.
 */
[[nodiscard]] feature::AssetKindResolver snapshot_editor_asset_kind_resolver(
    const std::filesystem::path& project_root_or_file);

/**
 * FeatureGraph is the authoring root. Frame execution and RenderGraph inspection
 * consume derived runtime snapshots. The editor only saves an author snapshot
 * after a successful transaction.
 */
class EditorFeatureGraphDocument final {
public:
    explicit EditorFeatureGraphDocument(feature::FeatureGraphDocument document, feature::FeatureCatalog catalog = {});
    [[nodiscard]] const feature::FeatureCatalog& catalog() const noexcept { return *catalog_; }
    [[nodiscard]] foundation::Result apply_contract_transaction(std::string label, feature::FeatureCatalog catalog,
        std::function<foundation::Result(feature::FeatureGraphDocument&)> mutation);

    [[nodiscard]] const feature::FeatureGraphDocument& snapshot() const noexcept { return *current_; }
    /** @brief Retains the immutable snapshot that owns a consumer's editor draft. */
    [[nodiscard]] std::shared_ptr<const feature::FeatureGraphDocument> snapshot_owner() const noexcept {
        return current_;
    }
    /** Stable across edits/Undo, different after reopening even at the same address. */
    [[nodiscard]] std::shared_ptr<const void> identity_owner() const noexcept { return identity_; }
    [[nodiscard]] foundation::Result execute(std::string label,
                                              std::function<foundation::Result(feature::FeatureGraphDocument&)> mutation);
    [[nodiscard]] foundation::Result undo();
    [[nodiscard]] foundation::Result redo();
    /** @brief Deletes a node and every nested descendant that owns it. */
    [[nodiscard]] foundation::Result erase_node(feature::FeatureNodeId node_id);
    /** Deletes the complete selection and owned descendants in one transaction. */
    [[nodiscard]] foundation::Result erase_nodes(std::span<const feature::FeatureNodeId> node_ids);
    /** Copies selected subtrees and only internal edges, with new authored identities. */
    [[nodiscard]] foundation::Result clone_nodes(std::span<const feature::FeatureNodeId> node_ids);
    /** @brief Deletes one directed edge from the graph. */
    [[nodiscard]] foundation::Result erase_edge(feature::FeatureNodeId source_node, feature::PortId source_port,
                                                feature::FeatureNodeId target_node, feature::PortId target_port);
    /** @brief Adds a catalog-backed node and records the catalog digest with the edit. */
    [[nodiscard]] foundation::Result add_node(feature::FeatureNode node, uint64_t contract_digest);
    /** Validates and replaces a node's parameters as one undoable transaction. */
    [[nodiscard]] foundation::Result set_parameters(feature::FeatureNodeId node_id,
        std::span<const feature::FeatureParameterRecord> parameters, const feature::FeatureCatalog& catalog,
        const feature::AssetKindResolver& resolver = {});
    /** Validates supplied asset values and effective defaults for known contracts; unfinished required values remain drafts. */
    [[nodiscard]] foundation::Result validate_parameter_assets(const feature::AssetKindResolver& resolver) const;
    /** Installs the saved snapshot only after validation and atomic file replacement succeed. */
    [[nodiscard]] foundation::Result save(const std::filesystem::path& path,
                                          const feature::AssetKindResolver& resolver = {});
    /** Binds the exact bytes loaded for this source; other Save paths remain unconditional. */
    void bind_source_file(const std::filesystem::path&, authoring::FeatureGraphFileRevision);
    /** @brief Adds one schema-checked edge and records it as one undoable edit. */
    [[nodiscard]] foundation::Result add_edge(feature::FeatureEdge edge, const feature::FeatureCatalog& catalog);
    /** @brief Adds a state to an owner Process subgraph, creating the subgraph when needed. */
    [[nodiscard]] foundation::Result add_process_state(feature::FeatureNodeId owner_node, std::string state,
                                                       uint32_t state_schema_version = 1);
    /** @brief Adds a From->To transition to an owner Process subgraph. */
    [[nodiscard]] foundation::Result add_process_transition(feature::FeatureNodeId owner_node,
                                                            std::string from_state, std::string to_state);
    /** @brief Sets the explicit entry state of an owner Process subgraph. */
    [[nodiscard]] foundation::Result set_process_initial_state(feature::FeatureNodeId owner_node,
                                                               std::string state);
    /** @brief Adds or removes one explicit terminal state of an owner Process subgraph. */
    [[nodiscard]] foundation::Result set_process_terminal_state(feature::FeatureNodeId owner_node,
                                                                std::string state, bool terminal);
    /** @brief Removes a state and its transitions from an owner Process subgraph. */
    [[nodiscard]] foundation::Result erase_process_state(feature::FeatureNodeId owner_node,
                                                         std::string state);
    /** @brief Removes a transition from an owner Process subgraph. */
    [[nodiscard]] foundation::Result erase_process_transition(feature::FeatureNodeId owner_node,
                                                              std::string from_state,
                                                              std::string to_state);
    /** @brief Replaces one Process canvas as one author-only undoable transaction. */
    [[nodiscard]] foundation::Result set_process_layout(feature::FeatureNodeId owner_node,
                                                        const ProcessEditorLayout& layout);
    [[nodiscard]] foundation::Result set_feature_layout(const FeatureEditorLayout& layout);
    /** @brief Refreshes the saved graph metadata from the current module catalog. */
    void set_contract_digest(uint64_t contract_digest);
    void mark_saved() noexcept { saved_ = current_; }
    [[nodiscard]] bool dirty() const noexcept { return saved_ != current_; }
    [[nodiscard]] bool can_undo() const noexcept { return cursor_ != 0; }
    [[nodiscard]] bool can_redo() const noexcept { return cursor_ < history_.size(); }
    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }

private:
    struct HistoryEntry final {
        std::string label{};
        std::shared_ptr<const feature::FeatureGraphDocument> before{};
        std::shared_ptr<const feature::FeatureGraphDocument> after{};
        std::shared_ptr<const feature::FeatureCatalog> before_catalog{}, after_catalog{};
    };

    [[nodiscard]] foundation::Result execute_transaction(std::string label,
        std::shared_ptr<const feature::FeatureCatalog> catalog,
        std::function<foundation::Result(feature::FeatureGraphDocument&)> mutation);

    /** @brief Requires a catalog-known owner to declare an explicit Process Feature. */
    [[nodiscard]] foundation::Result validate_process_owner(feature::FeatureNodeId owner_node) const;

    [[nodiscard]] foundation::Result move_history(bool forward);

    std::shared_ptr<const feature::FeatureCatalog> catalog_{};
    std::shared_ptr<const feature::FeatureGraphDocument> current_{};
    std::shared_ptr<const void> identity_{};
    std::shared_ptr<const feature::FeatureGraphDocument> saved_{};
    std::vector<HistoryEntry> history_{};
    std::size_t cursor_{0};
    uint64_t revision_{0};
    std::filesystem::path source_file_{};
    authoring::FeatureGraphFileRevision source_revision_{};
};

}  // namespace aeg::application
